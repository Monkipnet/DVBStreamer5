#pragma once
#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace dvbstreamer5::media::network {

// Absolute rational TS clock. Small wakeup delays do not shift every future
// deadline. Limit recovery spacing and abandon stale deadlines after a long
// scheduler stall; never replay a backlog as a sendmmsg burst.
class UdpSendClock {
public:
    explicit UdpSendClock(std::uint64_t bitrate) : bitrate_(bitrate) {
        if (!bitrate) throw std::invalid_argument("UDP clock bitrate is zero");
    }
    void start(std::uint64_t now) { nominal_=now; previous_=0; remainder_=0; }
    std::uint64_t deadline() const {
        const auto spacing=(1316ULL*8ULL*1000000000ULL/bitrate_)*9ULL/10ULL;
        return previous_ ? std::max<std::uint64_t>(nominal_,previous_+spacing) : nominal_;
    }
    bool advance(std::uint64_t sent) {
        constexpr std::uint64_t numerator=1316ULL*8ULL*1000000000ULL;
        auto period=numerator/bitrate_;
        remainder_+=numerator%bitrate_;
        period+=remainder_/bitrate_; remainder_%=bitrate_;
        nominal_+=period;
        const bool rebase=sent>nominal_ && sent-nominal_>period*4ULL;
        if (rebase) nominal_=sent+period;
        previous_=sent;
        return rebase;
    }
private:
    const std::uint64_t bitrate_;
    std::uint64_t nominal_=0, previous_=0, remainder_=0;
};

} // namespace dvbstreamer5::media::network
