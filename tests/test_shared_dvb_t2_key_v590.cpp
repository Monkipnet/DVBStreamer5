#ifdef NDEBUG
#undef NDEBUG
#endif
#include "media/SharedDvbTuneKey.h"
#include <cassert>
#include <iostream>
#include <map>
#include <string>

using dvbstreamer5::media::network::LinuxDvbTuneConfig;
using dvbstreamer5::media::network::sharedDvbTransponderKey;

int main() {
    LinuxDvbTuneConfig udp1;
    udp1.adapter = 2;
    udp1.frontend = 0;
    udp1.deliverySystem = "dvb-t2";
    udp1.frequencyKHz = 666000;
    udp1.bandwidthHz = 8000000;
    udp1.streamId = 0;
    udp1.modulation = "auto";
    udp1.pids = "8192";

    // Imported/manual terrestrial URIs may retain irrelevant satellite
    // parameters. These must not split the same physical locked mux.
    auto udp2 = udp1;
    udp2.symbolRateK = 6875;
    udp2.polarity = "V";
    udp2.diseqcSource = 4;
    udp2.fec = "3/4";
    udp2.modulation = "qam64";
    udp2.lnbLof1KHz = 10750000;
    udp2.lnbLof2KHz = 11200000;
    udp2.lnbSlofKHz = 11800000;
    udp2.unicableMode = "en50494";
    udp2.unicableUserBand = 2;
    udp2.unicableFrequencyKHz = 1210000;
    udp2.jitterMs = 200;
    udp2.pids = "0:100:200";

    auto http = udp1;
    http.fec = "auto";
    http.modulation = "qam256";
    http.symbolRateK = 0;
    http.lnbLof1KHz = 9500000;
    http.jitterMs = 50;
    http.pids = "0:101:201";

    const auto key = sharedDvbTransponderKey(udp1);
    assert(sharedDvbTransponderKey(udp2) == key);
    assert(sharedDvbTransponderKey(http) == key);

    // No DVB hardware needed: simulate three channel subscriptions against
    // frontend owner admission (two UDP outputs, then one HTTP output).
    std::map<std::string, std::string> owners;
    auto accepts = [&](const LinuxDvbTuneConfig& config) {
        const auto frontend = std::to_string(config.adapter) + ":" +
            std::to_string(config.frontend);
        const auto tune = sharedDvbTransponderKey(config);
        const auto it = owners.find(frontend);
        if (it != owners.end() && it->second != tune) return false;
        owners[frontend] = tune;
        return true;
    };
    assert(accepts(udp1));
    assert(accepts(udp2));
    assert(accepts(http));
    assert(owners.size() == 1);

    // Never silently merge different physical muxes, PLPs or frontends.
    auto changed = udp1;
    changed.frequencyKHz += 8000;
    assert(!accepts(changed));
    changed = udp1;
    changed.bandwidthHz = 7000000;
    assert(!accepts(changed));
    changed = udp1;
    changed.streamId = 1; // independent PLP on the same RF
    assert(!accepts(changed));
    changed = udp1;
    changed.streamId = -1; // automatic PLP selection may not be PLP 0
    assert(!accepts(changed));
    changed = udp1;
    changed.deliverySystem = "dvb-t";
    changed.streamId = -1;
    assert(!accepts(changed));
    changed = udp1;
    changed.frontend = 1; // physically separate frontend
    assert(accepts(changed));

    // Satellite sharing must keep exactly the old tune-key discrimination.
    auto sat = udp1;
    sat.deliverySystem = "dvb-s2";
    sat.frequencyKHz = 11727000;
    sat.symbolRateK = 27500;
    sat.polarity = "H";
    sat.streamId = -1;
    auto satChanged = sat;
    satChanged.diseqcSource = 1;
    assert(sharedDvbTransponderKey(sat) != sharedDvbTransponderKey(satChanged));
    satChanged = sat;
    satChanged.unicableUserBand = 3;
    assert(sharedDvbTransponderKey(sat) != sharedDvbTransponderKey(satChanged));
    satChanged = sat;
    satChanged.symbolRateK = 30000;
    assert(sharedDvbTransponderKey(sat) != sharedDvbTransponderKey(satChanged));
    satChanged = sat;
    satChanged.modulation = "8psk";
    assert(sharedDvbTransponderKey(sat) != sharedDvbTransponderKey(satChanged));
    satChanged = sat;
    satChanged.streamId = 2;
    assert(sharedDvbTransponderKey(sat) != sharedDvbTransponderKey(satChanged));

    std::cout << "PASS V590: 2 UDP + HTTP same DVB-T2 mux accepted; "
                 "other RF/BW/PLP/system denied; DVB-S2 unchanged\n";
    return 0;
}
