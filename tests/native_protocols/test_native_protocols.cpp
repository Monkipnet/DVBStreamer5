#include "media/NativeCmaf.h"
#include "media/NativeMpegTsMux.h"
#include "media/NativeRtspTransport.h"
#include "media/NativeRtmpTransport.h"
#include "media/NativeSampleAes.h"
#include "media/NativeTsDemux.h"

#include <array>
#include <atomic>
#include <arpa/inet.h>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <thread>
#include <vector>

using namespace dvbstreamer5::media;


namespace rtmp_test {

bool readExact(int fd, void* data, std::size_t size) {
    auto* p = static_cast<std::uint8_t*>(data);
    std::size_t off = 0;
    while (off < size) {
        const ssize_t n = ::recv(fd, p + off, size - off, 0);
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

bool writeAll(int fd, const void* data, std::size_t size) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::size_t off = 0;
    while (off < size) {
        const ssize_t n = ::send(fd, p + off, size - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

void be24(std::vector<std::uint8_t>& o, std::uint32_t v) {
    o.push_back(static_cast<std::uint8_t>(v >> 16));
    o.push_back(static_cast<std::uint8_t>(v >> 8));
    o.push_back(static_cast<std::uint8_t>(v));
}
void be32(std::vector<std::uint8_t>& o, std::uint32_t v) {
    o.push_back(static_cast<std::uint8_t>(v >> 24));
    o.push_back(static_cast<std::uint8_t>(v >> 16));
    o.push_back(static_cast<std::uint8_t>(v >> 8));
    o.push_back(static_cast<std::uint8_t>(v));
}
std::uint32_t read24(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 16) |
           (static_cast<std::uint32_t>(p[1]) << 8) | p[2];
}
std::uint32_t read32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

void amfString(std::vector<std::uint8_t>& o, const std::string& v) {
    o.push_back(2); o.push_back(static_cast<std::uint8_t>(v.size() >> 8));
    o.push_back(static_cast<std::uint8_t>(v.size())); o.insert(o.end(), v.begin(), v.end());
}
void amfNumber(std::vector<std::uint8_t>& o, double v) {
    o.push_back(0); std::uint64_t bits = 0; std::memcpy(&bits, &v, sizeof(bits));
    for (int i = 7; i >= 0; --i) o.push_back(static_cast<std::uint8_t>(bits >> (i * 8)));
}
void amfNull(std::vector<std::uint8_t>& o) { o.push_back(5); }

bool sendMessage(int fd, std::uint8_t csid, std::uint8_t type, std::uint32_t streamId,
                 std::uint32_t timestamp, const std::vector<std::uint8_t>& body,
                 std::uint32_t chunkSize = 128) {
    std::size_t off = 0; bool first = true;
    do {
        std::vector<std::uint8_t> h;
        h.push_back(static_cast<std::uint8_t>(((first ? 0 : 3) << 6) | (csid & 0x3f)));
        if (first) {
            be24(h, std::min(timestamp, 0xffffffU)); be24(h, static_cast<std::uint32_t>(body.size())); h.push_back(type);
            h.push_back(static_cast<std::uint8_t>(streamId)); h.push_back(static_cast<std::uint8_t>(streamId >> 8));
            h.push_back(static_cast<std::uint8_t>(streamId >> 16)); h.push_back(static_cast<std::uint8_t>(streamId >> 24));
            if (timestamp >= 0xffffffU) be32(h, timestamp);
        } else if (timestamp >= 0xffffffU) be32(h, timestamp);
        const std::size_t n = std::min<std::size_t>(chunkSize, body.size() - off);
        if (!writeAll(fd, h.data(), h.size()) || (n && !writeAll(fd, body.data() + off, n))) return false;
        off += n; first = false;
    } while (off < body.size());
    return true;
}

struct InState { std::uint32_t length=0, streamId=0, timestamp=0; std::uint8_t type=0; std::vector<std::uint8_t> body; };
struct Message { std::uint8_t type=0; std::uint32_t streamId=0, timestamp=0; std::vector<std::uint8_t> body; };

bool receiveMessage(int fd, std::uint32_t& chunkSize, std::map<std::uint32_t, InState>& states, Message& out) {
    for (;;) {
        std::uint8_t bh=0; if (!readExact(fd,&bh,1)) return false;
        const int fmt=bh>>6; std::uint32_t csid=bh&0x3f;
        if (csid==0) { std::uint8_t x=0; if(!readExact(fd,&x,1))return false; csid=64+x; }
        else if(csid==1){std::uint8_t x[2]{};if(!readExact(fd,x,2))return false;csid=64+x[0]+256U*x[1];}
        auto& st=states[csid];
        if(fmt==0){std::uint8_t h[11]{};if(!readExact(fd,h,sizeof(h)))return false;st.timestamp=read24(h);st.length=read24(h+3);st.type=h[6];st.streamId=h[7]|(h[8]<<8)|(h[9]<<16)|(h[10]<<24);st.body.clear();if(st.timestamp==0xffffffU){std::uint8_t x[4]{};if(!readExact(fd,x,4))return false;st.timestamp=read32(x);}}
        else if(fmt==3){if(st.length==0)return false;} else { return false; }
        const std::size_t n=std::min<std::size_t>(chunkSize,st.length-st.body.size()); const std::size_t old=st.body.size();st.body.resize(old+n);if(n&&!readExact(fd,st.body.data()+old,n))return false;
        if(st.body.size()==st.length){out={st.type,st.streamId,st.timestamp,st.body};if(st.type==1&&st.body.size()>=4)chunkSize=read32(st.body.data());return true;}
    }
}

bool commandNameTx(const Message& m, std::string& name, double& tx) {
    if (m.type != 20 || m.body.size() < 4 || m.body[0] != 2) return false;
    const std::size_t n=(m.body[1]<<8)|m.body[2]; if(3+n+9>m.body.size())return false;
    name.assign(reinterpret_cast<const char*>(m.body.data()+3),n); const std::size_t p=3+n;if(m.body[p]!=0)return false;
    std::uint64_t bits=0;for(int i=0;i<8;++i)bits=(bits<<8)|m.body[p+1+i];std::memcpy(&tx,&bits,8);return true;
}

bool serverHandshake(int fd) {
    std::array<std::uint8_t,1537> c{}; if(!readExact(fd,c.data(),c.size())||c[0]!=3)return false;
    std::vector<std::uint8_t> s(3073,0);s[0]=3;std::copy(c.begin()+1,c.end(),s.begin()+1537);if(!writeAll(fd,s.data(),s.size()))return false;
    std::array<std::uint8_t,1536> c2{};return readExact(fd,c2.data(),c2.size());
}

int makeListener(int port) {
    int fd=::socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)return -1;int one=1;::setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=htons(static_cast<uint16_t>(port));if(::bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))||::listen(fd,2)){::close(fd);return -1;}return fd;
}

bool serviceCommands(int fd, bool publish, std::atomic<bool>& readyForMedia) {
    if(!serverHandshake(fd))return false;std::uint32_t chunk=128;std::map<std::uint32_t,InState>states;bool connected=false,created=false;
    for(int guard=0;guard<20;++guard){Message m;if(!receiveMessage(fd,chunk,states,m))return false;if(m.type==1)continue;std::string name;double tx=0;if(!commandNameTx(m,name,tx))continue;
        if(name=="connect"){std::vector<std::uint8_t>b;amfString(b,"_result");amfNumber(b,1);amfNull(b);if(!sendMessage(fd,3,20,0,0,b))return false;connected=true;}
        else if(name=="createStream"){std::vector<std::uint8_t>b;amfString(b,"_result");amfNumber(b,tx);amfNull(b);amfNumber(b,1);if(!sendMessage(fd,3,20,0,0,b))return false;created=true;}
        else if((publish&&name=="publish")||(!publish&&name=="play")){readyForMedia.store(true);return connected&&created;}
    }return false;
}

bool testPublish(const std::vector<std::uint8_t>& ts) {
    constexpr int port=19350;int l=makeListener(port);if(l<0)return false;std::atomic<bool>media{false};std::atomic<bool>ok{false};
    std::thread server([&]{int c=::accept4(l,nullptr,nullptr,SOCK_CLOEXEC);if(c<0)return;if(!serviceCommands(c,true,media)){::close(c);return;}std::uint32_t chunk=128;std::map<std::uint32_t,InState>st;for(int i=0;i<20;++i){Message m;if(!receiveMessage(c,chunk,st,m))break;if(m.type==8||m.type==9){ok.store(true);break;}}::close(c);});
    rtmp::NativeRtmpOutput out;rtmp::EndpointConfig cfg;cfg.uri="rtmp://127.0.0.1:"+std::to_string(port)+"/live/test";std::string e;bool started=out.start(cfg,{},e);if(started)out.push(ts.data(),ts.size());for(int i=0;i<30&&!ok.load();++i)std::this_thread::sleep_for(std::chrono::milliseconds(50));out.stop();::shutdown(l,SHUT_RDWR);::close(l);server.join();if(!started)std::cerr<<"rtmp publish start: "<<e<<"\n";return started&&ok.load();
}

bool testPlay() {
    constexpr int port=19351;int l=makeListener(port);if(l<0)return false;std::atomic<bool>ready{false};
    std::thread server([&]{int c=::accept4(l,nullptr,nullptr,SOCK_CLOEXEC);if(c<0)return;if(!serviceCommands(c,false,ready)){::close(c);return;}
        const std::vector<std::uint8_t>sps={0x67,0x42,0x00,0x1e,0x95,0xa8,0x14,0x01,0x6e,0x9b,0x80,0x80,0x80,0xa0};const std::vector<std::uint8_t>pps={0x68,0xce,0x3c,0x80};
        std::vector<std::uint8_t>cfg{0x17,0,0,0,0,1,sps[1],sps[2],sps[3],0xff,0xe1};cfg.push_back(static_cast<std::uint8_t>(sps.size()>>8));cfg.push_back(static_cast<std::uint8_t>(sps.size()));cfg.insert(cfg.end(),sps.begin(),sps.end());cfg.push_back(1);cfg.push_back(0);cfg.push_back(static_cast<std::uint8_t>(pps.size()));cfg.insert(cfg.end(),pps.begin(),pps.end());sendMessage(c,6,9,1,1000,cfg);
        const std::vector<std::uint8_t>idr={0x65,0x88,0x84,0x21,0xa0,0x01,0x02,0x03};std::vector<std::uint8_t>v{0x17,1,0,0,0};be32(v,static_cast<std::uint32_t>(idr.size()));v.insert(v.end(),idr.begin(),idr.end());sendMessage(c,6,9,1,1040,v);std::this_thread::sleep_for(std::chrono::milliseconds(300));::close(c);});
    std::vector<std::uint8_t>got;rtmp::NativeRtmpInput in;rtmp::EndpointConfig cfg;cfg.uri="rtmp://127.0.0.1:"+std::to_string(port)+"/live/test";cfg.ioTimeoutMs=1000;std::string e;const bool started=in.start(cfg,[&](const std::uint8_t*d,std::size_t n){got.insert(got.end(),d,d+n);return true;},{},e);for(int i=0;i<40&&got.empty();++i)std::this_thread::sleep_for(std::chrono::milliseconds(50));in.stop();::shutdown(l,SHUT_RDWR);::close(l);server.join();if(!started)std::cerr<<"rtmp play start: "<<e<<"\n";return started&&!got.empty();
}

} // namespace rtmp_test

static bool makeTs(std::vector<std::uint8_t>& out) {
    mpegts::NativeMpegTsMux mux; mpegts::NativeMuxConfig cfg; cfg.serviceId=100; cfg.videoPid=0x100; cfg.audioPid=0x101;
    std::string e; if(!mux.initialize(cfg,e)||!mux.setCodec(mpegts::ElementaryKind::Video,mpegts::ElementaryCodec::H264,e)||!mux.setCodec(mpegts::ElementaryKind::Audio,mpegts::ElementaryCodec::AacAdts,e)){std::cerr<<e<<"\n";return false;}
    // Baseline SPS/PPS + IDR. The native mux/demux treats them as opaque access units.
    std::vector<std::uint8_t> video={0,0,0,1,0x67,0x42,0x00,0x1e,0x95,0xa8,0x14,0x01,0x6e,0x9b,0x80,0x80,0x80,0xa0,0,0,0,1,0x68,0xce,0x3c,0x80,0,0,0,1,0x65,0x88,0x84,0x21,0xa0};
    for (int i=0;i<256;++i) video.push_back(static_cast<std::uint8_t>(0x40+(i%47)));
    // AAC-LC 48kHz stereo ADTS, tiny opaque payload.
    const std::vector<std::uint8_t> audio={0xff,0xf1,0x4c,0x80,0x02,0x9f,0xfc,1,2,3,4,5,6,7,8,9,10,11,12,13};
    for(int i=0;i<8;++i){
        mpegts::ElementarySample v{video.data(),video.size(),static_cast<std::uint64_t>(90000+i*3600),static_cast<std::uint64_t>(90000+i*3600),3600,true,true,i==0};
        std::vector<mpegts::Packet> p;if(!mux.write(mpegts::ElementaryKind::Video,v,p,e)){std::cerr<<e<<"\n";return false;}for(auto&x:p)out.insert(out.end(),x.begin(),x.end());
        mpegts::ElementarySample a{audio.data(),audio.size(),static_cast<std::uint64_t>(90000+i*1920),static_cast<std::uint64_t>(90000+i*1920),1920,true,true,false};
        p.clear();if(!mux.write(mpegts::ElementaryKind::Audio,a,p,e)){std::cerr<<e<<"\n";return false;}for(auto&x:p)out.insert(out.end(),x.begin(),x.end());
    }
    return !out.empty();
}

static bool testSampleAes(const std::vector<std::uint8_t>& ts) {
    std::array<std::uint8_t,16> key{},iv{};for(std::size_t i=0;i<16;++i){key[i]=static_cast<std::uint8_t>(i+1);iv[i]=static_cast<std::uint8_t>(0xa0+i);}std::vector<std::uint8_t> enc,dec;std::string e;
    if(!hls::transformSampleAesMpegTs(ts.data(),ts.size(),key,iv,true,enc,e)){std::cerr<<"sample-aes enc: "<<e<<"\n";return false;}
    if(!hls::transformSampleAesMpegTs(enc.data(),enc.size(),key,iv,false,dec,e)){std::cerr<<"sample-aes dec: "<<e<<"\n";return false;}
    mpegts::NativeTsDemux demux;int samples=0;demux.setSampleCallback([&](mpegts::DemuxSample&&){++samples;});if(!demux.push(dec.data(),dec.size(),e)){std::cerr<<e<<"\n";return false;}demux.flush();return samples>0;
}

static bool testCmaf(const std::vector<std::uint8_t>& ts) {
    const auto dir=std::filesystem::temp_directory_path()/"dvbstreamer5-stage9-cmaf-test";std::error_code ec;std::filesystem::remove_all(dir,ec);
    cmaf::NativeCmafSegmenter seg;cmaf::SegmenterConfig cfg;cfg.directory=dir;cfg.targetDurationSeconds=0.05;cfg.liveWindowSegments=3;std::string e;if(!seg.start(cfg,e)){std::cerr<<e<<"\n";return false;}if(!seg.push(ts.data(),ts.size())){std::cerr<<seg.lastError()<<"\n";return false;}seg.stop();
    const auto initPath=dir/"init.mp4";if(!std::filesystem::exists(initPath)){std::cerr<<"init.mp4 missing\n";return false;}std::filesystem::path fragment;for(auto&x:std::filesystem::directory_iterator(dir))if(x.path().extension()==".m4s"){fragment=x.path();break;}if(fragment.empty()){std::cerr<<"m4s missing\n";return false;}
    auto load=[](const std::filesystem::path&p){std::ifstream f(p,std::ios::binary);return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),{});};auto init=load(initPath),m4s=load(fragment);std::vector<std::uint8_t> round;
    cmaf::Fmp4ToMpegTs in;if(!in.initialize(init,[&](const std::uint8_t*d,std::size_t n){round.insert(round.end(),d,d+n);return true;},e)){std::cerr<<"cmaf init: "<<e<<"\n";return false;}if(!in.pushFragment(m4s,e)){std::cerr<<"cmaf frag: "<<e<<"\n";return false;}std::filesystem::remove_all(dir,ec);return !round.empty();
}

static bool testEncryptedCmaf(const std::vector<std::uint8_t>& ts) {
    const auto dir = std::filesystem::temp_directory_path() / "dvbstreamer5-stage9-cmaf-cbcs-test";
    std::error_code ec; std::filesystem::remove_all(dir, ec);
    cmaf::NativeCmafSegmenter seg;
    cmaf::SegmenterConfig cfg;
    cfg.directory = dir; cfg.targetDurationSeconds = 0.05; cfg.liveWindowSegments = 3;
    cfg.encryption = "sample-aes"; cfg.keyUri = "key.bin"; cfg.hasKey = true;
    for (std::size_t i=0;i<cfg.key.size();++i) cfg.key[i] = static_cast<std::uint8_t>(0x31+i);
    std::string e;
    if (!seg.start(cfg,e)) { std::cerr << "cmaf cbcs start: " << e << "\n"; return false; }
    if (!seg.push(ts.data(), ts.size())) { std::cerr << "cmaf cbcs push: " << seg.lastError() << "\n"; return false; }
    seg.stop();
    std::filesystem::path fragment;
    for (auto& x : std::filesystem::directory_iterator(dir)) if (x.path().extension()==".m4s") { fragment=x.path(); break; }
    if (fragment.empty()) { std::cerr << "encrypted m4s missing\n"; return false; }
    auto load=[](const std::filesystem::path&p){std::ifstream f(p,std::ios::binary);return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),{});};
    auto init=load(dir/"init.mp4"), m4s=load(fragment);
    const auto encryptedCopy = m4s;
    if (!cmaf::decryptSampleAesFragment(m4s, cfg.key, e)) { std::cerr << "cmaf cbcs decrypt: " << e << "\n"; return false; }
    if (m4s == encryptedCopy) { std::cerr << "cmaf cbcs did not transform fragment\n"; return false; }
    std::vector<std::uint8_t> round;
    cmaf::Fmp4ToMpegTs in;
    if (!in.initialize(init,[&](const std::uint8_t*d,std::size_t n){round.insert(round.end(),d,d+n);return true;},e)) { std::cerr << "cmaf cbcs init: " << e << "\n"; return false; }
    if (!in.pushFragment(m4s,e)) { std::cerr << "cmaf cbcs frag: " << e << "\n"; return false; }
    std::ifstream pf(dir/"video.m3u8"); std::string playlist((std::istreambuf_iterator<char>(pf)),{});
    std::filesystem::remove_all(dir,ec);
    if (playlist.find("METHOD=SAMPLE-AES") == std::string::npos || playlist.find("#EXT-X-MAP") == std::string::npos) {
        std::cerr << "CMAF encrypted playlist tags missing\n"; return false;
    }
    return !round.empty();
}

static bool testRtsp(const std::vector<std::uint8_t>& ts) {
    rtsp::NativeRtspOutput server;rtsp::OutputConfig oc;oc.bindAddress="127.0.0.1";oc.port=18554;oc.streamName="test";std::string e;if(!server.start(oc,{}, {}, {},e)){std::cerr<<"rtsp server: "<<e<<"\n";return false;}
    rtsp::NativeRtspInput client;rtsp::InputConfig ic;ic.uri="rtsp://127.0.0.1:18554/test";ic.mode="tcp";std::vector<std::uint8_t> got;if(!client.start(ic,[&](const std::uint8_t*d,std::size_t n){got.insert(got.end(),d,d+n);return true;},{},e)){server.stop();std::cerr<<"rtsp client: "<<e<<"\n";return false;}
    std::this_thread::sleep_for(std::chrono::milliseconds(250));for(int i=0;i<20&&got.empty();++i){server.push(ts.data(),ts.size());std::this_thread::sleep_for(std::chrono::milliseconds(50));}client.stop();server.stop();return !got.empty();
}

int main(){std::vector<std::uint8_t>ts;if(!makeTs(ts))return 1;if(!testSampleAes(ts))return 2;if(!testCmaf(ts))return 3;if(!testEncryptedCmaf(ts))return 4;if(!testRtsp(ts))return 5;if(!rtmp_test::testPublish(ts))return 6;if(!rtmp_test::testPlay())return 7;std::cout<<"PASS: native RTSP/RTMP/CMAF/cbcs/SAMPLE-AES protocols\n";return 0;}
