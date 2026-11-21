#pragma once
#include "media/TransportStream.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iterator>
#include <utility>

namespace dvbstreamer5::media::mpegts {

// Translate the selected programme's source-PCR intervals into the exact
// packet release timeline. Packet bursts/arrival speed never enter the clock.
// No PCR/PTS/DTS/CC rewrite and no null packet insertion.
class PcrVbrSourceClock {
public:
    static constexpr std::uint64_t kHz=27000000ULL;
    static constexpr std::uint64_t kWrap=(1ULL<<33U)*300ULL;
    struct Anchor {std::uint64_t index=0;std::int64_t ns=0;std::uint64_t pcr=0;};
    bool observe(const Packet& packet,std::uint64_t index,
                 std::chrono::steady_clock::time_point observedAt=
                     std::chrono::steady_clock::now()) {
        if(packet[0]!=0x47 || (packet[3]&0x20)==0 ||
           packet[4]<7 || (packet[5]&0x10)==0)return false;
        const std::uint16_t pid=static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(packet[1]&0x1f)<<8)|packet[2]);
        if(!havePid_){havePid_=true;referencePid_=pid;}
        if(pid!=referencePid_)return false;
        const std::uint64_t base=
            (static_cast<std::uint64_t>(packet[6])<<25)|
            (static_cast<std::uint64_t>(packet[7])<<17)|
            (static_cast<std::uint64_t>(packet[8])<<9)|
            (static_cast<std::uint64_t>(packet[9])<<1)|
            (static_cast<std::uint64_t>(packet[10])>>7);
        const std::uint64_t ext=((packet[10]&1U)<<8)|packet[11];
        const std::uint64_t pcr=base*300ULL+ext;
        const bool discontinuity=(packet[5]&0x80U)!=0;
        if(anchors_.empty()){
            anchors_.push_back({index,0,pcr});
            lastObservedAt_=observedAt;
            return false;
        }
        const auto last=anchors_.back();
        if(index<=last.index)return false;
        const auto ticks=(pcr+kWrap-last.pcr)%kWrap;
        // Real discontinuities still reset the clock, but sub-ms PCR
        // steps are valid on clean high-frequency PCR feeds (T49/T17).
        // Never discard the queued transport packets for a short step:
        // ignore only this timing anchor and retain the previous timeline.
        // A small backward PCR jitter is not a source-clock discontinuity.
        // Ignore only this anchor, preserving queued packets and pacing.
        // Genuine discontinuity flags and large jumps still force reset.
        if(!discontinuity && pcr<last.pcr &&
           last.pcr-pcr<kHz/1000ULL){
            ++shortStepsIgnored_;
            return false;
        }
        if(discontinuity || ticks>kHz*2ULL){
            // Diagnostic-only V605: distinguish a large source-PCR jump
            // over a few transport packets from an interval with few PCRs.
            // Never modify the source PCR, media packets or reset policy.
            lastResetPreviousPcr_=last.pcr;
            lastResetCurrentPcr_=pcr;
            lastResetPreviousIndex_=last.index;
            lastResetCurrentIndex_=index;
            lastResetPacketGap_=index-last.index;
            // Elapsed monotonic time between accepted PCR observations.
            // A 9-second PCR jump in 9 seconds suggests input starvation;
            // the same jump in milliseconds suggests a timestamp fault.
            const auto wallGapMs=std::chrono::duration_cast<
                std::chrono::milliseconds>(observedAt-lastObservedAt_).count();
            lastResetWallGapMs_=wallGapMs>0 ? wallGapMs : 0;
            lastResetCause_=discontinuity ? "discontinuity_flag" :
                (pcr<last.pcr ? "backward_jump" : "forward_jump");
            lastResetDeltaMs_=pcr<last.pcr ?
                -static_cast<std::int64_t>(
                    static_cast<unsigned __int128>(last.pcr-pcr)*1000ULL/kHz) :
                static_cast<std::int64_t>(
                    static_cast<unsigned __int128>(ticks)*1000ULL/kHz);
            // Keep the prior clock epoch available to the UDP sender so an
            // unflagged forward-PCR leap need not erase valid queued TS.
            previousAnchors_=std::move(anchors_);
            anchors_.clear();
            anchors_.push_back({index,0,pcr});
            lastObservedAt_=observedAt;
            ++resets_;
            return true;
        }
        if(ticks<kHz/1000ULL){
            ++shortStepsIgnored_;
            return false;
        }
        const auto deltaNs=static_cast<std::int64_t>(
            static_cast<unsigned __int128>(ticks)*1000000000ULL/kHz);
        anchors_.push_back({index,last.ns+deltaNs,pcr});
        lastObservedAt_=observedAt;
        while(anchors_.size()>512)anchors_.pop_front();
        return false;
    }
    bool ready()const noexcept{return anchors_.size()>=2;}
    std::deque<Anchor> takePreviousAnchors() noexcept {
        return std::move(previousAnchors_);
    }
    bool timeNs(std::uint64_t index,std::int64_t& out)const noexcept{
        return timeNsForAnchors(anchors_,index,out);
    }
    static bool timeNsForAnchors(const std::deque<Anchor>& anchors,
                                 std::uint64_t index,std::int64_t& out) noexcept {
        if(anchors.size()<2)return false;
        // PCR lookups run for every UDP packet. Search only O(log N)
        // anchor positions instead of scanning up to 512 timestamps.
        // Preserve the two-anchor interpolation/extrapolation semantics.
        auto upper=std::upper_bound(
            anchors.begin()+1,anchors.end(),index,
            [](std::uint64_t packetIndex,const Anchor& anchor) {
                return packetIndex<anchor.index;
            });
        const auto rightIt = upper==anchors.end()
            ? std::prev(anchors.end()) : upper;
        const auto leftIt = std::prev(rightIt);
        const Anchor* right = &*rightIt;
        const Anchor* left = &*leftIt;
        const auto deltaIndex=right->index-left->index;
        if(!deltaIndex)return false;
        const auto diffIndex=static_cast<__int128>(index)-
                              static_cast<__int128>(left->index);
        const auto diffNs=static_cast<__int128>(right->ns-left->ns);
        const auto mapped=static_cast<__int128>(left->ns)+diffIndex*diffNs/deltaIndex;
        if(mapped < -static_cast<__int128>(100000000000000LL) ||
           mapped >  static_cast<__int128>(100000000000000LL))return false;
        out=static_cast<std::int64_t>(mapped);
        return true;
    }
    std::uint64_t resets()const noexcept{return resets_;}
    const char* lastResetCause()const noexcept{return lastResetCause_;}
    std::int64_t lastResetDeltaMs()const noexcept{return lastResetDeltaMs_;}
    std::uint64_t lastResetPreviousPcr()const noexcept{return lastResetPreviousPcr_;}
    std::uint64_t lastResetCurrentPcr()const noexcept{return lastResetCurrentPcr_;}
    std::uint64_t lastResetPreviousIndex()const noexcept{return lastResetPreviousIndex_;}
    std::uint64_t lastResetCurrentIndex()const noexcept{return lastResetCurrentIndex_;}
    std::uint64_t lastResetPacketGap()const noexcept{return lastResetPacketGap_;}
    std::int64_t lastResetWallGapMs()const noexcept{return lastResetWallGapMs_;}
    std::uint64_t shortStepsIgnored()const noexcept{return shortStepsIgnored_;}
    std::uint16_t referencePid()const noexcept{return referencePid_;}
private:
    std::deque<Anchor> anchors_;
    std::deque<Anchor> previousAnchors_;
    std::uint16_t referencePid_=0x1fff;
    bool havePid_=false;
    std::uint64_t resets_=0;
    std::uint64_t shortStepsIgnored_=0;
    const char* lastResetCause_="none";
    std::int64_t lastResetDeltaMs_=0;
    std::uint64_t lastResetPreviousPcr_=0,lastResetCurrentPcr_=0;
    std::uint64_t lastResetPreviousIndex_=0,lastResetCurrentIndex_=0;
    std::uint64_t lastResetPacketGap_=0;
    std::int64_t lastResetWallGapMs_=0;
    std::chrono::steady_clock::time_point lastObservedAt_{};
};

} // namespace dvbstreamer5::media::mpegts
