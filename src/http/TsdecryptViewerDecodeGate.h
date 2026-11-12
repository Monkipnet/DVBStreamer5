#pragma once
// V509: UI-only TSDecrypt evidence. Real HLS media requests (not playlists)
// plus recent clear PES allow a stable green indicator. Independent codec
// frame verification is not implied, and no CA/transport bits are changed.
#include <chrono>
#include <cstdint>
#include <string>

namespace dvbstreamer5::http {
struct TsdecryptViewerDecodeGate {
    std::uint64_t generation = 0;
    bool seenPes = false;
    bool promoted = false;
    bool seenHealthyOutput = false;
    std::chrono::steady_clock::time_point lastPes{};
    std::chrono::steady_clock::time_point lastHealthyOutput{};
};

inline std::string updateTsdecryptViewerDecode(
    TsdecryptViewerDecodeGate& gate, std::uint64_t generation,
    const std::string& raw, bool active, bool outputHealthy,
    std::uint64_t clearPes, bool clientConnected,
    std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now()) {
    if (!active) {
        gate = {};
        return "offline";
    }
    if (generation == 0) {
        gate = {};
        return raw;
    }
    if (gate.generation != generation) {
        gate = {};
        gate.generation = generation;
    }
    constexpr auto pesGrace = std::chrono::seconds(15);
    constexpr auto outputGrace = std::chrono::seconds(6);
    if (outputHealthy) {
        gate.seenHealthyOutput = true;
        gate.lastHealthyOutput = now;
    }
    // Do not require the moment of an HLS segment request to coincide with
    // two consecutive /api/state snapshots. PES evidence can precede a
    // legitimate client connection by up to 15 seconds.
    if (outputHealthy && clearPes > 0 &&
        (raw == "unverified" || raw == "partial" ||
         raw == "viewer_pes_ok")) {
        gate.seenPes = true;
        gate.lastPes = now;
    }
    const bool recentPes = gate.seenPes &&
        now >= gate.lastPes && now - gate.lastPes <= pesGrace;
    const bool recentOutput = gate.seenHealthyOutput &&
        now >= gate.lastHealthyOutput &&
        now - gate.lastHealthyOutput <= outputGrace;
    if (clientConnected && recentPes && recentOutput)
        gate.promoted = true;
    // A prior successful client session is sufficient. Its disconnect does
    // not turn a still-healthy transport back into amber PES.
    if (gate.promoted && recentPes && recentOutput)
        return "viewer_pes_ok";
    gate.promoted = false;
    return raw;
}
} // namespace dvbstreamer5::http
