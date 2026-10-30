#pragma once

#include <cstdint>

// Pure, offline-testable decisions used by the optional Irdeto CW buffer.
// All TS/key mutations remain under ServiceBinding::mutex in the plugin.
namespace dvbstreamer5::ca::newcamd {

struct IrdetoCwBufferPolicy {
    enum class Decision : std::uint8_t {
        SkipZero,
        SkipUnchanged,
        Stage,
        Install
    };

    static constexpr bool isScrambledParity(std::uint8_t parity) noexcept {
        return parity == 2 || parity == 3;
    }

    static constexpr Decision decide(bool zeroCw, bool changedCw,
                                     bool slotValid, std::uint8_t observedTsc,
                                     std::uint8_t keyTsc) noexcept {
        if (zeroCw) return Decision::SkipZero;
        if (slotValid && !changedCw) return Decision::SkipUnchanged;
        return slotValid && isScrambledParity(observedTsc) &&
                       observedTsc == keyTsc
            ? Decision::Stage : Decision::Install;
    }

    static constexpr bool shouldActivate(bool pendingValid,
                                         std::uint8_t previousTsc,
                                         std::uint8_t incomingTsc) noexcept {
        return pendingValid && isScrambledParity(incomingTsc) &&
               previousTsc != incomingTsc;
    }
};

} // namespace dvbstreamer5::ca::newcamd
