#include "media/NativeHardwareCodec.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <cctype>
#include <unistd.h>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#if defined(DVBSTREAMER5_HAVE_VPL)
#include <vpl/mfxdispatcher.h>
#include <vpl/mfxvideo.h>
#endif

#if defined(DVBSTREAMER5_HAVE_NVENC_HEADERS)
#include <nvEncodeAPI.h>
#endif

namespace dvbstreamer5::media::codec {
namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool canDlopen(const char* soname) {
    if (!soname) return false;
    void* h = dlopen(soname, RTLD_LAZY | RTLD_LOCAL);
    if (!h) return false;
    dlclose(h);
    return true;
}

bool haveRenderNode() {
#if defined(__linux__)
    for (int i = 128; i < 160; ++i) {
        const std::string path = "/dev/dri/renderD" + std::to_string(i);
        if (::access(path.c_str(), R_OK | W_OK) == 0) return true;
    }
#endif
    return false;
}

void i420ToNv12(const RawVideoFrame& in, std::uint8_t* y, std::uint8_t* uv, int pitch) {
    const int w = in.width;
    const int h = in.height;
    const std::uint8_t* srcY = in.i420.data();
    const std::uint8_t* srcU = srcY + static_cast<std::size_t>(w) * h;
    const std::uint8_t* srcV = srcU + static_cast<std::size_t>(w / 2) * (h / 2);
    for (int row = 0; row < h; ++row)
        std::memcpy(y + static_cast<std::size_t>(row) * pitch,
                    srcY + static_cast<std::size_t>(row) * w, static_cast<std::size_t>(w));
    for (int row = 0; row < h / 2; ++row) {
        std::uint8_t* dst = uv + static_cast<std::size_t>(row) * pitch;
        const std::uint8_t* u = srcU + static_cast<std::size_t>(row) * (w / 2);
        const std::uint8_t* v = srcV + static_cast<std::size_t>(row) * (w / 2);
        for (int x = 0; x < w / 2; ++x) {
            dst[2 * x] = u[x];
            dst[2 * x + 1] = v[x];
        }
    }
}

#if defined(DVBSTREAMER5_HAVE_VPL)
class QsvVplEncoder final : public VideoEncoder {
public:
    explicit QsvVplEncoder(mpegts::ElementaryCodec codec) : codec_(codec) {}
    ~QsvVplEncoder() override { close(); }

    bool configure(int width, int height, double fps, std::uint64_t bitrate,
                   std::string& error) override {
        error.clear();
        close();
        if (codec_ != mpegts::ElementaryCodec::H264 && codec_ != mpegts::ElementaryCodec::H265) {
            error = "oneVPL/QSV supports H.264 and HEVC only";
            return false;
        }
        if (width <= 0 || height <= 0 || (width & 1) || (height & 1)) {
            error = "oneVPL/QSV requires positive even dimensions";
            return false;
        }

        loader_ = MFXLoad();
        if (!loader_) { error = "oneVPL MFXLoad failed"; return false; }

        mfxConfig implCfg = MFXCreateConfig(loader_);
        if (implCfg) {
            mfxVariant v{};
            v.Type = MFX_VARIANT_TYPE_U32;
            v.Data.U32 = MFX_IMPL_TYPE_HARDWARE;
            (void)MFXSetConfigFilterProperty(implCfg,
                reinterpret_cast<const mfxU8*>("mfxImplDescription.Impl"), v);
        }
#if defined(MFX_ACCEL_MODE_VIA_VAAPI)
        mfxConfig accelCfg = MFXCreateConfig(loader_);
        if (accelCfg) {
            mfxVariant v{};
            v.Type = MFX_VARIANT_TYPE_U32;
            v.Data.U32 = MFX_ACCEL_MODE_VIA_VAAPI;
            (void)MFXSetConfigFilterProperty(accelCfg,
                reinterpret_cast<const mfxU8*>("mfxImplDescription.AccelerationMode"), v);
        }
#endif
        mfxStatus sts = MFXCreateSession(loader_, 0, &session_);
        if (sts < MFX_ERR_NONE || !session_) {
            error = "oneVPL hardware session unavailable: " + std::to_string(sts);
            close();
            return false;
        }

        std::memset(&params_, 0, sizeof(params_));
        params_.mfx.CodecId = codec_ == mpegts::ElementaryCodec::H264 ? MFX_CODEC_AVC : MFX_CODEC_HEVC;
        params_.mfx.TargetUsage = MFX_TARGETUSAGE_BEST_SPEED;
        params_.mfx.TargetKbps = static_cast<mfxU16>(std::min<std::uint64_t>(bitrate / 1000ULL, 65535ULL));
        params_.mfx.RateControlMethod = MFX_RATECONTROL_CBR;
        params_.mfx.GopPicSize = static_cast<mfxU16>(std::clamp<int>(static_cast<int>(std::lround((fps > 0 ? fps : 25.0) * 2.0)), 1, 65535));
        params_.mfx.GopRefDist = 1;
        params_.mfx.IdrInterval = 1;
        params_.mfx.NumSlice = 1;
        params_.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
        params_.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
        params_.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
        params_.mfx.FrameInfo.FrameRateExtN = static_cast<mfxU32>(std::max(1.0, std::round(fps > 0 ? fps : 25.0)));
        params_.mfx.FrameInfo.FrameRateExtD = 1;
        params_.mfx.FrameInfo.Width = static_cast<mfxU16>((width + 15) & ~15);
        params_.mfx.FrameInfo.Height = static_cast<mfxU16>((height + 15) & ~15);
        params_.mfx.FrameInfo.CropW = static_cast<mfxU16>(width);
        params_.mfx.FrameInfo.CropH = static_cast<mfxU16>(height);
        params_.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;
        params_.AsyncDepth = 1;

        sts = MFXVideoENCODE_Init(session_, &params_);
        if (sts < MFX_ERR_NONE) {
            error = "oneVPL MFXVideoENCODE_Init failed: " + std::to_string(sts);
            close();
            return false;
        }
        encoderInitialized_ = true;
        width_ = width;
        height_ = height;
        alignedWidth_ = params_.mfx.FrameInfo.Width;
        alignedHeight_ = params_.mfx.FrameInfo.Height;
        nv12_.assign(static_cast<std::size_t>(alignedWidth_) * alignedHeight_ * 3U / 2U, 0);
        bitstream_.assign(std::max<std::size_t>(4U * 1024U * 1024U,
            static_cast<std::size_t>(alignedWidth_) * alignedHeight_), 0);
        std::cerr << "NATIVE HW ENCODER backend=qsv-vaapi codec="
                  << (codec_ == mpegts::ElementaryCodec::H264 ? "h264" : "hevc")
                  << " size=" << width_ << "x" << height_
                  << " fps=" << params_.mfx.FrameInfo.FrameRateExtN
                  << " bitrate_kbps=" << (bitrate / 1000ULL) << std::endl;
        return true;
    }

    bool encode(const RawVideoFrame& input, std::vector<EncodedVideoFrame>& output,
                std::string& error) override {
        if (!encoderInitialized_) { error = "oneVPL/QSV encoder not configured"; return false; }
        if (input.width != width_ || input.height != height_) { error = "oneVPL/QSV geometry mismatch"; return false; }
        std::fill(nv12_.begin(), nv12_.end(), 0);
        auto* y = nv12_.data();
        auto* uv = y + static_cast<std::size_t>(alignedWidth_) * alignedHeight_;
        i420ToNv12(input, y, uv, alignedWidth_);

        mfxFrameSurface1 surface{};
        surface.Info = params_.mfx.FrameInfo;
        surface.Data.Y = y;
        surface.Data.UV = uv;
        surface.Data.Pitch = static_cast<mfxU16>(alignedWidth_);
        surface.Data.TimeStamp = input.hasPts ? input.pts90k : MFX_TIMESTAMP_UNKNOWN;
        return submit(&surface, input, output, error);
    }

    bool flush(std::vector<EncodedVideoFrame>& output, std::string& error) override {
        error.clear();
        if (!encoderInitialized_) return true;
        RawVideoFrame dummy{};
        for (int i = 0; i < 64; ++i) {
            const auto before = output.size();
            if (!submit(nullptr, dummy, output, error, true)) return false;
            if (output.size() == before) break;
        }
        return true;
    }

private:
    bool submit(mfxFrameSurface1* surface, const RawVideoFrame& input,
                std::vector<EncodedVideoFrame>& output, std::string& error,
                bool draining = false) {
        mfxBitstream bs{};
        bs.Data = bitstream_.data();
        bs.MaxLength = static_cast<mfxU32>(bitstream_.size());
        mfxSyncPoint sync{};
        mfxStatus sts = MFXVideoENCODE_EncodeFrameAsync(session_, nullptr, surface, &bs, &sync);
        if (sts == MFX_ERR_MORE_DATA) return true;
        if (sts == MFX_WRN_DEVICE_BUSY) {
            error = "oneVPL/QSV device busy";
            return false;
        }
        if (sts < MFX_ERR_NONE) {
            error = "oneVPL EncodeFrameAsync failed: " + std::to_string(sts);
            return false;
        }
        if (!sync) return true;
        sts = MFXVideoCORE_SyncOperation(session_, sync, 60000);
        if (sts < MFX_ERR_NONE) {
            error = "oneVPL SyncOperation failed: " + std::to_string(sts);
            return false;
        }
        if (bs.DataLength == 0) return true;
        EncodedVideoFrame frame;
        frame.data.assign(bs.Data + bs.DataOffset, bs.Data + bs.DataOffset + bs.DataLength);
        frame.hasPts = !draining && input.hasPts;
        frame.hasDts = !draining && input.hasDts;
        frame.pts90k = !draining && input.hasPts ? input.pts90k : 0;
        frame.dts90k = !draining && input.hasDts ? input.dts90k : frame.pts90k;
        frame.keyFrame = (bs.FrameType & (MFX_FRAMETYPE_IDR | MFX_FRAMETYPE_I)) != 0;
        output.push_back(std::move(frame));
        return true;
    }

    void close() {
        if (session_) {
            if (encoderInitialized_) MFXVideoENCODE_Close(session_);
            MFXClose(session_);
        }
        if (loader_) MFXUnload(loader_);
        session_ = nullptr;
        loader_ = nullptr;
        encoderInitialized_ = false;
        nv12_.clear();
        bitstream_.clear();
    }

    mpegts::ElementaryCodec codec_ = mpegts::ElementaryCodec::Unknown;
    mfxLoader loader_ = nullptr;
    mfxSession session_ = nullptr;
    mfxVideoParam params_{};
    bool encoderInitialized_ = false;
    int width_ = 0, height_ = 0, alignedWidth_ = 0, alignedHeight_ = 0;
    std::vector<std::uint8_t> nv12_;
    std::vector<std::uint8_t> bitstream_;
};
#endif

#if defined(DVBSTREAMER5_HAVE_NVENC_HEADERS)
class NvencEncoder final : public VideoEncoder {
public:
    explicit NvencEncoder(mpegts::ElementaryCodec codec) : codec_(codec) {}
    ~NvencEncoder() override { close(); }

    bool configure(int width, int height, double fps, std::uint64_t bitrate,
                   std::string& error) override {
        error.clear(); close();
        if (codec_ != mpegts::ElementaryCodec::H264 && codec_ != mpegts::ElementaryCodec::H265) {
            error = "NVENC supports H.264 and HEVC only"; return false;
        }
        if (width <= 0 || height <= 0 || (width & 1) || (height & 1)) {
            error = "NVENC requires positive even dimensions"; return false;
        }
        if (!loadCuda(error) || !loadNvenc(error)) { close(); return false; }
        if (cuInit_(0) != 0 || cuDeviceGet_(&device_, 0) != 0 || cuCtxCreate_(&cudaContext_, 0, device_) != 0) {
            error = "CUDA context creation failed"; close(); return false;
        }
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
        open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        open.apiVersion = NVENCAPI_VERSION;
        open.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
        open.device = cudaContext_;
        if (api_.nvEncOpenEncodeSessionEx(&open, &encoder_) != NV_ENC_SUCCESS || !encoder_) {
            error = "NVENC open session failed"; close(); return false;
        }

        NV_ENC_CONFIG cfg{}; cfg.version = NV_ENC_CONFIG_VER;
        NV_ENC_PRESET_CONFIG preset{}; preset.version = NV_ENC_PRESET_CONFIG_VER;
        preset.presetCfg.version = NV_ENC_CONFIG_VER;
        const GUID codecGuid = codec_ == mpegts::ElementaryCodec::H264 ? NV_ENC_CODEC_H264_GUID : NV_ENC_CODEC_HEVC_GUID;
#if defined(NV_ENC_PRESET_P1_GUID)
        const GUID presetGuid = NV_ENC_PRESET_P1_GUID;
#else
        const GUID presetGuid = NV_ENC_PRESET_LOW_LATENCY_HP_GUID;
#endif
        if (api_.nvEncGetEncodePresetConfig(encoder_, codecGuid, presetGuid, &preset) == NV_ENC_SUCCESS)
            cfg = preset.presetCfg;
        cfg.version = NV_ENC_CONFIG_VER;
        cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        cfg.rcParams.averageBitRate = static_cast<std::uint32_t>(std::min<std::uint64_t>(bitrate, 0xffffffffULL));
        cfg.rcParams.maxBitRate = cfg.rcParams.averageBitRate;
        cfg.gopLength = static_cast<std::uint32_t>(std::max(1.0, std::round((fps > 0 ? fps : 25.0) * 2.0)));
        cfg.frameIntervalP = 1;

        NV_ENC_INITIALIZE_PARAMS init{};
        init.version = NV_ENC_INITIALIZE_PARAMS_VER;
        init.encodeGUID = codecGuid;
        init.presetGUID = presetGuid;
        init.encodeWidth = width;
        init.encodeHeight = height;
        init.darWidth = width;
        init.darHeight = height;
        init.frameRateNum = static_cast<std::uint32_t>(std::max(1.0, std::round(fps > 0 ? fps : 25.0)));
        init.frameRateDen = 1;
        init.enablePTD = 1;
        init.maxEncodeWidth = width;
        init.maxEncodeHeight = height;
        init.encodeConfig = &cfg;
        if (api_.nvEncInitializeEncoder(encoder_, &init) != NV_ENC_SUCCESS) {
            error = "NVENC initialize failed"; close(); return false;
        }

        NV_ENC_CREATE_INPUT_BUFFER in{}; in.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
        in.width = width; in.height = height; in.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
        if (api_.nvEncCreateInputBuffer(encoder_, &in) != NV_ENC_SUCCESS) {
            error = "NVENC create input buffer failed"; close(); return false;
        }
        input_ = in.inputBuffer;
        NV_ENC_CREATE_BITSTREAM_BUFFER out{}; out.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        if (api_.nvEncCreateBitstreamBuffer(encoder_, &out) != NV_ENC_SUCCESS) {
            error = "NVENC create bitstream buffer failed"; close(); return false;
        }
        bitstream_ = out.bitstreamBuffer;
        width_ = width; height_ = height; fps_ = fps > 0 ? fps : 25.0;
        std::cerr << "NATIVE HW ENCODER backend=nvenc codec="
                  << (codec_ == mpegts::ElementaryCodec::H264 ? "h264" : "hevc")
                  << " size=" << width_ << "x" << height_
                  << " fps=" << fps_ << " bitrate_kbps=" << (bitrate / 1000ULL) << std::endl;
        return true;
    }

    bool encode(const RawVideoFrame& input, std::vector<EncodedVideoFrame>& output,
                std::string& error) override {
        error.clear();
        if (!encoder_ || !input_ || !bitstream_) { error = "NVENC encoder not configured"; return false; }
        if (input.width != width_ || input.height != height_) { error = "NVENC geometry mismatch"; return false; }
        NV_ENC_LOCK_INPUT_BUFFER lock{}; lock.version = NV_ENC_LOCK_INPUT_BUFFER_VER; lock.inputBuffer = input_;
        if (api_.nvEncLockInputBuffer(encoder_, &lock) != NV_ENC_SUCCESS) {
            error = "NVENC lock input failed"; return false;
        }
        auto* base = static_cast<std::uint8_t*>(lock.bufferDataPtr);
        auto* uv = base + static_cast<std::size_t>(lock.pitch) * height_;
        i420ToNv12(input, base, uv, static_cast<int>(lock.pitch));
        api_.nvEncUnlockInputBuffer(encoder_, input_);

        NV_ENC_PIC_PARAMS pic{}; pic.version = NV_ENC_PIC_PARAMS_VER;
        pic.inputWidth = width_; pic.inputHeight = height_;
        pic.inputPitch = 0;
        pic.inputBuffer = input_;
        pic.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
        pic.outputBitstream = bitstream_;
        pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        pic.inputTimeStamp = input.hasPts ? input.pts90k : frameIndex_++;
        const NVENCSTATUS st = api_.nvEncEncodePicture(encoder_, &pic);
        if (st != NV_ENC_SUCCESS && st != NV_ENC_ERR_NEED_MORE_INPUT) {
            error = "NVENC encode failed: " + std::to_string(static_cast<int>(st)); return false;
        }
        if (st == NV_ENC_ERR_NEED_MORE_INPUT) return true;
        return collect(input, output, error);
    }

    bool flush(std::vector<EncodedVideoFrame>& output, std::string& error) override {
        error.clear();
        if (!encoder_) return true;
        NV_ENC_PIC_PARAMS eos{}; eos.version = NV_ENC_PIC_PARAMS_VER; eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        const NVENCSTATUS st = api_.nvEncEncodePicture(encoder_, &eos);
        if (st != NV_ENC_SUCCESS && st != NV_ENC_ERR_NEED_MORE_INPUT) {
            error = "NVENC EOS failed: " + std::to_string(static_cast<int>(st)); return false;
        }
        return true;
    }

private:
    using CUdevice = int;
    using CUcontext = void*;
    using CUresult = int;
    using CuInit = CUresult (*)(unsigned int);
    using CuDeviceGet = CUresult (*)(CUdevice*, int);
    using CuCtxCreate = CUresult (*)(CUcontext*, unsigned int, CUdevice);
    using CuCtxDestroy = CUresult (*)(CUcontext);

    bool loadCuda(std::string& error) {
        cudaLib_ = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!cudaLib_) { error = "libcuda.so.1 not available"; return false; }
        cuInit_ = reinterpret_cast<CuInit>(dlsym(cudaLib_, "cuInit"));
        cuDeviceGet_ = reinterpret_cast<CuDeviceGet>(dlsym(cudaLib_, "cuDeviceGet"));
        cuCtxCreate_ = reinterpret_cast<CuCtxCreate>(dlsym(cudaLib_, "cuCtxCreate_v2"));
        if (!cuCtxCreate_) cuCtxCreate_ = reinterpret_cast<CuCtxCreate>(dlsym(cudaLib_, "cuCtxCreate"));
        cuCtxDestroy_ = reinterpret_cast<CuCtxDestroy>(dlsym(cudaLib_, "cuCtxDestroy_v2"));
        if (!cuCtxDestroy_) cuCtxDestroy_ = reinterpret_cast<CuCtxDestroy>(dlsym(cudaLib_, "cuCtxDestroy"));
        if (!cuInit_ || !cuDeviceGet_ || !cuCtxCreate_ || !cuCtxDestroy_) {
            error = "CUDA driver API symbols missing"; return false;
        }
        return true;
    }

    bool loadNvenc(std::string& error) {
        nvencLib_ = dlopen("libnvidia-encode.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!nvencLib_) { error = "libnvidia-encode.so.1 not available"; return false; }
        using Create = NVENCSTATUS (NVENCAPI *)(NV_ENCODE_API_FUNCTION_LIST*);
        auto create = reinterpret_cast<Create>(dlsym(nvencLib_, "NvEncodeAPICreateInstance"));
        if (!create) { error = "NvEncodeAPICreateInstance not found"; return false; }
        std::memset(&api_, 0, sizeof(api_)); api_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        if (create(&api_) != NV_ENC_SUCCESS) { error = "NvEncodeAPICreateInstance failed"; return false; }
        return true;
    }

    bool collect(const RawVideoFrame& input, std::vector<EncodedVideoFrame>& output, std::string& error) {
        NV_ENC_LOCK_BITSTREAM lock{}; lock.version = NV_ENC_LOCK_BITSTREAM_VER;
        lock.outputBitstream = bitstream_; lock.doNotWait = 0;
        const NVENCSTATUS st = api_.nvEncLockBitstream(encoder_, &lock);
        if (st != NV_ENC_SUCCESS) { error = "NVENC lock bitstream failed"; return false; }
        EncodedVideoFrame frame;
        auto* p = static_cast<const std::uint8_t*>(lock.bitstreamBufferPtr);
        frame.data.assign(p, p + lock.bitstreamSizeInBytes);
        frame.hasPts = input.hasPts; frame.hasDts = input.hasDts;
        frame.pts90k = input.pts90k; frame.dts90k = input.hasDts ? input.dts90k : input.pts90k;
        frame.keyFrame = lock.pictureType == NV_ENC_PIC_TYPE_IDR || lock.pictureType == NV_ENC_PIC_TYPE_I;
        api_.nvEncUnlockBitstream(encoder_, bitstream_);
        if (!frame.data.empty()) output.push_back(std::move(frame));
        return true;
    }

    void close() {
        if (encoder_) {
            if (input_) api_.nvEncDestroyInputBuffer(encoder_, input_);
            if (bitstream_) api_.nvEncDestroyBitstreamBuffer(encoder_, bitstream_);
            api_.nvEncDestroyEncoder(encoder_);
        }
        input_ = nullptr; bitstream_ = nullptr; encoder_ = nullptr;
        if (cudaContext_ && cuCtxDestroy_) cuCtxDestroy_(cudaContext_);
        cudaContext_ = nullptr;
        if (nvencLib_) dlclose(nvencLib_);
        if (cudaLib_) dlclose(cudaLib_);
        nvencLib_ = cudaLib_ = nullptr;
        std::memset(&api_, 0, sizeof(api_));
    }

    mpegts::ElementaryCodec codec_;
    void* cudaLib_ = nullptr;
    void* nvencLib_ = nullptr;
    CuInit cuInit_ = nullptr; CuDeviceGet cuDeviceGet_ = nullptr; CuCtxCreate cuCtxCreate_ = nullptr; CuCtxDestroy cuCtxDestroy_ = nullptr;
    CUdevice device_ = 0; CUcontext cudaContext_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api_{};
    void* encoder_ = nullptr;
    NV_ENC_INPUT_PTR input_ = nullptr;
    NV_ENC_OUTPUT_PTR bitstream_ = nullptr;
    int width_ = 0, height_ = 0; double fps_ = 25.0; std::uint64_t frameIndex_ = 0;
};
#endif

} // namespace

NativeHardwareCapabilities inspectNativeHardwareCapabilities() {
    NativeHardwareCapabilities c;
    c.vaapiRuntime = haveRenderNode() && (canDlopen("libva.so.2") || canDlopen("libva.so"));
#if defined(DVBSTREAMER5_HAVE_VPL)
    c.qsvAvailable = haveRenderNode() && (canDlopen("libvpl.so.2") || canDlopen("libvpl.so"));
    c.qsvH264 = c.qsvAvailable;
    c.qsvHevc = c.qsvAvailable;
    if (c.qsvAvailable) c.intelBackend = "oneVPL QSV via VAAPI";
#else
    c.intelBackend = c.vaapiRuntime ? "VAAPI runtime found; build with libvpl-dev for direct QSV/VAAPI encode" : "";
#endif
#if defined(DVBSTREAMER5_HAVE_NVENC_HEADERS)
    c.nvencAvailable = canDlopen("libnvidia-encode.so.1") && canDlopen("libcuda.so.1");
    c.nvencH264 = c.nvencAvailable;
    c.nvencHevc = c.nvencAvailable;
    if (c.nvencAvailable) c.nvencBackend = "NVIDIA NVENC SDK";
#else
    c.nvencBackend = canDlopen("libnvidia-encode.so.1")
        ? "NVENC runtime found; build with nvEncodeAPI.h to enable direct encode" : "";
#endif
    return c;
}

std::unique_ptr<VideoEncoder> createNativeHardwareVideoEncoder(
    mpegts::ElementaryCodec codec, const std::string& backend, std::string& error) {
    error.clear();
    const auto b = lower(backend);
    if (b == "intel" || b == "qsv" || b == "vaapi") {
#if defined(DVBSTREAMER5_HAVE_VPL)
        auto p = std::make_unique<QsvVplEncoder>(codec);
        return p;
#else
        error = "Intel QSV/VAAPI backend not built: install libvpl-dev and rebuild";
        return {};
#endif
    }
    if (b == "nvenc") {
#if defined(DVBSTREAMER5_HAVE_NVENC_HEADERS)
        auto p = std::make_unique<NvencEncoder>(codec);
        return p;
#else
        error = "NVENC backend not built: install NVIDIA Video Codec SDK headers (nvEncodeAPI.h) and rebuild";
        return {};
#endif
    }
    error = "unknown native hardware encoder backend: " + backend;
    return {};
}

} // namespace dvbstreamer5::media::codec
