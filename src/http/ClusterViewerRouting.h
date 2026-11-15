#pragma once
// V537: deterministic, allocation-light origin selection for NEW HTTP/HLS viewers.
// No media packet processing or background source reconfiguration.
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace dvbstreamer5::cluster::routing {
struct Origin {
    std::string name;
    std::string redirect;
    std::uint64_t viewers=0;
    bool local=false;
    bool live=false;
};
struct Result {
    bool found=false;
    bool local=false;
    std::string redirect;
    std::string name;
};
inline std::uint64_t fingerprint(const std::string& value) {
    std::uint64_t hash=14695981039346656037ULL;
    for(unsigned char c:value){hash^=c;hash*=1099511628211ULL;}
    return hash;
}
inline Result select(const std::vector<Origin>& origins,bool balance,
                     const std::string& clientKey) {
    std::vector<const Origin*> healthy;
    for(const auto& origin:origins)
        if(origin.live && !origin.redirect.empty())healthy.push_back(&origin);
    if(healthy.empty())return {};
    // Failover mode: local origin always wins when healthy.
    if(!balance){
        for(const auto* origin:healthy)
            if(origin->local)return {true,true,origin->redirect,origin->name};
    }
    // Balance only by actual connected sessions. The stable per-client
    // rendezvous score spreads tied low-load nodes without random flapping.
    const Origin* chosen=nullptr;
    for(const auto* origin:healthy) {
        const auto score=fingerprint(clientKey+"|"+origin->name+"|"+origin->redirect);
        const auto bestScore=chosen?fingerprint(clientKey+"|"+chosen->name+"|"+chosen->redirect):0;
        if(!chosen || origin->viewers<chosen->viewers ||
           (origin->viewers==chosen->viewers && score>bestScore))
            chosen=origin;
    }
    return {true,chosen->local,chosen->redirect,chosen->name};
}
inline bool safeChannelId(const std::string& id) {
    if(id.empty()||id.size()>128)return false;
    for(unsigned char c:id)
        if(!((c>='0'&&c<='9')||(c>='A'&&c<='Z')||
             (c>='a'&&c<='z')||c=='_'||c=='-'))return false;
    return true;
}
inline bool parseWatch(const std::string& raw,std::string& id,bool& hls) {
    id.clear();hls=false;
    const std::string path=raw.substr(0,raw.find('?'));
    constexpr const char* prefix="/cluster/watch/";
    if(path.compare(0,15,prefix)!=0)return false;
    const std::string suffix=path.substr(15);
    if(suffix.size()>5 && suffix.compare(suffix.size()-5,5,".m3u8")==0){
        hls=true;id=suffix.substr(0,suffix.size()-5);
    }else if(suffix.size()>3 && suffix.compare(suffix.size()-3,3,".ts")==0){
        id=suffix.substr(0,suffix.size()-3);
    }else return false;
    if(!safeChannelId(id)){id.clear();return false;}
    return true;
}
} // namespace dvbstreamer5::cluster::routing
