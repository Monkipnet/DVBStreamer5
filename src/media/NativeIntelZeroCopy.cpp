#include "media/NativeIntelZeroCopy.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__) && defined(DVBSTREAMER5_HAVE_VPL)
#include <vpl/mfxdispatcher.h>
#include <vpl/mfxvideo.h>
#endif

namespace dvbstreamer5::media::codec {
namespace {

#if defined(__linux__) && defined(DVBSTREAMER5_HAVE_VPL)

mfxU32 codecId(mpegts::ElementaryCodec codec) {
    switch (codec) {
        case mpegts::ElementaryCodec::H264: return MFX_CODEC_AVC;
        case mpegts::ElementaryCodec::H265: return MFX_CODEC_HEVC;
        case mpegts::ElementaryCodec::Mpeg2Video: return MFX_CODEC_MPEG2;
        default: return 0;
    }
}

const char* codecName(mpegts::ElementaryCodec codec) {
    switch (codec) {
        case mpegts::ElementaryCodec::H264: return "h264";
        case mpegts::ElementaryCodec::H265: return "hevc";
        case mpegts::ElementaryCodec::Mpeg2Video: return "mpeg2";
        default: return "unknown";
    }
}

bool runtimeAtLeast21(mfxSession session) {
    mfxVersion version{};
    if (!session || MFXQueryVersion(session, &version) < MFX_ERR_NONE)
        return false;
    return version.Major > 2 || (version.Major == 2 && version.Minor >= 1);
}

void configureHardwareLoader(mfxLoader loader) {
    if (!loader) return;
    if (mfxConfig implCfg = MFXCreateConfig(loader)) {
        mfxVariant value{};
        value.Type = MFX_VARIANT_TYPE_U32;
        value.Data.U32 = MFX_IMPL_TYPE_HARDWARE;
        (void)MFXSetConfigFilterProperty(
            implCfg,
            reinterpret_cast<const mfxU8*>("mfxImplDescription.Impl"),
            value);
    }
#if defined(MFX_ACCEL_MODE_VIA_VAAPI)
    if (mfxConfig accelCfg = MFXCreateConfig(loader)) {
        mfxVariant value{};
        value.Type = MFX_VARIANT_TYPE_U32;
        value.Data.U32 = MFX_ACCEL_MODE_VIA_VAAPI;
        (void)MFXSetConfigFilterProperty(
            accelCfg,
            reinterpret_cast<const mfxU8*>(
                "mfxImplDescription.AccelerationMode"),
            value);
    }
#endif
}

void releaseSurface(mfxFrameSurface1*& surface) noexcept {
    if (surface && surface->FrameInterface && surface->FrameInterface->Release)
        (void)surface->FrameInterface->Release(surface);
    surface = nullptr;
}

bool retryableStatus(mfxStatus status) {
    return status == MFX_WRN_DEVICE_BUSY
#if defined(MFX_WRN_ALLOC_TIMEOUT_EXPIRED)
        || status == MFX_WRN_ALLOC_TIMEOUT_EXPIRED
#endif
        ;
}

class IntelZeroCopyTranscoderImpl final : public IntelZeroCopyTranscoder {
public:
    explicit IntelZeroCopyTranscoderImpl(IntelZeroCopyConfig config)
        : config_(std::move(config)) {}

    ~IntelZeroCopyTranscoderImpl() override { close(); }

    bool initialize(std::string& error) {
        error.clear();
        if (codecId(config_.inputCodec) == 0 ||
            (config_.outputCodec != mpegts::ElementaryCodec::H264 &&
             config_.outputCodec != mpegts::ElementaryCodec::H265)) {
            error = "Intel zero-copy supports MPEG-2/H.264/HEVC input and H.264/HEVC output";
            return false;
        }
        if (config_.width <= 0 || config_.height <= 0 ||
            (config_.width & 1) || (config_.height & 1)) {
            error = "Intel zero-copy requires positive even output dimensions";
            return false;
        }

        loader_ = MFXLoad();
        if (!loader_) {
            error = "oneVPL MFXLoad failed";
            return false;
        }
        configureHardwareLoader(loader_);
        const mfxStatus create = MFXCreateSession(loader_, 0, &session_);
        if (create < MFX_ERR_NONE || !session_) {
            error = "oneVPL hardware session unavailable: " +
                    std::to_string(create);
            return false;
        }
        if (!runtimeAtLeast21(session_)) {
            error = "oneVPL hardware runtime API 2.1+ required for zero-copy VPP surfaces";
            return false;
        }

        std::memset(&decodeParams_, 0, sizeof(decodeParams_));
        decodeParams_.mfx.CodecId = codecId(config_.inputCodec);
        decodeParams_.IOPattern = MFX_IOPATTERN_OUT_VIDEO_MEMORY;
        decodeParams_.AsyncDepth = 1;

        if (!initializeEncoder(error)) return false;

        std::cerr << "NATIVE INTEL ZERO-COPY session input="
                  << codecName(config_.inputCodec)
                  << " output=" << codecName(config_.outputCodec)
                  << " runtime=oneVPL-2.1+ memory=video"
                  << std::endl;
        return true;
    }

    bool process(const std::uint8_t* data, std::size_t size,
                 std::uint64_t pts90k, bool hasPts,
                 std::vector<EncodedVideoFrame>& output,
                 std::string& error) override {
        error.clear();
        if (!session_ || !encoderInitialized_) {
            error = "Intel zero-copy session is not initialized";
            return false;
        }
        if (!data || size == 0) return true;

        if (!decoderInitialized_) {
            if (headerBuffer_.size() + size > kMaxHeaderBytes) {
                error = "Intel zero-copy decoder header exceeded 4 MiB";
                return false;
            }
            if (headerBuffer_.empty()) {
                headerPts90k_ = pts90k;
                headerHasPts_ = hasPts;
            }
            headerBuffer_.insert(headerBuffer_.end(), data, data + size);
            if (!initializeDecoderAndVpp(error)) {
                if (error.empty()) return true;
                return false;
            }
            if (!decoderInitialized_) return true;

            const bool ok = decodeBytes(
                headerBuffer_.data(), headerBuffer_.size(),
                headerPts90k_, headerHasPts_, output, error);
            headerBuffer_.clear();
            return ok;
        }

        return decodeBytes(data, size, pts90k, hasPts, output, error);
    }

    bool flush(std::vector<EncodedVideoFrame>& output,
               std::string& error) override {
        error.clear();
        if (!session_) return true;

        if (decoderInitialized_) {
            for (int i = 0; i < 128; ++i) {
                mfxFrameSurface1* decoded = nullptr;
                mfxSyncPoint sync{};
                mfxStatus status = callDecode(nullptr, decoded, sync);
                if (status == MFX_ERR_MORE_DATA) break;
                if (status < MFX_ERR_NONE) {
                    error = "oneVPL zero-copy decoder drain failed: " +
                            std::to_string(status);
                    releaseSurface(decoded);
                    return false;
                }
                if (decoded) {
                    const bool ok = processDecodedSurface(decoded, output, error);
                    releaseSurface(decoded);
                    if (!ok) return false;
                }
            }
        }

        if (vppInitialized_) {
            for (int i = 0; i < 128; ++i) {
                mfxFrameSurface1* vpp = nullptr;
                mfxStatus status = callVpp(nullptr, vpp);
                if (status == MFX_ERR_MORE_DATA) break;
                if (status < MFX_ERR_NONE) {
                    error = "oneVPL zero-copy VPP drain failed: " +
                            std::to_string(status);
                    releaseSurface(vpp);
                    return false;
                }
                if (vpp) {
                    const bool ok = encodeSurface(vpp, output, error);
                    releaseSurface(vpp);
                    if (!ok) return false;
                }
            }
        }

        for (int i = 0; i < 128; ++i) {
            const std::size_t before = output.size();
            if (!encodeSurface(nullptr, output, error, true)) return false;
            if (output.size() == before) break;
        }
        return true;
    }

    int sourceWidth() const noexcept override { return sourceWidth_; }
    int sourceHeight() const noexcept override { return sourceHeight_; }
    int outputWidth() const noexcept override { return outputWidth_; }
    int outputHeight() const noexcept override { return outputHeight_; }

private:
    bool initializeEncoder(std::string& error) {
        std::memset(&encodeParams_, 0, sizeof(encodeParams_));
        encodeParams_.mfx.CodecId = codecId(config_.outputCodec);
        encodeParams_.mfx.TargetUsage = MFX_TARGETUSAGE_BEST_SPEED;
        encodeParams_.mfx.TargetKbps = static_cast<mfxU16>(
            std::min<std::uint64_t>(config_.bitrate / 1000ULL, 65535ULL));
        encodeParams_.mfx.RateControlMethod = MFX_RATECONTROL_CBR;
        const double fps = config_.fps > 0.0 ? config_.fps : 25.0;
        gopFrames_ = std::max<std::uint64_t>(
            1ULL, static_cast<std::uint64_t>(std::llround(fps * 2.0)));
        encodeParams_.mfx.GopPicSize = static_cast<mfxU16>(
            std::min<std::uint64_t>(gopFrames_, 65535ULL));
        encodeParams_.mfx.GopRefDist = 1;
        // AVC: 0 means every I is IDR. HEVC: 1 means every I is IDR.
        encodeParams_.mfx.IdrInterval =
            config_.outputCodec == mpegts::ElementaryCodec::H264 ? 0 : 1;
        encodeParams_.mfx.NumSlice = 1;
        encodeParams_.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
        encodeParams_.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
        encodeParams_.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
        encodeParams_.mfx.FrameInfo.FrameRateExtN = static_cast<mfxU32>(
            std::max(1.0, std::round(fps)));
        encodeParams_.mfx.FrameInfo.FrameRateExtD = 1;
        encodeParams_.mfx.FrameInfo.Width = static_cast<mfxU16>(
            (config_.width + 15) & ~15);
        encodeParams_.mfx.FrameInfo.Height = static_cast<mfxU16>(
            (config_.height + 15) & ~15);
        encodeParams_.mfx.FrameInfo.CropW = static_cast<mfxU16>(config_.width);
        encodeParams_.mfx.FrameInfo.CropH = static_cast<mfxU16>(config_.height);
        encodeParams_.IOPattern = MFX_IOPATTERN_IN_VIDEO_MEMORY;
        encodeParams_.AsyncDepth = 1;

        const mfxStatus status = MFXVideoENCODE_Init(session_, &encodeParams_);
        if (status < MFX_ERR_NONE) {
            error = "oneVPL zero-copy MFXVideoENCODE_Init failed: " +
                    std::to_string(status);
            return false;
        }
        encoderInitialized_ = true;
        outputWidth_ = config_.width;
        outputHeight_ = config_.height;
        bitstream_.assign(
            std::max<std::size_t>(8U * 1024U * 1024U,
                static_cast<std::size_t>(encodeParams_.mfx.FrameInfo.Width) *
                encodeParams_.mfx.FrameInfo.Height * 2U),
            0);
        return true;
    }

    bool initializeDecoderAndVpp(std::string& error) {
        mfxBitstream header{};
        header.Data = headerBuffer_.data();
        header.MaxLength = static_cast<mfxU32>(std::min<std::size_t>(
            headerBuffer_.size(), std::numeric_limits<mfxU32>::max()));
        header.DataLength = header.MaxLength;

        const mfxStatus headerStatus =
            MFXVideoDECODE_DecodeHeader(session_, &header, &decodeParams_);
        if (headerStatus == MFX_ERR_MORE_DATA) {
            error.clear();
            return false;
        }
        if (headerStatus < MFX_ERR_NONE) {
            error = "oneVPL zero-copy DecodeHeader failed: " +
                    std::to_string(headerStatus);
            return false;
        }

        const auto& info = decodeParams_.mfx.FrameInfo;
        sourceWidth_ = info.CropW ? info.CropW : info.Width;
        sourceHeight_ = info.CropH ? info.CropH : info.Height;
        if (sourceWidth_ <= 0 || sourceHeight_ <= 0) {
            error = "oneVPL zero-copy decoder reported invalid source geometry";
            return false;
        }
        if (info.ChromaFormat != MFX_CHROMAFORMAT_YUV420 ||
            (info.FourCC != 0 && info.FourCC != MFX_FOURCC_NV12)) {
            error = "Intel zero-copy currently requires 8-bit 4:2:0 NV12 decoder output";
            return false;
        }

        // Do not upscale beyond source geometry. This mirrors the NVIDIA
        // zero-copy path and avoids wasting GPU bandwidth on live television.
        const std::uint64_t sourcePixels =
            static_cast<std::uint64_t>(sourceWidth_) * sourceHeight_;
        const std::uint64_t requestedPixels =
            static_cast<std::uint64_t>(outputWidth_) * outputHeight_;
        if (requestedPixels > sourcePixels) {
            outputWidth_ = sourceWidth_ & ~1;
            outputHeight_ = sourceHeight_ & ~1;
        }
        outputPicStruct_ = config_.deinterlace
            ? MFX_PICSTRUCT_PROGRESSIVE
            : (info.PicStruct ? info.PicStruct : MFX_PICSTRUCT_PROGRESSIVE);

        // The first encoder Init above validates that the selected hardware
        // can encode the requested codec. Reinitialize it now that decode has
        // resolved the real source geometry and interlace mode.
        MFXVideoENCODE_Close(session_);
        encoderInitialized_ = false;
        if (!initializeEncoderForResolvedGeometry(error)) return false;

        decodeParams_.IOPattern = MFX_IOPATTERN_OUT_VIDEO_MEMORY;
        decodeParams_.AsyncDepth = 1;
        mfxStatus status = MFXVideoDECODE_Init(session_, &decodeParams_);
        if (status < MFX_ERR_NONE) {
            error = "oneVPL zero-copy MFXVideoDECODE_Init failed: " +
                    std::to_string(status);
            return false;
        }
        decoderInitialized_ = true;

        std::memset(&vppParams_, 0, sizeof(vppParams_));
        vppParams_.vpp.In = decodeParams_.mfx.FrameInfo;
        vppParams_.vpp.Out = encodeParams_.mfx.FrameInfo;
        vppParams_.vpp.Out.FourCC = MFX_FOURCC_NV12;
        vppParams_.vpp.Out.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
        vppParams_.vpp.Out.PicStruct = outputPicStruct_;
        vppParams_.IOPattern =
            MFX_IOPATTERN_IN_VIDEO_MEMORY | MFX_IOPATTERN_OUT_VIDEO_MEMORY;
        vppParams_.AsyncDepth = 1;
        status = MFXVideoVPP_Init(session_, &vppParams_);
        if (status < MFX_ERR_NONE) {
            error = "oneVPL zero-copy MFXVideoVPP_Init failed: " +
                    std::to_string(status);
            return false;
        }
        vppInitialized_ = true;

        std::cerr << "NATIVE INTEL ZERO-COPY active input="
                  << codecName(config_.inputCodec)
                  << " output=" << codecName(config_.outputCodec)
                  << " path=QSV-DECODE->VPP->QSV-ENCODE"
                  << " source=" << sourceWidth_ << "x" << sourceHeight_
                  << " encode=" << outputWidth_ << "x" << outputHeight_
                  << " deinterlace=" << (config_.deinterlace ? "vpp-auto" : "off")
                  << " decode_copies=0 gpu_surface_copies=0"
                  << std::endl;
        return true;
    }

    bool initializeEncoderForResolvedGeometry(std::string& error) {
        encodeParams_.mfx.CodecId = codecId(config_.outputCodec);
        encodeParams_.mfx.TargetUsage = MFX_TARGETUSAGE_BEST_SPEED;
        encodeParams_.mfx.TargetKbps = static_cast<mfxU16>(
            std::min<std::uint64_t>(config_.bitrate / 1000ULL, 65535ULL));
        encodeParams_.mfx.RateControlMethod = MFX_RATECONTROL_CBR;
        const double fps = config_.fps > 0.0 ? config_.fps : 25.0;
        encodeParams_.mfx.GopPicSize = static_cast<mfxU16>(
            std::min<std::uint64_t>(gopFrames_, 65535ULL));
        encodeParams_.mfx.GopRefDist = 1;
        encodeParams_.mfx.IdrInterval =
            config_.outputCodec == mpegts::ElementaryCodec::H264 ? 0 : 1;
        encodeParams_.mfx.NumSlice = 1;
        encodeParams_.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
        encodeParams_.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
        encodeParams_.mfx.FrameInfo.PicStruct = outputPicStruct_;
        encodeParams_.mfx.FrameInfo.FrameRateExtN = static_cast<mfxU32>(
            std::max(1.0, std::round(fps)));
        encodeParams_.mfx.FrameInfo.FrameRateExtD = 1;
        encodeParams_.mfx.FrameInfo.Width = static_cast<mfxU16>(
            (outputWidth_ + 15) & ~15);
        encodeParams_.mfx.FrameInfo.Height = static_cast<mfxU16>(
            (outputHeight_ + 15) & ~15);
        encodeParams_.mfx.FrameInfo.CropW = static_cast<mfxU16>(outputWidth_);
        encodeParams_.mfx.FrameInfo.CropH = static_cast<mfxU16>(outputHeight_);
        encodeParams_.IOPattern = MFX_IOPATTERN_IN_VIDEO_MEMORY;
        encodeParams_.AsyncDepth = 1;
        const mfxStatus status = MFXVideoENCODE_Init(session_, &encodeParams_);
        if (status < MFX_ERR_NONE) {
            error = "oneVPL zero-copy encoder resize reinit failed: " +
                    std::to_string(status);
            return false;
        }
        encoderInitialized_ = true;
        return true;
    }

    bool decodeBytes(const std::uint8_t* data, std::size_t size,
                     std::uint64_t pts90k, bool hasPts,
                     std::vector<EncodedVideoFrame>& output,
                     std::string& error) {
        mfxBitstream bitstream{};
        bitstream.Data = const_cast<mfxU8*>(data);
        bitstream.MaxLength = static_cast<mfxU32>(
            std::min<std::size_t>(size, std::numeric_limits<mfxU32>::max()));
        bitstream.DataLength = bitstream.MaxLength;
        bitstream.TimeStamp = hasPts
            ? static_cast<mfxU64>(pts90k)
            : MFX_TIMESTAMP_UNKNOWN;

        int safety = 0;
        while ((bitstream.DataLength != 0 || safety == 0) && safety++ < 256) {
            const mfxU32 beforeOffset = bitstream.DataOffset;
            const mfxU32 beforeLength = bitstream.DataLength;
            mfxFrameSurface1* decoded = nullptr;
            mfxSyncPoint sync{};
            const mfxStatus status = callDecode(&bitstream, decoded, sync);
            if (status == MFX_ERR_MORE_DATA) {
                releaseSurface(decoded);
                break;
            }
            if (status < MFX_ERR_NONE) {
                error = "oneVPL zero-copy DecodeFrameAsync failed: " +
                        std::to_string(status);
                releaseSurface(decoded);
                return false;
            }
            if (decoded) {
                const bool ok = processDecodedSurface(decoded, output, error);
                releaseSurface(decoded);
                if (!ok) return false;
            }
            if (!decoded && beforeOffset == bitstream.DataOffset &&
                beforeLength == bitstream.DataLength)
                break;
        }
        return true;
    }

    mfxStatus callDecode(mfxBitstream* bitstream,
                         mfxFrameSurface1*& decoded,
                         mfxSyncPoint& sync) {
        for (int retry = 0; retry < 64; ++retry) {
            const mfxStatus status = MFXVideoDECODE_DecodeFrameAsync(
                session_, bitstream, nullptr, &decoded, &sync);
            if (!retryableStatus(status)) return status;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return MFX_ERR_DEVICE_FAILED;
    }

    bool processDecodedSurface(mfxFrameSurface1* decoded,
                               std::vector<EncodedVideoFrame>& output,
                               std::string& error) {
        mfxFrameSurface1* vpp = nullptr;
        const mfxStatus status = callVpp(decoded, vpp);
        if (status == MFX_ERR_MORE_DATA) {
            releaseSurface(vpp);
            return true;
        }
        if (status < MFX_ERR_NONE) {
            error = "oneVPL zero-copy VPP failed: " +
                    std::to_string(status);
            releaseSurface(vpp);
            return false;
        }
        if (!vpp) return true;
        const bool ok = encodeSurface(vpp, output, error);
        releaseSurface(vpp);
        return ok;
    }

    mfxStatus callVpp(mfxFrameSurface1* input,
                      mfxFrameSurface1*& output) {
        for (int retry = 0; retry < 64; ++retry) {
            const mfxStatus status =
                MFXVideoVPP_ProcessFrameAsync(session_, input, &output);
            if (!retryableStatus(status)) return status;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return MFX_ERR_DEVICE_FAILED;
    }

    bool encodeSurface(mfxFrameSurface1* surface,
                       std::vector<EncodedVideoFrame>& output,
                       std::string& error,
                       bool draining = false) {
        mfxBitstream bitstream{};
        bitstream.Data = bitstream_.data();
        bitstream.MaxLength = static_cast<mfxU32>(bitstream_.size());

        mfxEncodeCtrl ctrl{};
        mfxEncodeCtrl* ctrlPtr = nullptr;
        const std::uint64_t frameNumber = frameIndex_;
        if (!draining && surface) {
            const bool forceIdr = frameNumber == 0 ||
                (gopFrames_ != 0 && (frameNumber % gopFrames_) == 0);
            if (forceIdr) {
                ctrl.FrameType = static_cast<mfxU16>(
                    MFX_FRAMETYPE_I | MFX_FRAMETYPE_IDR | MFX_FRAMETYPE_REF);
                ctrlPtr = &ctrl;
            }
        }

        mfxSyncPoint sync{};
        mfxStatus status = MFX_ERR_NONE;
        for (int retry = 0; retry < 64; ++retry) {
            status = MFXVideoENCODE_EncodeFrameAsync(
                session_, ctrlPtr, surface, &bitstream, &sync);
            if (!retryableStatus(status)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!draining && surface && status >= MFX_ERR_NONE)
            ++frameIndex_;
        if (status == MFX_ERR_MORE_DATA) return true;
        if (status == MFX_ERR_NOT_ENOUGH_BUFFER) {
            error = "oneVPL zero-copy encoded access unit exceeded 8 MiB buffer";
            return false;
        }
        if (status < MFX_ERR_NONE) {
            error = "oneVPL zero-copy EncodeFrameAsync failed: " +
                    std::to_string(status);
            return false;
        }
        if (!sync) return true;

        status = MFXVideoCORE_SyncOperation(session_, sync, 60000);
        if (status < MFX_ERR_NONE) {
            error = "oneVPL zero-copy encode sync failed: " +
                    std::to_string(status);
            return false;
        }
        if (bitstream.DataLength == 0) return true;

        EncodedVideoFrame encoded;
        encoded.data.assign(
            bitstream.Data + bitstream.DataOffset,
            bitstream.Data + bitstream.DataOffset + bitstream.DataLength);
        encoded.hasPts = bitstream.TimeStamp != MFX_TIMESTAMP_UNKNOWN;
        encoded.hasDts = encoded.hasPts;
        encoded.pts90k = encoded.hasPts ? bitstream.TimeStamp : 0;
        encoded.dts90k = encoded.pts90k;
        encoded.keyFrame =
            (bitstream.FrameType & (MFX_FRAMETYPE_IDR | MFX_FRAMETYPE_I)) != 0;
        output.push_back(std::move(encoded));

        if (!draining && surface) {
            if (frameNumber == 0 ||
                (gopFrames_ != 0 && (frameNumber % gopFrames_) == 0)) {
                ++forcedIdrCount_;
                if (forcedIdrCount_ <= 3 || (forcedIdrCount_ % 30ULL) == 0ULL) {
                    std::cerr << "NATIVE INTEL ZERO-COPY RANDOM ACCESS codec="
                              << codecName(config_.outputCodec)
                              << " frame=" << frameNumber
                              << " count=" << forcedIdrCount_
                              << " force_idr=1"
                              << std::endl;
                }
            }
        }
        return true;
    }

    void close() noexcept {
        if (session_) {
            if (encoderInitialized_) MFXVideoENCODE_Close(session_);
            if (vppInitialized_) MFXVideoVPP_Close(session_);
            if (decoderInitialized_) MFXVideoDECODE_Close(session_);
            MFXClose(session_);
        }
        if (loader_) MFXUnload(loader_);
        session_ = nullptr;
        loader_ = nullptr;
        decoderInitialized_ = false;
        vppInitialized_ = false;
        encoderInitialized_ = false;
        headerBuffer_.clear();
        bitstream_.clear();
    }

    static constexpr std::size_t kMaxHeaderBytes = 4U * 1024U * 1024U;
    IntelZeroCopyConfig config_;
    mfxLoader loader_ = nullptr;
    mfxSession session_ = nullptr;
    mfxVideoParam decodeParams_{};
    mfxVideoParam vppParams_{};
    mfxVideoParam encodeParams_{};
    bool decoderInitialized_ = false;
    bool vppInitialized_ = false;
    bool encoderInitialized_ = false;
    std::vector<std::uint8_t> headerBuffer_;
    std::vector<std::uint8_t> bitstream_;
    std::uint64_t headerPts90k_ = 0;
    bool headerHasPts_ = false;
    int sourceWidth_ = 0;
    int sourceHeight_ = 0;
    int outputWidth_ = 0;
    int outputHeight_ = 0;
    mfxU16 outputPicStruct_ = MFX_PICSTRUCT_PROGRESSIVE;
    std::uint64_t gopFrames_ = 50;
    std::uint64_t frameIndex_ = 0;
    std::uint64_t forcedIdrCount_ = 0;
};

#endif

} // namespace

bool intelZeroCopyRuntimeAvailable() noexcept {
#if defined(__linux__) && defined(DVBSTREAMER5_HAVE_VPL)
    mfxLoader loader = MFXLoad();
    if (!loader) return false;
    configureHardwareLoader(loader);
    mfxSession session = nullptr;
    const mfxStatus status = MFXCreateSession(loader, 0, &session);
    const bool available = status >= MFX_ERR_NONE && session &&
                           runtimeAtLeast21(session);
    if (session) MFXClose(session);
    MFXUnload(loader);
    return available;
#else
    return false;
#endif
}

std::unique_ptr<IntelZeroCopyTranscoder> createIntelZeroCopyTranscoder(
    const IntelZeroCopyConfig& config,
    std::string& error) {
    error.clear();
#if defined(__linux__) && defined(DVBSTREAMER5_HAVE_VPL)
    auto transcoder = std::make_unique<IntelZeroCopyTranscoderImpl>(config);
    if (!transcoder->initialize(error)) return nullptr;
    return transcoder;
#else
    (void)config;
    error = "Intel oneVPL zero-copy support was not compiled in";
    return nullptr;
#endif
}

} // namespace dvbstreamer5::media::codec
