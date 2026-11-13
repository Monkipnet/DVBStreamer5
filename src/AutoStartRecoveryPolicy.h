#pragma once

#include <chrono>
#include <string>

// Auto Start is a durable intent for an ONLINE tile, not an instruction to
// override an operator Stop, OnDemand idle, or an active backup transition.
namespace dvbstreamer5::stream_recovery {

inline bool eligible(bool autoStart, const std::string& activationMode) {
    return autoStart && activationMode == "online";
}

inline std::chrono::seconds retryDelay(unsigned attempt) {
    switch (attempt) {
        case 0:
        case 1: return std::chrono::seconds(10);
        case 2: return std::chrono::seconds(20);
        case 3: return std::chrono::seconds(40);
        case 4: return std::chrono::seconds(60);
        default: return std::chrono::seconds(120);
    }
}

} // namespace dvbstreamer5::stream_recovery
