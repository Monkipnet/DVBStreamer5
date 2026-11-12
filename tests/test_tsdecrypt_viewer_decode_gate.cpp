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
  // Before any real HLS/HTTP/SRT viewer, PES is an amber/mint indicator.
  assert(step(0,false,8,true)=="unverified");
  assert(step(1,false,0,true,"invalid")=="invalid");
  // A real HLS media segment triggers immediate green on existing PES data.
  assert(step(2,true,0,true,"invalid")=="viewer_pes_ok");
  // Disconnecting the viewer does not revert green while PES still arrives.
  assert(step(3,false,9,true)=="viewer_pes_ok");
  assert(step(9,false,0,true,"invalid")=="viewer_pes_ok");
  assert(step(17,false,0,true,"scrambled")=="viewer_pes_ok"); // 14s PES grace
  assert(step(19,false,0,true,"scrambled")=="scrambled"); // 16s PES starvation
  assert(step(20,true,0,true,"scrambled")=="scrambled"); // viewer alone insufficient
  assert(step(21,false,12,true)=="unverified"); // PES alone insufficient
  assert(step(22,true,0,true,"invalid")=="viewer_pes_ok"); // connect after PES
  assert(step(23,false,0,false,"waiting")=="viewer_pes_ok"); // short stall
  assert(step(29,false,0,false,"waiting")=="waiting"); // output not healthy >6s
  assert(step(30,false,12,true)=="unverified"); // must reconnect after reset
  assert(step(31,true,0,true,"invalid")=="viewer_pes_ok");
  assert(step(32,false,12,true,"unverified",2)=="unverified"); // restart
  assert(step(33,true,0,true,"invalid",2)=="viewer_pes_ok");
  assert(step(34,true,0,true,"unverified",2,false)=="offline");
  assert(step(35,true,0,true)=="unverified"); // cleared evidence on stop
  assert(step(36,false,11,true)=="unverified");
  std::cout << "TSDecrypt HLS/media + stable PES gate test OK\n";
}
