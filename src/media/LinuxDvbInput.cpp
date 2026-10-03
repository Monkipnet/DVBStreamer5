#include "media/LinuxDvbInput.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <utility>
#include <sstream>

#ifdef __linux__
#include <fcntl.h>
#include <linux/dvb/dmx.h>
#include <linux/dvb/frontend.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>
#endif

namespace dvbstreamer5::media::network {
namespace {
#ifdef __linux__
constexpr std::uint16_t kDmxAllPids = 0x2000;
constexpr std::size_t kMaxPidFilters = 32;
// The kernel DVB demux default can be too small for full-transponder reads,
// especially when CA requires PID 8192.  Keep several seconds of headroom.
constexpr unsigned long kDmxBufferBytes = 8UL * 1024UL * 1024UL;

using FrontendRegistryKey = std::pair<int, int>;

std::mutex& activeFrontendRegistryMutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<FrontendRegistryKey, int>& activeFrontendRegistry() {
    static std::map<FrontendRegistryKey, int> registry;
    return registry;
}

bool registerActiveFrontend(int adapter, int frontend, int fd) {
    std::lock_guard<std::mutex> lock(activeFrontendRegistryMutex());
    const FrontendRegistryKey key{adapter, frontend};
    const auto existing = activeFrontendRegistry().find(key);
    if (existing != activeFrontendRegistry().end() && existing->second != fd)
        return false;
    activeFrontendRegistry()[key] = fd;
    return true;
}

void releaseActiveFrontend(int adapter, int frontend, int fd) noexcept {
    std::lock_guard<std::mutex> lock(activeFrontendRegistryMutex());
    const FrontendRegistryKey key{adapter, frontend};
    const auto existing = activeFrontendRegistry().find(key);
    if (existing != activeFrontendRegistry().end() && existing->second == fd)
        activeFrontendRegistry().erase(existing);
    if (fd >= 0) ::close(fd);
}

int clampFrontendPercent(double value) {
    return std::clamp(static_cast<int>(value >= 0.0 ? value + 0.5 : value - 0.5), 0, 100);
}

bool readFrontendPropertyStat(int fd, std::uint32_t command, int& percent,
                              double& db, bool& hasDb) {
    dtv_property property{};
    property.cmd = command;
    dtv_properties properties{};
    properties.num = 1;
    properties.props = &property;
    if (::ioctl(fd, FE_GET_PROPERTY, &properties) != 0 || property.u.st.len < 1)
        return false;
    const auto& stat = property.u.st.stat[0];
    if (stat.scale == FE_SCALE_RELATIVE) {
        percent = clampFrontendPercent(static_cast<double>(stat.uvalue) * 100.0 / 65535.0);
        return true;
    }
    if (stat.scale == FE_SCALE_DECIBEL) {
        db = static_cast<double>(stat.svalue) / 1000.0;
        hasDb = true;
        return true;
    }
    return false;
}

void readFrontendStatsFromFd(int fd, LinuxDvbFrontendStats& result) {
    result = LinuxDvbFrontendStats{};
    result.available = fd >= 0;
    if (fd < 0) return;
    fe_status_t status{};
    if (::ioctl(fd, FE_READ_STATUS, &status) == 0)
        result.locked = (status & FE_HAS_LOCK) != 0;
    int signalRelative = -1;
    int qualityRelative = -1;
    readFrontendPropertyStat(fd, DTV_STAT_SIGNAL_STRENGTH, signalRelative, result.signalDb, result.hasSignalDb);
    readFrontendPropertyStat(fd, DTV_STAT_CNR, qualityRelative, result.cnrDb, result.hasCnrDb);
    if (signalRelative < 0) {
        std::uint16_t legacy = 0;
        if (::ioctl(fd, FE_READ_SIGNAL_STRENGTH, &legacy) == 0)
            signalRelative = clampFrontendPercent(static_cast<double>(legacy) * 100.0 / 65535.0);
    }
    if (qualityRelative < 0) {
        std::uint16_t legacy = 0;
        if (::ioctl(fd, FE_READ_SNR, &legacy) == 0)
            qualityRelative = clampFrontendPercent(static_cast<double>(legacy) * 100.0 / 65535.0);
    }
    if (signalRelative >= 0) result.signalPercent = signalRelative;
    else if (result.hasSignalDb) result.signalPercent = clampFrontendPercent((result.signalDb + 100.0) * 1.5);
    if (qualityRelative >= 0) result.qualityPercent = qualityRelative;
    else if (result.hasCnrDb) result.qualityPercent = clampFrontendPercent(result.cnrDb * 100.0 / 18.0);
}
#endif

#ifdef __linux__
std::string devicePath(const LinuxDvbTuneConfig& config, const char* device) {
    return "/dev/dvb/adapter" + std::to_string(config.adapter) + "/" + device +
        std::to_string(std::string(device) == "frontend" ? config.frontend : 0);
}

std::string deviceError(const std::string& operation, const std::string& path) {
    return operation + " (" + path + "): " + std::strerror(errno);
}

std::string readFirstLine(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::string value;
    if (input && std::getline(input, value)) {
        while (!value.empty() &&
               (value.back() == '\r' || value.back() == '\n' ||
                value.back() == ' ' || value.back() == '\t')) {
            value.pop_back();
        }
    }
    return value;
}

std::string frontendDriverModule(const LinuxDvbTuneConfig& config) {
    const std::filesystem::path link =
        std::filesystem::path("/sys/class/dvb") /
        ("dvb" + std::to_string(config.adapter) + ".frontend" +
         std::to_string(config.frontend)) /
        "device/driver/module";
    std::error_code ec;
    const auto resolved = std::filesystem::canonical(link, ec);
    return ec ? std::string{} : resolved.filename().string();
}

int driverMode(const std::string& module) {
    if (module.empty()) return -1;
    const std::string value = readFirstLine(
        std::filesystem::path("/sys/module") / module / "parameters/mode");
    if (value.empty()) return -1;
    try {
        std::size_t used = 0;
        const int mode = std::stoi(value, &used);
        return used == value.size() && mode >= 0 && mode <= 2 ? mode : -1;
    } catch (...) {
        return -1;
    }
}

const char* driverModeName(int mode) {
    switch (mode) {
        case 0: return "multiswitch";
        case 1: return "direct-diseqc";
        case 2: return "unicable";
        default: return "unknown";
    }
}

std::string frontendStatusText(fe_status_t status) {
    std::ostringstream out;
    bool first = true;
    auto add = [&](const char* name) {
        if (!first) out << '|';
        out << name;
        first = false;
    };
    if (status & FE_HAS_SIGNAL) add("SIGNAL");
    if (status & FE_HAS_CARRIER) add("CARRIER");
    if (status & FE_HAS_VITERBI) add("VITERBI");
    if (status & FE_HAS_SYNC) add("SYNC");
    if (status & FE_HAS_LOCK) add("LOCK");
    if (status & FE_TIMEDOUT) add("TIMEDOUT");
    if (status & FE_REINIT) add("REINIT");
    return first ? "NONE" : out.str();
}

bool setTone(int fd, fe_sec_tone_mode tone, const char* phase, std::string& error) {
    if (ioctl(fd, FE_SET_TONE, tone) == 0) return true;
    error = std::string(phase) + ": FE_SET_TONE failed: " + std::strerror(errno);
    return false;
}

bool setVoltage(int fd, fe_sec_voltage voltage, const char* phase, std::string& error) {
    if (ioctl(fd, FE_SET_VOLTAGE, voltage) == 0) return true;
    error = std::string(phase) + ": FE_SET_VOLTAGE failed: " + std::strerror(errno);
    return false;
}

bool sendMasterCommand(int fd, const dvb_diseqc_master_cmd& value,
                       const char* phase, std::string& error) {
    auto command = value;
    if (ioctl(fd, FE_DISEQC_SEND_MASTER_CMD, &command) == 0) return true;
    error = std::string(phase) + ": FE_DISEQC_SEND_MASTER_CMD failed: " +
        std::strerror(errno);
    return false;
}

bool sendSwitchCommands(int fd, int source, bool horizontal,
                        bool highBand, std::string& error) {
    if (source < 0) return true;

    if (source >= 4) {
        dvb_diseqc_master_cmd uncommitted {};
        uncommitted.msg[0] = 0xe0;
        uncommitted.msg[1] = 0x10;
        uncommitted.msg[2] = 0x39;
        uncommitted.msg[3] = static_cast<std::uint8_t>(
            0xf0 | ((source / 4) & 0x0f));
        uncommitted.msg_len = 4;
        if (!sendMasterCommand(fd, uncommitted,
                               "DiSEqC uncommitted switch", error)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }

    dvb_diseqc_master_cmd committed {};
    committed.msg[0] = 0xe0;
    committed.msg[1] = 0x10;
    committed.msg[2] = 0x38;
    committed.msg[3] = static_cast<std::uint8_t>(
        0xf0 | ((source % 4) << 2) |
        (horizontal ? 0x02 : 0x00) |
        (highBand ? 0x01 : 0x00));
    committed.msg_len = 4;
    return sendMasterCommand(fd, committed,
                             "DiSEqC committed switch", error);
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
        config.streamId < -1 || config.streamId > 255 ||
        config.lockTimeoutMs <= 0 || config.lockTimeoutMs > 120000) {
        error = "invalid DVB-S/S2 tuning parameters";
        return false;
    }

    const bool highBand = config.frequencyKHz >= config.lnbSlofKHz;
    const bool horizontal = config.polarity == "H";
    const std::uint32_t lof = highBand ? config.lnbLof2KHz : config.lnbLof1KHz;
    const std::uint32_t intermediateFrequency =
        config.frequencyKHz > lof
        ? config.frequencyKHz - lof
        : lof - config.frequencyKHz;
    if (intermediateFrequency == 0 ||
        intermediateFrequency >
            static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        error = "calculated satellite intermediate frequency is invalid";
        return false;
    }

    const std::string module = frontendDriverModule(config);
    const int mode = driverMode(module);
    const bool stid135 = module == "stid135";

    std::clog
        << "NATIVE DVB TUNE begin adapter=" << config.adapter
        << " frontend=" << config.frontend
        << " driver=" << (module.empty() ? "unknown" : module)
        << " mode=" << mode << "(" << driverModeName(mode) << ")"
        << " rf_khz=" << config.frequencyKHz
        << " if_khz=" << intermediateFrequency
        << " sr_ksps=" << config.symbolRateK
        << " pol=" << config.polarity
        << " band=" << (highBand ? "high" : "low")
        << " diseqc=" << config.diseqcSource
        << " stream_id=" << config.streamId
        << '\n';

    if (stid135 && mode == 2) {
        error =
            "STiD135 mode=2 (Unicable) detected, but DVBStreamer5 currently "
            "has no SCR/user-band frequency parameters in LinuxDvbTuneConfig; "
            "refusing legacy 13/18V + 22kHz tuning";
        std::clog
            << "NATIVE DVB TUNE reject "
               "reason=stid135_unicable_parameters_missing\n";
        return false;
    }

    const fe_sec_voltage voltage =
        horizontal ? SEC_VOLTAGE_18 : SEC_VOLTAGE_13;
    const fe_sec_tone_mode bandTone =
        highBand ? SEC_TONE_ON : SEC_TONE_OFF;

    if (stid135 && mode == 0) {
        if (!setVoltage(fd, voltage,
                        "STiD135 mode0 RF polarity select", error)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));

        if (!setTone(fd, bandTone,
                     "STiD135 mode0 RF band select", error)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(60));

        if (config.diseqcSource >= 0) {
            if (!sendSwitchCommands(fd, config.diseqcSource,
                                    horizontal, highBand, error)) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }
    } else {
        if (!setTone(fd, SEC_TONE_OFF,
                     "satellite pre-DiSEqC tone off", error)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        if (!setVoltage(fd, voltage,
                        "satellite LNB voltage select", error)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(60));

        if (config.diseqcSource >= 0) {
            if (!sendSwitchCommands(fd, config.diseqcSource,
                                    horizontal, highBand, error)) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }

        if (!setTone(fd, bandTone,
                     "satellite LNB band tone select", error)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    std::vector<dtv_property> properties(10);
    properties[0].cmd = DTV_CLEAR;
    properties[1].cmd = DTV_DELIVERY_SYSTEM;
    properties[1].u.data =
        config.deliverySystem == "dvb-s" ? SYS_DVBS : SYS_DVBS2;
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
        properties[count].u.data =
            static_cast<std::uint32_t>(config.streamId);
        ++count;
    }
    properties.resize(count + 1);
    properties[count].cmd = DTV_TUNE;

    if (!setFrontendProperties(fd, properties, error)) {
        std::clog
            << "NATIVE DVB TUNE property_error error=" << error << '\n';
        return false;
    }

    std::clog
        << "NATIVE DVB TUNE submitted properties="
        << properties.size() << '\n';

    const auto started = std::chrono::steady_clock::now();
    const auto deadline =
        started + std::chrono::milliseconds(config.lockTimeoutMs);
    auto nextLog = started;
    fe_status_t lastStatus {};
    bool haveLastStatus = false;

    while (std::chrono::steady_clock::now() < deadline) {
        fe_status_t status {};
        if (ioctl(fd, FE_READ_STATUS, &status) == 0) {
            const auto now = std::chrono::steady_clock::now();
            if (!haveLastStatus || status != lastStatus || now >= nextLog) {
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - started).count();

                std::clog
                    << "NATIVE DVB TUNE status elapsed_ms=" << elapsed
                    << " flags=" << frontendStatusText(status)
                    << " raw=0x" << std::hex
                    << static_cast<unsigned>(status)
                    << std::dec << '\n';

                lastStatus = status;
                haveLastStatus = true;
                nextLog = now + std::chrono::milliseconds(500);
            }

            if (status & FE_HAS_LOCK) {
                std::clog << "NATIVE DVB TUNE lock success\n";
                error.clear();
                return true;
            }
        } else if (errno != EAGAIN &&
                   errno != EWOULDBLOCK &&
                   errno != EINTR) {
            std::clog
                << "NATIVE DVB TUNE status_read_error errno=" << errno
                << " error=" << std::strerror(errno) << '\n';
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(stid135 ? 200 : 100));
    }

    error =
        "DVB frontend did not lock within " +
        std::to_string(config.lockTimeoutMs) +
        " milliseconds";

    std::clog
        << "NATIVE DVB TUNE lock timeout error=" << error << '\n';
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

bool LinuxDvbInput::hasActiveFrontend(int adapter, int frontend) noexcept {
#ifdef __linux__
    std::lock_guard<std::mutex> lock(activeFrontendRegistryMutex());
    return activeFrontendRegistry().count({adapter, frontend}) != 0;
#else
    (void)adapter;
    (void)frontend;
    return false;
#endif
}

bool LinuxDvbInput::activeFrontendStats(
    int adapter, int frontend, LinuxDvbFrontendStats& stats) noexcept {
    stats = LinuxDvbFrontendStats{};
#ifdef __linux__
    std::lock_guard<std::mutex> lock(activeFrontendRegistryMutex());
    const auto found = activeFrontendRegistry().find({adapter, frontend});
    if (found == activeFrontendRegistry().end()) return false;
    readFrontendStatsFromFd(found->second, stats);
    return true;
#else
    (void)adapter;
    (void)frontend;
    return false;
#endif
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
    if (!tuned) {
        ::close(frontendFd);
        return false;
    }
    if (!registerActiveFrontend(config.adapter, config.frontend, frontendFd)) {
        error = "DVB frontend already has an active in-process owner: " + frontend;
        ::close(frontendFd);
        return false;
    }
    frontendFd_ = frontendFd;
    frontendAdapter_ = config.adapter;
    frontendIndex_ = config.frontend;
    std::clog << "NATIVE DVB FRONTEND owner acquired adapter="
              << frontendAdapter_ << " frontend=" << frontendIndex_ << '\n';

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
        // Best-effort increase before starting the filter.  Some
        // drivers may reject DMX_SET_BUFFER_SIZE; tuning can still continue.
        if (::ioctl(fd, DMX_SET_BUFFER_SIZE, kDmxBufferBytes) != 0) {
            std::clog << "NATIVE DVB DEMUX buffer resize warning pid=" << pid
                      << " bytes=" << kDmxBufferBytes
                      << " error=" << std::strerror(errno) << '\n';
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
        if (errno == EOVERFLOW) {
            ++overflowCount_;
            // Linux DVB reports DVR/demux overrun as EOVERFLOW.  It means TS
            // packets were lost, but the frontend is still valid; dropping the
            // entire channel here turns a recoverable discontinuity into an
            // OFFLINE stream.  Continue draining and let CC diagnostics expose
            // any packet loss.
            if (overflowCount_ <= 3 || (overflowCount_ & (overflowCount_ - 1)) == 0) {
                std::clog << "NATIVE DVB DVR overflow recovered count="
                          << overflowCount_ << '\n';
            }
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
    if (frontendFd_ >= 0) {
        std::clog << "NATIVE DVB FRONTEND owner released adapter="
                  << frontendAdapter_ << " frontend=" << frontendIndex_ << '\n';
        releaseActiveFrontend(frontendAdapter_, frontendIndex_, frontendFd_);
        frontendFd_ = -1;
        frontendAdapter_ = -1;
        frontendIndex_ = -1;
    }
#endif
    demuxFds_.clear();
    overflowCount_ = 0;
}

} // namespace dvbstreamer5::media::network
