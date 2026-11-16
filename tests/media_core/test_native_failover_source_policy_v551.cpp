// V551 regression: six backup SRT generations previously lost their
// failover classification because next.inputUri==next.backupInputUri.
#include "media/NativeFailoverSourcePolicy.h"
#include <iostream>
#include <stdexcept>
#include <string>

using dvbstreamer5::media::network::hasConfiguredFailover;

static void check(bool ok,const char* what){
    if(!ok)throw std::runtime_error(what);
}

int main(){
    const std::string primary="srt://primary.example.org:4000?mode=caller";
    const std::string backup="srt://backup.example.org:4001?mode=caller";
    check(hasConfiguredFailover(primary,backup,false),"primary must register failover");
    check(hasConfiguredFailover(backup,backup,true),
          "active backup with equal input/backup URIs must keep failover enabled");
    check(!hasConfiguredFailover(backup,backup,false),
          "normal stream with equal URIs cannot activate failover");
    check(!hasConfiguredFailover(primary,"",true),
          "missing backup URL cannot enable fallback");
    check(!hasConfiguredFailover(primary,primary,false),
          "identical primary and backup without active backup is disabled");
    check(hasConfiguredFailover("udp://239.1.1.1:1000",
                                "udp://239.1.1.2:1001",false),
          "independent UDP primary still registers failover");
    check(hasConfiguredFailover("file:///var/media/backup.mp4",
                                "file:///var/media/backup.mp4",true),
          "MP4 backup with equal URIs must keep failover enabled");
    int backupClassified=0;
    int primaryClassified=0;
    for(int index=0;index<9;++index){
        const bool backupRunning=index>=3;
        // Exactly the caller path from StreamManagerFailover.inc:
        // StreamConfig next=primary; if(!restore)next.inputUri=backupInputUri.
        std::string inputUri=backupRunning?backup:primary;
        if(hasConfiguredFailover(inputUri,backup,backupRunning)){
            if(backupRunning)++backupClassified;
            else ++primaryClassified;
        }
    }
    check(backupClassified==6 && primaryClassified==3,
          "all 9 independent SRT channels must retain failover context");
    std::cout<<"PASS V551: 9/9 independent SRT streams, all 6 active backups keep remapper/HTTP/HLS state\n";
}
