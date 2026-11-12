#include "../src/http/TsdecryptViewerDecodeGate.h"
#include <cassert>
#include <chrono>
#include <iostream>
int main() {
    using dvbstreamer5::http::TsdecryptViewerDecodeGate;
    using dvbstreamer5::http::updateTsdecryptViewerDecode;
    using Clock=std::chrono::steady_clock;
    using namespace std::chrono_literals;
    const Clock::time_point t0{10s};
    TsdecryptViewerDecodeGate state;
    const auto step=[&](int seconds, bool client, unsigned pes,
                        bool healthy, const char* raw="unverified",
                        std::uint64_t generation=1, bool active=true) {
        return updateTsdecryptViewerDecode(state,generation,raw,
                active,healthy,pes,client,t0+std::chrono::seconds(seconds));
    };
    assert(step(0,false,10,true)=="unverified");
    assert(step(1,false,10,true)=="unverified");
    assert(step(2,true,10,true)=="viewer_pes_ok");
    assert(step(3,false,10,true)=="viewer_pes_ok"); // viewer left
    assert(step(4,false,0,true,"invalid")=="viewer_pes_ok"); // short gap
    assert(step(6,false,0,true,"scrambled")=="viewer_pes_ok");
    assert(step(10,false,0,true,"scrambled")=="scrambled"); // >6 s starvation
    assert(step(11,true,0,true,"invalid")=="invalid"); // client alone cannot promote
    assert(step(12,true,10,true)=="unverified"); // first new good sample
    assert(step(13,true,10,true)=="viewer_pes_ok");
    assert(step(14,false,0,false,"waiting")=="waiting"); // no output
    assert(step(15,false,10,true)=="unverified"); // requires new client
    assert(step(16,true,10,true)=="viewer_pes_ok");
    assert(step(17,false,10,true,"unverified",2)=="unverified"); // restart
    assert(step(18,true,10,true,"unverified",2)=="viewer_pes_ok");
    assert(step(19,true,10,true,"unverified",2,false)=="offline");
    assert(step(20,false,10,true,"unverified",2)=="unverified");
    std::cout<<"TSDecrypt viewer PES promotion state test OK\n";
}
