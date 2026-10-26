#pragma once
/**
 * Top-level AC-3 (ATSC A/52) frame/block decoder engine.
 *
 * Consumes one complete AC-3 sync frame (as located by AC3FrameHeader)
 * and produces full-bandwidth-discrete floating point PCM (i.e. no
 * downmixing: the output channel layout always matches the coded
 * acmod, in the bitstream's own channel order: for the modes with a
 * center and/or surround channels the per-block channel array is
 * [L, C, R, SL, SR] trimmed to whichever of those the coded acmod
 * contains, with LFE (if present) always the last channel).
 */

#include <string.h>
#include "AC3BitAllocation.h"
#include "AC3BitReader.h"
#include "AC3Config.h"
#include "AC3Downmix.h"
#include "AC3FrameHeader.h"
#include "AC3Imdct.h"
#include "AC3Tables.h"

namespace ac3 {

/// Per full-bandwidth-channel persistent decode state (survives across
/// blocks/frames so that "reuse previous" bitstream flags work, and so
/// IMDCT overlap-add has the previous block's trailing half available).
struct AC3ChannelState {
  uint8_t exp[256] = {0};
  int8_t bap[256] = {0};
  int endmant = 0;
  AC3ChannelBitAlloc ba;
  sample_t delay[256] = {0};
};

class AC3Decoder {
 public:
  AC3Decoder() { imdct_.begin(); }

  /// Resets all persistent decode state (call once before the first
  /// frame, and again after a stream discontinuity / seek).
  void reset() {
    for (auto& c : chan_) c = AC3ChannelState();
    lfe_ = AC3ChannelState();
    cplState_ = AC3ChannelState();
    memset(cplco_, 0, sizeof(cplco_));
    chincpl_ = 0;
    cplbndstrc_ = 0;
    rematflg_ = 0;
    lfsrState_ = 1;
    haveHeader_ = false;
    mergeFallback_ = false;
    memset(mixDelay_, 0, sizeof(mixDelay_));
  }

  /// Parses the sync frame header (syncinfo+bsi) at the start of 'buf'
  /// (which must hold at least 6 bytes). Returns the total frame size in
  /// bytes (>0) on success, or 0 if this is not a valid AC-3 sync frame.
  int parseHeader(const uint8_t* buf, int available) {
    AC3FrameHeader hdr;
    if (!ac3ParseSyncInfo(buf, available, hdr)) return 0;
    if (available < hdr.frameSizeBytes) return 0;
    AC3BitReader br;
    br.init(buf, hdr.frameSizeBytes);
    if (!ac3ParseBsi(br, hdr)) return 0;
    header_ = hdr;
    bitReader_ = br;
    haveHeader_ = true;

    // Per-frame resets (A/52 semantics: delta-bit-allocation state does
    // not persist across frames; dynamic range starts at unity gain).
    cplState_.ba.deltbae = kDeltaBitNone;
    for (int i = 0; i < kAc3MaxChannels - 1; i++)
      chan_[i].ba.deltbae = kDeltaBitNone;
    dynrng_ = 1.0f;
    return hdr.frameSizeBytes;
  }

  const AC3FrameHeader& header() const { return header_; }
  int channelCount() const {
    return header_.numFbwChannels + (header_.lfeon ? 1 : 0);
  }
  int sampleRate() const { return header_.sampleRate; }

  /// Requests that decodeBlock() itself downmix to 'n' channels (1 =
  /// mono, 2 = stereo) whenever the source has more channels than that,
  /// instead of producing the full discrete channel set. Pass 0 (the
  /// default) for discrete passthrough. Mirrors AC3DecoderT's own
  /// setOutputChannels() - the facade just forwards its request here.
  ///
  /// Doing the downmix at this level (rather than by mixing already-
  /// decoded PCM, as a caller could do standalone with AC3Downmix)
  /// lets it happen *before* the IMDCT, on the spectral coefficients:
  /// since the IMDCT is linear, mixing 5 channels' worth of spectra
  /// down to 2 and running 2 IMDCTs is the same result as running 5
  /// IMDCTs and mixing the PCM, but far cheaper - the IMDCT is the
  /// dominant cost of decoding (roughly two-thirds on FPU-less
  /// targets), so this is worth up to a ~2-3x reduction in per-block
  /// IMDCT work for the common 5.1->stereo/mono case. This requires
  /// every contributing channel to use the same transform size for
  /// the block (the AC-3 "block switch" flag, blksw); real-world
  /// encoders essentially never vary it per channel, but if one ever
  /// does, decodeBlock() safely falls back to full discrete decode +
  /// post-IMDCT mixing for the rest of the stream rather than risk an
  /// incorrect merge.
  void setOutputChannels(int n) { outputChannels_ = n; }

  /// Decodes the next of the 6 audio blocks in the current frame.
  /// 'pcmOut' must point to at least channelCount() arrays of 256
  /// floats each (nominal full scale +-1.0). Without setOutputChannels()
  /// (or when the source doesn't have more channels than requested),
  /// all channelCount() of them are filled, in bitstream channel order
  /// (full-bandwidth channels first as ac3NumFbwChannels() enumerates
  /// them, LFE last if present). With an active setOutputChannels()
  /// downmix request, only the requested channel count is filled (in
  /// pcmOut[0], pcmOut[1]) - already downmixed, ready to use; the rest
  /// of pcmOut is left untouched. Returns false on a malformed/
  /// unsupported bitstream (caller should drop the frame).
  bool decodeBlock(sample_t* const* pcmOut) {
    if (!haveHeader_) return false;
    AC3BitReader& br = bitReader_;
    const int acmod = header_.acmod;
    const int nfchans = header_.numFbwChannels;
    const int fscod = header_.fscod;
    const int halfrate = header_.halfrate;

    uint8_t blksw[kAc3MaxChannels] = {0};
    uint8_t dithflag[kAc3MaxChannels] = {0};
    for (int i = 0; i < nfchans; i++) blksw[i] = br.getBits(1);
    for (int i = 0; i < nfchans; i++) dithflag[i] = br.getBits(1);

    // ---- downmix planning ---------------------------------------------------
    // outCh > 0: caller wants a downmix and the source has more channels
    // than that. doMerge: this block's spectra can be safely merged pre-
    // IMDCT (see setOutputChannels()) - requires every full-bandwidth
    // channel to use the 512-point transform this block. A channel using
    // the 256-point one (blksw) blocks the merge for this block; since
    // that can only be discovered per-block, and un-merging later would
    // need individual per-channel overlap-add state that was never kept
    // warm while merging, we fall back permanently for the rest of the
    // stream rather than risk stale state - real encoders essentially
    // never vary blksw per channel, so this is a theoretical safety net,
    // not a real-world performance cliff.
    int outCh = downmixTargetChannels();
    bool doMerge = outCh > 0 && !mergeFallback_;
    if (doMerge) {
      for (int i = 0; i < nfchans; i++) {
        if (blksw[i]) {
          doMerge = false;
          mergeFallback_ = true;
          break;
        }
      }
    }

    int chaninfo = (acmod == 0) ? 1 : 0;
    do {
      if (br.getBits(1)) {  // dynrnge
        int dynrngRaw = br.getBitsSigned(8);
        int mantissa = (dynrngRaw & 0x1f) | 0x20;  // 32..63
        int expIdx = 3 - (dynrngRaw >> 5);          // arithmetic shift, -4..3 -> 0..7
        sample_t range = (sample_t)(mantissa << 13) * kExpScaleFactor[expIdx];
        dynrng_ = range;
      }
    } while (chaninfo--);

    // ---- coupling strategy -------------------------------------------------
    if (br.getBits(1)) {  // cplstre
      chincpl_ = 0;
      if (br.getBits(1)) {  // cplinu
        for (int i = 0; i < nfchans; i++)
          chincpl_ |= br.getBits(1) << i;
        if (acmod == kModeStereo) phsflginu_ = br.getBits(1) != 0;
        int cplbegf = br.getBits(4);
        int cplendf = br.getBits(4);
        int nsub = cplendf + 3 - cplbegf;
        if (nsub < 0) return false;
        ncplbnd_ = nsub;
        cplstrtbnd_ = kCouplingBandTab[cplbegf];
        cplstrtmant_ = cplbegf * 12 + 37;
        cplendmant_ = cplendf * 12 + 73;
        cplbndstrc_ = 0;
        for (int i = 0; i < nsub - 1; i++)
          if (br.getBits(1)) {
            cplbndstrc_ |= 1u << i;
            ncplbnd_--;
          }
      }
    }

    if (chincpl_) {
      bool cplcoe = false;
      for (int i = 0; i < nfchans; i++)
        if ((chincpl_ >> i) & 1)
          if (br.getBits(1)) {  // cplcoe
            cplcoe = true;
            int mstrcplco = 3 * br.getBits(2);
            for (int j = 0; j < ncplbnd_; j++) {
              int cplcoexp = br.getBits(4);
              int cplcomant = br.getBits(4);
              int mant = (cplcoexp == 15) ? (cplcomant << 14)
                                           : ((cplcomant | 0x10) << 13);
              cplco_[i][j] = (sample_t)mant * kExpScaleFactor[cplcoexp + mstrcplco];
            }
          }
      if (acmod == kModeStereo && phsflginu_ && cplcoe)
        for (int j = 0; j < ncplbnd_; j++)
          if (br.getBits(1)) cplco_[1][j] = -cplco_[1][j];  // phsflg
    }

    if (acmod == kModeStereo && br.getBits(1)) {  // rematstr
      rematflg_ = 0;
      int end = chincpl_ ? cplstrtmant_ : 253;
      int i = 0;
      do {
        rematflg_ |= br.getBits(1) << i;
      } while (kRematrixBand[i++] < end && i < 4);
    }

    int cplexpstr = kExpReuse, lfeexpstr = kExpReuse;
    int chexpstr[kAc3MaxChannels] = {0};
    if (chincpl_) cplexpstr = br.getBits(2);
    for (int i = 0; i < nfchans; i++) chexpstr[i] = br.getBits(2);
    if (header_.lfeon) lfeexpstr = br.getBits(1);

    for (int i = 0; i < nfchans; i++) {
      if (chexpstr[i] != kExpReuse) {
        if ((chincpl_ >> i) & 1) {
          chan_[i].endmant = cplstrtmant_;
        } else {
          int chbwcod = br.getBits(6);
          if (chbwcod > 60) return false;
          chan_[i].endmant = chbwcod * 3 + 73;
        }
      }
    }

    int doBitAllocMask = 0;  // bit i = channel i; bit 5 = lfe; bit 6 = coupling
    bool doBitAllocAll = false;

    if (cplexpstr != kExpReuse) {
      doBitAllocMask |= 64;
      int ngrps = (cplendmant_ - cplstrtmant_) / (3 << (cplexpstr - 1));
      int absexp = br.getBits(4) << 1;
      if (parseExponents(br, cplexpstr, ngrps, absexp,
                          cplState_.exp + cplstrtmant_))
        return false;
    }
    for (int i = 0; i < nfchans; i++) {
      if (chexpstr[i] != kExpReuse) {
        doBitAllocMask |= 1 << i;
        int grpSize = 3 << (chexpstr[i] - 1);
        int nchgrps = (chan_[i].endmant + grpSize - 4) / grpSize;
        chan_[i].exp[0] = br.getBits(4);
        if (parseExponents(br, chexpstr[i], nchgrps, chan_[i].exp[0],
                            chan_[i].exp + 1))
          return false;
        br.skipBits(2);  // gainrng
      }
    }
    if (lfeexpstr != kExpReuse) {
      doBitAllocMask |= 32;
      lfe_.exp[0] = br.getBits(4);
      if (parseExponents(br, lfeexpstr, 2, lfe_.exp[0], lfe_.exp + 1))
        return false;
    }

    if (br.getBits(1)) {  // baie
      doBitAllocAll = true;
      uint32_t v = br.getBits(11);
      baInfo_.sdcycod = (v >> 9) & 3;
      baInfo_.fdcycod = (v >> 7) & 3;
      baInfo_.sgaincod = (v >> 5) & 3;
      baInfo_.dbpbcod = (v >> 3) & 3;
      baInfo_.floorcod = v & 7;
    }
    if (br.getBits(1)) {  // snroffste
      doBitAllocAll = true;
      baInfo_.csnroffst = br.getBits(6);
      if (chincpl_) readBai(br, cplState_.ba);
      for (int i = 0; i < nfchans; i++) readBai(br, chan_[i].ba);
      if (header_.lfeon) readBai(br, lfe_.ba);
    }
    if (chincpl_ && br.getBits(1)) {  // cplleake
      doBitAllocMask |= 64;
      cplfleak_ = 9 - br.getBits(3);
      cplsleak_ = 9 - br.getBits(3);
    }

    if (br.getBits(1)) {  // deltbaie
      doBitAllocAll = true;
      if (chincpl_) cplState_.ba.deltbae = br.getBits(2);
      for (int i = 0; i < nfchans; i++) chan_[i].ba.deltbae = br.getBits(2);
      if (chincpl_ && cplState_.ba.deltbae == kDeltaBitNew &&
          parseDeltba(br, cplState_.ba.deltba))
        return false;
      for (int i = 0; i < nfchans; i++)
        if (chan_[i].ba.deltbae == kDeltaBitNew &&
            parseDeltba(br, chan_[i].ba.deltba))
          return false;
    }

    bool doBitAlloc = doBitAllocAll || doBitAllocMask;
    if (doBitAlloc) {
      if (chincpl_ && (doBitAllocAll || (doBitAllocMask & 64)))
        ac3BitAllocate(baInfo_, cplState_.ba, fscod, halfrate, cplstrtbnd_,
                        cplstrtmant_, cplendmant_, cplfleak_ << 8,
                        cplsleak_ << 8, cplState_.exp, cplState_.bap);
      for (int i = 0; i < nfchans; i++)
        if (doBitAllocAll || (doBitAllocMask & (1 << i)))
          ac3BitAllocate(baInfo_, chan_[i].ba, fscod, halfrate, 0, 0,
                          chan_[i].endmant, 0, 0, chan_[i].exp, chan_[i].bap);
      if (header_.lfeon && (doBitAllocAll || (doBitAllocMask & 32))) {
        lfe_.ba.deltbae = kDeltaBitNone;
        ac3BitAllocate(baInfo_, lfe_.ba, fscod, halfrate, 0, 0, 7, 0, 0,
                        lfe_.exp, lfe_.bap);
      }
    }

    if (br.getBits(1)) {  // skiple
      int skipl = br.getBits(9);
      br.skipBits(8 * skipl);
    }

    // ---- mantissa / coefficient decode ------------------------------------
    Quantizer q;
    sample_t level = 2.0f * dynrng_;  // matches the IMDCT's internal scaling
    // Every getCoefficients() call this block (all channels + LFE) uses
    // this same 'level', so the exponent->scale table only needs to be
    // computed once per block, not once per call (worth ~6x fewer soft-
    // float multiplies here on FPU-less targets).
    for (int i = 0; i <= 24; i++) coeffFactor_[i] = kExpScaleFactor[i] * level;
    bool cplDone = false;

    for (int i = 0; i < nfchans; i++) {
      sample_t* buf = pcmOut[i];
      getCoefficients(br, buf, chan_[i].exp, chan_[i].bap, &q, coeffFactor_,
                       dithflag[i], chan_[i].endmant);

      bool coupled = (chincpl_ >> i) & 1;
      if (coupled && !cplDone) {
        cplDone = true;
        getCouplingCoefficients(br, nfchans, pcmOut, &q, dithflag);
      }
      // For coupled channels the coupling call above already filled
      // [cplstrtmant, cplendmant); only zero what's left after that.
      int zeroFrom = coupled ? cplendmant_ : chan_[i].endmant;
      for (int j = zeroFrom; j < 256; j++) buf[j] = 0;
    }

    if (acmod == kModeStereo) {
      int end = chan_[0].endmant < chan_[1].endmant ? chan_[0].endmant
                                                      : chan_[1].endmant;
      int band = 0, j = 13;
      uint32_t rf = rematflg_;
      while (j < end) {
        if (!(rf & 1)) {
          rf >>= 1;
          j = kRematrixBand[band++];
          continue;
        }
        rf >>= 1;
        int lim = kRematrixBand[band++];
        if (lim > end) lim = end;
        for (; j < lim; j++) {
          sample_t a = pcmOut[0][j], b = pcmOut[1][j];
          pcmOut[0][j] = a + b;
          pcmOut[1][j] = a - b;
        }
      }
    }

    sample_t* lfeBuf = lfeBuf_;  // member scratch, not a stack array
    if (header_.lfeon) {
      // Mantissa bits must always be consumed to stay in sync with the
      // bitstream, even if - as below - the LFE IMDCT/output ends up
      // skipped entirely because it's being downmixed (LFE is dropped
      // from the standard downmix, never part of outCh).
      // dynrng_ * 2.0f == level, so coeffFactor_ (computed above) applies
      // unchanged here too.
      getCoefficients(br, lfeBuf, lfe_.exp, lfe_.bap, &q, coeffFactor_, 0, 7);
      for (int j = 7; j < 256; j++) lfeBuf[j] = 0;
    }

    // ---- synthesis (IMDCT) --------------------------------------------------
    if (doMerge) {
      // Mix the nfchans spectral buffers down to outCh in place (safe:
      // each downmix formula only ever reads a given input channel's
      // sample before writing that same-indexed output, elementwise),
      // then IMDCT only the merged result - the whole point of merging
      // pre-transform instead of post-transform.
      AC3Downmix::mix(pcmOut, acmod, header_.clev, header_.slev, outCh,
                       pcmOut);
      for (int c = 0; c < outCh; c++)
        imdct_.imdct512(pcmOut[c], mixDelay_[c], 0.0f);
    } else {
      for (int i = 0; i < nfchans; i++) {
        if (blksw[i])
          imdct_.imdct256(pcmOut[i], chan_[i].delay, 0.0f);
        else
          imdct_.imdct512(pcmOut[i], chan_[i].delay, 0.0f);
      }
      if (outCh > 0) {
        // Downmix requested but this block couldn't be merged
        // pre-IMDCT (see mergeFallback_ above): mix the discrete PCM
        // just produced, in place, into the first outCh buffers -
        // exactly what AC3DecoderT used to do itself.
        AC3Downmix::mix(pcmOut, acmod, header_.clev, header_.slev, outCh,
                         pcmOut);
      }
    }
    if (header_.lfeon && outCh == 0) {
      imdct_.imdct512(lfeBuf, lfe_.delay, 0.0f);
      memcpy(pcmOut[nfchans], lfeBuf, 256 * sizeof(sample_t));
    }

    return true;
  }

 private:
  /// 0 if the current stream shouldn't be downmixed (no request, or the
  /// source doesn't have more channels than requested / an unsupported
  /// target was requested); else 1 or 2, the channel count decodeBlock()
  /// will produce. Mirrors AC3DecoderT::effectiveChannels()'s rule.
  int downmixTargetChannels() const {
    int total = channelCount();
    if (outputChannels_ <= 0 || outputChannels_ >= total) return 0;
    if (outputChannels_ == 1 || outputChannels_ == 2) return outputChannels_;
    return 0;
  }

  struct Quantizer {
    sample_t q1[2] = {0, 0};
    sample_t q2[2] = {0, 0};
    sample_t q4 = 0;
    int q1ptr = -1, q2ptr = -1, q4ptr = -1;
  };

  static int parseExponents(AC3BitReader& br, int expstr, int ngrps,
                             int exponent, uint8_t* dest) {
    while (ngrps--) {
      int code = br.getBits(7);
      exponent += kExpDelta1[code];
      if (exponent > 24) return 1;
      if (expstr == kExpD45) *dest++ = exponent, *dest++ = exponent;
      if (expstr >= kExpD25) *dest++ = exponent;
      *dest++ = exponent;

      exponent += kExpDelta2[code];
      if (exponent > 24) return 1;
      if (expstr == kExpD45) *dest++ = exponent, *dest++ = exponent;
      if (expstr >= kExpD25) *dest++ = exponent;
      *dest++ = exponent;

      exponent += kExpDelta3[code];
      if (exponent > 24) return 1;
      if (expstr == kExpD45) *dest++ = exponent, *dest++ = exponent;
      if (expstr >= kExpD25) *dest++ = exponent;
      *dest++ = exponent;
    }
    return 0;
  }

  static int parseDeltba(AC3BitReader& br, int8_t* deltba) {
    memset(deltba, 0, 50);
    int nseg = br.getBits(3);
    int j = 0;
    do {
      j += br.getBits(5);
      int len = br.getBits(4);
      int delta = br.getBits(3);
      delta -= (delta >= 4) ? 3 : 4;
      if (!len) continue;
      if (j + len >= 50) return 1;
      while (len--) deltba[j++] = (int8_t)delta;
    } while (nseg--);
    return 0;
  }

  void readBai(AC3BitReader& br, AC3ChannelBitAlloc& ba) {
    uint32_t v = br.getBits(7);
    ba.fsnroffst = (v >> 3) & 0xF;
    ba.fgaincod = v & 7;
  }

  int16_t ditherGen() {
    int16_t nstate =
        (int16_t)(kDitherLut[lfsrState_ >> 8] ^ (uint16_t)(lfsrState_ << 8));
    lfsrState_ = (uint16_t)nstate;
    return nstate;
  }

  /// Dequantizes 'end' mantissas for one full-bandwidth/LFE channel into
  /// 'coeff' (A/52 7.4.4). 'factor' is the caller's precomputed 25-entry
  /// exponent->scale table (kExpScaleFactor[e] * level for e in 0..24).
  void getCoefficients(AC3BitReader& br, sample_t* coeff, const uint8_t* exp,
                        const int8_t* bap, Quantizer* q, const sample_t* factor,
                        int dither, int end) {
    if (end > 256) end = 256;
    if (end < 0) end = 0;

    for (int i = 0; i < end; i++) {
      int bapi = bap[i];
      int e = exp[i];
      if (e > 24) e = 24;  // defensive: guard against a malformed stream
      switch (bapi) {
        case 0:
          coeff[i] = dither ? ditherGen() * kLevel3Db * factor[e] : 0;
          break;
        case -1:
          if (q->q1ptr >= 0) {
            coeff[i] = q->q1[q->q1ptr--] * factor[e];
          } else {
            int code = br.getBits(5);
            q->q1ptr = 1;
            q->q1[0] = kQuant1_2[code];
            q->q1[1] = kQuant1_1[code];
            coeff[i] = kQuant1_0[code] * factor[e];
          }
          break;
        case -2:
          if (q->q2ptr >= 0) {
            coeff[i] = q->q2[q->q2ptr--] * factor[e];
          } else {
            int code = br.getBits(7);
            q->q2ptr = 1;
            q->q2[0] = kQuant2_2[code];
            q->q2[1] = kQuant2_1[code];
            coeff[i] = kQuant2_0[code] * factor[e];
          }
          break;
        case 3:
          coeff[i] = kQuant3[br.getBits(3)] * factor[e];
          break;
        case -3:
          if (q->q4ptr == 0) {
            q->q4ptr = -1;
            coeff[i] = q->q4 * factor[e];
          } else {
            int code = br.getBits(7);
            q->q4ptr = 0;
            q->q4 = kQuant4_1[code];
            coeff[i] = kQuant4_0[code] * factor[e];
          }
          break;
        case 4:
          coeff[i] = kQuant5[br.getBits(4)] * factor[e];
          break;
        default: {
          // Pre-shift in the integer domain (cheap, especially on FPU-less
          // targets) instead of an extra float multiply by a power of two.
          uint32_t v = (uint32_t)br.getBitsSigned(bapi);
          v <<= (16 - bapi);
          coeff[i] = (sample_t)(int32_t)v * factor[e];
        }
      }
    }
  }

  /// Distributes the coupling-channel mantissas to every channel that
  /// participates in coupling for this block (A/52 7.6).
  void getCouplingCoefficients(AC3BitReader& br, int nfchans,
                                sample_t* const* pcmOut, Quantizer* q,
                                const uint8_t* dithflag) {
    const uint8_t* exp = cplState_.exp;
    const int8_t* bap = cplState_.bap;
    int bnd = 0;
    uint32_t bndstrc = cplbndstrc_;
    int cplendmant = cplendmant_;
    if (cplendmant > 256) cplendmant = 256;
    int i = cplstrtmant_;
    if (i < 0) i = 0;
    while (i < cplendmant) {
      if (bnd >= 18) break;  // defensive: guard against a malformed stream
      int iEnd = i + 12;
      while (bndstrc & 1) {
        bndstrc >>= 1;
        iEnd += 12;
      }
      bndstrc >>= 1;
      if (iEnd > cplendmant) iEnd = cplendmant;
      sample_t cplco[kAc3MaxChannels];
      for (int ch = 0; ch < nfchans; ch++) cplco[ch] = cplco_[ch][bnd];
      bnd++;

      while (i < iEnd) {
        sample_t cplcoeff;
        int bapi = bap[i];
        int e = exp[i];
        if (e > 24) e = 24;  // defensive: guard against a malformed stream
        switch (bapi) {
          case 0:
            cplcoeff = kLevel3Db * kExpScaleFactor[e];
            for (int ch = 0; ch < nfchans; ch++)
              if ((chincpl_ >> ch) & 1) {
                if (dithflag[ch])
                  pcmOut[ch][i] = (2.0f * dynrng_) * cplco[ch] * cplcoeff *
                                   (sample_t)ditherGen();
                else
                  pcmOut[ch][i] = 0;
              }
            i++;
            continue;
          case -1:
            if (q->q1ptr >= 0) {
              cplcoeff = q->q1[q->q1ptr--];
            } else {
              int code = br.getBits(5);
              q->q1ptr = 1;
              q->q1[0] = kQuant1_2[code];
              q->q1[1] = kQuant1_1[code];
              cplcoeff = kQuant1_0[code];
            }
            break;
          case -2:
            if (q->q2ptr >= 0) {
              cplcoeff = q->q2[q->q2ptr--];
            } else {
              int code = br.getBits(7);
              q->q2ptr = 1;
              q->q2[0] = kQuant2_2[code];
              q->q2[1] = kQuant2_1[code];
              cplcoeff = kQuant2_0[code];
            }
            break;
          case 3:
            cplcoeff = kQuant3[br.getBits(3)];
            break;
          case -3:
            if (q->q4ptr == 0) {
              q->q4ptr = -1;
              cplcoeff = q->q4;
            } else {
              int code = br.getBits(7);
              q->q4ptr = 0;
              q->q4 = kQuant4_1[code];
              cplcoeff = kQuant4_0[code];
            }
            break;
          case 4:
            cplcoeff = kQuant5[br.getBits(4)];
            break;
          default: {
            // Pre-shift in the integer domain instead of an extra float
            // multiply by a power of two (see getCoefficients()).
            uint32_t v = (uint32_t)br.getBitsSigned(bapi);
            v <<= (16 - bapi);
            cplcoeff = (sample_t)(int32_t)v;
          }
        }
        cplcoeff *= kExpScaleFactor[e];
        for (int ch = 0; ch < nfchans; ch++)
          if ((chincpl_ >> ch) & 1)
            pcmOut[ch][i] = (2.0f * dynrng_) * cplco[ch] * cplcoeff;
        i++;
      }
    }
  }

  AC3FrameHeader header_;
  AC3BitReader bitReader_;
  bool haveHeader_ = false;

  AC3ChannelState chan_[kAc3MaxChannels - 1];
  AC3ChannelState lfe_;
  AC3ChannelState cplState_;

  AC3BitAllocInfo baInfo_;
  int cplfleak_ = 0, cplsleak_ = 0;

  uint32_t chincpl_ = 0;
  bool phsflginu_ = false;
  int cplstrtmant_ = 0, cplendmant_ = 0;
  uint32_t cplbndstrc_ = 0;
  int ncplbnd_ = 0, cplstrtbnd_ = 0;
  sample_t cplco_[kAc3MaxChannels][18] = {{0}};

  uint32_t rematflg_ = 0;
  sample_t dynrng_ = 1.0f;
  uint16_t lfsrState_ = 1;

  int outputChannels_ = 0;  // 0 = discrete passthrough; else downmix target
  bool mergeFallback_ = false;  // set permanently on a blksw merge conflict
  sample_t mixDelay_[2][256] = {{0}};  // overlap-add state for merged output

  // Per-block/per-call scratch, kept as members instead of stack locals so
  // decodeBlock() doesn't need ~1.3KB of transient stack.
  sample_t lfeBuf_[256] = {0};
  sample_t coeffFactor_[25] = {0};

  AC3Imdct imdct_;
};

}  // namespace ac3
