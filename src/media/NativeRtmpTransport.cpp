#include "media/NativeRtmpTransport.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace dvbstreamer5::media::rtmp {
namespace {

using Clock = std::chrono::steady_clock;

std::string lower(std::string v){std::transform(v.begin(),v.end(),v.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});return v;}

struct Url { bool tls=false; std::string host; int port=1935; std::string app; std::string stream; std::string tcUrl; };
bool parseUrl(const std::string& uri, Url& u){
    const auto p=uri.find("://"); if(p==std::string::npos)return false; const std::string scheme=lower(uri.substr(0,p));
    if(scheme!="rtmp"&&scheme!="rtmps")return false;u.tls=scheme=="rtmps";u.port=u.tls?443:1935;
    const std::size_t a=p+3; const auto slash=uri.find('/',a); std::string authority=uri.substr(a,slash==std::string::npos?std::string::npos:slash-a);
    if(!authority.empty()&&authority.front()=='['){const auto close=authority.find(']');if(close==std::string::npos)return false;u.host=authority.substr(1,close-1);if(close+1<authority.size()&&authority[close+1]==':'){try{u.port=std::stoi(authority.substr(close+2));}catch(...){return false;}}}
    else{const auto col=authority.rfind(':');if(col!=std::string::npos&&authority.find(':')==col){u.host=authority.substr(0,col);try{u.port=std::stoi(authority.substr(col+1));}catch(...){return false;}}else u.host=authority;}
    if(u.host.empty()||slash==std::string::npos)return false;std::string path=uri.substr(slash+1);const auto q=path.find('?');if(q!=std::string::npos)path.erase(q);const auto s=path.find('/');if(s==std::string::npos){u.app=path;u.stream="stream";}else{u.app=path.substr(0,s);u.stream=path.substr(s+1);}if(u.app.empty()||u.stream.empty())return false;
    u.tcUrl=scheme+"://"+u.host+":"+std::to_string(u.port)+"/"+u.app;return true;
}

class Io {
public:
    ~Io(){close();}
    bool connect(const Url& u,int timeout,std::atomic<bool>*stop,std::string&error){
        close();addrinfo h{};h.ai_socktype=SOCK_STREAM;h.ai_family=AF_UNSPEC;addrinfo*l=nullptr;const int rc=::getaddrinfo(u.host.c_str(),std::to_string(u.port).c_str(),&h,&l);if(rc){error=gai_strerror(rc);return false;}
        for(auto*ai=l;ai&&fd_<0;ai=ai->ai_next){int s=::socket(ai->ai_family,SOCK_STREAM|SOCK_CLOEXEC,ai->ai_protocol);if(s<0)continue;const int f=::fcntl(s,F_GETFL,0);::fcntl(s,F_SETFL,f|O_NONBLOCK);if(::connect(s,ai->ai_addr,ai->ai_addrlen)==0)fd_=s;else if(errno==EINPROGRESS){int left=timeout;while(left>0&&!(stop&&stop->load())){pollfd p{s,POLLOUT,0};const int slice=std::min(left,200);const int pr=::poll(&p,1,slice);left-=slice;if(pr>0){int so=0;socklen_t sl=sizeof(so);::getsockopt(s,SOL_SOCKET,SO_ERROR,&so,&sl);if(!so)fd_=s;break;}if(pr<0&&errno!=EINTR)break;}}if(fd_<0)::close(s);}
        ::freeaddrinfo(l);if(fd_<0){error="RTMP connect failed";return false;}const int f=::fcntl(fd_,F_GETFL,0);::fcntl(fd_,F_SETFL,f&~O_NONBLOCK);
        if(u.tls){ctx_=SSL_CTX_new(TLS_client_method());if(!ctx_){error="RTMPS SSL_CTX failed";close();return false;}SSL_CTX_set_default_verify_paths(ctx_);ssl_=SSL_new(ctx_);SSL_set_fd(ssl_,fd_);SSL_set_tlsext_host_name(ssl_,u.host.c_str());if(SSL_connect(ssl_)!=1){error="RTMPS TLS handshake failed";close();return false;}}
        return true;
    }
    void close(){if(ssl_){SSL_shutdown(ssl_);SSL_free(ssl_);ssl_=nullptr;}if(ctx_){SSL_CTX_free(ctx_);ctx_=nullptr;}if(fd_>=0){::shutdown(fd_,SHUT_RDWR);::close(fd_);fd_=-1;}}
    bool write(const void*d,std::size_t n){const auto*p=static_cast<const std::uint8_t*>(d);std::size_t off=0;while(off<n){int r=ssl_?SSL_write(ssl_,p+off,static_cast<int>(n-off)):static_cast<int>(::send(fd_,p+off,n-off,MSG_NOSIGNAL));if(r>0){off+=r;continue;}if(!ssl_&&r<0&&errno==EINTR)continue;return false;}return true;}
    int read(void*d,std::size_t n,int timeout,std::atomic<bool>*stop){if(stop&&stop->load())return -1;pollfd p{fd_,POLLIN,0};const int pr=::poll(&p,1,timeout);if(pr<=0)return pr;int r=ssl_?SSL_read(ssl_,d,static_cast<int>(n)):static_cast<int>(::recv(fd_,d,n,0));return r;}
private:int fd_=-1;SSL_CTX*ctx_=nullptr;SSL*ssl_=nullptr;
};

void be24(std::vector<std::uint8_t>&v,std::uint32_t x){v.push_back(static_cast<std::uint8_t>(x>>16));v.push_back(static_cast<std::uint8_t>(x>>8));v.push_back(static_cast<std::uint8_t>(x));}
void be32(std::vector<std::uint8_t>&v,std::uint32_t x){v.push_back(static_cast<std::uint8_t>(x>>24));v.push_back(static_cast<std::uint8_t>(x>>16));v.push_back(static_cast<std::uint8_t>(x>>8));v.push_back(static_cast<std::uint8_t>(x));}
std::uint32_t read24(const std::uint8_t*p){return(static_cast<std::uint32_t>(p[0])<<16)|(static_cast<std::uint32_t>(p[1])<<8)|p[2];}
std::uint32_t read32(const std::uint8_t*p){return(static_cast<std::uint32_t>(p[0])<<24)|(static_cast<std::uint32_t>(p[1])<<16)|(static_cast<std::uint32_t>(p[2])<<8)|p[3];}

namespace amf {
void string(std::vector<std::uint8_t>&o,const std::string&s){o.push_back(2);o.push_back(static_cast<std::uint8_t>(s.size()>>8));o.push_back(static_cast<std::uint8_t>(s.size()));o.insert(o.end(),s.begin(),s.end());}
void number(std::vector<std::uint8_t>&o,double d){o.push_back(0);std::uint64_t bits=0;std::memcpy(&bits,&d,8);for(int i=7;i>=0;--i)o.push_back(static_cast<std::uint8_t>(bits>>(i*8)));}
void boolean(std::vector<std::uint8_t>&o,bool b){o.push_back(1);o.push_back(b?1:0);}void nullv(std::vector<std::uint8_t>&o){o.push_back(5);}void objectBegin(std::vector<std::uint8_t>&o){o.push_back(3);}void key(std::vector<std::uint8_t>&o,const std::string&s){o.push_back(static_cast<std::uint8_t>(s.size()>>8));o.push_back(static_cast<std::uint8_t>(s.size()));o.insert(o.end(),s.begin(),s.end());}void objectEnd(std::vector<std::uint8_t>&o){o.insert(o.end(),{0,0,9});}
struct Value{enum Type{Undefined,Number,String,Boolean,Object,Null}type=Undefined;double number=0;std::string str;bool boolean=false;std::map<std::string,Value>object;};
bool parse(const std::uint8_t*&p,const std::uint8_t*e,Value&v,int depth=0){if(p>=e||depth>8)return false;const std::uint8_t t=*p++;if(t==0){if(e-p<8)return false;std::uint64_t b=0;for(int i=0;i<8;++i)b=(b<<8)|*p++;std::memcpy(&v.number,&b,8);v.type=Value::Number;return true;}if(t==1){if(p>=e)return false;v.type=Value::Boolean;v.boolean=*p++!=0;return true;}if(t==2){if(e-p<2)return false;const std::size_t n=(p[0]<<8)|p[1];p+=2;if(static_cast<std::size_t>(e-p)<n)return false;v.type=Value::String;v.str.assign(reinterpret_cast<const char*>(p),n);p+=n;return true;}if(t==5||t==6){v.type=t==5?Value::Null:Value::Undefined;return true;}if(t==3||t==8){if(t==8){if(e-p<4)return false;p+=4;}v.type=Value::Object;while(e-p>=3){if(p[0]==0&&p[1]==0&&p[2]==9){p+=3;return true;}const std::size_t n=(p[0]<<8)|p[1];p+=2;if(static_cast<std::size_t>(e-p)<n)return false;std::string k(reinterpret_cast<const char*>(p),n);p+=n;Value child;if(!parse(p,e,child,depth+1))return false;v.object[k]=std::move(child);}return false;}return false;}
std::vector<Value> parseAll(const std::vector<std::uint8_t>&b){std::vector<Value>r;const auto*p=b.data();const auto*e=p+b.size();while(p<e){Value v;if(!parse(p,e,v))break;r.push_back(std::move(v));}return r;}
}

struct Message{std::uint32_t timestamp=0;std::uint32_t streamId=0;std::uint8_t type=0;std::vector<std::uint8_t>body;};

class ChunkSession{
public:
    explicit ChunkSession(Io&io):io_(io){}
    bool send(std::uint8_t csid,std::uint8_t type,std::uint32_t streamId,std::uint32_t ts,const std::vector<std::uint8_t>&body){
        std::size_t off=0;bool first=true;do{std::vector<std::uint8_t>h;const std::uint8_t fmt=first?0:3;h.push_back(static_cast<std::uint8_t>((fmt<<6)|(csid&0x3f)));if(first){const std::uint32_t t=std::min<std::uint32_t>(ts,0xffffffU);be24(h,t);be24(h,static_cast<std::uint32_t>(body.size()));h.push_back(type);h.push_back(static_cast<std::uint8_t>(streamId));h.push_back(static_cast<std::uint8_t>(streamId>>8));h.push_back(static_cast<std::uint8_t>(streamId>>16));h.push_back(static_cast<std::uint8_t>(streamId>>24));if(ts>=0xffffffU)be32(h,ts);}else if(ts>=0xffffffU)be32(h,ts);const std::size_t n=std::min<std::size_t>(outChunk_,body.size()-off);if(!io_.write(h.data(),h.size())||(n&&!io_.write(body.data()+off,n)))return false;off+=n;first=false;}while(off<body.size());return true;
    }
    bool setOutboundChunk(std::uint32_t n){std::vector<std::uint8_t>b;be32(b,n);if(!send(2,1,0,0,b))return false;outChunk_=n;return true;}
    bool receive(Message&m,int timeout,std::atomic<bool>*stop,std::string&error){
        for(;;){if(parseOne(m))return true;std::array<std::uint8_t,16384>b{};const int n=io_.read(b.data(),b.size(),timeout,stop);if(n==0){error="RTMP read timeout";return false;}if(n<0){error="RTMP connection closed";return false;}buffer_.insert(buffer_.end(),b.begin(),b.begin()+n);}
    }
private:
    struct Cs{std::uint32_t timestamp=0,delta=0,length=0,streamId=0;std::uint8_t type=0;std::vector<std::uint8_t>body;std::size_t received=0;bool have=false;bool extended=false;};
    bool parseOne(Message&out){std::size_t pos=0;while(pos<buffer_.size()){
        const std::size_t start=pos;if(buffer_.size()-pos<1)return false;std::uint8_t bh=buffer_[pos++];int fmt=bh>>6;std::uint32_t csid=bh&0x3f;if(csid==0){if(buffer_.size()-pos<1)return false;csid=64+buffer_[pos++];}else if(csid==1){if(buffer_.size()-pos<2)return false;csid=64+buffer_[pos]+256U*buffer_[pos+1];pos+=2;}Cs&c=cs_[csid];std::uint32_t timeField=0;
        if(fmt==0){if(buffer_.size()-pos<11){pos=start;return false;}timeField=read24(&buffer_[pos]);pos+=3;c.length=read24(&buffer_[pos]);pos+=3;c.type=buffer_[pos++];c.streamId=buffer_[pos]|(buffer_[pos+1]<<8)|(buffer_[pos+2]<<16)|(buffer_[pos+3]<<24);pos+=4;c.delta=0;c.body.clear();c.received=0;c.have=true;c.extended=timeField==0xffffffU;if(!c.extended)c.timestamp=timeField;}
        else if(fmt==1){if(!c.have||buffer_.size()-pos<7){pos=start;return false;}timeField=read24(&buffer_[pos]);pos+=3;c.length=read24(&buffer_[pos]);pos+=3;c.type=buffer_[pos++];c.body.clear();c.received=0;c.extended=timeField==0xffffffU;if(!c.extended){c.delta=timeField;c.timestamp+=c.delta;}}
        else if(fmt==2){if(!c.have||buffer_.size()-pos<3){pos=start;return false;}timeField=read24(&buffer_[pos]);pos+=3;c.body.clear();c.received=0;c.extended=timeField==0xffffffU;if(!c.extended){c.delta=timeField;c.timestamp+=c.delta;}}
        else{if(!c.have){buffer_.erase(buffer_.begin(),buffer_.begin()+static_cast<std::ptrdiff_t>(pos));continue;}if(c.received==c.length){c.body.clear();c.received=0;c.timestamp+=c.delta;}}
        if(c.extended){if(buffer_.size()-pos<4){pos=start;return false;}const std::uint32_t ext=read32(&buffer_[pos]);pos+=4;if(fmt==0)c.timestamp=ext;else if(fmt==1||fmt==2){c.delta=ext;c.timestamp+=(fmt==1||fmt==2)?ext:0;}}
        const std::size_t need=c.length-c.received;const std::size_t n=std::min<std::size_t>(inChunk_,need);if(buffer_.size()-pos<n){pos=start;return false;}c.body.insert(c.body.end(),buffer_.begin()+static_cast<std::ptrdiff_t>(pos),buffer_.begin()+static_cast<std::ptrdiff_t>(pos+n));pos+=n;c.received+=n;buffer_.erase(buffer_.begin(),buffer_.begin()+static_cast<std::ptrdiff_t>(pos));pos=0;
        if(c.received==c.length){out={c.timestamp,c.streamId,c.type,c.body};if(c.type==1&&c.body.size()>=4)inChunk_=read32(c.body.data());return true;}
    }return false;}
    Io&io_;std::uint32_t inChunk_=128,outChunk_=128;std::vector<std::uint8_t>buffer_;std::unordered_map<std::uint32_t,Cs>cs_;
};

bool handshake(Io&io,std::atomic<bool>*stop,std::string&error){std::array<std::uint8_t,1537>c{};c[0]=3;const auto now=static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());c[1]=now>>24;c[2]=now>>16;c[3]=now>>8;c[4]=now;std::mt19937 rng(static_cast<std::uint32_t>(now));for(std::size_t i=9;i<c.size();++i)c[i]=static_cast<std::uint8_t>(rng());if(!io.write(c.data(),c.size())){error="RTMP C0/C1 send failed";return false;}std::vector<std::uint8_t>s(3073);std::size_t off=0;while(off<s.size()){const int n=io.read(s.data()+off,s.size()-off,5000,stop);if(n<=0){error="RTMP S0/S1/S2 receive failed";return false;}off+=n;}if(s[0]!=3){error="unsupported RTMP version";return false;}if(!io.write(s.data()+1,1536)){error="RTMP C2 send failed";return false;}return true;}

std::vector<std::uint8_t> commandConnect(const Url&u){std::vector<std::uint8_t>b;amf::string(b,"connect");amf::number(b,1);amf::objectBegin(b);amf::key(b,"app");amf::string(b,u.app);amf::key(b,"type");amf::string(b,"nonprivate");amf::key(b,"tcUrl");amf::string(b,u.tcUrl);amf::key(b,"flashVer");amf::string(b,"FMLE/3.0 DVBStreamer5");amf::key(b,"fpad");amf::boolean(b,false);amf::key(b,"capabilities");amf::number(b,15);amf::key(b,"audioCodecs");amf::number(b,4071);amf::key(b,"videoCodecs");amf::number(b,252);amf::key(b,"videoFunction");amf::number(b,1);amf::objectEnd(b);return b;}
std::vector<std::uint8_t> commandSimple(const std::string&name,double tx,const std::string&arg={}){std::vector<std::uint8_t>b;amf::string(b,name);amf::number(b,tx);amf::nullv(b);if(!arg.empty())amf::string(b,arg);return b;}

bool waitResult(ChunkSession&chunks,double tx,std::uint32_t&streamId,std::atomic<bool>*stop,std::string&error){for(int i=0;i<40;++i){Message m;if(!chunks.receive(m,5000,stop,error))return false;if(m.type!=20&&m.type!=17)continue;std::vector<std::uint8_t>body=m.body;if(m.type==17&&!body.empty()&&body[0]==0)body.erase(body.begin());const auto v=amf::parseAll(body);if(v.size()<2||v[0].type!=amf::Value::String||v[1].type!=amf::Value::Number)continue;if(v[0].str=="_error"){error="RTMP server returned _error";return false;}if(v[0].str=="_result"&&std::fabs(v[1].number-tx)<0.01){if(v.size()>3&&v[3].type==amf::Value::Number)streamId=static_cast<std::uint32_t>(v[3].number);return true;}}error="RTMP command result not received";return false;}

bool establish(Io&io,ChunkSession&chunks,const Url&u,bool publish,std::atomic<bool>*stop,std::uint32_t&streamId,std::string&error){if(!handshake(io,stop,error))return false;if(!chunks.setOutboundChunk(4096))return false;if(!chunks.send(3,20,0,0,commandConnect(u)))return false;std::uint32_t dummy=0;if(!waitResult(chunks,1,dummy,stop,error))return false;if(publish){chunks.send(3,20,0,0,commandSimple("releaseStream",2,u.stream));chunks.send(3,20,0,0,commandSimple("FCPublish",3,u.stream));}const double tx=publish?4:2;if(!chunks.send(3,20,0,0,commandSimple("createStream",tx)))return false;if(!waitResult(chunks,tx,streamId,stop,error))return false;if(publish){if(!chunks.send(3,20,streamId,0,commandSimple("publish",0,u.stream)))return false;}else{if(!chunks.send(3,20,streamId,0,commandSimple("play",0,u.stream)))return false;}return true;}

std::vector<std::pair<const std::uint8_t*,std::size_t>> annexBNals(const std::vector<std::uint8_t>&data){std::vector<std::pair<const std::uint8_t*,std::size_t>>r;std::vector<std::size_t>starts;for(std::size_t i=0;i+3<data.size();){std::size_t n=0;if(data[i]==0&&data[i+1]==0&&data[i+2]==1)n=3;else if(i+4<=data.size()&&data[i]==0&&data[i+1]==0&&data[i+2]==0&&data[i+3]==1)n=4;if(n){starts.push_back(i+n);i+=n;}else++i;}for(std::size_t i=0;i<starts.size();++i){std::size_t e=i+1<starts.size()?starts[i+1]:data.size();while(e>starts[i]&&data[e-1]==0)--e;if(i+1<starts.size()){const auto s=starts[i+1];e=s;while(e>starts[i]&&data[e-1]==0)--e;}if(e>starts[i])r.push_back({data.data()+starts[i],e-starts[i]});}return r;}

struct AvcConfig{std::vector<std::uint8_t>sps,pps;bool sent=false;};
std::vector<std::uint8_t> avcConfigRecord(const AvcConfig&c){if(c.sps.size()<4||c.pps.empty())return{};std::vector<std::uint8_t>o{1,c.sps[1],c.sps[2],c.sps[3],0xff,0xe1};o.push_back(c.sps.size()>>8);o.push_back(c.sps.size());o.insert(o.end(),c.sps.begin(),c.sps.end());o.push_back(1);o.push_back(c.pps.size()>>8);o.push_back(c.pps.size());o.insert(o.end(),c.pps.begin(),c.pps.end());return o;}

struct HevcConfig{std::vector<std::uint8_t>vps,sps,pps;bool sent=false;};
void append16(std::vector<std::uint8_t>&o,std::size_t n){o.push_back(static_cast<std::uint8_t>((n>>8)&0xff));o.push_back(static_cast<std::uint8_t>(n&0xff));}
std::vector<std::uint8_t> hevcConfigRecord(const HevcConfig&c){
    if(c.vps.empty()||c.sps.empty()||c.pps.empty())return{};
    std::vector<std::uint8_t>o(23,0);
    o[0]=1; o[1]=1; o[12]=120; o[13]=0xf0; o[14]=0; o[15]=0xfc; o[16]=0xfc; o[17]=0xf8; o[18]=0xf8; o[21]=3; o[22]=3;
    auto add=[&](std::uint8_t type,const std::vector<std::uint8_t>&nal){o.push_back(static_cast<std::uint8_t>(0x80|type));append16(o,1);append16(o,nal.size());o.insert(o.end(),nal.begin(),nal.end());};
    add(32,c.vps); add(33,c.sps); add(34,c.pps); return o;
}
bool parseHevcConfigRecord(const std::vector<std::uint8_t>&cfg,std::vector<std::uint8_t>&parameterSets){
    parameterSets.clear(); if(cfg.size()<23)return false; std::size_t q=23; const std::size_t arrays=cfg[22];
    for(std::size_t a=0;a<arrays;++a){if(q+3>cfg.size())return false;const std::uint8_t type=cfg[q++]&0x3f;const std::size_t count=(static_cast<std::size_t>(cfg[q])<<8)|cfg[q+1];q+=2;for(std::size_t i=0;i<count;++i){if(q+2>cfg.size())return false;const std::size_t n=(static_cast<std::size_t>(cfg[q])<<8)|cfg[q+1];q+=2;if(q+n>cfg.size())return false;if(type==32||type==33||type==34){parameterSets.insert(parameterSets.end(),{0,0,0,1});parameterSets.insert(parameterSets.end(),cfg.begin()+static_cast<std::ptrdiff_t>(q),cfg.begin()+static_cast<std::ptrdiff_t>(q+n));}q+=n;}}
    return !parameterSets.empty();
}

bool parseAdts(const std::vector<std::uint8_t>&in,std::size_t&off,std::vector<std::uint8_t>&raw,int&rate,int&channels,std::vector<std::uint8_t>&asc){if(off+7>in.size()||in[off]!=0xff||(in[off+1]&0xf6)!=0xf0)return false;static const int rates[]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};const int idx=(in[off+2]>>2)&15;if(idx>=13)return false;const int profile=((in[off+2]>>6)&3)+1;channels=((in[off+2]&1)<<2)|(in[off+3]>>6);rate=rates[idx];const std::size_t len=((in[off+3]&3)<<11)|(in[off+4]<<3)|(in[off+5]>>5);const std::size_t hdr=(in[off+1]&1)?7:9;if(len<hdr||off+len>in.size())return false;raw.assign(in.begin()+static_cast<std::ptrdiff_t>(off+hdr),in.begin()+static_cast<std::ptrdiff_t>(off+len));asc={static_cast<std::uint8_t>((profile<<3)|(idx>>1)),static_cast<std::uint8_t>(((idx&1)<<7)|(channels<<3))};off+=len;return true;}
bool adtsHeader(int rate,int ch,std::size_t raw,std::array<std::uint8_t,7>&h){static const int rs[]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};int idx=-1;for(int i=0;i<13;++i)if(rs[i]==rate){idx=i;break;}if(idx<0||ch<1||ch>7||raw+7>0x1fff)return false;const auto n=raw+7;h={0xff,0xf1,static_cast<std::uint8_t>((1<<6)|(idx<<2)|((ch>>2)&1)),static_cast<std::uint8_t>(((ch&3)<<6)|((n>>11)&3)),static_cast<std::uint8_t>(n>>3),static_cast<std::uint8_t>(((n&7)<<5)|0x1f),0xfc};return true;}

} // namespace

struct NativeRtmpOutput::Impl{
    Io io;std::unique_ptr<ChunkSession>chunks;std::uint32_t streamId=0;mpegts::NativeTsDemux demux;std::mutex sendMutex;AvcConfig avc;HevcConfig hevc;bool aacConfigSent=false;std::vector<std::uint8_t>asc;std::atomic<bool>stopping{false};
};

NativeRtmpOutput::NativeRtmpOutput():impl_(std::make_unique<Impl>()){}
NativeRtmpOutput::~NativeRtmpOutput(){stop();}
std::string NativeRtmpOutput::lastError()const{std::lock_guard<std::mutex>l(errorMutex_);return lastError_;}void NativeRtmpOutput::setError(const std::string&v){std::lock_guard<std::mutex>l(errorMutex_);lastError_=v;}

bool NativeRtmpOutput::start(const EndpointConfig&config,StatusCallback status,std::string&error){stop();Url u;if(!parseUrl(config.uri,u)){error="invalid RTMP URL";return false;}config_=config;status_=std::move(status);impl_=std::make_unique<Impl>();if(!impl_->io.connect(u,config.connectTimeoutMs,&impl_->stopping,error))return false;impl_->chunks=std::make_unique<ChunkSession>(impl_->io);if(!establish(impl_->io,*impl_->chunks,u,true,&impl_->stopping,impl_->streamId,error)){impl_->io.close();return false;}impl_->demux.setSampleCallback([this](mpegts::DemuxSample&&s){if(!running_.load()||!impl_||!impl_->chunks)return;std::lock_guard<std::mutex>lock(impl_->sendMutex);const std::uint32_t dtsMs=static_cast<std::uint32_t>((s.hasDts?s.dts90k:s.pts90k)/90);const std::uint32_t ptsMs=static_cast<std::uint32_t>((s.hasPts?s.pts90k:s.dts90k)/90);if(s.stream.codec==mpegts::ElementaryCodec::H264){auto nals=annexBNals(s.data);for(const auto&[p,n]:nals){const auto t=p[0]&0x1fU;if(t==7)impl_->avc.sps.assign(p,p+n);else if(t==8)impl_->avc.pps.assign(p,p+n);}if(!impl_->avc.sent){auto cfg=avcConfigRecord(impl_->avc);if(!cfg.empty()){std::vector<std::uint8_t>b{0x17,0,0,0,0};b.insert(b.end(),cfg.begin(),cfg.end());impl_->chunks->send(6,9,impl_->streamId,dtsMs,b);impl_->avc.sent=true;}}std::vector<std::uint8_t>b{static_cast<std::uint8_t>(s.randomAccess?0x17:0x27),1};std::int32_t comp=static_cast<std::int32_t>(ptsMs-dtsMs);b.push_back(static_cast<std::uint8_t>(comp>>16));b.push_back(static_cast<std::uint8_t>(comp>>8));b.push_back(static_cast<std::uint8_t>(comp));for(const auto&[p,n]:nals){if((p[0]&0x1fU)==7||(p[0]&0x1fU)==8||(p[0]&0x1fU)==9)continue;be32(b,static_cast<std::uint32_t>(n));b.insert(b.end(),p,p+n);}if(b.size()>5){impl_->chunks->send(6,9,impl_->streamId,dtsMs,b);sentBytes_.fetch_add(b.size());}}
        else if(s.stream.codec==mpegts::ElementaryCodec::H265){auto nals=annexBNals(s.data);for(const auto&[p,n]:nals){if(n<2)continue;const auto t=(p[0]>>1)&0x3fU;if(t==32)impl_->hevc.vps.assign(p,p+n);else if(t==33)impl_->hevc.sps.assign(p,p+n);else if(t==34)impl_->hevc.pps.assign(p,p+n);}if(!impl_->hevc.sent){auto cfg=hevcConfigRecord(impl_->hevc);if(!cfg.empty()){std::vector<std::uint8_t>b{0x90,'h','v','c','1'};b.insert(b.end(),cfg.begin(),cfg.end());if(!impl_->chunks->send(6,9,impl_->streamId,dtsMs,b)){setError("RTMP HEVC sequence header send failed");return;}impl_->hevc.sent=true;}}std::vector<std::uint8_t>b{static_cast<std::uint8_t>((s.randomAccess?0x90:0xa0)|0x01),'h','v','c','1'};std::int32_t comp=static_cast<std::int32_t>(ptsMs-dtsMs);b.push_back(static_cast<std::uint8_t>(comp>>16));b.push_back(static_cast<std::uint8_t>(comp>>8));b.push_back(static_cast<std::uint8_t>(comp));for(const auto&[p,n]:nals){if(n<2)continue;const auto t=(p[0]>>1)&0x3fU;if(t==32||t==33||t==34||t==35)continue;be32(b,static_cast<std::uint32_t>(n));b.insert(b.end(),p,p+n);}if(b.size()>8){if(!impl_->chunks->send(6,9,impl_->streamId,dtsMs,b)){setError("RTMP HEVC frame send failed");return;}sentBytes_.fetch_add(b.size());}}
        else if(s.stream.codec==mpegts::ElementaryCodec::AacAdts){std::size_t off=0;while(off<s.data.size()){std::vector<std::uint8_t>raw,asc;int rate=0,ch=0;if(!parseAdts(s.data,off,raw,rate,ch,asc))break;if(!impl_->aacConfigSent){std::vector<std::uint8_t>cfg{0xaf,0};cfg.insert(cfg.end(),asc.begin(),asc.end());impl_->chunks->send(4,8,impl_->streamId,dtsMs,cfg);impl_->aacConfigSent=true;}std::vector<std::uint8_t>b{0xaf,1};b.insert(b.end(),raw.begin(),raw.end());impl_->chunks->send(4,8,impl_->streamId,dtsMs,b);sentBytes_.fetch_add(b.size());}}
    });running_.store(true);if(status_)status_("publishing");error.clear();return true;}

void NativeRtmpOutput::stop()noexcept{running_.store(false);if(impl_){impl_->stopping.store(true);impl_->demux.flush();impl_->io.close();impl_.reset();}}
bool NativeRtmpOutput::push(const std::uint8_t*data,std::size_t size){if(!running_.load()||!impl_)return false;std::string e;if(!impl_->demux.push(data,size,e)){setError(e);return false;}return true;}

NativeRtmpInput::~NativeRtmpInput(){stop();}
bool NativeRtmpInput::start(const EndpointConfig&config,DataCallback data,StatusCallback status,std::string&error){stop();Url u;if(!parseUrl(config.uri,u)){error="invalid RTMP URL";return false;}config_=config;data_=std::move(data);status_=std::move(status);stopping_.store(false);running_.store(true);receivedBytes_.store(0);setError({});try{worker_=std::thread(&NativeRtmpInput::run,this);}catch(const std::exception&ex){running_.store(false);error=ex.what();return false;}error.clear();return true;}
void NativeRtmpInput::stop()noexcept{stopping_.store(true);if(worker_.joinable()&&worker_.get_id()!=std::this_thread::get_id())worker_.join();running_.store(false);}std::string NativeRtmpInput::lastError()const{std::lock_guard<std::mutex>l(errorMutex_);return lastError_;}void NativeRtmpInput::setError(const std::string&v){std::lock_guard<std::mutex>l(errorMutex_);lastError_=v;}

void NativeRtmpInput::run(){Url u;std::string error;if(!parseUrl(config_.uri,u)){setError("invalid RTMP URL");running_.store(false);return;}Io io;if(!io.connect(u,config_.connectTimeoutMs,&stopping_,error)){setError(error);running_.store(false);return;}ChunkSession chunks(io);std::uint32_t streamId=0;if(!establish(io,chunks,u,false,&stopping_,streamId,error)){setError(error);io.close();running_.store(false);return;}if(status_)status_("playing");mpegts::NativeMpegTsMux mux;mpegts::NativeMuxConfig mc;mc.serviceName="RTMP";mc.serviceProvider="DVBStreamer5";mux.initialize(mc,error);bool videoSet=false,audioSet=false;AvcConfig avc;std::vector<std::uint8_t>hevcParam;int aacRate=48000,aacChannels=2;auto emit=[&](mpegts::ElementaryKind k,mpegts::ElementaryCodec codec,const std::vector<std::uint8_t>&sample,std::uint64_t pts,std::uint64_t dts,bool key){if(k==mpegts::ElementaryKind::Video&&!videoSet){mux.setCodec(k,codec,error);videoSet=true;}if(k==mpegts::ElementaryKind::Audio&&!audioSet){mux.setCodec(k,codec,error);audioSet=true;}mpegts::ElementarySample es;es.data=sample.data();es.size=sample.size();es.hasPts=es.hasDts=true;es.pts90k=pts;es.dts90k=dts;es.randomAccess=key;std::vector<mpegts::Packet>out;if(!mux.write(k,es,out,error))return;std::vector<std::uint8_t>raw;raw.reserve(out.size()*188);for(auto&p:out)raw.insert(raw.end(),p.begin(),p.end());if(!raw.empty()&&data_&&data_(raw.data(),raw.size()))receivedBytes_.fetch_add(raw.size());};
    while(!stopping_.load()){Message m;if(!chunks.receive(m,config_.ioTimeoutMs,&stopping_,error))break;if(m.type==8&&m.body.size()>=2){const int fmt=m.body[0]>>4;if(fmt!=10)continue;const int pt=m.body[1];if(pt==0&&m.body.size()>=4){const auto*p=m.body.data()+2;const std::uint8_t aot=p[0]>>3;const int idx=((p[0]&7)<<1)|(p[1]>>7);static const int rates[]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};if(idx<13)aacRate=rates[idx];aacChannels=(p[1]>>3)&15;(void)aot;}else if(pt==1){std::array<std::uint8_t,7>h{};if(!adtsHeader(aacRate,aacChannels,m.body.size()-2,h))continue;std::vector<std::uint8_t>s(h.begin(),h.end());s.insert(s.end(),m.body.begin()+2,m.body.end());emit(mpegts::ElementaryKind::Audio,mpegts::ElementaryCodec::AacAdts,s,static_cast<std::uint64_t>(m.timestamp)*90,static_cast<std::uint64_t>(m.timestamp)*90,false);}}
        else if(m.type==9&&m.body.size()>=5){const bool enhanced=(m.body[0]&0x80)!=0;if(!enhanced){const int codec=m.body[0]&15;if(codec!=7)continue;const int pt=m.body[1];std::int32_t comp=(m.body[2]<<16)|(m.body[3]<<8)|m.body[4];if(comp&0x800000)comp|=~0xffffff;if(pt==0&&m.body.size()>10){const auto*p=m.body.data()+5;std::size_t n=m.body.size()-5;if(n<7)continue;std::size_t q=6;const int ns=p[5]&31;for(int i=0;i<ns&&q+2<=n;++i){const std::size_t z=(p[q]<<8)|p[q+1];q+=2;if(q+z>n)break;avc.sps.assign(p+q,p+q+z);q+=z;}if(q<n){const int np=p[q++];for(int i=0;i<np&&q+2<=n;++i){const std::size_t z=(p[q]<<8)|p[q+1];q+=2;if(q+z>n)break;avc.pps.assign(p+q,p+q+z);q+=z;}}}else if(pt==1){std::vector<std::uint8_t>s;if((m.body[0]>>4)==1){if(!avc.sps.empty()){s.insert(s.end(),{0,0,0,1});s.insert(s.end(),avc.sps.begin(),avc.sps.end());}if(!avc.pps.empty()){s.insert(s.end(),{0,0,0,1});s.insert(s.end(),avc.pps.begin(),avc.pps.end());}}std::size_t q=5;while(q+4<=m.body.size()){const std::size_t z=read32(&m.body[q]);q+=4;if(q+z>m.body.size())break;s.insert(s.end(),{0,0,0,1});s.insert(s.end(),m.body.begin()+static_cast<std::ptrdiff_t>(q),m.body.begin()+static_cast<std::ptrdiff_t>(q+z));q+=z;}const std::uint64_t d=static_cast<std::uint64_t>(m.timestamp)*90;const std::int64_t pp=static_cast<std::int64_t>(d)+static_cast<std::int64_t>(comp)*90;emit(mpegts::ElementaryKind::Video,mpegts::ElementaryCodec::H264,s,pp<0?0:static_cast<std::uint64_t>(pp),d,(m.body[0]>>4)==1);}}
            else if(m.body.size()>=6){const int packet=m.body[0]&15;const std::string fourcc(reinterpret_cast<const char*>(m.body.data()+1),4);if(fourcc!="hvc1"&&fourcc!="hev1")continue;if(packet==0){std::vector<std::uint8_t>cfg(m.body.begin()+5,m.body.end());if(!parseHevcConfigRecord(cfg,hevcParam))hevcParam.clear();}else if(packet==1){std::size_t q=5;std::int32_t comp=0;if(m.body.size()>=8){comp=(m.body[5]<<16)|(m.body[6]<<8)|m.body[7];if(comp&0x800000)comp|=~0xffffff;q=8;}const bool key=((m.body[0]>>4)&7)==1;std::vector<std::uint8_t>s;if(key&&!hevcParam.empty())s.insert(s.end(),hevcParam.begin(),hevcParam.end());while(q+4<=m.body.size()){const std::size_t z=read32(&m.body[q]);q+=4;if(q+z>m.body.size())break;s.insert(s.end(),{0,0,0,1});s.insert(s.end(),m.body.begin()+static_cast<std::ptrdiff_t>(q),m.body.begin()+static_cast<std::ptrdiff_t>(q+z));q+=z;}const std::uint64_t d=static_cast<std::uint64_t>(m.timestamp)*90;const std::int64_t pp=static_cast<std::int64_t>(d)+static_cast<std::int64_t>(comp)*90;emit(mpegts::ElementaryKind::Video,mpegts::ElementaryCodec::H265,s,pp<0?0:static_cast<std::uint64_t>(pp),d,key);}}}
    }
    if(!stopping_.load()){setError(error);if(status_)status_(error);}io.close();running_.store(false);
}

} // namespace dvbstreamer5::media::rtmp
