#include "media/NativeCodecRuntime.h"
#include "media/NativeFfmpegAvcDecoder.h"
#include "media/NativeMpegTsMux.h"
#include "media/NativeTranscoderPipeline.h"
#include "media/NativeTsDemux.h"
#include "media/NativeVideoProcessing.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
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

// The optimized scaler must be byte-for-byte identical to the old 8-bit
// fixed-point four-tap interpolation on Y, U and V planes. In particular,
// check reusing the chroma coordinate map for both U and V and warm-cache
// operation across several frame geometries.
static bool testScalerBitExact() {
    // Keep this a CPU-scaler regression even if a developer opts into VAAPI
    // software-frame VPP in their shell.
    (void)setenv("DVBSTREAMER5_ENABLE_VAAPI_SOFTWARE_SCALE", "0", 1);
    struct Geometry { int sw, sh, dw, dh; };
    const std::array<Geometry, 6> geometries{{
        {720, 576, 1920, 1080},
        {320, 240, 160, 120},
        {720, 576, 1920, 1080},
        {352, 288, 704, 576},
        {320, 240, 320, 240},
        {640, 480, 1280, 720}
    }};
    std::uint32_t seed = 0x1234abcdU;
    for (const auto& size : geometries) {
        codec::RawVideoFrame input;
        input.width = size.sw;
        input.height = size.sh;
        input.pts90k = 123456;
        input.hasPts = true;
        input.keyFrame = true;
        input.i420.resize(static_cast<std::size_t>(size.sw) * size.sh * 3U / 2U);
        for (auto& pixel : input.i420) {
            seed = seed * 1664525U + 1013904223U;
            pixel = static_cast<std::uint8_t>(seed >> 24U);
        }

        codec::RawVideoFrame actual;
        std::string error;
        if (!codec::scaleI420(input, size.dw, size.dh, actual, error)) {
            std::cerr << "I420 scaler returned error: " << error << "\n";
            return false;
        }
        std::vector<std::uint8_t> expected(actual.i420.size(), 0);
        std::size_t srcOffset = 0, dstOffset = 0;
        for (int plane = 0; plane < 3; ++plane) {
            const int sw = plane == 0 ? size.sw : size.sw / 2;
            const int sh = plane == 0 ? size.sh : size.sh / 2;
            const int dw = plane == 0 ? size.dw : size.dw / 2;
            const int dh = plane == 0 ? size.dh : size.dh / 2;
            for (int y = 0; y < dh; ++y) {
                const std::uint64_t yfp = dh > 1
                    ? static_cast<std::uint64_t>(y) * (sh - 1) * 256ULL /
                      static_cast<std::uint64_t>(dh - 1) : 0ULL;
                const int y0 = static_cast<int>(yfp >> 8U);
                const int y1 = std::min(y0 + 1, sh - 1);
                const unsigned fy = static_cast<unsigned>(yfp & 255U);
                for (int x = 0; x < dw; ++x) {
                    const std::uint64_t xfp = dw > 1
                        ? static_cast<std::uint64_t>(x) * (sw - 1) * 256ULL /
                          static_cast<std::uint64_t>(dw - 1) : 0ULL;
                    const int x0 = static_cast<int>(xfp >> 8U);
                    const int x1 = std::min(x0 + 1, sw - 1);
                    const unsigned fx = static_cast<unsigned>(xfp & 255U);
                    const auto* src = input.i420.data() + srcOffset;
                    const unsigned top =
                        static_cast<unsigned>(src[y0 * sw + x0]) * (256U - fx) +
                        static_cast<unsigned>(src[y0 * sw + x1]) * fx;
                    const unsigned bottom =
                        static_cast<unsigned>(src[y1 * sw + x0]) * (256U - fx) +
                        static_cast<unsigned>(src[y1 * sw + x1]) * fx;
                    expected[dstOffset + static_cast<std::size_t>(y) * dw + x] =
                        static_cast<std::uint8_t>(
                            (top * (256U - fy) + bottom * fy + 32768U) >> 16U);
                }
            }
            srcOffset += static_cast<std::size_t>(sw) * sh;
            dstOffset += static_cast<std::size_t>(dw) * dh;
        }
        if (actual.i420 != expected) {
            auto diff = std::mismatch(actual.i420.begin(), actual.i420.end(),
                                      expected.begin());
            const auto offset = std::distance(actual.i420.begin(), diff.first);
            std::cerr << "I420 scaler mismatch"
                      << " source=" << size.sw << "x" << size.sh
                      << " target=" << size.dw << "x" << size.dh
                      << " byte=" << offset
                      << " actual=" << static_cast<unsigned>(*diff.first)
                      << " expected=" << static_cast<unsigned>(*diff.second)
                      << "\n";
            return false;
        }
        if (actual.width != size.dw || actual.height != size.dh ||
            actual.pts90k != input.pts90k || !actual.keyFrame ||
            !actual.hasPts) {
            std::cerr << "I420 scaler did not preserve video frame metadata\n";
            return false;
        }
    }
    std::cout << "PASS: CPU I420 scaler bit-identical across six geometries\n";
    return true;
}

int main(){
    if (!testScalerBitExact()) return 20;
    std::string error; std::vector<std::uint8_t> input;
    if(!makeInput(input,error)){std::cerr<<"input: "<<error<<"\n";return 1;}
    transcode::NativeTranscoderConfig cfg; cfg.videoCodec="h264";cfg.audioCodec="copy";cfg.width=160;cfg.height=120;cfg.fps=25;cfg.videoBitrate=250000;cfg.serviceId=1;cfg.videoPid=0x110;cfg.audioPid=0x111;
    transcode::NativeTranscoderPipeline tc;if(!tc.initialize(cfg,error)){std::cerr<<"init: "<<error<<"\n";return 2;}
    std::vector<std::uint8_t> output,chunk; const std::size_t step=7*188;
    for(std::size_t off=0;off<input.size();off+=step){if(!tc.process(input.data()+off,std::min(step,input.size()-off),chunk,error)){std::cerr<<"process: "<<error<<"\n";return 3;}output.insert(output.end(),chunk.begin(),chunk.end());}
    if(!tc.flush(chunk,error)){std::cerr<<"flush: "<<error<<" status="<<tc.status()<<"\n";return 4;}output.insert(output.end(),chunk.begin(),chunk.end());
    if(output.empty()){std::cerr<<"no transcoded TS\n";return 5;}
    mpegts::NativeTsDemux demux;
    int samples = 0, width = 0, height = 0;
    int pesSamples = 0, decodeFailures = 0;
    std::size_t largestPesBytes = 0;
    std::string lastDecodeError;
    auto dec = codec::createVideoDecoder(mpegts::ElementaryCodec::H264, error);
    if (!dec) { std::cerr << error << "\n"; return 6; }
    auto ffmpeg = codec::createFfmpegAvcDecoder(error);
    int ffmpegFrames = 0;
    std::string ffmpegDecodeError;
    demux.setSampleCallback([&](mpegts::DemuxSample&& sample) {
        if (sample.stream.kind != mpegts::ElementaryKind::Video) return;
        ++pesSamples;
        largestPesBytes = std::max(largestPesBytes, sample.data.size());
        std::vector<codec::RawVideoFrame> raw;
        std::string decodeError;
        if (!dec->decode(sample.data.data(), sample.data.size(),
                         sample.pts90k, sample.hasPts, raw, decodeError)) {
            ++decodeFailures;
            lastDecodeError = decodeError;
        } else {
            samples += static_cast<int>(raw.size());
            if (!raw.empty()) {
                width = raw.back().width;
                height = raw.back().height;
            }
        }
        if (ffmpeg) {
            std::vector<codec::RawVideoFrame> ffmpegOutput;
            std::string ffmpegError;
            if (ffmpeg->decode(sample.data.data(), sample.data.size(),
                               sample.pts90k, sample.hasPts,
                               ffmpegOutput, ffmpegError)) {
                ffmpegFrames += static_cast<int>(ffmpegOutput.size());
            } else {
                ffmpegDecodeError = ffmpegError;
            }
        }
    });
    if (!demux.push(output.data(), output.size(), error)) {
        std::cerr << error << "\n";
        return 7;
    }
    demux.flush();
    if (samples <= 0 || width != 160 || height != 120) {
        std::cerr << "bad output samples=" << samples
                  << " size=" << width << "x" << height
                  << " output_ts_bytes=" << output.size()
                  << " pes_samples=" << pesSamples
                  << " largest_pes_bytes=" << largestPesBytes
                  << " decode_failures=" << decodeFailures
                  << " last_decode_error=" << lastDecodeError
                  << " ffmpeg_frames=" << ffmpegFrames
                  << " ffmpeg_error=" << ffmpegDecodeError
                  << "\n";
        return 8;
    }
    std::cout<<"PASS: native H.264 decode/scale/encode -> MPEG-TS\n";return 0;
}
