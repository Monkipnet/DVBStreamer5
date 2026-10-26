#pragma once
/**
 * AC-3 sync frame header (syncinfo + bsi) parsing, A/52 5.3.
 */

#include "AC3BitReader.h"
#include "AC3Config.h"
#include "AC3Tables.h"

namespace ac3 {

struct AC3FrameHeader {
  // syncinfo()
  int fscod = 0;       // sample rate code: 0=48k,1=44.1k,2=32k
  int frmsizecod = 0;  // frame size / bitrate code
  int sampleRate = 0;
  int bitRateKbps = 0;
  int frameSizeBytes = 0;

  // bsi()
  int bsid = 0;
  int halfrate = 0;
  int bsmod = 0;
  int acmod = 0;
  bool lfeon = false;
  sample_t clev = kLevel3Db;  // center mix level
  sample_t slev = kLevel3Db;  // surround mix level
  int dsurmod = 0;
  int dialnorm = 0;

  int numFbwChannels = 0;
  int totalChannels = 0;  // full-bandwidth + LFE

  int headerBits = 0;  // bit position right after bsi(), start of first audio block
};

/// Validate the 6-byte sync header and fill in fscod/frmsizecod/rate/size.
/// Returns false if this is not a valid AC-3 sync frame start.
static inline bool ac3ParseSyncInfo(const uint8_t* buf, int available,
                                     AC3FrameHeader& hdr) {
  if (available < 6) return false;
  if (buf[0] != 0x0B || buf[1] != 0x77) return false;
  int bsid = buf[5] >> 3;
  if (bsid >= 12) return false;  // only classic AC-3 (bsid < 12) supported

  int fscod = buf[4] >> 6;
  int frmsizecod = buf[4] & 0x3F;
  if (fscod == 3 || frmsizecod >= 38) return false;

  int halfrate = 0;
  static const uint8_t kHalfRateTab[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3};
  halfrate = kHalfRateTab[bsid];

  hdr.fscod = fscod;
  hdr.frmsizecod = frmsizecod;
  hdr.sampleRate = ac3SampleRateForFscod(fscod) >> halfrate;
  hdr.bitRateKbps = kAc3BitRateKbps[frmsizecod >> 1] >> halfrate;
  hdr.frameSizeBytes = ac3FrameSizeBytes(fscod, frmsizecod);
  hdr.bsid = bsid;
  hdr.halfrate = halfrate;
  return hdr.frameSizeBytes > 0;
}

/// Parse the rest of the header (bit-stream-info) starting right after the
/// syncinfo() bytes. On return, reader's bit position is at the start of
/// the first audio block. Returns false on invalid/unsupported syntax.
static inline bool ac3ParseBsi(AC3BitReader& br, AC3FrameHeader& hdr) {
  br.setBitPosition(32);  // skip syncword(16) + crc1(16)

  br.skipBits(8);  // fscod(2)+frmsizecod(6), already known from syncinfo
  hdr.bsid = br.getBits(5);
  hdr.bsmod = br.getBits(3);
  hdr.acmod = br.getBits(3);

  int acmod = hdr.acmod;
  hdr.numFbwChannels = ac3NumFbwChannels(acmod);

  hdr.dsurmod = 0;
  if (acmod == kModeStereo) {
    hdr.dsurmod = br.getBits(2);  // dsurmod
  }
  if ((acmod & 1) && acmod != kModeDualMono) {
    hdr.clev = kCenterMixLevel[br.getBits(2)];  // cmixlev
  }
  if (acmod & 4) {
    hdr.slev = kSurroundMixLevel[br.getBits(2)];  // surmixlev
  }
  hdr.lfeon = br.getBits(1) != 0;
  hdr.totalChannels = hdr.numFbwChannels + (hdr.lfeon ? 1 : 0);

  int chaninfo = (acmod == 0) ? 1 : 0;  // dual-mono has 2 "logical" channels
  do {
    hdr.dialnorm = br.getBits(5);
    if (br.getBits(1)) br.skipBits(8);  // compre / compr
    if (br.getBits(1)) br.skipBits(8);  // langcode / langcod
    if (br.getBits(1)) br.skipBits(7);  // audprodie / mixlevel+roomtyp
  } while (chaninfo--);

  br.skipBits(2);  // copyrightb + origbs

  if (br.getBits(1)) br.skipBits(14);  // timecod1e / timecod1
  if (br.getBits(1)) br.skipBits(14);  // timecod2e / timecod2

  if (br.getBits(1)) {  // addbsie
    int addbsil = br.getBits(6);
    br.skipBits(8 * (addbsil + 1));
  }

  hdr.headerBits = br.bitPosition();
  return !br.overflowed();
}

}  // namespace ac3
