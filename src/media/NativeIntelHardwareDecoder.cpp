#include "media/NativeIntelHardwareDecoder.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
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

bool retryableStatus(mfxStatus status) {
    return status == MFX_WRN_DEVICE_BUSY
#if defined(MFX_WRN_ALLOC_TIMEOUT_EXPIRED)
        || status == MFX_WRN_ALLOC_TIMEOUT_EXPIRED
#endif
        ;
}

void releaseSurface(mfxFrameSurface1*& surface) noexcept {
    if (surface && surface->FrameInterface && surface->FrameInterface->Release)
        (void)surface->FrameInterface->Release(surface);
    surface = nullptr;
}

class IntelHardwareDecoder final : public VideoDecoder {
public:
    explicit IntelHardwareDecoder(mpegts::ElementaryCodec codec)
        : codec_(codec) {}

    ~IntelHardwareDecoder() override { close(); }

    bool initialize(std::string& error) {
        error.clear();
        if (codecId(codec_) == 0) {
            error = "Intel HW decoder supports MPEG-2/H.264/HEVC only";
            return false;
        }

        loader_ = MFXLoad();
        if (!loader_) {
            error = "Intel HW decoder: oneVPL MFXLoad failed";
            return false;
        }
        configureHardwareLoader(loader_);
        const mfxStatus create = MFXCreateSession(loader_, 0, &session_);
        if (create < MFX_ERR_NONE || !session_) {
            error = "Intel HW decoder: hardware oneVPL session unavailable: " +
                    std::to_string(create);
            close();
            return false;
        }

        std::memset(&params_, 0, sizeof(params_));
        params_.mfx.CodecId = codecId(codec_);
        params_.IOPattern = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
        params_.AsyncDepth = 1;

        std::cerr << "NATIVE INTEL HW DECODER session codec="
                  << codecName(codec_)
                  << " memory=system output=i420 encoder_independent=1"
                  << std::endl;
        return true;
    }

    bool decode(const std::uint8_t* data, std::size_t size,
                std::uint64_t pts90k, bool hasPts,
                std::vector<RawVideoFrame>& output,
                std::string& error) override {
        error.clear();
        if (!session_ && !initialize(error)) return false;
        if (!data || size == 0) return true;

        if (!decoderInitialized_) {
            if (headerBuffer_.size() + size > kMaxHeaderBytes) {
                error = "Intel HW decoder header exceeded 4 MiB";
                return false;
            }
            if (headerBuffer_.empty()) {
                headerPts90k_ = pts90k;
                headerHasPts_ = hasPts;
            }
            headerBuffer_.insert(headerBuffer_.end(), data, data + size);
            if (!initializeDecoder(error)) {
                if (error.empty()) return true;
                return false;
            }

            const bool ok = decodeBytes(
                headerBuffer_.data(), headerBuffer_.size(),
                headerPts90k_, headerHasPts_, output, error);
            headerBuffer_.clear();
            return ok;
        }

        return decodeBytes(data, size, pts90k, hasPts, output, error);
    }

    void reset() override {
        close();
    }

private:
    bool initializeDecoder(std::string& error) {
        mfxBitstream header{};
        header.Data = headerBuffer_.data();
        header.MaxLength = static_cast<mfxU32>(std::min<std::size_t>(
            headerBuffer_.size(), std::numeric_limits<mfxU32>::max()));
        header.DataLength = header.MaxLength;

        const mfxStatus hs =
            MFXVideoDECODE_DecodeHeader(session_, &header, &params_);
        if (hs == MFX_ERR_MORE_DATA) {
            error.clear();
            return false;
        }
        if (hs < MFX_ERR_NONE) {
            error = "Intel HW decoder DecodeHeader failed: " +
                    std::to_string(hs);
            return false;
        }

        params_.IOPattern = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
        params_.AsyncDepth = 1;
        const mfxStatus init = MFXVideoDECODE_Init(session_, &params_);
        if (init < MFX_ERR_NONE) {
            error = "Intel HW decoder MFXVideoDECODE_Init failed: " +
                    std::to_string(init);
            return false;
        }
        decoderInitialized_ = true;

        const auto& info = params_.mfx.FrameInfo;
        std::cerr << "NATIVE INTEL HW DECODER active codec="
                  << codecName(codec_)
                  << " coded=" << info.Width << "x" << info.Height
                  << " crop=" << info.CropX << "," << info.CropY
                  << "+" << (info.CropW ? info.CropW : info.Width)
                  << "x" << (info.CropH ? info.CropH : info.Height)
                  << " fourcc=" << info.FourCC
                  << " picstruct=" << info.PicStruct
                  << std::endl;
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

    bool decodeBytes(const std::uint8_t* data, std::size_t size,
                     std::uint64_t pts90k, bool hasPts,
                     std::vector<RawVideoFrame>& output,
                     std::string& error) {
        mfxBitstream bitstream{};
        bitstream.Data = const_cast<mfxU8*>(data);
        bitstream.MaxLength = static_cast<mfxU32>(
            std::min<std::size_t>(size, std::numeric_limits<mfxU32>::max()));
        bitstream.DataLength = bitstream.MaxLength;
        bitstream.TimeStamp = hasPts ? static_cast<mfxU64>(pts90k)
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
                error = "Intel HW decoder DecodeFrameAsync failed: " +
                        std::to_string(status);
                releaseSurface(decoded);
                return false;
            }

            if (decoded) {
                // oneVPL 2.x surfaces are internally allocated. Even with
                // OUT_SYSTEM_MEMORY, Data.Y/Data.UV are not guaranteed valid
                // until the surface has been mapped for CPU reading.
                if (sync) {
                    const mfxStatus synced =
                        MFXVideoCORE_SyncOperation(session_, sync, 60000);
                    if (synced < MFX_ERR_NONE) {
                        error = "Intel HW decoder sync failed: " +
                                std::to_string(synced);
                        releaseSurface(decoded);
                        return false;
                    }
                }
                if (!decoded->FrameInterface ||
                    !decoded->FrameInterface->Map ||
                    !decoded->FrameInterface->Unmap) {
                    error = "Intel HW decoder oneVPL surface has no Map/Unmap interface";
                    releaseSurface(decoded);
                    return false;
                }

                const mfxStatus mapped =
                    decoded->FrameInterface->Map(decoded, MFX_MAP_READ);
                if (mapped < MFX_ERR_NONE) {
                    error = "Intel HW decoder surface Map(MFX_MAP_READ) failed: " +
                            std::to_string(mapped);
                    releaseSurface(decoded);
                    return false;
                }

                const bool copied = copySurface(*decoded, output, error);
                const mfxStatus unmapped =
                    decoded->FrameInterface->Unmap(decoded);
                releaseSurface(decoded);
                if (!copied) return false;
                if (unmapped < MFX_ERR_NONE) {
                    error = "Intel HW decoder surface Unmap failed: " +
                            std::to_string(unmapped);
                    return false;
                }
            }

            if (!decoded && beforeOffset == bitstream.DataOffset &&
                beforeLength == bitstream.DataLength)
                break;
        }
        return true;
    }

    bool copySurface(const mfxFrameSurface1& surface,
                     std::vector<RawVideoFrame>& output,
                     std::string& error) {
        const auto& info = surface.Info;
        const int cropX = info.CropX;
        const int cropY = info.CropY;
        const int width = info.CropW ? info.CropW : info.Width;
        const int height = info.CropH ? info.CropH : info.Height;
        const int pitch = surface.Data.Pitch;

        if (info.FourCC != MFX_FOURCC_NV12) {
            error = "Intel HW decoder output FourCC is not 8-bit NV12: " +
                    std::to_string(info.FourCC);
            return false;
        }
        if (width <= 0 || height <= 0 || (width & 1) || (height & 1) ||
            (cropX & 1) || (cropY & 1) ||
            cropX < 0 || cropY < 0 ||
            cropX + width > info.Width || cropY + height > info.Height ||
            pitch < cropX + width ||
            !surface.Data.Y || !surface.Data.UV) {
            error = "Intel HW decoder invalid mapped NV12 surface: coded=" +
                    std::to_string(info.Width) + "x" + std::to_string(info.Height) +
                    " crop=" + std::to_string(cropX) + "," +
                    std::to_string(cropY) + "+" +
                    std::to_string(width) + "x" + std::to_string(height) +
                    " pitch=" + std::to_string(pitch) +
                    " Y=" + (surface.Data.Y ? "set" : "null") +
                    " UV=" + (surface.Data.UV ? "set" : "null");
            return false;
        }

        RawVideoFrame frame;
        frame.width = width;
        frame.height = height;
        frame.i420.resize(static_cast<std::size_t>(width) * height * 3U / 2U);
        frame.hasPts = surface.Data.TimeStamp != MFX_TIMESTAMP_UNKNOWN;
        frame.pts90k = frame.hasPts
            ? static_cast<std::uint64_t>(surface.Data.TimeStamp) : 0;
        frame.hasDts = false;
        frame.dts90k = frame.pts90k;
        frame.keyFrame = false;
        frame.interlaced =
            (info.PicStruct & (MFX_PICSTRUCT_FIELD_TFF |
                               MFX_PICSTRUCT_FIELD_BFF)) != 0;

        auto* dstY = frame.i420.data();
        auto* dstU = dstY + static_cast<std::size_t>(width) * height;
        auto* dstV = dstU + static_cast<std::size_t>(width / 2) * (height / 2);

        const auto* srcY = surface.Data.Y +
            static_cast<std::size_t>(cropY) * pitch + cropX;
        const auto* srcUV = surface.Data.UV +
            static_cast<std::size_t>(cropY / 2) * pitch + cropX;

        for (int y = 0; y < height; ++y) {
            std::memcpy(
                dstY + static_cast<std::size_t>(y) * width,
                srcY + static_cast<std::size_t>(y) * pitch,
                static_cast<std::size_t>(width));
        }
        for (int y = 0; y < height / 2; ++y) {
            const auto* row = srcUV + static_cast<std::size_t>(y) * pitch;
            auto* u = dstU + static_cast<std::size_t>(y) * (width / 2);
            auto* v = dstV + static_cast<std::size_t>(y) * (width / 2);
            for (int x = 0; x < width / 2; ++x) {
                u[x] = row[x * 2];
                v[x] = row[x * 2 + 1];
            }
        }

        if (frames_ < 4 || (frames_ % 250U) == 0U) {
            std::cerr << "NATIVE INTEL HW DECODER frame=" << frames_
                      << " visible=" << width << "x" << height
                      << " pitch=" << pitch
                      << " crop=" << cropX << "," << cropY
                      << " interlaced=" << (frame.interlaced ? 1 : 0)
                      << " pts=" << (frame.hasPts
                          ? std::to_string(frame.pts90k) : std::string("-"))
                      << std::endl;
        }
        ++frames_;
        output.push_back(std::move(frame));
        return true;
    }

    void close() noexcept {
        if (session_) {
            if (decoderInitialized_) MFXVideoDECODE_Close(session_);
            MFXClose(session_);
        }
        if (loader_) MFXUnload(loader_);
        session_ = nullptr;
        loader_ = nullptr;
        decoderInitialized_ = false;
        headerBuffer_.clear();
        frames_ = 0;
    }

    static constexpr std::size_t kMaxHeaderBytes = 4U * 1024U * 1024U;
    mpegts::ElementaryCodec codec_ = mpegts::ElementaryCodec::Unknown;
    mfxLoader loader_ = nullptr;
    mfxSession session_ = nullptr;
    mfxVideoParam params_{};
    bool decoderInitialized_ = false;
    std::vector<std::uint8_t> headerBuffer_;
    std::uint64_t headerPts90k_ = 0;
    bool headerHasPts_ = false;
    std::uint64_t frames_ = 0;
};

#endif

} // namespace

bool intelHardwareDecoderRuntimeAvailable() noexcept {
#if defined(__linux__) && defined(DVBSTREAMER5_HAVE_VPL)
    mfxLoader loader = MFXLoad();
    if (!loader) return false;
    configureHardwareLoader(loader);
    mfxSession session = nullptr;
    const mfxStatus status = MFXCreateSession(loader, 0, &session);
    const bool available = status >= MFX_ERR_NONE && session != nullptr;
    if (session) MFXClose(session);
    MFXUnload(loader);
    return available;
#else
    return false;
#endif
}

std::unique_ptr<VideoDecoder> createIntelHardwareVideoDecoder(
    mpegts::ElementaryCodec codec,
    std::string& error) {
    error.clear();
#if defined(__linux__) && defined(DVBSTREAMER5_HAVE_VPL)
    auto decoder = std::make_unique<IntelHardwareDecoder>(codec);
    if (!decoder->initialize(error)) return {};
    return decoder;
#else
    (void)codec;
    error = "Intel HW decoder support was not compiled in: install libvpl-dev and rebuild";
    return {};
#endif
}

} // namespace dvbstreamer5::media::codec
