#pragma once
// V590: DVB frontend sharing identity.
// DVB-T/T2 tuning does not use satellite FEC, symbol rate, LNB, DiSEqC
// or Unicable settings. Terrestrial FEC is forced AUTO by LinuxDvbInput;
// modulation may be "auto" or an explicit description of the same locked mux.
// A physical terrestrial source is keyed by frontend, RF frequency,
// bandwidth, delivery system and selected T2 PLP (DTV_STREAM_ID).
// Never merge different PLPs, DVB-T vs DVB-T2 or different frequencies.
#include "media/LinuxDvbInput.h"
#include <sstream>
#include <string>

namespace dvbstreamer5::media::network {

inline bool isTerrestrialDvbSystem(const std::string& system) {
    return system == "dvb-t" || system == "dvb-t2";
}

inline std::string sharedDvbTransponderKey(const LinuxDvbTuneConfig& config) {
    std::ostringstream out;
    out << config.adapter << ':' << config.frontend
        << "|rf=" << config.frequencyKHz;
    if (isTerrestrialDvbSystem(config.deliverySystem)) {
        // The active frontend already has a transport lock. Per-service
        // output protocol and PID selection do not affect physical tuning.
        // Keep -1 ("auto PLP") different from explicit PLP 0, as the driver
        // might select a different PLP when left unspecified.
        out << "|bw=" << config.bandwidthHz
            << "|sys=" << config.deliverySystem
            << "|plp=" << config.streamId;
        return out.str();
    }
    // Preserve exact satellite identity and all Unicable safety constraints.
    out << "|sr=" << config.symbolRateK
        << "|bw=" << config.bandwidthHz
        << "|pol=" << config.polarity
        << "|sys=" << config.deliverySystem
        << "|mod=" << config.modulation
        << "|fec=" << config.fec
        << "|diseqc=" << config.diseqcSource
        << "|lof1=" << config.lnbLof1KHz
        << "|lof2=" << config.lnbLof2KHz
        << "|slof=" << config.lnbSlofKHz
        << "|unicable=" << config.unicableMode
        << "|ub=" << config.unicableUserBand
        << "|ubfreq=" << config.unicableFrequencyKHz
        << "|position=" << config.unicablePosition
        << "|pin=" << config.unicablePin
        << "|isi=" << config.streamId;
    return out.str();
}

} // namespace dvbstreamer5::media::network
