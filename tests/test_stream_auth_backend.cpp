#include "StreamAuthBackend.h"
#include <cstdlib>
#include <iostream>
#include <string>
using namespace dvbstreamer5;
static void check(bool ok,const char* reason) {
    if(!ok){std::cerr<<"FAIL: "<<reason<<"\n";std::exit(1);}
}
int main() {
    AppConfig cfg;
    check(!stream_auth::enabled(cfg),"legacy default must not require middleware");
    cfg.streamAuthBackendEnabled=true;
    check(!stream_auth::authorize(cfg,"channel1","127.0.0.1",
        "/stream/channel1.ts","Test","localhost","token=abc"),
        "enabled authorization without backend MUST deny");
    cfg.streamAuthBackendUrl="ftp://invalid.example/auth";
    check(!stream_auth::authorize(cfg,"channel1","127.0.0.1",
        "/stream/channel1.ts","Test","localhost","token=abc"),
        "non-HTTP backend URL MUST deny");
    cfg.streamAuthBackendEnabled=false;
    check(stream_auth::authorize(cfg,"channel1","127.0.0.1",
        "/stream/channel1.ts","Test","localhost",""),
        "disabled backend must retain legacy access behavior");
    const std::string sid=stream_auth::srtQuery("#!::r=live,token=abc,m=request");
    check(sid.find("streamid=")==0,"SRT should pass original streamid");
    check(sid.find("&token=abc")!=std::string::npos,
        "SRT token field must be forwarded to middleware");
    std::string playlist=
        "#EXTM3U\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\n"
        "#EXT-X-MAP:URI=\"init.mp4\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=1000000\n"
        "low/index.m3u8\n"
        "segment0001.ts\n"
        "#EXT-X-ENDLIST\n";
    stream_auth::propagateHlsQuery(playlist,"token=abc");
    check(playlist.find("URI=\"key.bin?token=abc\"")!=std::string::npos,
        "HLS key must retain token");
    check(playlist.find("URI=\"init.mp4?token=abc\"")!=std::string::npos,
        "CMAF init segment must retain token");
    check(playlist.find("low/index.m3u8?token=abc")!=std::string::npos,
        "variant playlist must retain token");
    check(playlist.find("segment0001.ts?token=abc")!=std::string::npos,
        "HLS media segment must retain token");
    check(playlist.find("#EXTM3U?token")==std::string::npos,
        "HLS directives must stay byte-compatible");
    std::cout<<"Stream auth backend: OK\n";
}
