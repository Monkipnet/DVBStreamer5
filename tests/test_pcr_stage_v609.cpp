#include "media/PcrStageTrace.h"
#include <array>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using dvbstreamer5::media::mpegts::PcrStageTrace;
static std::array<std::uint8_t,188> packet(std::uint64_t pcr,bool hasPcr) {
    std::array<std::uint8_t,188> p{};
    p[0]=0x47;p[1]=0x01;p[2]=0x02;p[3]=hasPcr?0x30:0x10;
    if(hasPcr){
        p[4]=7;p[5]=0x10;
        const auto base=pcr/300ULL;
        const auto ext=pcr%300ULL;
        p[6]=static_cast<std::uint8_t>(base>>25);
        p[7]=static_cast<std::uint8_t>(base>>17);
        p[8]=static_cast<std::uint8_t>(base>>9);
        p[9]=static_cast<std::uint8_t>(base>>1);
        p[10]=static_cast<std::uint8_t>(((base&1ULL)<<7)|((ext>>8)&1ULL));
        p[11]=static_cast<std::uint8_t>(ext);
    }
    return p;
}
static std::string trace(const std::vector<std::uint8_t>& bytes){
    std::ostringstream output;
    auto* const previous=std::cerr.rdbuf(output.rdbuf());
    {
        PcrStageTrace stage("home2-v609","post_ca");
        stage.inspect(bytes.data(),bytes.size());
    }
    std::cerr.rdbuf(previous);
    return output.str();
}
int main(){
    std::vector<std::uint8_t> source;
    for(int i=0;i<301;++i){
        const bool hasPcr=i==0||i==100||i==200||i==300;
        const std::uint64_t ticks=2700000ULL+
            static_cast<std::uint64_t>(i)*27000ULL+
            (i==300 ? 270000000ULL : 0ULL);
        const auto p=packet(ticks,hasPcr);
        source.insert(source.end(),p.begin(),p.end());
    }
    const auto original=source;
    const auto logs=trace(source);
    if(source!=original || logs.find("PCR STAGE TRACK stream=home2-v609 stage=post_ca")
          ==std::string::npos ||
       logs.find("PCR STAGE RESET stream=home2-v609 stage=post_ca cause=forward_jump")
          ==std::string::npos ||
       logs.find("pcr_pid=258")==std::string::npos ||
       logs.find("packet_gap=100")==std::string::npos){
        std::cerr<<"V609 passive PCR staging did not classify the source jump\n";
        return 1;
    }
    std::cout<<"V609 passive PCR stage emulation: PASS\n";
    return 0;
}
