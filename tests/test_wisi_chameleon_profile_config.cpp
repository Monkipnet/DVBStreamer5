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
    // An old checked/unchecked UI setting must no longer affect runtime
    // compatibility or reappear in new JSON after upgrade.
    for (const bool oldFlag : {false, true}) {
        legacy["wisi_chameleon_profile"]=oldFlag;
        const auto parsed=StreamConfig::fromJson(legacy);
        const auto stored=parsed.toJson();
        check(!stored.isMember("wisi_chameleon_profile"),
              "removed UI switch was written back to stream JSON");
        check(stored["target_bitrate"].asUInt64()==8000000ULL,
              "transport bitrate changed during migration");
        check(stored["transcode_video_bitrate"].asUInt64()==6000000ULL,
              "video bitrate changed during migration");
        const auto restored=StreamConfig::fromJson(stored);
        check(WisiChameleonProfilePolicy::cbrEligible(
            restored.transcodeEnabled, restored.cbr, true,
            restored.transcodeVideoCodec),
            "saved legacy WISI checkbox disabled automatic CBR");
        check(WisiChameleonProfilePolicy::vbrEligible(
            restored.transcodeEnabled, true,
            restored.transcodeVideoCodec),
            "saved legacy WISI checkbox disabled automatic VBR");
        check(!WisiChameleonProfilePolicy::cbrEligible(
            restored.transcodeEnabled, restored.cbr, true, "copy"),
            "Copy must never acquire the encoder profile");
    }
    std::cout << "V518 automatic WISI stream-config migration: OK\n";
}
