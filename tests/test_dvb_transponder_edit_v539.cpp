#ifdef NDEBUG
#undef NDEBUG
#endif
#include "http/DvbTransponderUriEditor.h"
#include <cassert>
#include <iostream>
#include <string>
using namespace dvbstreamer5::dvb_tp_editor;
int main() {
  const std::string one="dvb://satellite?adapter=1&frontend=0&frequency_khz=12383000"
    "&symbol_rate=27500&polarity=H&delivery_system=dvb-s2"
    "&stream_id=0&diseqc_source=1&service_id=101&pids=101:102:8192"
    "&jitter_ms=120&satellite_name=BetaDigital&custom_vendor_flag=1";
  const std::string two="dvb://satellite?adapter=1&frontend=0&frequency_khz=12383000"
    "&symbol_rate=27500&polarity=H&delivery_system=dvb-s2"
    "&stream_id=0&diseqc_source=1&service_id=202&pids=202:203"
    "&custom_vendor_flag=2";
  const std::string tuned="dvb://satellite?adapter=1&frontend=0"
    "&frequency_khz=12456000&symbol_rate=30000&polarity=V"
    "&delivery_system=dvb-s2&modulation=8psk&fec=3%2F4"
    "&stream_id=1&diseqc_source=2&unicable_mode=none"
    "&lnb_lof1_khz=9750000&lnb_lof2_khz=10600000"
    "&lnb_slof_khz=11700000&unicable_user_band=-1"
    "&unicable_frequency_khz=0&unicable_position=0&unicable_pin=-1"
    "&pids=8192&jitter_ms=0";
  const auto a=queryParts(editUri(one,tuned,"Новая сеть 19.2°E"));
  const auto b=queryParts(editUri(two,tuned,"Новая сеть 19.2°E"));
  assert(a.at("frequency_khz")=="12456000");
  assert(b.at("frequency_khz")=="12456000");
  assert(a.at("symbol_rate")=="30000");
  assert(b.at("polarity")=="V");
  assert(a.at("service_id")=="101");
  assert(b.at("service_id")=="202");
  assert(a.at("pids")=="101:102:8192");
  assert(b.at("pids")=="202:203");
  assert(a.at("jitter_ms")=="120");
  assert(a.at("custom_vendor_flag")=="1");
  assert(b.at("custom_vendor_flag")=="2");
  assert(a.at("adapter")=="1"&&a.at("frontend")=="0");
  assert(a.at("satellite_name")!="");
  assert(a.at("satellite_name").find('%')!=std::string::npos);
  const auto cleared=queryParts(editUri(one,tuned,""));
  assert(cleared.count("satellite_name")==0);
  assert(editUri("invalid-without-query",tuned,"").empty());
  std::cout<<"PASS V539: two synthetic transponder services keep individual SID, PID, jitter and vendor options on TP retune\n";
}
