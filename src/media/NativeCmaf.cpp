#include "media/NativeCmaf.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <system_error>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <memory>

namespace dvbstreamer5::media::cmaf {
namespace {

std::uint16_t u16(const std::uint8_t*p){return static_cast<std::uint16_t>((p[0]<<8)|p[1]);}
std::uint32_t u32(const std::uint8_t*p){return(static_cast<std::uint32_t>(p[0])<<24)|(static_cast<std::uint32_t>(p[1])<<16)|(static_cast<std::uint32_t>(p[2])<<8)|p[3];}
std::uint64_t u64(const std::uint8_t*p){return(static_cast<std::uint64_t>(u32(p))<<32)|u32(p+4);}
void w16(std::vector<std::uint8_t>&v,std::uint16_t x){v.push_back(x>>8);v.push_back(x);}
void w24(std::vector<std::uint8_t>&v,std::uint32_t x){v.push_back(x>>16);v.push_back(x>>8);v.push_back(x);}
void w32(std::vector<std::uint8_t>&v,std::uint32_t x){v.push_back(x>>24);v.push_back(x>>16);v.push_back(x>>8);v.push_back(x);}
void w64(std::vector<std::uint8_t>&v,std::uint64_t x){w32(v,static_cast<std::uint32_t>(x>>32));w32(v,static_cast<std::uint32_t>(x));}
void patch32(std::vector<std::uint8_t>&v,std::size_t p,std::uint32_t x){v[p]=x>>24;v[p+1]=x>>16;v[p+2]=x>>8;v[p+3]=x;}
std::uint32_t fourcc(const char*s){return(static_cast<std::uint32_t>(s[0])<<24)|(static_cast<std::uint32_t>(s[1])<<16)|(static_cast<std::uint32_t>(s[2])<<8)|s[3];}

struct Box{std::uint32_t type=0;std::size_t start=0,header=0,size=0;std::size_t payload()const{return start+header;}std::size_t end()const{return start+size;}};
bool boxAt(const std::vector<std::uint8_t>&b,std::size_t pos,std::size_t limit,Box&o){if(pos+8>limit||limit>b.size())return false;std::uint64_t size=u32(&b[pos]);std::size_t h=8;if(size==1){if(pos+16>limit)return false;size=u64(&b[pos+8]);h=16;}else if(size==0)size=limit-pos;if(size<h||size>limit-pos)return false;o={u32(&b[pos+4]),pos,h,static_cast<std::size_t>(size)};return true;}
std::vector<Box> children(const std::vector<std::uint8_t>&b,const Box&parent,std::size_t skip=0){std::vector<Box>r;std::size_t p=parent.payload()+skip;while(p+8<=parent.end()){Box x;if(!boxAt(b,p,parent.end(),x))break;r.push_back(x);p=x.end();}return r;}
const Box* find(const std::vector<Box>&v,std::uint32_t t){for(const auto&b:v)if(b.type==t)return &b;return nullptr;}

struct CtxFree { void operator()(EVP_CIPHER_CTX* p) const noexcept { EVP_CIPHER_CTX_free(p); } };
using CipherCtx = std::unique_ptr<EVP_CIPHER_CTX,CtxFree>;
struct SubsamplePair { std::uint16_t clear = 0; std::uint32_t encrypted = 0; };
struct SampleAux { std::array<std::uint8_t,16> iv{}; std::vector<SubsamplePair> subs; };

bool aesBlock(const std::array<std::uint8_t,16>&key,bool encrypt,const std::uint8_t in[16],std::uint8_t out[16]){
    CipherCtx ctx(EVP_CIPHER_CTX_new());if(!ctx)return false;int n=0,tail=0;
    if(encrypt){if(EVP_EncryptInit_ex(ctx.get(),EVP_aes_128_ecb(),nullptr,key.data(),nullptr)!=1)return false;EVP_CIPHER_CTX_set_padding(ctx.get(),0);return EVP_EncryptUpdate(ctx.get(),out,&n,in,16)==1&&n==16&&EVP_EncryptFinal_ex(ctx.get(),out+n,&tail)==1;}
    if(EVP_DecryptInit_ex(ctx.get(),EVP_aes_128_ecb(),nullptr,key.data(),nullptr)!=1)return false;EVP_CIPHER_CTX_set_padding(ctx.get(),0);return EVP_DecryptUpdate(ctx.get(),out,&n,in,16)==1&&n==16&&EVP_DecryptFinal_ex(ctx.get(),out+n,&tail)==1;
}

bool cbcsPatternState(std::uint8_t* data, std::size_t size, const std::array<std::uint8_t,16>& key,
                      std::array<std::uint8_t,16>& chain, bool encrypt,
                      unsigned cryptBlocks, unsigned skipBlocks) {
    if (!data || size < 16) return true;
    unsigned phase = 0;
    const unsigned cycle = std::max(1U, cryptBlocks + skipBlocks);
    for (std::size_t off = 0; off + 16 <= size; off += 16, phase = (phase + 1) % cycle) {
        if (phase >= cryptBlocks) continue;
        std::uint8_t in[16], tmp[16], out[16];
        std::memcpy(in, data + off, 16);
        if (encrypt) {
            for (int i = 0; i < 16; ++i) tmp[i] = static_cast<std::uint8_t>(in[i] ^ chain[static_cast<std::size_t>(i)]);
            if (!aesBlock(key, true, tmp, out)) return false;
            std::memcpy(data + off, out, 16);
            std::copy(out, out + 16, chain.begin());
        } else {
            if (!aesBlock(key, false, in, tmp)) return false;
            for (int i = 0; i < 16; ++i) out[i] = static_cast<std::uint8_t>(tmp[i] ^ chain[static_cast<std::size_t>(i)]);
            std::copy(in, in + 16, chain.begin());
            std::memcpy(data + off, out, 16);
        }
    }
    return true;
}

void makeIv(std::uint64_t sequence,std::uint32_t track,std::uint32_t index,std::array<std::uint8_t,16>&iv){
    iv.fill(0);for(int i=0;i<8;++i)iv[7-i]=static_cast<std::uint8_t>(sequence>>(i*8));iv[8]=track>>24;iv[9]=track>>16;iv[10]=track>>8;iv[11]=track;iv[12]=index>>24;iv[13]=index>>16;iv[14]=index>>8;iv[15]=index;
}

std::vector<SubsamplePair> videoSubsamples(const std::vector<std::uint8_t>&payload){
    std::vector<SubsamplePair> out;std::size_t q=0;while(q+4<=payload.size()){const std::uint32_t n=u32(payload.data()+q);if(!n||q+4+n>payload.size())break;const std::size_t head=std::min<std::size_t>(32,n);const std::size_t enc=((n-head)/16)*16;const std::size_t tail=n-head-enc;std::size_t clear=4+head;while(clear>0xffff){out.push_back({0xffff,0});clear-=0xffff;}out.push_back({static_cast<std::uint16_t>(clear),static_cast<std::uint32_t>(enc)});if(tail)out.push_back({static_cast<std::uint16_t>(tail),0});q+=4+n;}if(q<payload.size()){const std::size_t rest=payload.size()-q;out.push_back({static_cast<std::uint16_t>(std::min<std::size_t>(rest,0xffff)),0});}return out;
}
std::vector<SubsamplePair> audioSubsamples(std::size_t size){const std::size_t clear=std::min<std::size_t>(16,size);const std::size_t enc=((size-clear)/16)*16;const std::size_t tail=size-clear-enc;std::vector<SubsamplePair>r{{static_cast<std::uint16_t>(clear),static_cast<std::uint32_t>(enc)}};if(tail)r.push_back({static_cast<std::uint16_t>(tail),0});return r;}

bool transformBySubsamples(std::vector<std::uint8_t>& payload, const SampleAux& aux,
                           const std::array<std::uint8_t,16>& key, bool encrypt, bool video) {
    std::size_t off = 0;
    std::array<std::uint8_t,16> chain = aux.iv;
    for (const auto& s : aux.subs) {
        if (off + s.clear + s.encrypted > payload.size()) return false;
        off += s.clear;
        if (s.encrypted) {
            if (!cbcsPatternState(payload.data() + off, s.encrypted, key, chain, encrypt, 1U, video ? 9U : 0U)) return false;
            off += s.encrypted;
        }
    }
    return off <= payload.size();
}

std::size_t beginBox(std::vector<std::uint8_t>&o,const char*t);
void endBox(std::vector<std::uint8_t>&o,std::size_t p);
void full(std::vector<std::uint8_t>&o,std::uint8_t version,std::uint32_t flags);

void writeSinf(std::vector<std::uint8_t>&o,const char*original,bool video,const std::array<std::uint8_t,16>&kid){auto s=beginBox(o,"sinf");auto f=beginBox(o,"frma");o.insert(o.end(),original,original+4);endBox(o,f);auto m=beginBox(o,"schm");full(o,0,0);o.insert(o.end(),{'c','b','c','s'});w32(o,0x00010000);endBox(o,m);auto sh=beginBox(o,"schi");auto t=beginBox(o,"tenc");full(o,1,0);o.push_back(0);o.push_back(video?0x19:0x10);o.push_back(1);o.push_back(16);o.insert(o.end(),kid.begin(),kid.end());endBox(o,t);endBox(o,sh);endBox(o,s);}

std::size_t beginBox(std::vector<std::uint8_t>&o,const char*t){const auto p=o.size();w32(o,0);o.insert(o.end(),t,t+4);return p;}
void endBox(std::vector<std::uint8_t>&o,std::size_t p){patch32(o,p,static_cast<std::uint32_t>(o.size()-p));}
void full(std::vector<std::uint8_t>&o,std::uint8_t version,std::uint32_t flags){o.push_back(version);w24(o,flags);}

std::vector<std::pair<const std::uint8_t*,std::size_t>> nals(const std::vector<std::uint8_t>&d){std::vector<std::pair<const std::uint8_t*,std::size_t>>r;std::vector<std::size_t>s;for(std::size_t i=0;i+3<d.size();){std::size_t n=0;if(d[i]==0&&d[i+1]==0&&d[i+2]==1)n=3;else if(i+4<=d.size()&&d[i]==0&&d[i+1]==0&&d[i+2]==0&&d[i+3]==1)n=4;if(n){s.push_back(i+n);i+=n;}else++i;}for(std::size_t i=0;i<s.size();++i){std::size_t e=i+1<s.size()?s[i+1]:d.size();if(i+1<s.size()){while(e>s[i]&&d[e-1]==0)--e;}if(e>s[i])r.push_back({d.data()+s[i],e-s[i]});}return r;}
std::vector<std::uint8_t> lengthPrefixed(const std::vector<std::uint8_t>&d){std::vector<std::uint8_t>o;for(auto[p,n]:nals(d)){w32(o,static_cast<std::uint32_t>(n));o.insert(o.end(),p,p+n);}return o;}
std::vector<std::uint8_t> annexB(const std::uint8_t*p,std::size_t n,std::uint8_t lenSize){std::vector<std::uint8_t>o;std::size_t q=0;while(q+lenSize<=n){std::uint32_t z=0;for(std::uint8_t i=0;i<lenSize;++i)z=(z<<8)|p[q++];if(z==0||q+z>n)break;o.insert(o.end(),{0,0,0,1});o.insert(o.end(),p+q,p+q+z);q+=z;}return o;}

bool adtsInfo(const std::vector<std::uint8_t>&d,std::size_t&hdr,std::size_t&frame,int&rate,int&ch,std::vector<std::uint8_t>&asc){if(d.size()<7||d[0]!=0xff||(d[1]&0xf6)!=0xf0)return false;static const int rs[]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};const int idx=(d[2]>>2)&15;if(idx>=13)return false;const int profile=((d[2]>>6)&3)+1;ch=((d[2]&1)<<2)|(d[3]>>6);rate=rs[idx];frame=((d[3]&3)<<11)|(d[4]<<3)|(d[5]>>5);hdr=(d[1]&1)?7:9;if(frame<hdr||frame>d.size())return false;asc={static_cast<std::uint8_t>((profile<<3)|(idx>>1)),static_cast<std::uint8_t>(((idx&1)<<7)|(ch<<3))};return true;}
bool makeAdts(const TrackInfo&t,std::size_t raw,std::array<std::uint8_t,7>&h){static const int rs[]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};int idx=-1;for(int i=0;i<13;++i)if(rs[i]==static_cast<int>(t.sampleRate)){idx=i;break;}if(idx<0)return false;const std::size_t n=raw+7;const int ch=t.channels;h={0xff,0xf1,static_cast<std::uint8_t>((1<<6)|(idx<<2)|((ch>>2)&1)),static_cast<std::uint8_t>(((ch&3)<<6)|((n>>11)&3)),static_cast<std::uint8_t>(n>>3),static_cast<std::uint8_t>(((n&7)<<5)|0x1f),0xfc};return true;}

std::vector<std::uint8_t> avcC(const std::vector<std::uint8_t>&ps){std::vector<std::uint8_t>sps,pps;for(auto[p,n]:nals(ps)){const auto t=p[0]&31;if(t==7)sps.assign(p,p+n);else if(t==8)pps.assign(p,p+n);}if(sps.size()<4||pps.empty())return{};std::vector<std::uint8_t>o{1,sps[1],sps[2],sps[3],0xff,0xe1};w16(o,sps.size());o.insert(o.end(),sps.begin(),sps.end());o.push_back(1);w16(o,pps.size());o.insert(o.end(),pps.begin(),pps.end());return o;}
std::vector<std::uint8_t> hvcC(const std::vector<std::uint8_t>&ps){std::vector<std::pair<std::uint8_t,std::vector<std::uint8_t>>> arrays;for(auto[p,n]:nals(ps)){const std::uint8_t t=(p[0]>>1)&0x3f;if(t==32||t==33||t==34)arrays.push_back({t,std::vector<std::uint8_t>(p,p+n)});}if(arrays.empty())return{};std::vector<std::uint8_t>o(23,0);o[0]=1;o[1]=1;o[12]=120;o[13]=0xf0;o[14]=0;o[15]=0xfc;o[16]=0xfc;o[17]=0xf8;o[18]=0xf8;o[21]=3;o[22]=static_cast<std::uint8_t>(arrays.size());for(auto&[t,d]:arrays){o.push_back(static_cast<std::uint8_t>(0x80|t));w16(o,1);w16(o,d.size());o.insert(o.end(),d.begin(),d.end());}return o;}

void writeMvhd(std::vector<std::uint8_t>&o){auto b=beginBox(o,"mvhd");full(o,0,0);w32(o,0);w32(o,0);w32(o,1000);w32(o,0);w32(o,0x00010000);w16(o,0x0100);w16(o,0);o.insert(o.end(),10,0);const std::uint32_t m[9]={0x00010000,0,0,0,0x00010000,0,0,0,0x40000000};for(auto x:m)w32(o,x);o.insert(o.end(),24,0);w32(o,3);endBox(o,b);}
void writeTkhd(std::vector<std::uint8_t>&o,const TrackInfo&t){auto b=beginBox(o,"tkhd");full(o,0,7);w32(o,0);w32(o,0);w32(o,t.id);w32(o,0);w32(o,0);o.insert(o.end(),8,0);w16(o,0);w16(o,0);w16(o,t.kind==mpegts::ElementaryKind::Audio?0x0100:0);w16(o,0);const std::uint32_t m[9]={0x00010000,0,0,0,0x00010000,0,0,0,0x40000000};for(auto x:m)w32(o,x);w32(o,t.width<<16);w32(o,t.height<<16);endBox(o,b);}
void writeMdhd(std::vector<std::uint8_t>&o,const TrackInfo&t){auto b=beginBox(o,"mdhd");full(o,0,0);w32(o,0);w32(o,0);w32(o,t.timescale);w32(o,0);w16(o,0x55c4);w16(o,0);endBox(o,b);}
void writeHdlr(std::vector<std::uint8_t>&o,const TrackInfo&t){auto b=beginBox(o,"hdlr");full(o,0,0);w32(o,0);o.insert(o.end(),t.kind==mpegts::ElementaryKind::Video?std::initializer_list<std::uint8_t>{'v','i','d','e'}:std::initializer_list<std::uint8_t>{'s','o','u','n'});o.insert(o.end(),12,0);const char*name=t.kind==mpegts::ElementaryKind::Video?"VideoHandler":"SoundHandler";o.insert(o.end(),name,name+std::strlen(name)+1);endBox(o,b);}
void writeDinf(std::vector<std::uint8_t>&o){auto d=beginBox(o,"dinf");auto r=beginBox(o,"dref");full(o,0,0);w32(o,1);auto u=beginBox(o,"url ");full(o,0,1);endBox(o,u);endBox(o,r);endBox(o,d);}
void writeStbl(std::vector<std::uint8_t>&o,const TrackInfo&t,bool encrypted,const std::array<std::uint8_t,16>&kid){
    auto stbl=beginBox(o,"stbl");auto stsd=beginBox(o,"stsd");full(o,0,0);w32(o,1);
    if(t.kind==mpegts::ElementaryKind::Video){
        const char*orig=t.codec==mpegts::ElementaryCodec::H265?"hvc1":"avc1";const char*fmt=encrypted?"encv":orig;auto e=beginBox(o,fmt);
        o.insert(o.end(),6,0);w16(o,1);o.insert(o.end(),16,0);w16(o,t.width);w16(o,t.height);w32(o,0x00480000);w32(o,0x00480000);w32(o,0);w16(o,1);o.push_back(0);o.insert(o.end(),31,0);w16(o,0x0018);w16(o,0xffff);
        auto c=beginBox(o,t.codec==mpegts::ElementaryCodec::H265?"hvcC":"avcC");o.insert(o.end(),t.codecConfig.begin(),t.codecConfig.end());endBox(o,c);if(encrypted)writeSinf(o,orig,true,kid);endBox(o,e);
    }else{
        const char*fmt=encrypted?"enca":"mp4a";auto e=beginBox(o,fmt);o.insert(o.end(),6,0);w16(o,1);o.insert(o.end(),8,0);w16(o,t.channels);w16(o,16);w16(o,0);w16(o,0);w32(o,t.sampleRate<<16);
        auto es=beginBox(o,"esds");full(o,0,0);std::vector<std::uint8_t>d{0x03,0x19,0x00,0x01,0x00,0x04,0x11,0x40,0x15,0,0,0,0,0,0,0,0,0,0,0x05,static_cast<std::uint8_t>(t.codecConfig.size())};d.insert(d.end(),t.codecConfig.begin(),t.codecConfig.end());d.insert(d.end(),{0x06,0x01,0x02});o.insert(o.end(),d.begin(),d.end());endBox(o,es);if(encrypted)writeSinf(o,"mp4a",false,kid);endBox(o,e);
    }
    endBox(o,stsd);for(const char*name:{"stts","stsc","stsz","stco"}){auto b=beginBox(o,name);full(o,0,0);w32(o,0);endBox(o,b);}endBox(o,stbl);
}
void writeTrak(std::vector<std::uint8_t>&o,const TrackInfo&t,bool encrypted,const std::array<std::uint8_t,16>&kid){auto tr=beginBox(o,"trak");writeTkhd(o,t);auto md=beginBox(o,"mdia");writeMdhd(o,t);writeHdlr(o,t);auto mi=beginBox(o,"minf");if(t.kind==mpegts::ElementaryKind::Video){auto v=beginBox(o,"vmhd");full(o,0,1);o.insert(o.end(),8,0);endBox(o,v);}else{auto s=beginBox(o,"smhd");full(o,0,0);o.insert(o.end(),4,0);endBox(o,s);}writeDinf(o);writeStbl(o,t,encrypted,kid);endBox(o,mi);endBox(o,md);endBox(o,tr);}
std::vector<std::uint8_t> makeInit(const std::vector<TrackInfo>&tracks,bool encrypted,const std::array<std::uint8_t,16>&kid){std::vector<std::uint8_t>o;auto f=beginBox(o,"ftyp");o.insert(o.end(),{'i','s','o','6'});w32(o,1);o.insert(o.end(),{'i','s','o','6','i','s','o','m','m','p','4','1','d','a','s','h','c','m','f','c'});endBox(o,f);auto m=beginBox(o,"moov");writeMvhd(o);for(auto&t:tracks)writeTrak(o,t,encrypted,kid);auto mv=beginBox(o,"mvex");for(auto&t:tracks){auto tr=beginBox(o,"trex");full(o,0,0);w32(o,t.id);w32(o,1);w32(o,0);w32(o,0);w32(o,t.kind==mpegts::ElementaryKind::Video?0x01010000:0);endBox(o,tr);}endBox(o,mv);endBox(o,m);return o;}

std::string pdt(std::chrono::system_clock::time_point v){std::time_t t=std::chrono::system_clock::to_time_t(v);std::tm tm{};gmtime_r(&t,&tm);std::ostringstream o;o<<std::put_time(&tm,"%Y-%m-%dT%H:%M:%SZ");return o.str();}
bool atomicFile(const std::filesystem::path&path,const std::vector<std::uint8_t>&data,std::string&error){const auto tmp=path.string()+".tmp";std::ofstream f(tmp,std::ios::binary|std::ios::trunc);if(!f){error="cannot create "+tmp;return false;}f.write(reinterpret_cast<const char*>(data.data()),data.size());f.close();std::error_code ec;std::filesystem::rename(tmp,path,ec);if(ec){std::filesystem::remove(path,ec);ec.clear();std::filesystem::rename(tmp,path,ec);}if(ec){error=ec.message();return false;}return true;}

} // namespace

bool Fmp4ToMpegTs::initialize(const std::vector<std::uint8_t>& init, DataCallback output, std::string& error) {
    tracks_.clear();
    output_ = std::move(output);
    Box root{0, 0, 0, init.size()};
    const auto top = children(init, root);
    const Box* moov = find(top, fourcc("moov"));
    if (!moov) { error = "CMAF init has no moov"; return false; }

    for (const auto& trak : children(init, *moov)) {
        if (trak.type != fourcc("trak")) continue;
        TrackInfo t;
        const auto tc = children(init, trak);
        const Box* tkhd = find(tc, fourcc("tkhd"));
        const Box* mdia = find(tc, fourcc("mdia"));
        if (!tkhd || !mdia || tkhd->size < tkhd->header + 20) continue;
        const auto* tp = init.data() + tkhd->payload();
        t.id = u32(tp + 12);

        const auto mc = children(init, *mdia);
        const Box* mdhd = find(mc, fourcc("mdhd"));
        const Box* hdlr = find(mc, fourcc("hdlr"));
        const Box* minf = find(mc, fourcc("minf"));
        if (!mdhd || !hdlr || !minf) continue;
        t.timescale = u32(init.data() + mdhd->payload() + 12);
        const std::uint32_t handler = u32(init.data() + hdlr->payload() + 8);
        t.kind = handler == fourcc("soun") ? mpegts::ElementaryKind::Audio : mpegts::ElementaryKind::Video;

        const auto minic = children(init, *minf);
        const Box* stbl = find(minic, fourcc("stbl"));
        if (!stbl) continue;
        const auto sc = children(init, *stbl);
        const Box* stsd = find(sc, fourcc("stsd"));
        if (!stsd) continue;
        const auto entries = children(init, *stsd, 8);
        if (entries.empty()) continue;
        const Box& e = entries.front();
        const std::size_t sampleSkip = t.kind == mpegts::ElementaryKind::Video ? 78 : 28;
        const auto ep = children(init, e, sampleSkip);
        std::uint32_t sampleType = e.type;
        if (e.type == fourcc("encv") || e.type == fourcc("enca")) {
            const Box* sinf = find(ep, fourcc("sinf"));
            if (!sinf) { error = "CMAF encrypted sample entry has no sinf"; return false; }
            const auto sic = children(init, *sinf);
            const Box* frma = find(sic, fourcc("frma"));
            if (!frma || frma->payload() + 4 > frma->end()) { error = "CMAF encrypted sample entry has no frma"; return false; }
            sampleType = u32(init.data() + frma->payload());
        }

        if (sampleType == fourcc("avc1") || sampleType == fourcc("avc3")) {
            t.codec = mpegts::ElementaryCodec::H264;
            const Box* c = find(ep, fourcc("avcC"));
            if (c) {
                t.codecConfig.assign(init.begin() + static_cast<std::ptrdiff_t>(c->payload()), init.begin() + static_cast<std::ptrdiff_t>(c->end()));
                if (t.codecConfig.size() > 4) t.nalLengthSize = (t.codecConfig[4] & 3) + 1;
                if (t.codecConfig.size() > 6) {
                    std::size_t q = 5;
                    const int ns = t.codecConfig[q++] & 31;
                    for (int i = 0; i < ns && q + 2 <= t.codecConfig.size(); ++i) {
                        const auto n = u16(&t.codecConfig[q]); q += 2;
                        if (q + n > t.codecConfig.size()) break;
                        t.parameterSets.insert(t.parameterSets.end(), {0,0,0,1});
                        t.parameterSets.insert(t.parameterSets.end(), t.codecConfig.begin() + static_cast<std::ptrdiff_t>(q), t.codecConfig.begin() + static_cast<std::ptrdiff_t>(q+n));
                        q += n;
                    }
                    if (q < t.codecConfig.size()) {
                        const int np = t.codecConfig[q++];
                        for (int i = 0; i < np && q + 2 <= t.codecConfig.size(); ++i) {
                            const auto n = u16(&t.codecConfig[q]); q += 2;
                            if (q + n > t.codecConfig.size()) break;
                            t.parameterSets.insert(t.parameterSets.end(), {0,0,0,1});
                            t.parameterSets.insert(t.parameterSets.end(), t.codecConfig.begin() + static_cast<std::ptrdiff_t>(q), t.codecConfig.begin() + static_cast<std::ptrdiff_t>(q+n));
                            q += n;
                        }
                    }
                }
            }
        } else if (sampleType == fourcc("hvc1") || sampleType == fourcc("hev1")) {
            t.codec = mpegts::ElementaryCodec::H265;
            const Box* c = find(ep, fourcc("hvcC"));
            if (c) {
                t.codecConfig.assign(init.begin() + static_cast<std::ptrdiff_t>(c->payload()), init.begin() + static_cast<std::ptrdiff_t>(c->end()));
                if (t.codecConfig.size() > 21) t.nalLengthSize = (t.codecConfig[21] & 3) + 1;
                if (t.codecConfig.size() > 23) {
                    std::size_t q = 23;
                    const int arrays = t.codecConfig[22];
                    for (int a = 0; a < arrays && q + 3 <= t.codecConfig.size(); ++a) {
                        ++q;
                        const int num = u16(&t.codecConfig[q]); q += 2;
                        for (int i = 0; i < num && q + 2 <= t.codecConfig.size(); ++i) {
                            const auto n = u16(&t.codecConfig[q]); q += 2;
                            if (q + n > t.codecConfig.size()) break;
                            t.parameterSets.insert(t.parameterSets.end(), {0,0,0,1});
                            t.parameterSets.insert(t.parameterSets.end(), t.codecConfig.begin() + static_cast<std::ptrdiff_t>(q), t.codecConfig.begin() + static_cast<std::ptrdiff_t>(q+n));
                            q += n;
                        }
                    }
                }
            }
        } else if (sampleType == fourcc("mp4a")) {
            t.codec = mpegts::ElementaryCodec::AacAdts;
            t.channels = u16(init.data() + e.payload() + 16);
            t.sampleRate = u32(init.data() + e.payload() + 24) >> 16;
            t.timescale = t.sampleRate;
            const Box* es = find(ep, fourcc("esds"));
            if (es) {
                const auto* q = init.data() + es->payload() + 4;
                const auto* finish = init.data() + es->end();
                while (q + 2 < finish) {
                    if (*q == 0x05) {
                        const std::size_t n = q[1];
                        if (q + 2 + n <= finish) t.codecConfig.assign(q + 2, q + 2 + n);
                        break;
                    }
                    ++q;
                }
            }
        }
        if (t.codec != mpegts::ElementaryCodec::Unknown) tracks_.push_back(std::move(t));
    }

    if (tracks_.empty()) { error = "CMAF init has no supported AVC/HEVC/AAC tracks"; return false; }
    mpegts::NativeMuxConfig mc;
    mc.serviceName = "CMAF"; mc.serviceProvider = "DVBStreamer5";
    if (!mux_.initialize(mc, error)) return false;
    for (const auto& t : tracks_) if (!mux_.setCodec(t.kind, t.codec, error)) return false;
    return true;
}

bool Fmp4ToMpegTs::pushFragment(const std::vector<std::uint8_t>&frag,std::string&error){Box root{0,0,0,frag.size()};const auto top=children(frag,root);const Box*moof=find(top,fourcc("moof"));const Box*mdat=find(top,fourcc("mdat"));if(!moof||!mdat){error="CMAF fragment missing moof/mdat";return false;}for(const auto&traf:children(frag,*moof)){if(traf.type!=fourcc("traf"))continue;const auto tc=children(frag,traf);const Box*tfhd=find(tc,fourcc("tfhd"));const Box*tfdt=find(tc,fourcc("tfdt"));const Box*trun=find(tc,fourcc("trun"));if(!tfhd||!trun)continue;const auto*fh=frag.data()+tfhd->payload();const std::uint32_t tfFlags=(fh[1]<<16)|(fh[2]<<8)|fh[3];const std::uint32_t trackId=u32(fh+4);const auto ti=std::find_if(tracks_.begin(),tracks_.end(),[&](const TrackInfo&t){return t.id==trackId;});if(ti==tracks_.end())continue;std::size_t fp=8;std::uint64_t baseOffset=moof->start;if(tfFlags&0x000001){baseOffset=u64(fh+fp);fp+=8;}if(tfFlags&0x000002)fp+=4;std::uint32_t defDur=0,defSize=0,defFlags=0;if(tfFlags&0x000008){defDur=u32(fh+fp);fp+=4;}if(tfFlags&0x000010){defSize=u32(fh+fp);fp+=4;}if(tfFlags&0x000020){defFlags=u32(fh+fp);fp+=4;}std::uint64_t decodeTime=0;if(tfdt){const auto*p=frag.data()+tfdt->payload();decodeTime=p[0]?u64(p+4):u32(p+4);}const auto*rp=frag.data()+trun->payload();const std::uint8_t ver=rp[0];const std::uint32_t flags=(rp[1]<<16)|(rp[2]<<8)|rp[3];const std::uint32_t count=u32(rp+4);std::size_t q=8;std::int32_t dataOffset=0;std::uint32_t firstFlags=defFlags;if(flags&1){dataOffset=static_cast<std::int32_t>(u32(rp+q));q+=4;}if(flags&4){firstFlags=u32(rp+q);q+=4;}std::size_t dataPos=static_cast<std::size_t>(static_cast<std::int64_t>(baseOffset)+dataOffset);if(!(flags&1))dataPos=mdat->payload();for(std::uint32_t i=0;i<count;++i){std::uint32_t dur=defDur,size=defSize,sflags=i==0?firstFlags:defFlags;std::int64_t cto=0;if(flags&0x100){dur=u32(rp+q);q+=4;}if(flags&0x200){size=u32(rp+q);q+=4;}if(flags&0x400){sflags=u32(rp+q);q+=4;}if(flags&0x800){const std::uint32_t raw=u32(rp+q);q+=4;cto=ver?static_cast<std::int32_t>(raw):raw;}if(!size||dataPos+size>frag.size()){error="CMAF trun sample outside mdat";return false;}std::vector<std::uint8_t>sample;if(ti->kind==mpegts::ElementaryKind::Video){sample=annexB(frag.data()+dataPos,size,ti->nalLengthSize);const bool key=(sflags&0x00010000U)==0;if(key&&!ti->parameterSets.empty())sample.insert(sample.begin(),ti->parameterSets.begin(),ti->parameterSets.end());}else{std::array<std::uint8_t,7>h{};if(!makeAdts(*ti,size,h)){error="CMAF AAC configuration unsupported";return false;}sample.assign(h.begin(),h.end());sample.insert(sample.end(),frag.begin()+static_cast<std::ptrdiff_t>(dataPos),frag.begin()+static_cast<std::ptrdiff_t>(dataPos+size));}mpegts::ElementarySample es;es.data=sample.data();es.size=sample.size();es.hasDts=es.hasPts=true;es.dts90k=decodeTime*90000ULL/ti->timescale;const std::int64_t ptsScale=static_cast<std::int64_t>(decodeTime)+cto;es.pts90k=ptsScale<0?0:static_cast<std::uint64_t>(ptsScale)*90000ULL/ti->timescale;es.randomAccess=(sflags&0x00010000U)==0;std::vector<mpegts::Packet>out;if(!mux_.write(ti->kind,es,out,error))return false;std::vector<std::uint8_t>raw;raw.reserve(out.size()*188);for(auto&p:out)raw.insert(raw.end(),p.begin(),p.end());if(output_&&!raw.empty()&&!output_(raw.data(),raw.size())){error="CMAF output callback rejected TS";return false;}decodeTime+=dur;dataPos+=size;}}
    return true;}

NativeCmafSegmenter::NativeCmafSegmenter(){demux_.setSampleCallback([this](mpegts::DemuxSample&&s){onSample(std::move(s));});}
NativeCmafSegmenter::~NativeCmafSegmenter(){stop();}
bool NativeCmafSegmenter::start(const SegmenterConfig& c, std::string& error) {
    stop();
    std::lock_guard<std::mutex> l(mutex_);
    config_ = c;
    config_.targetDurationSeconds = std::clamp(c.targetDurationSeconds, 1.0, 10.0);
    config_.liveWindowSegments = std::clamp<std::size_t>(c.liveWindowSegments, 3, 30);
    if (config_.encryption != "sample-aes") config_.encryption = "none";
    if (config_.encryption == "sample-aes" && !config_.hasKey) {
        if (RAND_bytes(config_.key.data(), static_cast<int>(config_.key.size())) != 1) {
            error = "cannot generate CMAF SAMPLE-AES key"; return false;
        }
        config_.hasKey = true;
    }
    std::error_code ec;
    std::filesystem::create_directories(config_.directory, ec);
    if (ec) { error = ec.message(); return false; }
    if (config_.encryption == "sample-aes") {
        const std::string keyName = std::filesystem::path(config_.keyUri.empty() ? "key.bin" : config_.keyUri).filename().string();
        std::ofstream k(config_.directory / keyName, std::ios::binary | std::ios::trunc);
        if (!k) { error = "cannot write CMAF SAMPLE-AES key"; return false; }
        k.write(reinterpret_cast<const char*>(config_.key.data()), static_cast<std::streamsize>(config_.key.size()));
    }
    pending_.clear(); tracks_.clear(); live_.clear(); sequence_ = completed_ = 0;
    haveSegmentStart_ = initialized_ = false; programTime_ = std::chrono::system_clock::now();
    lastError_.clear(); running_ = true; demux_.reset();
    if (!writePlaylist(false, error)) { running_ = false; return false; }
    return true;
}

bool NativeCmafSegmenter::push(const std::uint8_t*d,std::size_t n){std::lock_guard<std::mutex>l(mutex_);if(!running_)return false;std::string e;if(!demux_.push(d,n,e)){fail(e);return false;}return true;}
void NativeCmafSegmenter::onSample(mpegts::DemuxSample&&s){if(!running_)return;Sample v;v.stream=s.stream;v.data=std::move(s.data);v.pts90k=s.hasPts?s.pts90k:s.dts90k;v.dts90k=s.hasDts?s.dts90k:v.pts90k;v.key=s.randomAccess;if(!haveSegmentStart_){segmentStartPts_=v.pts90k;haveSegmentStart_=true;}const double elapsed=static_cast<double>(v.pts90k>=segmentStartPts_?v.pts90k-segmentStartPts_:0)/90000.0;if(v.stream.kind==mpegts::ElementaryKind::Video&&v.key&&elapsed>=config_.targetDurationSeconds&&!pending_.empty()){std::string e;if(!rotate(e)){fail(e);return;}segmentStartPts_=v.pts90k;}pending_.push_back(std::move(v));std::string e;if(!initialized_)maybeInitialize(e);}
bool NativeCmafSegmenter::maybeInitialize(std::string&error){std::map<std::uint16_t,TrackInfo>byPid;for(const auto&s:pending_){auto&t=byPid[s.stream.pid];t.kind=s.stream.kind;t.codec=s.stream.codec;t.timescale=s.stream.kind==mpegts::ElementaryKind::Video?90000:48000;t.width=config_.width;t.height=config_.height;if(s.stream.kind==mpegts::ElementaryKind::Video){for(auto[p,n]:nals(s.data)){const auto type=s.stream.codec==mpegts::ElementaryCodec::H264?(p[0]&31):((p[0]>>1)&63);if((s.stream.codec==mpegts::ElementaryCodec::H264&&(type==7||type==8))||(s.stream.codec==mpegts::ElementaryCodec::H265&&(type==32||type==33||type==34))){t.parameterSets.insert(t.parameterSets.end(),{0,0,0,1});t.parameterSets.insert(t.parameterSets.end(),p,p+n);}}}else if(s.stream.codec==mpegts::ElementaryCodec::AacAdts){std::size_t h=0,f=0;int rate=0,ch=0;std::vector<std::uint8_t>asc;if(adtsInfo(s.data,h,f,rate,ch,asc)){t.sampleRate=t.timescale=rate;t.channels=ch;t.codecConfig=asc;}}}
    bool haveVideo=false;for(auto&[pid,t]:byPid){(void)pid;if(t.kind==mpegts::ElementaryKind::Video){haveVideo=true;t.codecConfig=t.codec==mpegts::ElementaryCodec::H264?avcC(t.parameterSets):hvcC(t.parameterSets);if(t.codecConfig.empty())return true;}}
    if(!haveVideo&&byPid.empty())return true;tracks_.clear();std::uint32_t id=1;for(auto&[pid,t]:byPid){(void)pid;if(t.kind==mpegts::ElementaryKind::Audio&&t.codecConfig.empty())continue;t.id=id++;tracks_.push_back(std::move(t));}if(tracks_.empty())return true;if(!writeInit(error))return false;initialized_=true;return true;}
bool NativeCmafSegmenter::writeInit(std::string&error){return atomicFile(config_.directory/"init.mp4",makeInit(tracks_,config_.encryption=="sample-aes",config_.key),error);}

bool NativeCmafSegmenter::rotate(std::string& error, bool force) {
    if (pending_.empty()) return true;
    if (!initialized_ && !maybeInitialize(error)) return false;
    if (!initialized_ && !force) return true;

    std::map<mpegts::ElementaryKind, const TrackInfo*> trackByKind;
    for (const auto& t : tracks_) trackByKind[t.kind] = &t;

    std::vector<Sample> samples;
    samples.swap(pending_);
    std::map<mpegts::ElementaryKind, std::vector<Sample*>> groups;
    for (auto& s : samples) groups[s.stream.kind].push_back(&s);

    struct BuiltTrack {
        mpegts::ElementaryKind kind{};
        const TrackInfo* track = nullptr;
        std::vector<std::vector<std::uint8_t>> payloads;
        std::vector<SampleAux> aux;
        std::size_t trunOffsetPatch = 0;
        std::size_t saioOffsetPatch = 0;
        std::size_t sencAuxOffset = 0;
        std::size_t payloadBytes = 0;
    };
    std::vector<BuiltTrack> built;
    std::vector<std::uint8_t> moof;
    const bool encrypted = config_.encryption == "sample-aes";

    const auto mf = beginBox(moof, "moof");
    const auto mh = beginBox(moof, "mfhd");
    full(moof, 0, 0);
    w32(moof, static_cast<std::uint32_t>(sequence_ + 1));
    endBox(moof, mh);

    for (auto& [kind, vec] : groups) {
        auto it = trackByKind.find(kind);
        if (it == trackByKind.end() || vec.empty()) continue;
        const TrackInfo& t = *it->second;
        BuiltTrack bt;
        bt.kind = kind;
        bt.track = &t;
        const bool video = kind == mpegts::ElementaryKind::Video;

        const auto traf = beginBox(moof, "traf");
        const auto tf = beginBox(moof, "tfhd");
        full(moof, 0, 0x020000); // default-base-is-moof
        w32(moof, t.id);
        endBox(moof, tf);

        const auto td = beginBox(moof, "tfdt");
        full(moof, 1, 0);
        w64(moof, vec.front()->dts90k * t.timescale / 90000ULL);
        endBox(moof, td);

        const auto tr = beginBox(moof, "trun");
        full(moof, 1, 0x000f01); // data-offset + dur/size/flags/cto
        w32(moof, static_cast<std::uint32_t>(vec.size()));
        bt.trunOffsetPatch = moof.size();
        w32(moof, 0);

        bt.payloads.reserve(vec.size());
        bt.aux.reserve(vec.size());
        for (std::size_t i = 0; i < vec.size(); ++i) {
            const auto* cur = vec[i];
            const std::uint64_t nextDts = i + 1 < vec.size() ? vec[i + 1]->dts90k
                : cur->dts90k + (video ? 3600 : 1920);
            const std::uint32_t dur = static_cast<std::uint32_t>(std::max<std::uint64_t>(
                1, (nextDts - cur->dts90k) * t.timescale / 90000ULL));

            std::vector<std::uint8_t> payload;
            if (video) {
                payload = lengthPrefixed(cur->data);
            } else {
                std::size_t h = 0, f = 0; int rate = 0, ch = 0; std::vector<std::uint8_t> asc;
                if (adtsInfo(cur->data, h, f, rate, ch, asc))
                    payload.assign(cur->data.begin() + static_cast<std::ptrdiff_t>(h), cur->data.begin() + static_cast<std::ptrdiff_t>(f));
                else
                    payload = cur->data;
            }

            SampleAux aux;
            if (encrypted) {
                makeIv(sequence_ + 1, t.id, static_cast<std::uint32_t>(i), aux.iv);
                aux.subs = video ? videoSubsamples(payload) : audioSubsamples(payload.size());
                if (!transformBySubsamples(payload, aux, config_.key, true, video)) {
                    error = "CMAF cbcs encryption failed";
                    return false;
                }
            }

            w32(moof, dur);
            w32(moof, static_cast<std::uint32_t>(payload.size()));
            w32(moof, video ? (cur->key ? 0x02000000U : 0x01010000U) : 0x02000000U);
            const std::int64_t cto90 = static_cast<std::int64_t>(cur->pts90k) - static_cast<std::int64_t>(cur->dts90k);
            const std::int64_t cto = cto90 * static_cast<std::int64_t>(t.timescale) / 90000;
            w32(moof, static_cast<std::uint32_t>(static_cast<std::int32_t>(cto)));
            bt.payloadBytes += payload.size();
            bt.payloads.push_back(std::move(payload));
            if (encrypted) bt.aux.push_back(std::move(aux));
        }
        endBox(moof, tr);

        if (encrypted) {
            const auto senc = beginBox(moof, "senc");
            full(moof, 0, 0x000002); // subsample encryption present
            w32(moof, static_cast<std::uint32_t>(bt.aux.size()));
            bt.sencAuxOffset = moof.size(); // first IV, relative to moof start
            for (const auto& aux : bt.aux) {
                moof.insert(moof.end(), aux.iv.begin(), aux.iv.end());
                w16(moof, static_cast<std::uint16_t>(aux.subs.size()));
                for (const auto& ss : aux.subs) { w16(moof, ss.clear); w32(moof, ss.encrypted); }
            }
            endBox(moof, senc);

            const auto saiz = beginBox(moof, "saiz");
            full(moof, 0, 0);
            moof.push_back(0); // per-sample sizes follow
            w32(moof, static_cast<std::uint32_t>(bt.aux.size()));
            for (const auto& aux : bt.aux) {
                const std::size_t n = 16 + 2 + aux.subs.size() * 6;
                if (n > 255) { error = "CMAF SAMPLE-AES auxiliary record too large"; return false; }
                moof.push_back(static_cast<std::uint8_t>(n));
            }
            endBox(moof, saiz);

            const auto saio = beginBox(moof, "saio");
            full(moof, 0, 0);
            w32(moof, 1);
            bt.saioOffsetPatch = moof.size();
            w32(moof, 0);
            endBox(moof, saio);
        }

        endBox(moof, traf);
        built.push_back(std::move(bt));
    }
    endBox(moof, mf);

    std::size_t payloadBefore = 0;
    for (auto& bt : built) {
        patch32(moof, bt.trunOffsetPatch, static_cast<std::uint32_t>(moof.size() + 8 + payloadBefore));
        if (encrypted) patch32(moof, bt.saioOffsetPatch, static_cast<std::uint32_t>(bt.sencAuxOffset));
        payloadBefore += bt.payloadBytes;
    }

    std::vector<std::uint8_t> out;
    const auto st = beginBox(out, "styp");
    out.insert(out.end(), {'m','s','d','h'}); w32(out, 0); out.insert(out.end(), {'m','s','d','h','m','s','i','x'});
    endBox(out, st);
    out.insert(out.end(), moof.begin(), moof.end());
    const auto md = beginBox(out, "mdat");
    for (const auto& bt : built) for (const auto& payload : bt.payloads) out.insert(out.end(), payload.begin(), payload.end());
    endBox(out, md);

    std::ostringstream name;
    name << "segment" << std::setw(10) << std::setfill('0') << sequence_ << ".m4s";
    if (!atomicFile(config_.directory / name.str(), out, error)) return false;

    double dur = config_.targetDurationSeconds;
    if (!samples.empty()) {
        auto mm = std::minmax_element(samples.begin(), samples.end(), [](const Sample& a, const Sample& b){ return a.pts90k < b.pts90k; });
        dur = std::max(0.05, static_cast<double>(mm.second->pts90k - mm.first->pts90k) / 90000.0);
    }
    live_.push_back({sequence_++, name.str(), dur, programTime_});
    programTime_ += std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::duration<double>(dur));
    ++completed_;
    prune();
    return writePlaylist(false, error);
}

bool decryptSampleAesFragment(std::vector<std::uint8_t>& fragment,
                              const std::array<std::uint8_t,16>& key,
                              std::string& error) {
    Box root{0,0,0,fragment.size()};
    const auto top = children(fragment, root);
    const Box* moof = find(top, fourcc("moof"));
    const Box* mdat = find(top, fourcc("mdat"));
    if (!moof || !mdat) { error = "CMAF encrypted fragment missing moof/mdat"; return false; }

    for (const auto& traf : children(fragment, *moof)) {
        if (traf.type != fourcc("traf")) continue;
        const auto tc = children(fragment, traf);
        const Box* tfhd = find(tc, fourcc("tfhd"));
        const Box* trun = find(tc, fourcc("trun"));
        const Box* senc = find(tc, fourcc("senc"));
        if (!tfhd || !trun || !senc) continue;

        const auto* fh = fragment.data() + tfhd->payload();
        if (tfhd->payload() + 8 > tfhd->end()) { error = "CMAF tfhd truncated"; return false; }
        const std::uint32_t tfFlags = (fh[1] << 16) | (fh[2] << 8) | fh[3];
        std::size_t fp = 8;
        std::uint64_t baseOffset = moof->start;
        if (tfFlags & 0x000001) { if (tfhd->payload()+fp+8>tfhd->end()) return false; baseOffset = u64(fh + fp); fp += 8; }
        if (tfFlags & 0x000002) fp += 4;
        std::uint32_t defDur = 0, defSize = 0, defFlags = 0;
        if (tfFlags & 0x000008) { defDur = u32(fh + fp); fp += 4; }
        if (tfFlags & 0x000010) { defSize = u32(fh + fp); fp += 4; }
        if (tfFlags & 0x000020) { defFlags = u32(fh + fp); fp += 4; }
        (void)defDur; (void)defFlags;

        const auto* rp = fragment.data() + trun->payload();
        if (trun->payload()+8>trun->end()) { error="CMAF trun truncated"; return false; }
        const std::uint32_t flags = (rp[1] << 16) | (rp[2] << 8) | rp[3];
        const std::uint32_t count = u32(rp + 4);
        std::size_t q = 8;
        std::int32_t dataOffset = 0;
        if (flags & 1) { dataOffset = static_cast<std::int32_t>(u32(rp + q)); q += 4; }
        if (flags & 4) q += 4;
        std::size_t dataPos = (flags & 1) ? static_cast<std::size_t>(static_cast<std::int64_t>(baseOffset) + dataOffset) : mdat->payload();

        const auto* sp = fragment.data() + senc->payload();
        if (senc->payload()+8>senc->end()) { error="CMAF senc truncated"; return false; }
        const std::uint32_t sencFlags = (sp[1] << 16) | (sp[2] << 8) | sp[3];
        if ((sencFlags & 0x2) == 0) { error = "CMAF SAMPLE-AES senc without subsamples is unsupported"; return false; }
        const std::uint32_t auxCount = u32(sp + 4);
        if (auxCount != count) { error = "CMAF senc/trun sample count mismatch"; return false; }
        std::size_t aq = 8;

        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t size = defSize;
            if (flags & 0x100) q += 4;
            if (flags & 0x200) { size = u32(rp + q); q += 4; }
            if (flags & 0x400) q += 4;
            if (flags & 0x800) q += 4;
            if (!size || dataPos + size > fragment.size()) { error = "CMAF encrypted sample outside mdat"; return false; }
            if (senc->payload()+aq+18>senc->end()) { error="CMAF senc sample truncated"; return false; }
            SampleAux aux;
            std::copy(sp + aq, sp + aq + 16, aux.iv.begin()); aq += 16;
            const std::uint16_t sc = u16(sp + aq); aq += 2;
            for (std::uint16_t j=0; j<sc; ++j) {
                if (senc->payload()+aq+6>senc->end()) { error="CMAF senc subsample truncated"; return false; }
                aux.subs.push_back({u16(sp+aq), u32(sp+aq+2)}); aq += 6;
            }
            std::vector<std::uint8_t> payload(fragment.begin()+static_cast<std::ptrdiff_t>(dataPos), fragment.begin()+static_cast<std::ptrdiff_t>(dataPos+size));
            bool video = false;
            for (const auto& ss : aux.subs) if (ss.clear >= 32) { video = true; break; }
            if (!transformBySubsamples(payload, aux, key, false, video)) { error="CMAF cbcs decryption failed"; return false; }
            std::copy(payload.begin(), payload.end(), fragment.begin()+static_cast<std::ptrdiff_t>(dataPos));
            dataPos += size;
        }
    }
    return true;
}

bool NativeCmafSegmenter::writePlaylist(bool end, std::string& error) {
    const auto tmp = config_.directory / "video.m3u8.tmp";
    std::ofstream o(tmp, std::ios::trunc);
    if (!o) { error = "cannot create CMAF playlist"; return false; }
    double mx = config_.targetDurationSeconds;
    for (auto& s : live_) mx = std::max(mx, s.duration);
    o << "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-INDEPENDENT-SEGMENTS\n"
      << "#EXT-X-TARGETDURATION:" << static_cast<unsigned>(std::ceil(mx)) << "\n"
      << "#EXT-X-MEDIA-SEQUENCE:" << (live_.empty() ? sequence_ : live_.front().sequence) << "\n";
    if (config_.encryption == "sample-aes") {
        o << "#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\""
          << (config_.keyUri.empty() ? "key.bin" : config_.keyUri)
          << "\",KEYFORMAT=\"identity\"\n";
    }
    o << "#EXT-X-MAP:URI=\"init.mp4\"\n";
    for (auto& s : live_) {
        o << "#EXT-X-PROGRAM-DATE-TIME:" << pdt(s.wallTime) << "\n"
          << "#EXTINF:" << std::fixed << std::setprecision(3) << s.duration << ",\n"
          << s.fileName << "\n";
    }
    if (end) o << "#EXT-X-ENDLIST\n";
    o.close();
    std::error_code ec; std::filesystem::rename(tmp, config_.directory / "video.m3u8", ec);
    if (ec) { std::filesystem::remove(config_.directory / "video.m3u8", ec); ec.clear(); std::filesystem::rename(tmp, config_.directory / "video.m3u8", ec); }
    if (ec) { error = ec.message(); return false; }
    return true;
}

void NativeCmafSegmenter::prune(){while(live_.size()>config_.liveWindowSegments){auto old=live_.front();live_.pop_front();if(!config_.archiveEnabled){std::error_code ec;std::filesystem::remove(config_.directory/old.fileName,ec);}}}
void NativeCmafSegmenter::fail(const std::string&e){lastError_=e;running_=false;}
void NativeCmafSegmenter::stop()noexcept{std::lock_guard<std::mutex>l(mutex_);if(!running_)return;demux_.flush();std::string e;rotate(e,true);writePlaylist(true,e);running_=false;}
bool NativeCmafSegmenter::isRunning()const noexcept{std::lock_guard<std::mutex>l(mutex_);return running_;}std::string NativeCmafSegmenter::lastError()const{std::lock_guard<std::mutex>l(mutex_);return lastError_;}std::uint64_t NativeCmafSegmenter::segmentCount()const noexcept{std::lock_guard<std::mutex>l(mutex_);return completed_;}

} // namespace dvbstreamer5::media::cmaf
