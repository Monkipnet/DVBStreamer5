#pragma once

#include "media/TransportStream.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace dvbstreamer5::media::mpegts {

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
    void tick(std::vector<Packet>& output);
    bool wantsInputPid(std::uint16_t pid) const noexcept;

private:
    struct PsiSectionState {
        std::vector<std::uint8_t> bytes;
        std::size_t expected = 0;
        std::uint8_t continuity = 0;
        bool continuityValid = false;
    };

    bool collectSections(
        const Packet& input,
        PsiSectionState& state,
        std::vector<std::vector<std::uint8_t>>& sections,
        std::string& error);
    bool processPatSection(const std::vector<std::uint8_t>& section, std::string& error);
    bool processCatSection(const std::vector<std::uint8_t>& section, std::string& error);
    bool processPmtSection(
        std::vector<std::uint8_t> section,
        std::vector<Packet>& output,
        std::string& error);
    bool processSdtSection(
        const std::vector<std::uint8_t>& section,
        std::vector<Packet>& output,
        std::string& error);
    void packetizeSection(
        std::uint16_t pid,
        const std::vector<std::uint8_t>& section,
        std::uint8_t& continuity,
        std::vector<Packet>& output);
    void emitPeriodicPsi(std::vector<Packet>& output);
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
    std::uint8_t patOutputContinuity_ = 0;
    std::uint8_t pmtOutputContinuity_ = 0;
    std::uint8_t sdtOutputContinuity_ = 0;
    PsiSectionState patSection_;
    PsiSectionState catSection_;
    PsiSectionState pmtSection_;
    PsiSectionState sdtSection_;
    std::vector<std::uint8_t> pmtOutputSection_;
    std::chrono::steady_clock::time_point nextPatPmtAt_ {};
    std::chrono::steady_clock::time_point nextSdtAt_ {};
    bool initialized_ = false;
    bool remapReady_ = false;
};

} // namespace dvbstreamer5::media::mpegts
