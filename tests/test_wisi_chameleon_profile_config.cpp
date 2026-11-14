#include "ConfigManager.h"
#include "media/WisiChameleonProfilePolicy.h"

#include <cstdlib>
#include <iostream>

static void check(bool ok, const char* why) {
    if (!ok) { std::cerr << why << '\n'; std::exit(1); }
}
int main() {
    using dvbstreamer5::media::codec::WisiChameleonProfilePolicy;
    Json::Value legacy;
    legacy["id"]="wisi-test";
    legacy["transcode_enabled"]=true;
    legacy["transcode_video_codec"]="h264";
    legacy["transcode_video_encoder"]="vaapi";
    legacy["transcode_video_bitrate"]=Json::UInt64(6000000ULL);
    legacy["transcode_audio_bitrate"]=Json::UInt64(192000ULL);
    legacy["target_bitrate"]=Json::UInt64(8000000ULL);
    legacy["cbr"]=true;
    const auto parsed=StreamConfig::fromJson(legacy);
    check(!parsed.wisiChameleonProfile,"previous streams must remain off");
    auto enabled=parsed;
    enabled.wisiChameleonProfile=true;
    const auto stored=enabled.toJson();
    check(stored["wisi_chameleon_profile"].asBool(),"WISI flag not saved");
    check(stored["target_bitrate"].asUInt64()==8000000ULL,
          "physical WISI bitrate modified");
    check(stored["transcode_video_bitrate"].asUInt64()==6000000ULL,
          "encoder bitrate modified");
    const auto restored=StreamConfig::fromJson(stored);
    check(restored.wisiChameleonProfile,"WISI flag not restored");
    check(WisiChameleonProfilePolicy::cbrEligible(
        restored.transcodeEnabled, restored.cbr,
        true, restored.transcodeVideoCodec),"MIR24 CBR auto profile not eligible");
    check(WisiChameleonProfilePolicy::vbrEligible(
        restored.transcodeEnabled, true, restored.transcodeVideoCodec),
        "MIR24 VBR auto profile not eligible");
    enabled.wisiChameleonProfile=false;
    check(!StreamConfig::fromJson(enabled.toJson()).wisiChameleonProfile,
          "rollback not persisted");
    check(!StreamConfig::fromJson(legacy).wisiChameleonProfile,
          "unrelated config unexpectedly enabled");
    std::cout<<"WISI Chameleon persisted profile: OK\n";
}
