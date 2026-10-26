#pragma once
/**
 * Downmixes fully-decoded, discrete AC-3 PCM (as produced by
 * AC3Decoder::decodeBlock(), one 256-sample block per full-bandwidth
 * channel, in the decoder's own channel order [L, C, R, SL, SR] trimmed
 * to the coded acmod) to 1 (mono) or 2 (stereo) output channels.
 *
 * The mix weights implement the standard AC-3 downmix equations (A/52
 * Annex A / ITU-R BS.775-style center & surround mix levels), using the
 * cmixlev/surmixlev ("clev"/"slev") values carried in the frame header,
 * combined with the matching headroom normalization so the result stays
 * within +-1.0 for full-scale input. LFE is intentionally excluded (it
 * is not part of the standard downmix; route it separately - e.g. sum it
 * in yourself at whatever gain suits your bass management - if desired).
 */

#include "AC3Config.h"

namespace ac3 {

class AC3Downmix {
 public:
  /// in[0..nfchans) are the decoded full-bandwidth channel buffers for
  /// this acmod (nfchans = ac3NumFbwChannels(acmod)), each 'numSamples'
  /// long. 'out' must point to 'targetChannels' buffers of the same
  /// length (targetChannels == 1 or 2; for targetChannels == 1, only
  /// out[0] is used). Returns false for an unsupported targetChannels.
  static bool mix(const sample_t* const* in, int acmod, sample_t clev,
                   sample_t slev, int targetChannels, sample_t* const* out,
                   int numSamples = kAc3BlockSamples) {
    if (targetChannels == 2)
      return mixToStereo(in, acmod, clev, slev, out[0], out[1], numSamples);
    if (targetChannels == 1)
      return mixToMono(in, acmod, clev, slev, out[0], numSamples);
    return false;
  }

 private:
  static bool mixToStereo(const sample_t* const* in, int acmod,
                           sample_t clev, sample_t slev, sample_t* L,
                           sample_t* R, int n) {
    switch (acmod) {
      case kModeDualMono:
      case kModeStereo:
        for (int i = 0; i < n; i++) {
          L[i] = in[0][i];
          R[i] = in[1][i];
        }
        return true;

      case kModeMono:
        for (int i = 0; i < n; i++) L[i] = R[i] = in[0][i];
        return true;

      case kMode3F: {
        sample_t inv = 1.0f / (1.0f + clev);
        for (int i = 0; i < n; i++) {
          sample_t c = clev * in[1][i];
          L[i] = (in[0][i] + c) * inv;
          R[i] = (in[2][i] + c) * inv;
        }
        return true;
      }

      case kMode2F1R: {
        sample_t sc = slev * kLevel3Db;
        sample_t inv = 1.0f / (1.0f + sc);
        for (int i = 0; i < n; i++) {
          sample_t s = sc * in[2][i];
          L[i] = (in[0][i] + s) * inv;
          R[i] = (in[1][i] + s) * inv;
        }
        return true;
      }

      case kMode3F1R: {
        sample_t sc = slev * kLevel3Db;
        sample_t inv = 1.0f / (1.0f + clev + sc);
        for (int i = 0; i < n; i++) {
          sample_t common = clev * in[1][i] + sc * in[3][i];
          L[i] = (in[0][i] + common) * inv;
          R[i] = (in[2][i] + common) * inv;
        }
        return true;
      }

      case kMode2F2R: {
        sample_t inv = 1.0f / (1.0f + slev);
        for (int i = 0; i < n; i++) {
          L[i] = (in[0][i] + slev * in[2][i]) * inv;
          R[i] = (in[1][i] + slev * in[3][i]) * inv;
        }
        return true;
      }

      case kMode3F2R: {
        sample_t inv = 1.0f / (1.0f + clev + slev);
        for (int i = 0; i < n; i++) {
          sample_t c = clev * in[1][i];
          L[i] = (in[0][i] + c + slev * in[3][i]) * inv;
          R[i] = (in[2][i] + c + slev * in[4][i]) * inv;
        }
        return true;
      }
    }
    return false;
  }

  static bool mixToMono(const sample_t* const* in, int acmod, sample_t clev,
                         sample_t slev, sample_t* out, int n) {
    switch (acmod) {
      case kModeDualMono:
      case kModeStereo:
        for (int i = 0; i < n; i++) out[i] = 0.5f * (in[0][i] + in[1][i]);
        return true;

      case kModeMono:
        for (int i = 0; i < n; i++) out[i] = in[0][i];
        return true;

      case kMode3F: {
        sample_t inv = 1.0f / (1.0f + clev);
        for (int i = 0; i < n; i++)
          out[i] = (0.5f * in[0][i] + clev * in[1][i] + 0.5f * in[2][i]) * inv;
        return true;
      }

      case kMode2F1R: {
        sample_t inv = 1.0f / (2.0f + slev);
        for (int i = 0; i < n; i++)
          out[i] = (in[0][i] + in[1][i] + slev * in[2][i]) * inv;
        return true;
      }

      case kMode3F1R: {
        sample_t inv = 1.0f / (1.0f + clev + 0.5f * slev);
        for (int i = 0; i < n; i++)
          out[i] = (0.5f * in[0][i] + clev * in[1][i] + 0.5f * in[2][i] +
                    0.5f * slev * in[3][i]) *
                   inv;
        return true;
      }

      case kMode2F2R: {
        sample_t inv = 0.5f / (1.0f + slev);
        for (int i = 0; i < n; i++)
          out[i] = (in[0][i] + in[1][i] + slev * (in[2][i] + in[3][i])) * inv;
        return true;
      }

      case kMode3F2R: {
        sample_t inv = 1.0f / (1.0f + clev + slev);
        for (int i = 0; i < n; i++)
          out[i] = (0.5f * in[0][i] + clev * in[1][i] + 0.5f * in[2][i] +
                    0.5f * slev * (in[3][i] + in[4][i])) *
                   inv;
        return true;
      }
    }
    return false;
  }
};

}  // namespace ac3
