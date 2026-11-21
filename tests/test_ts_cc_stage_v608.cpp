#include "TsCcStageTrace.h"
#include <array>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static std::array<std::uint8_t,188> packet(std::uint8_t cc) {
    std::array<std::uint8_t,188> p{};
    p[0]=0x47; p[1]=0x01; p[2]=0x02;
    p[3]=static_cast<std::uint8_t>(0x10U | cc);
    for(std::size_t i=4;i<p.size();++i)
        p[i]=static_cast<std::uint8_t>(i+cc);
    return p;
}
static std::string trace(const std::vector<std::uint8_t>& bytes) {
    std::ostringstream output;
    auto* const saved=std::cerr.rdbuf(output.rdbuf());
    {
        TsCcStageTrace probe("home2-test","post_ca");
        probe.inspect(bytes.data(),bytes.size());
    }
    std::cerr.rdbuf(saved);
    return output.str();
}
int main() {
    auto append=[](std::vector<std::uint8_t>& bytes,
                   const std::array<std::uint8_t,188>& p) {
        bytes.insert(bytes.end(),p.begin(),p.end());
    };
    std::vector<std::uint8_t> clean,broken;
    append(clean,packet(0));append(clean,packet(1));append(clean,packet(2));
    append(broken,packet(0));append(broken,packet(2));
    const auto cleanLog=trace(clean),brokenLog=trace(broken);
    if(cleanLog.find("cc_delta=0")==std::string::npos ||
       brokenLog.find("cc_delta=1")==std::string::npos ||
       brokenLog.find("pid_errors_delta=258:1")==std::string::npos) {
        std::cerr<<"V608 stage CC diagnostic did not identify lost video TS\n";
        return 1;
    }
    std::cout<<"V608 home2 TS stage continuity emulation: PASS\n";
}
