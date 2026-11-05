#include "media/UdpTsMux.h"
#include "media/UdpTsOutput.h"
#include "media/UdpSendClock.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>
using namespace dvbstreamer5::media::network;
using Packet=dvbstreamer5::media::mpegts::Packet;
static void check(bool ok,const char* text) {if(!ok){std::cerr<<text<<'\n';std::exit(1);}}
static Packet packet(std::uint64_t id,std::uint64_t pcr=0) {
 Packet p;p.fill(0xff);p[0]=0x47;p[1]=1;p[2]=0;p[3]=0x10|(id&15);
 std::memcpy(p.data()+40,&id,sizeof(id));
 if(pcr){p[3]|=0x20;p[4]=7;p[5]=0x10;auto b=pcr/300,e=pcr%300;
  p[6]=b>>25;p[7]=b>>17;p[8]=b>>9;p[9]=b>>1;p[10]=((b&1)<<7)|0x7e|(e>>8);p[11]=e;}
 return p;
}
static std::vector<Packet> unpack(const std::uint8_t* bytes,std::size_t size) {
 check(size&&size%188==0&&size<=1316,"invalid UDP TS wire size");std::vector<Packet> result;
 for(std::size_t at=0;at<size;at+=188){Packet p;std::memcpy(p.data(),bytes+at,188);check(p[0]==0x47,"lost TS alignment");result.push_back(p);}return result;
}
static void framing() {
 UdpTsMux mux(false,0);std::vector<Packet> input;
 for(unsigned i=0;i<12;++i)input.push_back(packet(i));
 input[4][1]=0x1f;input[4][2]=0xff; // VBR must preserve source NULL too
 check(mux.push({input.begin(),input.begin()+3},100),"push partial VBR");UdpTsMux::Datagram d;
 check(!mux.emit(100,d),"premature partial VBR");
 check(mux.push({input.begin()+3,input.end()},200),"push VBR remainder");
 check(mux.emit(201,d)&&d.size==1316,"VBR failed to join input chunks");
 auto first=unpack(d.bytes.data(),d.size);check(std::equal(first.begin(),first.end(),input.begin()),"VBR bytes changed");
 check(!mux.emit(1000000,d),"partial VBR timeout ignored");
 check(mux.emit(2000200,d)&&d.size==5*188,"partial VBR not flushed");
 auto last=unpack(d.bytes.data(),d.size);check(std::equal(last.begin(),last.end(),input.begin()+7),"VBR tail changed");
 auto bad=packet(30);bad[0]=0;check(!mux.push({packet(29),bad},3000000)&&mux.queuedPackets()==0,"non-atomic invalid chunk admission");
 UdpTsMux bounded(false,0);std::vector<Packet> full(UdpTsMux::kMaxBytes/188,packet(1));
 check(bounded.push(full,1),"bounded queue rejects valid capacity");
 check(!bounded.push({packet(2)},2),"queue limit exceeded silently");
}
static void clockTest() {
 for(auto rate:{3000000ULL,9300000ULL}){
  UdpSendClock c(rate);c.start(1000000000);std::uint64_t first=c.deadline();
  for(unsigned i=0;i<200000;++i){const auto due=c.deadline();check(!c.advance(due+50000),"small lateness reset clock");}
  auto expected=first+static_cast<std::uint64_t>(static_cast<unsigned __int128>(200000)*1316*8*1000000000ULL/rate);
  check(c.deadline()==expected,"scheduler wakeup lateness accumulates into rate error");
  const auto stalled=c.deadline()+100000000;check(c.advance(stalled),"long stall not detected");
  check(c.deadline()>stalled,"stale deadlines replayed as a burst");
 }
}
static void pcrModel(bool source) {
 UdpTsMux mux(true,source?3000808:3000708);std::deque<Packet> expected;
 std::uint64_t inputNs=0,nextPcr=0,id=0,slots=0,firstSlot=0;bool seen=false;
 constexpr std::uint64_t hz=27000000,wrap=(1ULL<<33)*300ULL,origin=wrap-hz;long double worst=0;
 std::uint64_t previousTicks=0,elapsedTicks=0;
 for(std::uint64_t dg=0;;++dg){auto wall=dg*1316ULL*8*1000000000/3000000;if(wall>600000000000ULL)break;
  while(inputNs<=wall){auto rate=(inputNs/15000000000ULL)&1?2800000ULL:2000000ULL;
   bool withPcr=inputNs>=nextPcr;auto p=packet(id++,withPcr?origin+inputNs*hz/1000000000:0);
   check(mux.push({p},inputNs),"PCR model queue overflow");expected.push_back(p);
   if(withPcr)nextPcr=inputNs+20000000;inputNs+=188ULL*8*1000000000/rate;}
  if(wall<4500000000ULL)continue;UdpTsMux::Datagram d;check(mux.emit(wall,d)&&d.size==1316,"CBR layout changed");
  for(auto p:unpack(d.bytes.data(),d.size)){auto slot=slots++;if(p[1]==0x1f&&p[2]==0xff)continue;
   check(!expected.empty(),"unexpected media");auto original=expected.front();expected.pop_front();
   if(p[3]&0x20){auto ticks=[](const Packet& t){return (((std::uint64_t(t[6])<<25)|(std::uint64_t(t[7])<<17)|(std::uint64_t(t[8])<<9)|(std::uint64_t(t[9])<<1)|(t[10]>>7))*300+((t[10]&1)<<8)+t[11]);};
    auto originalTicks=ticks(original);if(!seen){seen=true;previousTicks=originalTicks;firstSlot=slot;}
    elapsedTicks+=(originalTicks+wrap-previousTicks)%wrap;previousTicks=originalTicks;
    auto phase=(static_cast<long double>(slot-firstSlot)*188*8*hz/3000000-elapsedTicks)*1000/hz;
    worst=std::max(worst,std::abs(phase));
    if(!source){auto delta=(ticks(p)+wrap-originalTicks)%wrap;check(std::min(delta,wrap-delta)<27000,"Fixed PCR diverged from source timestamps");
      std::copy(original.begin()+6,original.begin()+12,p.begin()+6);}}
   check(p==original,"CBR changed payload/order/PTS/DTS/CC");}
 }
 check(worst<0.502L,"new mux lost source PCR delivery phase");
 std::cout<<(source?"source":"fixed")<<" PCR phase max ms="<<static_cast<double>(worst)<<'\n';
}
static void retryAndObserver() {
 UdpSocket a,b;std::string error;check(a.openSender("127.0.0.1",19099,"",error)&&b.openSender("127.0.0.1",19100,"",error),"test socket setup");
 std::atomic<std::uint64_t> bytes{0};std::atomic<unsigned> retries{0};std::mutex mutex;
 std::vector<Packet> inputs,received[2],observed;for(unsigned i=0;i<100;++i)inputs.push_back(packet(i));
 std::string failure;
 UdpTsOutput output(false,0,"retry-test",{{&a,0,{}},{&b,1,{}}},&bytes,
  [&](const std::uint8_t* p,std::size_t n){std::lock_guard<std::mutex> lock(mutex);auto v=unpack(p,n);observed.insert(observed.end(),v.begin(),v.end());},
  [&](const std::string& e){std::lock_guard<std::mutex> lock(mutex);failure=e;},
  [&](UdpSocket&,std::size_t index,const std::uint8_t* p,std::size_t n,std::string&){
   if(index==0 && retries.fetch_add(1)<80)return UdpSocket::SendResult::Retry;
   std::lock_guard<std::mutex> lock(mutex);auto v=unpack(p,n);received[index].insert(received[index].end(),v.begin(),v.end());return UdpSocket::SendResult::Sent;});
 check(output.enqueue(inputs),"enqueue retry fixture");output.finish();
 const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(3);
 while(!output.drained()&&std::chrono::steady_clock::now()<limit)std::this_thread::sleep_for(std::chrono::milliseconds(1));
 check(output.drained(),"retry did not drain");auto stats=output.stats();output.stop();
 check(failure.empty()&&received[0]==inputs&&received[1]==inputs&&observed==inputs,"retry/fanout lost or duplicated TS");
 check(stats.retries>=80&&bytes==inputs.size()*188ULL*2,"retry counters inaccurate");
}
static void explicitFailure() {
 UdpSocket socket;std::string error;check(socket.openSender("127.0.0.1",19101,"",error),"failure fixture socket");
 std::atomic<std::uint64_t> bytes{0};std::atomic<bool> failed{false};
 UdpTsOutput output(false,0,"failure-test",{{&socket,0,{}}},&bytes,{},
  [&](const std::string& detail){check(detail.find("injected send failure")!=std::string::npos,"failure detail lost");failed=true;},
  [](UdpSocket&,std::size_t,const std::uint8_t*,std::size_t,std::string& detail){detail="injected send failure";return UdpSocket::SendResult::Failed;});
 check(output.enqueue({packet(1)}),"failure fixture enqueue");output.finish();
 const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(2);
 while(!failed&&std::chrono::steady_clock::now()<limit)std::this_thread::sleep_for(std::chrono::milliseconds(1));
 check(failed&&bytes==0&&!output.lastError().empty(),"failed datagram counted as delivered or failure not reported");
 output.stop();
}
int main(){framing();clockTest();pcrModel(true);pcrModel(false);retryAndObserver();explicitFailure();std::cout<<"UDP transport core: OK\n";}
