#include "../src/media/DvbFrontendOwners.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#ifdef __linux__
#include <unistd.h>
#endif
int main(){
#ifdef __linux__
namespace fs=std::filesystem;
auto root=fs::temp_directory_path()/("dvb-owners-test-"+std::to_string(::getpid()));
fs::remove_all(root);
auto add=[&](int pid,int adapter,const char* flags,const char* name){
 auto dir=root/std::to_string(pid);
 fs::create_directories(dir/"fd");fs::create_directories(dir/"fdinfo");
 fs::create_symlink("/dev/dvb/adapter"+std::to_string(adapter)+"/frontend0",dir/"fd"/"9");
 std::ofstream(dir/"fdinfo"/"9")<<"flags:\t"<<flags<<"\n";
 std::ofstream(dir/"comm")<<name<<"\n";
};
add(310001,0,"0100002","tvheadend");
add(310002,0,"0100000","signal-observer");
add(310003,1,"0100002","dvblast");
const auto owners=dvbstreamer5::media::network::externalDvbFrontendOwners(0,0,root,-1);
assert(owners.size()==2&&owners[0].writer&&!owners[1].writer);
assert(owners[0].pid==310001&&owners[0].name=="tvheadend");
const auto noSelf=dvbstreamer5::media::network::externalDvbFrontendOwners(0,0,root,310001);
assert(noSelf.size()==1&&!noSelf[0].writer);
assert(dvbstreamer5::media::network::externalDvbFrontendOwners(2,0,root,-1).empty());
fs::remove_all(root);
#endif
std::cout<<"DVB external owner test OK\n";
}
