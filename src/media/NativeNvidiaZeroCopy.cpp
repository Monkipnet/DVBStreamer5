#include "media/NativeNvidiaZeroCopy.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <dlfcn.h>
#endif

#if defined(DVBSTREAMER5_HAVE_NVDEC_HEADERS) && defined(DVBSTREAMER5_HAVE_NVENC_HEADERS)
#include <dynlink_cuda.h>
#include <dynlink_cuviddec.h>
#include <dynlink_nvcuvid.h>
#include <nvEncodeAPI.h>
#endif

namespace dvbstreamer5::media::codec {
namespace {

#if defined(__linux__) && defined(DVBSTREAMER5_HAVE_NVDEC_HEADERS) && defined(DVBSTREAMER5_HAVE_NVENC_HEADERS)

bool canDlopen(const char* soname) noexcept {
    void* handle = dlopen(soname, RTLD_LAZY | RTLD_LOCAL);
    if (!handle) return false;
    dlclose(handle);
    return true;
}

template <typename Function>
bool loadSymbol(void* library, const char* name, Function*& target) {
    target = reinterpret_cast<Function*>(dlsym(library, name));
    return target != nullptr;
}

int configuredGpuOrdinal(std::string& error) {
    error.clear();
    const char* value = std::getenv("DVBSTREAMER5_NVENC_GPU");
    if (!value) return 0;
    char* end = nullptr;
    const long ordinal = std::strtol(value, &end, 10);
    if (!end || end == value || *end != '\0' || ordinal < 0 || ordinal > 1024) {
        error = "DVBSTREAMER5_NVENC_GPU must be a non-negative GPU ordinal";
        return -1;
    }
    return static_cast<int>(ordinal);
}

cudaVideoCodec nvdecCodec(mpegts::ElementaryCodec codec) {
    if (codec == mpegts::ElementaryCodec::Mpeg2Video) return cudaVideoCodec_MPEG2;
    if (codec == mpegts::ElementaryCodec::H264) return cudaVideoCodec_H264;
    if (codec == mpegts::ElementaryCodec::H265) return cudaVideoCodec_HEVC;
    return cudaVideoCodec_NumCodecs;
}

const GUID& nvencCodecGuid(mpegts::ElementaryCodec codec) {
    return codec == mpegts::ElementaryCodec::H264
        ? NV_ENC_CODEC_H264_GUID : NV_ENC_CODEC_HEVC_GUID;
}

const char* codecName(mpegts::ElementaryCodec codec) {
    if (codec == mpegts::ElementaryCodec::Mpeg2Video) return "mpeg2";
    if (codec == mpegts::ElementaryCodec::H264) return "h264";
    if (codec == mpegts::ElementaryCodec::H265) return "hevc";
    return "unknown";
}

bool sameGuid(const GUID& a, const GUID& b) noexcept {
    return a.Data1 == b.Data1 && a.Data2 == b.Data2 &&
           a.Data3 == b.Data3 &&
           std::memcmp(a.Data4, b.Data4, sizeof(a.Data4)) == 0;
}

class NvidiaZeroCopyTranscoderImpl final : public NvidiaZeroCopyTranscoder {
public:
    explicit NvidiaZeroCopyTranscoderImpl(NvidiaZeroCopyConfig config)
        : config_(std::move(config)) {}

    ~NvidiaZeroCopyTranscoderImpl() override {
        close();
    }

    bool initialize(std::string& error) {
        error.clear();
        if ((config_.inputCodec != mpegts::ElementaryCodec::Mpeg2Video &&
             config_.inputCodec != mpegts::ElementaryCodec::H264 &&
             config_.inputCodec != mpegts::ElementaryCodec::H265) ||
            (config_.outputCodec != mpegts::ElementaryCodec::H264 &&
             config_.outputCodec != mpegts::ElementaryCodec::H265)) {
            error = "NVIDIA zero-copy supports MPEG-2/H.264/HEVC input and H.264/HEVC output";
            return false;
        }
        if (config_.width <= 0 || config_.height <= 0 ||
            (config_.width & 1) || (config_.height & 1)) {
            error = "NVIDIA zero-copy requires positive even output dimensions";
            return false;
        }

        gpuOrdinal_ = configuredGpuOrdinal(error);
        if (gpuOrdinal_ < 0) return false;
        // Opt-in diagnostic GPU-only repack. No CPU pixels or FFmpeg path:
        // NVDEC mapped NV12 -> CUDA device-to-device copies into a dedicated
        // pitched NVENC input buffer. Default unchanged for production tiles.
        const char* safeCopy = std::getenv("DVBSTREAMER5_NVDEC_SAFE_COPY");
        safeGpuCopy_ = safeCopy && std::strcmp(safeCopy, "1") == 0;
        // V10.8.228: strictly opt-in chroma isolation test. Keep the
        // decoded Y plane unchanged and force interleaved UV bytes to 0x80
        // on GPU. The intentional output is grayscale, not a color fix.
        const char* chromaDiag = std::getenv("DVBSTREAMER5_NVDEC_DIAG_CHROMA");
        neutralChromaDiag_ = chromaDiag && std::strcmp(chromaDiag, "neutral") == 0;
        if (neutralChromaDiag_) safeGpuCopy_ = true;
        const char* vppMode = std::getenv("DVBSTREAMER5_NVDEC_VPP_MODE");
        if (vppMode && (std::strcmp(vppMode, "adaptive") == 0 ||
                        std::strcmp(vppMode, "bob") == 0 ||
                        std::strcmp(vppMode, "weave") == 0)) {
            nvdecVppMode_ = vppMode;
        } else if (vppMode && *vppMode) {
            std::cerr << "NATIVE NVDEC VPP invalid mode=" << vppMode
                      << " fallback=adaptive" << std::endl;
        }
        const char* vppProbe = std::getenv("DVBSTREAMER5_NVDEC_VPP_PROBE");
        nvdecVppProbe_ = vppProbe && std::strcmp(vppProbe, "1") == 0;
        const char* uvProbe = std::getenv("DVBSTREAMER5_NVDEC_UV_PROBE");
        nvdecUvProbe_ = uvProbe && std::strcmp(uvProbe, "1") == 0;
        // V10.8.234 diagnostic A/B: FFmpeg h264_cuvid -deint bob
        // -drop_second_field 1 maps the first field (second_field=0).
        // The standard NVIDIA NvDecoder sample instead maps
        // repeat_first_field+1. Keep the old behavior unless enabled.
        const char* firstField = std::getenv("DVBSTREAMER5_NVDEC_FIRST_FIELD_TEST");
        nvdecFirstFieldTest_ = firstField && std::strcmp(firstField, "1") == 0;
        // A/B isolation: use the identical NVENC-owned input buffer layout as
        // the working software-decoder -> NVENC encoder instead of registering
        // an external CUDA pointer. NVDEC stays hardware. This mode downloads
        // the full Y and UV planes and is strictly diagnostic, not zero-copy.
        const char* swInput = std::getenv("DVBSTREAMER5_NVENC_SOFTWARE_INPUT_TEST");
        nvencSoftwareInputTest_ = swInput && std::strcmp(swInput, "1") == 0;
        if (!loadRuntime(error)) return false;

        if (cuInit_(0) != CUDA_SUCCESS ||
            cuDeviceGet_(&device_, gpuOrdinal_) != CUDA_SUCCESS ||
            cuCtxCreate_(&context_, CU_CTX_SCHED_BLOCKING_SYNC, device_) != CUDA_SUCCESS ||
            !context_) {
            error = "CUDA context creation failed for GPU ordinal " +
                    std::to_string(gpuOrdinal_);
            return false;
        }

        // cuCtxCreate makes the new context current. Keep the object context
        // floating and explicitly push it around every decode/encode operation;
        // reset/destruction can then safely happen on a different worker thread.
        CUcontext popped = nullptr;
        if (cuCtxPopCurrent_(&popped) != CUDA_SUCCESS || popped != context_) {
            error = "CUDA context detach failed";
            return false;
        }

        if (!pushContext(error)) return false;
        const bool capsOk = validateDecoderCapabilities(error);
        bool parserOk = false;
        if (capsOk) parserOk = createParser(error);
        popContext();
        if (!capsOk || !parserOk) return false;

        return true;
    }

    bool process(const std::uint8_t* data, std::size_t size,
                 std::uint64_t pts90k, bool hasPts,
                 std::vector<EncodedVideoFrame>& output,
                 std::string& error) override {
        error.clear();
        if (!parser_) {
            error = "NVDEC parser is not initialized";
            return false;
        }
        if (!data || size == 0) return true;
        if (!pushContext(error)) return false;

        callbackOutput_ = &output;
        callbackError_.clear();
        if (hasPts) timestampsSeen_ = true;

        CUVIDSOURCEDATAPACKET packet{};
        packet.payload = data;
        packet.payload_size = static_cast<tcu_ulong>(size);
        if (hasPts) {
            packet.flags |= CUVID_PKT_TIMESTAMP;
            packet.timestamp = static_cast<CUvideotimestamp>(pts90k);
        }

        const CUresult status = cuvidParseVideoData_(parser_, &packet);
        callbackOutput_ = nullptr;
        popContext();
        if (status != CUDA_SUCCESS || !callbackError_.empty()) {
            error = callbackError_.empty()
                ? "cuvidParseVideoData failed: " + std::to_string(static_cast<int>(status))
                : callbackError_;
            return false;
        }
        return true;
    }

    bool flush(std::vector<EncodedVideoFrame>& output,
               std::string& error) override {
        error.clear();
        if (!parser_) return true;
        if (!pushContext(error)) return false;

        callbackOutput_ = &output;
        callbackError_.clear();
        CUVIDSOURCEDATAPACKET packet{};
        packet.flags = CUVID_PKT_ENDOFSTREAM;
        CUresult decodeStatus = cuvidParseVideoData_(parser_, &packet);
        callbackOutput_ = nullptr;

        bool ok = decodeStatus == CUDA_SUCCESS && callbackError_.empty();
        if (!ok) {
            error = callbackError_.empty()
                ? "NVDEC end-of-stream failed: " +
                    std::to_string(static_cast<int>(decodeStatus))
                : callbackError_;
        }

        if (ok && encoder_) {
            NV_ENC_PIC_PARAMS eos{};
            eos.version = NV_ENC_PIC_PARAMS_VER;
            eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
            const NVENCSTATUS encStatus = api_.nvEncEncodePicture(encoder_, &eos);
            if (encStatus != NV_ENC_SUCCESS &&
                encStatus != NV_ENC_ERR_NEED_MORE_INPUT) {
                error = "NVENC zero-copy EOS failed: " +
                        std::to_string(static_cast<int>(encStatus));
                ok = false;
            }
        }

        popContext();
        return ok;
    }

    int sourceWidth() const noexcept override { return sourceWidth_; }
    int sourceHeight() const noexcept override { return sourceHeight_; }
    int outputWidth() const noexcept override { return outputWidth_; }
    int outputHeight() const noexcept override { return outputHeight_; }

private:
    bool loadRuntime(std::string& error) {
        cudaLib_ = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!cudaLib_) {
            error = "libcuda.so.1 not available";
            return false;
        }
        cuvidLib_ = dlopen("libnvcuvid.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!cuvidLib_) {
            error = "libnvcuvid.so.1 not available";
            return false;
        }
        nvencLib_ = dlopen("libnvidia-encode.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!nvencLib_) {
            error = "libnvidia-encode.so.1 not available";
            return false;
        }

        if (!loadSymbol(cudaLib_, "cuInit", cuInit_) ||
            !loadSymbol(cudaLib_, "cuDeviceGet", cuDeviceGet_)) {
            error = "CUDA driver API initialization symbols missing";
            return false;
        }
        if (!loadSymbol(cudaLib_, "cuCtxCreate_v2", cuCtxCreate_))
            loadSymbol(cudaLib_, "cuCtxCreate", cuCtxCreate_);
        if (!loadSymbol(cudaLib_, "cuCtxDestroy_v2", cuCtxDestroy_))
            loadSymbol(cudaLib_, "cuCtxDestroy", cuCtxDestroy_);
        if (!loadSymbol(cudaLib_, "cuCtxPushCurrent_v2", cuCtxPushCurrent_))
            loadSymbol(cudaLib_, "cuCtxPushCurrent", cuCtxPushCurrent_);
        if (!loadSymbol(cudaLib_, "cuCtxPopCurrent_v2", cuCtxPopCurrent_))
            loadSymbol(cudaLib_, "cuCtxPopCurrent", cuCtxPopCurrent_);
        if (!cuCtxCreate_ || !cuCtxDestroy_ ||
            !cuCtxPushCurrent_ || !cuCtxPopCurrent_) {
            error = "CUDA context API symbols missing";
            return false;
        }
        if (safeGpuCopy_ &&
            (!loadSymbol(cudaLib_, "cuMemAllocPitch_v2", cuMemAllocPitch_) ||
             !loadSymbol(cudaLib_, "cuMemFree_v2", cuMemFree_))) {
            error = "CUDA pitched allocation driver functions missing";
            return false;
        }
        if ((safeGpuCopy_ || nvencSoftwareInputTest_) &&
            !loadSymbol(cudaLib_, "cuMemcpy2D_v2", cuMemcpy2D_)) {
            error = "CUDA NV12 plane copying requires cuMemcpy2D_v2";
            return false;
        }
        if (neutralChromaDiag_ &&
            (!loadSymbol(cudaLib_, "cuMemsetD2D8Async", cuMemsetD2D8Async_) ||
             !loadSymbol(cudaLib_, "cuCtxSynchronize", cuCtxSynchronize_))) {
            error = "CUDA chroma diagnostic requires device memset/synchronize APIs";
            return false;
        }
        if (nvdecUvProbe_ &&
            !loadSymbol(cudaLib_, "cuMemcpyDtoH_v2", cuMemcpyDtoH_)) {
            error = "CUDA chroma probe requires cuMemcpyDtoH_v2";
            return false;
        }

        if (!loadSymbol(cuvidLib_, "cuvidGetDecoderCaps", cuvidGetDecoderCaps_) ||
            !loadSymbol(cuvidLib_, "cuvidCreateVideoParser", cuvidCreateVideoParser_) ||
            !loadSymbol(cuvidLib_, "cuvidParseVideoData", cuvidParseVideoData_) ||
            !loadSymbol(cuvidLib_, "cuvidDestroyVideoParser", cuvidDestroyVideoParser_) ||
            !loadSymbol(cuvidLib_, "cuvidCreateDecoder", cuvidCreateDecoder_) ||
            !loadSymbol(cuvidLib_, "cuvidDestroyDecoder", cuvidDestroyDecoder_) ||
            !loadSymbol(cuvidLib_, "cuvidDecodePicture", cuvidDecodePicture_) ||
            !loadSymbol(cuvidLib_, "cuvidMapVideoFrame64", cuvidMapVideoFrame_) ||
            !loadSymbol(cuvidLib_, "cuvidUnmapVideoFrame64", cuvidUnmapVideoFrame_)) {
            error = "NVDEC/CUVID runtime symbols missing";
            return false;
        }
        // Optional on SDK/driver combinations without decode status support.
        (void)loadSymbol(cuvidLib_, "cuvidGetDecodeStatus",
                         cuvidGetDecodeStatus_);

        using CreateNvencApi = NVENCSTATUS (NVENCAPI *)(NV_ENCODE_API_FUNCTION_LIST*);
        auto createApi = reinterpret_cast<CreateNvencApi>(
            dlsym(nvencLib_, "NvEncodeAPICreateInstance"));
        if (!createApi) {
            error = "NvEncodeAPICreateInstance not found";
            return false;
        }
        std::memset(&api_, 0, sizeof(api_));
        api_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        if (createApi(&api_) != NV_ENC_SUCCESS) {
            error = "NvEncodeAPICreateInstance failed";
            return false;
        }
        return true;
    }

    bool pushContext(std::string& error) {
        if (!context_ || !cuCtxPushCurrent_ ||
            cuCtxPushCurrent_(context_) != CUDA_SUCCESS) {
            error = "CUDA context activation failed";
            return false;
        }
        return true;
    }

    void popContext() noexcept {
        if (!cuCtxPopCurrent_) return;
        CUcontext popped = nullptr;
        (void)cuCtxPopCurrent_(&popped);
    }

    bool validateDecoderCapabilities(std::string& error) {
        CUVIDDECODECAPS caps{};
        caps.eCodecType = nvdecCodec(config_.inputCodec);
        caps.eChromaFormat = cudaVideoChromaFormat_420;
        caps.nBitDepthMinus8 = 0;
        const CUresult status = cuvidGetDecoderCaps_(&caps);
        if (status != CUDA_SUCCESS || !caps.bIsSupported ||
            !(caps.nOutputFormatMask &
              (1U << static_cast<unsigned>(cudaVideoSurfaceFormat_NV12)))) {
            error = std::string("NVDEC GPU does not support 8-bit 4:2:0 ") +
                    codecName(config_.inputCodec) + " to NV12";
            return false;
        }
        std::cerr << "NATIVE NVDEC CAPABILITIES gpu=" << gpuOrdinal_
                  << " codec=" << codecName(config_.inputCodec)
                  << " nv12=1 max=" << caps.nMaxWidth << "x" << caps.nMaxHeight
                  << " nvdec_engines=" << static_cast<unsigned>(caps.nNumNVDECs)
                  << std::endl;
        return true;
    }

    bool createParser(std::string& error) {
        CUVIDPARSERPARAMS params{};
        params.CodecType = nvdecCodec(config_.inputCodec);
        params.ulMaxNumDecodeSurfaces = 20;
        params.ulClockRate = 90000;
        params.ulErrorThreshold = 0;
        params.ulMaxDisplayDelay = 0;
        params.pUserData = this;
        params.pfnSequenceCallback = &sequenceCallback;
        params.pfnDecodePicture = &decodeCallback;
        params.pfnDisplayPicture = &displayCallback;
        const CUresult status = cuvidCreateVideoParser_(&parser_, &params);
        if (status != CUDA_SUCCESS || !parser_) {
            error = "cuvidCreateVideoParser failed: " +
                    std::to_string(static_cast<int>(status));
            return false;
        }
        return true;
    }

    static int CUDAAPI sequenceCallback(void* opaque, CUVIDEOFORMAT* format) {
        return static_cast<NvidiaZeroCopyTranscoderImpl*>(opaque)->onSequence(format);
    }

    static int CUDAAPI decodeCallback(void* opaque, CUVIDPICPARAMS* picture) {
        return static_cast<NvidiaZeroCopyTranscoderImpl*>(opaque)->onDecode(picture);
    }

    static int CUDAAPI displayCallback(void* opaque, CUVIDPARSERDISPINFO* display) {
        return static_cast<NvidiaZeroCopyTranscoderImpl*>(opaque)->onDisplay(display);
    }

    int onSequence(CUVIDEOFORMAT* format) {
        if (!format) return failCallback("NVDEC sequence callback received no format") ? 1 : 0;
        if (format->chroma_format != cudaVideoChromaFormat_420 ||
            format->bit_depth_luma_minus8 != 0 ||
            format->bit_depth_chroma_minus8 != 0) {
            failCallback("NVDEC zero-copy currently supports only 8-bit 4:2:0 video");
            return 0;
        }

        CUVIDDECODECAPS caps{};
        caps.eCodecType = format->codec;
        caps.eChromaFormat = format->chroma_format;
        caps.nBitDepthMinus8 = format->bit_depth_luma_minus8;
        if (cuvidGetDecoderCaps_(&caps) != CUDA_SUCCESS || !caps.bIsSupported ||
            !(caps.nOutputFormatMask &
              (1U << static_cast<unsigned>(cudaVideoSurfaceFormat_NV12)))) {
            failCallback("NVDEC sequence is not supported by this GPU");
            return 0;
        }
        if (format->coded_width < caps.nMinWidth ||
            format->coded_height < caps.nMinHeight ||
            format->coded_width > caps.nMaxWidth ||
            format->coded_height > caps.nMaxHeight ||
            (static_cast<std::uint64_t>(format->coded_width) *
             static_cast<std::uint64_t>(format->coded_height) / 256ULL) >
                caps.nMaxMBCount) {
            failCallback("NVDEC sequence dimensions exceed GPU decode capability");
            return 0;
        }

        int displayWidth = format->display_area.right - format->display_area.left;
        int displayHeight = format->display_area.bottom - format->display_area.top;
        if (displayWidth <= 0 || displayHeight <= 0) {
            displayWidth = static_cast<int>(format->coded_width);
            displayHeight = static_cast<int>(format->coded_height);
        }
        displayWidth &= ~1;
        displayHeight &= ~1;
        if (displayWidth <= 0 || displayHeight <= 0) {
            failCallback("NVDEC reported invalid display geometry");
            return 0;
        }

        sourceWidth_ = displayWidth;
        sourceHeight_ = displayHeight;
        outputWidth_ = config_.width;
        outputHeight_ = config_.height;
        const std::uint64_t sourcePixels =
            static_cast<std::uint64_t>(sourceWidth_) * sourceHeight_;
        const std::uint64_t requestedPixels =
            static_cast<std::uint64_t>(outputWidth_) * outputHeight_;
        if (!config_.lockOutputGeometry && requestedPixels > sourcePixels) {
            outputWidth_ = sourceWidth_;
            outputHeight_ = sourceHeight_;
        }
        outputWidth_ &= ~1;
        outputHeight_ &= ~1;

        destroyEncoder();
        destroyDecoder();

        const unsigned decodeSurfaces = std::max<unsigned>(
            2U, static_cast<unsigned>(format->min_num_decode_surfaces));
        CUVIDDECODECREATEINFO create{};
        create.ulWidth = format->coded_width;
        create.ulHeight = format->coded_height;
        create.ulNumDecodeSurfaces = decodeSurfaces;
        create.CodecType = format->codec;
        create.ChromaFormat = format->chroma_format;
        create.ulCreationFlags = cudaVideoCreate_PreferCUVID;
        create.bitDepthMinus8 = format->bit_depth_luma_minus8;
        create.ulMaxWidth = format->coded_width;
        create.ulMaxHeight = format->coded_height;
        create.display_area.left = static_cast<short>(format->display_area.left);
        create.display_area.top = static_cast<short>(format->display_area.top);
        create.display_area.right = static_cast<short>(format->display_area.right);
        create.display_area.bottom = static_cast<short>(format->display_area.bottom);
        create.OutputFormat = cudaVideoSurfaceFormat_NV12;
        // V10.8.227: isolated A/B test for field-processing artifacts on
        // interlaced H.264. Default is the original adaptive path; the
        // explicit per-tile deinterlace=off continues to force weave.
        const bool doDeinterlace = config_.deinterlace;
        const char* vppName = !doDeinterlace ? "weave" : nvdecVppMode_.c_str();
        create.DeinterlaceMode = !doDeinterlace || nvdecVppMode_ == "weave"
            ? cudaVideoDeinterlaceMode_Weave
            : nvdecVppMode_ == "bob"
                ? cudaVideoDeinterlaceMode_Bob
                : cudaVideoDeinterlaceMode_Adaptive;
        std::cerr << "NATIVE NVDEC VPP configuration mode=" << vppName
                  << " requested_deinterlace=" << (config_.deinterlace ? 1 : 0)
                  << " progressive_sequence=" << static_cast<int>(format->progressive_sequence)
                  << " coded=" << format->coded_width << "x" << format->coded_height
                  << " target=" << outputWidth_ << "x" << outputHeight_
                  << " fps=" << format->frame_rate.numerator
                  << "/" << format->frame_rate.denominator
                  << std::endl;
        create.ulTargetWidth = static_cast<tcu_ulong>(outputWidth_);
        create.ulTargetHeight = static_cast<tcu_ulong>(outputHeight_);
        create.ulNumOutputSurfaces = 4;
        create.target_rect.left = 0;
        create.target_rect.top = 0;
        create.target_rect.right = static_cast<short>(outputWidth_);
        create.target_rect.bottom = static_cast<short>(outputHeight_);

        const CUresult decodeCreate = cuvidCreateDecoder_(&decoder_, &create);
        if (decodeCreate != CUDA_SUCCESS || !decoder_) {
            failCallback("cuvidCreateDecoder failed: " +
                         std::to_string(static_cast<int>(decodeCreate)));
            return 0;
        }
        if (!createEncoder(callbackError_)) {
            destroyDecoder();
            return 0;
        }
        if (nvencSoftwareInputTest_) {
            NV_ENC_CREATE_INPUT_BUFFER input{};
            input.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
            input.width = static_cast<std::uint32_t>(outputWidth_);
            input.height = static_cast<std::uint32_t>(outputHeight_);
            input.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
            const NVENCSTATUS createStatus =
                api_.nvEncCreateInputBuffer(encoder_, &input);
            if (createStatus != NV_ENC_SUCCESS || !input.inputBuffer) {
                failCallback("NVENC software-input diagnostic buffer allocation failed: " +
                             std::to_string(static_cast<int>(createStatus)));
                destroyEncoder();
                destroyDecoder();
                return 0;
            }
            nvencSoftwareInputBuffer_ = input.inputBuffer;
            hostNv12_.resize(static_cast<std::size_t>(outputWidth_) *
                             static_cast<std::size_t>(outputHeight_) * 3U / 2U);
        }
        if (safeGpuCopy_) {
            const CUresult allocResult = cuMemAllocPitch_(
                &gpuCopyFrame_, &gpuCopyPitch_,
                static_cast<std::size_t>(outputWidth_),
                static_cast<std::size_t>(outputHeight_) * 3U / 2U, 16U);
            if (allocResult != CUDA_SUCCESS || !gpuCopyFrame_ ||
                gpuCopyPitch_ < static_cast<std::size_t>(outputWidth_) ||
                gpuCopyPitch_ > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
                failCallback("NVDEC safe-copy CUDA pitched surface allocation failed: " +
                             std::to_string(static_cast<int>(allocResult)));
                destroyEncoder();
                destroyDecoder();
                return 0;
            }
        }

        frameIndex_ = 0;
        forcedIdrCount_ = 0;
        fpsClockValid_ = false;
        fpsDroppedFrames_ = 0;
        std::cerr << "NATIVE NVIDIA ZERO-COPY active gpu=" << gpuOrdinal_
                  << " input=" << codecName(config_.inputCodec)
                  << " output=" << codecName(config_.outputCodec)
                  << " path=" << (nvencSoftwareInputTest_
                      ? "NVDEC->CPU(NV12)->NVENC"
                      : "NVDEC->CUDA(NV12)->NVENC")
                  << " source=" << sourceWidth_ << "x" << sourceHeight_
                  << " encode=" << outputWidth_ << "x" << outputHeight_
                  << " deinterlace=" << vppName
                  << " safe_gpu_copy=" << (safeGpuCopy_ ? 1 : 0)
                  << " chroma_diag=" << (neutralChromaDiag_ ? "neutral" : "off")
                  << " uv_probe=" << (nvdecUvProbe_ ? 1 : 0)
                  << " nvenc_software_input_test=" << (nvencSoftwareInputTest_ ? 1 : 0)
                  << " nvdec_first_field_test=" << (nvdecFirstFieldTest_ ? 1 : 0)
                  << " host_video_copies=" << (nvencSoftwareInputTest_ ? 2 : 0)
                  << " gpu_surface_copies=" << (neutralChromaDiag_ ? 1 : safeGpuCopy_ ? 2 : 0)
                  << std::endl;
        return static_cast<int>(decodeSurfaces);
    }

    int onDecode(CUVIDPICPARAMS* picture) {
        if (!decoder_ || !picture) {
            failCallback("NVDEC decode callback without decoder/picture");
            return 0;
        }
        const CUresult status = cuvidDecodePicture_(decoder_, picture);
        if (status != CUDA_SUCCESS) {
            failCallback("cuvidDecodePicture failed: " +
                         std::to_string(static_cast<int>(status)));
            return 0;
        }
        return 1;
    }

    int onDisplay(CUVIDPARSERDISPINFO* display) {
        if (!decoder_ || !encoder_ || !display || !callbackOutput_) {
            failCallback("NVDEC display callback is not ready for zero-copy encode");
            return 0;
        }

        const bool hasPts = timestampsSeen_;
        const std::uint64_t pts = static_cast<std::uint64_t>(display->timestamp);
        if (hasPts && config_.fps > 0.0) {
            constexpr std::uint64_t kPtsMask = (1ULL << 33U) - 1ULL;
            constexpr std::uint64_t kPtsHalf = 1ULL << 32U;
            const std::uint64_t interval = std::max<std::uint64_t>(
                1ULL, static_cast<std::uint64_t>(
                    std::llround(90000.0 / config_.fps)));
            const std::uint64_t normalized = pts & kPtsMask;
            if (!fpsClockValid_) {
                fpsClockValid_ = true;
                fpsNextPts90k_ = normalized;
            }
            const auto atOrAfter = [](std::uint64_t a, std::uint64_t b) {
                return ((a - b) & kPtsMask) < kPtsHalf;
            };
            if (!atOrAfter(normalized, fpsNextPts90k_)) {
                ++fpsDroppedFrames_;
                if (fpsDroppedFrames_ <= 4 ||
                    (fpsDroppedFrames_ % 250ULL) == 0ULL) {
                    std::cerr << "NATIVE NVIDIA ZERO-COPY FPS DROP dropped="
                              << fpsDroppedFrames_
                              << " target_fps=" << config_.fps
                              << " pts=" << normalized << std::endl;
                }
                return 1;
            }
            do {
                fpsNextPts90k_ = (fpsNextPts90k_ + interval) & kPtsMask;
            } while (atOrAfter(normalized, fpsNextPts90k_));
        }

        CUVIDPROCPARAMS proc{};
        proc.progressive_frame = display->progressive_frame;
        // NVIDIA's NvDecoder sample uses repeat_first_field + 1 here.
        // For interlaced frames, always selecting field zero can map the
        // incorrect field and produce combing/corrupted output after VPP.
        // A value of -1 denotes an unpaired field and must not underflow.
        proc.second_field = nvdecFirstFieldTest_ && config_.deinterlace &&
            nvdecVppMode_ != "weave" ? 0 :
            std::max(0, display->repeat_first_field + 1);
        proc.top_field_first = display->top_field_first;
        proc.unpaired_field = display->repeat_first_field < 0 ? 1 : 0;
        ++nvdecDisplayFrames_;
        if (nvdecVppProbe_ &&
            (nvdecDisplayFrames_ <= 12 ||
             (nvdecDisplayFrames_ % 500ULL) == 0ULL)) {
            std::cerr << "NATIVE NVDEC VPP FRAME count=" << nvdecDisplayFrames_
                      << " picture=" << display->picture_index
                      << " progressive=" << display->progressive_frame
                      << " top_first=" << display->top_field_first
                      << " repeat_first=" << display->repeat_first_field
                      << " second_field=" << proc.second_field
                      << " field_policy=" << (nvdecFirstFieldTest_ ? "first_test" : "default")
                      << " unpaired=" << proc.unpaired_field
                      << " mode=" << (config_.deinterlace ? nvdecVppMode_ : "weave")
                      << std::endl;
        }

        CUdeviceptr devicePtr = 0;
        unsigned int pitch = 0;
        const CUresult mapStatus = cuvidMapVideoFrame_(
            decoder_, display->picture_index, &devicePtr, &pitch, &proc);
        if (mapStatus != CUDA_SUCCESS || devicePtr == 0 || pitch == 0) {
            failCallback("cuvidMapVideoFrame failed: " +
                         std::to_string(static_cast<int>(mapStatus)));
            return 0;
        }

        if (pitch < static_cast<unsigned>(outputWidth_)) {
            (void)cuvidUnmapVideoFrame_(decoder_, devicePtr);
            failCallback("NVDEC mapped NV12 pitch smaller than output width");
            return 0;
        }

        // Sparse, opt-in GPU->CPU diagnostics only: read 128 bytes from Y
        // and two UV rows. No frame leaves the GPU in regular operation.
        // UV is interleaved NV12, so even bytes represent U and odd V.
        const bool emitUvProbe = nvdecUvProbe_ &&
            (nvdecDisplayFrames_ <= 4 ||
             (nvdecDisplayFrames_ % 500ULL) == 0ULL);
        if (emitUvProbe) {
            const auto probe = [&](const char* plane, CUdeviceptr base,
                                   std::size_t stride, std::size_t row,
                                   bool interleaved) {
                std::uint8_t bytes[128]{};
                const std::size_t length = std::min<std::size_t>(
                    sizeof(bytes), static_cast<std::size_t>(outputWidth_));
                const std::size_t x = (static_cast<std::size_t>(outputWidth_) - length) / 2U;
                const CUresult status = cuMemcpyDtoH_(
                    bytes, base + static_cast<CUdeviceptr>(row * stride + x), length);
                if (status != CUDA_SUCCESS) {
                    std::cerr << "NATIVE NVDEC UV PROBE frame=" << nvdecDisplayFrames_
                              << " plane=" << plane << " error=" << static_cast<int>(status)
                              << std::endl;
                    return;
                }
                std::uint64_t even = 0, odd = 0;
                unsigned zero = 0, maximum = 0, minimum = 255;
                for (std::size_t i = 0; i < length; ++i) {
                    if ((i & 1U) == 0U) even += bytes[i];
                    else odd += bytes[i];
                    zero += bytes[i] == 0;
                    minimum = std::min(minimum, static_cast<unsigned>(bytes[i]));
                    maximum = std::max(maximum, static_cast<unsigned>(bytes[i]));
                }
                const std::size_t evenCount = (length + 1U) / 2U;
                const std::size_t oddCount = length / 2U;
                std::cerr << "NATIVE NVDEC UV PROBE frame=" << nvdecDisplayFrames_
                          << " plane=" << plane
                          << " pitch=" << stride << " row=" << row
                          << " bytes=" << length
                          << " avg0=" << (evenCount ? even / evenCount : 0)
                          << " avg1=" << (oddCount ? odd / oddCount : 0)
                          << " zeros=" << zero
                          << " min=" << minimum << " max=" << maximum
                          << " interleaved=" << (interleaved ? 1 : 0)
                          << std::endl;
            };
            probe("src_y", devicePtr, pitch,
                  static_cast<std::size_t>(outputHeight_ / 2), false);
            const CUdeviceptr uv = devicePtr +
                static_cast<CUdeviceptr>(pitch) * static_cast<CUdeviceptr>(outputHeight_);
            probe("src_uv_top", uv, pitch, 0U, true);
            probe("src_uv_mid", uv, pitch,
                  static_cast<std::size_t>(outputHeight_ / 4), true);
        }

        if (cuvidGetDecodeStatus_) {
            CUVIDGETDECODESTATUS decodeStatus{};
            if (cuvidGetDecodeStatus_(
                    decoder_, display->picture_index, &decodeStatus) == CUDA_SUCCESS &&
                (decodeStatus.decodeStatus == cuvidDecodeStatus_Error ||
                 decodeStatus.decodeStatus == cuvidDecodeStatus_Error_Concealed)) {
                ++nvdecCorruptFrames_;
                if (nvdecCorruptFrames_ <= 8 ||
                    (nvdecCorruptFrames_ % 100ULL) == 0ULL) {
                    std::cerr << "NATIVE NVDEC DECODE ERROR status="
                              << static_cast<int>(decodeStatus.decodeStatus)
                              << " picture=" << display->picture_index
                              << " count=" << nvdecCorruptFrames_ << std::endl;
                }
            }
        }

        // Repack NV12 planes on-device only. A dedicated, correctly pitched
        // CUDA allocation avoids binding NVENC directly to the NVDEC mapping.
        // Only a single frame is in flight (synchronous bitstream lock).
        if (safeGpuCopy_) {
            CUDA_MEMCPY2D copy{};
            copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
            copy.srcDevice = devicePtr;
            copy.srcPitch = pitch;
            copy.dstDevice = gpuCopyFrame_;
            copy.dstPitch = gpuCopyPitch_;
            copy.WidthInBytes = static_cast<std::size_t>(outputWidth_);
            copy.Height = static_cast<std::size_t>(outputHeight_);
            CUresult result = cuMemcpy2D_(&copy);
            if (result == CUDA_SUCCESS) {
                // The source UV offset follows NVIDIA's decoder sample:
                // mapped pitch * configured output surface height.
                copy.srcDevice = devicePtr +
                    static_cast<CUdeviceptr>(pitch) *
                    static_cast<CUdeviceptr>(outputHeight_);
                copy.dstDevice = gpuCopyFrame_ +
                    static_cast<CUdeviceptr>(gpuCopyPitch_) *
                    static_cast<CUdeviceptr>(outputHeight_);
                copy.Height = static_cast<std::size_t>(outputHeight_) / 2U;
                if (!neutralChromaDiag_) {
                    result = cuMemcpy2D_(&copy);
                } else {
                    // Force a neutral U=V=128 plane, preserving the entire Y
                    // plane. Wait for completion before NVENC consumes it.
                    result = cuMemsetD2D8Async_(
                        copy.dstDevice, gpuCopyPitch_, 128U,
                        static_cast<std::size_t>(outputWidth_),
                        static_cast<std::size_t>(outputHeight_) / 2U,
                        nullptr);
                    if (result == CUDA_SUCCESS) result = cuCtxSynchronize_();
                }
            }
            if (result != CUDA_SUCCESS) {
                (void)cuvidUnmapVideoFrame_(decoder_, devicePtr);
                failCallback("NVDEC safe-copy CUDA device copy failed: " +
                             std::to_string(static_cast<int>(result)));
                return 0;
            }
            if (emitUvProbe) {
                std::uint8_t bytes[128]{};
                const std::size_t length = std::min<std::size_t>(
                    sizeof(bytes), static_cast<std::size_t>(outputWidth_));
                const std::size_t x = (static_cast<std::size_t>(outputWidth_) - length) / 2U;
                const CUresult stat = cuMemcpyDtoH_(
                    bytes, gpuCopyFrame_ +
                        static_cast<CUdeviceptr>(gpuCopyPitch_) *
                        static_cast<CUdeviceptr>(outputHeight_ + outputHeight_ / 4) +
                        static_cast<CUdeviceptr>(x), length);
                if (stat == CUDA_SUCCESS) {
                    std::uint64_t u = 0, v = 0;
                    for (std::size_t j = 0; j + 1 < length; j += 2U) {
                        u += bytes[j]; v += bytes[j + 1];
                    }
                    const std::size_t pairs = length / 2U;
                    std::cerr << "NATIVE NVDEC UV PROBE frame=" << nvdecDisplayFrames_
                              << " plane=dst_uv_mid"
                              << " pitch=" << gpuCopyPitch_
                              << " avg0=" << (pairs ? u / pairs : 0)
                              << " avg1=" << (pairs ? v / pairs : 0)
                              << " status=ok" << std::endl;
                } else {
                    std::cerr << "NATIVE NVDEC UV PROBE frame=" << nvdecDisplayFrames_
                              << " plane=dst_uv_mid error=" << static_cast<int>(stat)
                              << std::endl;
                }
            }
        }

        NV_ENC_REGISTER_RESOURCE registration{};
        registration.version = NV_ENC_REGISTER_RESOURCE_VER;
        registration.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
        registration.width = static_cast<std::uint32_t>(outputWidth_);
        registration.height = static_cast<std::uint32_t>(outputHeight_);
        registration.pitch = static_cast<std::uint32_t>(
            safeGpuCopy_ ? gpuCopyPitch_ : pitch);
        registration.resourceToRegister = reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(
                safeGpuCopy_ ? gpuCopyFrame_ : devicePtr));
        registration.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
        registration.bufferUsage = NV_ENC_INPUT_IMAGE;

        bool resourceRegistered = false;
        bool resourceMapped = false;
        NV_ENC_MAP_INPUT_RESOURCE mapped{};
        mapped.version = NV_ENC_MAP_INPUT_RESOURCE_VER;

        auto cleanup = [&]() {
            if (resourceMapped && api_.nvEncUnmapInputResource)
                (void)api_.nvEncUnmapInputResource(encoder_, mapped.mappedResource);
            if (resourceRegistered && api_.nvEncUnregisterResource)
                (void)api_.nvEncUnregisterResource(
                    encoder_, registration.registeredResource);
            (void)cuvidUnmapVideoFrame_(decoder_, devicePtr);
        };

        NVENCSTATUS status = NV_ENC_SUCCESS;
        if (nvencSoftwareInputTest_) {
            // Download the exact same NV12 layout that the software decoder
            // writes: Y followed by interleaved UV, both with *width* bytes
            // per row.  Then copy row-by-row into NVENC's own locked buffer
            // using the pitch returned by nvEncLockInputBuffer.  This isolates
            // CUDA resource registration from NVDEC's decoded pixel values.
            if (!nvencSoftwareInputBuffer_ || hostNv12_.empty()) {
                cleanup();
                failCallback("NVENC software-input diagnostic buffer unavailable");
                return 0;
            }
            const CUdeviceptr source =
                safeGpuCopy_ ? gpuCopyFrame_ : devicePtr;
            const std::size_t sourcePitch =
                safeGpuCopy_ ? gpuCopyPitch_ : pitch;
            const std::size_t rowBytes =
                static_cast<std::size_t>(outputWidth_);
            const std::size_t lumaRows =
                static_cast<std::size_t>(outputHeight_);
            CUDA_MEMCPY2D transfer{};
            transfer.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            transfer.dstMemoryType = CU_MEMORYTYPE_HOST;
            transfer.srcDevice = source;
            transfer.srcPitch = sourcePitch;
            transfer.dstHost = hostNv12_.data();
            transfer.dstPitch = rowBytes;
            transfer.WidthInBytes = rowBytes;
            transfer.Height = lumaRows;
            CUresult copyStatus = cuMemcpy2D_(&transfer);
            if (copyStatus == CUDA_SUCCESS) {
                transfer.srcDevice = source +
                    static_cast<CUdeviceptr>(sourcePitch) *
                    static_cast<CUdeviceptr>(lumaRows);
                transfer.dstHost = hostNv12_.data() + rowBytes * lumaRows;
                transfer.Height = lumaRows / 2U;
                copyStatus = cuMemcpy2D_(&transfer);
            }
            if (copyStatus != CUDA_SUCCESS) {
                cleanup();
                failCallback("NVENC software-input CUDA NV12 download failed: " +
                             std::to_string(static_cast<int>(copyStatus)));
                return 0;
            }
            NV_ENC_LOCK_INPUT_BUFFER inputLock{};
            inputLock.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
            inputLock.inputBuffer = nvencSoftwareInputBuffer_;
            status = api_.nvEncLockInputBuffer(encoder_, &inputLock);
            if (status != NV_ENC_SUCCESS) {
                cleanup();
                failCallback("NVENC software-input lock failed: " +
                             std::to_string(static_cast<int>(status)));
                return 0;
            }
            if (!inputLock.bufferDataPtr || inputLock.pitch < rowBytes) {
                (void)api_.nvEncUnlockInputBuffer(
                    encoder_, nvencSoftwareInputBuffer_);
                cleanup();
                failCallback("NVENC software-input invalid pitch or pointer");
                return 0;
            }
            auto* dstY = static_cast<std::uint8_t*>(inputLock.bufferDataPtr);
            auto* dstUV = dstY +
                static_cast<std::size_t>(inputLock.pitch) * lumaRows;
            const auto* srcY = hostNv12_.data();
            const auto* srcUV = srcY + rowBytes * lumaRows;
            for (std::size_t row = 0; row < lumaRows; ++row)
                std::memcpy(dstY + row * inputLock.pitch,
                            srcY + row * rowBytes, rowBytes);
            for (std::size_t row = 0; row < lumaRows / 2U; ++row)
                std::memcpy(dstUV + row * inputLock.pitch,
                            srcUV + row * rowBytes, rowBytes);
            registration.pitch = inputLock.pitch;
            status = api_.nvEncUnlockInputBuffer(
                encoder_, nvencSoftwareInputBuffer_);
            if (status != NV_ENC_SUCCESS) {
                cleanup();
                failCallback("NVENC software-input unlock failed: " +
                             std::to_string(static_cast<int>(status)));
                return 0;
            }
            mapped.mappedResource = nvencSoftwareInputBuffer_;
            mapped.mappedBufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
        } else {
            status = api_.nvEncRegisterResource(encoder_, &registration);
            if (status != NV_ENC_SUCCESS || !registration.registeredResource) {
                cleanup();
                failCallback("NVENC register NVDEC CUDA surface failed: " +
                             std::to_string(static_cast<int>(status)));
                return 0;
            }
            resourceRegistered = true;
            mapped.registeredResource = registration.registeredResource;
            status = api_.nvEncMapInputResource(encoder_, &mapped);
            if (status != NV_ENC_SUCCESS || !mapped.mappedResource) {
                cleanup();
                failCallback("NVENC map NVDEC CUDA surface failed: " +
                             std::to_string(static_cast<int>(status)));
                return 0;
            }
            resourceMapped = true;
        }

        const std::uint64_t frameNumber = frameIndex_++;
        const bool forceRandomAccess =
            frameNumber == 0 ||
            (gopFrames_ != 0 && (frameNumber % gopFrames_) == 0);

        NV_ENC_PIC_PARAMS pic{};
        pic.version = NV_ENC_PIC_PARAMS_VER;
        pic.inputWidth = static_cast<std::uint32_t>(outputWidth_);
        pic.inputHeight = static_cast<std::uint32_t>(outputHeight_);
        pic.inputPitch = registration.pitch;
        pic.frameIdx = static_cast<std::uint32_t>(frameNumber & 0xffffffffULL);
        pic.inputBuffer = mapped.mappedResource;
        pic.bufferFmt = mapped.mappedBufferFmt;
        pic.outputBitstream = bitstream_;
        pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        pic.inputTimeStamp = pts;
        if (forceRandomAccess) {
            pic.encodePicFlags |=
                NV_ENC_PIC_FLAG_FORCEIDR |
                NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        }

        status = api_.nvEncEncodePicture(encoder_, &pic);
        if (status == NV_ENC_ERR_NEED_MORE_INPUT) {
            cleanup();
            failCallback(
                "NVENC zero-copy requested buffered input despite zero-delay profile");
            return 0;
        }
        if (status != NV_ENC_SUCCESS) {
            cleanup();
            failCallback("NVENC zero-copy encode failed: " +
                         std::to_string(static_cast<int>(status)));
            return 0;
        }

        NV_ENC_LOCK_BITSTREAM lock{};
        lock.version = NV_ENC_LOCK_BITSTREAM_VER;
        lock.outputBitstream = bitstream_;
        lock.doNotWait = 0;
        status = api_.nvEncLockBitstream(encoder_, &lock);
        if (status != NV_ENC_SUCCESS) {
            cleanup();
            failCallback("NVENC zero-copy lock bitstream failed: " +
                         std::to_string(static_cast<int>(status)));
            return 0;
        }

        EncodedVideoFrame encoded;
        const auto* bytes = static_cast<const std::uint8_t*>(
            lock.bitstreamBufferPtr);
        if (bytes && lock.bitstreamSizeInBytes != 0) {
            encoded.data.assign(bytes, bytes + lock.bitstreamSizeInBytes);
        }
        encoded.hasPts = hasPts;
        encoded.hasDts = hasPts;
        encoded.pts90k = hasPts ? lock.outputTimeStamp : 0;
        encoded.dts90k = encoded.pts90k;
        encoded.keyFrame = forceRandomAccess ||
            lock.pictureType == NV_ENC_PIC_TYPE_IDR ||
            lock.pictureType == NV_ENC_PIC_TYPE_I;
        (void)api_.nvEncUnlockBitstream(encoder_, bitstream_);

        cleanup();
        if (!encoded.data.empty()) callbackOutput_->push_back(std::move(encoded));

        if (forceRandomAccess) {
            ++forcedIdrCount_;
            if (forcedIdrCount_ <= 3 || (forcedIdrCount_ % 30ULL) == 0ULL) {
                std::cerr << "NATIVE NVIDIA ZERO-COPY RANDOM ACCESS codec="
                          << codecName(config_.outputCodec)
                          << " frame=" << frameNumber
                          << " count=" << forcedIdrCount_
                          << " force_idr=1 parameter_sets=1"
                          << std::endl;
            }
        }
        return 1;
    }

    bool failCallback(std::string message) {
        if (callbackError_.empty()) callbackError_ = std::move(message);
        return false;
    }

    bool createEncoder(std::string& error) {
        error.clear();
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
        open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        open.apiVersion = NVENCAPI_VERSION;
        open.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
        open.device = context_;
        NVENCSTATUS status = api_.nvEncOpenEncodeSessionEx(&open, &encoder_);
        if (status != NV_ENC_SUCCESS || !encoder_) {
            error = "NVENC zero-copy open session failed: " +
                    std::to_string(static_cast<int>(status));
            return false;
        }

        const GUID codecGuid = nvencCodecGuid(config_.outputCodec);
        if (!validateEncoderCapabilities(codecGuid, error)) {
            destroyEncoder();
            return false;
        }

        NV_ENC_CONFIG encodeConfig{};
        encodeConfig.version = NV_ENC_CONFIG_VER;
        NV_ENC_PRESET_CONFIG preset{};
        preset.version = NV_ENC_PRESET_CONFIG_VER;
        preset.presetCfg.version = NV_ENC_CONFIG_VER;
#if defined(NVENCAPI_MAJOR_VERSION) && NVENCAPI_MAJOR_VERSION >= 10
        GUID presetGuid = NV_ENC_PRESET_P5_GUID;
        const char* presetName = "p5";
        const auto selectPreset = [&](const std::string& name) {
            if (name == "fastest" || name == "p1") { presetName = "p1"; return NV_ENC_PRESET_P1_GUID; }
            if (name == "fast" || name == "p2" || name == "p3") { presetName = "p2"; return NV_ENC_PRESET_P2_GUID; }
            if (name == "quality" || name == "p6") { presetName = "p6"; return NV_ENC_PRESET_P6_GUID; }
            if (name == "best" || name == "p7") { presetName = "p7"; return NV_ENC_PRESET_P7_GUID; }
            presetName = "p5";
            return NV_ENC_PRESET_P5_GUID;
        };
        presetGuid = selectPreset(config_.preset);
#else
        GUID presetGuid = NV_ENC_PRESET_LOW_LATENCY_HP_GUID;
        const char* presetName = "legacy-low-latency-hp";
#endif
        bool modernLowLatency = false;
        bool presetLoaded = false;
#if defined(NVENCAPI_MAJOR_VERSION) && NVENCAPI_MAJOR_VERSION >= 10
        if (api_.nvEncGetEncodePresetConfigEx) {
            if (api_.nvEncGetEncodePresetConfigEx(
                    encoder_, codecGuid, presetGuid,
                    NV_ENC_TUNING_INFO_LOW_LATENCY,
                    &preset) == NV_ENC_SUCCESS) {
                modernLowLatency = true;
                presetLoaded = true;
            }
        }
#endif
        if (!presetLoaded) {
            preset = {};
            preset.version = NV_ENC_PRESET_CONFIG_VER;
            preset.presetCfg.version = NV_ENC_CONFIG_VER;
            if (!api_.nvEncGetEncodePresetConfig ||
                api_.nvEncGetEncodePresetConfig(
                    encoder_, codecGuid, presetGuid, &preset) != NV_ENC_SUCCESS) {
                error = "NVENC low-latency preset configuration unavailable";
                destroyEncoder();
                return false;
            }
        }
        encodeConfig = preset.presetCfg;
        encodeConfig.version = NV_ENC_CONFIG_VER;
        encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        encodeConfig.rcParams.averageBitRate = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(config_.bitrate, 0xffffffffULL));
        encodeConfig.rcParams.maxBitRate = encodeConfig.rcParams.averageBitRate;
        const double fps = config_.fps > 0.0 ? config_.fps : 25.0;
        gopFrames_ = std::max<std::uint64_t>(
            1ULL, static_cast<std::uint64_t>(
                config_.gopFrames > 0 ? config_.gopFrames :
                std::llround(fps * 2.0)));
        encodeConfig.gopLength = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(gopFrames_, 0xffffffffULL));
        if (config_.bFrames > 0 || config_.openGop) {
            std::cerr << "NATIVE VIDEO OPTIONS backend=nvidia-zero-copy"
                      << " bframes_requested=" << config_.bFrames
                      << " open_gop_requested=" << (config_.openGop ? 1 : 0)
                      << " applied_bframes=0 applied_open_gop=0"
                      << " reason=live_zero_reorder"
                      << std::endl;
        }
        encodeConfig.frameIntervalP = 1;
        encodeConfig.rcParams.enableLookahead = 0;
        encodeConfig.rcParams.zeroReorderDelay = 1;
        encodeConfig.rcParams.enableAQ = 1;
        encodeConfig.rcParams.aqStrength = static_cast<std::uint32_t>(
            std::clamp<std::uint32_t>(config_.aqStrength, 1U, 15U));
#if defined(NVENCAPI_MAJOR_VERSION) && NVENCAPI_MAJOR_VERSION >= 10
        encodeConfig.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
#endif
        const std::uint64_t roundedFps = std::max<std::uint64_t>(
            1ULL, static_cast<std::uint64_t>(std::llround(fps)));
        const std::uint64_t frameBudget = std::max<std::uint64_t>(
            1ULL, static_cast<std::uint64_t>(encodeConfig.rcParams.averageBitRate) /
                roundedFps);
        const std::uint64_t vbv = std::min<std::uint64_t>(
            0xffffffffULL, frameBudget * 2ULL);
        encodeConfig.rcParams.vbvBufferSize = static_cast<std::uint32_t>(vbv);
        encodeConfig.rcParams.vbvInitialDelay = encodeConfig.rcParams.vbvBufferSize;
        if (config_.outputCodec == mpegts::ElementaryCodec::H264) {
            encodeConfig.profileGUID = NV_ENC_CODEC_PROFILE_AUTOSELECT_GUID;
            if (config_.profile == "main")
                encodeConfig.profileGUID = NV_ENC_H264_PROFILE_MAIN_GUID;
            else if (config_.profile == "high")
                encodeConfig.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
            encodeConfig.encodeCodecConfig.h264Config.level =
                config_.level == "3.1" ? NV_ENC_LEVEL_H264_31 :
                config_.level == "4.0" ? NV_ENC_LEVEL_H264_4 :
                config_.level == "4.1" ? NV_ENC_LEVEL_H264_41 :
                config_.level == "5.0" ? NV_ENC_LEVEL_H264_5 :
                config_.level == "5.1" ? NV_ENC_LEVEL_H264_51 :
                NV_ENC_LEVEL_AUTOSELECT;
            if (config_.refFrames > 0)
                encodeConfig.encodeCodecConfig.h264Config.maxNumRefFrames =
                    std::min<std::uint32_t>(config_.refFrames, 4U);
            encodeConfig.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
            encodeConfig.encodeCodecConfig.h264Config.outputAUD = 1;
            encodeConfig.encodeCodecConfig.h264Config.idrPeriod = encodeConfig.gopLength;
        } else {
            encodeConfig.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
            encodeConfig.encodeCodecConfig.hevcConfig.outputAUD = 1;
            encodeConfig.encodeCodecConfig.hevcConfig.idrPeriod = encodeConfig.gopLength;
        }

        NV_ENC_INITIALIZE_PARAMS init{};
        init.version = NV_ENC_INITIALIZE_PARAMS_VER;
        init.encodeGUID = codecGuid;
        init.presetGUID = presetGuid;
        init.encodeWidth = static_cast<std::uint32_t>(outputWidth_);
        init.encodeHeight = static_cast<std::uint32_t>(outputHeight_);
        init.darWidth = static_cast<std::uint32_t>(outputWidth_);
        init.darHeight = static_cast<std::uint32_t>(outputHeight_);
        init.frameRateNum = static_cast<std::uint32_t>(
            std::max(1.0, std::round(fps)));
        init.frameRateDen = 1;
        init.enablePTD = 1;
#if defined(NVENCAPI_MAJOR_VERSION) && NVENCAPI_MAJOR_VERSION >= 10
        if (modernLowLatency)
            init.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
#endif
        init.maxEncodeWidth = static_cast<std::uint32_t>(outputWidth_);
        init.maxEncodeHeight = static_cast<std::uint32_t>(outputHeight_);
        init.encodeConfig = &encodeConfig;
        status = api_.nvEncInitializeEncoder(encoder_, &init);
        if (status != NV_ENC_SUCCESS) {
            error = "NVENC zero-copy initialize failed: " +
                    std::to_string(static_cast<int>(status));
            destroyEncoder();
            return false;
        }

        NV_ENC_CREATE_BITSTREAM_BUFFER bitstream{};
        bitstream.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        status = api_.nvEncCreateBitstreamBuffer(encoder_, &bitstream);
        if (status != NV_ENC_SUCCESS || !bitstream.bitstreamBuffer) {
            error = "NVENC zero-copy bitstream buffer creation failed: " +
                    std::to_string(static_cast<int>(status));
            destroyEncoder();
            return false;
        }
        bitstream_ = bitstream.bitstreamBuffer;

        std::cerr << "NATIVE NVIDIA ZERO-COPY NVENC gpu=" << gpuOrdinal_
                  << " codec=" << codecName(config_.outputCodec)
                  << " preset=" << presetName
                  << " profile=" << config_.profile
                  << " level=" << config_.level
                  << " refs=" << config_.refFrames
                  << " aq_strength=" << std::clamp<std::uint32_t>(config_.aqStrength, 1U, 15U)
                  << " tuning="
                  << (modernLowLatency ? "low-latency" : "legacy-low-latency")
                  << " rc=cbr bframes=0 lookahead=0 reorder_delay=0"
                  << " vbv_frames=2 spatial_aq=1 aq_strength=8"
                  << " gop_frames=" << gopFrames_
                  << std::endl;
        return true;
    }

    bool validateEncoderCapabilities(const GUID& codecGuid,
                                     std::string& error) {
        if (!api_.nvEncGetEncodeGUIDCount || !api_.nvEncGetEncodeGUIDs ||
            !api_.nvEncGetInputFormatCount || !api_.nvEncGetInputFormats ||
            !api_.nvEncRegisterResource || !api_.nvEncUnregisterResource ||
            !api_.nvEncMapInputResource || !api_.nvEncUnmapInputResource) {
            error = "NVENC CUDA resource APIs are unavailable";
            return false;
        }
        std::uint32_t guidCount = 0;
        if (api_.nvEncGetEncodeGUIDCount(encoder_, &guidCount) != NV_ENC_SUCCESS ||
            guidCount == 0) {
            error = "NVENC did not report any encode codecs";
            return false;
        }
        std::vector<GUID> guids(guidCount);
        std::uint32_t actualGuidCount = 0;
        if (api_.nvEncGetEncodeGUIDs(
                encoder_, guids.data(), guidCount, &actualGuidCount) != NV_ENC_SUCCESS) {
            error = "NVENC encode codec query failed";
            return false;
        }
        bool codecSupported = false;
        for (std::uint32_t i = 0; i < std::min(guidCount, actualGuidCount); ++i) {
            if (sameGuid(guids[i], codecGuid)) {
                codecSupported = true;
                break;
            }
        }
        if (!codecSupported) {
            error = std::string("NVENC GPU does not support requested ") +
                    codecName(config_.outputCodec) + " encoder";
            return false;
        }

        std::uint32_t formatCount = 0;
        if (api_.nvEncGetInputFormatCount(
                encoder_, codecGuid, &formatCount) != NV_ENC_SUCCESS ||
            formatCount == 0) {
            error = "NVENC input format query failed";
            return false;
        }
        std::vector<NV_ENC_BUFFER_FORMAT> formats(formatCount);
        std::uint32_t actualFormatCount = 0;
        if (api_.nvEncGetInputFormats(
                encoder_, codecGuid, formats.data(), formatCount,
                &actualFormatCount) != NV_ENC_SUCCESS) {
            error = "NVENC input format list query failed";
            return false;
        }
        if (std::find(formats.begin(),
                      formats.begin() + std::min(formatCount, actualFormatCount),
                      NV_ENC_BUFFER_FORMAT_NV12) ==
            formats.begin() + std::min(formatCount, actualFormatCount)) {
            error = "NVENC GPU does not support NV12 input";
            return false;
        }
        return true;
    }

    void destroyEncoder() noexcept {
        if (encoder_) {
            if (nvencSoftwareInputBuffer_ && api_.nvEncDestroyInputBuffer)
                (void)api_.nvEncDestroyInputBuffer(encoder_, nvencSoftwareInputBuffer_);
            if (bitstream_ && api_.nvEncDestroyBitstreamBuffer)
                (void)api_.nvEncDestroyBitstreamBuffer(encoder_, bitstream_);
            if (api_.nvEncDestroyEncoder)
                (void)api_.nvEncDestroyEncoder(encoder_);
        }
        bitstream_ = nullptr;
        nvencSoftwareInputBuffer_ = nullptr;
        hostNv12_.clear();
        encoder_ = nullptr;
        if (gpuCopyFrame_ && cuMemFree_) (void)cuMemFree_(gpuCopyFrame_);
        gpuCopyFrame_ = 0;
        gpuCopyPitch_ = 0;
    }

    void destroyDecoder() noexcept {
        if (decoder_ && cuvidDestroyDecoder_)
            (void)cuvidDestroyDecoder_(decoder_);
        decoder_ = nullptr;
    }

    void close() noexcept {
        if (context_ && cuCtxPushCurrent_ &&
            cuCtxPushCurrent_(context_) == CUDA_SUCCESS) {
            if (parser_ && cuvidDestroyVideoParser_)
                (void)cuvidDestroyVideoParser_(parser_);
            parser_ = nullptr;
            destroyEncoder();
            destroyDecoder();
            popContext();
        } else {
            parser_ = nullptr;
            encoder_ = nullptr;
            decoder_ = nullptr;
            bitstream_ = nullptr;
        }
        if (context_ && cuCtxDestroy_)
            (void)cuCtxDestroy_(context_);
        context_ = nullptr;

        if (nvencLib_) dlclose(nvencLib_);
        if (cuvidLib_) dlclose(cuvidLib_);
        if (cudaLib_) dlclose(cudaLib_);
        nvencLib_ = nullptr;
        cuvidLib_ = nullptr;
        cudaLib_ = nullptr;
        std::memset(&api_, 0, sizeof(api_));
    }

    NvidiaZeroCopyConfig config_;
    int gpuOrdinal_ = 0;
    int sourceWidth_ = 0;
    int sourceHeight_ = 0;
    int outputWidth_ = 0;
    int outputHeight_ = 0;

    void* cudaLib_ = nullptr;
    void* cuvidLib_ = nullptr;
    void* nvencLib_ = nullptr;

    tcuInit* cuInit_ = nullptr;
    tcuDeviceGet* cuDeviceGet_ = nullptr;
    tcuCtxCreate_v2* cuCtxCreate_ = nullptr;
    tcuCtxDestroy_v2* cuCtxDestroy_ = nullptr;
    tcuCtxPushCurrent_v2* cuCtxPushCurrent_ = nullptr;
    tcuCtxPopCurrent_v2* cuCtxPopCurrent_ = nullptr;
    tcuCtxSynchronize* cuCtxSynchronize_ = nullptr;
    tcuMemsetD2D8Async* cuMemsetD2D8Async_ = nullptr;
    tcuMemAllocPitch_v2* cuMemAllocPitch_ = nullptr;
    tcuMemcpyDtoH_v2* cuMemcpyDtoH_ = nullptr;
    tcuMemFree_v2* cuMemFree_ = nullptr;
    tcuMemcpy2D_v2* cuMemcpy2D_ = nullptr;

    tcuvidGetDecoderCaps* cuvidGetDecoderCaps_ = nullptr;
    tcuvidGetDecodeStatus* cuvidGetDecodeStatus_ = nullptr;
    tcuvidCreateVideoParser* cuvidCreateVideoParser_ = nullptr;
    tcuvidParseVideoData* cuvidParseVideoData_ = nullptr;
    tcuvidDestroyVideoParser* cuvidDestroyVideoParser_ = nullptr;
    tcuvidCreateDecoder* cuvidCreateDecoder_ = nullptr;
    tcuvidDestroyDecoder* cuvidDestroyDecoder_ = nullptr;
    tcuvidDecodePicture* cuvidDecodePicture_ = nullptr;
    tcuvidMapVideoFrame64* cuvidMapVideoFrame_ = nullptr;
    tcuvidUnmapVideoFrame64* cuvidUnmapVideoFrame_ = nullptr;

    CUdevice device_ = 0;
    CUcontext context_ = nullptr;
    CUvideoparser parser_ = nullptr;
    CUvideodecoder decoder_ = nullptr;
    bool safeGpuCopy_ = false;
    bool neutralChromaDiag_ = false;
    bool nvdecUvProbe_ = false;
    bool nvdecFirstFieldTest_ = false;
    bool nvencSoftwareInputTest_ = false;
    std::string nvdecVppMode_ = "adaptive";
    bool nvdecVppProbe_ = false;
    std::uint64_t nvdecDisplayFrames_ = 0;
    CUdeviceptr gpuCopyFrame_ = 0;
    std::size_t gpuCopyPitch_ = 0;
    std::uint64_t nvdecCorruptFrames_ = 0;

    NV_ENCODE_API_FUNCTION_LIST api_{};
    void* encoder_ = nullptr;
    NV_ENC_INPUT_PTR nvencSoftwareInputBuffer_ = nullptr;
    std::vector<std::uint8_t> hostNv12_;
    NV_ENC_OUTPUT_PTR bitstream_ = nullptr;

    std::vector<EncodedVideoFrame>* callbackOutput_ = nullptr;
    std::string callbackError_;
    bool timestampsSeen_ = false;
    bool fpsClockValid_ = false;
    std::uint64_t fpsNextPts90k_ = 0;
    std::uint64_t fpsDroppedFrames_ = 0;
    std::uint64_t gopFrames_ = 50;
    std::uint64_t frameIndex_ = 0;
    std::uint64_t forcedIdrCount_ = 0;
};

#endif

} // namespace

bool nvidiaZeroCopyRuntimeAvailable() noexcept {
#if defined(__linux__) && defined(DVBSTREAMER5_HAVE_NVDEC_HEADERS) && defined(DVBSTREAMER5_HAVE_NVENC_HEADERS)
    // Emergency runtime switch: keep CPU decode + ordinary NVENC available
    // if an NVIDIA driver fails to produce correct zero-copy pictures.
    const char* disable = std::getenv("DVBSTREAMER5_DISABLE_NVIDIA_ZERO_COPY");
    if (disable && std::strcmp(disable, "1") == 0) return false;
    return canDlopen("libcuda.so.1") &&
           canDlopen("libnvcuvid.so.1") &&
           canDlopen("libnvidia-encode.so.1");
#else
    return false;
#endif
}

std::unique_ptr<NvidiaZeroCopyTranscoder> createNvidiaZeroCopyTranscoder(
    const NvidiaZeroCopyConfig& config,
    std::string& error) {
    error.clear();
#if defined(__linux__) && defined(DVBSTREAMER5_HAVE_NVDEC_HEADERS) && defined(DVBSTREAMER5_HAVE_NVENC_HEADERS)
    auto transcoder = std::make_unique<NvidiaZeroCopyTranscoderImpl>(config);
    if (!transcoder->initialize(error)) return {};
    return transcoder;
#else
    (void)config;
    error = "NVIDIA zero-copy backend not built: NVDEC/NVENC SDK headers are unavailable";
    return {};
#endif
}

} // namespace dvbstreamer5::media::codec
