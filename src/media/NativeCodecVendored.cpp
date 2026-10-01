#include "media/NativeCodecRuntime.h"

#include <wels/codec_api.h>
#include <libde265/de265.h>
#include <kvazaar.h>
#include <fdk-aac/aacdecoder_lib.h>
#include <fdk-aac/aacenc_lib.h>
#define PL_MPEG_IMPLEMENTATION
#define PLM_NO_STDIO
#include <pl_mpeg/pl_mpeg.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <climits>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>

namespace dvbstreamer5::media::codec {
namespace {

struct H264AnnexBInfo {
    bool annexB = false;
    bool hasSps = false;
    bool hasPps = false;
    bool hasIdr = false;
    std::vector<std::uint8_t> sps;
    std::vector<std::uint8_t> pps;
};

std::size_t findAnnexBStartCode(const std::uint8_t* data, std::size_t size,
                                std::size_t from, std::size_t& length) {
    length = 0;
    if (!data || from >= size) return size;
    for (std::size_t i = from; i + 3 <= size; ++i) {
        if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 &&
            data[i + 2] == 0 && data[i + 3] == 1) {
            length = 4;
            return i;
        }
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            length = 3;
            return i;
        }
    }
    return size;
}

std::vector<std::uint8_t> copyAnnexBNal(const std::uint8_t* data,
                                        std::size_t begin,
                                        std::size_t end) {
    while (end > begin && data[end - 1] == 0) --end;
    if (end <= begin) return {};
    std::vector<std::uint8_t> result = {0, 0, 0, 1};
    result.insert(result.end(), data + begin, data + end);
    return result;
}

H264AnnexBInfo inspectH264AnnexB(const std::uint8_t* data, std::size_t size) {
    H264AnnexBInfo info;
    std::size_t startCodeLength = 0;
    std::size_t start = findAnnexBStartCode(data, size, 0, startCodeLength);
    while (start < size) {
        info.annexB = true;
        const std::size_t nalBegin = start + startCodeLength;
        if (nalBegin >= size) break;
        std::size_t nextStartCodeLength = 0;
        const std::size_t next = findAnnexBStartCode(
            data, size, nalBegin, nextStartCodeLength);
        const std::size_t nalEnd = next < size ? next : size;
        const std::uint8_t type = static_cast<std::uint8_t>(data[nalBegin] & 0x1fU);
        if (type == 7) {
            info.hasSps = true;
            info.sps = copyAnnexBNal(data, nalBegin, nalEnd);
        } else if (type == 8) {
            info.hasPps = true;
            info.pps = copyAnnexBNal(data, nalBegin, nalEnd);
        } else if (type == 5) {
            info.hasIdr = true;
        }
        if (next >= size) break;
        start = next;
        startCodeLength = nextStartCodeLength;
    }
    return info;
}

class OpenH264Decoder final : public VideoDecoder {
public:
    OpenH264Decoder() {
        if (WelsCreateDecoder(&decoder_) != 0 || !decoder_) return;
        if (!initializeDecoder()) {
            WelsDestroyDecoder(decoder_);
            decoder_ = nullptr;
        }
    }
    ~OpenH264Decoder() override {
        if (decoder_) {
            decoder_->Uninitialize();
            WelsDestroyDecoder(decoder_);
        }
    }
    bool valid() const noexcept { return decoder_ != nullptr; }

    bool decode(const std::uint8_t* data, std::size_t size,
                std::uint64_t pts90k, bool hasPts,
                std::vector<RawVideoFrame>& output,
                std::string& error) override {
        error.clear();
        if (!decoder_) {
            error = "OpenH264 decoder unavailable";
            return false;
        }
        if (!data || !size) return true;
        if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            error = "H.264 access unit too large";
            return false;
        }

        const H264AnnexBInfo nals = inspectH264AnnexB(data, size);
        if (nals.hasSps && !nals.sps.empty()) sps_ = nals.sps;
        if (nals.hasPps && !nals.pps.empty()) pps_ = nals.pps;

        const std::uint8_t* decodeData = data;
        std::size_t decodeSize = size;
        std::vector<std::uint8_t> primedAccessUnit;

        if (nals.annexB && !synchronized_) {
            // Joining a live transport in the middle of a GOP is normal. Do
            // not feed arbitrary P/B slices to OpenH264 before parameter sets
            // and a random-access picture are available.
            if (sps_.empty() || pps_.empty() || !nals.hasIdr) {
                return true;
            }
            if (!nals.hasSps) {
                primedAccessUnit.insert(
                    primedAccessUnit.end(), sps_.begin(), sps_.end());
            }
            if (!nals.hasPps) {
                primedAccessUnit.insert(
                    primedAccessUnit.end(), pps_.begin(), pps_.end());
            }
            primedAccessUnit.insert(
                primedAccessUnit.end(), data, data + size);
            decodeData = primedAccessUnit.data();
            decodeSize = primedAccessUnit.size();
        }

        unsigned char* planes[3]{};
        SBufferInfo info{};
        info.uiInBsTimeStamp = hasPts ? pts90k : 0;
        const DECODING_STATE rc = decoder_->DecodeFrameNoDelay(
            decodeData, static_cast<int>(decodeSize), planes, &info);

        const int decodeState = static_cast<int>(rc);
        const int fatalMask =
            static_cast<int>(dsInvalidArgument) |
            static_cast<int>(dsInitialOptExpected) |
            static_cast<int>(dsOutOfMemory) |
            static_cast<int>(dsDstBufNeedExpan);
        if ((decodeState & fatalMask) != 0) {
            error = "OpenH264 decode failed: " + std::to_string(decodeState);
            return false;
        }

        // The low OpenH264 status bits describe recoverable bitstream/live
        // transport conditions. In particular dsNoParamSets == 16 is common
        // when attaching in the middle of a GOP. A reconnect can also cause
        // reference/bitstream-loss flags until the next clean IDR. Do not
        // take the whole service OFFLINE for those states; force a clean
        // SPS/PPS + IDR resynchronization instead.
        const int resyncMask =
            static_cast<int>(dsNoParamSets) |
            static_cast<int>(dsRefLost) |
            static_cast<int>(dsBitstreamError) |
            static_cast<int>(dsDepLayerLost) |
            static_cast<int>(dsRefListNullPtrs);
        if ((decodeState & resyncMask) != 0) synchronized_ = false;

        if (nals.annexB && nals.hasIdr && !sps_.empty() && !pps_.empty() &&
            (decodeState & static_cast<int>(dsNoParamSets)) == 0) {
            synchronized_ = true;
        }
        if (info.iBufferStatus != 1) return true;

        const int w = info.UsrData.sSystemBuffer.iWidth;
        const int h = info.UsrData.sSystemBuffer.iHeight;
        if (w <= 0 || h <= 0 || !planes[0] || !planes[1] || !planes[2]) {
            error = "OpenH264 returned invalid I420";
            return false;
        }
        RawVideoFrame frame;
        frame.width = w;
        frame.height = h;
        frame.pts90k = pts90k;
        frame.dts90k = pts90k;
        frame.hasPts = hasPts;
        frame.hasDts = hasPts;
        frame.i420.resize(static_cast<std::size_t>(w) * h * 3U / 2U);
        auto* y = frame.i420.data();
        auto* u = y + static_cast<std::size_t>(w) * h;
        auto* v = u + static_cast<std::size_t>(w / 2) * (h / 2);
        for (int row = 0; row < h; ++row) {
            std::memcpy(
                y + static_cast<std::size_t>(row) * w,
                planes[0] + static_cast<std::size_t>(row) *
                    info.UsrData.sSystemBuffer.iStride[0],
                w);
        }
        for (int row = 0; row < h / 2; ++row) {
            std::memcpy(
                u + static_cast<std::size_t>(row) * (w / 2),
                planes[1] + static_cast<std::size_t>(row) *
                    info.UsrData.sSystemBuffer.iStride[1],
                w / 2);
            std::memcpy(
                v + static_cast<std::size_t>(row) * (w / 2),
                planes[2] + static_cast<std::size_t>(row) *
                    info.UsrData.sSystemBuffer.iStride[1],
                w / 2);
        }
        output.push_back(std::move(frame));
        return true;
    }

    void reset() override {
        sps_.clear();
        pps_.clear();
        synchronized_ = false;
        if (decoder_) {
            decoder_->Uninitialize();
            initializeDecoder();
        }
    }

private:
    bool initializeDecoder() {
        if (!decoder_) return false;
        SDecodingParam params{};
        params.eEcActiveIdc = ERROR_CON_DISABLE;
        params.bParseOnly = false;
        params.sVideoProperty.size = sizeof(params.sVideoProperty);
        params.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
        return decoder_->Initialize(&params) == 0;
    }

    ISVCDecoder* decoder_ = nullptr;
    std::vector<std::uint8_t> sps_;
    std::vector<std::uint8_t> pps_;
    bool synchronized_ = false;
};

class OpenH264Encoder final : public VideoEncoder {
public:
    OpenH264Encoder(){ WelsCreateSVCEncoder(&encoder_); }
    ~OpenH264Encoder() override { close(); if(encoder_){WelsDestroySVCEncoder(encoder_);encoder_=nullptr;} }
    bool valid() const noexcept { return encoder_ != nullptr; }
    bool configure(int w,int h,double fps,std::uint64_t bitrate,std::string&error) override {
        error.clear(); if(!encoder_){error="OpenH264 encoder unavailable";return false;} close();
        SEncParamBase p{};p.iUsageType=CAMERA_VIDEO_REAL_TIME;p.iPicWidth=w;p.iPicHeight=h;p.iTargetBitrate=static_cast<int>(std::min<std::uint64_t>(bitrate,INT_MAX));p.iRCMode=RC_BITRATE_MODE;p.fMaxFrameRate=static_cast<float>(fps>0?fps:25.0);
        if(encoder_->Initialize(&p)!=cmResultSuccess){error="OpenH264 Initialize failed";return false;} int fmt=videoFormatI420;encoder_->SetOption(ENCODER_OPTION_DATAFORMAT,&fmt);w_=w;h_=h;fps_=fps>0?fps:25.0;configured_=true;return true;
    }
    bool encode(const RawVideoFrame&in,std::vector<EncodedVideoFrame>&out,std::string&error) override {
        error.clear();if(!configured_){error="OpenH264 encoder not configured";return false;}if(in.width!=w_||in.height!=h_){error="OpenH264 geometry mismatch";return false;}
        SSourcePicture p{};p.iColorFormat=videoFormatI420;p.iPicWidth=w_;p.iPicHeight=h_;p.iStride[0]=w_;p.iStride[1]=p.iStride[2]=w_/2;p.pData[0]=const_cast<unsigned char*>(in.i420.data());p.pData[1]=p.pData[0]+static_cast<std::size_t>(w_)*h_;p.pData[2]=p.pData[1]+static_cast<std::size_t>(w_/2)*(h_/2);p.uiTimeStamp=static_cast<long long>((in.hasPts?in.pts90k:nextPts_)/90ULL);
        SFrameBSInfo bi{};const int rc=encoder_->EncodeFrame(&p,&bi);if(rc!=cmResultSuccess){error="OpenH264 EncodeFrame failed: "+std::to_string(rc);return false;}if(bi.eFrameType==videoFrameTypeSkip)return true;
        EncodedVideoFrame f;f.pts90k=in.hasPts?in.pts90k:nextPts_;f.dts90k=f.pts90k;f.hasPts=f.hasDts=true;f.keyFrame=bi.eFrameType==videoFrameTypeIDR||bi.eFrameType==videoFrameTypeI;
        for(int l=0;l<bi.iLayerNum;++l){auto&li=bi.sLayerInfo[l];std::size_t off=0;for(int n=0;n<li.iNalCount;++n){int z=li.pNalLengthInByte[n];if(z>0){f.data.insert(f.data.end(),li.pBsBuf+off,li.pBsBuf+off+z);off+=z;}}}if(!f.data.empty())out.push_back(std::move(f));nextPts_=(in.hasPts?in.pts90k:nextPts_)+static_cast<std::uint64_t>(90000.0/fps_);return true;
    }
    bool flush(std::vector<EncodedVideoFrame>&,std::string&error) override {error.clear();return true;}
private:
    void close(){if(configured_&&encoder_){encoder_->Uninitialize();configured_=false;}}
    ISVCEncoder*encoder_=nullptr;int w_=0,h_=0;double fps_=25.0;bool configured_=false;std::uint64_t nextPts_=0;
};

class De265Decoder final : public VideoDecoder {
public:
    De265Decoder(){ctx_=de265_new_decoder();}
    ~De265Decoder() override {if(ctx_)de265_free_decoder(ctx_);}
    bool valid()const noexcept{return ctx_!=nullptr;}
    bool decode(const std::uint8_t*d,std::size_t n,std::uint64_t pts,bool hasPts,std::vector<RawVideoFrame>&out,std::string&error)override{
        error.clear();if(!ctx_){error="libde265 decoder unavailable";return false;}if(!d||!n)return true;if(n>static_cast<std::size_t>(INT_MAX)){error="HEVC AU too large";return false;}
        auto e=de265_push_data(ctx_,d,static_cast<int>(n),static_cast<de265_PTS>(hasPts?pts:0),nullptr);if(e!=DE265_OK){error="libde265 push failed: "+std::to_string(e);return false;}de265_push_end_of_frame(ctx_);
        for(int guard=0;guard<128;++guard){int more=0;e=de265_decode(ctx_,&more);if(!drain(out,pts,hasPts,error))return false;if(!more)break;if(e!=DE265_OK&&e!=DE265_ERROR_IMAGE_BUFFER_FULL&&e!=DE265_ERROR_WAITING_FOR_INPUT_DATA){error="libde265 decode failed: "+std::to_string(e);return false;}}
        return drain(out,pts,hasPts,error);
    }
    void reset()override{if(ctx_)de265_reset(ctx_);}
private:
    bool drain(std::vector<RawVideoFrame>&out,std::uint64_t fallback,bool hasPts,std::string&error){while(const de265_image*img=de265_get_next_picture(ctx_)){const int w=de265_get_image_width(img,0),h=de265_get_image_height(img,0);if(de265_get_bits_per_pixel(img,0)!=8||de265_get_chroma_format(img)!=de265_chroma_420){de265_release_next_picture(ctx_);error="libde265 output is not 8-bit 4:2:0";return false;}int sy=0,su=0,sv=0;auto*y=de265_get_image_plane(img,0,&sy);auto*u=de265_get_image_plane(img,1,&su);auto*v=de265_get_image_plane(img,2,&sv);if(w<=0||h<=0||!y||!u||!v){de265_release_next_picture(ctx_);error="libde265 invalid frame";return false;}RawVideoFrame f;f.width=w;f.height=h;const auto p=de265_get_image_PTS(img);f.pts90k=p>=0?static_cast<std::uint64_t>(p):fallback;f.dts90k=f.pts90k;f.hasPts=f.hasDts=hasPts;f.i420.resize(static_cast<std::size_t>(w)*h*3U/2U);auto*dy=f.i420.data();auto*du=dy+static_cast<std::size_t>(w)*h;auto*dv=du+static_cast<std::size_t>(w/2)*(h/2);for(int r=0;r<h;++r)std::memcpy(dy+static_cast<std::size_t>(r)*w,y+static_cast<std::size_t>(r)*sy,w);for(int r=0;r<h/2;++r){std::memcpy(du+static_cast<std::size_t>(r)*(w/2),u+static_cast<std::size_t>(r)*su,w/2);std::memcpy(dv+static_cast<std::size_t>(r)*(w/2),v+static_cast<std::size_t>(r)*sv,w/2);}out.push_back(std::move(f));de265_release_next_picture(ctx_);}return true;}
    de265_decoder_context*ctx_=nullptr;
};

class KvazaarEncoder final : public VideoEncoder {
public:
    KvazaarEncoder(){api_=kvz_api_get(8);}
    ~KvazaarEncoder() override {close();}
    bool valid()const noexcept{return api_!=nullptr;}
    bool configure(int w,int h,double fps,std::uint64_t bitrate,std::string&error)override{
        error.clear();close();if(!api_){error="Kvazaar API unavailable";return false;}cfg_=api_->config_alloc();if(!cfg_||!api_->config_init(cfg_)){error="Kvazaar config init failed";close();return false;}cfg_->width=w;cfg_->height=h;cfg_->framerate_num=static_cast<int32_t>(std::max(1.0,std::round(fps>0?fps:25.0)));cfg_->framerate_denom=1;cfg_->target_bitrate=static_cast<int32_t>(std::min<std::uint64_t>(bitrate,INT_MAX));cfg_->owf=0;cfg_->threads=1;cfg_->gop_len=0;cfg_->ref_frames=1;cfg_->bipred=0;cfg_->intra_period=std::max(1,cfg_->framerate_num*2);cfg_->vps_period=1;cfg_->input_format=KVZ_FORMAT_P420;cfg_->input_bitdepth=8;cfg_->enable_logging_output=0;
        enc_=api_->encoder_open(cfg_);if(!enc_){error="Kvazaar encoder_open failed";close();return false;}w_=w;h_=h;fps_=fps>0?fps:25.0;return true;
    }
    bool encode(const RawVideoFrame&in,std::vector<EncodedVideoFrame>&out,std::string&error)override{return encodeOne(&in,out,error);}
    bool flush(std::vector<EncodedVideoFrame>&out,std::string&error)override{for(int i=0;i<64;++i){std::size_t before=out.size();if(!encodeOne(nullptr,out,error))return false;if(out.size()==before)break;}return true;}
private:
    bool encodeOne(const RawVideoFrame*in,std::vector<EncodedVideoFrame>&out,std::string&error){error.clear();if(!enc_){error="Kvazaar encoder not configured";return false;}kvz_picture*pic=nullptr;if(in){if(in->width!=w_||in->height!=h_){error="Kvazaar geometry mismatch";return false;}pic=api_->picture_alloc_csp(KVZ_CSP_420,w_,h_);if(!pic){error="Kvazaar picture alloc failed";return false;}for(int r=0;r<h_;++r)std::memcpy(pic->y+static_cast<std::size_t>(r)*pic->stride,in->i420.data()+static_cast<std::size_t>(r)*w_,w_);const auto*su=in->i420.data()+static_cast<std::size_t>(w_)*h_;const auto*sv=su+static_cast<std::size_t>(w_/2)*(h_/2);for(int r=0;r<h_/2;++r){std::memcpy(pic->u+static_cast<std::size_t>(r)*(pic->stride/2),su+static_cast<std::size_t>(r)*(w_/2),w_/2);std::memcpy(pic->v+static_cast<std::size_t>(r)*(pic->stride/2),sv+static_cast<std::size_t>(r)*(w_/2),w_/2);}pic->pts=static_cast<int64_t>(in->hasPts?in->pts90k:nextPts_);pts_.push_back(static_cast<std::uint64_t>(pic->pts));}
        kvz_data_chunk*chunks=nullptr;uint32_t len=0;kvz_picture*rec=nullptr,*src=nullptr;kvz_frame_info fi{};const int ok=api_->encoder_encode(enc_,pic,&chunks,&len,&rec,&src,&fi);if(pic)api_->picture_free(pic);if(!ok){error="Kvazaar encoder_encode failed";return false;}if(chunks&&len){EncodedVideoFrame f;for(auto*c=chunks;c;c=c->next)f.data.insert(f.data.end(),c->data,c->data+c->len);f.keyFrame=fi.nal_unit_type>=KVZ_NAL_BLA_W_LP&&fi.nal_unit_type<=KVZ_NAL_CRA_NUT;if(src){f.pts90k=src->pts>=0?static_cast<std::uint64_t>(src->pts):0;f.hasPts=true;}else if(!pts_.empty()){f.pts90k=pts_.front();f.hasPts=true;}f.dts90k=f.pts90k;f.hasDts=f.hasPts;if(!pts_.empty())pts_.pop_front();out.push_back(std::move(f));}if(chunks)api_->chunk_free(chunks);if(rec)api_->picture_free(rec);if(src)api_->picture_free(src);if(in)nextPts_=(in->hasPts?in->pts90k:nextPts_)+static_cast<std::uint64_t>(90000.0/fps_);return true;}
    void close(){if(enc_){api_->encoder_close(enc_);enc_=nullptr;}if(cfg_){api_->config_destroy(cfg_);cfg_=nullptr;}pts_.clear();}
    const kvz_api*api_=nullptr;kvz_config*cfg_=nullptr;kvz_encoder*enc_=nullptr;int w_=0,h_=0;double fps_=25.0;std::uint64_t nextPts_=0;std::deque<std::uint64_t>pts_;
};

class FdkAacDecoder final : public AudioDecoder {
public:
    FdkAacDecoder(){h_=aacDecoder_Open(TT_MP4_ADTS,1);}
    ~FdkAacDecoder()override{if(h_)aacDecoder_Close(h_);}
    bool valid()const noexcept{return h_!=nullptr;}
    bool decode(const std::uint8_t*d,std::size_t n,std::uint64_t pts,bool hasPts,std::vector<PcmAudioFrame>&out,std::string&error)override{
        error.clear();if(!h_){error="FDK-AAC decoder unavailable";return false;}if(!d||!n)return true;UCHAR*ptr=const_cast<UCHAR*>(reinterpret_cast<const UCHAR*>(d));UINT size=static_cast<UINT>(std::min<std::size_t>(n,UINT_MAX)),valid=size;if(aacDecoder_Fill(h_,&ptr,&size,&valid)!=AAC_DEC_OK){error="FDK-AAC Fill failed";return false;}
        std::uint64_t current=pts;for(int guard=0;guard<32;++guard){std::array<INT_PCM,8*2048>pcm{};AAC_DECODER_ERROR rc=aacDecoder_DecodeFrame(h_,pcm.data(),static_cast<INT>(pcm.size()),0);if(rc==AAC_DEC_NOT_ENOUGH_BITS)break;if(rc!=AAC_DEC_OK){error="FDK-AAC DecodeFrame failed: "+std::to_string(rc);return false;}CStreamInfo*si=aacDecoder_GetStreamInfo(h_);if(!si||si->sampleRate<=0||si->numChannels<=0||si->frameSize<=0){error="FDK-AAC invalid stream info";return false;}const std::size_t count=static_cast<std::size_t>(si->frameSize)*si->numChannels;PcmAudioFrame f;f.sampleRate=si->sampleRate;f.channels=si->numChannels;f.pts90k=current;f.hasPts=hasPts;f.samples.assign(pcm.begin(),pcm.begin()+static_cast<std::ptrdiff_t>(std::min(count,pcm.size())));out.push_back(std::move(f));current+=static_cast<std::uint64_t>(si->frameSize)*90000ULL/static_cast<unsigned>(si->sampleRate);}
        return true;
    }
    void reset()override{if(h_)aacDecoder_SetParam(h_,AAC_TPDEC_CLEAR_BUFFER,1);}
private:HANDLE_AACDECODER h_=nullptr;
};

class FdkAacEncoder final : public AudioEncoder {
public:
    ~FdkAacEncoder()override{close();}
    bool configure(int rate,int channels,std::uint64_t bitrate,std::string&error)override{
        error.clear();close();if(channels<1||channels>2){error="FDK-AAC Stage 9 supports mono/stereo";return false;}if(aacEncOpen(&h_,0,static_cast<UINT>(channels))!=AACENC_OK){error="FDK-AAC open failed";return false;}CHANNEL_MODE mode=channels==1?MODE_1:MODE_2;if(aacEncoder_SetParam(h_,AACENC_AOT,AOT_AAC_LC)!=AACENC_OK||aacEncoder_SetParam(h_,AACENC_SAMPLERATE,rate)!=AACENC_OK||aacEncoder_SetParam(h_,AACENC_CHANNELMODE,mode)!=AACENC_OK||aacEncoder_SetParam(h_,AACENC_BITRATE,static_cast<UINT>(bitrate))!=AACENC_OK||aacEncoder_SetParam(h_,AACENC_TRANSMUX,TT_MP4_ADTS)!=AACENC_OK||aacEncoder_SetParam(h_,AACENC_AFTERBURNER,1)!=AACENC_OK||aacEncEncode(h_,nullptr,nullptr,nullptr,nullptr)!=AACENC_OK){error="FDK-AAC configuration failed";close();return false;}AACENC_InfoStruct info{};if(aacEncInfo(h_,&info)!=AACENC_OK||info.frameLength<=0){error="FDK-AAC info failed";close();return false;}rate_=rate;channels_=channels;frameLength_=info.frameLength;return true;
    }
    bool encode(const PcmAudioFrame&in,std::vector<EncodedAudioFrame>&out,std::string&error)override{if(!h_){error="FDK-AAC encoder not configured";return false;}if(in.sampleRate!=rate_||in.channels!=channels_){error="FDK-AAC PCM format mismatch";return false;}pending_.insert(pending_.end(),in.samples.begin(),in.samples.end());if(!havePts_&&in.hasPts){nextPts_=in.pts90k;havePts_=true;}const std::size_t need=static_cast<std::size_t>(frameLength_)*channels_;while(pending_.size()>=need){if(!encodeFrame(pending_.data(),static_cast<int>(need),out,error))return false;pending_.erase(pending_.begin(),pending_.begin()+static_cast<std::ptrdiff_t>(need));}return true;}
    bool flush(std::vector<EncodedAudioFrame>&out,std::string&error)override{if(!h_)return true;const std::size_t need=static_cast<std::size_t>(frameLength_)*channels_;if(!pending_.empty()){pending_.resize(need,0);if(!encodeFrame(pending_.data(),static_cast<int>(need),out,error))return false;pending_.clear();}return true;}
private:
    bool encodeFrame(const std::int16_t*pcm,int count,std::vector<EncodedAudioFrame>&out,std::string&error){void*inPtr=const_cast<std::int16_t*>(pcm);INT inId=IN_AUDIO_DATA,inSize=count*static_cast<INT>(sizeof(INT_PCM)),inEl=sizeof(INT_PCM);AACENC_BufDesc ib{1,&inPtr,&inId,&inSize,&inEl};std::array<UCHAR,16384>bytes{};void*outPtr=bytes.data();INT outId=OUT_BITSTREAM_DATA,outSize=bytes.size(),outEl=1;AACENC_BufDesc ob{1,&outPtr,&outId,&outSize,&outEl};AACENC_InArgs ia{};ia.numInSamples=count;AACENC_OutArgs oa{};AACENC_ERROR rc=aacEncEncode(h_,&ib,&ob,&ia,&oa);if(rc!=AACENC_OK){error="FDK-AAC encode failed: "+std::to_string(rc);return false;}if(oa.numOutBytes>0){EncodedAudioFrame f;f.data.assign(bytes.begin(),bytes.begin()+oa.numOutBytes);f.pts90k=nextPts_;f.hasPts=havePts_;out.push_back(std::move(f));nextPts_+=static_cast<std::uint64_t>(frameLength_)*90000ULL/static_cast<unsigned>(rate_);}return true;}
    void close(){if(h_)aacEncClose(&h_);h_=nullptr;pending_.clear();}
    HANDLE_AACENCODER h_=nullptr;int rate_=0,channels_=0,frameLength_=1024;std::vector<std::int16_t>pending_;std::uint64_t nextPts_=0;bool havePts_=false;
};

class PlMpegMp2Decoder final : public AudioDecoder {
public:
    PlMpegMp2Decoder(){buffer_=plm_buffer_create_with_capacity(128*1024);if(buffer_)audio_=plm_audio_create_with_buffer(buffer_,0);}
    ~PlMpegMp2Decoder()override{if(audio_)plm_audio_destroy(audio_);if(buffer_)plm_buffer_destroy(buffer_);}
    bool valid()const noexcept{return buffer_&&audio_;}
    bool decode(const std::uint8_t*d,std::size_t n,std::uint64_t pts,bool hasPts,std::vector<PcmAudioFrame>&out,std::string&error)override{error.clear();if(!valid()){error="PL_MPEG MP2 decoder unavailable";return false;}plm_buffer_write(buffer_,const_cast<std::uint8_t*>(d),n);std::uint64_t current=pts;for(int guard=0;guard<64;++guard){plm_samples_t*s=plm_audio_decode(audio_);if(!s)break;const int rate=plm_audio_get_samplerate(audio_);if(rate<=0){error="PL_MPEG invalid sample rate";return false;}PcmAudioFrame f;f.sampleRate=rate;f.channels=2;f.pts90k=current;f.hasPts=hasPts;f.samples.resize(static_cast<std::size_t>(s->count)*2);for(std::size_t i=0;i<static_cast<std::size_t>(s->count)*2;++i){float x=std::clamp(s->interleaved[i],-1.0f,1.0f);f.samples[i]=static_cast<std::int16_t>(std::lrint(x*32767.0f));}out.push_back(std::move(f));current+=static_cast<std::uint64_t>(s->count)*90000ULL/static_cast<unsigned>(rate);}return true;}
    void reset()override{if(audio_)plm_audio_rewind(audio_);}
private:plm_buffer_t*buffer_=nullptr;plm_audio_t*audio_=nullptr;
};

} // namespace

RuntimeCapabilities inspectRuntimeCapabilities(){RuntimeCapabilities c;c.h264Decoder=c.h264Encoder=c.hevcDecoder=c.hevcEncoder=c.aacDecoder=c.aacEncoder=c.mpegAudioDecoder=true;c.h264Library="built-in OpenH264 2.6.0 static";c.hevcDecoderLibrary="built-in libde265 1.1.3 static";c.hevcEncoderLibrary="built-in Kvazaar 2.3.2 static";c.aacDecoderLibrary=c.aacEncoderLibrary="built-in FDK-AAC 2.0.3 static";c.mpegAudioLibrary="built-in PL_MPEG static";return c;}
std::unique_ptr<VideoDecoder> createVideoDecoder(mpegts::ElementaryCodec c,std::string&error){error.clear();if(c==mpegts::ElementaryCodec::H264){auto p=std::make_unique<OpenH264Decoder>();if(p->valid())return p;}else if(c==mpegts::ElementaryCodec::H265){auto p=std::make_unique<De265Decoder>();if(p->valid())return p;}error="built-in decoder does not support/initialize requested codec";return{};}
std::unique_ptr<VideoEncoder> createVideoEncoder(mpegts::ElementaryCodec c,std::string&error){error.clear();if(c==mpegts::ElementaryCodec::H264){auto p=std::make_unique<OpenH264Encoder>();if(p->valid())return p;}else if(c==mpegts::ElementaryCodec::H265){auto p=std::make_unique<KvazaarEncoder>();if(p->valid())return p;}error="built-in encoder does not support/initialize requested codec";return{};}
std::unique_ptr<AudioDecoder> createAudioDecoder(mpegts::ElementaryCodec c,std::string&error){error.clear();if(c==mpegts::ElementaryCodec::AacAdts){auto p=std::make_unique<FdkAacDecoder>();if(p->valid())return p;}else if(c==mpegts::ElementaryCodec::MpegAudio){auto p=std::make_unique<PlMpegMp2Decoder>();if(p->valid())return p;}error="built-in audio decoder does not support/initialize requested codec";return{};}
std::unique_ptr<AudioEncoder> createAacEncoder(std::string&error){auto p=std::make_unique<FdkAacEncoder>();error.clear();return p;}

} // namespace dvbstreamer5::media::codec
