#include "media/NativeSampleAes.h"

#include "media/NativeMpegTsMux.h"
#include "media/NativeTsDemux.h"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

namespace dvbstreamer5::media::hls {
namespace {

struct CtxFree { void operator()(EVP_CIPHER_CTX* p) const noexcept { EVP_CIPHER_CTX_free(p); } };
using Ctx = std::unique_ptr<EVP_CIPHER_CTX, CtxFree>;

bool aesEcbBlock(const std::array<std::uint8_t,16>& key, bool encrypt,
                 const std::uint8_t in[16], std::uint8_t out[16]) {
    Ctx ctx(EVP_CIPHER_CTX_new());
    if (!ctx) return false;
    int n=0, tail=0;
    if (encrypt) {
        if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_128_ecb(), nullptr, key.data(), nullptr) != 1) return false;
        EVP_CIPHER_CTX_set_padding(ctx.get(), 0);
        return EVP_EncryptUpdate(ctx.get(), out, &n, in, 16) == 1 && n == 16 &&
               EVP_EncryptFinal_ex(ctx.get(), out+n, &tail) == 1;
    }
    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_128_ecb(), nullptr, key.data(), nullptr) != 1) return false;
    EVP_CIPHER_CTX_set_padding(ctx.get(), 0);
    return EVP_DecryptUpdate(ctx.get(), out, &n, in, 16) == 1 && n == 16 &&
           EVP_DecryptFinal_ex(ctx.get(), out+n, &tail) == 1;
}

bool cbcPattern(std::uint8_t* p, std::size_t size, std::size_t clearPrefix,
                bool oneInTen, const std::array<std::uint8_t,16>& key,
                const std::array<std::uint8_t,16>& iv, bool encrypt) {
    if (size <= clearPrefix) return true;
    std::array<std::uint8_t,16> chain=iv;
    std::size_t off=clearPrefix;
    unsigned pattern=0;
    while (off + 16 <= size) {
        const bool crypt = !oneInTen || pattern == 0;
        if (crypt) {
            std::uint8_t in[16], tmp[16], out[16];
            std::memcpy(in,p+off,16);
            if (encrypt) {
                for(int i=0;i<16;++i) tmp[i]=static_cast<std::uint8_t>(in[i]^chain[static_cast<std::size_t>(i)]);
                if(!aesEcbBlock(key,true,tmp,out)) return false;
                std::memcpy(p+off,out,16); std::copy(out,out+16,chain.begin());
            } else {
                if(!aesEcbBlock(key,false,in,tmp)) return false;
                for(int i=0;i<16;++i) out[i]=static_cast<std::uint8_t>(tmp[i]^chain[static_cast<std::size_t>(i)]);
                std::copy(in,in+16,chain.begin()); std::memcpy(p+off,out,16);
            }
        }
        off += 16;
        if (oneInTen) pattern = (pattern + 1) % 10;
    }
    return true;
}

std::vector<std::pair<std::size_t,std::size_t>> annexBNals(const std::vector<std::uint8_t>& d) {
    std::vector<std::pair<std::size_t,std::size_t>> out;
    auto start=[&](std::size_t i)->std::size_t {
        if(i+3<=d.size()&&d[i]==0&&d[i+1]==0&&d[i+2]==1) return 3;
        if(i+4<=d.size()&&d[i]==0&&d[i+1]==0&&d[i+2]==0&&d[i+3]==1) return 4;
        return 0;
    };
    std::size_t i=0;
    while(i<d.size()) {
        std::size_t sc=0; while(i<d.size() && !(sc=start(i))) ++i;
        if(!sc) break;
        const std::size_t b=i+sc; i=b;
        while(i<d.size() && start(i)==0) ++i;
        if(i>b) out.emplace_back(b,i-b);
    }
    return out;
}

bool transformVideo(std::vector<std::uint8_t>& d, mpegts::ElementaryCodec codec,
                    const std::array<std::uint8_t,16>& key,
                    const std::array<std::uint8_t,16>& iv, bool encrypt) {
    for (const auto& n:annexBNals(d)) {
        if(!n.second) continue;
        bool vcl=false;
        if(codec==mpegts::ElementaryCodec::H264) {
            const unsigned t=d[n.first]&0x1fU; vcl=(t>=1&&t<=5);
        } else if(codec==mpegts::ElementaryCodec::H265) {
            const unsigned t=(d[n.first]>>1)&0x3fU; vcl=t<=31;
        }
        if(vcl && !cbcPattern(d.data()+n.first,n.second,32,true,key,iv,encrypt)) return false;
    }
    return true;
}

bool transformAac(std::vector<std::uint8_t>& d,
                  const std::array<std::uint8_t,16>& key,
                  const std::array<std::uint8_t,16>& iv, bool encrypt) {
    std::size_t off=0;
    while(off+7<=d.size()) {
        if(d[off]!=0xff || (d[off+1]&0xf6)!=0xf0) { ++off; continue; }
        const std::size_t hdr=(d[off+1]&1U)?7U:9U;
        const std::size_t len=((static_cast<std::size_t>(d[off+3]&3U)<<11)|
                              (static_cast<std::size_t>(d[off+4])<<3)|
                              (static_cast<std::size_t>(d[off+5])>>5));
        if(len<hdr || off+len>d.size()) break;
        const std::size_t raw=len-hdr;
        // SAMPLE-AES leaves 16 bytes of each AAC access unit clear and CBC-encrypts
        // the complete following blocks; any trailing partial block stays clear.
        if(raw>16 && !cbcPattern(d.data()+off+hdr,raw,16,false,key,iv,encrypt)) return false;
        off += len;
    }
    return true;
}

} // namespace

bool parseHexKey16(const std::string& value, std::array<std::uint8_t,16>& key) {
    std::string s=value;
    if(s.rfind("0x",0)==0||s.rfind("0X",0)==0) s.erase(0,2);
    if(s.size()!=32) return false;
    for(std::size_t i=0;i<16;++i) {
        try { key[i]=static_cast<std::uint8_t>(std::stoul(s.substr(i*2,2),nullptr,16)); }
        catch(...) { return false; }
    }
    return true;
}

bool transformSampleAesMpegTs(const std::uint8_t* data, std::size_t size,
                              const std::array<std::uint8_t,16>& key,
                              const std::array<std::uint8_t,16>& iv,
                              bool encrypt, std::vector<std::uint8_t>& output,
                              std::string& error) {
    output.clear(); error.clear();
    if(!data || !size) { error="SAMPLE-AES MPEG-TS input is empty"; return false; }
    mpegts::NativeTsDemux demux;
    std::vector<mpegts::DemuxSample> samples;
    std::vector<mpegts::DemuxStreamInfo> streams;
    demux.setProgramCallback([&](const std::vector<mpegts::DemuxStreamInfo>& s){streams=s;});
    demux.setSampleCallback([&](mpegts::DemuxSample&& s){samples.push_back(std::move(s));});
    std::string demuxError; if(!demux.push(data,size,demuxError)) { error=demuxError.empty()?"SAMPLE-AES TS demux failed":demuxError; return false; }
    demux.flush();
    if(samples.empty()) { error="SAMPLE-AES TS contains no elementary samples"; return false; }

    mpegts::NativeMuxConfig cfg;
    cfg.serviceId=demux.programNumber()?demux.programNumber():1;
    for(const auto& s:streams) {
        if(s.kind==mpegts::ElementaryKind::Video) cfg.videoPid=s.pid;
        else if(s.kind==mpegts::ElementaryKind::Audio) cfg.audioPid=s.pid;
    }
    cfg.targetBitrate=0;
    mpegts::NativeMpegTsMux mux;
    std::string muxError;
    if(!mux.initialize(cfg,muxError)) {error=muxError;return false;}
    for(const auto& s:streams) if(!mux.setCodec(s.kind,s.codec,muxError)){error=muxError;return false;}
    for(auto& sample:samples) {
        if(sample.stream.codec==mpegts::ElementaryCodec::H264 || sample.stream.codec==mpegts::ElementaryCodec::H265) {
            if(!transformVideo(sample.data,sample.stream.codec,key,iv,encrypt)) {error="SAMPLE-AES video AES transform failed";return false;}
        } else if(sample.stream.codec==mpegts::ElementaryCodec::AacAdts) {
            if(!transformAac(sample.data,key,iv,encrypt)) {error="SAMPLE-AES AAC transform failed";return false;}
        }
        mpegts::ElementarySample in;
        in.data=sample.data.data(); in.size=sample.data.size();
        in.pts90k=sample.pts90k; in.dts90k=sample.dts90k; in.hasPts=sample.hasPts; in.hasDts=sample.hasDts;
        in.randomAccess=sample.randomAccess;
        std::vector<mpegts::Packet> packets;
        if(!mux.write(sample.stream.kind,in,packets,muxError)) {error=muxError.empty()?"SAMPLE-AES native TS mux failed":muxError;return false;}
        const auto before=output.size(); output.resize(before+packets.size()*mpegts::kPacketSize);
        std::size_t p=before; for(const auto& pkt:packets){std::memcpy(output.data()+p,pkt.data(),mpegts::kPacketSize);p+=mpegts::kPacketSize;}
    }
    return !output.empty();
}

} // namespace dvbstreamer5::media::hls
