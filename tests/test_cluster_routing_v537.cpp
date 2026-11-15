// V537: tuner-free three-node origin/router emulation.
// Simulate HTTP MPEG-TS / HLS viewers, primary outage, backup recovery,
// active connection balancing and stable tie-breaks.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "http/ClusterViewerRouting.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <set>
#include <string>
#include <vector>
using dvbstreamer5::cluster::routing::Origin;
using dvbstreamer5::cluster::routing::select;
using dvbstreamer5::cluster::routing::parseWatch;
int main(){
    std::vector<Origin> ts{
      {"streamer","http://10.8.0.1:9000/Sport_TV/mpegts",12,true,true},
      {"iptv","http://10.8.0.2:9000/Sport_TV/mpegts",2,false,true},
      {"home2","https://home2.example.net/Sport_TV/mpegts",5,false,true}
    };
    const auto fallback=select(ts,false,"192.168.1.100");
    assert(fallback.found && fallback.local && fallback.name=="streamer");
    const auto balanced=select(ts,true,"192.168.1.100");
    assert(balanced.found && !balanced.local && balanced.name=="iptv");

    // Primary fails. New clients are redirected to the least loaded backup.
    ts[0].live=false;
    const auto failover=select(ts,false,"192.168.1.100");
    assert(failover.found && failover.name=="iptv");
    assert(failover.redirect.find("/cluster/watch/")==std::string::npos);
    ts[1].live=false;
    assert(select(ts,false,"192.168.1.100").name=="home2");
    ts[2].live=false;
    assert(!select(ts,false,"192.168.1.100").found);
    // Primary recovers, new clients use it again in failover mode.
    ts[0].live=true;ts[1].live=true;ts[2].live=true;
    assert(select(ts,false,"192.168.1.100").name=="streamer");
    ts[0].viewers=1;ts[1].viewers=2;ts[2].viewers=5;
    assert(select(ts,true,"192.168.1.100").name=="streamer");
    ts[2].viewers=0;
    assert(select(ts,true,"192.168.1.100").name=="home2");
    // Equal load: stable client affinity per client key.
    for(auto& node:ts)node.viewers=3;
    std::set<std::string> distribution;
    for(int i=1;i<=200;++i){
        const auto ip="192.168.1."+std::to_string(i);
        auto route=select(ts,true,ip);
        assert(route.found);
        assert(select(ts,true,ip).name==route.name);
        distribution.insert(route.name);
    }
    assert(distribution.size()==3);

    std::string id;bool hls=false;
    assert(parseWatch("/cluster/watch/Sport_TV.ts",id,hls));
    assert(id=="Sport_TV"&&!hls);
    assert(parseWatch("/cluster/watch/Sport_TV.m3u8?token=signed",id,hls));
    assert(id=="Sport_TV"&&hls);
    assert(!parseWatch("/cluster/watch/../../admin.ts",id,hls));
    assert(!parseWatch("/cluster/watch/%2e%2e.ts",id,hls));
    assert(!parseWatch("/cluster/watch/Sport_TV.m3u8/../../",id,hls));
    assert(!parseWatch("/cluster/watch/channel%2fsecret.ts",id,hls));
    assert(!parseWatch("/cluster/watch/.ts",id,hls));
    assert(!parseWatch("/cluster/watch/long.ts/extra",id,hls));
    assert(parseWatch("/cluster/watch/foo.m3u8?x=1",id,hls));
    std::cout<<"PASS V537: simulated 3-node TS/HLS failover, viewer balancing, outage/recovery, stable affinity and URL constraints\n";
}
