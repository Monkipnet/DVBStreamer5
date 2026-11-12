#pragma once
// V506: UI-only evidence gate for TSDecrypt MPEG-TS, not a codec/CW verifier.
// A real output client plus sustained clear PES can promote the dashboard to
// green. Subsequent healthy PES preserves it even after the client disconnects.
// Reset on stream restart, output starvation, or sustained scrambled-only data.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>

namespace dvbstreamer5::http {
struct TsdecryptViewerDecodeGate {
    std::uint64_t generation = 0;
    bool hasSample = false;
    bool hasPes = false;
    bool promoted = false;
    unsigned goodSamples = 0;
    std::chrono::steady_clock::time_point lastSample{};
    std::chrono::steady_clock::time_point lastPes{};
};

inline std::string updateTsdecryptViewerDecode(
    TsdecryptViewerDecodeGate& gate, std::uint64_t generation,
    const std::string& raw, bool active, bool outputHealthy,
    std::uint64_t clearPes, bool clientConnected,
    std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now()) {
    if (!active || generation == 0 || gate.generation != generation) {
        gate = {};
        gate.generation = generation;
        if (!active) return "offline";
    }
    const bool goodPes = outputHealthy && clearPes > 0 &&
        (raw == "unverified" || raw == "partial");
    if (goodPes) {
        if (!gate.hasPes || now - gate.lastPes > std::chrono::seconds(4))
            gate.goodSamples = 0;
        // The /api/state endpoint may be polled repeatedly within one
        // second; never treat duplicate rapid polls as multiple good samples.
        if (!gate.hasSample ||
            now - gate.lastSample >= std::chrono::milliseconds(650)) {
            gate.goodSamples = std::min(gate.goodSamples + 1u, 8u);
            gate.lastSample = now;
            gate.hasSample = true;
        }
        gate.lastPes = now;
        gate.hasPes = true;
        if (clientConnected && gate.goodSamples >= 2) gate.promoted = true;
    }
    // Do not turn a stopped/erroring source green because the client remains
    // connected. Short missing-PES windows must not undo a healthy state.
    if (gate.promoted && gate.hasPes && outputHealthy &&
        now - gate.lastPes <= std::chrono::seconds(6))
        return "viewer_pes_ok";
    if (!outputHealthy || (gate.hasPes &&
                          now - gate.lastPes > std::chrono::seconds(6))) {
        gate.promoted = false;
        gate.goodSamples = 0;
    }
    return raw;
}
} // namespace dvbstreamer5::http
