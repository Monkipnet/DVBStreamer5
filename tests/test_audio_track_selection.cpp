#include "media/MpegTsRemapper.h"
#include "DvbSubtitlePassthrough.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace dvbstreamer5::media::mpegts;

static void check(bool ok,const char* why) {
    if(!ok) { std::cerr<<"FAIL audio-track selection: "<<why<<"\n";std::exit(1); }
}
static uint16_t pidOf(const Packet& p) {
    return static_cast<uint16_t>(((p[1]&31U)<<8U)|p[2]);
}
static uint32_t crc32Section(const uint8_t* b,size_t n){
    uint32_t c=0xffffffffU;
    for(size_t i=0;i<n;++i){
        c^=static_cast<uint32_t>(b[i])<<24U;
        for(int bit=0;bit<8;++bit)c=(c&0x80000000U)?(c<<1U)^0x04c11db7U:c<<1U;
    }
    return c;
}
static void appendCrc(std::vector<uint8_t>& b){
    auto c=crc32Section(b.data(),b.size());
    b.push_back(static_cast<uint8_t>(c>>24U));
    b.push_back(static_cast<uint8_t>(c>>16U));
    b.push_back(static_cast<uint8_t>(c>>8U));
    b.push_back(static_cast<uint8_t>(c));
}
static Packet ts(uint16_t pid,uint8_t cc=0){
    Packet p{};p.fill(0xffU);
    p[0]=0x47;p[1]=static_cast<uint8_t>((pid>>8U)&31U);
    p[2]=static_cast<uint8_t>(pid);
    p[3]=static_cast<uint8_t>(0x10U|(cc&15U));
    return p;
}
static Packet section(uint16_t pid,const std::vector<uint8_t>& body){
    check(body.size()<=183U,"fixture PMT must fit one TS packet");
    Packet p=ts(pid);
    p[1]|=0x40U;p[4]=0;
    std::copy(body.begin(),body.end(),p.begin()+5);
    return p;
}
static std::vector<uint8_t> pat(){
    std::vector<uint8_t> body{
        0x00,0xb0,0x0d,0x00,0x02,0xc1,0,0,
        0,37,0xf0,0x00
    };
    appendCrc(body);return body;
}
static void es(std::vector<uint8_t>& b,uint8_t type,uint16_t pid,
               const char* language=nullptr){
    const bool hasLang=language!=nullptr;
    b.push_back(type);
    b.push_back(static_cast<uint8_t>(0xe0U|(pid>>8U)));
    b.push_back(static_cast<uint8_t>(pid));
    b.push_back(0xf0U);
    b.push_back(hasLang?6U:0U);
    if(hasLang){
        b.push_back(0x0a);b.push_back(4);
        b.push_back(static_cast<uint8_t>(language[0]));
        b.push_back(static_cast<uint8_t>(language[1]));
        b.push_back(static_cast<uint8_t>(language[2]));
        b.push_back(0);
    }
}
static std::vector<uint8_t> pmt(){
    std::vector<uint8_t> b{
        0x02,0xb0,0,0,37,0xc1,0,0,0xe1,0x00,0xf0,0
    };
    es(b,0x1b,0x100);
    es(b,0x03,0x110,"eng");
    es(b,0x04,0x111,"rus");
    const auto len=b.size()-3U+4U;
    b[1]|=static_cast<uint8_t>((len>>8U)&15U);
    b[2]=static_cast<uint8_t>(len);
    appendCrc(b);return b;
}
static std::vector<uint16_t> pmtAudioPids(const Packet& p){
    check(pidOf(p)==0x1000,"expected PMT");
    const size_t start=5;
    const size_t total=3U+(((p[start+1]&15U)<<8U)|p[start+2]);
    check(total<184U&&crc32Section(p.data()+start,total)==0,"valid CRC");
    const auto end=start+total-4U;
    size_t at=start+12U+(((p[start+10]&15U)<<8U)|p[start+11]);
    std::vector<uint16_t> audio;
    while(at+5U<=end){
        const uint8_t type=p[at];
        const uint16_t pid=static_cast<uint16_t>(((p[at+1]&31U)<<8U)|p[at+2]);
        const auto len=static_cast<size_t>(((p[at+3]&15U)<<8U)|p[at+4]);
        if(type==0x03U||type==0x04U)audio.push_back(pid);
        at+=5U+len;
    }
    check(at==end,"complete PMT ES list");
    return audio;
}
static const Packet& find(const std::vector<Packet>& out,uint16_t pid){
    for(const auto& p:out)if(pidOf(p)==pid)return p;
    std::cerr<<"missing output PID "<<pid<<"\n";std::exit(1);
}

static void selectedAudio(bool remapped) {
    Remapper mapper;
    RemapConfig cfg;
    cfg.inputServiceId=37;
    cfg.outputServiceId=remapped?52:37;
    cfg.inputAudioPid=0x111;
    cfg.outputAudioPid=remapped?0x201:0;
    cfg.preserveSourceServiceId=!remapped;
    std::string error;
    check(mapper.initialize(cfg,error),"remapper init");
    std::vector<Packet> out;
    check(mapper.process(section(0,pat()),out,error),"PAT");
    out.clear();
    check(mapper.process(section(0x1000,pmt()),out,error),"PMT");
    mapper.tick(out);
    const auto audios=pmtAudioPids(find(out,0x1000));
    check(audios.size()==1U,"only selected audio advertised");
    check(audios[0]==(remapped?0x201U:0x111U),"output audio PID correct");
    check(!mapper.wantsInputPid(0x110),"other language never enters from DVB full MPTS");
    check(mapper.wantsInputPid(0x111),"selected language admitted");
    out.clear();
    check(mapper.process(ts(0x110),out,error),"unused audio input accepted");
    check(std::none_of(out.begin(),out.end(),
        [](const Packet& p){return pidOf(p)==0x110;}),"unused audio dropped");
    out.clear();
    check(mapper.process(ts(0x111),out,error),"selected audio input accepted");
    check(std::any_of(out.begin(),out.end(),
        [&](const Packet& p){return pidOf(p)==(remapped?0x201U:0x111U);}),
        "selected audio forwarded");
    out.clear();
    check(mapper.process(ts(0x100),out,error),"video preserved");
    check(std::any_of(out.begin(),out.end(),
        [](const Packet& p){return pidOf(p)==0x100;}),"video PID preserved");
}
static void outputPidMayEqualUnselectedInputPid() {
    Remapper mapper;
    RemapConfig cfg;
    cfg.inputServiceId=37;
    cfg.outputServiceId=37;
    cfg.inputAudioPid=0x111; // select Russian
    cfg.outputAudioPid=0x110; // output PID formerly used by English source
    std::string error;
    check(mapper.initialize(cfg,error),"collision remapper init");
    std::vector<Packet> out;
    check(mapper.process(section(0,pat()),out,error),"collision PAT");
    out.clear();
    check(mapper.process(section(0x1000,pmt()),out,error),"collision PMT");
    mapper.tick(out);
    const auto audio=pmtAudioPids(find(out,0x1000));
    check(audio.size()==1U&&audio.front()==0x110,"single mapped output PID");
    check(!mapper.wantsInputPid(0x110),"old source English must be rejected");
    check(mapper.wantsInputPid(0x111),"Russian source must be admitted");
    out.clear();
    check(mapper.process(ts(0x110),out,error),"English transport filtered");
    check(std::none_of(out.begin(),out.end(),[](const Packet& p) {
        return pidOf(p)==0x110;
    }),"English audio with same output PID cannot leak into output");
    out.clear();
    check(mapper.process(ts(0x111),out,error),"Russian transport forwarded");
    check(std::any_of(out.begin(),out.end(),[](const Packet& p) {
        return pidOf(p)==0x110;
    }),"Russian audio remapped to 0x110");
}
static void autoAudioUnchanged() {
    Remapper mapper;
    RemapConfig cfg;
    cfg.inputServiceId=37;cfg.outputServiceId=37;
    std::string error;
    check(mapper.initialize(cfg,error),"auto mapper init");
    std::vector<Packet> out;
    check(mapper.process(section(0,pat()),out,error),"auto PAT");
    out.clear();
    check(mapper.process(section(0x1000,pmt()),out,error),"auto PMT");
    mapper.tick(out);
    const auto audio=pmtAudioPids(find(out,0x1000));
    check(audio.size()==2U && audio[0]==0x110 && audio[1]==0x111,
          "legacy Auto must preserve both original audio tracks");
}
static void pmtDetection() {
    DvbSubtitlePassthrough monitor;
    const auto sourcePat=section(0,pat());
    const auto sourcePmt=section(0x1000,pmt());
    monitor.observe(sourcePat.data(),sourcePat.size());
    monitor.observe(sourcePmt.data(),sourcePmt.size());
    check(monitor.hasParsedPmt(),"source PMT scanned");
    const auto tracks=monitor.availableMediaTracks();
    check(tracks.size()==3U,"H264 video and two audio tracks discovered");
    check(tracks[0].codec=="H.264" && tracks[0].kind=="video","source video codec");
    check(tracks[1].pid==0x110 && tracks[1].language=="eng" &&
          tracks[1].codec=="MPEG Audio","english MPEG audio descriptor");
    check(tracks[2].pid==0x111 && tracks[2].language=="rus" &&
          tracks[2].codec=="MPEG Audio","russian MPEG audio descriptor");
}
int main(){
    selectedAudio(false);
    selectedAudio(true);
    outputPidMayEqualUnselectedInputPid();
    autoAudioUnchanged();
    pmtDetection();
    std::cout<<"PASS audio track selector, source PMT languages, codecs and CRC\n";
    return 0;
}
