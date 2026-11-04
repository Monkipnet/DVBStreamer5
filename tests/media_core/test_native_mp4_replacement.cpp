#include "media/NativeMp4Replacement.h"
#include "media/TransportStream.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main(){
    using namespace dvbstreamer5::media;
    using namespace dvbstreamer5::media::mpegts;
    const std::string b64=R"B64(AAAAIGZ0eXBpc29tAAAAAGlzb21pc28yYXZjMW1wNDEAAAApbWRhdAAAAANliIQAAAADQZoiAAAAA0GaMyEQViEgZiEwdiFAhgAAAoltb292AAABW3RyYWsAAAFTbWRpYQAAACBtZGhkAAAAAAAAAAAAAAAAAAFfkAAAKjBVxAAAAAAAIGhkbHIAAAAAAAAAAHZpZGUAAAAAAAAAAAAAAAAAAAELbWluZgAAAQNzdGJsAAAAf3N0c2QAAAAAAAAAAQAAAG9hdmMxAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAGWF2Y0MBQgAe/+EABGdCAB4BAAJozgAAABhzdHRzAAAAAAAAAAEAAAADAAAOEAAAABxzdHNjAAAAAAAAAAEAAAABAAAAAwAAAAEAAAAgc3RzegAAAAAAAAAAAAAAAwAAAAcAAAAHAAAABwAAABRzdGNvAAAAAAAAAAEAAAAoAAAAFHN0c3MAAAAAAAAAAQAAAAEAAAEmdHJhawAAAR5tZGlhAAAAIG1kaGQAAAAAAAAAAAAAAAAAALuAAAAQAFXEAAAAAAAgaGRscgAAAAAAAAAAc291bgAAAAAAAAAAAAAAAAAAANZtaW5mAAAAznN0YmwAAABac3RzZAAAAAAAAAABAAAASm1wNGEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAJmVzZHMAAAAAAxkAAQAEEUAVAAAAAAAAAAAAAAUCEZAGAQIAAAAYc3R0cwAAAAAAAAABAAAABAAABAAAAAAcc3RzYwAAAAAAAAABAAAAAQAAAAQAAAABAAAAJHN0c3oAAAAAAAAAAAAAAAQAAAADAAAAAwAAAAMAAAADAAAAFHN0Y28AAAAAAAAAAQAAAD0=)B64";
    std::vector<unsigned char> bytes;
    int acc=0,bits=-8;
    for(char c:b64){
        if(c=='=') break;
        int x=-1;
        if(c>='A'&&c<='Z')x=c-'A';
        if(c>='a'&&c<='z')x=c-'a'+26;
        if(c>='0'&&c<='9')x=c-'0'+52;
        if(c=='+')x=62;if(c=='/')x=63;
        if(x<0)continue;
        acc=(acc<<6)|x;bits+=6;
        if(bits>=0){bytes.push_back(static_cast<unsigned char>((acc>>bits)&255));bits-=8;}
    }
    auto path=std::filesystem::temp_directory_path()/
        ("dvbstreamer5_mp4_"+std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count())+".mp4");
    {std::ofstream f(path,std::ios::binary);
     if(!f || bytes.size()!=722)return 1;
     f.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));}
    auto cleanup=[&]{std::error_code ec;std::filesystem::remove(path,ec);};
    mp4::NativeMp4Replacement reader;
    std::string error;
    if(!reader.open(path.string(),true,42,"MP4 backup","Provider",error)){
        std::cerr<<error<<"\n";cleanup();return 2;}
    if(reader.videoSamples()!=3 || reader.audioSamples()!=4){cleanup();return 3;}
    bool pat=false,pmt=false,pcr=false,video=false,audio=false;
    unsigned videoCount=0,audioCount=0;
    std::uint64_t lastClock=0,lastPcr=0;
    for(unsigned i=0;i<28;++i){
        std::vector<Packet> packets;
        std::uint64_t clock=0;
        if(!reader.next(packets,clock,error) || packets.empty()){cleanup();return 4;}
        if(clock<lastClock){cleanup();return 5;}
        lastClock=clock;
        for(const auto& p:packets){
            PacketInfo info;
            if(!inspectPacket(p.data(),p.size(),info)){cleanup();return 6;}
            pat|=info.pid==0;pmt|=info.pid==0x1000;
            if(info.pid==0x100){video=true;if(info.payloadUnitStart)++videoCount;}
            if(info.pid==0x101){audio=true;if(info.payloadUnitStart)++audioCount;}
            if(info.hasPcr){
                if(pcr&&info.pcrBase90k<lastPcr){cleanup();return 7;}
                pcr=true;lastPcr=info.pcrBase90k;
            }
        }
    }
    cleanup();
    if(!pat||!pmt||!pcr||!video||!audio||reader.loops()<3||
       videoCount<10||audioCount<10) return 8;
    std::cout<<"native MP4 remux loop and TS transport OK\n";
}
