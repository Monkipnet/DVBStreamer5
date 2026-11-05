#include "ConfigManager.h"
#include "media/DvbSourcePcrPacingPolicy.h"

#include <cstdlib>
#include <iostream>

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

int main() {
    using dvbstreamer5::media::network::enableDvbSourcePcrPacketPacing;
    Json::Value legacy;
    legacy["id"] = "satellite-test";
    legacy["input_uri"] = "dvb://adapter0?service=1";
    legacy["target_bitrate"] = Json::UInt64(3000808ULL);
    auto config = StreamConfig::fromJson(legacy);
    require(!config.dvbSourcePcrPacing, "legacy configuration enabled the new scheduler");
    config.dvbSourcePcrPacing = true;
    auto saved = config.toJson();
    require(saved["dvb_source_pcr_pacing"].asBool(), "opt-in not saved");
    require(saved["target_bitrate"].asUInt64() == 3000808ULL, "PCR/buffer/bitrate preference changed");
    auto restored = StreamConfig::fromJson(saved);
    require(restored.dvbSourcePcrPacing && restored.id == config.id, "opt-in round trip failed");
    restored.dvbSourcePcrPacing = false;
    require(!StreamConfig::fromJson(restored.toJson()).dvbSourcePcrPacing, "rollback not saved");
    require(!StreamConfig::fromJson(legacy).dvbSourcePcrPacing, "unrelated stream enabled");

    const auto eligible = [](bool enabled, bool dvb, bool transcode, bool cbr,
                             bool udp, bool test, std::uint64_t target) {
        return enableDvbSourcePcrPacketPacing(enabled,dvb,transcode,cbr,udp,test,target);
    };
    require(eligible(true,true,false,true,true,false,3000808), "requested DVB profile disabled");
    require(!eligible(false,true,false,true,true,false,3000808), "default-off policy");
    require(!eligible(true,false,false,true,true,false,3000808), "IP/file input changed");
    require(!eligible(true,true,true,true,true,false,3000808), "transcoder changed");
    require(!eligible(true,true,false,false,true,false,3000808), "non-CBR changed");
    require(!eligible(true,true,false,true,false,false,3000808), "SRT-only changed");
    require(!eligible(true,true,false,true,true,true,3000808), "test pattern changed");
    for (const auto target : {3000000ULL,3000708ULL,3000699ULL,3000799ULL})
        require(!eligible(true,true,false,true,true,false,target), "fixed/zero-buffer profile changed");
    std::cout << "DVB Source PCR config/eligibility: OK\n";
}
