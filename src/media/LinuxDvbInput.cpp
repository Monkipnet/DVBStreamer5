#include "media/LinuxDvbInput.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>

#ifdef __linux__
#include <fcntl.h>
#include <linux/dvb/dmx.h>
#include <linux/dvb/frontend.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>
#endif

namespace tvs::media::network {
namespace {
#ifdef __linux__
constexpr std::uint16_t kDmxAllPids = 0x2000;
constexpr std::size_t kMaxPidFilters = 32;
#endif

#ifdef __linux__
std::string devicePath(const LinuxDvbTuneConfig& config, const char* device) {
    return "/dev/dvb/adapter" + std::to_string(config.adapter) + "/" + device +
        std::to_string(std::string(device) == "frontend" ? config.frontend : 0);
}

std::string deviceError(const std::string& operation, const std::string& path) {
    return operation + " (" + path + "): " + std::strerror(errno);
}

fe_code_rate_t parseFec(const std::string& fec) {
    if (fec == "1/2") return FEC_1_2;
    if (fec == "2/3") return FEC_2_3;
    if (fec == "3/4") return FEC_3_4;
    if (fec == "4/5") return FEC_4_5;
    if (fec == "5/6") return FEC_5_6;
    if (fec == "6/7") return FEC_6_7;
    if (fec == "7/8") return FEC_7_8;
    if (fec == "8/9") return FEC_8_9;
    if (fec == "3/5") return FEC_3_5;
    if (fec == "2/5") return FEC_2_5;
    if (fec == "9/10") return FEC_9_10;
    return FEC_AUTO;
}

fe_modulation_t parseModulation(const LinuxDvbTuneConfig& config) {
    if (config.modulation == "qpsk") return QPSK;
    if (config.modulation == "8psk") return PSK_8;
    if (config.modulation == "16apsk") return APSK_16;
    if (config.modulation == "32apsk") return APSK_32;
    return config.deliverySystem == "dvb-s" ? QPSK : QAM_AUTO;
}

bool setFrontendProperties(
    int fd, const std::vector<dtv_property>& values, std::string& error) {
    dtv_properties properties {};
    properties.num = static_cast<std::uint32_t>(values.size());
    properties.props = const_cast<dtv_property*>(values.data());
    if (ioctl(fd, FE_SET_PROPERTY, &properties) == 0) return true;
    error = "FE_SET_PROPERTY failed: " + std::string(std::strerror(errno));
    return false;
}

bool tuneFrontend(const LinuxDvbTuneConfig& config, int fd, std::string& error) {
    if (config.adapter < 0 || config.adapter > 31 ||
        config.frontend < 0 || config.frontend > 31 ||
        config.frequencyKHz < 900000 || config.frequencyKHz > 14000000 ||
        config.symbolRateK < 100 || config.symbolRateK > 60000 ||
        (config.polarity != "H" && config.polarity != "V") ||
        (config.deliverySystem != "dvb-s" && config.deliverySystem != "dvb-s2") ||
        config.diseqcSource < -1 || config.diseqcSource > 7 ||
        config.streamId < -1 || config.streamId > 255) {
        error = "invalid DVB-S/S2 tuning parameters";
        return false;
    }

    const bool highBand = config.frequencyKHz >= config.lnbSlofKHz;
    const std::uint32_t lof = highBand ? config.lnbLof2KHz : config.lnbLof1KHz;
    const std::uint32_t intermediateFrequency =
        config.frequencyKHz > lof
        ? config.frequencyKHz - lof
        : lof - config.frequencyKHz;
    if (intermediateFrequency == 0 ||
        intermediateFrequency > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        error = "calculated satellite intermediate frequency is invalid";
        return false;
    }

    if (ioctl(fd, FE_SET_TONE, SEC_TONE_OFF) != 0 ||
        ioctl(fd, FE_SET_VOLTAGE,
              config.polarity == "H" ? SEC_VOLTAGE_18 : SEC_VOLTAGE_13) != 0) {
        error = "satellite LNB control failed: " + std::string(std::strerror(errno));
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    if (config.diseqcSource >= 0) {
        dvb_diseqc_master_cmd command {};
        command.msg[0] = 0xe0;
        command.msg[1] = 0x10;
        command.msg[2] = 0x38;
        command.msg[3] = static_cast<std::uint8_t>(
            0xf0 | ((config.diseqcSource & 3) << 2) |
            (config.polarity == "H" ? 0x02 : 0x00) |
            (highBand ? 0x01 : 0x00));
        command.msg_len = 4;
        if (ioctl(fd, FE_DISEQC_SEND_MASTER_CMD, &command) != 0) {
            error = "DiSEqC switch command failed: " + std::string(std::strerror(errno));
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    if (ioctl(fd, FE_SET_TONE, highBand ? SEC_TONE_ON : SEC_TONE_OFF) != 0) {
        error = "satellite LNB tone selection failed: " + std::string(std::strerror(errno));
        return false;
    }

    std::vector<dtv_property> properties(10);
    properties[0].cmd = DTV_CLEAR;
    properties[1].cmd = DTV_DELIVERY_SYSTEM;
    properties[1].u.data = config.deliverySystem == "dvb-s" ? SYS_DVBS : SYS_DVBS2;
    properties[2].cmd = DTV_FREQUENCY;
    properties[2].u.data = intermediateFrequency;
    properties[3].cmd = DTV_SYMBOL_RATE;
    properties[3].u.data = config.symbolRateK * 1000U;
    properties[4].cmd = DTV_MODULATION;
    properties[4].u.data = parseModulation(config);
    properties[5].cmd = DTV_INNER_FEC;
    properties[5].u.data = parseFec(config.fec);
    properties[6].cmd = DTV_INVERSION;
    properties[6].u.data = INVERSION_AUTO;
    properties[7].cmd = DTV_PILOT;
    properties[7].u.data = PILOT_AUTO;
    properties[8].cmd = DTV_ROLLOFF;
    properties[8].u.data = ROLLOFF_AUTO;
    std::size_t count = 9;
    if (config.streamId >= 0) {
        properties[count].cmd = DTV_STREAM_ID;
        properties[count].u.data = static_cast<std::uint32_t>(config.streamId);
        ++count;
    }
    properties.resize(count + 1);
    properties[count].cmd = DTV_TUNE;
    if (!setFrontendProperties(fd, properties, error)) return false;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
        fe_status_t status {};
        if (ioctl(fd, FE_READ_STATUS, &status) == 0 && (status & FE_HAS_LOCK)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    error = "DVB frontend did not lock within 8 seconds";
    return false;
}
#endif

} // namespace

LinuxDvbInput::~LinuxDvbInput() {
    close();
}

bool LinuxDvbInput::parsePidList(
    const std::string& text,
    std::vector<std::uint16_t>& pids,
    std::string& error) {
    pids.clear();
    error.clear();
    if (text.empty() || text == "8192") return true;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find(':', start);
        const std::string token = text.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (token.empty()) {
            error = "DVB PID list contains an empty value";
            pids.clear();
            return false;
        }
        try {
            std::size_t used = 0;
            const unsigned long value = std::stoul(token, &used);
            if (used != token.size() || value >= 8192) {
                error = "DVB PID must be in range 0..8191";
                pids.clear();
                return false;
            }
            const auto pid = static_cast<std::uint16_t>(value);
            if (std::find(pids.begin(), pids.end(), pid) == pids.end()) {
                pids.push_back(pid);
            }
        } catch (const std::exception&) {
            error = "DVB PID list contains an invalid value";
            pids.clear();
            return false;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

bool LinuxDvbInput::open(const LinuxDvbTuneConfig& config, std::string& error) {
    close();
#ifdef __linux__
    std::vector<std::uint16_t> pids;
    if (!parsePidList(config.pids, pids, error)) return false;
    if (pids.size() > kMaxPidFilters) {
        error = "native DVB input supports at most 32 explicit PID filters";
        return false;
    }
    const std::string frontend = devicePath(config, "frontend");
    const int frontendFd = ::open(frontend.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (frontendFd < 0) {
        error = deviceError("cannot open DVB frontend", frontend);
        return false;
    }
    const bool tuned = tuneFrontend(config, frontendFd, error);
    ::close(frontendFd);
    if (!tuned) return false;

    const std::string demux = devicePath(config, "demux");
    const std::vector<std::uint16_t> filters =
        pids.empty() ? std::vector<std::uint16_t>{kDmxAllPids} : pids;
    for (const std::uint16_t pid : filters) {
        const int fd = ::open(demux.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            error = deviceError("cannot open DVB demux", demux);
            close();
            return false;
        }
        dmx_pes_filter_params filter {};
        filter.pid = pid;
        filter.input = DMX_IN_FRONTEND;
        filter.output = DMX_OUT_TS_TAP;
        filter.pes_type = DMX_PES_OTHER;
        filter.flags = DMX_IMMEDIATE_START;
        if (ioctl(fd, DMX_SET_PES_FILTER, &filter) != 0) {
            error = deviceError("cannot start DVB transport-stream filter", demux);
            ::close(fd);
            close();
            return false;
        }
        demuxFds_.push_back(fd);
    }

    const std::string dvr = devicePath(config, "dvr");
    dvrFd_ = ::open(dvr.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (dvrFd_ < 0) {
        error = deviceError("cannot open DVB transport-stream tap", dvr);
        close();
        return false;
    }
    error.clear();
    return true;
#else
    (void)config;
    error = "native DVB input is supported only on Linux";
    return false;
#endif
}

bool LinuxDvbInput::read(
    std::uint8_t* buffer,
    std::size_t capacity,
    std::size_t& received,
    int timeoutMs,
    std::string& error) {
    received = 0;
#ifdef __linux__
    if (dvrFd_ < 0 || !buffer || capacity == 0 || timeoutMs < 0) {
        error = "invalid native DVB read request";
        return false;
    }
    pollfd descriptor {};
    descriptor.fd = dvrFd_;
    descriptor.events = POLLIN;
    int result;
    do {
        result = ::poll(&descriptor, 1, timeoutMs);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        error = "DVB transport-stream poll failed: " + std::string(std::strerror(errno));
        return false;
    }
    if (result == 0) {
        error.clear();
        return true;
    }
    const ssize_t bytes = ::read(dvrFd_, buffer, capacity);
    if (bytes < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            error.clear();
            return true;
        }
        error = "DVB transport-stream read failed: " + std::string(std::strerror(errno));
        return false;
    }
    if (bytes == 0) {
        error = "DVB transport-stream tap reached end of stream";
        return false;
    }
    received = static_cast<std::size_t>(bytes);
    error.clear();
    return true;
#else
    (void)buffer;
    (void)capacity;
    (void)timeoutMs;
    error = "native DVB input is supported only on Linux";
    return false;
#endif
}

void LinuxDvbInput::close() noexcept {
#ifdef __linux__
    if (dvrFd_ >= 0) {
        ::close(dvrFd_);
        dvrFd_ = -1;
    }
    for (const int fd : demuxFds_) {
        ::ioctl(fd, DMX_STOP);
        ::close(fd);
    }
#endif
    demuxFds_.clear();
}

} // namespace tvs::media::network
