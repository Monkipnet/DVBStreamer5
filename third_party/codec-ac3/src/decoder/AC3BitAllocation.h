#pragma once
/**
 * AC-3 core bit allocation (psychoacoustic model), A/52 7.2.
 *
 * Given the decoded exponents for a channel/coupling/LFE spectrum and the
 * bit-allocation parameters signaled in the bitstream, computes the "bit
 * allocation pointer" (bap) for every mantissa, which selects how that
 * mantissa is subsequently dequantized.
 */

#include <stdint.h>
#include "AC3Config.h"
#include "AC3Tables.h"

namespace ac3 {

/// Global bit-allocation parameters, decoded from baie (11 bits) and
/// snroffste (csnroffst, 6 bits) syntax elements.
struct AC3BitAllocInfo {
  int sdcycod = 0;
  int fdcycod = 1;
  int sgaincod = 0;
  int dbpbcod = 2;
  int floorcod = 7;
  int csnroffst = 0;
};

/// Per-channel (or coupling / LFE) bit-allocation state.
struct AC3ChannelBitAlloc {
  int fsnroffst = 0;
  int fgaincod = 0;
  int deltbae = kDeltaBitNone;
  int8_t deltba[50] = {0};
};

/// Computes bap[start..end) from exp[start..end). 'bndstart' is the index
/// into kBandTab-relative band numbering to resume from (0 for full-band
/// channels/LFE, the coupling channel's starting band for the coupling
/// channel). 'fastleakInit'/'slowleakInit' seed the leakage integrators
/// for the coupling channel (continuing from the previous call is not
/// needed here since each block recomputes coupling from cplfleak/
/// cplsleak); pass 0 for normal full-bandwidth / LFE channels.
static inline void ac3BitAllocate(const AC3BitAllocInfo& info,
                                   const AC3ChannelBitAlloc& cba, int fscod,
                                   int halfrate, int bndstart, int start,
                                   int end, int fastleakInit,
                                   int slowleakInit, const uint8_t* exp,
                                   int8_t* bap) {
  static const int kSlowGainTab[4] = {0x540, 0x4d8, 0x478, 0x410};
  static const int kDbPerBitTab[4] = {0xc00, 0x500, 0x300, 0x100};
  static const int kFloorTab[8] = {0x910, 0x950, 0x990, 0x9d0,
                                    0xa10, 0xa90, 0xb10, 0x1400};

  int fdecay = (63 + 20 * info.fdcycod) >> halfrate;
  int fgain = 128 + 128 * cba.fgaincod;
  int sdecay = (15 + 2 * info.sdcycod) >> halfrate;
  int sgain = kSlowGainTab[info.sgaincod];
  int dbknee = kDbPerBitTab[info.dbpbcod];
  const int16_t* hth = kHearingThreshold[fscod];

  const int8_t* deltba =
      (cba.deltbae == kDeltaBitNone) ? nullptr : cba.deltba;

  int floorFull = kFloorTab[info.floorcod];
  int snroffset = 960 - 64 * info.csnroffst - 4 * cba.fsnroffst + floorFull;
  int floor = floorFull >> 5;

  auto computeMask = [&](int i, int psd, int mask) -> int {
    if (psd > dbknee) mask -= (psd - dbknee) >> 2;
    if (mask > hth[i >> halfrate]) mask = hth[i >> halfrate];
    int delta = deltba ? deltba[i] : 0;
    mask -= snroffset + 128 * delta;
    mask = (mask > 0) ? 0 : ((-mask) >> 5);
    mask -= floor;
    return mask;
  };

  auto bapOf = [&](int mask, int e) -> int8_t {
    int idx = kBapTabZeroOffset + mask + 4 * e;
    if (idx < 0) idx = 0;
    if (idx > 304) idx = 304;
    return kBapTab[idx];
  };

  int i = bndstart;
  int j = start;
  int fastleak = fastleakInit;
  int slowleak = slowleakInit;

  if (start == 0) {  // full-bandwidth or LFE channel (not coupling)
    int lowcomp = 0;
    int psd = 0;
    j = end - 1;
    do {
      if (i < j) {
        if (exp[i + 1] == exp[i] - 2)
          lowcomp = 384;
        else if (lowcomp && (exp[i + 1] > exp[i]))
          lowcomp -= 64;
      }
      psd = 128 * exp[i];
      int mask = psd + fgain + lowcomp;
      bap[i] = bapOf(computeMask(i, psd, mask), exp[i]);
      i++;
    } while ((i < 3) || ((i < 7) && (exp[i] > exp[i - 1])));
    fastleak = psd + fgain;
    slowleak = psd + sgain;

    while (i < 7) {
      if (i < j) {
        if (exp[i + 1] == exp[i] - 2)
          lowcomp = 384;
        else if (lowcomp && (exp[i + 1] > exp[i]))
          lowcomp -= 64;
      }
      psd = 128 * exp[i];
      fastleak += fdecay;
      if (fastleak > psd + fgain) fastleak = psd + fgain;
      slowleak += sdecay;
      if (slowleak > psd + sgain) slowleak = psd + sgain;
      int mask = (fastleak + lowcomp < slowleak) ? fastleak + lowcomp : slowleak;
      bap[i] = bapOf(computeMask(i, psd, mask), exp[i]);
      i++;
    }

    if (end == 7) return;  // LFE channel: done

    do {
      if (exp[i + 1] == exp[i] - 2)
        lowcomp = 320;
      else if (lowcomp && (exp[i + 1] > exp[i]))
        lowcomp -= 64;
      psd = 128 * exp[i];
      fastleak += fdecay;
      if (fastleak > psd + fgain) fastleak = psd + fgain;
      slowleak += sdecay;
      if (slowleak > psd + sgain) slowleak = psd + sgain;
      int mask = (fastleak + lowcomp < slowleak) ? fastleak + lowcomp : slowleak;
      bap[i] = bapOf(computeMask(i, psd, mask), exp[i]);
      i++;
    } while (i < 20);

    while (lowcomp > 128) {  // at most two iterations
      lowcomp -= 128;
      psd = 128 * exp[i];
      fastleak += fdecay;
      if (fastleak > psd + fgain) fastleak = psd + fgain;
      slowleak += sdecay;
      if (slowleak > psd + sgain) slowleak = psd + sgain;
      int mask = (fastleak + lowcomp < slowleak) ? fastleak + lowcomp : slowleak;
      bap[i] = bapOf(computeMask(i, psd, mask), exp[i]);
      i++;
    }
    j = i;
  }

  do {
    int startband = j;
    int endband = (kBandTab[i - 20] < end) ? kBandTab[i - 20] : end;
    int psd = 128 * exp[j++];
    while (j < endband) {
      int next = 128 * exp[j++];
      int delta = next - psd;
      int bucket = delta >> 9;
      if (bucket <= -2) {
        psd = next;
      } else if (bucket == -1) {
        psd = next + kLogAddTab[(-delta) >> 1];
      } else if (bucket == 0) {
        psd += kLogAddTab[delta >> 1];
      }
      // bucket >= 1: psd unchanged (next is much quieter, ignored)
    }
    int fdum = 0;
    (void)fdum;
    fastleak += fdecay;
    if (fastleak > psd + fgain) fastleak = psd + fgain;
    slowleak += sdecay;
    if (slowleak > psd + sgain) slowleak = psd + sgain;
    int mask = (fastleak < slowleak) ? fastleak : slowleak;
    int computedMask = computeMask(i, psd, mask);
    i++;
    j = startband;
    do {
      bap[j] = bapOf(computedMask, exp[j]);
    } while (++j < endband);
  } while (j < end);
}

}  // namespace ac3
