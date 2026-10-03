#include "media/NativeIttiamAvc.h"

extern "C" {
#include <ittiam-avc/ih264_typedefs.h>
#include <ittiam-avc/iv.h>
#include <ittiam-avc/ivd.h>
#include <ittiam-avc/ih264d.h>
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace dvbstreamer5::media::codec {
namespace {

constexpr std::size_t kMaxBufferedAvcBytes = 16U * 1024U * 1024U;
constexpr unsigned kMaxDecodeCallsPerInput = 100U;

void* avcAlignedAlloc(void*, WORD32 alignment, WORD32 size) {
    if (size <= 0) size = 1;
    if (alignment < static_cast<WORD32>(sizeof(void*)))
        alignment = static_cast<WORD32>(sizeof(void*));
#if defined(_WIN32)
    return _aligned_malloc(static_cast<std::size_t>(size),
                           static_cast<std::size_t>(alignment));
#else
    void* ptr = nullptr;
    if (::posix_memalign(&ptr, static_cast<std::size_t>(alignment),
                         static_cast<std::size_t>(size)) != 0)
        return nullptr;
    return ptr;
#endif
}

void avcAlignedFree(void*, void* ptr) {
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

class IttiamAvcDecoder final : public VideoDecoder {
public:
    IttiamAvcDecoder() {
        valid_ = initialize(initError_);
        if (!valid_ && !initError_.empty()) {
            std::cerr << "NATIVE AVC ITTIAM init failed: "
                      << initError_ << '\n';
        }
    }

    ~IttiamAvcDecoder() override { release(); }

    bool valid() const noexcept { return valid_; }
    const std::string& initError() const noexcept { return initError_; }

    bool decode(const std::uint8_t* data, std::size_t size,
                std::uint64_t pts90k, bool hasPts,
                std::vector<RawVideoFrame>& output,
                std::string& error) override {
        error.clear();
        if (!valid_ || !codec_) {
            error = "Ittiam AVC decoder unavailable";
            return false;
        }
        if (!data || size == 0) return true;
        if (size > static_cast<std::size_t>(std::numeric_limits<UWORD32>::max())) {
            error = "Ittiam AVC input access unit is too large";
            return false;
        }

        if (!headerReady_)
            return decodeHeaders(data, size, pts90k, hasPts, output, error);

        // A recoverable libavc call is allowed to leave an unconsumed tail.
        // Preserve that byte-exact tail and prepend it to the next demuxed AU;
        // never silently discard bytes at an arbitrary decoder-call boundary.
        if (!pendingFrame_.empty()) {
            const std::size_t pendingSize = pendingFrame_.size();
            if (pendingSize > kMaxBufferedAvcBytes - std::min(size, kMaxBufferedAvcBytes)) {
                std::cerr << "NATIVE AVC ITTIAM pending overflow bytes="
                          << pendingSize << " incoming=" << size
                          << " action=drop-stale-tail\n";
                pendingFrame_.clear();
            } else {
                std::vector<std::uint8_t> joined;
                joined.reserve(pendingSize + size);
                joined.insert(joined.end(), pendingFrame_.begin(), pendingFrame_.end());
                joined.insert(joined.end(), data, data + size);
                const std::uint64_t joinedPts = pendingFramePts90k_;
                const bool joinedHasPts = pendingFrameHasPts_;
                pendingFrame_.clear();
                return decodeFrame(joined.data(), joined.size(), joinedPts,
                                   joinedHasPts, output, error);
            }
        }

        return decodeFrame(data, size, pts90k, hasPts, output, error);
    }

    void reset() override {
        timestamps_.clear();
        headerPending_.clear();
        pendingFrame_.clear();
        nextToken_ = 1;
        outputFrames_ = 0;
        headerAttempts_ = 0;
        decodeErrors_ = 0;
        pendingEvents_ = 0;
        resolutionRetries_ = 0;
        headerReady_ = false;
        width_ = 0;
        height_ = 0;
        outputBuffers_ = {};
        outputStorage_.clear();
        if (!codec_) return;

        ivd_ctl_reset_ip_t ip{};
        ivd_ctl_reset_op_t op{};
        ip.u4_size = sizeof(ip);
        ip.e_cmd = IVD_CMD_VIDEO_CTL;
        ip.e_sub_cmd = IVD_CMD_CTL_RESET;
        op.u4_size = sizeof(op);
        (void)ih264d_api_function(codec_, &ip, &op);
        std::string ignored;
        (void)setDecodeMode(IVD_DECODE_HEADER, ignored);
    }

private:
    struct TimestampEntry {
        UWORD32 token = 0;
        std::uint64_t pts90k = 0;
        bool hasPts = false;
    };

    static std::string hex(UWORD32 value) {
        static constexpr char digits[] = "0123456789abcdef";
        std::string out(8, '0');
        for (int i = 7; i >= 0; --i) {
            out[static_cast<std::size_t>(i)] = digits[value & 0x0fU];
            value >>= 4U;
        }
        return out;
    }

    bool initialize(std::string& error) {
        error.clear();
        release();

        ih264d_create_ip_t createIp{};
        ih264d_create_op_t createOp{};
        createIp.s_ivd_create_ip_t.u4_size = sizeof(createIp);
        createIp.s_ivd_create_ip_t.e_cmd = IVD_CMD_CREATE;
        createIp.s_ivd_create_ip_t.u4_share_disp_buf = 0;
        createIp.s_ivd_create_ip_t.e_output_format = IV_YUV_420P;
        createIp.s_ivd_create_ip_t.pf_aligned_alloc = avcAlignedAlloc;
        createIp.s_ivd_create_ip_t.pf_aligned_free = avcAlignedFree;
        createIp.s_ivd_create_ip_t.pv_mem_ctxt = nullptr;
        createIp.u4_enable_frame_info = 0;
        createIp.u4_keep_threads_active = 0;
        createOp.s_ivd_create_op_t.u4_size = sizeof(createOp);

        const IV_API_CALL_STATUS_T createStatus =
            ih264d_api_function(nullptr, &createIp, &createOp);
        if (createStatus != IV_SUCCESS ||
            !createOp.s_ivd_create_op_t.pv_handle) {
            error = "Ittiam AVC create failed: 0x" +
                hex(createOp.s_ivd_create_op_t.u4_error_code);
            return false;
        }

        codec_ = static_cast<iv_obj_t*>(createOp.s_ivd_create_op_t.pv_handle);
        codec_->u4_size = sizeof(iv_obj_t);
        codec_->pv_fxns = reinterpret_cast<void*>(&ih264d_api_function);

        // Two decode threads are enough for SD/HD live IPTV and avoid creating
        // a large worker pool for every preview/transcode instance.
        ih264d_ctl_set_num_cores_ip_t coresIp{};
        ih264d_ctl_set_num_cores_op_t coresOp{};
        coresIp.u4_size = sizeof(coresIp);
        coresIp.e_cmd = IVD_CMD_VIDEO_CTL;
        coresIp.e_sub_cmd = static_cast<IVD_CONTROL_API_COMMAND_TYPE_T>(
            IH264D_CMD_CTL_SET_NUM_CORES);
        coresIp.u4_num_cores = 2;
        coresOp.u4_size = sizeof(coresOp);
        (void)ih264d_api_function(codec_, &coresIp, &coresOp);

        if (!setDecodeMode(IVD_DECODE_HEADER, error)) {
            release();
            return false;
        }

        std::cerr << "NATIVE AVC DECODER backend=ittiam-libavc"
                  << " interlaced=1 threads=2 streaming=1\n";
        return true;
    }

    bool setDecodeMode(IVD_VIDEO_DECODE_MODE_T mode, std::string& error) {
        if (!codec_) {
            error = "Ittiam AVC decoder handle unavailable";
            return false;
        }
        ivd_ctl_set_config_ip_t ip{};
        ivd_ctl_set_config_op_t op{};
        ip.u4_size = sizeof(ip);
        ip.e_cmd = IVD_CMD_VIDEO_CTL;
        ip.e_sub_cmd = IVD_CMD_CTL_SETPARAMS;
        ip.u4_disp_wd = 0;
        ip.e_frm_skip_mode = IVD_SKIP_NONE;
        ip.e_frm_out_mode = IVD_DISPLAY_FRAME_OUT;
        ip.e_vid_dec_mode = mode;
        op.u4_size = sizeof(op);
        const IV_API_CALL_STATUS_T status =
            ih264d_api_function(codec_, &ip, &op);
        if (status != IV_SUCCESS) {
            error = "Ittiam AVC set decode mode failed: 0x" +
                hex(op.u4_error_code);
            return false;
        }
        return true;
    }

    bool resetCodecForFrame(std::string& error) {
        ivd_ctl_reset_ip_t ip{};
        ivd_ctl_reset_op_t op{};
        ip.u4_size = sizeof(ip);
        ip.e_cmd = IVD_CMD_VIDEO_CTL;
        ip.e_sub_cmd = IVD_CMD_CTL_RESET;
        op.u4_size = sizeof(op);
        const IV_API_CALL_STATUS_T status =
            ih264d_api_function(codec_, &ip, &op);
        if (status != IV_SUCCESS) {
            error = "Ittiam AVC reset after resolution change failed";
            return false;
        }
        return setDecodeMode(IVD_DECODE_FRAME, error);
    }

    bool allocateOutputBuffers(std::string& error) {
        ivd_ctl_getbufinfo_ip_t infoIp{};
        ivd_ctl_getbufinfo_op_t infoOp{};
        infoIp.u4_size = sizeof(infoIp);
        infoIp.e_cmd = IVD_CMD_VIDEO_CTL;
        infoIp.e_sub_cmd = IVD_CMD_CTL_GETBUFINFO;
        infoOp.u4_size = sizeof(infoOp);

        const IV_API_CALL_STATUS_T status =
            ih264d_api_function(codec_, &infoIp, &infoOp);

        outputBuffers_ = {};
        outputStorage_.clear();

        if (status == IV_SUCCESS && infoOp.u4_min_num_out_bufs > 0 &&
            infoOp.u4_min_num_out_bufs <= IVD_VIDDEC_MAX_IO_BUFFERS) {
            outputStorage_.resize(infoOp.u4_min_num_out_bufs);
            outputBuffers_.u4_num_bufs = infoOp.u4_min_num_out_bufs;
            for (UWORD32 i = 0; i < infoOp.u4_min_num_out_bufs; ++i) {
                const std::size_t bytes = std::max<std::size_t>(
                    1U, infoOp.u4_min_out_buf_size[i]);
                outputStorage_[i].resize(bytes);
                outputBuffers_.pu1_bufs[i] = outputStorage_[i].data();
                outputBuffers_.u4_min_out_buf_size[i] =
                    static_cast<UWORD32>(outputStorage_[i].size());
            }
            return true;
        }

        // Conservative planar 4:2:0 fallback if an older libavc build does not
        // expose GETBUFINFO after header parsing.
        if (width_ <= 0 || height_ <= 0 || (width_ & 1) || (height_ & 1)) {
            error = "Ittiam AVC reported invalid output geometry";
            return false;
        }
        const std::size_t alignedW = static_cast<std::size_t>((width_ + 31) & ~31);
        const std::size_t alignedH = static_cast<std::size_t>((height_ + 31) & ~31);
        const std::size_t y = alignedW * alignedH;
        const std::size_t c = (alignedW / 2U) * (alignedH / 2U);
        const std::array<std::size_t, 3> sizes{y, c, c};
        outputStorage_.resize(3);
        outputBuffers_.u4_num_bufs = 3;
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            outputStorage_[i].resize(sizes[i]);
            outputBuffers_.pu1_bufs[i] = outputStorage_[i].data();
            outputBuffers_.u4_min_out_buf_size[i] =
                static_cast<UWORD32>(sizes[i]);
        }
        return true;
    }

    void saveHeaderTail(const std::uint8_t* data, std::size_t size,
                        std::uint64_t pts90k, bool hasPts) {
        if (!data || size == 0) return;
        if (size > kMaxBufferedAvcBytes) {
            std::cerr << "NATIVE AVC ITTIAM header tail too large bytes="
                      << size << " action=drop\n";
            headerPending_.clear();
            return;
        }
        headerPending_.assign(data, data + size);
        headerPendingPts90k_ = pts90k;
        headerPendingHasPts_ = hasPts;
        ++pendingEvents_;
        if (pendingEvents_ <= 8U || (pendingEvents_ % 100U) == 0U) {
            std::cerr << "NATIVE AVC ITTIAM header pending bytes=" << size
                      << " events=" << pendingEvents_ << '\n';
        }
    }

    void saveFrameTail(const std::uint8_t* data, std::size_t size,
                       std::uint64_t pts90k, bool hasPts) {
        if (!data || size == 0) return;
        if (size > kMaxBufferedAvcBytes) {
            std::cerr << "NATIVE AVC ITTIAM frame tail too large bytes="
                      << size << " action=drop\n";
            pendingFrame_.clear();
            return;
        }
        pendingFrame_.assign(data, data + size);
        pendingFramePts90k_ = pts90k;
        pendingFrameHasPts_ = hasPts;
        ++pendingEvents_;
        if (pendingEvents_ <= 8U || (pendingEvents_ % 100U) == 0U) {
            std::cerr << "NATIVE AVC ITTIAM frame pending bytes=" << size
                      << " events=" << pendingEvents_ << '\n';
        }
    }

    bool decodeHeaders(const std::uint8_t* data, std::size_t size,
                       std::uint64_t pts90k, bool hasPts,
                       std::vector<RawVideoFrame>& output,
                       std::string& error) {
        std::vector<std::uint8_t> joined;
        const std::uint8_t* input = data;
        std::size_t inputSize = size;
        std::uint64_t inputPts = pts90k;
        bool inputHasPts = hasPts;

        if (!headerPending_.empty()) {
            const std::size_t pendingSize = headerPending_.size();
            inputPts = headerPendingPts90k_;
            inputHasPts = headerPendingHasPts_;
            if (pendingSize > kMaxBufferedAvcBytes - std::min(size, kMaxBufferedAvcBytes)) {
                std::cerr << "NATIVE AVC ITTIAM header pending overflow bytes="
                          << pendingSize << " incoming=" << size
                          << " action=drop-stale-tail\n";
                headerPending_.clear();
                inputPts = pts90k;
                inputHasPts = hasPts;
            } else {
                joined.reserve(pendingSize + size);
                joined.insert(joined.end(), headerPending_.begin(), headerPending_.end());
                joined.insert(joined.end(), data, data + size);
                headerPending_.clear();
                input = joined.data();
                inputSize = joined.size();
            }
        }

        std::size_t offset = 0;
        for (unsigned guard = 0;
             guard < kMaxDecodeCallsPerInput && offset < inputSize; ++guard) {
            ivd_video_decode_ip_t ip{};
            ivd_video_decode_op_t op{};
            ip.u4_size = sizeof(ip);
            ip.e_cmd = IVD_CMD_VIDEO_DECODE;
            ip.u4_ts = 0;
            ip.u4_num_Bytes = static_cast<UWORD32>(inputSize - offset);
            ip.pv_stream_buffer = const_cast<std::uint8_t*>(input + offset);
            op.u4_size = sizeof(op);

            const IV_API_CALL_STATUS_T status =
                ih264d_api_function(codec_, &ip, &op);
            ++headerAttempts_;

            if (op.u4_pic_wd != 0 && op.u4_pic_ht != 0) {
                width_ = static_cast<int>(op.u4_pic_wd);
                height_ = static_cast<int>(op.u4_pic_ht);
                if (!allocateOutputBuffers(error) ||
                    !setDecodeMode(IVD_DECODE_FRAME, error))
                    return false;
                headerReady_ = true;
                headerPending_.clear();
                std::cerr << "NATIVE AVC ITTIAM header size="
                          << width_ << "x" << height_
                          << " progressive=" << op.u4_progressive_frame_flag
                          << " attempts=" << headerAttempts_
                          << " consumed=" << op.u4_num_bytes_consumed << '\n';

                // Re-feed the byte range that completed header discovery in
                // frame mode. Header decode may have consumed SPS/PPS and part
                // of an IDR; the frame path must see the same Annex-B bytes.
                return decodeFrame(input, inputSize, inputPts, inputHasPts,
                                   output, error);
            }

            if (status != IV_SUCCESS && IS_IVD_FATAL_ERROR(op.u4_error_code)) {
                error = "Ittiam AVC header fatal error: 0x" + hex(op.u4_error_code);
                return false;
            }

            const std::size_t consumed = std::min<std::size_t>(
                op.u4_num_bytes_consumed, inputSize - offset);
            if (status != IV_SUCCESS &&
                (headerAttempts_ <= 8U || (headerAttempts_ % 100U) == 0U)) {
                std::cerr << "NATIVE AVC ITTIAM header wait status="
                          << static_cast<int>(status)
                          << " error=0x" << hex(op.u4_error_code)
                          << " consumed=" << consumed
                          << " remaining=" << (inputSize - offset) << '\n';
            }

            if (consumed == 0) {
                saveHeaderTail(input + offset, inputSize - offset,
                               inputPts, inputHasPts);
                return true;
            }
            offset += consumed;
        }

        if (offset < inputSize) {
            saveHeaderTail(input + offset, inputSize - offset,
                           inputPts, inputHasPts);
        }
        return true;
    }

    bool decodeFrame(const std::uint8_t* data, std::size_t size,
                     std::uint64_t pts90k, bool hasPts,
                     std::vector<RawVideoFrame>& output,
                     std::string& error) {
        UWORD32 token = nextToken_++;
        if (token == 0) token = nextToken_++;
        timestamps_.push_back({token, pts90k, hasPts});
        while (timestamps_.size() > 512) timestamps_.pop_front();

        std::size_t offset = 0;
        unsigned resolutionRetries = 0;
        unsigned guard = 0;
        for (; guard < kMaxDecodeCallsPerInput && offset < size; ++guard) {
            ivd_video_decode_ip_t ip{};
            ivd_video_decode_op_t op{};
            ip.u4_size = sizeof(ip);
            ip.e_cmd = IVD_CMD_VIDEO_DECODE;
            ip.u4_ts = token;
            ip.u4_num_Bytes = static_cast<UWORD32>(size - offset);
            ip.pv_stream_buffer = const_cast<std::uint8_t*>(data + offset);
            ip.s_out_buffer = outputBuffers_;
            op.u4_size = sizeof(op);

            const IV_API_CALL_STATUS_T status =
                ih264d_api_function(codec_, &ip, &op);

            // libavc's own decoder harness resets and re-feeds exactly the
            // same input when IVD_RES_CHANGED is reported. Do not advance the
            // pointer here: dropping the triggering SPS/IDR can leave a live
            // broadcast stream permanently without decoded pictures.
            const bool resolutionChanged =
                (op.u4_error_code & IVD_ERROR_MASK) == IVD_RES_CHANGED;
            if (resolutionChanged) {
                ++resolutionRetries;
                ++resolutionRetries_;
                std::cerr << "NATIVE AVC ITTIAM resolution change error=0x"
                          << hex(op.u4_error_code)
                          << " retry=" << resolutionRetries
                          << " offset=" << offset << '\n';
                if (resolutionRetries > 2U) {
                    saveFrameTail(data + offset, size - offset, pts90k, hasPts);
                    return true;
                }
                if (!resetCodecForFrame(error)) return false;
                timestamps_.clear();
                timestamps_.push_back({token, pts90k, hasPts});
                continue;
            }
            resolutionRetries = 0;

            if (op.u4_output_present) {
                RawVideoFrame frame;
                if (!copyFrame(op, pts90k, hasPts, frame, error))
                    return false;
                output.push_back(std::move(frame));
                ++outputFrames_;
                if (outputFrames_ == 1U || (outputFrames_ % 100U) == 0U) {
                    std::cerr << "NATIVE AVC ITTIAM OUTPUT frames="
                              << outputFrames_
                              << " size=" << op.s_disp_frm_buf.u4_y_wd
                              << "x" << op.s_disp_frm_buf.u4_y_ht
                              << " progressive=" << op.u4_progressive_frame_flag
                              << " pic_type=" << static_cast<int>(op.e_pic_type)
                              << '\n';
                }
            }

            const std::size_t consumed = std::min<std::size_t>(
                op.u4_num_bytes_consumed, size - offset);
            offset += consumed;

            if (status != IV_SUCCESS) {
                ++decodeErrors_;
                if (IS_IVD_FATAL_ERROR(op.u4_error_code)) {
                    error = "Ittiam AVC decode fatal error: 0x" +
                        hex(op.u4_error_code);
                    return false;
                }
                if (decodeErrors_ <= 8U || (decodeErrors_ % 100U) == 0U) {
                    std::cerr << "NATIVE AVC ITTIAM recoverable status="
                              << static_cast<int>(status)
                              << " error=0x" << hex(op.u4_error_code)
                              << " consumed=" << consumed
                              << " remaining=" << (size - offset)
                              << '\n';
                }
            }

            if (consumed == 0) {
                saveFrameTail(data + offset, size - offset, pts90k, hasPts);
                return true;
            }
        }

        if (offset < size) {
            saveFrameTail(data + offset, size - offset, pts90k, hasPts);
        }
        return true;
    }

    bool copyFrame(const ivd_video_decode_op_t& op,
                   std::uint64_t fallbackPts, bool fallbackHasPts,
                   RawVideoFrame& frame, std::string& error) {
        const auto& src = op.s_disp_frm_buf;
        const int width = static_cast<int>(src.u4_y_wd != 0 ? src.u4_y_wd : op.u4_pic_wd);
        const int height = static_cast<int>(src.u4_y_ht != 0 ? src.u4_y_ht : op.u4_pic_ht);
        if (width <= 0 || height <= 0 || (width & 1) || (height & 1) ||
            !src.pv_y_buf || !src.pv_u_buf || !src.pv_v_buf ||
            src.u4_y_strd < static_cast<UWORD32>(width) ||
            src.u4_u_strd < static_cast<UWORD32>(width / 2) ||
            src.u4_v_strd < static_cast<UWORD32>(width / 2)) {
            error = "Ittiam AVC decoder returned invalid I420 frame";
            return false;
        }

        frame.width = width;
        frame.height = height;
        frame.i420.resize(static_cast<std::size_t>(width) * height * 3U / 2U);

        const auto* y = static_cast<const std::uint8_t*>(src.pv_y_buf);
        const auto* u = static_cast<const std::uint8_t*>(src.pv_u_buf);
        const auto* v = static_cast<const std::uint8_t*>(src.pv_v_buf);
        auto* dstY = frame.i420.data();
        auto* dstU = dstY + static_cast<std::size_t>(width) * height;
        auto* dstV = dstU + static_cast<std::size_t>(width / 2) * (height / 2);

        for (int row = 0; row < height; ++row)
            std::memcpy(dstY + static_cast<std::size_t>(row) * width,
                        y + static_cast<std::size_t>(row) * src.u4_y_strd,
                        static_cast<std::size_t>(width));
        for (int row = 0; row < height / 2; ++row) {
            std::memcpy(dstU + static_cast<std::size_t>(row) * (width / 2),
                        u + static_cast<std::size_t>(row) * src.u4_u_strd,
                        static_cast<std::size_t>(width / 2));
            std::memcpy(dstV + static_cast<std::size_t>(row) * (width / 2),
                        v + static_cast<std::size_t>(row) * src.u4_v_strd,
                        static_cast<std::size_t>(width / 2));
        }

        frame.pts90k = fallbackPts;
        frame.hasPts = fallbackHasPts;
        for (auto it = timestamps_.begin(); it != timestamps_.end(); ++it) {
            if (it->token == op.u4_ts) {
                frame.pts90k = it->pts90k;
                frame.hasPts = it->hasPts;
                timestamps_.erase(it);
                break;
            }
        }
        frame.dts90k = frame.pts90k;
        frame.hasDts = frame.hasPts;
        frame.keyFrame = op.e_pic_type == IV_IDR_FRAME || op.e_pic_type == IV_I_FRAME;
        return true;
    }

    void release() {
        if (codec_) {
            ivd_delete_ip_t ip{};
            ivd_delete_op_t op{};
            ip.u4_size = sizeof(ip);
            ip.e_cmd = IVD_CMD_DELETE;
            op.u4_size = sizeof(op);
            (void)ih264d_api_function(codec_, &ip, &op);
        }
        codec_ = nullptr;
        valid_ = false;
        headerReady_ = false;
        outputBuffers_ = {};
        outputStorage_.clear();
        timestamps_.clear();
        headerPending_.clear();
        pendingFrame_.clear();
    }

    iv_obj_t* codec_ = nullptr;
    bool valid_ = false;
    bool headerReady_ = false;
    int width_ = 0;
    int height_ = 0;
    ivd_out_bufdesc_t outputBuffers_{};
    std::vector<std::vector<std::uint8_t>> outputStorage_;
    std::deque<TimestampEntry> timestamps_;
    std::vector<std::uint8_t> headerPending_;
    std::vector<std::uint8_t> pendingFrame_;
    std::uint64_t headerPendingPts90k_ = 0;
    std::uint64_t pendingFramePts90k_ = 0;
    bool headerPendingHasPts_ = false;
    bool pendingFrameHasPts_ = false;
    UWORD32 nextToken_ = 1;
    std::uint64_t outputFrames_ = 0;
    std::uint64_t headerAttempts_ = 0;
    std::uint64_t decodeErrors_ = 0;
    std::uint64_t pendingEvents_ = 0;
    std::uint64_t resolutionRetries_ = 0;
    std::string initError_;
};

} // namespace

std::unique_ptr<VideoDecoder> createIttiamAvcDecoder(std::string& error) {
    auto decoder = std::make_unique<IttiamAvcDecoder>();
    if (!decoder->valid()) {
        error = decoder->initError().empty()
            ? "Ittiam AVC decoder initialization failed"
            : decoder->initError();
        return {};
    }
    error.clear();
    return decoder;
}

} // namespace dvbstreamer5::media::codec
