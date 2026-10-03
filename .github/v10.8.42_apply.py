from pathlib import Path


def replace_once(path, old, new, label):
    p = Path(path)
    s = p.read_text()
    count = s.count(old)
    if count != 1:
        raise SystemExit(f"{label}: matches={count}")
    p.write_text(s.replace(old, new, 1))


replace_once(
    "src/AppVersion.h",
    'inline constexpr const char* kProgramVersion = "10.8.41";',
    'inline constexpr const char* kProgramVersion = "10.8.42";',
    "version",
)

old = r'''        // V10.1: NativeTsDemux already emits one live H.264 PES sample for
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
'''

new = r'''        // V10.8.42: PES boundaries are transport boundaries, not AVC
        // access-unit boundaries. Some HLS MPEG-TS inputs split the final NAL
        // of a picture across PES packets. Feeding each PES directly made
        // OpenH264 see SPS/PPS/IDR but return dsNoParamSets+dsBitstreamError
        // forever. Keep the Annex-B stream byte-exact across PES calls and
        // release one picture only after the start of the following picture
        // is visible. That guarantees the last NAL of the current picture is
        // complete and preserves multi-slice/B-picture reference ordering.
        ++decodeCalls_;
        appendPending(data, size, pts90k, hasPts);

        constexpr std::size_t kMaxPendingBytes = 8U * 1024U * 1024U;

        // A live join may begin in the middle of a NAL. Discard only bytes
        // before the first Annex-B start code; never rewrite bytes between
        // NAL units.
        if (!pending_.empty()) {
            std::size_t startCodeLength = 0;
            const std::size_t first = findAnnexBStartCode(
                pending_.data(), pending_.size(), 0, startCodeLength);
            if (first < pending_.size() && first != 0) {
                consumePending(first);
            } else if (first == pending_.size()) {
                if (pending_.size() > kMaxPendingBytes) {
                    const std::size_t dropped = pending_.size();
                    clearPending();
                    synchronized_ = false;
                    std::cerr << "NATIVE AVC AU RESYNC reason=no_start_code bytes="
                              << dropped << '\n';
                }
                return true;
            }
        }

        if (pending_.size() > kMaxPendingBytes) {
            const std::size_t dropped = pending_.size();
            clearPending();
            synchronized_ = false;
            std::cerr << "NATIVE AVC AU RESYNC reason=pending_overflow bytes="
                      << dropped << '\n';
            return true;
        }

        bool ok = true;
        for (unsigned guard = 0; guard < 256U; ++guard) {
            const std::size_t pictureLength = openH264ReadPictureLength(
                pending_.data(), pending_.size());
            if (pictureLength == 0) break;

            const std::uint64_t picturePts90k = pendingPts();
            const bool pictureHasPts = pendingHasPts();
            std::vector<std::uint8_t> picture(
                pending_.begin(),
                pending_.begin() + static_cast<std::ptrdiff_t>(pictureLength));
            consumePending(pictureLength);
            ++pictureCalls_;

            if (!decodePicture(std::move(picture), picturePts90k,
                               pictureHasPts, output, error)) {
                ok = false;
                break;
            }
        }

        if ((decodeCalls_ % 100U) == 0U) {
            std::cerr << "NATIVE AVC AU DIAG decodeCalls=" << decodeCalls_
                      << " pictures=" << pictureCalls_
                      << " pending=" << pending_.size()
                      << " sps=" << spsSeen_ << " pps=" << ppsSeen_
                      << " idr=" << idrSeen_
                      << " synced=" << (synchronized_ ? 1 : 0)
                      << " refLost=" << refLostSeen_
                      << " noParam=" << noParamSeen_
                      << " bitErr=" << bitstreamErrorSeen_
                      << " out=" << outputFrames_ << '\n';
        }
        return ok;
'''

replace_once(
    "src/media/NativeCodecVendored.cpp",
    old,
    new,
    "OpenH264 direct-PES decode block",
)
