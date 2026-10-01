#include "media/NativeCodecRuntime.h"
#include "media/NativeMpegTsMux.h"
#include "media/NativeTranscoderPipeline.h"
#include "media/NativeTsDemux.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace dvbstreamer5::media;

static bool makeInput(std::vector<std::uint8_t>& ts, std::string& error) {
    auto enc = codec::createVideoEncoder(mpegts::ElementaryCodec::H264, error);
    if (!enc || !enc->configure(320, 240, 25.0, 500000, error)) return false;
    mpegts::NativeMpegTsMux mux;
    mpegts::NativeMuxConfig mc; mc.serviceId=1; mc.videoPid=0x100; mc.audioPid=0x101;
    if (!mux.initialize(mc,error) || !mux.setCodec(mpegts::ElementaryKind::Video,mpegts::ElementaryCodec::H264,error)) return false;
    codec::RawVideoFrame raw; raw.width=320;raw.height=240;raw.i420.resize(320*240*3/2,128);raw.hasPts=raw.hasDts=true;
    std::fill(raw.i420.begin(),raw.i420.begin()+320*240,32);
    for(int i=0;i<8;++i){
        raw.pts90k=raw.dts90k=90000ULL+i*3600ULL;
        std::vector<codec::EncodedVideoFrame> frames;
        if(!enc->encode(raw,frames,error)) return false;
        for(auto& f:frames){
            // Preserve the encoder access unit as one PES.  The demux must not
            // invent AVC/HEVC picture boundaries from partial slice headers;
            // live decoder resynchronization is handled by the codec wrapper's
            // IDR/IRAP gate.
            mpegts::ElementarySample s;
            s.data=f.data.data();s.size=f.data.size();
            s.pts90k=f.pts90k;s.dts90k=f.dts90k;
            s.hasPts=f.hasPts;s.hasDts=f.hasDts;
            s.randomAccess=f.keyFrame;s.duration90k=3600;
            std::vector<mpegts::Packet> p;
            if(!mux.write(mpegts::ElementaryKind::Video,s,p,error))return false;
            for(auto&x:p)ts.insert(ts.end(),x.begin(),x.end());
        }
    }
    return !ts.empty();
}

int main(){
    std::string error; std::vector<std::uint8_t> input;
    if(!makeInput(input,error)){std::cerr<<"input: "<<error<<"\n";return 1;}
    transcode::NativeTranscoderConfig cfg; cfg.videoCodec="h264";cfg.audioCodec="copy";cfg.width=160;cfg.height=120;cfg.fps=25;cfg.videoBitrate=250000;cfg.serviceId=1;cfg.videoPid=0x110;cfg.audioPid=0x111;
    transcode::NativeTranscoderPipeline tc;if(!tc.initialize(cfg,error)){std::cerr<<"init: "<<error<<"\n";return 2;}
    std::vector<std::uint8_t> output,chunk; const std::size_t step=7*188;
    for(std::size_t off=0;off<input.size();off+=step){if(!tc.process(input.data()+off,std::min(step,input.size()-off),chunk,error)){std::cerr<<"process: "<<error<<"\n";return 3;}output.insert(output.end(),chunk.begin(),chunk.end());}
    if(!tc.flush(chunk,error)){std::cerr<<"flush: "<<error<<"\n";return 4;}output.insert(output.end(),chunk.begin(),chunk.end());
    if(output.empty()){std::cerr<<"no transcoded TS\n";return 5;}
    mpegts::NativeTsDemux demux;int samples=0;int width=0,height=0;auto dec=codec::createVideoDecoder(mpegts::ElementaryCodec::H264,error);if(!dec){std::cerr<<error<<"\n";return 6;}
    demux.setSampleCallback([&](mpegts::DemuxSample&& s){if(s.stream.kind!=mpegts::ElementaryKind::Video)return;std::vector<codec::RawVideoFrame> raw;std::string e;if(dec->decode(s.data.data(),s.data.size(),s.pts90k,s.hasPts,raw,e)){samples+=static_cast<int>(raw.size());if(!raw.empty()){width=raw.back().width;height=raw.back().height;}}});
    if(!demux.push(output.data(),output.size(),error)){std::cerr<<error<<"\n";return 7;}demux.flush();
    if(samples<=0||width!=160||height!=120){std::cerr<<"bad output samples="<<samples<<" size="<<width<<"x"<<height<<"\n";return 8;}
    std::cout<<"PASS: native H.264 decode/scale/encode -> MPEG-TS\n";return 0;
}
