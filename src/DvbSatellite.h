#pragma once

#include <jsoncpp/json/json.h>

#include <cstdint>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

struct DvbSatelliteParams {
    int adapter = 0;
    int frontend = 0;
    uint32_t frequencyKHz = 11727000;
    uint32_t symbolRateK = 27500;
    std::string polarity = "H";
    std::string deliverySystem = "dvb-s2";
    std::string modulation = "auto";
    std::string fec = "auto";
    int diseqcSource = -1;
    uint32_t lnbLof1KHz = 9750000;
    uint32_t lnbLof2KHz = 10600000;
    uint32_t lnbSlofKHz = 11700000;
    std::string unicableMode = "none";
    int unicableUserBand = -1;
    uint32_t unicableFrequencyKHz = 0;
    int unicablePosition = 0;
    int unicablePin = -1;
    int streamId = -1;
    // Colon-separated DVB PID filter. 8192 means full transport stream.
    std::string pids = "8192";
};

// Lightweight NIT-actual observer for a running full-MPTS satellite input.
// Never opens or retunes /dev/dvb. Satellite position is accepted only from
// DVB descriptor 0x43 matching the configured frequency, SR and polarization.
class DvbNitSatelliteMonitor {
public:
    explicit DvbNitSatelliteMonitor(const DvbSatelliteParams& tuned);
    void push(const std::uint8_t* bytes, std::size_t size);
    std::string satelliteName() const;
private:
    void consume(const std::uint8_t* data, std::size_t length);
    void parseSection(const std::vector<std::uint8_t>& section);
    DvbSatelliteParams tuned_;
    std::vector<std::uint8_t> section_;
    std::size_t expected_ = 0;
    std::uint8_t continuity_ = 0;
    bool continuityKnown_ = false;
    mutable std::mutex mutex_;
    std::string satelliteName_;
};

namespace DvbSatellite {

bool isDvbUri(const std::string& uri);
bool parseUri(const std::string& uri, DvbSatelliteParams& params, std::string& error);
std::string buildUri(const DvbSatelliteParams& params);

// Serialize short-lived scan/signal pipelines with a real stream startup on the
// same physical frontend. The guard is released automatically when destroyed.
std::unique_lock<std::mutex> acquireFrontendTuneGuard(const DvbSatelliteParams& params);
Json::Value adapters();
Json::Value scan(const Json::Value& request);
Json::Value signal(const Json::Value& request);
// Read current frontend statistics without retuning it. Safe for live DVB tiles.
Json::Value signalFromUri(const std::string& uri);
// Resolve the PMT/PCR/elementary PIDs for one service so dvbsrc can capture
// the complete service without relying on tsparse program_%u filtering.
bool resolveServicePids(const DvbSatelliteParams& params, uint32_t serviceId,
                        std::string& pids, bool& scrambled, std::string& error);

} // namespace DvbSatellite
