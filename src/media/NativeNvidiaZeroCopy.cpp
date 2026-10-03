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
        if (requestedPixels > sourcePixels) {
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
        create.DeinterlaceMode = config_.deinterlace
            ? cudaVideoDeinterlaceMode_Adaptive
            : cudaVideoDeinterlaceMode_Weave;
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

        frameIndex_ = 0;
        forcedIdrCount_ = 0;
        fpsClockValid_ = false;
        fpsDroppedFrames_ = 0;
        std::cerr << "NATIVE NVIDIA ZERO-COPY active gpu=" << gpuOrdinal_
                  << " input=" << codecName(config_.inputCodec)
                  << " output=" << codecName(config_.outputCodec)
                  << " path=NVDEC->CUDA(NV12)->NVENC"
                  << " source=" << sourceWidth_ << "x" << sourceHeight_
                  << " encode=" << outputWidth_ << "x" << outputHeight_
                  << " deinterlace=" << (config_.deinterlace ? "nvdec-adaptive" : "off")
                  << " decode_copies=0 gpu_surface_copies=0"
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
        proc.second_field = 0;
        proc.top_field_first = display->top_field_first;
        proc.unpaired_field = display->repeat_first_field < 0 ? 1 : 0;

        CUdeviceptr devicePtr = 0;
        unsigned int pitch = 0;
        const CUresult mapStatus = cuvidMapVideoFrame_(
            decoder_, display->picture_index, &devicePtr, &pitch, &proc);
        if (mapStatus != CUDA_SUCCESS || devicePtr == 0 || pitch == 0) {
            failCallback("cuvidMapVideoFrame failed: " +
                         std::to_string(static_cast<int>(mapStatus)));
            return 0;
        }

        NV_ENC_REGISTER_RESOURCE registration{};
        registration.version = NV_ENC_REGISTER_RESOURCE_VER;
        registration.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
        registration.width = static_cast<std::uint32_t>(outputWidth_);
        registration.height = static_cast<std::uint32_t>(outputHeight_);
        registration.pitch = pitch;
        registration.resourceToRegister = reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(devicePtr));
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

        NVENCSTATUS status = api_.nvEncRegisterResource(
            encoder_, &registration);
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

        const std::uint64_t frameNumber = frameIndex_++;
        const bool forceRandomAccess =
            frameNumber == 0 ||
            (gopFrames_ != 0 && (frameNumber % gopFrames_) == 0);

        NV_ENC_PIC_PARAMS pic{};
        pic.version = NV_ENC_PIC_PARAMS_VER;
        pic.inputWidth = static_cast<std::uint32_t>(outputWidth_);
        pic.inputHeight = static_cast<std::uint32_t>(outputHeight_);
        pic.inputPitch = pitch;
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
        GUID presetGuid = NV_ENC_PRESET_P3_GUID;
        const char* presetName = "p3";
#else
        GUID presetGuid = NV_ENC_PRESET_LOW_LATENCY_HP_GUID;
        const char* presetName = "legacy-low-latency-hp";
#endif
        bool modernLowLatency = false;
        bool presetLoaded = false;
#if defined(NVENCAPI_MAJOR_VERSION) && NVENCAPI_MAJOR_VERSION >= 10
        if (api_.nvEncGetEncodePresetConfigEx) {
            if (api_.nvEncGetEncodePresetConfigEx(
                    encoder_, codecGuid, NV_ENC_PRESET_P3_GUID,
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
            1ULL, static_cast<std::uint64_t>(std::llround(fps * 2.0)));
        encodeConfig.gopLength = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(gopFrames_, 0xffffffffULL));
        encodeConfig.frameIntervalP = 1;
        encodeConfig.rcParams.enableLookahead = 0;
        encodeConfig.rcParams.zeroReorderDelay = 1;
        encodeConfig.rcParams.enableAQ = 1;
        encodeConfig.rcParams.aqStrength = 8;
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
        if (!encoder_) {
            bitstream_ = nullptr;
            return;
        }
        if (bitstream_ && api_.nvEncDestroyBitstreamBuffer)
            (void)api_.nvEncDestroyBitstreamBuffer(encoder_, bitstream_);
        bitstream_ = nullptr;
        if (api_.nvEncDestroyEncoder)
            (void)api_.nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
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

    tcuvidGetDecoderCaps* cuvidGetDecoderCaps_ = nullptr;
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

    NV_ENCODE_API_FUNCTION_LIST api_{};
    void* encoder_ = nullptr;
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
