#include "DvbSatellite.h"
#include "media/LinuxDvbInput.h"
#include "media/DvbText.h"

#include <linux/dvb/frontend.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string_view>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

struct DvbService {
    uint16_t serviceId = 0;
    uint16_t pmtPid = 0;
    uint16_t pcrPid = 0x1FFF;
    uint8_t serviceType = 0;
    bool scrambled = false;
    bool pmtParsed = false;
    std::vector<uint16_t> streamPids;
    std::vector<uint16_t> caPids;      // compatibility: ECM + EMM
    std::vector<uint16_t> ecmPids;
    std::vector<uint16_t> emmPids;
    std::string name;
    std::string provider;
};

struct FrontendStats {
    bool available = false;
    bool locked = false;
    int signalPercent = 0;
    int qualityPercent = 0;
    double signalDb = 0.0;
    double cnrDb = 0.0;
    bool hasSignalDb = false;
    bool hasCnrDb = false;
    bool qualityEstimated = false;
};

struct FrontendMetadata {
    std::string name;
    std::string model;
    std::string driverModule;
    int driverMode = -1;
};

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

std::string percentEncode(const std::string& value) {
    std::ostringstream out;
    const char hex[] = "0123456789ABCDEF";
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out << static_cast<char>(c);
        } else {
            out << '%' << hex[c >> 4] << hex[c & 0x0F];
        }
    }
    return out.str();
}

int fromHex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string percentDecode(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int hi = fromHex(value[i + 1]);
            const int lo = fromHex(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                result.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        if (value[i] == '+') result.push_back(' ');
        else result.push_back(value[i]);
    }
    return result;
}

std::map<std::string, std::string> parseQuery(const std::string& uri) {
    std::map<std::string, std::string> values;
    const auto pos = uri.find('?');
    if (pos == std::string::npos) return values;
    std::string_view query(uri.data() + pos + 1, uri.size() - pos - 1);
    size_t start = 0;
    while (start <= query.size()) {
        const size_t end = query.find('&', start);
        const std::string_view item = query.substr(start, end == std::string_view::npos ? query.size() - start : end - start);
        const size_t eq = item.find('=');
        if (eq != std::string_view::npos) {
            values[percentDecode(std::string(item.substr(0, eq)))] = percentDecode(std::string(item.substr(eq + 1)));
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return values;
}

bool parseInt(const std::map<std::string, std::string>& query, const std::string& key, int& value) {
    const auto found = query.find(key);
    if (found == query.end() || found->second.empty()) return true;
    try {
        size_t used = 0;
        const int parsed = std::stoi(found->second, &used);
        if (used != found->second.size()) return false;
        value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool parseUInt(const std::map<std::string, std::string>& query, const std::string& key, uint32_t& value) {
    const auto found = query.find(key);
    if (found == query.end() || found->second.empty()) return true;
    try {
        size_t used = 0;
        const unsigned long parsed = std::stoul(found->second, &used);
        if (used != found->second.size() || parsed > 0xFFFFFFFFUL) return false;
        value = static_cast<uint32_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool validateUnicableParams(const DvbSatelliteParams& params, std::string& error) {
    if (params.unicableMode == "none") return true;
    if (params.unicableMode != "en50494" && params.unicableMode != "en50607") {
        error = "Unicable mode must be none, en50494 or en50607";
        return false;
    }
    const bool en50494 = params.unicableMode == "en50494";
    const int maxUserBand = en50494 ? 7 : 31;
    const int maxPosition = en50494 ? 1 : 63;
    if (params.unicableUserBand < 0 || params.unicableUserBand > maxUserBand) {
        error = en50494 ? "EN50494 User Band ID must be 0..7"
                        : "EN50607 User Band ID must be 0..31";
        return false;
    }
    if (params.unicableFrequencyKHz < 950000 || params.unicableFrequencyKHz > 2150000) {
        error = "Unicable User Band frequency must be 950..2150 MHz";
        return false;
    }
    if (params.unicablePosition < 0 || params.unicablePosition > maxPosition) {
        error = en50494 ? "EN50494 satellite position must be 0..1"
                        : "EN50607 satellite position must be 0..63";
        return false;
    }
    if (params.unicablePin < -1 || params.unicablePin > 255) {
        error = "Unicable PIN must be -1 (disabled) or 0..255";
        return false;
    }
    return true;
}

DvbSatelliteParams paramsFromJson(const Json::Value& root, std::string& error) {
    DvbSatelliteParams params;
    params.adapter = std::clamp(root.get("adapter", 0).asInt(), 0, 31);
    params.frontend = std::clamp(root.get("frontend", 0).asInt(), 0, 31);

    const double frequencyMHz = root.get("frequency_mhz", 11727.0).asDouble();
    if (!std::isfinite(frequencyMHz) || frequencyMHz < 900.0 || frequencyMHz > 14000.0) {
        error = "Frequency must be specified in MHz (900..14000)";
        return params;
    }
    params.frequencyKHz = static_cast<uint32_t>(std::llround(frequencyMHz * 1000.0));
    params.symbolRateK = root.get("symbol_rate", 27500).asUInt();
    if (params.symbolRateK < 100 || params.symbolRateK > 60000) {
        error = "Symbol rate must be in kSym/s (100..60000)";
        return params;
    }

    params.polarity = upper(root.get("polarity", "H").asString());
    if (params.polarity != "H" && params.polarity != "V") {
        error = "Polarity must be H or V";
        return params;
    }
    params.deliverySystem = lower(root.get("delivery_system", "dvb-s2").asString());
    if (params.deliverySystem != "dvb-s" && params.deliverySystem != "dvb-s2") {
        error = "Delivery system must be dvb-s or dvb-s2";
        return params;
    }
    params.modulation = lower(root.get("modulation", "auto").asString());
    static const std::set<std::string> allowedModulations = {"auto", "qpsk", "8psk", "16apsk", "32apsk"};
    if (!allowedModulations.count(params.modulation)) params.modulation = "auto";
    params.fec = lower(root.get("fec", "auto").asString());
    static const std::set<std::string> allowedFec = {"auto", "1/2", "2/3", "3/4", "4/5", "5/6", "6/7", "7/8", "8/9", "9/10", "3/5", "2/5"};
    if (!allowedFec.count(params.fec)) params.fec = "auto";
    params.diseqcSource = std::clamp(root.get("diseqc_source", -1).asInt(), -1, 7);
    params.lnbLof1KHz = root.get("lnb_lof1_mhz", 9750).asUInt() * 1000U;
    params.lnbLof2KHz = root.get("lnb_lof2_mhz", 10600).asUInt() * 1000U;
    params.lnbSlofKHz = root.get("lnb_slof_mhz", 11700).asUInt() * 1000U;
    params.unicableMode = lower(root.get("unicable_mode", "none").asString());
    params.unicableUserBand = root.get("unicable_user_band", -1).asInt();
    const double unicableFrequencyMHz = root.get("unicable_frequency_mhz", 0.0).asDouble();
    if (std::isfinite(unicableFrequencyMHz) && unicableFrequencyMHz > 0.0)
        params.unicableFrequencyKHz =
            static_cast<uint32_t>(std::llround(unicableFrequencyMHz * 1000.0));
    params.unicablePosition = root.get("unicable_position", 0).asInt();
    params.unicablePin = root.get("unicable_pin", -1).asInt();
    params.streamId = std::clamp(root.get("stream_id", -1).asInt(), -1, 255);
    if (params.unicableMode == "none") {
        params.unicableUserBand = -1;
        params.unicableFrequencyKHz = 0;
        params.unicablePosition = 0;
        params.unicablePin = -1;
    }
    if (!validateUnicableParams(params, error)) return params;
    return params;
}

std::string frontendPath(const DvbSatelliteParams& params) {
    return "/dev/dvb/adapter" + std::to_string(params.adapter) + "/frontend" + std::to_string(params.frontend);
}

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    value.erase(value.begin(), first);
    return value;
}

bool readTextFile(const std::filesystem::path& path, std::string& value) {
    std::ifstream input(path);
    if (!input) return false;
    std::getline(input, value);
    value = trim(value);
    return !value.empty();
}

std::string normalizedTbsModel(const std::string& frontendName) {
    const std::string name = lower(frontendName);
    if (name.find("6909x") != std::string::npos || name.find("6909-x") != std::string::npos) {
        return "TBS6909-X v2";
    }
    if (name.find("6909") != std::string::npos) return "TBS6909";
    return frontendName;
}

int readDriverMode(const std::string& module) {
    if (module.empty()) return -1;
    std::string value;
    if (!readTextFile(std::filesystem::path("/sys/module") / module / "parameters/mode", value)) {
        return -1;
    }
    try {
        std::size_t used = 0;
        const int mode = std::stoi(value, &used);
        return used == value.size() && mode >= 0 && mode <= 2 ? mode : -1;
    } catch (...) {
        return -1;
    }
}

FrontendMetadata readFrontendMetadata(int adapter, int frontend) {
    FrontendMetadata metadata;
#ifdef __linux__
    // Live source owns frontendN; dashboard metadata must not open it again.
    if (!dvbstreamer5::media::network::LinuxDvbInput::hasActiveFrontend(adapter, frontend)) {
        const std::string device = "/dev/dvb/adapter" + std::to_string(adapter) +
            "/frontend" + std::to_string(frontend);
        const int fd = open(device.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            dvb_frontend_info info {};
            if (ioctl(fd, FE_GET_INFO, &info) == 0) {
                metadata.name = trim(std::string(info.name, strnlen(info.name, sizeof(info.name))));
            }
            close(fd);
        }
    }

    const std::filesystem::path moduleLink = std::filesystem::path("/sys/class/dvb") /
        ("dvb" + std::to_string(adapter) + ".frontend" + std::to_string(frontend)) /
        "device/driver/module";
    std::error_code ec;
    const auto resolved = std::filesystem::canonical(moduleLink, ec);
    if (!ec) metadata.driverModule = resolved.filename().string();

    metadata.model = normalizedTbsModel(metadata.name);
    const std::string model = lower(metadata.model);
    if (model.find("tbs6909-x") != std::string::npos) {
        metadata.driverModule = "stid135";
    } else if (model.find("tbs6909") != std::string::npos) {
        metadata.driverModule = "mxl58x";
    }
    metadata.driverMode = readDriverMode(metadata.driverModule);
#else
    (void)adapter;
    (void)frontend;
#endif
    return metadata;
}

const char* driverModeName(int mode) {
    switch (mode) {
        case 0: return "multiswitch";
        case 1: return "direct-diseqc";
        case 2: return "unicable";
        default: return "unknown";
    }
}

int clampPercent(double value) {
    return std::clamp(static_cast<int>(std::lround(value)), 0, 100);
}

bool readPropertyStat(int fd, uint32_t command, int& percent, double& db, bool& hasDb) {
    dtv_property property{};
    property.cmd = command;
    dtv_properties properties{};
    properties.num = 1;
    properties.props = &property;
    if (ioctl(fd, FE_GET_PROPERTY, &properties) != 0 || property.u.st.len < 1) return false;

    const auto& stat = property.u.st.stat[0];
    if (stat.scale == FE_SCALE_RELATIVE) {
        percent = clampPercent(static_cast<double>(stat.uvalue) * 100.0 / 65535.0);
        return true;
    }
    if (stat.scale == FE_SCALE_DECIBEL) {
        db = static_cast<double>(stat.svalue) / 1000.0;
        hasDb = true;
        return true;
    }
    return false;
}

FrontendStats readFrontendStats(const DvbSatelliteParams& params) {
    FrontendStats result;
    dvbstreamer5::media::network::LinuxDvbFrontendStats live;
    if (dvbstreamer5::media::network::LinuxDvbInput::activeFrontendStats(
            params.adapter, params.frontend, live)) {
        result.available = live.available;
        result.locked = live.locked;
        result.signalPercent = live.signalPercent;
        result.qualityPercent = live.qualityPercent;
        result.signalDb = live.signalDb;
        result.cnrDb = live.cnrDb;
        result.hasSignalDb = live.hasSignalDb;
        result.hasCnrDb = live.hasCnrDb;
        result.qualityEstimated = live.qualityEstimated;
        return result;
    }
    const int fd = open(frontendPath(params).c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) return result;
    result.available = true;

    fe_status_t status{};
    if (ioctl(fd, FE_READ_STATUS, &status) == 0) result.locked = (status & FE_HAS_LOCK) != 0;

    int signalRelative = -1;
    int qualityRelative = -1;
    readPropertyStat(fd, DTV_STAT_SIGNAL_STRENGTH, signalRelative, result.signalDb, result.hasSignalDb);
    readPropertyStat(fd, DTV_STAT_CNR, qualityRelative, result.cnrDb, result.hasCnrDb);

    if (signalRelative < 0) {
        uint16_t legacy = 0;
        if (ioctl(fd, FE_READ_SIGNAL_STRENGTH, &legacy) == 0) {
            signalRelative = clampPercent(static_cast<double>(legacy) * 100.0 / 65535.0);
        }
    }
    if (qualityRelative < 0) {
        uint16_t legacy = 0;
        if (ioctl(fd, FE_READ_SNR, &legacy) == 0) {
            qualityRelative = clampPercent(static_cast<double>(legacy) * 100.0 / 65535.0);
        }
    }

    if (signalRelative >= 0) result.signalPercent = signalRelative;
    else if (result.hasSignalDb) result.signalPercent = clampPercent((result.signalDb + 100.0) * 1.5);

    if (qualityRelative > 0) result.qualityPercent = qualityRelative;
    else if (result.hasCnrDb) result.qualityPercent = clampPercent(result.cnrDb * 100.0 / 18.0);
    else if (result.locked && result.signalPercent > 0) {
        result.qualityPercent = result.signalPercent;
        result.qualityEstimated = true;
    }

    close(fd);
    return result;
}

Json::Value statsToJson(const FrontendStats& stats) {
    Json::Value root;
    root["available"] = stats.available;
    root["locked"] = stats.locked;
    root["signal"] = stats.signalPercent;
    root["quality"] = stats.qualityPercent;
    root["quality_estimated"] = stats.qualityEstimated;
    if (stats.hasSignalDb) root["signal_db"] = stats.signalDb;
    if (stats.hasCnrDb) root["cnr_db"] = stats.cnrDb;
    return root;
}

std::string decodeDvbText(const uint8_t* data, size_t size) {
    return dvbstreamer5::media::dvbtext::decode(data, size);
}


class PsiScanner {
public:
    void feed(const uint8_t* data, size_t size) {
        if (!data || !size) return;
        remainder.insert(remainder.end(), data, data + size);
        size_t offset = 0;
        while (remainder.size() - offset >= 188) {
            if (remainder[offset] != 0x47) {
                const auto next = std::find(remainder.begin() + static_cast<std::ptrdiff_t>(offset + 1), remainder.end(), 0x47);
                if (next == remainder.end()) {
                    remainder.clear();
                    return;
                }
                offset = static_cast<size_t>(std::distance(remainder.begin(), next));
                continue;
            }
            parsePacket(remainder.data() + offset);
            offset += 188;
        }
        if (offset) remainder.erase(remainder.begin(), remainder.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    std::vector<DvbService> services() const {
        std::vector<DvbService> result;
        for (const auto& [sid, pmt] : programMap) {
            DvbService item;
            item.serviceId = sid;
            item.pmtPid = pmt;
            const auto meta = serviceMap.find(sid);
            if (meta != serviceMap.end()) {
                item = meta->second;
                item.serviceId = sid;
                item.pmtPid = pmt;
            }
            if (item.scrambled) {
                for (uint16_t caPid : catCaPids) {
                    if (std::find(item.emmPids.begin(), item.emmPids.end(), caPid) == item.emmPids.end())
                        item.emmPids.push_back(caPid);
                    if (std::find(item.caPids.begin(), item.caPids.end(), caPid) == item.caPids.end())
                        item.caPids.push_back(caPid);
                }
            }
            if (item.name.empty()) item.name = "Service " + std::to_string(sid);
            result.push_back(std::move(item));
        }
        std::sort(result.begin(), result.end(), [](const DvbService& a, const DvbService& b) {
            if (a.name != b.name) return a.name < b.name;
            return a.serviceId < b.serviceId;
        });
        return result;
    }

private:
    struct SectionState {
        std::vector<uint8_t> bytes;
        size_t expected = 0;
    };

    bool isPmtPid(uint16_t pid) const {
        for (const auto& [sid, pmtPid] : programMap) {
            (void)sid;
            if (pmtPid == pid) return true;
        }
        return false;
    }

    void markPacketScrambled(uint16_t pid) {
        for (auto& [sid, service] : serviceMap) {
            (void)sid;
            if (service.pcrPid == pid ||
                std::find(service.streamPids.begin(), service.streamPids.end(), pid) != service.streamPids.end()) {
                service.scrambled = true;
            }
        }
    }

    void parsePacket(const uint8_t* packet) {
        if (packet[0] != 0x47 || (packet[1] & 0x80)) return;
        const uint16_t pid = static_cast<uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);
        const uint8_t scrambling = static_cast<uint8_t>((packet[3] >> 6) & 0x03);
        if (scrambling != 0) markPacketScrambled(pid);

        if (pid != 0x0000 && pid != 0x0001 && pid != 0x0011 && !isPmtPid(pid)) return;
        const bool payloadStart = (packet[1] & 0x40) != 0;
        const uint8_t adaptation = static_cast<uint8_t>((packet[3] >> 4) & 0x03);
        if (adaptation == 0 || adaptation == 2) return;
        size_t offset = 4;
        if (adaptation == 3) {
            offset += static_cast<size_t>(packet[4]) + 1;
            if (offset >= 188) return;
        }
        feedSection(pid, packet + offset, 188 - offset, payloadStart);
    }

    void feedSection(uint16_t pid, const uint8_t* payload, size_t size, bool payloadStart) {
        if (!payload || !size) return;
        SectionState& state = sections[pid];
        size_t offset = 0;
        if (payloadStart) {
            const size_t pointer = payload[0];
            offset = 1;
            if (!state.bytes.empty() && pointer && offset + pointer <= size) {
                appendSection(pid, state, payload + offset, pointer);
            }
            offset += pointer;
            state.bytes.clear();
            state.expected = 0;
        }

        while (offset < size) {
            if (!state.bytes.empty()) {
                if (state.expected == 0) {
                    const size_t headerNeeded = state.bytes.size() < 3 ? 3 - state.bytes.size() : 0;
                    const size_t takeHeader = std::min(headerNeeded, size - offset);
                    state.bytes.insert(state.bytes.end(), payload + offset, payload + offset + takeHeader);
                    offset += takeHeader;
                    if (state.bytes.size() < 3) return;
                    const size_t sectionLength = static_cast<size_t>(((state.bytes[1] & 0x0F) << 8) | state.bytes[2]);
                    state.expected = 3 + sectionLength;
                    if (state.expected < 8 || state.expected > 4096) {
                        state.bytes.clear();
                        state.expected = 0;
                        return;
                    }
                }
                const size_t needed = state.expected > state.bytes.size() ? state.expected - state.bytes.size() : 0;
                const size_t take = std::min(needed, size - offset);
                state.bytes.insert(state.bytes.end(), payload + offset, payload + offset + take);
                offset += take;
                if (state.expected && state.bytes.size() >= state.expected) {
                    parseSection(pid, state.bytes.data(), state.expected);
                    state.bytes.clear();
                    state.expected = 0;
                }
                continue;
            }
            if (payload[offset] == 0xFF) return;
            if (size - offset < 3) {
                state.bytes.assign(payload + offset, payload + size);
                state.expected = 0;
                return;
            }
            const size_t sectionLength = static_cast<size_t>(((payload[offset + 1] & 0x0F) << 8) | payload[offset + 2]);
            const size_t total = 3 + sectionLength;
            if (total < 8 || total > 4096) return;
            if (size - offset >= total) {
                parseSection(pid, payload + offset, total);
                offset += total;
            } else {
                state.bytes.assign(payload + offset, payload + size);
                state.expected = total;
                return;
            }
        }
    }

    void appendSection(uint16_t pid, SectionState& state, const uint8_t* data, size_t size) {
        if (!state.bytes.empty() && state.expected) {
            const size_t take = std::min(size, state.expected - state.bytes.size());
            state.bytes.insert(state.bytes.end(), data, data + take);
            if (state.bytes.size() >= state.expected) parseSection(pid, state.bytes.data(), state.expected);
        }
        state.bytes.clear();
        state.expected = 0;
    }

    void parseSection(uint16_t pid, const uint8_t* section, size_t size) {
        if (pid == 0x0000) parsePat(section, size);
        else if (pid == 0x0001) parseCat(section, size);
        else if (pid == 0x0011) parseSdt(section, size);
        else if (isPmtPid(pid)) parsePmt(section, size);
    }

    void parsePat(const uint8_t* section, size_t size) {
        if (size < 12 || section[0] != 0x00) return;
        const size_t end = size >= 4 ? size - 4 : 0;
        for (size_t pos = 8; pos + 4 <= end; pos += 4) {
            const uint16_t program = static_cast<uint16_t>((section[pos] << 8) | section[pos + 1]);
            const uint16_t pid = static_cast<uint16_t>(((section[pos + 2] & 0x1F) << 8) | section[pos + 3]);
            if (program != 0) {
                programMap[program] = pid;
                DvbService& service = serviceMap[program];
                service.serviceId = program;
                service.pmtPid = pid;
            }
        }
    }

    static bool collectCaDescriptors(const uint8_t* data, size_t size, std::vector<uint16_t>* caPids) {
        bool found = false;
        size_t pos = 0;
        while (pos + 2 <= size) {
            const uint8_t tag = data[pos];
            const size_t length = data[pos + 1];
            if (pos + 2 + length > size) break;
            if (tag == 0x09) { // conditional_access_descriptor
                found = true;
                if (caPids && length >= 4) {
                    const uint8_t* d = data + pos + 2;
                    const uint16_t caPid = static_cast<uint16_t>(((d[2] & 0x1F) << 8) | d[3]);
                    if (caPid < 0x1FFF &&
                        std::find(caPids->begin(), caPids->end(), caPid) == caPids->end()) {
                        caPids->push_back(caPid);
                    }
                }
            }
            pos += 2 + length;
        }
        return found;
    }

    void parseCat(const uint8_t* section, size_t size) {
        if (size < 12 || section[0] != 0x01) return;
        const size_t end = size >= 4 ? size - 4 : 0;
        if (end <= 8) return;
        std::vector<uint16_t> found;
        collectCaDescriptors(section + 8, end - 8, &found);
        for (uint16_t pid : found) catCaPids.insert(pid);
    }

    void parsePmt(const uint8_t* section, size_t size) {
        if (size < 16 || section[0] != 0x02) return;
        const uint16_t sid = static_cast<uint16_t>((section[3] << 8) | section[4]);
        const size_t end = size >= 4 ? size - 4 : 0;
        if (end <= 12) return;

        DvbService& service = serviceMap[sid];
        service.serviceId = sid;
        if (const auto it = programMap.find(sid); it != programMap.end()) service.pmtPid = it->second;
        service.pcrPid = static_cast<uint16_t>(((section[8] & 0x1F) << 8) | section[9]);

        const size_t programInfoLength = static_cast<size_t>(((section[10] & 0x0F) << 8) | section[11]);
        size_t pos = 12;
        if (pos + programInfoLength > end) return;
        std::vector<uint16_t> programCaPids;
        if (collectCaDescriptors(section + pos, programInfoLength, &programCaPids)) service.scrambled = true;
        for (uint16_t caPid : programCaPids) {
            if (std::find(service.ecmPids.begin(), service.ecmPids.end(), caPid) == service.ecmPids.end()) service.ecmPids.push_back(caPid);
            if (std::find(service.caPids.begin(), service.caPids.end(), caPid) == service.caPids.end()) service.caPids.push_back(caPid);
        }
        pos += programInfoLength;

        std::vector<uint16_t> streamPids;
        while (pos + 5 <= end) {
            const uint16_t elementaryPid = static_cast<uint16_t>(((section[pos + 1] & 0x1F) << 8) | section[pos + 2]);
            const size_t esInfoLength = static_cast<size_t>(((section[pos + 3] & 0x0F) << 8) | section[pos + 4]);
            pos += 5;
            if (pos + esInfoLength > end) break;
            std::vector<uint16_t> esCaPids;
            if (collectCaDescriptors(section + pos, esInfoLength, &esCaPids)) service.scrambled = true;
            for (uint16_t caPid : esCaPids) {
                if (std::find(service.ecmPids.begin(), service.ecmPids.end(), caPid) == service.ecmPids.end()) service.ecmPids.push_back(caPid);
                if (std::find(service.caPids.begin(), service.caPids.end(), caPid) == service.caPids.end()) service.caPids.push_back(caPid);
            }
            if (elementaryPid != 0x1FFF &&
                std::find(streamPids.begin(), streamPids.end(), elementaryPid) == streamPids.end()) {
                streamPids.push_back(elementaryPid);
            }
            pos += esInfoLength;
        }
        if (!streamPids.empty()) service.streamPids = std::move(streamPids);
        service.pmtParsed = true;
    }

    void parseSdt(const uint8_t* section, size_t size) {
        if (size < 15 || (section[0] != 0x42 && section[0] != 0x46)) return;
        const size_t end = size >= 4 ? size - 4 : 0;
        size_t pos = 11;
        while (pos + 5 <= end) {
            const uint16_t sid = static_cast<uint16_t>((section[pos] << 8) | section[pos + 1]);
            DvbService& service = serviceMap[sid];
            service.serviceId = sid;
            if ((section[pos + 3] & 0x10) != 0) service.scrambled = true; // free_CA_mode
            const size_t descriptorLength = static_cast<size_t>(((section[pos + 3] & 0x0F) << 8) | section[pos + 4]);
            pos += 5;
            const size_t descriptorEnd = std::min(end, pos + descriptorLength);
            while (pos + 2 <= descriptorEnd) {
                const uint8_t tag = section[pos];
                const size_t length = section[pos + 1];
                if (pos + 2 + length > descriptorEnd) break;
                if (tag == 0x48 && length >= 3) {
                    const uint8_t* descriptor = section + pos + 2;
                    service.serviceType = descriptor[0];
                    size_t inner = 1;
                    if (inner < length) {
                        const size_t providerLength = descriptor[inner++];
                        if (inner + providerLength <= length) {
                            service.provider = decodeDvbText(descriptor + inner, providerLength);
                            inner += providerLength;
                        }
                    }
                    if (inner < length) {
                        const size_t nameLength = descriptor[inner++];
                        if (inner + nameLength <= length) {
                            service.name = decodeDvbText(descriptor + inner, nameLength);
                        }
                    }
                } else if (tag == 0x09) {
                    service.scrambled = true;
                }
                pos += 2 + length;
            }
            pos = descriptorEnd;
        }
    }

    std::vector<uint8_t> remainder;
    std::map<uint16_t, SectionState> sections;
    std::map<uint16_t, uint16_t> programMap;
    std::map<uint16_t, DvbService> serviceMap;
    std::set<uint16_t> catCaPids;
};

std::string servicePidsString(const DvbService& service) {
    // Never build a PSI-only service filter. If PMT/ES data has not arrived,
    // keep the full transport stream so startup can retry PID discovery.
    if (!service.pmtParsed || service.streamPids.empty()) return {};

    std::set<uint16_t> pids = {0x0000, 0x0001, 0x0010, 0x0011, 0x0012, 0x0014};
    if (service.pmtPid > 0 && service.pmtPid < 0x1FFF) pids.insert(service.pmtPid);
    if (service.pcrPid < 0x1FFF) pids.insert(service.pcrPid);
    for (uint16_t pid : service.streamPids) {
        if (pid < 0x1FFF) pids.insert(pid);
    }
    for (uint16_t pid : service.caPids) {
        if (pid < 0x1FFF) pids.insert(pid);
    }
    std::ostringstream out;
    bool first = true;
    for (uint16_t pid : pids) {
        if (!first) out << ':';
        first = false;
        out << pid;
    }
    return out.str();
}

std::mutex& frontendTuneMutex(const DvbSatelliteParams& params) {
    // The web scan modal polls /api/dvb-signal and /api/dvb-scan can be
    // requested from the same or another browser.  Serialize tune pipelines
    // per physical frontend so a signal probe cannot overlap the scan/open
    // sequence and transiently produce EBUSY.
    static std::array<std::mutex, 32 * 32> locks;
    const int adapter = std::clamp(params.adapter, 0, 31);
    const int frontend = std::clamp(params.frontend, 0, 31);
    return locks[static_cast<std::size_t>(adapter * 32 + frontend)];
}

Json::Value runTune(const DvbSatelliteParams& params, bool collectServices, int timeoutMs) {
    Json::Value result;
    const std::string device = frontendPath(params);
    result["adapter"] = params.adapter;
    result["frontend"] = params.frontend;
    result["device"] = device;
    result["stream_id"] = params.streamId;
    result["diseqc_source"] = params.diseqcSource;
    result["unicable_mode"] = params.unicableMode;
    result["unicable_user_band"] = params.unicableUserBand;
    result["unicable_frequency_khz"] = params.unicableFrequencyKHz;
    result["unicable_position"] = params.unicablePosition;
    result["unicable_pin"] = params.unicablePin;
    const FrontendMetadata metadata = readFrontendMetadata(params.adapter, params.frontend);
    result["frontend_name"] = metadata.name;
    result["frontend_model"] = metadata.model;
    result["driver_module"] = metadata.driverModule;
    result["driver_mode"] = metadata.driverMode;
    result["driver_mode_name"] = driverModeName(metadata.driverMode);
    std::cerr << "DVB " << (collectServices ? "scan" : "signal")
              << " tune request: adapter=" << params.adapter
              << " frontend=" << params.frontend
              << " device=" << device
              << " frequency_khz=" << params.frequencyKHz
              << " symbol_rate=" << params.symbolRateK
              << " polarity=" << params.polarity
              << " isi=" << params.streamId
              << " diseqc_source=" << params.diseqcSource
              << " unicable=" << params.unicableMode
              << " ub=" << params.unicableUserBand
              << " ub_khz=" << params.unicableFrequencyKHz
              << " position=" << params.unicablePosition
              << " frontend_model=" << metadata.model
              << " driver_mode=" << metadata.driverMode
              << std::endl;
    if (!std::filesystem::exists(device)) {
        result["error"] = "DVB frontend not found: " + device;
        return result;
    }

    std::unique_lock<std::mutex> frontendLock(frontendTuneMutex(params));

    PsiScanner scanner;
    FrontendStats stats;
    std::string tuneError;
    bool tuned = false;
    constexpr int kTuneStartupAttempts = 3;
    constexpr auto kTuneRetryDelay = std::chrono::milliseconds(700);

    for (int attempt = 1; attempt <= kTuneStartupAttempts; ++attempt) {
        PsiScanner attemptScanner;
        FrontendStats attemptStats;
        std::string attemptError;
        const auto attemptDeadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        dvbstreamer5::media::network::LinuxDvbTuneConfig tune;
        tune.adapter = params.adapter;
        tune.frontend = params.frontend;
        tune.frequencyKHz = params.frequencyKHz;
        tune.symbolRateK = params.symbolRateK;
        tune.polarity = params.polarity;
        tune.deliverySystem = params.deliverySystem;
        tune.modulation = params.modulation;
        tune.fec = params.fec;
        tune.diseqcSource = params.diseqcSource;
        tune.lnbLof1KHz = params.lnbLof1KHz;
        tune.lnbLof2KHz = params.lnbLof2KHz;
        tune.lnbSlofKHz = params.lnbSlofKHz;
        tune.unicableMode = params.unicableMode;
        tune.unicableUserBand = params.unicableUserBand;
        tune.unicableFrequencyKHz = params.unicableFrequencyKHz;
        tune.unicablePosition = params.unicablePosition;
        tune.unicablePin = params.unicablePin;
        tune.streamId = params.streamId;
        tune.pids = "8192";
        tune.lockTimeoutMs = timeoutMs;

        dvbstreamer5::media::network::LinuxDvbInput input;
        tuned = input.open(tune, attemptError);
        if (tuned) {
            attemptStats = readFrontendStats(params);
            attemptStats.locked = true;
            auto lastStatsRead = std::chrono::steady_clock::time_point{};
            std::array<std::uint8_t, 188 * 128> buffer {};
            while (std::chrono::steady_clock::now() < attemptDeadline) {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    attemptDeadline - std::chrono::steady_clock::now());
                const int waitMs = std::max(1, static_cast<int>(
                    std::min(remaining, std::chrono::milliseconds(150)).count()));
                std::size_t received = 0;
                if (!input.read(buffer.data(), buffer.size(), received, waitMs, attemptError)) {
                    break;
                }
                if (received && collectServices) {
                    attemptScanner.feed(buffer.data(), received);
                }

                const auto now = std::chrono::steady_clock::now();
                if (lastStatsRead.time_since_epoch().count() == 0 ||
                    now - lastStatsRead >= std::chrono::milliseconds(350)) {
                    const FrontendStats current = readFrontendStats(params);
                    attemptStats.available = attemptStats.available || current.available;
                    attemptStats.signalPercent =
                        std::max(attemptStats.signalPercent, current.signalPercent);
                    attemptStats.qualityPercent =
                        std::max(attemptStats.qualityPercent, current.qualityPercent);
                    attemptStats.locked = attemptStats.locked || current.locked;
                    if (current.hasSignalDb) {
                        attemptStats.signalDb = current.signalDb;
                        attemptStats.hasSignalDb = true;
                    }
                    if (current.hasCnrDb) {
                        attemptStats.cnrDb = current.cnrDb;
                        attemptStats.hasCnrDb = true;
                    }
                    lastStatsRead = now;
                }
            }
            input.close();
        } else {
            attemptStats = readFrontendStats(params);
        }

        scanner = std::move(attemptScanner);
        stats = attemptStats;
        tuneError = attemptError;

        // A normal no-lock/no-data result is not a startup failure. Retry only
        // native frontend/tap errors, such as a transient driver EBUSY.
        if (tuned || tuneError.empty() ||
            tuneError.find("did not lock") != std::string::npos) break;
        if (attempt < kTuneStartupAttempts) {
            std::cerr << "DVB " << (collectServices ? "scan" : "signal")
                      << " startup retry: adapter=" << params.adapter
                      << " frontend=" << params.frontend
                      << " attempt=" << attempt
                      << " error=" << tuneError << std::endl;
            std::this_thread::sleep_for(kTuneRetryDelay);
            tuneError.clear();
            continue;
        }
    }

    // Let DVB drivers finish releasing the frontend/demux after closing the
    // native tap before a live stream starts on the same frontend.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    result["locked"] = stats.locked;
    result["signal"] = stats.signalPercent;
    result["quality"] = stats.qualityPercent;
    if (stats.hasSignalDb) result["signal_db"] = stats.signalDb;
    if (stats.hasCnrDb) result["cnr_db"] = stats.cnrDb;
    if (!tuneError.empty()) result["error"] = tuneError;
    else if (!tuned) result["error"] = "No satellite signal / frontend lock";

    if (collectServices) {
        Json::Value services(Json::arrayValue);
        for (const auto& service : scanner.services()) {
            Json::Value item;
            item["service_id"] = service.serviceId;
            item["pmt_pid"] = service.pmtPid;
            item["pcr_pid"] = service.pcrPid < 0x1FFF ? service.pcrPid : 0;
            item["service_type"] = service.serviceType;
            item["name"] = service.name;
            item["provider"] = service.provider;
            item["scrambled"] = service.scrambled;
            const bool pmtReady = service.pmtParsed && !service.streamPids.empty();
            item["pmt_ready"] = pmtReady;
            item["access"] = service.scrambled ? "CA" : (pmtReady ? "FTA" : "UNKNOWN");
            Json::Value streamPids(Json::arrayValue);
            for (uint16_t pid : service.streamPids) streamPids.append(pid);
            item["stream_pids"] = streamPids;
            Json::Value caPids(Json::arrayValue);
            for (uint16_t pid : service.caPids) caPids.append(pid);
            item["ca_pids"] = caPids;
            Json::Value ecmPids(Json::arrayValue);
            for (uint16_t pid : service.ecmPids) ecmPids.append(pid);
            item["ecm_pids"] = ecmPids;
            Json::Value emmPids(Json::arrayValue);
            for (uint16_t pid : service.emmPids) emmPids.append(pid);
            item["emm_pids"] = emmPids;
            item["emm_receive_enabled"] = service.scrambled && !service.emmPids.empty();
            const std::string pids = servicePidsString(service);
            item["service_pids"] = pids;
            DvbSatelliteParams serviceParams = params;
            serviceParams.pids = pids.empty() ? "8192" : pids;
            item["input_uri"] = DvbSatellite::buildUri(serviceParams);
            services.append(item);
        }
        result["services"] = services;
        result["service_count"] = services.size();
    }
    return result;
}

} // namespace

namespace DvbSatellite {

std::unique_lock<std::mutex> acquireFrontendTuneGuard(const DvbSatelliteParams& params) {
    return std::unique_lock<std::mutex>(frontendTuneMutex(params));
}

bool isDvbUri(const std::string& uri) {
    return lower(uri).rfind("dvb://", 0) == 0;
}

bool parseUri(const std::string& uri, DvbSatelliteParams& params, std::string& error) {
    if (!isDvbUri(uri)) {
        error = "Not a DVB URI";
        return false;
    }
    const auto query = parseQuery(uri);
    if (!parseInt(query, "adapter", params.adapter) ||
        !parseInt(query, "frontend", params.frontend) ||
        !parseUInt(query, "frequency_khz", params.frequencyKHz) ||
        !parseUInt(query, "symbol_rate", params.symbolRateK) ||
        !parseInt(query, "diseqc_source", params.diseqcSource) ||
        !parseUInt(query, "lnb_lof1_khz", params.lnbLof1KHz) ||
        !parseUInt(query, "lnb_lof2_khz", params.lnbLof2KHz) ||
        !parseUInt(query, "lnb_slof_khz", params.lnbSlofKHz) ||
        !parseInt(query, "unicable_user_band", params.unicableUserBand) ||
        !parseUInt(query, "unicable_frequency_khz", params.unicableFrequencyKHz) ||
        !parseInt(query, "unicable_position", params.unicablePosition) ||
        !parseInt(query, "unicable_pin", params.unicablePin) ||
        !parseInt(query, "stream_id", params.streamId)) {
        error = "Invalid DVB URI numeric parameter";
        return false;
    }
    if (const auto it = query.find("polarity"); it != query.end()) params.polarity = upper(it->second);
    if (const auto it = query.find("delivery_system"); it != query.end()) params.deliverySystem = lower(it->second);
    if (const auto it = query.find("modulation"); it != query.end()) params.modulation = lower(it->second);
    if (const auto it = query.find("fec"); it != query.end()) params.fec = lower(it->second);
    if (const auto it = query.find("unicable_mode"); it != query.end()) params.unicableMode = lower(it->second);
    if (const auto it = query.find("pids"); it != query.end() && !it->second.empty()) params.pids = it->second;
    if (params.unicableMode == "none") {
        params.unicableUserBand = -1;
        params.unicableFrequencyKHz = 0;
        params.unicablePosition = 0;
        params.unicablePin = -1;
    }

    if (params.frequencyKHz < 900000 || params.frequencyKHz > 14000000 ||
        params.symbolRateK < 100 || params.symbolRateK > 60000 ||
        (params.polarity != "H" && params.polarity != "V") ||
        (params.deliverySystem != "dvb-s" && params.deliverySystem != "dvb-s2")) {
        error = "Invalid DVB-S/S2 tuning parameters";
        return false;
    }
    return validateUnicableParams(params, error);
}

std::string buildUri(const DvbSatelliteParams& params) {
    std::ostringstream uri;
    uri << "dvb://satellite?adapter=" << params.adapter
        << "&frontend=" << params.frontend
        << "&frequency_khz=" << params.frequencyKHz
        << "&symbol_rate=" << params.symbolRateK
        << "&polarity=" << percentEncode(params.polarity)
        << "&delivery_system=" << percentEncode(params.deliverySystem)
        << "&modulation=" << percentEncode(params.modulation)
        << "&fec=" << percentEncode(params.fec)
        << "&diseqc_source=" << params.diseqcSource
        << "&lnb_lof1_khz=" << params.lnbLof1KHz
        << "&lnb_lof2_khz=" << params.lnbLof2KHz
        << "&lnb_slof_khz=" << params.lnbSlofKHz
        << "&unicable_mode=" << percentEncode(params.unicableMode)
        << "&unicable_user_band=" << params.unicableUserBand
        << "&unicable_frequency_khz=" << params.unicableFrequencyKHz
        << "&unicable_position=" << params.unicablePosition
        << "&unicable_pin=" << params.unicablePin
        << "&stream_id=" << params.streamId
        << "&pids=" << percentEncode(params.pids.empty() ? "8192" : params.pids);
    return uri.str();
}

Json::Value adapters() {
    Json::Value root;
    Json::Value list(Json::arrayValue);
#ifdef __linux__
    constexpr bool nativeDvbAvailable = true;
#else
    constexpr bool nativeDvbAvailable = false;
#endif
    const std::filesystem::path base("/dev/dvb");
    std::error_code ec;
    if (std::filesystem::exists(base, ec) && !ec) {
        for (const auto& adapterEntry : std::filesystem::directory_iterator(base, ec)) {
            if (ec || !adapterEntry.is_directory()) continue;
            const std::string adapterName = adapterEntry.path().filename().string();
            if (adapterName.rfind("adapter", 0) != 0) continue;
            int adapter = -1;
            try { adapter = std::stoi(adapterName.substr(7)); } catch (...) { continue; }
            for (const auto& frontendEntry : std::filesystem::directory_iterator(adapterEntry.path(), ec)) {
                if (ec) break;
                const std::string frontendName = frontendEntry.path().filename().string();
                if (frontendName.rfind("frontend", 0) != 0) continue;
                int frontend = -1;
                try { frontend = std::stoi(frontendName.substr(8)); } catch (...) { continue; }
                Json::Value item;
                item["adapter"] = adapter;
                item["frontend"] = frontend;
                item["device"] = frontendEntry.path().string();
                const FrontendMetadata metadata = readFrontendMetadata(adapter, frontend);
                item["frontend_name"] = metadata.name;
                item["model"] = metadata.model;
                item["driver_module"] = metadata.driverModule;
                item["driver_mode"] = metadata.driverMode;
                item["driver_mode_name"] = driverModeName(metadata.driverMode);
                item["tbs6909_family"] =
                    lower(metadata.model).find("tbs6909") != std::string::npos;
                list.append(item);
            }
        }
    }
    root["adapters"] = list;
    root["available"] = !list.empty();
    root["native_dvb_available"] = nativeDvbAvailable;
    root["native_stream_id_supported"] = true;
    root["native_diseqc_source_supported"] = true;
    root["native_unicable_supported"] = true;
    return root;
}

Json::Value scan(const Json::Value& request) {
    std::string error;
    const DvbSatelliteParams params = paramsFromJson(request, error);
    if (!error.empty()) {
        Json::Value result;
        result["error"] = error;
        return result;
    }
    const bool holdLock = request.get("hold_lock", false).asBool();
    const int timeoutMs = holdLock ? 18000 : 6500;
    Json::Value result = runTune(params, true, timeoutMs);
    result["hold_lock"] = holdLock;
    result["scan_timeout_ms"] = timeoutMs;
    result["input_uri"] = buildUri(params);
    result["frequency_mhz"] = static_cast<double>(params.frequencyKHz) / 1000.0;
    result["symbol_rate"] = params.symbolRateK;
    result["polarity"] = params.polarity;
    result["delivery_system"] = params.deliverySystem;
    result["modulation"] = params.modulation;
    result["unicable_mode"] = params.unicableMode;
    result["unicable_user_band"] = params.unicableUserBand;
    result["unicable_frequency_khz"] = params.unicableFrequencyKHz;
    result["unicable_position"] = params.unicablePosition;
    result["unicable_pin"] = params.unicablePin;
    return result;
}

Json::Value signal(const Json::Value& request) {
    std::string error;
    const DvbSatelliteParams params = paramsFromJson(request, error);
    if (!error.empty()) {
        Json::Value result;
        result["error"] = error;
        return result;
    }
    return runTune(params, false, 900);
}

Json::Value signalFromUri(const std::string& uri) {
    Json::Value result;
    DvbSatelliteParams params;
    std::string error;
    if (!parseUri(uri, params, error)) {
        result["error"] = error.empty() ? "Invalid DVB URI" : error;
        result["locked"] = false;
        result["signal"] = 0;
        result["quality"] = 0;
        return result;
    }

    // Important: this path does NOT instantiate dvbsrc and does NOT issue a
    // tune command. It only reads FE status/statistics from the frontend that
    // the live stream pipeline already owns. This prevents the dashboard from
    // retuning or interrupting a channel while refreshing Signal/Quality.
    const FrontendStats stats = readFrontendStats(params);
    result = statsToJson(stats);
    result["adapter"] = params.adapter;
    result["frontend"] = params.frontend;
    result["stream_id"] = params.streamId;
    result["diseqc_source"] = params.diseqcSource;
    result["unicable_mode"] = params.unicableMode;
    result["unicable_user_band"] = params.unicableUserBand;
    result["unicable_frequency_khz"] = params.unicableFrequencyKHz;
    result["unicable_position"] = params.unicablePosition;
    result["unicable_pin"] = params.unicablePin;
    const FrontendMetadata metadata = readFrontendMetadata(params.adapter, params.frontend);
    result["frontend_name"] = metadata.name;
    result["frontend_model"] = metadata.model;
    result["driver_mode"] = metadata.driverMode;
    result["driver_mode_name"] = driverModeName(metadata.driverMode);
    return result;
}

bool resolveServicePids(const DvbSatelliteParams& params, uint32_t serviceId,
                        std::string& pids, bool& scrambled, std::string& error) {
    pids.clear();
    scrambled = false;
    if (serviceId == 0 || serviceId > 0xFFFF) {
        error = "Invalid DVB service id";
        return false;
    }

    DvbSatelliteParams scanParams = params;
    scanParams.pids = "8192";
    Json::Value result = runTune(scanParams, true, 3200);
    if (result.isMember("services") && result["services"].isArray()) {
        for (const auto& item : result["services"]) {
            if (item.get("service_id", 0).asUInt() != serviceId) continue;
            pids = item.get("service_pids", "").asString();
            scrambled = item.get("scrambled", false).asBool();
            if (!pids.empty()) {
                error.clear();
                return true;
            }
        }
    }
    error = result.get("error", "").asString();
    if (error.empty()) error = "DVB service PMT/PIDs not found for SID " + std::to_string(serviceId);
    return false;
}

} // namespace DvbSatellite


namespace {
int nitBcd(const std::uint8_t* p, std::size_t digits) {
    int value = 0;
    for (std::size_t i=0;i<digits;++i) {
        const std::uint8_t v=p[i/2U];
        const int digit=(i%2U==0U)?(v>>4U):(v&0x0fU);
        if(digit>9) return -1;
        value=value*10+digit;
    }
    return value;
}
bool nitCrcValid(const std::vector<std::uint8_t>& section) {
    std::uint32_t crc=0xffffffffU;
    for (const std::uint8_t b:section) {
        crc ^= static_cast<std::uint32_t>(b)<<24U;
        for(int bit=0;bit<8;++bit)
            crc=(crc&0x80000000U)?((crc<<1U)^0x04c11db7U):(crc<<1U);
    }
    return crc==0;
}
std::string nitOrbitalLabel(int tenths, bool east) {
    const std::map<int,std::string> knownEast = {
        {48,"Astra 4A"},{70,"Eutelsat 7"},{90,"Eutelsat 9"},
        {130,"Hot Bird"},{160,"Eutelsat 16"},{192,"Astra"},
        {235,"Astra"},{282,"Astra"},{360,"Eutelsat 36"},
        {390,"Hellas Sat"},{420,"Türksat"},{520,"TürkmenÄlem/MonacoSat"},
        {750,"ABS-2"},{850,"Intelsat 15"}
    };
    const std::map<int,std::string> knownWest = {
        {8,"Thor/Intelsat"},{40,"Amos"},{300,"Hispasat"}
    };
    const auto& known=east?knownEast:knownWest;
    const auto match=known.find(tenths);
    std::ostringstream result;
    if(match!=known.end())result<<match->second<<" ";
    result<<(tenths/10)<<"."<<(tenths%10)<<(east?"°E":"°W");
    return result.str();
}
} // namespace

DvbNitSatelliteMonitor::DvbNitSatelliteMonitor(const DvbSatelliteParams& tuned)
    : tuned_(tuned) {
    section_.reserve(1024U);
}

std::string DvbNitSatelliteMonitor::satelliteName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return satelliteName_;
}

void DvbNitSatelliteMonitor::parseSection(const std::vector<std::uint8_t>& s) {
    if(s.size()<16U||s[0]!=0x40U||(s[5]&0x01U)==0U||
       !nitCrcValid(s))return;
    const std::size_t payloadEnd=s.size()-4U;
    const std::size_t networkLen=
        ((static_cast<std::size_t>(s[8]&0x0fU)<<8U)|s[9]);
    const std::size_t loopIndex=10U+networkLen;
    if(loopIndex+2U>payloadEnd)return;
    const std::size_t tsLength=
        (static_cast<std::size_t>(s[loopIndex]&0x0fU)<<8U)|s[loopIndex+1U];
    const std::size_t tsEnd=loopIndex+2U+tsLength;
    if(tsEnd>payloadEnd)return;
    for(std::size_t at=loopIndex+2U;at+6U<=tsEnd;) {
        const std::size_t len=
            (static_cast<std::size_t>(s[at+4U]&0x0fU)<<8U)|s[at+5U];
        const std::size_t end=at+6U+len;
        if(end>tsEnd)return;
        for(std::size_t desc=at+6U;desc+2U<=end;) {
            const std::uint8_t tag=s[desc];
            const std::size_t n=s[desc+1U];
            if(desc+2U+n>end)break;
            if(tag==0x43U&&n>=11U) {
                const std::uint8_t* p=s.data()+desc+2U;
                const int freq=nitBcd(p,8U); // DVB: BCD units of 10 kHz
                const int symbol=nitBcd(p+7U,7U); // DVB: BCD units of 100 baud
                const int orbit=nitBcd(p+4U,4U); // tenths of degree
                const int polarity=(p[6U]>>5U)&0x03U;
                const bool east=(p[6U]&0x80U)!=0U;
                const int configuredPol=tuned_.polarity=="H"?0:1;
                // An NIT can describe many transponders and satellites.
                // Never name the tuned input from a nonmatching record.
                if(freq>=0&&symbol>=0&&orbit>=0&&
                   polarity==configuredPol&&
                   std::llabs(static_cast<long long>(freq)*10LL-
                              tuned_.frequencyKHz)<=1000LL&&
                   std::llabs(static_cast<long long>(symbol)/10LL-
                              tuned_.symbolRateK)<=50LL) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    satelliteName_=nitOrbitalLabel(orbit,east);
                    return;
                }
            }
            desc+=n+2U;
        }
        at=end;
    }
}

void DvbNitSatelliteMonitor::consume(const std::uint8_t* data,std::size_t length) {
    while(length>0U) {
        if(section_.empty()&&data[0]==0xffU)return;
        if(expected_==0U) {
            const std::size_t need=3U-section_.size();
            const std::size_t take=std::min(need,length);
            section_.insert(section_.end(),data,data+take);
            data+=take;length-=take;
            if(section_.size()<3U)return;
            expected_=3U+
                ((static_cast<std::size_t>(section_[1]&0x0fU)<<8U)|section_[2]);
            if(expected_<16U||expected_>4096U) {
                section_.clear();expected_=0U;
                return;
            }
        }
        const std::size_t take=std::min(length,expected_-section_.size());
        section_.insert(section_.end(),data,data+take);
        data+=take;length-=take;
        if(section_.size()==expected_) {
            parseSection(section_);
            section_.clear();expected_=0U;
            if(!satelliteName().empty())return; // Already resolved for this source.
        }
    }
}

void DvbNitSatelliteMonitor::push(const std::uint8_t* data,std::size_t size) {
    if(!data||size<188U||!satelliteName().empty())return;
    // The shared receiver supplies aligned full-MPTS chunks. Inspect only
    // NIT PID 0x0010; no frontend, tuning or conditional-access operations.
    for(std::size_t at=0;at+188U<=size;at+=188U) {
        const auto* p=data+at;
        if(p[0]!=0x47U||(p[1]&0x1fU)!=0U||p[2]!=0x10U||
           (p[1]&0x80U)!=0U)continue;
        const std::uint8_t afc=(p[3]>>4U)&0x03U;
        if((afc&0x01U)==0U)continue;
        std::size_t off=4U;
        if((afc&0x02U)!=0U)off+=1U+p[4];
        if(off>=188U)continue;
        const std::uint8_t cc=p[3]&0x0fU;
        if(continuityKnown_ && cc != ((continuity_+1U)&0x0fU)) {
            section_.clear();expected_=0U;
        }
        continuity_=cc;continuityKnown_=true;
        if((p[1]&0x40U)!=0U) {
            const std::size_t pointer=p[off++];
            if(pointer>188U-off) {section_.clear();expected_=0U;continue;}
            if(!section_.empty()&&pointer>0U)consume(p+off,pointer);
            section_.clear();expected_=0U;
            off+=pointer;
        } else if(section_.empty())continue;
        if(off<188U)consume(p+off,188U-off);
        if(!satelliteName().empty())return;
    }
}
