#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
namespace dvbstreamer5::media::codec {
inline unsigned nvencPresetRank(std::string_view name) noexcept {
    if(name=="fastest" || name=="p1") return 1;
    if(name=="fast" || name=="p2") return 2;
    if(name=="p3") return 3;
    if(name=="p4") return 4;
    if(name=="quality" || name=="p6") return 6;
    if(name=="best" || name=="p7") return 7;
    return 5;
}
inline unsigned nvencPresetCap(const char* setting) noexcept {
    if(!setting) return 0;
    const std::string_view value(setting);
    if(value.size()!=2 || value[0]!='p' || value[1]<'1' || value[1]>'7') return 0;
    return static_cast<unsigned>(value[1]-'0');
}
inline unsigned effectiveNvencPreset(std::string_view requested,const char* cap,bool qualityOptIn,bool wisi) noexcept {
    const auto rank=nvencPresetRank(requested);
    const auto limit=nvencPresetCap(cap);
    // Explicit quality experiment and WISI profile retain precedence.
    return limit && !qualityOptIn && !wisi ? std::min(rank,limit) : rank;
}
struct NvencStageStats {
    std::uint64_t count=0,totalUs=0,maxUs=0;
    void add(std::uint64_t us) noexcept {++count;totalUs+=us;maxUs=std::max(maxUs,us);}
    std::uint64_t average() const noexcept {return count?totalUs/count:0;}
    void clear() noexcept {*this={};}
};
// 33-bit PTS clock: skip an arbitrary gap in constant time instead of
// iterating once per nominal output frame. Call only for an accepted PTS.
inline std::uint64_t advanceNvencPts(std::uint64_t next,std::uint64_t pts,std::uint64_t interval) noexcept {
    constexpr std::uint64_t mask=(1ULL<<33U)-1ULL;
    interval=std::clamp<std::uint64_t>(interval,1,mask);
    const auto delta=(pts-next)&mask;
    return (next+(delta/interval+1)*interval)&mask;
}
}
