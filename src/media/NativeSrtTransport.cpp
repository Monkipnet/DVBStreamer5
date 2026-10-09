#include "media/NativeSrtTransport.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <iomanip>
#include <memory>
#include <netdb.h>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>

namespace dvbstreamer5::media::srt {
namespace {
using SrtSocket = std::int32_t;
constexpr SrtSocket kInvalidSocket = -1;
constexpr int kSrtError = -1;
constexpr int kSrtListenerBacklog = 32;

enum SrtSockOpt : int {
    SRTO_SNDSYN=1, SRTO_RCVSYN=2, SRTO_FC=4, SRTO_SNDBUF=5, SRTO_RCVBUF=6,
    SRTO_RENDEZVOUS=12, SRTO_SNDTIMEO=13, SRTO_RCVTIMEO=14, SRTO_SENDER=21,
    SRTO_TSBPDMODE=22, SRTO_LATENCY=23, SRTO_PASSPHRASE=26, SRTO_PBKEYLEN=27,
    SRTO_CONNTIMEO=36, SRTO_RCVLATENCY=43, SRTO_PEERLATENCY=44, SRTO_STREAMID=46,
    SRTO_MESSAGEAPI=48, SRTO_PAYLOADSIZE=49, SRTO_TRANSTYPE=50
};
constexpr int SRTT_LIVE = 0;
constexpr int SRTS_CONNECTED = 5;

extern "C" {
int srt_startup();
int srt_cleanup();
SrtSocket srt_create_socket();
int srt_close(SrtSocket);
int srt_setsockopt(SrtSocket, int, int, const void*, int);
int srt_bind(SrtSocket, const sockaddr*, int);
int srt_listen(SrtSocket, int);
SrtSocket srt_accept(SrtSocket, sockaddr*, int*);
int srt_connect(SrtSocket, const sockaddr*, int);
int srt_sendmsg(SrtSocket, const char*, int, int, int);
int srt_recvmsg(SrtSocket, char*, int);
const char* srt_getlasterror_str();
std::uint32_t srt_getversion();
int srt_getsockstate(SrtSocket);
void srt_setloglevel(int);
}

struct SrtApi {
    using FnStartup=int(*)(); using FnCleanup=int(*)(); using FnCreate=SrtSocket(*)();
    using FnClose=int(*)(SrtSocket); using FnSet=int(*)(SrtSocket,int,int,const void*,int);
    using FnBind=int(*)(SrtSocket,const sockaddr*,int); using FnListen=int(*)(SrtSocket,int);
    using FnAccept=SrtSocket(*)(SrtSocket,sockaddr*,int*); using FnConnect=int(*)(SrtSocket,const sockaddr*,int);
    using FnSend=int(*)(SrtSocket,const char*,int,int,int); using FnRecv=int(*)(SrtSocket,char*,int);
    using FnError=const char*(*)(); using FnVersion=std::uint32_t(*)(); using FnState=int(*)(SrtSocket);
    using FnLog=void(*)(int);

    FnStartup startup=&srt_startup; FnCleanup cleanup=&srt_cleanup; FnCreate create=&srt_create_socket;
    FnClose close=&srt_close; FnSet set=&srt_setsockopt; FnBind bind=&srt_bind; FnListen listen=&srt_listen;
    FnAccept accept=&srt_accept; FnConnect connect=&srt_connect; FnSend send=&srt_sendmsg; FnRecv recv=&srt_recvmsg;
    FnError error=&srt_getlasterror_str; FnVersion version=&srt_getversion; FnState state=&srt_getsockstate;
    FnLog log=&srt_setloglevel;
    bool initialized=false;
    std::string libraryName="built-in:SRT 1.5.7/OpenSSL-EVP (vendored source)";
    std::string loadError;

    ~SrtApi() { if (initialized && cleanup) cleanup(); }

    bool load(){
        if(initialized) return true;
        if(startup()!=0){ loadError="built-in srt_startup failed"; return false; }
        if(log) log(2);
        constexpr std::uint32_t min=0x010507;
        const std::uint32_t actual=version();
        if(actual<min){
            std::ostringstream o;
            o<<"built-in SRT runtime 0x"<<std::hex<<actual<<" is older than required 1.5.7";
            loadError=o.str(); cleanup(); return false;
        }
        initialized=true;
        return true;
    }
    std::string lastError()const{ const char* e=error?error():nullptr; return e&&*e?e:"unknown SRT error"; }
};
SrtApi& api(){ static SrtApi a; static bool loaded=a.load(); (void)loaded; return a; }

bool setOpt(SrtApi& a,SrtSocket s,int opt,const void* v,int len,std::string& err,const char* label){ if(a.set(s,0,opt,v,len)==0)return true; err=std::string("SRT option ")+label+" failed: "+a.lastError(); return false; }
template<class T> bool setOpt(SrtApi& a,SrtSocket s,int opt,const T& v,std::string& err,const char* label){ return setOpt(a,s,opt,&v,sizeof(v),err,label); }

std::string lower(std::string v){ for(char& c:v)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return v; }
int toInt(const std::string& v,int d,int lo,int hi){ try{std::size_t u=0; long x=std::stol(v,&u); return u==v.size()&&x>=lo&&x<=hi?static_cast<int>(x):d;}catch(...){return d;} }
std::string decode(const std::string& v){ std::string o; auto h=[](char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}; for(std::size_t i=0;i<v.size();++i){if(v[i]=='%'&&i+2<v.size()){int a=h(v[i+1]),b=h(v[i+2]);if(a>=0&&b>=0){o.push_back(static_cast<char>((a<<4)|b));i+=2;continue;}}o.push_back(v[i]=='+'?' ':v[i]);}return o;}
std::string encode(const std::string& v){ static const char* H="0123456789ABCDEF"; std::string o; for(unsigned char c:v){if(std::isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~')o.push_back(static_cast<char>(c));else{o.push_back('%');o.push_back(H[c>>4]);o.push_back(H[c&15]);}}return o;}
std::vector<std::pair<std::string,std::string>> query(const std::string& q){ std::vector<std::pair<std::string,std::string>> r; std::size_t p=0; while(p<=q.size()){auto a=q.find('&',p),e=a==std::string::npos?q.size():a;auto t=q.substr(p,e-p);if(!t.empty()){auto z=t.find('=');r.push_back({decode(t.substr(0,z)),z==std::string::npos?"":decode(t.substr(z+1))});}if(a==std::string::npos)break;p=a+1;}return r;}
std::string qv(const std::vector<std::pair<std::string,std::string>>& q,const std::string& k){for(auto& x:q)if(lower(x.first)==lower(k))return x.second;return{};}

bool resolve(const std::string& host,int port,bool passive,sockaddr_storage& storage,socklen_t& length,std::string& err){addrinfo hints{};hints.ai_family=AF_UNSPEC;hints.ai_socktype=SOCK_DGRAM;hints.ai_flags=passive?AI_PASSIVE:0;addrinfo* res=nullptr;std::string service=std::to_string(port);int rc=getaddrinfo(host.empty()?nullptr:host.c_str(),service.c_str(),&hints,&res);if(rc!=0||!res){err=std::string("SRT resolve failed: ")+gai_strerror(rc);return false;}std::memcpy(&storage,res->ai_addr,res->ai_addrlen);length=static_cast<socklen_t>(res->ai_addrlen);freeaddrinfo(res);return true;}
std::string peerIp(const sockaddr_storage& s){char h[NI_MAXHOST]{};socklen_t n=s.ss_family==AF_INET?sizeof(sockaddr_in):sizeof(sockaddr_in6);return getnameinfo(reinterpret_cast<const sockaddr*>(&s),n,h,sizeof(h),nullptr,0,NI_NUMERICHOST)==0?h:"";}

bool configure(SrtApi& a,SrtSocket s,const EndpointConfig& c,bool sender,bool listener,std::string& err){
    int one=1, zero=0, live=SRTT_LIVE;
    if(!setOpt(a,s,SRTO_TRANSTYPE,live,err,"live transport"))return false;
    if(!setOpt(a,s,SRTO_MESSAGEAPI,one,err,"message api"))return false;
    if(!setOpt(a,s,SRTO_TSBPDMODE,one,err,"TSBPD"))return false;
    int sndsyn=1, rcvsyn=1; if(listener){sndsyn=0;rcvsyn=0;}
    if(!setOpt(a,s,SRTO_SNDSYN,sndsyn,err,"send mode")||!setOpt(a,s,SRTO_RCVSYN,rcvsyn,err,"receive mode"))return false;
    int senderFlag=sender?1:0; if(!setOpt(a,s,SRTO_SENDER,senderFlag,err,"sender"))return false;
    int latency=std::clamp(c.latencyMs,20,60000); if(!setOpt(a,s,SRTO_LATENCY,latency,err,"latency"))return false;
    if(c.receiveLatencyMs>0){int x=std::clamp(c.receiveLatencyMs,20,60000);if(!setOpt(a,s,SRTO_RCVLATENCY,x,err,"rcvlatency"))return false;}
    if(c.peerLatencyMs>0){int x=std::clamp(c.peerLatencyMs,20,60000);if(!setOpt(a,s,SRTO_PEERLATENCY,x,err,"peerlatency"))return false;}
    int con=std::clamp(c.connectTimeoutMs,100,60000), io=std::clamp(c.ioTimeoutMs,100,60000), payload=std::clamp(c.payloadSize,188,1456);
    setOpt(a,s,SRTO_CONNTIMEO,con,err,"connect timeout"); setOpt(a,s,SRTO_SNDTIMEO,io,err,"send timeout"); setOpt(a,s,SRTO_RCVTIMEO,io,err,"receive timeout"); setOpt(a,s,SRTO_PAYLOADSIZE,payload,err,"payload size");
    if(c.receiveBufferBytes>0&&!setOpt(a,s,SRTO_RCVBUF,c.receiveBufferBytes,err,"receive buffer"))return false;
    if(c.sendBufferBytes>0&&!setOpt(a,s,SRTO_SNDBUF,c.sendBufferBytes,err,"send buffer"))return false;
    if(c.flightWindowPackets>0&&!setOpt(a,s,SRTO_FC,c.flightWindowPackets,err,"flight window"))return false;
    if(!c.passphrase.empty()){
        if(c.passphrase.size()<10||c.passphrase.size()>79){err="SRT passphrase must contain 10..79 characters";return false;}
        if(!setOpt(a,s,SRTO_PASSPHRASE,c.passphrase.data(),static_cast<int>(c.passphrase.size()),err,"passphrase"))return false;
        int key=c.pbkeylen?c.pbkeylen:16;if(key!=16&&key!=24&&key!=32){err="SRT pbkeylen must be 16, 24 or 32";return false;} if(!setOpt(a,s,SRTO_PBKEYLEN,key,err,"pbkeylen"))return false;
    }
    if(!c.streamId.empty()&&!listener&&!setOpt(a,s,SRTO_STREAMID,c.streamId.data(),static_cast<int>(c.streamId.size()),err,"streamid"))return false;
    (void)zero; return true;
}

struct Socket { SrtSocket s=kInvalidSocket; ~Socket(){reset();} void reset(SrtSocket n=kInvalidSocket){if(s!=kInvalidSocket&&api().close)api().close(s);s=n;} };
void pause(const std::atomic<bool>& stop){for(int i=0;i<10&&!stop.load();++i)std::this_thread::sleep_for(std::chrono::milliseconds(100));}

bool createListenerSocket(const EndpointConfig& c,Socket& out,std::string& err){
    auto& a=api(); SrtSocket s=a.create(); if(s==kInvalidSocket){err="srt_create_socket failed: "+a.lastError();return false;} out.reset(s);
    if(!configure(a,s,c,true,true,err))return false;
    sockaddr_storage bindAddr{};socklen_t bindLen=0;std::string bindHost=c.bindAddress.empty()?c.host:c.bindAddress;if(bindHost.empty()||bindHost=="@")bindHost="0.0.0.0";
    if(!resolve(bindHost,c.port,true,bindAddr,bindLen,err))return false;
    if(a.bind(s,reinterpret_cast<sockaddr*>(&bindAddr),bindLen)==kSrtError){err="SRT bind failed: "+a.lastError();return false;}
    if(a.listen(s,kSrtListenerBacklog)==kSrtError){err="SRT listen failed: "+a.lastError();return false;}
    err.clear();return true;
}

bool establishConnection(const EndpointConfig& c,bool sender,Socket& out,std::string& peer,const std::atomic<bool>& stop,std::string& err){
    auto& a=api(); SrtSocket s=a.create(); if(s==kInvalidSocket){err="srt_create_socket failed: "+a.lastError();return false;} out.reset(s);
    bool listener=lower(c.mode)=="listener";
    if(!configure(a,s,c,sender,listener,err))return false;
    if(listener){
        sockaddr_storage bindAddr{};socklen_t bindLen=0;std::string bindHost=c.bindAddress.empty()?c.host:c.bindAddress;if(bindHost.empty()||bindHost=="@")bindHost="0.0.0.0";
        if(!resolve(bindHost,c.port,true,bindAddr,bindLen,err))return false;
        if(a.bind(s,reinterpret_cast<sockaddr*>(&bindAddr),bindLen)==kSrtError){err="SRT bind failed: "+a.lastError();return false;}
        if(a.listen(s,1)==kSrtError){err="SRT listen failed: "+a.lastError();return false;}
        while(!stop.load()){
            sockaddr_storage remote{};int len=sizeof(remote);SrtSocket client=a.accept(s,reinterpret_cast<sockaddr*>(&remote),&len);
            if(client!=kInvalidSocket){peer=peerIp(remote);out.reset(client);return true;}
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        err="SRT listener stopped";return false;
    }
    sockaddr_storage remote{};socklen_t remoteLen=0;if(!resolve(c.host,c.port,false,remote,remoteLen,err))return false;
    if(!c.bindAddress.empty()) { sockaddr_storage local{};socklen_t localLen=0;if(resolve(c.bindAddress,0,true,local,localLen,err)&&a.bind(s,reinterpret_cast<sockaddr*>(&local),localLen)==kSrtError){err="SRT local bind failed: "+a.lastError();return false;} }
    if(a.connect(s,reinterpret_cast<sockaddr*>(&remote),remoteLen)==kSrtError){err="SRT connect failed: "+a.lastError();return false;}
    peer=peerIp(remote);return true;
}
} // namespace

bool parseUri(const std::string& uri,EndpointConfig& c,std::string& err){
    if(lower(uri).rfind("srt://",0)!=0){err="SRT URI must start with srt://";return false;} std::string rest=uri.substr(6);auto qm=rest.find('?');std::string authority=rest.substr(0,qm),qs=qm==std::string::npos?"":rest.substr(qm+1);std::string host;std::string port;
    if(!authority.empty()&&authority[0]=='['){auto e=authority.find(']');if(e==std::string::npos){err="invalid IPv6 SRT URI";return false;}host=authority.substr(1,e-1);if(e+1<authority.size()&&authority[e+1]==':')port=authority.substr(e+2);}else{auto col=authority.rfind(':');if(col==std::string::npos){err="SRT URI requires port";return false;}host=authority.substr(0,col);port=authority.substr(col+1);}
    c.host=host=="@"?"0.0.0.0":host;c.port=toInt(port,0,1,65535);if(!c.port){err="invalid SRT port";return false;}auto q=query(qs);std::string mode=lower(qv(q,"mode"));if(!mode.empty())c.mode=mode;if(c.mode!="caller"&&c.mode!="listener"){err="SRT mode must be caller or listener";return false;}
    c.latencyMs=toInt(qv(q,"latency"),c.latencyMs,20,60000);c.receiveLatencyMs=toInt(qv(q,"rcvlatency"),c.receiveLatencyMs,0,60000);c.peerLatencyMs=toInt(qv(q,"peerlatency"),c.peerLatencyMs,0,60000);c.connectTimeoutMs=toInt(qv(q,"conntimeo"),c.connectTimeoutMs,100,60000);c.ioTimeoutMs=toInt(qv(q,"poll-timeout"),c.ioTimeoutMs,100,60000);c.payloadSize=toInt(qv(q,"payloadsize"),c.payloadSize,188,1456);c.receiveBufferBytes=toInt(qv(q,"rcvbuf"),c.receiveBufferBytes,0,128*1024*1024);c.sendBufferBytes=toInt(qv(q,"sndbuf"),c.sendBufferBytes,0,128*1024*1024);c.flightWindowPackets=toInt(qv(q,"fc"),c.flightWindowPackets,0,1000000);c.passphrase=qv(q,"passphrase");c.pbkeylen=toInt(qv(q,"pbkeylen"),c.pbkeylen,0,32);c.streamId=qv(q,"streamid");c.bindAddress=qv(q,"bind");
    if(!c.passphrase.empty()&&(c.passphrase.size()<10||c.passphrase.size()>79)){err="SRT passphrase must contain 10..79 characters";return false;} if(c.pbkeylen&&c.pbkeylen!=16&&c.pbkeylen!=24&&c.pbkeylen!=32){err="SRT pbkeylen must be 16, 24 or 32";return false;}err.clear();return true;
}

std::string buildUri(const EndpointConfig& c,bool secrets){std::ostringstream o;o<<"srt://";if(c.host.find(':')!=std::string::npos)o<<'['<<c.host<<']';else o<<c.host;o<<':'<<c.port<<"?mode="<<c.mode<<"&latency="<<c.latencyMs;if(c.receiveLatencyMs)o<<"&rcvlatency="<<c.receiveLatencyMs;if(c.peerLatencyMs)o<<"&peerlatency="<<c.peerLatencyMs;if(!c.streamId.empty())o<<"&streamid="<<encode(c.streamId);if(!c.bindAddress.empty())o<<"&bind="<<encode(c.bindAddress);if(secrets&&!c.passphrase.empty())o<<"&passphrase="<<encode(c.passphrase)<<"&pbkeylen="<<(c.pbkeylen?c.pbkeylen:16);return o.str();}
bool runtimeAvailable(std::string* detail){auto& a=api();if(detail){if(a.initialized){std::ostringstream o;o<<a.libraryName<<" version=0x"<<std::hex<<a.version();*detail=o.str();}else *detail=a.loadError;}return a.initialized;}
std::uint32_t runtimeVersion(){auto& a=api();return a.initialized?a.version():0;}

NativeSrtInput::~NativeSrtInput(){stop();}
bool NativeSrtInput::start(const EndpointConfig& c,DataCallback data,StateCallback state,std::string& err){stop();if(!runtimeAvailable(&err))return false;if(!data){err="native SRT input requires callback";return false;}config_=c;onData_=std::move(data);onState_=std::move(state);stopRequested_=false;running_=true;receivedBytes_=0;reconnects_=0;setError({});try{worker_=std::thread(&NativeSrtInput::run,this);}catch(const std::exception& e){running_=false;err=e.what();return false;}err.clear();return true;}
void NativeSrtInput::stop()noexcept{stopRequested_=true;if(worker_.joinable())worker_.join();running_=false;}
void NativeSrtInput::setError(const std::string& v){std::lock_guard<std::mutex>l(errorMutex_);lastError_=v;}
std::string NativeSrtInput::lastError()const{std::lock_guard<std::mutex>l(errorMutex_);return lastError_;}
void NativeSrtInput::run(){std::vector<std::uint8_t>b(static_cast<std::size_t>(std::max(1316,config_.payloadSize))*8);bool first=true;while(!stopRequested_){Socket s;std::string peer,e;if(!establishConnection(config_,false,s,peer,stopRequested_,e)){if(stopRequested_)break;setError(e);if(onState_)onState_("reconnecting: "+e);++reconnects_;pause(stopRequested_);continue;}if(!first)++reconnects_;first=false;setError({});if(onState_)onState_("connected"+(peer.empty()?std::string():" "+peer));for(;;){if(stopRequested_)break;int n=api().recv(s.s,reinterpret_cast<char*>(b.data()),static_cast<int>(b.size()));if(n>0){receivedBytes_+=static_cast<std::uint64_t>(n);if(!onData_(b.data(),static_cast<std::size_t>(n))){stopRequested_=true;break;}continue;}if(api().state(s.s)==SRTS_CONNECTED)continue;setError("SRT receive disconnected: "+api().lastError());break;}if(!stopRequested_)pause(stopRequested_);}running_=false;}

NativeSrtOutput::~NativeSrtOutput(){stop();}
bool NativeSrtOutput::start(const EndpointConfig& c,PeerAllowedCallback allow,PeerStateCallback con,PeerStateCallback dis,std::string& err){stop();if(!runtimeAvailable(&err))return false;config_=c;peerAllowed_=std::move(allow);onConnected_=std::move(con);onDisconnected_=std::move(dis);stopRequested_=false;running_=true;connected_=false;sentBytes_=0;droppedBytes_=0;reconnects_=0;setError({});clearQueuedData();try{worker_=std::thread(&NativeSrtOutput::run,this);}catch(const std::exception&e){running_=false;err=e.what();return false;}err.clear();return true;}
bool NativeSrtOutput::push(const std::uint8_t*d,std::size_t n){if(!d||!n||!running_)return false;constexpr std::size_t maxQ=4*1024*1024,chunk=64*1024;std::unique_lock<std::mutex>l(queueMutex_);while(queuedBytes_+n>maxQ&&!queue_.empty()){droppedBytes_+=queue_.front().size();queuedBytes_-=queue_.front().size();queue_.pop_front();}if(n>maxQ){std::size_t keep=maxQ-(maxQ%188);droppedBytes_+=n-keep;d+=n-keep;n=keep;}for(std::size_t p=0;p<n;){std::size_t z=std::min(chunk,n-p);queue_.emplace_back(d+p,d+p+z);queuedBytes_+=z;p+=z;}l.unlock();queueCondition_.notify_one();return true;}
void NativeSrtOutput::stop()noexcept{stopRequested_=true;queueCondition_.notify_all();if(worker_.joinable())worker_.join();running_=false;connected_=false;clearQueuedData();}
void NativeSrtOutput::setError(const std::string&v){std::lock_guard<std::mutex>l(errorMutex_);lastError_=v;}
std::string NativeSrtOutput::lastError()const{std::lock_guard<std::mutex>l(errorMutex_);return lastError_;}
bool NativeSrtOutput::popChunk(std::vector<std::uint8_t>&c){std::unique_lock<std::mutex>l(queueMutex_);queueCondition_.wait_for(l,std::chrono::milliseconds(250),[&]{return stopRequested_||!queue_.empty();});if(stopRequested_)return false;if(queue_.empty()){c.clear();return true;}c=std::move(queue_.front());queuedBytes_-=c.size();queue_.pop_front();return true;}
void NativeSrtOutput::clearQueuedData(){std::lock_guard<std::mutex>l(queueMutex_);for(auto&c:queue_)droppedBytes_+=c.size();queue_.clear();queuedBytes_=0;}
void NativeSrtOutput::run(){
    if(lower(config_.mode)=="listener"){
        struct Peer{SrtSocket socket=kInvalidSocket;std::string ip;};
        Socket listener;std::string e;if(!createListenerSocket(config_,listener,e)){setError(e);running_=false;connected_=false;return;}
        std::vector<Peer> peers;std::vector<std::uint8_t> pending;bool first=true;
        std::size_t payload=static_cast<std::size_t>(std::max(188,config_.payloadSize));payload-=payload%188;if(!payload)payload=188;
        auto removePeer=[&](std::size_t i,const std::string& reason){if(i>=peers.size())return;const std::string ip=peers[i].ip;if(peers[i].socket!=kInvalidSocket)api().close(peers[i].socket);peers.erase(peers.begin()+static_cast<std::ptrdiff_t>(i));connected_=!peers.empty();if(!reason.empty())setError(reason);if(onDisconnected_)onDisconnected_(ip);};
        while(!stopRequested_){
            for(;;){sockaddr_storage remote{};int len=sizeof(remote);SrtSocket client=api().accept(listener.s,reinterpret_cast<sockaddr*>(&remote),&len);if(client==kInvalidSocket)break;std::string peer=peerIp(remote);if(peerAllowed_&&!peerAllowed_(peer)){api().close(client);setError("SRT peer rejected: "+peer);continue;}if(!first)++reconnects_;first=false;peers.push_back({client,peer});connected_=true;setError({});if(onConnected_)onConnected_(peer);}
            std::vector<std::uint8_t> c;if(!popChunk(c))break;
            for(std::size_t i=0;i<peers.size();){if(api().state(peers[i].socket)==SRTS_CONNECTED){++i;continue;}removePeer(i,"SRT listener peer disconnected");}
            if(peers.empty()){droppedBytes_+=c.size()+pending.size();pending.clear();continue;}
            if(c.empty())continue;
            pending.insert(pending.end(),c.begin(),c.end());std::size_t off=0;
            while(pending.size()-off>=payload){const char* message=reinterpret_cast<const char*>(pending.data()+off);for(std::size_t i=0;i<peers.size();){int n=api().send(peers[i].socket,message,static_cast<int>(payload),-1,1);if(n==static_cast<int>(payload)){sentBytes_+=static_cast<std::uint64_t>(n);++i;continue;}std::string reason=n<=0?"SRT send disconnected peer="+peers[i].ip+": "+api().lastError():"partial SRT MPEG-TS message peer="+peers[i].ip;droppedBytes_+=payload;removePeer(i,reason);}off+=payload;if(peers.empty()){droppedBytes_+=pending.size()-off;off=pending.size();break;}}
            if(off)pending.erase(pending.begin(),pending.begin()+static_cast<std::ptrdiff_t>(off));
        }
        for(auto& peer:peers){if(peer.socket!=kInvalidSocket)api().close(peer.socket);if(onDisconnected_)onDisconnected_(peer.ip);}connected_=false;droppedBytes_+=pending.size();clearQueuedData();running_=false;return;
    }
    bool first=true;std::vector<std::uint8_t>pending;while(!stopRequested_){Socket s;std::string peer,e;if(!establishConnection(config_,true,s,peer,stopRequested_,e)){if(stopRequested_)break;setError(e);++reconnects_;clearQueuedData();pause(stopRequested_);continue;}if(peerAllowed_&&!peerAllowed_(peer)){setError("SRT peer rejected: "+peer);clearQueuedData();pause(stopRequested_);continue;}if(!first)++reconnects_;first=false;connected_=true;setError({});if(onConnected_)onConnected_(peer);bool lost=false;pending.clear();while(!stopRequested_&&!lost){std::vector<std::uint8_t>c;if(!popChunk(c))break;if(c.empty()){if(api().state(s.s)!=SRTS_CONNECTED)lost=true;continue;}pending.insert(pending.end(),c.begin(),c.end());std::size_t payload=static_cast<std::size_t>(std::max(188,config_.payloadSize));payload-=payload%188;if(!payload)payload=188;std::size_t off=0;while(pending.size()-off>=payload){int n=api().send(s.s,reinterpret_cast<const char*>(pending.data()+off),static_cast<int>(payload),-1,1);if(n!=static_cast<int>(payload)){setError(n<=0?"SRT send disconnected: "+api().lastError():"partial SRT MPEG-TS message");lost=true;break;}sentBytes_+=static_cast<std::uint64_t>(n);off+=payload;}if(off)pending.erase(pending.begin(),pending.begin()+static_cast<std::ptrdiff_t>(off));}connected_=false;if(onDisconnected_)onDisconnected_(peer);droppedBytes_+=pending.size();pending.clear();clearQueuedData();if(!stopRequested_)pause(stopRequested_);}running_=false;connected_=false;
}

} // namespace dvbstreamer5::media::srt
