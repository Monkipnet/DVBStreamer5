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
    'inline constexpr const char* kProgramVersion = "10.8.42";',
    'inline constexpr const char* kProgramVersion = "10.8.43";',
    "version",
)

old_decoder = r'''        // V10.8.42: PES boundaries are transport boundaries, not AVC
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
new_decoder = r'''        // V10.8.43: the live demux sample cadence is one AVC picture per
        // source frame (~25 fps for the LVM service). V10.8.42 incorrectly
        // split a single already-framed sample into roughly four synthetic
        // pictures. Keep each demux sample byte-exact. Broadcast AVC Main/CABAC
        // is handled by the hardware zero-copy paths when available; this CPU
        // OpenH264 path remains the compatible fallback.
        ++decodeCalls_;
        ++pictureCalls_;

        std::vector<std::uint8_t> picture(data, data + size);
        const bool ok = decodePicture(
            std::move(picture), pts90k, hasPts, output, error);

        if ((decodeCalls_ % 100U) == 0U) {
            std::cerr << "NATIVE AVC SAMPLE DIAG decodeCalls=" << decodeCalls_
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
replace_once(
    "src/media/NativeCodecVendored.cpp",
    old_decoder,
    new_decoder,
    "OpenH264 V10.8.42 splitter",
)

replace_once(
    "src/StreamManager.cpp",
    '''    // Browser preview must not depend on the production codec. mpegts.js /\n    // MediaSource playback is reliable with AVC + AAC, while DVB services can\n    // legitimately carry MPEG-2, HEVC, MP2 or AC-3. Keep a dedicated,\n    // low-bitrate CPU pipeline whose output is used only by preview.ts.\n''',
    '''    // Browser preview must not depend on the production codec. mpegts.js /\n    // MediaSource playback is reliable with AVC + AAC, while DVB services can\n    // legitimately carry MPEG-2, HEVC, MP2 or AC-3. Keep a dedicated,\n    // low-bitrate hardware-auto pipeline whose output is used only by preview.ts.\n    // NVDEC/NVENC or QSV decode/VPP/encode is preferred for broadcast Main/HEVC;\n    // compatible streams can still fall back to the native CPU codecs.\n''',
    "preview comment",
)
replace_once(
    "src/StreamManager.cpp",
    '        previewTc.videoEncoder = "cpu";\n',
    '        previewTc.videoEncoder = "auto";\n',
    "preview encoder",
)
replace_once(
    "src/StreamManager.cpp",
    '                      << " codec=h264/aac size=1280x720 dar=16:9"\n                      << " video_kbps=1800 audio_kbps=128"\n',
    '                      << " codec=h264/aac size=1280x720 dar=16:9"\n                      << " encoder=auto video_kbps=1800 audio_kbps=128"\n',
    "preview ready log",
)

for path, struct_name in [
    ("src/media/NativeIntelZeroCopy.h", "IntelZeroCopyConfig"),
    ("src/media/NativeNvidiaZeroCopy.h", "NvidiaZeroCopyConfig"),
]:
    replace_once(
        path,
        '''    std::uint64_t bitrate = 6000000;\n    bool deinterlace = true;\n''',
        '''    std::uint64_t bitrate = 6000000;\n    bool deinterlace = true;\n    // Preserve an explicitly requested output raster even when it is larger\n    // than the decoded raster. Browser preview uses 1280x720 square pixels\n    // for anamorphic SD television sources.\n    bool lockOutputGeometry = false;\n''',
        struct_name,
    )

replace_once(
    "src/media/NativeTranscoderPipeline.cpp",
    '''                zeroCopyConfig.bitrate = config_.videoBitrate;\n                zeroCopyConfig.deinterlace = config_.deinterlace;\n                nvidiaZeroCopy_ = codec::createNvidiaZeroCopyTranscoder(\n''',
    '''                zeroCopyConfig.bitrate = config_.videoBitrate;\n                zeroCopyConfig.deinterlace = config_.deinterlace;\n                zeroCopyConfig.lockOutputGeometry = config_.lockOutputGeometry;\n                nvidiaZeroCopy_ = codec::createNvidiaZeroCopyTranscoder(\n''',
    "NVIDIA geometry flag",
)
replace_once(
    "src/media/NativeTranscoderPipeline.cpp",
    '''                zeroCopyConfig.bitrate = config_.videoBitrate;\n                zeroCopyConfig.deinterlace = config_.deinterlace;\n                intelZeroCopy_ = codec::createIntelZeroCopyTranscoder(\n''',
    '''                zeroCopyConfig.bitrate = config_.videoBitrate;\n                zeroCopyConfig.deinterlace = config_.deinterlace;\n                zeroCopyConfig.lockOutputGeometry = config_.lockOutputGeometry;\n                intelZeroCopy_ = codec::createIntelZeroCopyTranscoder(\n''',
    "Intel geometry flag",
)

replace_once(
    "src/media/NativeIntelZeroCopy.cpp",
    '''        // Do not upscale beyond source geometry. This mirrors the NVIDIA\n        // zero-copy path and avoids wasting GPU bandwidth on live television.\n        const std::uint64_t sourcePixels =\n            static_cast<std::uint64_t>(sourceWidth_) * sourceHeight_;\n        const std::uint64_t requestedPixels =\n            static_cast<std::uint64_t>(outputWidth_) * outputHeight_;\n        if (requestedPixels > sourcePixels) {\n            outputWidth_ = sourceWidth_ & ~1;\n            outputHeight_ = sourceHeight_ & ~1;\n        }\n''',
    '''        // Production transcodes avoid accidental upscaling. Browser preview\n        // explicitly locks 1280x720 so anamorphic SD input becomes square-pixel\n        // 16:9 in the GPU VPP path as well.\n        const std::uint64_t sourcePixels =\n            static_cast<std::uint64_t>(sourceWidth_) * sourceHeight_;\n        const std::uint64_t requestedPixels =\n            static_cast<std::uint64_t>(outputWidth_) * outputHeight_;\n        if (!config_.lockOutputGeometry && requestedPixels > sourcePixels) {\n            outputWidth_ = sourceWidth_ & ~1;\n            outputHeight_ = sourceHeight_ & ~1;\n        }\n''',
    "Intel anti-upscale",
)

replace_once(
    "src/media/NativeNvidiaZeroCopy.cpp",
    '''        const std::uint64_t requestedPixels =\n            static_cast<std::uint64_t>(outputWidth_) * outputHeight_;\n        if (requestedPixels > sourcePixels) {\n            outputWidth_ = sourceWidth_;\n            outputHeight_ = sourceHeight_;\n        }\n''',
    '''        const std::uint64_t requestedPixels =\n            static_cast<std::uint64_t>(outputWidth_) * outputHeight_;\n        if (!config_.lockOutputGeometry && requestedPixels > sourcePixels) {\n            outputWidth_ = sourceWidth_;\n            outputHeight_ = sourceHeight_;\n        }\n''',
    "NVIDIA anti-upscale",
)
