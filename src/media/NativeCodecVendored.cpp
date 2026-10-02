#include "media/NativeCodecRuntime.h"
#include "media/NativeHardwareCodec.h"

#include <wels/codec_api.h>
#include <libde265/de265.h>
#include <kvazaar.h>
#include <fdk-aac/aacdecoder_lib.h>
#include <fdk-aac/aacenc_lib.h>
extern "C" {
#include <iv_datatypedef.h>
#include <iv.h>
#include <ivd.h>
#include <impeg2d.h>
}
#define PL_MPEG_IMPLEMENTATION
#define PLM_NO_STDIO
#include <pl_mpeg/pl_mpeg.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <iostream>

namespace dvbstreamer5::media::codec {
namespace {

struct H264AnnexBInfo {
    bool annexB = false;
    bool hasSps = false;
    bool hasPps = false;
    bool hasIdr = false;
    bool hasIntraPicture = false;
    std::vector<std::uint8_t> sps;
    std::vector<std::uint8_t> pps;
};

std::size_t findAnnexBStartCode(const std::uint8_t* data, std::size_t size,
                                std::size_t from, std::size_t& length) {
    length = 0;
    if (!data || from >= size) return size;
    for (std::size_t i = from; i + 3 <= size; ++i) {
        if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 &&
            data[i + 2] == 0 && data[i + 3] == 1) {
            length = 4;
            return i;
        }
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            length = 3;
            return i;
        }
    }
    return size;
}

std::vector<std::uint8_t> copyAnnexBNal(const std::uint8_t* data,
                                        std::size_t begin,
                                        std::size_t end) {
    while (end > begin && data[end - 1] == 0) --end;
    if (end <= begin) return {};
    std::vector<std::uint8_t> result = {0, 0, 0, 1};
    result.insert(result.end(), data + begin, data + end);
    return result;
}

// Read just enough of an AVC slice header to classify the picture type.
// first_mb_in_slice and slice_type are the first two Exp-Golomb values and do
// not depend on SPS/PPS, which makes this safe for a live join where parameter
// sets may arrive shortly before the first decodable intra picture.
bool h264NalIsFirstMbZero(const std::uint8_t* nal, std::size_t size) {
    if (!nal || size < 2) return false;
    const std::uint8_t nalType = static_cast<std::uint8_t>(nal[0] & 0x1fU);
    if (nalType != 1 && nalType != 5) return false;

    std::vector<std::uint8_t> rbsp;
    rbsp.reserve(size - 1);
    unsigned zeros = 0;
    for (std::size_t i = 1; i < size; ++i) {
        const std::uint8_t v = nal[i];
        if (zeros >= 2 && v == 0x03) { zeros = 0; continue; }
        rbsp.push_back(v);
        zeros = (v == 0) ? zeros + 1U : 0U;
    }
    std::size_t bit = 0;
    unsigned zerosCount = 0;
    for (;;) {
        if (bit >= rbsp.size() * 8U || zerosCount > 31U) return false;
        const int b = (rbsp[bit / 8U] >> (7U - (bit % 8U))) & 1U;
        ++bit;
        if (b != 0) break;
        ++zerosCount;
    }
    std::uint32_t suffix = 0;
    for (unsigned i = 0; i < zerosCount; ++i) {
        if (bit >= rbsp.size() * 8U) return false;
        const int b = (rbsp[bit / 8U] >> (7U - (bit % 8U))) & 1U;
        ++bit;
        suffix = (suffix << 1U) | static_cast<std::uint32_t>(b);
    }
    const std::uint32_t firstMb = ((1U << zerosCount) - 1U) + suffix;
    return firstMb == 0U;
}

bool h264NalIsIntraSlice(const std::uint8_t* nal, std::size_t size) {
    if (!nal || size < 2) return false;
    const std::uint8_t nalType = static_cast<std::uint8_t>(nal[0] & 0x1fU);
    if (nalType == 5) return true;
    if (nalType != 1) return false;

    std::vector<std::uint8_t> rbsp;
    rbsp.reserve(size - 1);
    unsigned zeros = 0;
    for (std::size_t i = 1; i < size; ++i) {
        const std::uint8_t v = nal[i];
        if (zeros >= 2 && v == 0x03) { zeros = 0; continue; }
        rbsp.push_back(v);
        zeros = (v == 0) ? zeros + 1U : 0U;
    }
    std::size_t bit = 0;
    const auto readBit = [&]() -> int {
        if (bit >= rbsp.size() * 8U) return -1;
        const int value = (rbsp[bit / 8U] >> (7U - (bit % 8U))) & 1U;
        ++bit;
        return value;
    };
    const auto readUe = [&]() -> std::uint32_t {
        unsigned zerosCount = 0;
        for (;;) {
            const int b = readBit();
            if (b < 0 || zerosCount > 31) return std::numeric_limits<std::uint32_t>::max();
            if (b != 0) break;
            ++zerosCount;
        }
        std::uint32_t suffix = 0;
        for (unsigned i = 0; i < zerosCount; ++i) {
            const int b = readBit();
            if (b < 0) return std::numeric_limits<std::uint32_t>::max();
            suffix = (suffix << 1U) | static_cast<std::uint32_t>(b);
        }
        return ((1U << zerosCount) - 1U) + suffix;
    };

    if (readUe() == std::numeric_limits<std::uint32_t>::max()) return false; // first_mb_in_slice
    const std::uint32_t sliceType = readUe();
    if (sliceType == std::numeric_limits<std::uint32_t>::max()) return false;
    const std::uint32_t normalized = sliceType % 5U;
    return normalized == 2U || normalized == 4U; // I or SI
}

H264AnnexBInfo inspectH264AnnexB(const std::uint8_t* data, std::size_t size) {
    H264AnnexBInfo info;
    std::size_t startCodeLength = 0;
    std::size_t start = findAnnexBStartCode(data, size, 0, startCodeLength);
    while (start < size) {
        info.annexB = true;
        const std::size_t nalBegin = start + startCodeLength;
        if (nalBegin >= size) break;
        std::size_t nextStartCodeLength = 0;
        const std::size_t next = findAnnexBStartCode(
            data, size, nalBegin, nextStartCodeLength);
        const std::size_t nalEnd = next < size ? next : size;
        const std::uint8_t type = static_cast<std::uint8_t>(data[nalBegin] & 0x1fU);
        if (type == 7) {
            info.hasSps = true;
            info.sps = copyAnnexBNal(data, nalBegin, nalEnd);
        } else if (type == 8) {
            info.hasPps = true;
            info.pps = copyAnnexBNal(data, nalBegin, nalEnd);
        } else if (type == 5) {
            info.hasIdr = true;
            info.hasIntraPicture = true;
        } else if (type == 1 && h264NalIsIntraSlice(data + nalBegin, nalEnd - nalBegin)) {
            info.hasIntraPicture = true;
        }
        if (next >= size) break;
        start = next;
        startCodeLength = nextStartCodeLength;
    }
    return info;
}


struct H265AnnexBInfo {
    bool annexB = false;
    bool hasVps = false;
    bool hasSps = false;
    bool hasPps = false;
    bool hasIrap = false;
    std::vector<std::uint8_t> vps;
    std::vector<std::uint8_t> sps;
    std::vector<std::uint8_t> pps;
};

bool startsWithAnnexB(const std::uint8_t* data, std::size_t size) {
    return data && ((size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) ||
                    (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1));
}

void appendNormalizedAnnexBNal(std::vector<std::uint8_t>& out,
                               const std::uint8_t* data, std::size_t size) {
    if (!data || size == 0) return;
    if (!startsWithAnnexB(data, size)) out.insert(out.end(), {0, 0, 0, 1});
    out.insert(out.end(), data, data + size);
}

H265AnnexBInfo inspectH265AnnexB(const std::uint8_t* data, std::size_t size) {
    H265AnnexBInfo info;
    std::size_t startCodeLength = 0;
    std::size_t start = findAnnexBStartCode(data, size, 0, startCodeLength);
    while (start < size) {
        info.annexB = true;
        const std::size_t nalBegin = start + startCodeLength;
        if (nalBegin + 1 >= size) break;
        std::size_t nextStartCodeLength = 0;
        const std::size_t next = findAnnexBStartCode(data, size, nalBegin, nextStartCodeLength);
        const std::size_t nalEnd = next < size ? next : size;
        const std::uint8_t type = static_cast<std::uint8_t>((data[nalBegin] >> 1) & 0x3fU);
        if (type == 32) { info.hasVps = true; info.vps = copyAnnexBNal(data, nalBegin, nalEnd); }
        else if (type == 33) { info.hasSps = true; info.sps = copyAnnexBNal(data, nalBegin, nalEnd); }
        else if (type == 34) { info.hasPps = true; info.pps = copyAnnexBNal(data, nalBegin, nalEnd); }
        else if (type >= 16 && type <= 23) info.hasIrap = true;
        if (next >= size) break;
        start = next;
        startCodeLength = nextStartCodeLength;
    }
    return info;
}


std::size_t openH264ReadPictureLength(const std::uint8_t* data, std::size_t size) {
    if (!data || size < 5) return 0;

    unsigned spsCount = 0;
    unsigned ppsCount = 0;
    unsigned nonIdrPictureCount = 0;
    unsigned idrPictureCount = 0;
    unsigned audCount = 0;

    for (std::size_t pos = 0; pos + 4 <= size; ++pos) {
        bool start4 = pos + 4 <= size && data[pos] == 0 && data[pos + 1] == 0 &&
                      data[pos + 2] == 0 && data[pos + 3] == 1;
        bool start3 = !start4 && pos + 3 <= size && data[pos] == 0 &&
                      data[pos + 1] == 0 && data[pos + 2] == 1;
        if (!start4 && !start3) continue;

        const std::size_t sc = start4 ? 4U : 3U;
        if (pos + sc >= size) return 0;
        const std::uint8_t type = static_cast<std::uint8_t>(data[pos + sc] & 0x1fU);

        if (type == 1 || type == 5) {
            // OpenH264's own h264dec console tool uses first_mb_in_slice == 0
            // to find the next primary coded picture.  Read only that syntax
            // element; it is the first Exp-Golomb value after the NAL header.
            const bool firstMbZero = h264NalIsFirstMbZero(
                data + pos + sc, size - (pos + sc));
            if (type == 1) {
                ++nonIdrPictureCount;
                if (firstMbZero &&
                    ((nonIdrPictureCount >= 1U && idrPictureCount >= 1U) ||
                     nonIdrPictureCount >= 2U)) {
                    return pos;
                }
            } else {
                ++idrPictureCount;
                if (firstMbZero &&
                    ((idrPictureCount >= 1U && nonIdrPictureCount >= 1U) ||
                     idrPictureCount >= 2U)) {
                    return pos;
                }
            }
        } else if (type == 7) {
            ++spsCount;
            if ((spsCount >= 1U && (nonIdrPictureCount >= 1U || idrPictureCount >= 1U)) ||
                spsCount >= 2U) {
                return pos;
            }
        } else if (type == 8) {
            ++ppsCount;
            if (ppsCount >= 1U && (nonIdrPictureCount >= 1U || idrPictureCount >= 1U)) {
                return pos;
            }
        } else if (type == 9) {
            ++audCount;
            if (audCount >= 2U) return pos;
        }
    }
    return 0;
}


class IttiamMpeg2Decoder final : public VideoDecoder {
public:
    IttiamMpeg2Decoder() {
        std::string ignored;
        valid_ = initialize(ignored);
        if (!valid_ && !ignored.empty()) {
            std::cerr << "NATIVE MPEG2 DECODER init failed: "
                      << ignored << '\n';
        }
    }

    ~IttiamMpeg2Decoder() override {
        release();
    }

    bool valid() const noexcept {
        return valid_;
    }

    bool decode(const std::uint8_t* data, std::size_t size,
                std::uint64_t pts90k, bool hasPts,
                std::vector<RawVideoFrame>& output,
                std::string& error) override {
        error.clear();
        if (!valid_ || !codec_) {
            error = "Ittiam MPEG-2 decoder unavailable";
            return false;
        }
        if (!data || size == 0) return true;

        // NativeTsDemux supplies elementary MPEG-2 video PES payloads. Keep
        // unconsumed bytes between PES calls because the decoder may stop at a
        // picture/sequence boundary before consuming the complete buffer.
        if (pending_.size() + size > kMaxPendingBytes) {
            const auto sequence = findLastSequenceHeader(data, size);
            pending_.clear();
            if (sequence < size) {
                pending_.insert(pending_.end(), data + sequence, data + size);
            } else {
                error = "MPEG-2 decoder pending input exceeded 16 MiB";
                return false;
            }
        } else {
            pending_.insert(pending_.end(), data, data + size);
        }

        for (unsigned guard = 0; guard < 64 && !pending_.empty(); ++guard) {
            ivd_video_decode_ip_t ip{};
            ivd_video_decode_op_t op{};
            ip.u4_size = sizeof(ip);
            ip.e_cmd = IVD_CMD_VIDEO_DECODE;
            ip.u4_ts = static_cast<UWORD32>(pts90k & 0xffffffffULL);
            ip.u4_num_Bytes = static_cast<UWORD32>(
                std::min<std::size_t>(pending_.size(),
                                      std::numeric_limits<UWORD32>::max()));
            ip.pv_stream_buffer = pending_.data();
            ip.s_out_buffer = outputBuffers_;
            op.u4_size = sizeof(op);

            const IV_API_CALL_STATUS_T status =
                impeg2d_api_function(codec_, &ip, &op);

            if (op.u4_output_present) {
                RawVideoFrame frame;
                if (!copyFrame(op, pts90k, hasPts, frame, error)) {
                    return false;
                }
                output.push_back(std::move(frame));
                ++outputFrames_;
                if (outputFrames_ == 1U || (outputFrames_ % 100U) == 0U) {
                    std::cerr << "NATIVE MPEG2 OUTPUT frames=" << outputFrames_
                              << " size=" << op.s_disp_frm_buf.u4_y_wd
                              << "x" << op.s_disp_frm_buf.u4_y_ht << '\n';
                }
            }

            const std::size_t consumed =
                std::min<std::size_t>(op.u4_num_bytes_consumed, pending_.size());
            if (consumed != 0) {
                pending_.erase(
                    pending_.begin(),
                    pending_.begin() + static_cast<std::ptrdiff_t>(consumed));
            }

            if (status != IV_SUCCESS &&
                ((op.u4_error_code >> IVD_FATALERROR) & 1U) != 0U) {
                error = "Ittiam MPEG-2 decode fatal error: 0x" +
                    hex(op.u4_error_code);
                return false;
            }

            // Recoverable "need more data" conditions are normal at PES
            // boundaries. Preserve the bytes and wait for the next PES.
            if (consumed == 0) break;
        }

        return true;
    }

    void reset() override {
        pending_.clear();
        outputFrames_ = 0;
        if (!codec_) return;

        ivd_ctl_reset_ip_t ip{};
        ivd_ctl_reset_op_t op{};
        ip.u4_size = sizeof(ip);
        ip.e_cmd = IVD_CMD_VIDEO_CTL;
        ip.e_sub_cmd = IVD_CMD_CTL_RESET;
        op.u4_size = sizeof(op);
        (void)impeg2d_api_function(codec_, &ip, &op);
        std::string ignored;
        (void)setDecodeMode(ignored);
    }

private:
    static constexpr UWORD32 kMaxWidth = 1920;
    static constexpr UWORD32 kMaxHeight = 1088;
    static constexpr std::size_t kMaxPendingBytes = 16U * 1024U * 1024U;

    static std::string hex(UWORD32 value) {
        static constexpr char digits[] = "0123456789abcdef";
        std::string out(8, '0');
        for (int i = 7; i >= 0; --i) {
            out[static_cast<std::size_t>(i)] = digits[value & 0x0fU];
            value >>= 4U;
        }
        return out;
    }

    static std::size_t nextPowerOfTwo(std::size_t value) {
        std::size_t result = sizeof(void*);
        while (result < value && result <= (std::numeric_limits<std::size_t>::max() >> 1U)) {
            result <<= 1U;
        }
        return result;
    }

    static void* alignedAllocate(std::size_t alignment, std::size_t size) {
        if (size == 0) size = 1;
        alignment = nextPowerOfTwo(std::max<std::size_t>(alignment, sizeof(void*)));
        void* ptr = nullptr;
#if defined(_WIN32)
        ptr = _aligned_malloc(size, alignment);
#else
        if (::posix_memalign(&ptr, alignment, size) != 0) ptr = nullptr;
#endif
        if (ptr) std::memset(ptr, 0, size);
        return ptr;
    }

    static void alignedRelease(void* ptr) {
        if (!ptr) return;
#if defined(_WIN32)
        _aligned_free(ptr);
#else
        std::free(ptr);
#endif
    }

    static std::size_t findLastSequenceHeader(
        const std::uint8_t* data, std::size_t size) {
        if (!data || size < 4) return size;
        for (std::size_t i = size - 4;; --i) {
            if (data[i] == 0x00 && data[i + 1] == 0x00 &&
                data[i + 2] == 0x01 && data[i + 3] == 0xb3) {
                return i;
            }
            if (i == 0) break;
        }
        return size;
    }

    bool initialize(std::string& error) {
        release();
        error.clear();

        iv_num_mem_rec_ip_t queryIp{};
        iv_num_mem_rec_op_t queryOp{};
        queryIp.u4_size = sizeof(queryIp);
        queryIp.e_cmd = IV_CMD_GET_NUM_MEM_REC;
        queryOp.u4_size = sizeof(queryOp);
        if (impeg2d_api_function(nullptr, &queryIp, &queryOp) != IV_SUCCESS ||
            queryOp.u4_num_mem_rec == 0) {
            error = "Ittiam MPEG-2 get memory records failed";
            return false;
        }

        memoryRecords_.resize(queryOp.u4_num_mem_rec);
        for (auto& record : memoryRecords_) {
            record = {};
            record.u4_size = sizeof(iv_mem_rec_t);
        }

        impeg2d_fill_mem_rec_ip_t fillIp{};
        impeg2d_fill_mem_rec_op_t fillOp{};
        fillIp.s_ivd_fill_mem_rec_ip_t.u4_size = sizeof(fillIp);
        fillIp.s_ivd_fill_mem_rec_ip_t.e_cmd = IV_CMD_FILL_NUM_MEM_REC;
        fillIp.s_ivd_fill_mem_rec_ip_t.pv_mem_rec_location =
            memoryRecords_.data();
        fillIp.s_ivd_fill_mem_rec_ip_t.u4_max_frm_wd = kMaxWidth;
        fillIp.s_ivd_fill_mem_rec_ip_t.u4_max_frm_ht = kMaxHeight;
        fillIp.u4_share_disp_buf = 0;
        fillIp.e_output_format = IV_YUV_420P;
        fillIp.u4_deinterlace = 0;
        fillIp.u4_keep_threads_active = 0;
        fillOp.s_ivd_fill_mem_rec_op_t.u4_size = sizeof(fillOp);
        if (impeg2d_api_function(nullptr, &fillIp, &fillOp) != IV_SUCCESS ||
            fillOp.s_ivd_fill_mem_rec_op_t.u4_num_mem_rec_filled == 0) {
            error = "Ittiam MPEG-2 fill memory records failed";
            release();
            return false;
        }

        const std::size_t records = std::min<std::size_t>(
            memoryRecords_.size(),
            fillOp.s_ivd_fill_mem_rec_op_t.u4_num_mem_rec_filled);
        allocations_.reserve(records);
        for (std::size_t i = 0; i < records; ++i) {
            auto& record = memoryRecords_[i];
            record.pv_base = alignedAllocate(
                record.u4_mem_alignment, record.u4_mem_size);
            if (!record.pv_base) {
                error = "Ittiam MPEG-2 memory allocation failed";
                release();
                return false;
            }
            allocations_.push_back(record.pv_base);
        }

        impeg2d_init_ip_t initIp{};
        impeg2d_init_op_t initOp{};
        initIp.s_ivd_init_ip_t.u4_size = sizeof(initIp);
        initIp.s_ivd_init_ip_t.e_cmd =
            static_cast<IVD_API_COMMAND_TYPE_T>(IV_CMD_INIT);
        initIp.s_ivd_init_ip_t.u4_num_mem_rec =
            static_cast<UWORD32>(records);
        initIp.s_ivd_init_ip_t.u4_frm_max_wd = kMaxWidth;
        initIp.s_ivd_init_ip_t.u4_frm_max_ht = kMaxHeight;
        initIp.s_ivd_init_ip_t.e_output_format = IV_YUV_420P;
        initIp.s_ivd_init_ip_t.pv_mem_rec_location = memoryRecords_.data();
        initIp.u4_share_disp_buf = 0;
        initIp.u4_deinterlace = 0;
        initIp.u4_keep_threads_active = 0;
        initOp.s_ivd_init_op_t.u4_size = sizeof(initOp);

        codec_ = static_cast<iv_obj_t*>(memoryRecords_[0].pv_base);
        codec_->u4_size = sizeof(iv_obj_t);
        codec_->pv_fxns =
            reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(
                &impeg2d_api_function));

        if (impeg2d_api_function(codec_, &initIp, &initOp) != IV_SUCCESS) {
            error = "Ittiam MPEG-2 init failed: 0x" +
                hex(initOp.s_ivd_init_op_t.u4_error_code);
            release();
            return false;
        }

        ivd_ctl_getbufinfo_ip_t infoIp{};
        ivd_ctl_getbufinfo_op_t infoOp{};
        infoIp.u4_size = sizeof(infoIp);
        infoIp.e_cmd = IVD_CMD_VIDEO_CTL;
        infoIp.e_sub_cmd = IVD_CMD_CTL_GETBUFINFO;
        infoOp.u4_size = sizeof(infoOp);
        if (impeg2d_api_function(codec_, &infoIp, &infoOp) != IV_SUCCESS ||
            infoOp.u4_min_num_out_bufs == 0 ||
            infoOp.u4_min_num_out_bufs > 3) {
            error = "Ittiam MPEG-2 output buffer query failed";
            release();
            return false;
        }

        outputBuffers_ = {};
        outputBuffers_.u4_num_bufs = infoOp.u4_min_num_out_bufs;
        for (UWORD32 i = 0; i < infoOp.u4_min_num_out_bufs; ++i) {
            outputStorage_[i].resize(infoOp.u4_min_out_buf_size[i]);
            outputBuffers_.pu1_bufs[i] = outputStorage_[i].data();
            outputBuffers_.u4_min_out_buf_size[i] =
                infoOp.u4_min_out_buf_size[i];
        }

        if (!setDecodeMode(error)) {
            release();
            return false;
        }

        valid_ = true;
        return true;
    }

    bool setDecodeMode(std::string& error) {
        if (!codec_) return false;
        ivd_ctl_set_config_ip_t cfgIp{};
        ivd_ctl_set_config_op_t cfgOp{};
        cfgIp.u4_size = sizeof(cfgIp);
        cfgIp.e_cmd = IVD_CMD_VIDEO_CTL;
        cfgIp.e_sub_cmd = IVD_CMD_CTL_SETPARAMS;
        cfgIp.e_vid_dec_mode = IVD_DECODE_FRAME;
        cfgIp.u4_disp_wd = 0;
        cfgIp.e_frm_skip_mode = IVD_SKIP_NONE;
        cfgIp.e_frm_out_mode = IVD_DISPLAY_FRAME_OUT;
        cfgOp.u4_size = sizeof(cfgOp);
        if (impeg2d_api_function(codec_, &cfgIp, &cfgOp) != IV_SUCCESS) {
            error = "Ittiam MPEG-2 set decode mode failed: 0x" +
                hex(cfgOp.u4_error_code);
            return false;
        }
        return true;
    }

    bool copyFrame(const ivd_video_decode_op_t& op,
                   std::uint64_t inputPts, bool inputHasPts,
                   RawVideoFrame& frame, std::string& error) const {
        const auto& src = op.s_disp_frm_buf;
        const int width = static_cast<int>(src.u4_y_wd);
        const int height = static_cast<int>(src.u4_y_ht);
        if (width <= 0 || height <= 0 || (width & 1) || (height & 1) ||
            !src.pv_y_buf || !src.pv_u_buf || !src.pv_v_buf) {
            error = "Ittiam MPEG-2 decoder returned invalid YUV420 frame";
            return false;
        }

        frame.width = width;
        frame.height = height;
        frame.keyFrame = op.e_pic_type == IV_I_FRAME;

        std::uint64_t outputPts =
            (inputPts & ~0xffffffffULL) | static_cast<std::uint64_t>(op.u4_ts);
        if (outputPts + 0x80000000ULL < inputPts) {
            outputPts += 0x100000000ULL;
        } else if (outputPts > inputPts + 0x80000000ULL &&
                   outputPts >= 0x100000000ULL) {
            outputPts -= 0x100000000ULL;
        }
        frame.pts90k = inputHasPts ? outputPts : inputPts;
        frame.dts90k = frame.pts90k;
        frame.hasPts = inputHasPts;
        frame.hasDts = inputHasPts;

        const std::size_t ySize =
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        const std::size_t cWidth = static_cast<std::size_t>(width / 2);
        const std::size_t cHeight = static_cast<std::size_t>(height / 2);
        frame.i420.assign(ySize + 2U * cWidth * cHeight, 0);
        std::fill(frame.i420.begin() + static_cast<std::ptrdiff_t>(ySize),
                  frame.i420.end(), 128);

        const auto* srcY = static_cast<const std::uint8_t*>(src.pv_y_buf);
        const auto* srcU = static_cast<const std::uint8_t*>(src.pv_u_buf);
        const auto* srcV = static_cast<const std::uint8_t*>(src.pv_v_buf);
        auto* dstY = frame.i420.data();
        auto* dstU = dstY + ySize;
        auto* dstV = dstU + cWidth * cHeight;

        for (int row = 0; row < height; ++row) {
            std::memcpy(
                dstY + static_cast<std::size_t>(row) * width,
                srcY + static_cast<std::size_t>(row) * src.u4_y_strd,
                static_cast<std::size_t>(width));
        }
        const std::size_t copyU = std::min<std::size_t>(
            cWidth, src.u4_u_wd);
        const std::size_t copyV = std::min<std::size_t>(
            cWidth, src.u4_v_wd);
        const std::size_t rowsU = std::min<std::size_t>(
            cHeight, src.u4_u_ht);
        const std::size_t rowsV = std::min<std::size_t>(
            cHeight, src.u4_v_ht);
        for (std::size_t row = 0; row < rowsU; ++row) {
            std::memcpy(
                dstU + row * cWidth,
                srcU + row * src.u4_u_strd,
                copyU);
        }
        for (std::size_t row = 0; row < rowsV; ++row) {
            std::memcpy(
                dstV + row * cWidth,
                srcV + row * src.u4_v_strd,
                copyV);
        }
        return true;
    }

    void release() {
        valid_ = false;
        codec_ = nullptr;
        outputBuffers_ = {};
        for (auto& storage : outputStorage_) storage.clear();
        for (void* allocation : allocations_) alignedRelease(allocation);
        allocations_.clear();
        memoryRecords_.clear();
        pending_.clear();
    }

    bool valid_ = false;
    iv_obj_t* codec_ = nullptr;
    std::vector<iv_mem_rec_t> memoryRecords_;
    std::vector<void*> allocations_;
    ivd_out_bufdesc_t outputBuffers_{};
    std::array<std::vector<std::uint8_t>, 3> outputStorage_;
    std::vector<std::uint8_t> pending_;
    std::uint64_t outputFrames_ = 0;
};

class OpenH264Decoder final : public VideoDecoder {
public:
    OpenH264Decoder() {
        if (WelsCreateDecoder(&decoder_) != 0 || !decoder_) return;
        if (!initializeDecoder()) {
            WelsDestroyDecoder(decoder_);
            decoder_ = nullptr;
        }
    }
    ~OpenH264Decoder() override {
        if (decoder_) {
            decoder_->Uninitialize();
            WelsDestroyDecoder(decoder_);
        }
    }
    bool valid() const noexcept { return decoder_ != nullptr; }

    bool decode(const std::uint8_t* data, std::size_t size,
                std::uint64_t pts90k, bool hasPts,
                std::vector<RawVideoFrame>& output,
                std::string& error) override {
        error.clear();
        if (!decoder_) { error = "OpenH264 decoder unavailable"; return false; }
        if (!data || !size) return true;

        // V10.1: NativeTsDemux already emits one live H.264 PES sample for
        // approximately every source picture (the LVM source is 25 fps and
        // the sample cadence is ~40 ms).  The V9.5/V9.6 stream reassembler
        // was incorrectly splitting those PES samples into roughly two
        // synthetic pictures, destroying the B-picture reference chain.
        // Feed the byte-exact PES elementary payload directly to OpenH264.
        ++decodeCalls_;
        ++pictureCalls_;

        std::vector<std::uint8_t> picture(data, data + size);
        const bool ok = decodePicture(
            std::move(picture), pts90k, hasPts, output, error);

        if ((decodeCalls_ % 100U) == 0U) {
            std::cerr << "NATIVE AVC PES DIAG decodeCalls=" << decodeCalls_
                      << " samples=" << pictureCalls_
                      << " sps=" << spsSeen_ << " pps=" << ppsSeen_
                      << " idr=" << idrSeen_
                      << " synced=" << (synchronized_ ? 1 : 0)
                      << " refLost=" << refLostSeen_
                      << " noParam=" << noParamSeen_
                      << " bitErr=" << bitstreamErrorSeen_
                      << " out=" << outputFrames_ << '\n';
        }
        return ok;
    }

    void reset() override {
        sps_.clear();
        pps_.clear();
        clearPending();
        synchronized_ = false;
        decodeCalls_ = pictureCalls_ = spsSeen_ = ppsSeen_ = idrSeen_ = 0;
        refLostSeen_ = noParamSeen_ = bitstreamErrorSeen_ = outputFrames_ = 0;
        if (decoder_) {
            decoder_->Uninitialize();
            initializeDecoder();
        }
    }

private:
    struct PendingStamp {
        std::size_t end = 0;
        std::uint64_t pts90k = 0;
        bool hasPts = false;
    };

    void appendPending(const std::uint8_t* data, std::size_t size,
                       std::uint64_t pts90k, bool hasPts) {
        pending_.insert(pending_.end(), data, data + size);
        stamps_.push_back({pending_.size(), pts90k, hasPts});
    }

    void clearPending() {
        pending_.clear();
        stamps_.clear();
    }

    void consumePending(std::size_t count) {
        if (count == 0) return;
        if (count >= pending_.size()) {
            clearPending();
            return;
        }
        pending_.erase(
            pending_.begin(),
            pending_.begin() + static_cast<std::ptrdiff_t>(count));
        std::deque<PendingStamp> next;
        for (auto stamp : stamps_) {
            if (stamp.end <= count) continue;
            stamp.end -= count;
            next.push_back(stamp);
        }
        stamps_.swap(next);
    }

    std::uint64_t pendingPts() const noexcept {
        for (const auto& stamp : stamps_) if (stamp.hasPts) return stamp.pts90k;
        return stamps_.empty() ? 0 : stamps_.front().pts90k;
    }

    bool pendingHasPts() const noexcept {
        for (const auto& stamp : stamps_) if (stamp.hasPts) return true;
        return false;
    }

    bool decodePicture(std::vector<std::uint8_t>&& picture,
                       std::uint64_t pts90k, bool hasPts,
                       std::vector<RawVideoFrame>& output,
                       std::string& error) {
        const auto h264 = inspectH264AnnexB(picture.data(), picture.size());
        if (h264.hasSps && !h264.sps.empty()) { sps_ = h264.sps; ++spsSeen_; }
        if (h264.hasPps && !h264.pps.empty()) { pps_ = h264.pps; ++ppsSeen_; }
        if (h264.hasIdr) ++idrSeen_;

        if (!synchronized_) {
            if (!h264.hasIdr || sps_.empty() || pps_.empty()) return true;
            if (!reinitializeDecoderState()) {
                error = "OpenH264 decoder reinitialize failed at IDR picture";
                return false;
            }
            std::vector<std::uint8_t> primed;
            if (!h264.hasSps) primed.insert(primed.end(), sps_.begin(), sps_.end());
            if (!h264.hasPps) primed.insert(primed.end(), pps_.begin(), pps_.end());
            primed.insert(primed.end(), picture.begin(), picture.end());
            synchronized_ = true;
            return decodePictureBytes(
                primed.data(), primed.size(), pts90k, hasPts, output, error);
        }

        return decodePictureBytes(
            picture.data(), picture.size(), pts90k, hasPts, output, error);
    }

    bool decodePictureBytes(const std::uint8_t* data, std::size_t size,
                            std::uint64_t pts90k, bool hasPts,
                            std::vector<RawVideoFrame>& output,
                            std::string& error) {
        unsigned char* planes[3]{};
        SBufferInfo info{};
        info.uiInBsTimeStamp = hasPts ? pts90k : 0;
        const DECODING_STATE rc = decoder_->DecodeFrameNoDelay(
            data, static_cast<int>(size), planes, &info);
        const int state = static_cast<int>(rc);
        const int fatalMask = static_cast<int>(dsInvalidArgument) |
                              static_cast<int>(dsInitialOptExpected) |
                              static_cast<int>(dsOutOfMemory) |
                              static_cast<int>(dsDstBufNeedExpan);
        if ((state & fatalMask) != 0) {
            error = "OpenH264 DecodeFrameNoDelay failed: " + std::to_string(state);
            return false;
        }
        if ((state & static_cast<int>(dsRefLost)) != 0) ++refLostSeen_;
        if ((state & static_cast<int>(dsNoParamSets)) != 0) ++noParamSeen_;
        if ((state & static_cast<int>(dsBitstreamError)) != 0) ++bitstreamErrorSeen_;
        if ((state & (static_cast<int>(dsNoParamSets) |
                      static_cast<int>(dsDepLayerLost))) != 0) {
            synchronized_ = false;
        }

        if (info.iBufferStatus != 1) return true;
        const int w = info.UsrData.sSystemBuffer.iWidth;
        const int h = info.UsrData.sSystemBuffer.iHeight;
        if (w <= 0 || h <= 0 || !planes[0] || !planes[1] || !planes[2]) {
            error = "OpenH264 returned invalid I420";
            return false;
        }

        RawVideoFrame frame;
        frame.width = w;
        frame.height = h;
        frame.pts90k = info.uiOutYuvTimeStamp ? info.uiOutYuvTimeStamp : pts90k;
        frame.dts90k = frame.pts90k;
        frame.hasPts = hasPts || info.uiOutYuvTimeStamp != 0;
        frame.hasDts = frame.hasPts;
        frame.i420.resize(static_cast<std::size_t>(w) * h * 3U / 2U);
        auto* y = frame.i420.data();
        auto* u = y + static_cast<std::size_t>(w) * h;
        auto* v = u + static_cast<std::size_t>(w / 2) * (h / 2);
        for (int row = 0; row < h; ++row) {
            std::memcpy(y + static_cast<std::size_t>(row) * w,
                        planes[0] + static_cast<std::size_t>(row) *
                            info.UsrData.sSystemBuffer.iStride[0], w);
        }
        for (int row = 0; row < h / 2; ++row) {
            std::memcpy(u + static_cast<std::size_t>(row) * (w / 2),
                        planes[1] + static_cast<std::size_t>(row) *
                            info.UsrData.sSystemBuffer.iStride[1], w / 2);
            std::memcpy(v + static_cast<std::size_t>(row) * (w / 2),
                        planes[2] + static_cast<std::size_t>(row) *
                            info.UsrData.sSystemBuffer.iStride[1], w / 2);
        }
        output.push_back(std::move(frame));
        ++outputFrames_;
        if (outputFrames_ == 1U || (outputFrames_ % 100U) == 0U) {
            std::cerr << "NATIVE AVC OUTPUT frames=" << outputFrames_
                      << " size=" << w << "x" << h
                      << " decodeCalls=" << decodeCalls_
                      << " pictures=" << pictureCalls_ << '\n';
        }
        return true;
    }

    bool reinitializeDecoderState() {
        if (!decoder_) return false;
        decoder_->Uninitialize();
        return initializeDecoder();
    }

    bool initializeDecoder() {
        if (!decoder_) return false;
        // Match OpenH264's console decoder setup as closely as possible for
        // Main-profile broadcast AVC with B pictures.  Keep decode single-
        // threaded; OpenH264 has historically had B-picture/thread ordering
        // fixes and 720x576 does not need threaded slice reconstruction.
        int threads = 0;
        decoder_->SetOption(DECODER_OPTION_NUM_OF_THREADS, &threads);
        SDecodingParam params{};
        params.eEcActiveIdc = ERROR_CON_SLICE_MV_COPY_CROSS_IDR_FREEZE_RES_CHANGE;
        params.uiTargetDqLayer = static_cast<std::uint8_t>(-1);
        params.bParseOnly = false;
        params.sVideoProperty.size = sizeof(params.sVideoProperty);
        params.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
        return decoder_->Initialize(&params) == 0;
    }

    ISVCDecoder* decoder_ = nullptr;
    std::vector<std::uint8_t> sps_, pps_, pending_;
    std::deque<PendingStamp> stamps_;
    bool synchronized_ = false;
    std::uint64_t decodeCalls_ = 0, pictureCalls_ = 0;
    std::uint64_t spsSeen_ = 0, ppsSeen_ = 0, idrSeen_ = 0;
    std::uint64_t refLostSeen_ = 0, noParamSeen_ = 0;
    std::uint64_t bitstreamErrorSeen_ = 0, outputFrames_ = 0;
};

class OpenH264Encoder final : public VideoEncoder {
public:
    OpenH264Encoder(){ WelsCreateSVCEncoder(&encoder_); }
    ~OpenH264Encoder() override { close(); if(encoder_){WelsDestroySVCEncoder(encoder_);encoder_=nullptr;} }
    bool valid() const noexcept { return encoder_ != nullptr; }
    bool configure(int w,int h,double fps,std::uint64_t bitrate,std::string&error) override {
        error.clear(); if(!encoder_){error="OpenH264 encoder unavailable";return false;} close(); haveTimestampBase_=false; timestampBase90k_=0;
        SEncParamBase p{};p.iUsageType=CAMERA_VIDEO_REAL_TIME;p.iPicWidth=w;p.iPicHeight=h;p.iTargetBitrate=static_cast<int>(std::min<std::uint64_t>(bitrate,INT_MAX));p.iRCMode=RC_BITRATE_MODE;p.fMaxFrameRate=static_cast<float>(fps>0?fps:25.0);
        if(encoder_->Initialize(&p)!=cmResultSuccess){error="OpenH264 Initialize failed";return false;} int fmt=videoFormatI420;encoder_->SetOption(ENCODER_OPTION_DATAFORMAT,&fmt);w_=w;h_=h;fps_=fps>0?fps:25.0;gopFrames_=std::max<std::uint64_t>(1,static_cast<std::uint64_t>(std::llround(fps_*2.0)));frameIndex_=0;configured_=true;return true;
    }
    bool encode(const RawVideoFrame&in,std::vector<EncodedVideoFrame>&out,std::string&error) override {
        error.clear();if(!configured_){error="OpenH264 encoder not configured";return false;}if(in.width!=w_||in.height!=h_){error="OpenH264 geometry mismatch";return false;}
        SSourcePicture p{};p.iColorFormat=videoFormatI420;p.iPicWidth=w_;p.iPicHeight=h_;p.iStride[0]=w_;p.iStride[1]=p.iStride[2]=w_/2;p.pData[0]=const_cast<unsigned char*>(in.i420.data());p.pData[1]=p.pData[0]+static_cast<std::size_t>(w_)*h_;p.pData[2]=p.pData[1]+static_cast<std::size_t>(w_/2)*(h_/2);const std::uint64_t inputPts90k = in.hasPts ? in.pts90k : nextPts_;
        if (!haveTimestampBase_) { timestampBase90k_ = inputPts90k; haveTimestampBase_ = true; }
        const std::uint64_t relativePts90k = inputPts90k >= timestampBase90k_ ? inputPts90k - timestampBase90k_ : 0;
        p.uiTimeStamp=static_cast<long long>(relativePts90k/90ULL);
        // Live outputs can be joined long after the encoder's first IDR.
        // Force a fresh random-access point about every two seconds so a new
        // SRT/RTSP/HTTP client does not have to wait for a stream restart.
        if(frameIndex_==0 || (gopFrames_>0 && (frameIndex_%gopFrames_)==0)){
            const int frc=encoder_->ForceIntraFrame(true);
            if(frc!=cmResultSuccess){error="OpenH264 ForceIntraFrame failed: "+std::to_string(frc);return false;}
        }
        SFrameBSInfo bi{};const int rc=encoder_->EncodeFrame(&p,&bi);if(rc!=cmResultSuccess){error="OpenH264 EncodeFrame failed: "+std::to_string(rc);return false;}++frameIndex_;if(bi.eFrameType==videoFrameTypeSkip)return true;
        EncodedVideoFrame f;f.pts90k=in.hasPts?in.pts90k:nextPts_;f.dts90k=f.pts90k;f.hasPts=f.hasDts=true;f.keyFrame=bi.eFrameType==videoFrameTypeIDR||bi.eFrameType==videoFrameTypeI;
        for(int l=0;l<bi.iLayerNum;++l){auto&li=bi.sLayerInfo[l];std::size_t off=0;for(int n=0;n<li.iNalCount;++n){const int z=li.pNalLengthInByte[n];if(z<=0)continue;const auto*nal=li.pBsBuf+off;appendNormalizedAnnexBNal(f.data,nal,static_cast<std::size_t>(z));off+=static_cast<std::size_t>(z);}}
        if(!f.data.empty()){const auto info=inspectH264AnnexB(f.data.data(),f.data.size());if(info.hasSps&&!info.sps.empty())sps_=info.sps;if(info.hasPps&&!info.pps.empty())pps_=info.pps;if((f.keyFrame||info.hasIdr)&&!sps_.empty()&&!pps_.empty()&&(!info.hasSps||!info.hasPps)){std::vector<std::uint8_t>primed;if(!info.hasSps)primed.insert(primed.end(),sps_.begin(),sps_.end());if(!info.hasPps)primed.insert(primed.end(),pps_.begin(),pps_.end());primed.insert(primed.end(),f.data.begin(),f.data.end());f.data.swap(primed);}out.push_back(std::move(f));}
        nextPts_=(in.hasPts?in.pts90k:nextPts_)+static_cast<std::uint64_t>(90000.0/fps_);return true;
    }
    bool flush(std::vector<EncodedVideoFrame>&,std::string&error) override {error.clear();return true;}
private:
    void close(){if(configured_&&encoder_){encoder_->Uninitialize();configured_=false;}sps_.clear();pps_.clear();}
    ISVCEncoder*encoder_=nullptr;int w_=0,h_=0;double fps_=25.0;bool configured_=false;std::uint64_t nextPts_=0;std::uint64_t frameIndex_=0,gopFrames_=50;bool haveTimestampBase_=false;std::uint64_t timestampBase90k_=0;std::vector<std::uint8_t>sps_,pps_;
};

class De265Decoder final : public VideoDecoder {
public:
    De265Decoder(){ctx_=de265_new_decoder();}
    ~De265Decoder() override {if(ctx_)de265_free_decoder(ctx_);}
    bool valid()const noexcept{return ctx_!=nullptr;}
    bool decode(const std::uint8_t*d,std::size_t n,std::uint64_t pts,bool hasPts,std::vector<RawVideoFrame>&out,std::string&error)override{
        error.clear();if(!ctx_){error="libde265 decoder unavailable";return false;}if(!d||!n)return true;if(n>static_cast<std::size_t>(INT_MAX)){error="HEVC AU too large";return false;}
        const auto ni=inspectH265AnnexB(d,n);
        if(ni.hasVps&&!ni.vps.empty())vps_=ni.vps;if(ni.hasSps&&!ni.sps.empty())sps_=ni.sps;if(ni.hasPps&&!ni.pps.empty())pps_=ni.pps;
        const std::uint8_t*feed=d;std::size_t feedSize=n;std::vector<std::uint8_t>primed;
        // Join live HEVC only at IRAP.  Starting on inter pictures leaves the
        // decoder without its DPB references just like AVC B/P late-join.
        if(!synchronized_){
            if(!ni.hasIrap)return true;
            de265_reset(ctx_);
            if(!ni.hasVps&&!vps_.empty())primed.insert(primed.end(),vps_.begin(),vps_.end());
            if(!ni.hasSps&&!sps_.empty())primed.insert(primed.end(),sps_.begin(),sps_.end());
            if(!ni.hasPps&&!pps_.empty())primed.insert(primed.end(),pps_.begin(),pps_.end());
            if(!primed.empty()){primed.insert(primed.end(),d,d+n);feed=primed.data();feedSize=primed.size();}
        }
        auto e=de265_push_data(ctx_,feed,static_cast<int>(feedSize),static_cast<de265_PTS>(hasPts?pts:0),nullptr);if(e!=DE265_OK){error="libde265 push failed: "+std::to_string(e);synchronized_=false;return false;}de265_push_end_of_frame(ctx_);
        for(int guard=0;guard<128;++guard){int more=0;e=de265_decode(ctx_,&more);if(!drain(out,pts,hasPts,error))return false;if(!more)break;if(e!=DE265_OK&&e!=DE265_ERROR_IMAGE_BUFFER_FULL&&e!=DE265_ERROR_WAITING_FOR_INPUT_DATA){error="libde265 decode failed: "+std::to_string(e);synchronized_=false;return false;}}
        if(ni.hasIrap)synchronized_=true;
        return drain(out,pts,hasPts,error);
    }
    void reset()override{vps_.clear();sps_.clear();pps_.clear();synchronized_=false;if(ctx_)de265_reset(ctx_);}
private:
    bool drain(std::vector<RawVideoFrame>&out,std::uint64_t fallback,bool hasPts,std::string&error){while(const de265_image*img=de265_get_next_picture(ctx_)){const int w=de265_get_image_width(img,0),h=de265_get_image_height(img,0);if(de265_get_bits_per_pixel(img,0)!=8||de265_get_chroma_format(img)!=de265_chroma_420){de265_release_next_picture(ctx_);error="libde265 output is not 8-bit 4:2:0";return false;}int sy=0,su=0,sv=0;auto*y=de265_get_image_plane(img,0,&sy);auto*u=de265_get_image_plane(img,1,&su);auto*v=de265_get_image_plane(img,2,&sv);if(w<=0||h<=0||!y||!u||!v){de265_release_next_picture(ctx_);error="libde265 invalid frame";return false;}RawVideoFrame f;f.width=w;f.height=h;const auto p=de265_get_image_PTS(img);f.pts90k=p>=0?static_cast<std::uint64_t>(p):fallback;f.dts90k=f.pts90k;f.hasPts=f.hasDts=hasPts;f.i420.resize(static_cast<std::size_t>(w)*h*3U/2U);auto*dy=f.i420.data();auto*du=dy+static_cast<std::size_t>(w)*h;auto*dv=du+static_cast<std::size_t>(w/2)*(h/2);for(int r=0;r<h;++r)std::memcpy(dy+static_cast<std::size_t>(r)*w,y+static_cast<std::size_t>(r)*sy,w);for(int r=0;r<h/2;++r){std::memcpy(du+static_cast<std::size_t>(r)*(w/2),u+static_cast<std::size_t>(r)*su,w/2);std::memcpy(dv+static_cast<std::size_t>(r)*(w/2),v+static_cast<std::size_t>(r)*sv,w/2);}out.push_back(std::move(f));de265_release_next_picture(ctx_);}return true;}
    de265_decoder_context*ctx_=nullptr;
    std::vector<std::uint8_t>vps_,sps_,pps_;
    bool synchronized_=false;
};

class KvazaarEncoder final : public VideoEncoder {
public:
    KvazaarEncoder(){api_=kvz_api_get(8);}
    ~KvazaarEncoder() override {close();}
    bool valid()const noexcept{return api_!=nullptr;}
    bool configure(int w,int h,double fps,std::uint64_t bitrate,std::string&error)override{
        error.clear();
        close();
        if(!api_){error="Kvazaar API unavailable";return false;}
        if(w<=0||h<=0){error="Kvazaar invalid output geometry";return false;}
        // Kvazaar 2.3.x requires both coded dimensions to be multiples of 8.
        // Fail with an actionable message instead of the opaque encoder_open
        // failure that was previously shown in the UI.
        if((w&7)||(h&7)){
            error="Kvazaar requires width and height divisible by 8 (got "+
                  std::to_string(w)+"x"+std::to_string(h)+")";
            return false;
        }
        cfg_=api_->config_alloc();
        if(!cfg_||!api_->config_init(cfg_)){error="Kvazaar config init failed";close();return false;}
        cfg_->width=w;
        cfg_->height=h;
        cfg_->framerate_num=static_cast<int32_t>(std::max(1.0,std::round(fps>0?fps:25.0)));
        cfg_->framerate_denom=1;
        cfg_->target_bitrate=static_cast<int32_t>(std::min<std::uint64_t>(bitrate,INT_MAX));
        if (cfg_->target_bitrate > 0) cfg_->rc_algorithm = KVZ_LAMBDA;
        cfg_->input_format=KVZ_FORMAT_P420;
        cfg_->input_bitdepth=8;

        // V10.6: use a realtime-oriented Kvazaar profile for live IPTV
        // transcoding.  Leaving Kvazaar at its defaults caused the 2.3.2
        // build to select threads=auto (12 on the test host) and owf=auto,
        // driving CPU usage close to saturation for a single SD H.265 stream.
        // Apply the preset first because a preset may modify several encoder
        // options; explicit thread/OWF limits must therefore come afterwards.
        auto parseKvazaar = [&](const char* name, const char* value) -> bool {
            if (!api_->config_parse || !api_->config_parse(cfg_, name, value)) {
                error = std::string("Kvazaar config option failed: ") + name + "=" + value;
                return false;
            }
            return true;
        };
        const std::uint64_t pixels = static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h);
        int realtimeThreads = 4;
        int realtimeOwf = 1;
        if (pixels > 1280ULL * 720ULL) { realtimeThreads = 6; realtimeOwf = 2; }
        if (pixels > 1920ULL * 1080ULL) { realtimeThreads = 8; realtimeOwf = 2; }
        if (pixels > 2560ULL * 1440ULL) { realtimeThreads = 12; realtimeOwf = 3; }
        const std::string threadsValue = std::to_string(realtimeThreads);
        const std::string owfValue = std::to_string(realtimeOwf);
        if (!parseKvazaar("preset", "ultrafast") ||
            !parseKvazaar("threads", threadsValue.c_str()) ||
            !parseKvazaar("owf", owfValue.c_str())) {
            close();
            return false;
        }
        std::cerr << "NATIVE H265 REALTIME PROFILE preset=ultrafast threads=" << realtimeThreads
                  << " owf=" << realtimeOwf
                  << " size=" << w << "x" << h
                  << " fps=" << cfg_->framerate_num
                  << " bitrate_kbps=" << (static_cast<unsigned long long>(bitrate) / 1000ULL)
                  << " adaptive=1"
                  << "\n";

        // Keep Kvazaar's native GOP/intra defaults here.  V8.3 tried to
        // override period/vps-period through config_parse(), which proved
        // unstable with the project-local 2.3.2 build during live encoding.
        // Kvazaar's default intra period already provides periodic random
        // access; our wrapper caches VPS/SPS/PPS and prepends them to every
        // IRAP access unit when the encoder omits them, so late join remains
        // self-contained without mutating interdependent GOP settings.

        // Keep the Kvazaar configuration intentionally minimal.  This mirrors
        // the library's supported embedding pattern: initialize defaults, then
        // set coded size, frame rate and target bitrate.  The previous code
        // overrode several interdependent GOP/OWF fields and could leave the
        // config in a state rejected by encoder_open().
        enc_=api_->encoder_open(cfg_);
        if(!enc_){
            error="Kvazaar encoder_open failed for "+std::to_string(w)+"x"+
                  std::to_string(h)+" @ "+std::to_string(cfg_->framerate_num)+
                  " fps, bitrate="+std::to_string(bitrate);
            close();
            return false;
        }
        w_=w;h_=h;fps_=fps>0?fps:25.0;
        return true;
    }
    bool encode(const RawVideoFrame&in,std::vector<EncodedVideoFrame>&out,std::string&error)override{return encodeOne(&in,out,error);}
    bool flush(std::vector<EncodedVideoFrame>&out,std::string&error)override{for(int i=0;i<64;++i){std::size_t before=out.size();if(!encodeOne(nullptr,out,error))return false;if(out.size()==before)break;}return true;}
private:
    bool encodeOne(const RawVideoFrame*in,std::vector<EncodedVideoFrame>&out,std::string&error){error.clear();if(!enc_){error="Kvazaar encoder not configured";return false;}kvz_picture*pic=nullptr;if(in){if(in->width!=w_||in->height!=h_){error="Kvazaar geometry mismatch";return false;}pic=api_->picture_alloc_csp(KVZ_CSP_420,w_,h_);if(!pic){error="Kvazaar picture alloc failed";return false;}for(int r=0;r<h_;++r)std::memcpy(pic->y+static_cast<std::size_t>(r)*pic->stride,in->i420.data()+static_cast<std::size_t>(r)*w_,w_);const auto*su=in->i420.data()+static_cast<std::size_t>(w_)*h_;const auto*sv=su+static_cast<std::size_t>(w_/2)*(h_/2);for(int r=0;r<h_/2;++r){std::memcpy(pic->u+static_cast<std::size_t>(r)*(pic->stride/2),su+static_cast<std::size_t>(r)*(w_/2),w_/2);std::memcpy(pic->v+static_cast<std::size_t>(r)*(pic->stride/2),sv+static_cast<std::size_t>(r)*(w_/2),w_/2);}pic->pts=static_cast<int64_t>(in->hasPts?in->pts90k:nextPts_);pts_.push_back(static_cast<std::uint64_t>(pic->pts));}
        kvz_data_chunk*chunks=nullptr;uint32_t len=0;kvz_picture*rec=nullptr,*src=nullptr;kvz_frame_info fi{};
        const int ok=api_->encoder_encode(enc_,pic,&chunks,&len,&rec,&src,&fi);
        if(pic)api_->picture_free(pic);
        if(!ok){
            error="Kvazaar encoder_encode failed at "+std::to_string(w_)+"x"+std::to_string(h_)+", pts="+
                  std::to_string(in?static_cast<unsigned long long>(in->hasPts?in->pts90k:nextPts_):0ULL);
            return false;
        }
        if(chunks&&len){
            EncodedVideoFrame f;
            // Kvazaar's linked data chunks are byte fragments of one complete
            // Annex-B access unit.  They are NOT guaranteed to begin on NAL
            // boundaries.  Concatenate them verbatim; adding a start code to
            // each chunk corrupts HEVC when a NAL spans two chunks.
            std::size_t written=0;
            f.data.reserve(len);
            for(auto*c=chunks;c;c=c->next){
                if(!c->data||c->len==0)continue;
                if(written+static_cast<std::size_t>(c->len)>static_cast<std::size_t>(len)){
                    error="Kvazaar chunk chain exceeds advertised output length";
                    if(chunks)api_->chunk_free(chunks);
                    if(rec)api_->picture_free(rec);
                    if(src)api_->picture_free(src);
                    return false;
                }
                f.data.insert(f.data.end(),c->data,c->data+c->len);
                written+=static_cast<std::size_t>(c->len);
            }
            if(written!=static_cast<std::size_t>(len)){
                error="Kvazaar chunk chain length mismatch";
                if(chunks)api_->chunk_free(chunks);
                if(rec)api_->picture_free(rec);
                if(src)api_->picture_free(src);
                return false;
            }
            // Kvazaar normally emits Annex-B.  Only add one leading start code
            // if the complete access unit itself lacks one.
            if(!f.data.empty()&&!startsWithAnnexB(f.data.data(),f.data.size())){
                f.data.insert(f.data.begin(),{0,0,0,1});
            }
            const auto info=inspectH265AnnexB(f.data.data(),f.data.size());
            if(info.hasVps&&!info.vps.empty())vps_=info.vps;
            if(info.hasSps&&!info.sps.empty())sps_=info.sps;
            if(info.hasPps&&!info.pps.empty())pps_=info.pps;
            f.keyFrame=info.hasIrap||(fi.nal_unit_type>=KVZ_NAL_BLA_W_LP&&fi.nal_unit_type<=KVZ_NAL_CRA_NUT);
            if(f.keyFrame&&!vps_.empty()&&!sps_.empty()&&!pps_.empty()&&(!info.hasVps||!info.hasSps||!info.hasPps)){
                std::vector<std::uint8_t>primed;
                if(!info.hasVps)primed.insert(primed.end(),vps_.begin(),vps_.end());
                if(!info.hasSps)primed.insert(primed.end(),sps_.begin(),sps_.end());
                if(!info.hasPps)primed.insert(primed.end(),pps_.begin(),pps_.end());
                primed.insert(primed.end(),f.data.begin(),f.data.end());
                f.data.swap(primed);
            }
            // Use Kvazaar's reconstructed/output picture timestamps.  These
            // carry the encoder's actual display/decode ordering when B frames
            // or lookahead are enabled.
            if(rec){
                if(rec->pts>=0){f.pts90k=static_cast<std::uint64_t>(rec->pts);f.hasPts=true;}
                if(rec->dts>=0){f.dts90k=static_cast<std::uint64_t>(rec->dts);f.hasDts=true;}
            }
            if(!f.hasPts&&!pts_.empty()){f.pts90k=pts_.front();f.hasPts=true;}
            if(!f.hasDts&&f.hasPts){f.dts90k=f.pts90k;f.hasDts=true;}
            if(!pts_.empty())pts_.pop_front();
            if(!f.data.empty())out.push_back(std::move(f));
        }
        if(chunks)api_->chunk_free(chunks);
        if(rec)api_->picture_free(rec);
        if(src)api_->picture_free(src);
        if(in)nextPts_=(in->hasPts?in->pts90k:nextPts_)+static_cast<std::uint64_t>(90000.0/fps_);
        return true;}
    void close(){if(enc_){api_->encoder_close(enc_);enc_=nullptr;}if(cfg_){api_->config_destroy(cfg_);cfg_=nullptr;}pts_.clear();vps_.clear();sps_.clear();pps_.clear();}
    const kvz_api*api_=nullptr;kvz_config*cfg_=nullptr;kvz_encoder*enc_=nullptr;int w_=0,h_=0;double fps_=25.0;std::vector<std::uint8_t>vps_,sps_,pps_;std::uint64_t nextPts_=0;std::deque<std::uint64_t>pts_;
};

class FdkAacDecoder final : public AudioDecoder {
public:
    explicit FdkAacDecoder(TRANSPORT_TYPE transport,
                           TRANSPORT_TYPE alternateTransport = TT_UNKNOWN)
        : transport_(transport), alternateTransport_(alternateTransport) {
        openDecoder();
    }
    ~FdkAacDecoder() override { closeDecoder(); }

    bool valid() const noexcept { return h_ != nullptr; }

    bool decode(const std::uint8_t* d, std::size_t n, std::uint64_t pts,
                bool hasPts, std::vector<PcmAudioFrame>& out,
                std::string& error) override {
        error.clear();
        if (!h_) {
            error = "FDK-AAC decoder unavailable";
            return false;
        }
        if (d && n) {
            // Keep bytes that FDK could not accept in a previous call.  AAC PES
            // packets may contain several access units and can be larger than
            // the decoder's internal transport buffer.
            constexpr std::size_t kMaxPendingBytes = 2U * 1024U * 1024U;
            if (pending_.size() + n > kMaxPendingBytes) {
                // Treat a pathological/live discontinuity as a resync event,
                // not as a service-fatal error.
                resetDecoder();
                pending_.clear();
                haveNextPts_ = false;
            }
            pending_.insert(pending_.end(), d, d + n);
            if (hasPts && !haveNextPts_) {
                nextPts90k_ = pts;
                haveNextPts_ = true;
            }
        }

        for (int guard = 0; guard < 128; ++guard) {
            bool supplied = false;
            if (!pending_.empty()) {
                const UINT offered = static_cast<UINT>(
                    std::min<std::size_t>(pending_.size(), UINT_MAX));
                UCHAR* ptr = reinterpret_cast<UCHAR*>(pending_.data());
                UINT bufferSize = offered;
                UINT bytesValid = offered;
                const AAC_DECODER_ERROR fillRc =
                    aacDecoder_Fill(h_, &ptr, &bufferSize, &bytesValid);
                if (fillRc != AAC_DEC_OK) {
                    error = "FDK-AAC Fill failed: " +
                            std::to_string(static_cast<int>(fillRc)) +
                            " (" + transportName() + ")";
                    return false;
                }
                const std::size_t consumed =
                    static_cast<std::size_t>(offered - bytesValid);
                if (consumed > 0) {
                    pending_.erase(
                        pending_.begin(),
                        pending_.begin() + static_cast<std::ptrdiff_t>(consumed));
                    supplied = true;
                }
            }

            std::array<INT_PCM, 8 * 2048> pcm{};
            const AAC_DECODER_ERROR rc = aacDecoder_DecodeFrame(
                h_, pcm.data(), static_cast<INT>(pcm.size()), 0);

            if (rc == AAC_DEC_NOT_ENOUGH_BITS) {
                // FDK has consumed all complete frames currently available.
                // If it could not accept any additional bytes there is nothing
                // more useful to do until the next PES arrives.
                if (!supplied || pending_.empty()) break;
                continue;
            }

            if (rc == AAC_DEC_TRANSPORT_SYNC_ERROR) {
                // Joining a live ADTS/LOAS stream mid-frame or losing TS/PES
                // bytes is recoverable. FDK explicitly documents this as a
                // feed-more-data condition, so do not take the service OFFLINE.
                if (++consecutiveTransportErrors_ >= 32) {
                    restartTransportDecoder();
                    consecutiveTransportErrors_ = 0;
                    haveNextPts_ = false;
                }
                if (pending_.empty()) break;
                continue;
            }

            if (rc == AAC_DEC_UNKNOWN) {
                // AAC_DEC_UNKNOWN (5) is reported by FDK for an error coming
                // from another module. It must not kill a live service on the
                // first damaged access unit. Reopen the transport decoder after
                // a short run of such errors and wait for the next sync frame.
                if (++consecutiveTransportErrors_ >= 8) {
                    restartTransportDecoder();
                    consecutiveTransportErrors_ = 0;
                    pending_.clear();
                    haveNextPts_ = false;
                }
                break;
            }

            if (rc != AAC_DEC_OK && !IS_DECODE_ERROR(rc)) {
                error = "FDK-AAC DecodeFrame failed: " +
                        std::to_string(static_cast<int>(rc)) +
                        " (" + transportName() + ")";
                return false;
            }

            CStreamInfo* si = aacDecoder_GetStreamInfo(h_);
            if (!si || si->sampleRate <= 0 || si->numChannels <= 0 ||
                si->frameSize <= 0) {
                if (rc != AAC_DEC_OK) break;
                error = std::string("FDK-AAC invalid stream info (") + transportName() + ")";
                return false;
            }

            const std::size_t count =
                static_cast<std::size_t>(si->frameSize) * si->numChannels;
            if (count > pcm.size()) {
                error = "FDK-AAC PCM output exceeds decoder buffer";
                return false;
            }

            PcmAudioFrame f;
            f.sampleRate = si->sampleRate;
            f.channels = si->numChannels;
            f.pts90k = haveNextPts_ ? nextPts90k_ : pts;
            f.hasPts = haveNextPts_ || hasPts;
            const bool frameHasPts = f.hasPts;
            f.samples.assign(
                pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(count));
            out.push_back(std::move(f));

            if (frameHasPts) {
                nextPts90k_ = (haveNextPts_ ? nextPts90k_ : pts) +
                    static_cast<std::uint64_t>(si->frameSize) * 90000ULL /
                    static_cast<unsigned>(si->sampleRate);
                haveNextPts_ = true;
            }
            consecutiveTransportErrors_ = 0;
        }
        return true;
    }

    void reset() override {
        pending_.clear();
        haveNextPts_ = false;
        consecutiveTransportErrors_ = 0;
        resetDecoder();
    }

private:
    const char* transportName() const noexcept {
        switch (transport_) {
            case TT_MP4_ADTS: return "ADTS";
            case TT_MP4_LOAS: return "LOAS/LATM";
            case TT_MP4_LATM_MCP1: return "LATM MCP1";
            case TT_MP4_LATM_MCP0: return "LATM MCP0";
            default: return "AAC transport";
        }
    }

    void openDecoder() {
        h_ = aacDecoder_Open(transport_, 1);
    }

    void closeDecoder() {
        if (h_) aacDecoder_Close(h_);
        h_ = nullptr;
    }

    void resetDecoder() {
        closeDecoder();
        openDecoder();
    }

    void restartTransportDecoder() {
        // MPEG-TS stream_type 0x11 is seen in the wild both as LOAS-framed
        // LATM and as packetized LATM with in-band StreamMuxConfig.  Start
        // with LOAS, but if the transport decoder repeatedly cannot lock,
        // retry with LATM MCP1 instead of killing the whole service.
        if (alternateTransport_ != TT_UNKNOWN)
            std::swap(transport_, alternateTransport_);
        resetDecoder();
    }

    TRANSPORT_TYPE transport_ = TT_MP4_ADTS;
    TRANSPORT_TYPE alternateTransport_ = TT_UNKNOWN;
    HANDLE_AACDECODER h_ = nullptr;
    std::vector<std::uint8_t> pending_;
    std::uint64_t nextPts90k_ = 0;
    bool haveNextPts_ = false;
    unsigned consecutiveTransportErrors_ = 0;
};

class FdkAacEncoder final : public AudioEncoder {
public:
    ~FdkAacEncoder()override{close();}
    bool configure(int rate,int channels,std::uint64_t bitrate,std::string&error)override{
        error.clear();close();if(channels<1||channels>2){error="FDK-AAC Stage 9 supports mono/stereo";return false;}if(aacEncOpen(&h_,0,static_cast<UINT>(channels))!=AACENC_OK){error="FDK-AAC open failed";return false;}CHANNEL_MODE mode=channels==1?MODE_1:MODE_2;if(aacEncoder_SetParam(h_,AACENC_AOT,AOT_AAC_LC)!=AACENC_OK||aacEncoder_SetParam(h_,AACENC_SAMPLERATE,rate)!=AACENC_OK||aacEncoder_SetParam(h_,AACENC_CHANNELMODE,mode)!=AACENC_OK||aacEncoder_SetParam(h_,AACENC_BITRATE,static_cast<UINT>(bitrate))!=AACENC_OK||aacEncoder_SetParam(h_,AACENC_TRANSMUX,TT_MP4_ADTS)!=AACENC_OK||aacEncoder_SetParam(h_,AACENC_AFTERBURNER,1)!=AACENC_OK||aacEncEncode(h_,nullptr,nullptr,nullptr,nullptr)!=AACENC_OK){error="FDK-AAC configuration failed";close();return false;}AACENC_InfoStruct info{};if(aacEncInfo(h_,&info)!=AACENC_OK||info.frameLength<=0){error="FDK-AAC info failed";close();return false;}rate_=rate;channels_=channels;frameLength_=info.frameLength;return true;
    }
    bool encode(const PcmAudioFrame&in,std::vector<EncodedAudioFrame>&out,std::string&error)override{if(!h_){error="FDK-AAC encoder not configured";return false;}if(in.sampleRate!=rate_||in.channels!=channels_){error="FDK-AAC PCM format mismatch";return false;}pending_.insert(pending_.end(),in.samples.begin(),in.samples.end());if(!havePts_&&in.hasPts){nextPts_=in.pts90k;havePts_=true;}const std::size_t need=static_cast<std::size_t>(frameLength_)*channels_;while(pending_.size()>=need){if(!encodeFrame(pending_.data(),static_cast<int>(need),out,error))return false;pending_.erase(pending_.begin(),pending_.begin()+static_cast<std::ptrdiff_t>(need));}return true;}
    bool flush(std::vector<EncodedAudioFrame>&out,std::string&error)override{if(!h_)return true;const std::size_t need=static_cast<std::size_t>(frameLength_)*channels_;if(!pending_.empty()){pending_.resize(need,0);if(!encodeFrame(pending_.data(),static_cast<int>(need),out,error))return false;pending_.clear();}return true;}
private:
    bool encodeFrame(const std::int16_t*pcm,int count,std::vector<EncodedAudioFrame>&out,std::string&error){void*inPtr=const_cast<std::int16_t*>(pcm);INT inId=IN_AUDIO_DATA,inSize=count*static_cast<INT>(sizeof(INT_PCM)),inEl=sizeof(INT_PCM);AACENC_BufDesc ib{1,&inPtr,&inId,&inSize,&inEl};std::array<UCHAR,16384>bytes{};void*outPtr=bytes.data();INT outId=OUT_BITSTREAM_DATA,outSize=bytes.size(),outEl=1;AACENC_BufDesc ob{1,&outPtr,&outId,&outSize,&outEl};AACENC_InArgs ia{};ia.numInSamples=count;AACENC_OutArgs oa{};AACENC_ERROR rc=aacEncEncode(h_,&ib,&ob,&ia,&oa);if(rc!=AACENC_OK){error="FDK-AAC encode failed: "+std::to_string(rc);return false;}if(oa.numOutBytes>0){EncodedAudioFrame f;f.data.assign(bytes.begin(),bytes.begin()+oa.numOutBytes);f.pts90k=nextPts_;f.hasPts=havePts_;out.push_back(std::move(f));nextPts_+=static_cast<std::uint64_t>(frameLength_)*90000ULL/static_cast<unsigned>(rate_);}return true;}
    void close(){if(h_)aacEncClose(&h_);h_=nullptr;pending_.clear();}
    HANDLE_AACENCODER h_=nullptr;int rate_=0,channels_=0,frameLength_=1024;std::vector<std::int16_t>pending_;std::uint64_t nextPts_=0;bool havePts_=false;
};

class PlMpegMp2Decoder final : public AudioDecoder {
public:
    PlMpegMp2Decoder(){buffer_=plm_buffer_create_with_capacity(128*1024);if(buffer_)audio_=plm_audio_create_with_buffer(buffer_,0);}
    ~PlMpegMp2Decoder()override{if(audio_)plm_audio_destroy(audio_);if(buffer_)plm_buffer_destroy(buffer_);}
    bool valid()const noexcept{return buffer_&&audio_;}
    bool decode(const std::uint8_t*d,std::size_t n,std::uint64_t pts,bool hasPts,std::vector<PcmAudioFrame>&out,std::string&error)override{error.clear();if(!valid()){error="PL_MPEG MP2 decoder unavailable";return false;}plm_buffer_write(buffer_,const_cast<std::uint8_t*>(d),n);std::uint64_t current=pts;for(int guard=0;guard<64;++guard){plm_samples_t*s=plm_audio_decode(audio_);if(!s)break;const int rate=plm_audio_get_samplerate(audio_);if(rate<=0){error="PL_MPEG invalid sample rate";return false;}PcmAudioFrame f;f.sampleRate=rate;f.channels=2;f.pts90k=current;f.hasPts=hasPts;f.samples.resize(static_cast<std::size_t>(s->count)*2);for(std::size_t i=0;i<static_cast<std::size_t>(s->count)*2;++i){float x=std::clamp(s->interleaved[i],-1.0f,1.0f);f.samples[i]=static_cast<std::int16_t>(std::lrint(x*32767.0f));}out.push_back(std::move(f));current+=static_cast<std::uint64_t>(s->count)*90000ULL/static_cast<unsigned>(rate);}return true;}
    void reset()override{if(audio_)plm_audio_rewind(audio_);}
private:plm_buffer_t*buffer_=nullptr;plm_audio_t*audio_=nullptr;
};

} // namespace

RuntimeCapabilities inspectRuntimeCapabilities(){RuntimeCapabilities c;c.h264Decoder=c.h264Encoder=c.hevcDecoder=c.hevcEncoder=c.aacDecoder=c.aacEncoder=c.mpegAudioDecoder=true;c.h264Library="built-in OpenH264 2.6.0 static";c.hevcDecoderLibrary="built-in libde265 1.1.3 static";c.hevcEncoderLibrary="built-in Kvazaar 2.3.2 static";c.aacDecoderLibrary=c.aacEncoderLibrary="built-in FDK-AAC 2.0.3 static";c.mpegAudioLibrary="built-in PL_MPEG static";const auto hw=inspectNativeHardwareCapabilities();c.vaapiRuntime=hw.vaapiRuntime;c.qsvEncoder=hw.qsvAvailable;c.qsvH264Encoder=hw.qsvH264;c.qsvHevcEncoder=hw.qsvHevc;c.nvencEncoder=hw.nvencAvailable;c.nvencH264Encoder=hw.nvencH264;c.nvencHevcEncoder=hw.nvencHevc;c.intelHardwareLibrary=hw.intelBackend;c.nvencHardwareLibrary=hw.nvencBackend;return c;}
std::unique_ptr<VideoDecoder> createVideoDecoder(mpegts::ElementaryCodec c,std::string&error){error.clear();if(c==mpegts::ElementaryCodec::H264){auto p=std::make_unique<OpenH264Decoder>();if(p->valid())return p;}else if(c==mpegts::ElementaryCodec::H265){auto p=std::make_unique<De265Decoder>();if(p->valid())return p;}else if(c==mpegts::ElementaryCodec::Mpeg2Video){auto p=std::make_unique<IttiamMpeg2Decoder>();if(p->valid())return p;}error="built-in decoder does not support/initialize requested codec";return{};}
static std::unique_ptr<VideoEncoder> createCpuVideoEncoder(mpegts::ElementaryCodec c,std::string&error){error.clear();if(c==mpegts::ElementaryCodec::H264){auto p=std::make_unique<OpenH264Encoder>();if(p->valid())return p;}else if(c==mpegts::ElementaryCodec::H265){auto p=std::make_unique<KvazaarEncoder>();if(p->valid())return p;}error="built-in CPU encoder does not support/initialize requested codec";return{};}
std::unique_ptr<VideoEncoder> createVideoEncoder(mpegts::ElementaryCodec c,std::string&error){return createVideoEncoder(c,"auto",error);}
std::unique_ptr<VideoEncoder> createVideoEncoder(mpegts::ElementaryCodec c,const std::string&backend,std::string&error){std::string b=backend;std::transform(b.begin(),b.end(),b.begin(),[](unsigned char x){return static_cast<char>(std::tolower(x));});if(b.empty())b="auto";if(b=="x264"||b=="x265"||b=="cpu")return createCpuVideoEncoder(c,error);if(b=="nvenc"||b=="intel"||b=="qsv"||b=="vaapi")return createNativeHardwareVideoEncoder(c,b,error);if(b=="auto"){const auto hw=inspectNativeHardwareCapabilities();std::string ignored;if(hw.nvencAvailable){auto p=createNativeHardwareVideoEncoder(c,"nvenc",ignored);if(p){std::cerr<<"NATIVE VIDEO ENCODER auto selected=nvenc\n";return p;}}if(hw.qsvAvailable){auto p=createNativeHardwareVideoEncoder(c,"qsv",ignored);if(p){std::cerr<<"NATIVE VIDEO ENCODER auto selected=qsv-vaapi\n";return p;}}auto p=createCpuVideoEncoder(c,error);if(p)std::cerr<<"NATIVE VIDEO ENCODER auto selected=cpu\n";return p;}error="unsupported native video encoder backend: "+backend;return{};}
std::unique_ptr<AudioDecoder> createAudioDecoder(mpegts::ElementaryCodec c,std::string&error){error.clear();if(c==mpegts::ElementaryCodec::AacAdts){auto p=std::make_unique<FdkAacDecoder>(TT_MP4_ADTS);if(p->valid())return p;}else if(c==mpegts::ElementaryCodec::AacLatm){auto p=std::make_unique<FdkAacDecoder>(TT_MP4_LOAS,TT_MP4_LATM_MCP1);if(p->valid())return p;}else if(c==mpegts::ElementaryCodec::MpegAudio){auto p=std::make_unique<PlMpegMp2Decoder>();if(p->valid())return p;}error="built-in audio decoder does not support/initialize requested codec";return{};}
std::unique_ptr<AudioEncoder> createAacEncoder(std::string&error){auto p=std::make_unique<FdkAacEncoder>();error.clear();return p;}

} // namespace dvbstreamer5::media::codec
