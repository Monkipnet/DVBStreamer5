#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/DvbEitOutput.h"
#include <cassert>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <set>
#include <vector>
using Packet=dvbstreamer5::media::mpegts::Packet;
static unsigned b16(const uint8_t*p){return (static_cast<unsigned>(p[0])<<8U)|p[1];}
static uint32_t crc(const uint8_t*p,size_t len){
 uint32_t n=0xffffffffU;
 for(size_t i=0;i<len;++i){
  n ^= static_cast<uint32_t>(p[i])<<24;
  for(int b=0;b<8;++b)n=(n&0x80000000U)?(n<<1U)^0x04c11db7U:n<<1U;
 }
 return n;
}
static Json::Value sample(){
 Json::Value root(Json::objectValue),events(Json::arrayValue),e(Json::objectValue);
 auto now=static_cast<Json::Int64>(time(nullptr));
 e["start_utc"]=now-60;e["end_utc"]=now+3600;e["event_id"]=1U;
 e["title"]="Synthetic programme";e["description"]="Fixture";
 events.append(e);root["events"]=events;return root;
}
static void verify(bool other){
 DvbEitOutput gen([]{return sample();},101,202,303,other);
 std::vector<Packet> packets;
 gen.process(packets);
 assert(!packets.empty());
 std::set<unsigned> table;
 for(const auto&p:packets){
  assert(p[0]==0x47 && (p[1]&0x1f)==0 && p[2]==0x12);
  if(!(p[1]&0x40))continue;
  assert(p[4]==0);
  const auto *s=p.data()+5;
  const auto len=3U+((static_cast<unsigned>(s[1]&0xf)<<8U)|s[2]);
  assert(len>=18 && len<=183);
  assert(crc(s,len)==0);
  assert(b16(s+3)==101 && b16(s+8)==202 && b16(s+10)==303);
  table.insert(s[0]);
 }
 assert(table.count(other?0x4f:0x4e));
 assert(table.count(other?0x60:0x50));
}
int main(){verify(false);verify(true);std::cout<<"PASS WISI Actual/Barker EIT CRC\n";}
