#include "media/NativeHlsSegmenter.h"
#include "media/TransportStream.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

using dvbstreamer5::media::mpegts::Packet;

static Packet packetAt(std::uint64_t t) {
    Packet p{};
    p.fill(0xff);
    p[0]=0x47; p[1]=0x41; p[2]=0x00; p[3]=0x30;
    p[4]=7; p[5]=0x50; // PCR + random_access_indicator
    p[6]=static_cast<std::uint8_t>(t>>25U);
    p[7]=static_cast<std::uint8_t>(t>>17U);
    p[8]=static_cast<std::uint8_t>(t>>9U);
    p[9]=static_cast<std::uint8_t>(t>>1U);
    p[10]=static_cast<std::uint8_t>((t&1ULL)<<7U);
    p[11]=0;
    // A minimal synthetic video PES; only the HLS segmentation state is tested.
    const std::array<std::uint8_t,15> pes = {
        0,0,1,0xe0,0,0,0x80,0,0,0,0,1,0xb3,0x2d,2
    };
    std::copy(pes.begin(),pes.end(),p.begin()+12);
    return p;
}
static std::string readText(const std::filesystem::path& p) {
    std::ifstream f(p,std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}
int main() {
    const auto dir=std::filesystem::temp_directory_path()/
        "dvbstreamer5-hls-failover-v350-test";
    std::error_code ec;
    std::filesystem::remove_all(dir,ec);
    dvbstreamer5::media::hls::NativeHlsSegmenter s;
    dvbstreamer5::media::hls::NativeHlsSegmenterConfig cfg;
    cfg.directory=dir;
    cfg.durationExplicit=true;
    cfg.targetDurationSeconds=1;
    cfg.liveWindowSegments=4;
    cfg.countExplicit=true;
    cfg.independentSegments=true;
    std::string error;
    if(!s.start(cfg,error)){std::cerr<<error<<"\n";return 1;}
    for(unsigned i=0;i<4;++i){
        auto p=packetAt(i*90000ULL);
        if(!s.push(p.data(),p.size()))return 2;
    }
    const auto initial=s.segmentCount();
    if(initial<2)return 3;
    if(!s.sourceSwitched())return 4;
    if(readText(dir/"video.m3u8").find("#EXT-X-ENDLIST")!=std::string::npos)return 5;
    for(unsigned i=0;i<3;++i){
        auto p=packetAt(i*90000ULL);
        if(!s.push(p.data(),p.size()))return 6;
    }
    const auto backup=s.segmentCount();
    const auto list1=readText(dir/"video.m3u8");
    if(backup<=initial||list1.find("#EXT-X-DISCONTINUITY\n")==std::string::npos||
       list1.find("#EXT-X-ENDLIST")!=std::string::npos){
        std::cerr<<"BACKUP playlist: "<<list1<<"\n";return 7;
    }
    if(!s.sourceSwitched())return 8;
    for(unsigned i=0;i<3;++i){
        auto p=packetAt(i*90000ULL);
        if(!s.push(p.data(),p.size()))return 9;
    }
    const auto restored=s.segmentCount();
    const auto list2=readText(dir/"video.m3u8");
    if(restored<=backup||list2.find("#EXT-X-DISCONTINUITY\n")==std::string::npos||
       list2.find("#EXT-X-ENDLIST")!=std::string::npos||
       list2.find("#EXT-X-DISCONTINUITY-SEQUENCE:")==std::string::npos){
        std::cerr<<"FAILBACK playlist: "<<list2<<"\n";return 10;
    }
    s.stop();
    if(readText(dir/"video.m3u8").find("#EXT-X-ENDLIST")==std::string::npos)return 11;
    std::filesystem::remove_all(dir,ec);

    // V530: an OnDemand transcoded HLS player must not receive a manifest
    // containing only the first 1–2 seconds of media. Prefill three CLOSED
    // IDR-aligned chunks so VLC can start behind the live edge. This is
    // opt-in: the legacy test above still uses an immediate playlist.
    const auto prefillDir=std::filesystem::temp_directory_path()/
        "dvbstreamer5-hls-ondemand-prefill-v530-test";
    std::filesystem::remove_all(prefillDir,ec);
    dvbstreamer5::media::hls::NativeHlsSegmenter prefill;
    auto prefillConfig=cfg;
    prefillConfig.directory=prefillDir;
    prefillConfig.startupPrefillSegments=3U;
    if(!prefill.start(prefillConfig,error))return 12;
    bool published=false;
    for(unsigned i=0;i<10;++i){
        const auto p=packetAt(i*90000ULL);
        if(!prefill.push(p.data(),p.size()))return 13;
        const auto completed=prefill.segmentCount();
        const bool hasManifest=std::filesystem::exists(prefillDir/"video.m3u8");
        if(completed<3U && hasManifest)return 14;
        if(completed>=3U){
            if(!hasManifest)return 15;
            const auto playlist=readText(prefillDir/"video.m3u8");
            std::size_t count=0,offset=0;
            while((offset=playlist.find("#EXTINF:",offset))!=std::string::npos){
                ++count;
                offset+=8;
            }
            if(count<3U||playlist.find("#EXT-X-ENDLIST")!=std::string::npos)
                return 16;
            published=true;
            break;
        }
    }
    if(!published)return 17;
    prefill.stop();
    std::filesystem::remove_all(prefillDir,ec);
    std::cout<<"PASS: HLS failover and OnDemand three-segment prefill\n";
    return 0;
}
