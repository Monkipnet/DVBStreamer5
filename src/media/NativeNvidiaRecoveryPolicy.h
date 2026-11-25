#pragma once
#include <cstdint>
#include <tuple>
namespace dvbstreamer5::media::codec {
// Only compare fields that determine the lifetime/geometry of a CUVID decoder.
struct NvidiaSequenceKey {
    unsigned codec=0, chroma=0, lumaDepth=0, chromaDepth=0;
    unsigned width=0, height=0, progressive=0, surfaces=0;
    int left=0, top=0, right=0, bottom=0;
    bool operator==(const NvidiaSequenceKey& other) const noexcept {
        return std::tie(codec,chroma,lumaDepth,chromaDepth,width,height,progressive,surfaces,left,top,right,bottom) ==
               std::tie(other.codec,other.chroma,other.lumaDepth,other.chromaDepth,other.width,other.height,other.progressive,other.surfaces,other.left,other.top,other.right,other.bottom);
    }
};
enum class NvidiaSequenceAction { Create, Reuse, RejectChange };
inline NvidiaSequenceAction nvidiaSequenceAction(bool active,const NvidiaSequenceKey& current,const NvidiaSequenceKey& next) noexcept {
    if(!active) return NvidiaSequenceAction::Create;
    return current==next ? NvidiaSequenceAction::Reuse : NvidiaSequenceAction::RejectChange;
}
enum class VideoRecoveryAction { Continue, DropUntilRandomAccess, Recover };
inline VideoRecoveryAction videoRecoveryAction(bool pending,bool randomAccess) noexcept {
    if(!pending) return VideoRecoveryAction::Continue;
    return randomAccess ? VideoRecoveryAction::Recover : VideoRecoveryAction::DropUntilRandomAccess;
}
}
