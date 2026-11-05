#include "media/UdpTsOutput.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>
using namespace dvbstreamer5::media::network;
using Packet=dvbstreamer5::media::mpegts::Packet;
using Clock=std::chrono::steady_clock;
static void check(bool ok,const char* text){if(!ok){std::cerr<<text<<'\n';std::exit(1);}}
int main(){
 constexpr unsigned channels=100,packets=2000;
 std::vector<Packet> input;for(unsigned i=0;i<packets;++i){Packet p;p.fill(0xff);p[0]=0x47;p[1]=1;p[2]=0;p[3]=0x10|(i&15);std::memcpy(p.data()+40,&i,sizeof(i));input.push_back(p);}
 std::vector<std::unique_ptr<UdpSocket>> receivers,senders;
 std::vector<std::unique_ptr<UdpTsOutput>> outputs;
 std::atomic<std::uint64_t> total{0};std::atomic<unsigned> failures{0};std::string error;
 for(unsigned i=0;i<channels;++i){
  auto rx=std::make_unique<UdpSocket>(),tx=std::make_unique<UdpSocket>();
  check(rx->openReceiver("127.0.0.1",0,"","",2*1024*1024,error),"receiver setup failed");
  check(tx->openSender("127.0.0.1",rx->localPort(),"127.0.0.1",error),"sender setup failed");
  outputs.push_back(std::make_unique<UdpTsOutput>(true,3000799,"load-"+std::to_string(i),
   std::vector<UdpTsOutput::Destination>{{tx.get(),i,{}}},&total,UdpTsOutput::Observer{},
   [&](const std::string& e){std::cerr<<e<<'\n';++failures;}));
  receivers.push_back(std::move(rx));senders.push_back(std::move(tx));
 }
 for(auto& output:outputs)check(output->enqueue(input),"load enqueue failed");
 std::vector<UdpTsOutput::Stats> before;for(auto& output:outputs)before.push_back(output->stats());
 const auto start=Clock::now();std::vector<unsigned> received(channels,0);
 std::uint8_t buffer[2048];std::uint64_t receivedDatagrams=0;
 const auto end=start+std::chrono::seconds(3);
 while(Clock::now()<end){
  for(unsigned i=0;i<channels;++i)for(unsigned burst=0;burst<8;++burst){
   std::size_t size=0;check(receivers[i]->receive(buffer,sizeof(buffer),size,0,error),"load receive failed");
   if(!size)break;check(size==1316,"CBR load wire size changed");++receivedDatagrams;
   for(unsigned at=0;at<7;++at){Packet p;std::memcpy(p.data(),buffer+at*188,188);check(p[0]==0x47,"load TS alignment");
    if(((p[1]&31)*256+p[2])==0x1fff)continue;
    if (!(received[i]<packets&&p==input[received[i]])) {
      unsigned actual=0;std::memcpy(&actual,p.data()+40,sizeof(actual));
      std::cerr<<"channel="<<i<<" expected="<<received[i]<<" actual="<<actual
       <<" retry="<<outputs[i]->stats().retries<<" clock_reset="<<outputs[i]->stats().clockResets<<'\n';
      check(false,"load TS lost/duplicated/reordered");
    }++received[i];}
  }
 }
 const auto elapsed=std::chrono::duration<double>(Clock::now()-start).count();
 std::vector<UdpTsOutput::Stats> after;for(auto& output:outputs)after.push_back(output->stats());
 auto lowest=1e9,highest=0.0,sum=0.0;
 for(unsigned i=0;i<channels;++i){auto stats=after[i];outputs[i]->stop();
  check(received[i]==packets,"100-channel test did not deliver all media");
  const auto rate=(stats.datagrams-before[i].datagrams)*1316.0*8.0/elapsed/1000.0;lowest=std::min(lowest,rate);highest=std::max(highest,rate);sum+=rate;
  check(stats.clockResets==0,"shared scheduler could not sustain 100-channel load");
 }
 check(failures==0,"load output failure");
 check(lowest>2850&&highest<3150,"100-channel CBR rate outside 5% window");
 std::cout<<"100 UDP-CBR channels, target 300000 kbit/s total; per-channel min/mean/max="<<lowest<<'/'<<sum/channels<<'/'<<highest
  <<" kbit/s; media packets verified="<<channels*packets<<" received datagrams="<<receivedDatagrams<<'\n';
}
