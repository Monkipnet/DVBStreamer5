#include "media/UdpTsMux.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace dvbstreamer5::media::network {
namespace {
constexpr std::uint64_t kHz = 27000000ULL;
constexpr std::uint64_t kWrap = (1ULL<<33U)*300ULL;
std::uint64_t startup(std::uint64_t encoded) {
    using mpegts::CbrTsPacer;
    const auto rate = CbrTsPacer::decodeTargetBitrate(encoded);
    if (!rate) return 0;
    const auto requested = CbrTsPacer::startupBufferMilliseconds(encoded)*1000000ULL;
    const auto cap = UdpTsMux::kMaxBytes*8ULL*700000000ULL/rate;
    return std::min<std::uint64_t>(requested,cap);
}
}

UdpTsMux::UdpTsMux(bool cbr, std::uint64_t encoded)
    : cbr_(cbr), sourcePcr_(mpegts::CbrTsPacer::sourcePllEncodedTargetBitrate(encoded)),
      bitrate_(mpegts::CbrTsPacer::decodeTargetBitrate(encoded)),
      startupNs_(cbr ? startup(encoded) : 0) {
    if (cbr && (bitrate_<100000 || bitrate_>200000000))
        throw std::invalid_argument("UDP-CBR bitrate must be 100..200000 kbit/s");
}

bool UdpTsMux::pcr(const Packet& p, std::uint64_t& ticks, bool& discontinuity) {
    if (!(p[3]&0x20U) || p[4]<7 || p[4]>183 || !(p[5]&0x10U)) return false;
    discontinuity=(p[5]&0x80U)!=0;
    const auto base=(std::uint64_t(p[6])<<25U)|(std::uint64_t(p[7])<<17U)|
        (std::uint64_t(p[8])<<9U)|(std::uint64_t(p[9])<<1U)|(p[10]>>7U);
    const auto extension=((p[10]&1U)<<8U)|p[11];
    if (extension>=300) return false;
    ticks=base*300ULL+extension;
    return true;
}

bool UdpTsMux::push(const std::vector<Packet>& packets, std::uint64_t arrivalNs) {
    pushError_=PushError::None;
    std::size_t count=0;
    for (const auto& p:packets) {
        if (p[0]!=0x47 || (p[3]&0x30U)==0) { pushError_=PushError::InvalidPacket; return false; }
        if (!cbr_ || pid(p)!=0x1fffU) ++count;
    }
    if (count>(kMaxBytes/188U)-queued_) { pushError_=PushError::QueueFull; return false; }
    if (!count) return true;
    Block block{{},0,arrivalNs}; block.packets.reserve(count);
    for (const auto& p:packets) {
        if (cbr_ && pid(p)==0x1fffU) continue;
        if (cbr_) {
            std::uint64_t ticks; bool reset;
            if (pcr(p,ticks,reset)) {
                if (referencePid_==8192) referencePid_=pid(p);
                if (referencePid_==pid(p)) schedule_.observe(accepted_,ticks,reset);
            }
        }
        block.packets.push_back(p); ++accepted_;
    }
    if (!started_) { started_=true; firstArrival_=arrivalNs; }
    queued_+=count; blocks_.push_back(std::move(block));
    return true;
}

bool UdpTsMux::due() {
    if (schedule_.ready()) return schedule_.due(emitted_,slots_,bitrate_);
    // Without PCR preserve input timing, rather than drain the reservoir at
    // full CBR or invent a 75%-of-target useful media rate.
    const auto elapsed=blocks_.front().arrival-firstArrival_;
    return static_cast<unsigned __int128>(elapsed)*bitrate_ <=
        static_cast<unsigned __int128>(slots_)*188ULL*8ULL*1000000000ULL;
}

void UdpTsMux::restamp(Packet& p) {
    std::uint64_t ticks; bool reset;
    if (sourcePcr_ || !pcr(p,ticks,reset)) return;
    const auto key=pid(p);
    auto it=pcrAnchors_.find(key);
    if (reset || it==pcrAnchors_.end()) {
        pcrAnchors_[key]={ticks,slots_}; return;
    }
    ticks=(it->second.ticks+static_cast<std::uint64_t>(
        static_cast<unsigned __int128>(slots_-it->second.slot)*188ULL*8ULL*kHz/bitrate_))%kWrap;
    const auto base=ticks/300ULL, ext=ticks%300ULL;
    p[6]=base>>25U; p[7]=base>>17U; p[8]=base>>9U; p[9]=base>>1U;
    p[10]=((base&1U)<<7U)|0x7eU|(ext>>8U); p[11]=ext;
}

bool UdpTsMux::emit(std::uint64_t nowNs, Datagram& out, bool finish) {
    out.size=out.useful=0;
    if (!started_) return false;
    if (!cbr_ && (!queued_ || (queued_<7 && !finish &&
            nowNs-blocks_.front().arrival<2000000ULL))) return false;
    const auto count=cbr_ ? 7U : std::min<std::size_t>(7,queued_);
    for (std::size_t i=0;i<count;++i) {
        Packet p;
        if (queued_ && (!cbr_ || due())) {
            auto& block=blocks_.front(); p=block.packets[block.offset++];
            if (block.offset==block.packets.size()) blocks_.pop_front();
            --queued_; ++emitted_; ++out.useful;
            if (cbr_) restamp(p);
        } else {
            p.fill(0xff); p[0]=0x47; p[1]=0x1f; p[2]=0xff;
            p[3]=0x10U|(nullCc_++&15U);
        }
        std::memcpy(out.bytes.data()+out.size,p.data(),188);
        out.size+=188; ++slots_;
    }
    return true;
}

} // namespace dvbstreamer5::media::network
