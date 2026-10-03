#include "media/NativeHardwareCodec.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <cctype>
#include <unistd.h>
#if defined(__linux__)
#include <fcntl.h>
#endif
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#if defined(DVBSTREAMER5_HAVE_VPL)
#include <vpl/mfxdispatcher.h>
#include <vpl/mfxvideo.h>
#endif

#if defined(DVBSTREAMER5_HAVE_VAAPI)
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_enc_h264.h>
#include <va/va_vpp.h>
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

int openRenderNode() {
#if defined(__linux__)
    for (int i = 128; i < 160; ++i) {
        const std::string path = "/dev/dri/renderD" + std::to_string(i);
        const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd >= 0) return fd;
    }
#endif
    return -1;
}

#if defined(DVBSTREAMER5_HAVE_VPL)
bool probeOneVplHardware() {
    mfxLoader loader = MFXLoad();
    if (!loader) return false;

    mfxConfig implCfg = MFXCreateConfig(loader);
    if (implCfg) {
        mfxVariant v{};
        v.Type = MFX_VARIANT_TYPE_U32;
        v.Data.U32 = MFX_IMPL_TYPE_HARDWARE;
        (void)MFXSetConfigFilterProperty(
            implCfg,
            reinterpret_cast<const mfxU8*>("mfxImplDescription.Impl"),
            v);
    }
#if defined(MFX_ACCEL_MODE_VIA_VAAPI)
    mfxConfig accelCfg = MFXCreateConfig(loader);
    if (accelCfg) {
        mfxVariant v{};
        v.Type = MFX_VARIANT_TYPE_U32;
        v.Data.U32 = MFX_ACCEL_MODE_VIA_VAAPI;
        (void)MFXSetConfigFilterProperty(
            accelCfg,
            reinterpret_cast<const mfxU8*>(
                "mfxImplDescription.AccelerationMode"),
            v);
    }
#endif
    mfxSession session = nullptr;
    const mfxStatus sts = MFXCreateSession(loader, 0, &session);
    const bool ok = sts >= MFX_ERR_NONE && session != nullptr;
    if (session) MFXClose(session);
    MFXUnload(loader);
    return ok;
}
#endif

#if defined(DVBSTREAMER5_HAVE_VAAPI)
bool hasVaEntrypoint(VADisplay display, VAProfile profile, VAEntrypoint wanted) {
    VAEntrypoint entries[32]{};
    int count = 0;
    if (vaQueryConfigEntrypoints(display, profile, entries, &count) != VA_STATUS_SUCCESS)
        return false;
    for (int i = 0; i < count; ++i)
        if (entries[i] == wanted) return true;
    return false;
}

bool probeVaapiH264Encode() {
    const int fd = openRenderNode();
    if (fd < 0) return false;
    VADisplay display = vaGetDisplayDRM(fd);
    if (!display) {
        ::close(fd);
        return false;
    }
    int major = 0, minor = 0;
    if (vaInitialize(display, &major, &minor) != VA_STATUS_SUCCESS) {
        ::close(fd);
        return false;
    }
    const bool ok =
        hasVaEntrypoint(display, VAProfileH264High, VAEntrypointEncSlice) ||
        hasVaEntrypoint(display, VAProfileH264Main, VAEntrypointEncSlice) ||
        hasVaEntrypoint(display, VAProfileH264ConstrainedBaseline,
                        VAEntrypointEncSlice);
    vaTerminate(display);
    ::close(fd);
    return ok;
}
#endif

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

#if defined(DVBSTREAMER5_HAVE_VAAPI)
class H264VaapiBitWriter {
public:
    void bit(bool value) {
        if (bitOffset_ == 0) data_.push_back(0);
        if (value)
            data_.back() |= static_cast<std::uint8_t>(
                1U << (7U - bitOffset_));
        bitOffset_ = (bitOffset_ + 1U) & 7U;
    }

    void bits(std::uint32_t value, unsigned count) {
        for (unsigned i = 0; i < count; ++i)
            bit(((value >> (count - 1U - i)) & 1U) != 0);
    }

    void ue(std::uint32_t value) {
        const std::uint32_t n = value + 1U;
        unsigned count = 0;
        for (std::uint32_t v = n; v; v >>= 1U) ++count;
        for (unsigned i = 1; i < count; ++i) bit(false);
        bits(n, count);
    }

    void se(std::int32_t value) {
        ue(value <= 0
            ? static_cast<std::uint32_t>(-value) * 2U
            : static_cast<std::uint32_t>(value) * 2U - 1U);
    }

    std::vector<std::uint8_t> finish() {
        bit(true);
        while (bitOffset_ != 0) bit(false);
        return data_;
    }

private:
    std::vector<std::uint8_t> data_;
    unsigned bitOffset_ = 0;
};

std::vector<std::uint8_t> makeH264AnnexBNal(
    std::uint8_t header,
    const std::vector<std::uint8_t>& rbsp) {
    std::vector<std::uint8_t> out{0, 0, 0, 1, header};
    unsigned zeros = 0;
    for (std::uint8_t b : rbsp) {
        if (zeros >= 2U && b <= 0x03U) {
            out.push_back(0x03);
            zeros = 0;
        }
        out.push_back(b);
        zeros = b == 0 ? zeros + 1U : 0U;
    }
    return out;
}

std::uint8_t vaapiH264ProfileIdc(VAProfile profile) {
    if (profile == VAProfileH264High) return 100;
    if (profile == VAProfileH264Main) return 77;
    return 66;
}

std::vector<std::uint8_t> makeVaapiH264Sps(
    VAProfile profile,
    int width,
    int height,
    int alignedWidth,
    int alignedHeight) {
    H264VaapiBitWriter w;
    const auto profileIdc = vaapiH264ProfileIdc(profile);

    w.bits(profileIdc, 8);
    w.bits(0, 8);
    w.bits(40, 8);
    w.ue(0);

    if (profileIdc >= 100) {
        w.ue(1);
        w.ue(0);
        w.ue(0);
        w.bit(false);
        w.bit(false);
    }

    w.ue(0);
    w.ue(0);
    w.ue(0);
    w.ue(1);
    w.bit(false);
    w.ue(static_cast<std::uint32_t>(alignedWidth / 16 - 1));
    w.ue(static_cast<std::uint32_t>(alignedHeight / 16 - 1));
    w.bit(true);
    w.bit(true);

    const bool crop =
        alignedWidth != width || alignedHeight != height;
    w.bit(crop);
    if (crop) {
        w.ue(0);
        w.ue(static_cast<std::uint32_t>(
            (alignedWidth - width) / 2));
        w.ue(0);
        w.ue(static_cast<std::uint32_t>(
            (alignedHeight - height) / 2));
    }

    w.bit(false);
    return makeH264AnnexBNal(0x67, w.finish());
}

std::vector<std::uint8_t> makeVaapiH264Pps() {
    H264VaapiBitWriter w;
    w.ue(0);
    w.ue(0);
    w.bit(false);
    w.bit(false);
    w.ue(0);
    w.ue(0);
    w.ue(0);
    w.bit(false);
    w.bits(0, 2);
    w.se(0);
    w.se(0);
    w.se(0);
    w.bit(true);
    w.bit(false);
    w.bit(false);
    return makeH264AnnexBNal(0x68, w.finish());
}

bool isAnnexB(const std::vector<std::uint8_t>& data) {
    return (data.size() >= 4 &&
            data[0] == 0 && data[1] == 0 &&
            data[2] == 0 && data[3] == 1) ||
           (data.size() >= 3 &&
            data[0] == 0 && data[1] == 0 &&
            data[2] == 1);
}

bool hasH264NalType(
    const std::vector<std::uint8_t>& data,
    unsigned wanted) {
    for (std::size_t i = 0; i + 3 <= data.size(); ++i) {
        std::size_t sc = 0;
        if (i + 4 <= data.size() &&
            data[i] == 0 && data[i + 1] == 0 &&
            data[i + 2] == 0 && data[i + 3] == 1) {
            sc = 4;
        } else if (data[i] == 0 &&
                   data[i + 1] == 0 &&
                   data[i + 2] == 1) {
            sc = 3;
        }
        if (sc && i + sc < data.size() &&
            (data[i + sc] & 0x1fU) == wanted)
            return true;
    }
    return false;
}

class VaapiH264Encoder final : public VideoEncoder {
public:
    bool supportsNativeScaling() const noexcept override { return true; }
    explicit VaapiH264Encoder(mpegts::ElementaryCodec codec) : codec_(codec) {}
    ~VaapiH264Encoder() override { close(); }

    bool configure(int width, int height, double fps, std::uint64_t bitrate,
                   std::string& error) override {
        error.clear();
        close();
        if (codec_ != mpegts::ElementaryCodec::H264) {
            error = "direct VAAPI fallback currently supports H.264 only";
            return false;
        }
        if (width <= 0 || height <= 0 || (width & 1) || (height & 1)) {
            error = "VAAPI requires positive even dimensions";
            return false;
        }

        drmFd_ = openRenderNode();
        if (drmFd_ < 0) {
            error = "VAAPI render node /dev/dri/renderD* unavailable";
            return false;
        }

        display_ = vaGetDisplayDRM(drmFd_);
        if (!display_) {
            error = "vaGetDisplayDRM failed";
            close();
            return false;
        }

        int major = 0, minor = 0;
        VAStatus st = vaInitialize(display_, &major, &minor);
        if (st != VA_STATUS_SUCCESS) {
            error = "vaInitialize failed: " + std::string(vaErrorStr(st));
            close();
            return false;
        }
        vaInitialized_ = true;

        if (hasVaEntrypoint(display_, VAProfileH264High, VAEntrypointEncSlice))
            profile_ = VAProfileH264High;
        else if (hasVaEntrypoint(display_, VAProfileH264Main, VAEntrypointEncSlice))
            profile_ = VAProfileH264Main;
        else if (hasVaEntrypoint(display_, VAProfileH264ConstrainedBaseline,
                                 VAEntrypointEncSlice))
            profile_ = VAProfileH264ConstrainedBaseline;
        else {
            error = "VAAPI H.264 EncSlice entrypoint unavailable";
            close();
            return false;
        }

        VAConfigAttrib attrs[2]{};
        attrs[0].type = VAConfigAttribRTFormat;
        attrs[1].type = VAConfigAttribRateControl;
        st = vaGetConfigAttributes(
            display_, profile_, VAEntrypointEncSlice, attrs, 2);
        if (st != VA_STATUS_SUCCESS) {
            error = "vaGetConfigAttributes failed: " + std::string(vaErrorStr(st));
            close();
            return false;
        }
        if (!(attrs[0].value & VA_RT_FORMAT_YUV420)) {
            error = "VAAPI H.264 encoder does not support YUV420";
            close();
            return false;
        }

        cbr_ = attrs[1].value != VA_ATTRIB_NOT_SUPPORTED &&
               (attrs[1].value & VA_RC_CBR) != 0;
        attrs[0].value = VA_RT_FORMAT_YUV420;
        attrs[1].value = cbr_ ? VA_RC_CBR : VA_RC_CQP;

        st = vaCreateConfig(
            display_, profile_, VAEntrypointEncSlice,
            attrs, 2, &config_);
        if (st != VA_STATUS_SUCCESS) {
            error = "vaCreateConfig failed: " + std::string(vaErrorStr(st));
            close();
            return false;
        }

        width_ = width;
        height_ = height;
        alignedWidth_ = (width + 15) & ~15;
        alignedHeight_ = (height + 15) & ~15;
        fps_ = fps > 0.0 ? fps : 25.0;
        bitrate_ = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(bitrate, 0xffffffffULL));
        gop_ = std::max(1, static_cast<int>(std::lround(fps_ * 2.0)));
        spsAnnexB_ = makeVaapiH264Sps(
            profile_, width_, height_, alignedWidth_, alignedHeight_);
        ppsAnnexB_ = makeVaapiH264Pps();

        VASurfaceAttrib surfaceAttr{};
        surfaceAttr.type = VASurfaceAttribPixelFormat;
        surfaceAttr.flags = VA_SURFACE_ATTRIB_SETTABLE;
        surfaceAttr.value.type = VAGenericValueTypeInteger;
        surfaceAttr.value.value.i = VA_FOURCC_NV12;

        st = vaCreateSurfaces(
            display_, VA_RT_FORMAT_YUV420,
            static_cast<unsigned>(alignedWidth_),
            static_cast<unsigned>(alignedHeight_),
            surfaces_, 2, &surfaceAttr, 1);
        if (st != VA_STATUS_SUCCESS) {
            error = "vaCreateSurfaces failed: " + std::string(vaErrorStr(st));
            close();
            return false;
        }
        surfacesCreated_ = true;

        st = vaCreateContext(
            display_, config_, alignedWidth_, alignedHeight_,
            VA_PROGRESSIVE, surfaces_, 2, &context_);
        if (st != VA_STATUS_SUCCESS) {
            error = "vaCreateContext failed: " + std::string(vaErrorStr(st));
            close();
            return false;
        }

        configured_ = true;
        std::cerr
            << "NATIVE HW ENCODER backend=vaapi-direct codec=h264"
            << " size=" << width_ << "x" << height_
            << " fps=" << fps_
            << " bitrate_kbps=" << (bitrate_ / 1000U)
            << " rc=" << (cbr_ ? "cbr" : "cqp")
            << std::endl;
        return true;
    }

    bool encode(const RawVideoFrame& input,
                std::vector<EncodedVideoFrame>& output,
                std::string& error) override {
        error.clear();
        if (!configured_) {
            error = "VAAPI encoder not configured";
            return false;
        }
        const bool needsScale =
            input.width != width_ || input.height != height_;

        const bool idr = frameIndex_ == 0 || (frameIndex_ % gop_) == 0;
        const int currentIndex = static_cast<int>(frameIndex_ & 1U);
        const int referenceIndex = currentIndex ^ 1;
        const VASurfaceID current = surfaces_[currentIndex];
        const VASurfaceID reference =
            frameIndex_ == 0 ? VA_INVALID_SURFACE : surfaces_[referenceIndex];

        if (needsScale) {
            if (!scaleToEncodeSurface(current, input, error))
                return false;
        } else {
            if (!uploadNv12(current, input, error))
                return false;
        }

        VABufferID coded = VA_INVALID_ID;
        const unsigned codedSize = static_cast<unsigned>(
            std::max<std::size_t>(
                1024U * 1024U,
                static_cast<std::size_t>(alignedWidth_) *
                    alignedHeight_ * 2U));
        VAStatus st = vaCreateBuffer(
            display_, context_, VAEncCodedBufferType,
            codedSize, 1, nullptr, &coded);
        if (st != VA_STATUS_SUCCESS) {
            error = "vaCreateBuffer(coded) failed: " +
                std::string(vaErrorStr(st));
            return false;
        }

        std::vector<VABufferID> params;
        auto createParam = [&](VABufferType type, const void* data,
                               unsigned size) -> bool {
            VABufferID id = VA_INVALID_ID;
            VAStatus local = vaCreateBuffer(
                display_, context_, type, size, 1,
                const_cast<void*>(data), &id);
            if (local != VA_STATUS_SUCCESS) {
                error = "vaCreateBuffer(param) failed: " +
                    std::string(vaErrorStr(local));
                return false;
            }
            params.push_back(id);
            return true;
        };

        if (idr) {
            VAEncSequenceParameterBufferH264 seq{};
            seq.seq_parameter_set_id = 0;
            seq.level_idc = 40;
            seq.intra_period = gop_;
            seq.intra_idr_period = gop_;
            seq.ip_period = 1;
            seq.bits_per_second = bitrate_;
            seq.max_num_ref_frames = 1;
            seq.picture_width_in_mbs =
                static_cast<unsigned short>(alignedWidth_ / 16);
            seq.picture_height_in_mbs =
                static_cast<unsigned short>(alignedHeight_ / 16);
            seq.seq_fields.bits.chroma_format_idc = 1;
            seq.seq_fields.bits.frame_mbs_only_flag = 1;
            seq.seq_fields.bits.direct_8x8_inference_flag = 1;
            seq.seq_fields.bits.log2_max_frame_num_minus4 = 0;
            seq.seq_fields.bits.pic_order_cnt_type = 0;
            seq.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 0;
            if (alignedWidth_ != width_ || alignedHeight_ != height_) {
                seq.frame_cropping_flag = 1;
                seq.frame_crop_right_offset =
                    static_cast<unsigned short>((alignedWidth_ - width_) / 2);
                seq.frame_crop_bottom_offset =
                    static_cast<unsigned short>((alignedHeight_ - height_) / 2);
            }
            if (!createParam(
                    VAEncSequenceParameterBufferType,
                    &seq, sizeof(seq))) {
                destroyBuffers(params);
                vaDestroyBuffer(display_, coded);
                return false;
            }
        }

        VAEncPictureParameterBufferH264 pic{};
        pic.CurrPic.picture_id = current;
        pic.CurrPic.frame_idx = static_cast<unsigned>(frameIndex_ & 0x0fU);
        pic.CurrPic.flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
        for (auto& r : pic.ReferenceFrames) {
            r.picture_id = VA_INVALID_SURFACE;
            r.flags = VA_PICTURE_H264_INVALID;
        }
        if (!idr && reference != VA_INVALID_SURFACE) {
            pic.ReferenceFrames[0].picture_id = reference;
            pic.ReferenceFrames[0].frame_idx =
                static_cast<unsigned>((frameIndex_ - 1U) & 0x0fU);
            pic.ReferenceFrames[0].flags =
                VA_PICTURE_H264_SHORT_TERM_REFERENCE;
        }
        pic.coded_buf = coded;
        pic.pic_parameter_set_id = 0;
        pic.seq_parameter_set_id = 0;
        pic.frame_num = static_cast<unsigned>(frameIndex_ & 0x0fU);
        pic.pic_init_qp = 26;
        pic.num_ref_idx_l0_active_minus1 = 0;
        pic.num_ref_idx_l1_active_minus1 = 0;
        pic.pic_fields.bits.idr_pic_flag = idr ? 1 : 0;
        pic.pic_fields.bits.reference_pic_flag = 1;
        pic.pic_fields.bits.entropy_coding_mode_flag = 0;
        pic.pic_fields.bits.deblocking_filter_control_present_flag = 1;

        if (!createParam(
                VAEncPictureParameterBufferType,
                &pic, sizeof(pic))) {
            destroyBuffers(params);
            vaDestroyBuffer(display_, coded);
            return false;
        }

        VAEncSliceParameterBufferH264 slice{};
        slice.macroblock_address = 0;
        slice.num_macroblocks = static_cast<unsigned>(
            (alignedWidth_ / 16) * (alignedHeight_ / 16));
        slice.slice_type = idr ? 2 : 0;
        slice.pic_parameter_set_id = 0;
        slice.idr_pic_id =
            idr ? static_cast<unsigned short>((frameIndex_ / gop_) & 0xffffU)
                : 0;
        slice.pic_order_cnt_lsb =
            static_cast<unsigned>((frameIndex_ * 2U) & 0x0fU);

        for (auto& r : slice.RefPicList0) {
            r.picture_id = VA_INVALID_SURFACE;
            r.flags = VA_PICTURE_H264_INVALID;
        }
        for (auto& r : slice.RefPicList1) {
            r.picture_id = VA_INVALID_SURFACE;
            r.flags = VA_PICTURE_H264_INVALID;
        }
        if (!idr && reference != VA_INVALID_SURFACE) {
            slice.RefPicList0[0].picture_id = reference;
            slice.RefPicList0[0].frame_idx =
                static_cast<unsigned>((frameIndex_ - 1U) & 0x0fU);
            slice.RefPicList0[0].flags =
                VA_PICTURE_H264_SHORT_TERM_REFERENCE;
        }
        slice.slice_qp_delta = 0;
        slice.disable_deblocking_filter_idc = 0;

        if (!createParam(
                VAEncSliceParameterBufferType,
                &slice, sizeof(slice))) {
            destroyBuffers(params);
            vaDestroyBuffer(display_, coded);
            return false;
        }

        st = vaBeginPicture(display_, context_, current);
        if (st == VA_STATUS_SUCCESS)
            st = vaRenderPicture(
                display_, context_, params.data(),
                static_cast<int>(params.size()));
        if (st == VA_STATUS_SUCCESS)
            st = vaEndPicture(display_, context_);

        destroyBuffers(params);

        if (st != VA_STATUS_SUCCESS) {
            vaDestroyBuffer(display_, coded);
            error = "VAAPI encode submit failed: " +
                std::string(vaErrorStr(st));
            return false;
        }

        st = vaSyncSurface(display_, current);
        if (st != VA_STATUS_SUCCESS) {
            vaDestroyBuffer(display_, coded);
            error = "vaSyncSurface failed: " +
                std::string(vaErrorStr(st));
            return false;
        }

        VACodedBufferSegment* segment = nullptr;
        st = vaMapBuffer(
            display_, coded,
            reinterpret_cast<void**>(&segment));
        if (st != VA_STATUS_SUCCESS) {
            vaDestroyBuffer(display_, coded);
            error = "vaMapBuffer(coded) failed: " +
                std::string(vaErrorStr(st));
            return false;
        }

        EncodedVideoFrame frame;
        frame.hasPts = input.hasPts;
        frame.hasDts = input.hasDts;
        frame.pts90k = input.pts90k;
        frame.dts90k = input.hasDts ? input.dts90k : input.pts90k;
        frame.keyFrame = idr;

        for (auto* p = segment; p;
             p = static_cast<VACodedBufferSegment*>(p->next)) {
            if (!p->buf || p->size == 0) continue;
            const auto* bytes =
                static_cast<const std::uint8_t*>(p->buf);
            frame.data.insert(frame.data.end(), bytes, bytes + p->size);
        }

        if (!frame.data.empty() && !isAnnexB(frame.data))
            frame.data.insert(frame.data.begin(), {0, 0, 0, 1});

        if (idr && !frame.data.empty()) {
            std::vector<std::uint8_t> au;
            au.reserve(
                spsAnnexB_.size() +
                ppsAnnexB_.size() +
                frame.data.size());
            au.insert(
                au.end(),
                spsAnnexB_.begin(),
                spsAnnexB_.end());
            au.insert(
                au.end(),
                ppsAnnexB_.begin(),
                ppsAnnexB_.end());
            au.insert(
                au.end(),
                frame.data.begin(),
                frame.data.end());
            frame.data.swap(au);

            if (!startupLogged_) {
                std::cerr
                    << "NATIVE VAAPI H264 startup"
                    << " bytes=" << frame.data.size()
                    << " sps=" << (hasH264NalType(frame.data, 7) ? 1 : 0)
                    << " pps=" << (hasH264NalType(frame.data, 8) ? 1 : 0)
                    << " idr=" << (hasH264NalType(frame.data, 5) ? 1 : 0)
                    << std::endl;
                startupLogged_ = true;
            }
        }

        vaUnmapBuffer(display_, coded);
        vaDestroyBuffer(display_, coded);

        ++frameIndex_;
        if (!frame.data.empty())
            output.push_back(std::move(frame));
        return true;
    }

    bool flush(std::vector<EncodedVideoFrame>&,
               std::string& error) override {
        error.clear();
        return true;
    }

private:
    bool uploadNv12(VASurfaceID surface,
                    const RawVideoFrame& input,
                    std::string& error) {
        VAImage image{};
        VAStatus st = vaDeriveImage(display_, surface, &image);
        if (st != VA_STATUS_SUCCESS) {
            error = "vaDeriveImage failed: " +
                std::string(vaErrorStr(st));
            return false;
        }

        void* mapped = nullptr;
        st = vaMapBuffer(display_, image.buf, &mapped);
        if (st != VA_STATUS_SUCCESS) {
            vaDestroyImage(display_, image.image_id);
            error = "vaMapBuffer(surface) failed: " +
                std::string(vaErrorStr(st));
            return false;
        }

        bool ok = image.format.fourcc == VA_FOURCC_NV12;
        if (ok) {
            const int srcWidth = input.width;
            const int srcHeight = input.height;
            auto* base = static_cast<std::uint8_t*>(mapped);
            const auto* srcY = input.i420.data();
            const auto* srcU =
                srcY + static_cast<std::size_t>(srcWidth) * srcHeight;
            const auto* srcV =
                srcU + static_cast<std::size_t>(srcWidth / 2) *
                    (srcHeight / 2);

            for (int row = 0; row < srcHeight; ++row) {
                std::memcpy(
                    base + image.offsets[0] +
                        static_cast<std::size_t>(row) * image.pitches[0],
                    srcY + static_cast<std::size_t>(row) * srcWidth,
                    static_cast<std::size_t>(srcWidth));
            }

            for (int row = 0; row < srcHeight / 2; ++row) {
                auto* dst =
                    base + image.offsets[1] +
                    static_cast<std::size_t>(row) * image.pitches[1];
                const auto* u =
                    srcU + static_cast<std::size_t>(row) * (srcWidth / 2);
                const auto* v =
                    srcV + static_cast<std::size_t>(row) * (srcWidth / 2);
                for (int x = 0; x < srcWidth / 2; ++x) {
                    dst[2 * x] = u[x];
                    dst[2 * x + 1] = v[x];
                }
            }
        }

        vaUnmapBuffer(display_, image.buf);
        vaDestroyImage(display_, image.image_id);

        if (!ok) {
            error = "VAAPI surface is not NV12";
            return false;
        }
        return true;
    }

    bool ensureVppInput(int srcWidth, int srcHeight, std::string& error) {
        if (vppReady_ && vppInputWidth_ == srcWidth &&
            vppInputHeight_ == srcHeight)
            return true;

        destroyVpp();

        VAStatus st = vaCreateConfig(
            display_, VAProfileNone, VAEntrypointVideoProc,
            nullptr, 0, &vppConfig_);
        if (st != VA_STATUS_SUCCESS) {
            error = "vaCreateConfig(VPP) failed: " +
                std::string(vaErrorStr(st));
            return false;
        }

        VASurfaceAttrib attr{};
        attr.type = VASurfaceAttribPixelFormat;
        attr.flags = VA_SURFACE_ATTRIB_SETTABLE;
        attr.value.type = VAGenericValueTypeInteger;
        attr.value.value.i = VA_FOURCC_NV12;

        st = vaCreateSurfaces(
            display_, VA_RT_FORMAT_YUV420,
            static_cast<unsigned>(srcWidth),
            static_cast<unsigned>(srcHeight),
            &vppInputSurface_, 1, &attr, 1);
        if (st != VA_STATUS_SUCCESS) {
            error = "vaCreateSurfaces(VPP input) failed: " +
                std::string(vaErrorStr(st));
            destroyVpp();
            return false;
        }

        st = vaCreateContext(
            display_, vppConfig_, alignedWidth_, alignedHeight_,
            VA_PROGRESSIVE, surfaces_, 2, &vppContext_);
        if (st != VA_STATUS_SUCCESS) {
            error = "vaCreateContext(VPP) failed: " +
                std::string(vaErrorStr(st));
            destroyVpp();
            return false;
        }

        vppInputWidth_ = srcWidth;
        vppInputHeight_ = srcHeight;
        vppReady_ = true;
        return true;
    }

    bool scaleToEncodeSurface(VASurfaceID target,
                              const RawVideoFrame& input,
                              std::string& error) {
        if (!ensureVppInput(input.width, input.height, error))
            return false;
        if (!uploadNv12(vppInputSurface_, input, error))
            return false;

        VARectangle src{};
        src.x = 0;
        src.y = 0;
        src.width = static_cast<unsigned short>(input.width);
        src.height = static_cast<unsigned short>(input.height);

        VARectangle dst{};
        dst.x = 0;
        dst.y = 0;
        dst.width = static_cast<unsigned short>(width_);
        dst.height = static_cast<unsigned short>(height_);

        VAProcPipelineParameterBuffer param{};
        param.surface = vppInputSurface_;
        param.surface_region = &src;
        param.output_region = &dst;
        param.filter_flags = VA_FILTER_SCALING_DEFAULT;

        VABufferID pipeline = VA_INVALID_ID;
        VAStatus st = vaCreateBuffer(
            display_, vppContext_, VAProcPipelineParameterBufferType,
            sizeof(param), 1, &param, &pipeline);
        if (st != VA_STATUS_SUCCESS) {
            error = "vaCreateBuffer(VPP) failed: " +
                std::string(vaErrorStr(st));
            return false;
        }

        st = vaBeginPicture(display_, vppContext_, target);
        if (st == VA_STATUS_SUCCESS)
            st = vaRenderPicture(display_, vppContext_, &pipeline, 1);
        if (st == VA_STATUS_SUCCESS)
            st = vaEndPicture(display_, vppContext_);
        vaDestroyBuffer(display_, pipeline);

        if (st != VA_STATUS_SUCCESS) {
            error = "VAAPI VPP scale failed: " +
                std::string(vaErrorStr(st));
            return false;
        }

        st = vaSyncSurface(display_, target);
        if (st != VA_STATUS_SUCCESS) {
            error = "vaSyncSurface(VPP target) failed: " +
                std::string(vaErrorStr(st));
            return false;
        }

        if (!vppLogged_) {
            std::cerr
                << "NATIVE HW ENCODER internal_scale=vaapi-vpp-direct-surface"
                << " src=" << input.width << "x" << input.height
                << " dst=" << width_ << "x" << height_
                << std::endl;
            vppLogged_ = true;
        }
        return true;
    }

    void destroyVpp() {
        vppReady_ = false;
        if (display_ && vppContext_ != VA_INVALID_ID)
            vaDestroyContext(display_, vppContext_);
        vppContext_ = VA_INVALID_ID;

        if (display_ && vppInputSurface_ != VA_INVALID_SURFACE)
            vaDestroySurfaces(display_, &vppInputSurface_, 1);
        vppInputSurface_ = VA_INVALID_SURFACE;

        if (display_ && vppConfig_ != VA_INVALID_ID)
            vaDestroyConfig(display_, vppConfig_);
        vppConfig_ = VA_INVALID_ID;

        vppInputWidth_ = 0;
        vppInputHeight_ = 0;
    }

    void destroyBuffers(const std::vector<VABufferID>& buffers) {
        for (VABufferID id : buffers)
            if (id != VA_INVALID_ID)
                vaDestroyBuffer(display_, id);
    }

    void close() {
        configured_ = false;
        destroyVpp();
        if (display_ && context_ != VA_INVALID_ID)
            vaDestroyContext(display_, context_);
        context_ = VA_INVALID_ID;

        if (display_ && surfacesCreated_) {
            vaDestroySurfaces(display_, surfaces_, 2);
            surfacesCreated_ = false;
        }
        surfaces_[0] = surfaces_[1] = VA_INVALID_SURFACE;

        if (display_ && config_ != VA_INVALID_ID)
            vaDestroyConfig(display_, config_);
        config_ = VA_INVALID_ID;

        if (display_ && vaInitialized_)
            vaTerminate(display_);
        vaInitialized_ = false;
        display_ = nullptr;

        if (drmFd_ >= 0)
            ::close(drmFd_);
        drmFd_ = -1;
        frameIndex_ = 0;
        startupLogged_ = false;
        spsAnnexB_.clear();
        ppsAnnexB_.clear();
    }

    mpegts::ElementaryCodec codec_ = mpegts::ElementaryCodec::Unknown;
    int drmFd_ = -1;
    VADisplay display_ = nullptr;
    VAProfile profile_ = VAProfileNone;
    VAConfigID config_ = VA_INVALID_ID;
    VAContextID context_ = VA_INVALID_ID;
    VASurfaceID surfaces_[2]{VA_INVALID_SURFACE, VA_INVALID_SURFACE};
    bool surfacesCreated_ = false;
    bool vaInitialized_ = false;
    bool configured_ = false;
    bool cbr_ = false;
    int width_ = 0;
    int height_ = 0;
    int alignedWidth_ = 0;
    int alignedHeight_ = 0;
    double fps_ = 25.0;
    std::uint32_t bitrate_ = 0;
    int gop_ = 50;
    std::uint64_t frameIndex_ = 0;
    bool startupLogged_ = false;
    std::vector<std::uint8_t> spsAnnexB_;
    std::vector<std::uint8_t> ppsAnnexB_;
    VAConfigID vppConfig_ = VA_INVALID_ID;
    VAContextID vppContext_ = VA_INVALID_ID;
    VASurfaceID vppInputSurface_ = VA_INVALID_SURFACE;
    int vppInputWidth_ = 0;
    int vppInputHeight_ = 0;
    bool vppReady_ = false;
    bool vppLogged_ = false;
};
#endif

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

        gpuOrdinal_ = 0;
        if (const char* gpu = std::getenv("DVBSTREAMER5_NVENC_GPU")) {
            char* end = nullptr;
            const long value = std::strtol(gpu, &end, 10);
            if (!end || end == gpu || *end != '\0' || value < 0 || value > 1024) {
                error = "DVBSTREAMER5_NVENC_GPU must be a non-negative GPU ordinal";
                return false;
            }
            gpuOrdinal_ = static_cast<int>(value);
        }

        if (!loadCuda(error) || !loadNvenc(error)) { close(); return false; }
        if (cuInit_(0) != 0 ||
            cuDeviceGet_(&device_, gpuOrdinal_) != 0 ||
            cuCtxCreate_(&cudaContext_, 0, device_) != 0) {
            error = "CUDA context creation failed for GPU ordinal " +
                std::to_string(gpuOrdinal_);
            close(); return false;
        }
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
        open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        open.apiVersion = NVENCAPI_VERSION;
        open.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
        open.device = cudaContext_;
        if (api_.nvEncOpenEncodeSessionEx(&open, &encoder_) != NV_ENC_SUCCESS || !encoder_) {
            error = "NVENC open session failed"; close(); return false;
        }

        const GUID codecGuid = codec_ == mpegts::ElementaryCodec::H264
            ? NV_ENC_CODEC_H264_GUID : NV_ENC_CODEC_HEVC_GUID;
        if (!validateCapabilities(codecGuid, error)) {
            close(); return false;
        }

        NV_ENC_CONFIG cfg{}; cfg.version = NV_ENC_CONFIG_VER;
        NV_ENC_PRESET_CONFIG preset{}; preset.version = NV_ENC_PRESET_CONFIG_VER;
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
            const GUID p3 = NV_ENC_PRESET_P3_GUID;
            if (api_.nvEncGetEncodePresetConfigEx(
                    encoder_, codecGuid, p3,
                    NV_ENC_TUNING_INFO_LOW_LATENCY,
                    &preset) == NV_ENC_SUCCESS) {
                presetGuid = p3;
                presetName = "p3";
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
                    encoder_, codecGuid, presetGuid,
                    &preset) != NV_ENC_SUCCESS) {
                error = "NVENC low-latency preset configuration unavailable";
                close(); return false;
            }
        }
        cfg = preset.presetCfg;
        cfg.version = NV_ENC_CONFIG_VER;
        cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        cfg.rcParams.averageBitRate = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(bitrate, 0xffffffffULL));
        cfg.rcParams.maxBitRate = cfg.rcParams.averageBitRate;
        fps_ = fps > 0.0 ? fps : 25.0;
        gopFrames_ = std::max<std::uint64_t>(
            1ULL, static_cast<std::uint64_t>(std::llround(fps_ * 2.0)));
        cfg.gopLength = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(gopFrames_, 0xffffffffULL));

        // Live contribution profile: no B frames, no lookahead and no
        // reorder delay. A two-frame VBV keeps latency bounded without
        // starving forced IDR frames at normal broadcast bitrates.
        cfg.frameIntervalP = 1;
        cfg.rcParams.enableLookahead = 0;
        cfg.rcParams.zeroReorderDelay = 1;
        cfg.rcParams.enableAQ = 1;
        cfg.rcParams.aqStrength = 8;
#if defined(NVENCAPI_MAJOR_VERSION) && NVENCAPI_MAJOR_VERSION >= 10
        cfg.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
#endif
        const std::uint64_t roundedFps = std::max<std::uint64_t>(
            1ULL, static_cast<std::uint64_t>(std::llround(fps_)));
        const std::uint64_t frameBudgetBits = std::max<std::uint64_t>(
            1ULL, static_cast<std::uint64_t>(cfg.rcParams.averageBitRate) /
                roundedFps);
        const std::uint64_t vbvBits = std::min<std::uint64_t>(
            0xffffffffULL, frameBudgetBits * 2ULL);
        cfg.rcParams.vbvBufferSize = static_cast<std::uint32_t>(vbvBits);
        cfg.rcParams.vbvInitialDelay = cfg.rcParams.vbvBufferSize;

        if (codec_ == mpegts::ElementaryCodec::H264) {
            cfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
            cfg.encodeCodecConfig.h264Config.outputAUD = 1;
            cfg.encodeCodecConfig.h264Config.idrPeriod = cfg.gopLength;
        } else {
            cfg.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
            cfg.encodeCodecConfig.hevcConfig.outputAUD = 1;
            cfg.encodeCodecConfig.hevcConfig.idrPeriod = cfg.gopLength;
        }

        NV_ENC_INITIALIZE_PARAMS init{};
        init.version = NV_ENC_INITIALIZE_PARAMS_VER;
        init.encodeGUID = codecGuid;
        init.presetGUID = presetGuid;
        init.encodeWidth = width;
        init.encodeHeight = height;
        init.darWidth = width;
        init.darHeight = height;
        init.frameRateNum = static_cast<std::uint32_t>(
            std::max(1.0, std::round(fps_)));
        init.frameRateDen = 1;
        init.enablePTD = 1;
#if defined(NVENCAPI_MAJOR_VERSION) && NVENCAPI_MAJOR_VERSION >= 10
        if (modernLowLatency)
            init.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
#endif
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
        width_ = width;
        height_ = height;
        frameIndex_ = 0;
        forcedIdrCount_ = 0;
        std::cerr << "NATIVE HW ENCODER backend=nvenc codec="
                  << (codec_ == mpegts::ElementaryCodec::H264 ? "h264" : "hevc")
                  << " size=" << width_ << "x" << height_
                  << " fps=" << fps_
                  << " bitrate_kbps=" << (bitrate / 1000ULL)
                  << " gpu=" << gpuOrdinal_
                  << " preset=" << presetName
                  << " tuning=" << (modernLowLatency ? "low-latency" : "legacy-low-latency")
                  << " rc=cbr"
                  << " bframes=0 lookahead=0 reorder_delay=0"
                  << " vbv_frames=2 spatial_aq=1 aq_strength=8"
                  << " gop_frames=" << gopFrames_
                  << " live_random_access=force-idr+spspps"
                  << std::endl;
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

        const std::uint64_t frameNumber = frameIndex_++;
        const bool forceRandomAccess =
            frameNumber == 0 || (gopFrames_ != 0 && (frameNumber % gopFrames_) == 0);

        NV_ENC_PIC_PARAMS pic{}; pic.version = NV_ENC_PIC_PARAMS_VER;
        pic.inputWidth = width_; pic.inputHeight = height_;
        pic.inputPitch = 0;
        pic.inputBuffer = input_;
        pic.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
        pic.outputBitstream = bitstream_;
        pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        pic.inputTimeStamp = input.hasPts ? input.pts90k : frameNumber;
        if (forceRandomAccess) {
            // Late-joining SRT/RTSP/HTTP/HLS clients must be able to
            // start without restarting the encoder. FORCEIDR creates a
            // true random-access picture; OUTPUT_SPSPPS emits SPS/PPS
            // for AVC and VPS/SPS/PPS for HEVC with that access point.
            pic.encodePicFlags |=
                NV_ENC_PIC_FLAG_FORCEIDR |
                NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        }
        const NVENCSTATUS st = api_.nvEncEncodePicture(encoder_, &pic);
        if (st != NV_ENC_SUCCESS && st != NV_ENC_ERR_NEED_MORE_INPUT) {
            error = "NVENC encode failed: " + std::to_string(static_cast<int>(st)); return false;
        }
        if (st == NV_ENC_ERR_NEED_MORE_INPUT) return true;
        return collect(
            input, output, error, forceRandomAccess, frameNumber);
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

    static bool sameGuid(const GUID& a, const GUID& b) noexcept {
        return a.Data1 == b.Data1 && a.Data2 == b.Data2 &&
            a.Data3 == b.Data3 &&
            std::memcmp(a.Data4, b.Data4, sizeof(a.Data4)) == 0;
    }

    bool validateCapabilities(const GUID& codecGuid, std::string& error) {
        if (!api_.nvEncGetEncodeGUIDCount || !api_.nvEncGetEncodeGUIDs ||
            !api_.nvEncGetInputFormatCount || !api_.nvEncGetInputFormats) {
            error = "NVENC capability query functions missing";
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
                encoder_, guids.data(), guidCount,
                &actualGuidCount) != NV_ENC_SUCCESS) {
            error = "NVENC encode codec capability query failed";
            return false;
        }
        bool codecSupported = false;
        for (std::uint32_t i = 0;
             i < std::min(guidCount, actualGuidCount); ++i) {
            if (sameGuid(guids[i], codecGuid)) {
                codecSupported = true;
                break;
            }
        }
        if (!codecSupported) {
            error = std::string("NVENC GPU does not support requested ") +
                (codec_ == mpegts::ElementaryCodec::H264 ? "H.264" : "HEVC") +
                " encoder";
            return false;
        }

        std::uint32_t formatCount = 0;
        if (api_.nvEncGetInputFormatCount(
                encoder_, codecGuid, &formatCount) != NV_ENC_SUCCESS ||
            formatCount == 0) {
            error = "NVENC input format capability query failed";
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
        bool nv12 = false;
        for (std::uint32_t i = 0;
             i < std::min(formatCount, actualFormatCount); ++i) {
            if (formats[i] == NV_ENC_BUFFER_FORMAT_NV12) {
                nv12 = true;
                break;
            }
        }
        if (!nv12) {
            error = "NVENC GPU does not support NV12 input for requested codec";
            return false;
        }

        std::cerr << "NATIVE NVENC CAPABILITIES gpu=" << gpuOrdinal_
                  << " codec="
                  << (codec_ == mpegts::ElementaryCodec::H264 ? "h264" : "hevc")
                  << " nv12=1"
                  << " codec_guids=" << actualGuidCount
                  << " input_formats=" << actualFormatCount
                  << std::endl;
        return true;
    }

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

    bool collect(const RawVideoFrame& input,
               std::vector<EncodedVideoFrame>& output,
               std::string& error,
               bool forcedRandomAccess,
               std::uint64_t frameNumber) {
        NV_ENC_LOCK_BITSTREAM lock{}; lock.version = NV_ENC_LOCK_BITSTREAM_VER;
        lock.outputBitstream = bitstream_; lock.doNotWait = 0;
        const NVENCSTATUS st = api_.nvEncLockBitstream(encoder_, &lock);
        if (st != NV_ENC_SUCCESS) { error = "NVENC lock bitstream failed"; return false; }
        EncodedVideoFrame frame;
        auto* p = static_cast<const std::uint8_t*>(lock.bitstreamBufferPtr);
        frame.data.assign(p, p + lock.bitstreamSizeInBytes);
        frame.hasPts = input.hasPts; frame.hasDts = input.hasDts;
        frame.pts90k = input.pts90k; frame.dts90k = input.hasDts ? input.dts90k : input.pts90k;
        frame.keyFrame = forcedRandomAccess ||
            lock.pictureType == NV_ENC_PIC_TYPE_IDR ||
            lock.pictureType == NV_ENC_PIC_TYPE_I;
        api_.nvEncUnlockBitstream(encoder_, bitstream_);

        if (forcedRandomAccess) {
            ++forcedIdrCount_;
            if (forcedIdrCount_ <= 3 || (forcedIdrCount_ % 30ULL) == 0ULL) {
                std::cerr << "NATIVE NVENC RANDOM ACCESS codec="
                          << (codec_ == mpegts::ElementaryCodec::H264 ? "h264" : "hevc")
                          << " frame=" << frameNumber
                          << " count=" << forcedIdrCount_
                          << " bytes=" << frame.data.size()
                          << " force_idr=1 parameter_sets=1"
                          << std::endl;
            }
        }

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
        frameIndex_ = 0;
        forcedIdrCount_ = 0;
        gopFrames_ = 50;
        gpuOrdinal_ = 0;
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
    int width_ = 0, height_ = 0;
    double fps_ = 25.0;
    std::uint64_t gopFrames_ = 50;
    std::uint64_t frameIndex_ = 0;
    std::uint64_t forcedIdrCount_ = 0;
    int gpuOrdinal_ = 0;
};
#endif

} // namespace

NativeHardwareCapabilities inspectNativeHardwareCapabilities() {
    NativeHardwareCapabilities c;
    c.vaapiRuntime =
        haveRenderNode() &&
        (canDlopen("libva.so.2") || canDlopen("libva.so"));

    bool vplHardware = false;
#if defined(DVBSTREAMER5_HAVE_VPL)
    vplHardware = probeOneVplHardware();
#endif

    bool vaapiH264 = false;
#if defined(DVBSTREAMER5_HAVE_VAAPI)
    vaapiH264 = probeVaapiH264Encode();
#endif

    c.qsvAvailable = vplHardware || vaapiH264;
    c.qsvH264 = vplHardware || vaapiH264;
    c.qsvHevc = vplHardware;

    if (vplHardware && vaapiH264)
        c.intelBackend = "oneVPL QSV + direct VAAPI fallback";
    else if (vplHardware)
        c.intelBackend = "oneVPL QSV via VAAPI";
    else if (vaapiH264)
        c.intelBackend = "direct VAAPI H.264 (legacy Intel fallback)";
    else if (c.vaapiRuntime)
        c.intelBackend = "VAAPI runtime present, no supported encode entrypoint";
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
    if (b == "vaapi") {
#if defined(DVBSTREAMER5_HAVE_VAAPI)
        if (codec != mpegts::ElementaryCodec::H264) {
            error = "direct VAAPI fallback currently supports H.264 only";
            return {};
        }
        return std::make_unique<VaapiH264Encoder>(codec);
#else
        error = "direct VAAPI backend not built: install libva-dev and rebuild";
        return {};
#endif
    }

    if (b == "intel" || b == "qsv") {
#if defined(DVBSTREAMER5_HAVE_VPL)
        if (probeOneVplHardware())
            return std::make_unique<QsvVplEncoder>(codec);
#endif
#if defined(DVBSTREAMER5_HAVE_VAAPI)
        if (codec == mpegts::ElementaryCodec::H264 &&
            probeVaapiH264Encode()) {
            std::cerr
                << "NATIVE HW ENCODER Intel oneVPL unavailable; "
                   "falling back to direct VAAPI H.264"
                << std::endl;
            return std::make_unique<VaapiH264Encoder>(codec);
        }
#endif
        error =
            codec == mpegts::ElementaryCodec::H265
            ? "Intel HEVC hardware encoder unavailable: oneVPL implementation "
              "not found and direct legacy VAAPI fallback is H.264-only"
            : "Intel hardware encoder unavailable: no oneVPL implementation "
              "and no direct VAAPI H.264 EncSlice support";
        return {};
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
