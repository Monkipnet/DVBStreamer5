#pragma once

#include "media/TransportStream.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace tvs::media::mpegts {

struct RemapConfig {
    std::uint16_t inputServiceId = 0;
    std::uint16_t outputServiceId = 0;
    std::uint16_t outputVideoPid = 0;
    std::uint16_t outputAudioPid = 0;
    std::string serviceName;
    std::string serviceProvider;
};

class Remapper {
public:
    bool initialize(const RemapConfig& config, std::string& error);
    bool process(const Packet& input, std::vector<Packet>& output, std::string& error);

private:
    bool processPat(const Packet& input, std::string& error);
    bool processCat(const Packet& input, std::string& error);
    bool processPmt(const Packet& input, std::vector<Packet>& output, std::string& error);
    bool processSdt(const Packet& input, Packet& output, std::string& error);
    bool isAllowed(std::uint16_t pid) const noexcept;
    Packet makePat(std::uint8_t continuity) const;
    Packet makeSdt(std::uint8_t continuity) const;

    RemapConfig config_;
    std::array<bool, 8192> allowedPids_ {};
    std::array<bool, 8192> caPids_ {};
    std::uint16_t inputServiceId_ = 0;
    std::uint16_t pmtPid_ = 0x1fff;
    std::uint16_t inputVideoPid_ = 0;
    std::uint16_t inputAudioPid_ = 0;
    std::uint16_t transportStreamId_ = 1;
    std::uint16_t originalNetworkId_ = 1;
    std::uint8_t patContinuity_ = 0;
    bool initialized_ = false;
    bool remapReady_ = false;
};

} // namespace tvs::media::mpegts
