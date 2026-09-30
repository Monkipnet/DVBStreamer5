#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dvbstreamer5::media::network {

struct LinuxDvbTuneConfig {
    int adapter = 0;
    int frontend = 0;
    std::uint32_t frequencyKHz = 11727000;
    std::uint32_t symbolRateK = 27500;
    std::string polarity = "H";
    std::string deliverySystem = "dvb-s2";
    std::string modulation = "auto";
    std::string fec = "auto";
    int diseqcSource = -1;
    std::uint32_t lnbLof1KHz = 9750000;
    std::uint32_t lnbLof2KHz = 10600000;
    std::uint32_t lnbSlofKHz = 11700000;
    int streamId = -1;
    std::string pids = "8192";
    int lockTimeoutMs = 8000;
};

class LinuxDvbInput {
public:
    LinuxDvbInput() = default;
    ~LinuxDvbInput();

    LinuxDvbInput(const LinuxDvbInput&) = delete;
    LinuxDvbInput& operator=(const LinuxDvbInput&) = delete;

    bool open(const LinuxDvbTuneConfig& config, std::string& error);
    bool read(
        std::uint8_t* buffer,
        std::size_t capacity,
        std::size_t& received,
        int timeoutMs,
        std::string& error);
    void close() noexcept;

    static bool parsePidList(
        const std::string& text,
        std::vector<std::uint16_t>& pids,
        std::string& error);

private:
    int dvrFd_ = -1;
    std::vector<int> demuxFds_;
};

} // namespace dvbstreamer5::media::network
