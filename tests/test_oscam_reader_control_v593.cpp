#ifdef NDEBUG
#undef NDEBUG
#endif
#include "OscamReaderControl.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace dvbstreamer5::oscam_reader_control;
int main() {
    assert(validLabel("remote_reader_01"));
    assert(validLabel("pcsc.0"));
    assert(validLabel("reader-1@remote"));
    assert(validLabel(std::string(64, 'Z')));
    // The OSCam-mini daemon accepts ASCII only, irrespective of C++ locale.
    assert(!validLabel(std::string("\xC3\xA9")));
    assert(!validLabel(std::string(1, static_cast<char>(0xFF))));
    assert(!validLabel("name\treader"));
    assert(!validLabel(""));
    assert(!validLabel("a; systemctl restart oscam-mini"));
    assert(!validLabel("one\nrestart other"));
    assert(!validLabel(std::string(65,'x')));
    std::string error;
    assert(!restartReader("bad label",error));
    assert(error=="Invalid OSCam reader label");
    const auto sock=std::string("/tmp/oscam-reader-v593-")+std::to_string(getpid())+".sock";
    int server=::socket(AF_UNIX,SOCK_SEQPACKET,0);
    assert(server>=0);
    sockaddr_un addr{};
    addr.sun_family=AF_UNIX;
    std::strncpy(addr.sun_path,sock.c_str(),sizeof(addr.sun_path)-1);
    assert(::bind(server,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0);
    assert(::listen(server,2)==0);
    std::thread daemon([&] {
        for(int i=0;i<2;++i){
            int peer=::accept(server,nullptr,nullptr);
            assert(peer>=0);
            char buf[128]{};
            const auto n=::recv(peer,buf,sizeof(buf)-1,0);
            assert(n>0);
            const std::string command(buf,static_cast<size_t>(n));
            if(i==0){
                assert(command=="restart remote_reader_01");
                assert(::send(peer,"OK queued",9,0)==9);
            }else{
                assert(command=="restart other_reader");
                const char* response="ERROR reader not found or disabled";
                assert(::send(peer,response,std::strlen(response),0)==
                    static_cast<ssize_t>(std::strlen(response)));
            }
            ::close(peer);
        }
    });
    assert(restartReader("remote_reader_01",error,sock.c_str()));
    assert(error.empty());
    assert(!restartReader("other_reader",error,sock.c_str()));
    assert(error=="reader not found or disabled");
    daemon.join();
    ::close(server);
    ::unlink(sock.c_str());
    std::cout<<"PASS V593: one-reader local restart IPC; no shell; invalid input rejected\n";
}
