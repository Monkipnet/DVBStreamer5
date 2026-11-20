#ifdef NDEBUG
#undef NDEBUG
#endif
#include "http/DvbTransponderUriEditor.h"
#include <cassert>
#include <iostream>
#include <string>
using namespace dvbstreamer5::dvb_tp_editor;
int main(){
  const std::string base="dvb://terrestrial?adapter=3&frontend=1"
    "&frequency_khz=666000&bandwidth_hz=8000000&delivery_system=dvb-t2"
    "&modulation=auto&stream_id=5";
  const std::string a=base+"&service_id=101&pnr=101&pids=100:101:8192"
    "&jitter_ms=140&vendor_option=alpha";
  const std::string b=base+"&service_id=202&pnr=202&pids=200:201:8192"
    "&jitter_ms=220&vendor_option=beta";
  const std::string tune="dvb://terrestrial?adapter=3&frontend=1"
    "&frequency_khz=674000&bandwidth_hz=7000000&delivery_system=dvb-t2"
    "&modulation=qam256&stream_id=6&jitter_ms=0&pids=8192";
  const auto one=editUri(a,tune,"");
  const auto two=editUri(b,tune,"");
  assert(one.rfind("dvb://terrestrial?",0)==0);
  assert(two.rfind("dvb://terrestrial?",0)==0);
  const auto x=queryParts(one),y=queryParts(two);
  for(const auto* key:{"frequency_khz","bandwidth_hz","delivery_system",
                       "modulation","stream_id"}){
    assert(x.at(key)==queryParts(tune).at(key));
    assert(y.at(key)==queryParts(tune).at(key));
  }
  assert(x.at("frequency_khz")=="674000");
  assert(x.at("bandwidth_hz")=="7000000");
  assert(x.at("stream_id")=="6");
  assert(x.at("service_id")=="101"&&y.at("service_id")=="202");
  assert(x.at("pnr")=="101"&&y.at("pnr")=="202");
  assert(x.at("pids")=="100:101:8192"&&y.at("pids")=="200:201:8192");
  assert(x.at("jitter_ms")=="140"&&y.at("jitter_ms")=="220");
  assert(x.at("vendor_option")=="alpha"&&y.at("vendor_option")=="beta");
  const auto t=editUri(
    "dvb://terrestrial?frequency_khz=474000&bandwidth_hz=8000000"
    "&delivery_system=dvb-t&stream_id=-1&service_id=77",
    "dvb://terrestrial?frequency_khz=482000&bandwidth_hz=6000000"
    "&delivery_system=dvb-t&stream_id=-1","");
  const auto z=queryParts(t);
  assert(z.at("bandwidth_hz")=="6000000"&&z.at("service_id")=="77");
  assert(z.at("stream_id")=="-1");
  std::cout << "PASS V602: DVB-T/T2 MUX retune preserves per-service PID, SID,"
    " jitter, vendor keys and PLP/bandwidth\n";
}
