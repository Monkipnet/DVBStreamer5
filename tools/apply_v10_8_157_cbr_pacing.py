from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match, got {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


replace_once(
    "src/AppVersion.h",
    'inline constexpr const char* kProgramVersion = "10.8.156";',
    'inline constexpr const char* kProgramVersion = "10.8.157";',
)

replace_once(
    "src/media/CbrTsPacer.cpp",
    '''    // Keep the pacing clock continuous across ordinary scheduler jitter. The
    // caller drains overdue datagrams; only a genuinely long stall re-anchors
    // the clock so resume cannot produce a huge catch-up burst.
    constexpr auto kMaximumCatchupWindow = std::chrono::milliseconds(250);
    if (now - nextDeadline_ > kMaximumCatchupWindow) {
        nextDeadline_ = now;
        pacingRemainder_ = 0;
    }
''',
    '''    // V10.8.157: never catch up more than one datagram period. The old
    // fixed 250 ms window allowed several overdue 1316-byte UDP datagrams to be
    // emitted back-to-back after ordinary scheduler jitter. Strict receivers
    // such as WISI then measured short 10-20+ Mbit/s bursts followed by gaps
    // even though the long-term average matched the configured CBR.
    //
    // Preserve sub-period timing error so the monotonic clock does not drift,
    // but once we are at least one whole datagram late, re-anchor to now. After
    // advanceDeadline() the next datagram is therefore in the future and the
    // caller cannot produce an immediate catch-up burst.
    const std::uint64_t periodNs =
        (kDatagramBits * kNanosecondsPerSecond) / targetBitrate_;
    const auto maximumCatchup = std::chrono::nanoseconds(
        (std::max<std::uint64_t>)(periodNs, 1ULL));
    if (now - nextDeadline_ >= maximumCatchup) {
        nextDeadline_ = now;
        pacingRemainder_ = 0;
    }
''',
)
