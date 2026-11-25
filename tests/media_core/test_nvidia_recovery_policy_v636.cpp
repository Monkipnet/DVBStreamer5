#include "media/NativeNvidiaRecoveryPolicy.h"
#include <cassert>
#include <iostream>
using namespace dvbstreamer5::media::codec;
int main(){
    NvidiaSequenceKey key{1,1,0,0,1920,1088,0,16,0,0,1920,1080};
    assert(nvidiaSequenceAction(false,{},key)==NvidiaSequenceAction::Create);
    assert(nvidiaSequenceAction(true,key,key)==NvidiaSequenceAction::Reuse);
    auto changed=key; changed.width=1280;
    assert(nvidiaSequenceAction(true,key,changed)==NvidiaSequenceAction::RejectChange);
    changed=key; changed.surfaces=20;
    assert(nvidiaSequenceAction(true,key,changed)==NvidiaSequenceAction::RejectChange);
    changed=key; changed.progressive=1;
    assert(nvidiaSequenceAction(true,key,changed)==NvidiaSequenceAction::RejectChange);
    assert(videoRecoveryAction(false,false)==VideoRecoveryAction::Continue);
    assert(videoRecoveryAction(true,false)==VideoRecoveryAction::DropUntilRandomAccess);
    assert(videoRecoveryAction(true,true)==VideoRecoveryAction::Recover);
    for(int stream=0;stream<64;++stream){
        int recoveries=0,recreations=0;
        bool pending=false;
        for(int frame=0;frame<1000;++frame){
            if(frame%37==0)pending=true;
            const auto action=videoRecoveryAction(pending,frame%25==0);
            if(action==VideoRecoveryAction::Recover){
                ++recoveries; pending=false;
                if(nvidiaSequenceAction(true,key,key)!=NvidiaSequenceAction::Reuse)++recreations;
            }
        }
        assert(recoveries>0 && recreations==0);
    }
    std::cout<<"PASS NVIDIA recovery decision policy (64 simulated streams)\n";
}
