#include "media/NativeNvencThroughputPolicy.h"
#include <cassert>
#include <iostream>
using namespace dvbstreamer5::media::codec;
int main(){
    assert(effectiveNvencPreset("balanced",nullptr,false,false)==5);
    assert(effectiveNvencPreset("balanced","p3",false,false)==3);
    assert(effectiveNvencPreset("p4",nullptr,false,false)==4);
    assert(effectiveNvencPreset("p3",nullptr,false,false)==3);
    assert(effectiveNvencPreset("p1","p3",false,false)==1);
    assert(effectiveNvencPreset("quality","p2",true,false)==6);
    assert(effectiveNvencPreset("quality","p2",false,true)==6);
    assert(effectiveNvencPreset("balanced","bad",false,false)==5);
    assert(nvencPresetCap("p0")==0 && nvencPresetCap("p8")==0);
    NvencStageStats s;
    s.add(10); s.add(30); assert(s.average()==20 && s.maxUs==30 && s.count==2);
    s.clear(); assert(s.count==0 && s.average()==0);
    constexpr std::uint64_t mask=(1ULL<<33)-1;
    assert(advanceNvencPts(0,0,3600)==3600);
    assert(advanceNvencPts(0,7200,3600)==10800);
    assert(advanceNvencPts(mask-99,50,100)==100);
    for(std::uint64_t step:{1ULL,1800ULL,3600ULL,9000ULL}){
        for(std::uint64_t k=0;k<10000;k+=97){
            const auto start=(mask-3000)&mask,pts=(start+k)&mask;
            auto expected=start;
            do {expected=(expected+step)&mask;} while(((pts-expected)&mask)<(1ULL<<32));
            assert(advanceNvencPts(start,pts,step)==expected);
        }
    }
    std::cout<<"PASS NVENC throughput/preset policy and PTS wrap\n";
}
