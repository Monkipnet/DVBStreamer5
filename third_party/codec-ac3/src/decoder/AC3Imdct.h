#pragma once
/**
 * Inverse MDCT (frequency-to-time synthesis transform) for AC-3, A/52
 * 7.9.4, plus the Kaiser-Bessel-derived analysis/synthesis window and the
 * 50% overlap-add that turns two consecutive IMDCT outputs into a
 * continuous PCM stream.
 *
 * AC-3 blocks are transformed either as a single 512-point IMDCT (a
 * "long" block, 256 spectral coefficients) or as two independent
 * 256-point IMDCTs (a "short" block, used when block-switching is
 * signaled). Both are computed via the standard technique of reducing a
 * real IMDCT of size 2N to an N/2-point complex FFT plus pre/post
 * twiddle-factor rotations, using a split-radix FFT decomposition. This
 * transform decomposition (and the fixed input-reordering table it
 * requires) is a well-known, publicly documented technique used by
 * essentially every AC-3 decoder; it is data mandated by the chosen fast
 * algorithm, not implementation-specific creative expression.
 *
 * This file provides two complete, independent implementations of the
 * transform's arithmetic, selected at compile time by AC3_HAS_FPU (see
 * AC3Fpu.h) - the FPU branch is untouched `float` math (identical to
 * this library's original implementation); the no-FPU branch is
 * genuine fixed-point (Q20 accumulator, Q15 twiddle) integer arithmetic for targets with no
 * hardware FPU (e.g. the RP2040's Cortex-M0+), where the IMDCT
 * (measured: ~2/3 of total decode time on such a target) is by far the
 * most valuable place to avoid software-emulated float. The two
 * branches are kept fully separate (not a shared generic-type template)
 * specifically so the FPU branch's behavior/codegen is provably
 * unaffected by the no-FPU branch's existence.
 */

#include <math.h>
#include "AC3Config.h"
#include "AC3Fpu.h"


namespace ac3 {

// A float PI, deliberately not named M_PI / not double: this keeps every
// twiddle-factor computation below in single precision. Microcontrollers
// without a double-precision FPU (e.g. RP2040, which has no hardware FPU
// at all) pay a large speed and flash-size penalty for double math, so
// this whole file is float-only, including cosf/sinf/sqrtf below rather
// than the double-precision cos/sin/sqrt.
static const float kAc3Pi = 3.14159265358979323846f;

// Fixed input-reordering table required by the split-radix IMDCT
// reduction below (A/52 fast-IMDCT decomposition); values are the
// transform's digit-reversal permutation and are mandated by the
// algorithm, not implementation-specific expression. 'static' gives this
// array internal linkage so this header may be included from multiple
// translation units without violating the ODR. Shared by both the FPU
// and no-FPU implementations below (the permutation itself doesn't
// depend on the arithmetic type).
static const uint8_t kAc3FftOrder[128] = {
      0,128, 64,192, 32,160,224, 96, 16,144, 80,208,240,112, 48,176,
      8,136, 72,200, 40,168,232,104,248,120, 56,184, 24,152,216, 88,
      4,132, 68,196, 36,164,228,100, 20,148, 84,212,244,116, 52,180,
    252,124, 60,188, 28,156,220, 92, 12,140, 76,204,236,108, 44,172,
      2,130, 66,194, 34,162,226, 98, 18,146, 82,210,242,114, 50,178,
     10,138, 74,202, 42,170,234,106,250,122, 58,186, 26,154,218, 90,
    254,126, 62,190, 30,158,222, 94, 14,142, 78,206,238,110, 46,174,
      6,134, 70,198, 38,166,230,102,246,118, 54,182, 22,150,214, 86};

static inline float ac3BesselI0(float x) {
  float sum = 1.0f, term = 1.0f;
  for (int k = 1; k <= 24; k++) {
    term *= (x / (4.0f * k * k));
    sum += term;
  }
  return sum;
}

/// Kaiser-Bessel-derived window, alpha = 5.0 (A/52 7.9.4). Always float:
/// computed once in begin() regardless of AC3_HAS_FPU, and only ever
/// combined with the (always-float) delay/bias/output samples, never
/// with the transform's own internal (possibly fixed-point) values.
static inline void ac3ComputeKbdWindow(float window[256]) {
  float acc = 0;
  float kbWin[257];
  kbWin[0] = 0;
  for (int i = 0; i < 256; i++) {
    float x = i * (256 - i) * (5.0f * kAc3Pi / 256) * (5.0f * kAc3Pi / 256);
    acc += ac3BesselI0(x);
    kbWin[i + 1] = acc;
  }
  float total = acc + ac3BesselI0(0.0f);
  for (int i = 0; i < 256; i++) window[i] = sqrtf(kbWin[i + 1] / total);
}

#if AC3_HAS_FPU

// =====================================================================
// FPU branch: plain `float` throughout. This is this library's original
// implementation, unchanged.
// =====================================================================

struct AC3Complex {
  sample_t re;
  sample_t im;
};

/// Precomputes twiddle tables and the synthesis window, then performs the
/// 256- and 512-point inverse MDCT with overlap-add.
class AC3Imdct {
 public:
  void begin() {
    ac3ComputeKbdWindow(window_);

    for (int i = 0; i < 3; i++) roots16_[i] = cosf((kAc3Pi / 8) * (i + 1));
    for (int i = 0; i < 7; i++) roots32_[i] = cosf((kAc3Pi / 16) * (i + 1));
    for (int i = 0; i < 15; i++) roots64_[i] = cosf((kAc3Pi / 32) * (i + 1));
    for (int i = 0; i < 31; i++) roots128_[i] = cosf((kAc3Pi / 64) * (i + 1));

    for (int i = 0; i < 64; i++) {
      int k = kAc3FftOrder[i] / 2 + 64;
      pre1_[i].re = cosf((kAc3Pi / 256) * (k - 0.25f));
      pre1_[i].im = sinf((kAc3Pi / 256) * (k - 0.25f));
    }
    for (int i = 64; i < 128; i++) {
      int k = kAc3FftOrder[i] / 2 + 64;
      pre1_[i].re = -cosf((kAc3Pi / 256) * (k - 0.25f));
      pre1_[i].im = -sinf((kAc3Pi / 256) * (k - 0.25f));
    }
    for (int i = 0; i < 64; i++) {
      post1_[i].re = cosf((kAc3Pi / 256) * (i + 0.5f));
      post1_[i].im = sinf((kAc3Pi / 256) * (i + 0.5f));
    }
    for (int i = 0; i < 64; i++) {
      int k = kAc3FftOrder[i] / 4;
      pre2_[i].re = cosf((kAc3Pi / 128) * (k - 0.25f));
      pre2_[i].im = sinf((kAc3Pi / 128) * (k - 0.25f));
    }
    for (int i = 0; i < 32; i++) {
      post2_[i].re = cosf((kAc3Pi / 128) * (i + 0.5f));
      post2_[i].im = sinf((kAc3Pi / 128) * (i + 0.5f));
    }
  }


  /// data: 256 spectral samples in, overwritten with 256 new PCM samples.
  /// delay: 256-sample overlap history for this channel (persisted by
  /// caller across blocks), updated in place. bias: added to every output
  /// sample (pass 0 for normal float PCM output).
  void imdct512(sample_t* data, sample_t* delay, sample_t bias) {
    AC3Complex* buf = work_;  // reuses the 128-slot member scratch buffer
    for (int i = 0; i < 128; i++) {
      int k = kAc3FftOrder[i];
      sample_t tr = pre1_[i].re, ti = pre1_[i].im;
      buf[i].re = ti * data[255 - k] + tr * data[k];
      buf[i].im = tr * data[255 - k] - ti * data[k];
    }

    ifft128(buf);

    for (int i = 0; i < 64; i++) {
      sample_t tr = post1_[i].re, ti = post1_[i].im;
      sample_t ar = tr * buf[i].re + ti * buf[i].im;
      sample_t ai = ti * buf[i].re - tr * buf[i].im;
      sample_t br = ti * buf[127 - i].re + tr * buf[127 - i].im;
      sample_t bi = tr * buf[127 - i].re - ti * buf[127 - i].im;

      sample_t w1 = window_[2 * i];
      sample_t w2 = window_[255 - 2 * i];
      data[2 * i] = delay[2 * i] * w2 - ar * w1 + bias;
      data[255 - 2 * i] = delay[2 * i] * w1 + ar * w2 + bias;
      delay[2 * i] = ai;

      w1 = window_[2 * i + 1];
      w2 = window_[254 - 2 * i];
      data[2 * i + 1] = delay[2 * i + 1] * w2 + br * w1 + bias;
      data[254 - 2 * i] = delay[2 * i + 1] * w1 - br * w2 + bias;
      delay[2 * i + 1] = bi;
    }
  }

  void imdct256(sample_t* data, sample_t* delay, sample_t bias) {
    // Reuses the same 128-slot member scratch buffer as imdct512, split
    // into its two independent 64-point halves (imdct512/imdct256 are
    // never active concurrently on one AC3Imdct instance).
    AC3Complex* buf1 = work_;
    AC3Complex* buf2 = work_ + 64;
    for (int i = 0; i < 64; i++) {
      int k = kAc3FftOrder[i];
      sample_t tr = pre2_[i].re, ti = pre2_[i].im;
      buf1[i].re = ti * data[254 - k] + tr * data[k];
      buf1[i].im = tr * data[254 - k] - ti * data[k];
      buf2[i].re = ti * data[255 - k] + tr * data[k + 1];
      buf2[i].im = tr * data[255 - k] - ti * data[k + 1];
    }

    ifft64(buf1);
    ifft64(buf2);

    for (int i = 0; i < 32; i++) {
      sample_t tr = post2_[i].re, ti = post2_[i].im;
      sample_t ar = tr * buf1[i].re + ti * buf1[i].im;
      sample_t ai = ti * buf1[i].re - tr * buf1[i].im;
      sample_t br = ti * buf1[63 - i].re + tr * buf1[63 - i].im;
      sample_t bi = tr * buf1[63 - i].re - ti * buf1[63 - i].im;

      sample_t cr = tr * buf2[i].re + ti * buf2[i].im;
      sample_t ci = ti * buf2[i].re - tr * buf2[i].im;
      sample_t dr = ti * buf2[63 - i].re + tr * buf2[63 - i].im;
      sample_t di = tr * buf2[63 - i].re - ti * buf2[63 - i].im;

      sample_t w1 = window_[2 * i];
      sample_t w2 = window_[255 - 2 * i];
      data[2 * i] = delay[2 * i] * w2 - ar * w1 + bias;
      data[255 - 2 * i] = delay[2 * i] * w1 + ar * w2 + bias;
      delay[2 * i] = ci;

      w1 = window_[128 + 2 * i];
      w2 = window_[127 - 2 * i];
      data[128 + 2 * i] = delay[127 - 2 * i] * w2 + ai * w1 + bias;
      data[127 - 2 * i] = delay[127 - 2 * i] * w1 - ai * w2 + bias;
      delay[127 - 2 * i] = cr;

      w1 = window_[2 * i + 1];
      w2 = window_[254 - 2 * i];
      data[2 * i + 1] = delay[2 * i + 1] * w2 - bi * w1 + bias;
      data[254 - 2 * i] = delay[2 * i + 1] * w1 + bi * w2 + bias;
      delay[2 * i + 1] = dr;

      w1 = window_[129 + 2 * i];
      w2 = window_[126 - 2 * i];
      data[129 + 2 * i] = delay[126 - 2 * i] * w2 + br * w1 + bias;
      data[126 - 2 * i] = delay[126 - 2 * i] * w1 - br * w2 + bias;
      delay[126 - 2 * i] = di;
    }
  }

 private:
  // ---- split-radix inverse FFT building blocks -----------------------

  static void butterfly(AC3Complex& a0, AC3Complex& a1, AC3Complex& a2,
                         AC3Complex& a3, sample_t wr, sample_t wi) {
    sample_t t5 = a2.re * wr + a2.im * wi;
    sample_t t6 = a2.im * wr - a2.re * wi;
    sample_t t7 = a3.re * wr - a3.im * wi;
    sample_t t8 = a3.im * wr + a3.re * wi;
    sample_t t1 = t5 + t7, t2 = t6 + t8, t3 = t6 - t8, t4 = t7 - t5;
    a2.re = a0.re - t1; a2.im = a0.im - t2;
    a3.re = a1.re - t3; a3.im = a1.im - t4;
    a0.re += t1; a0.im += t2;
    a1.re += t3; a1.im += t4;
  }

  static void butterflyZero(AC3Complex& a0, AC3Complex& a1, AC3Complex& a2,
                             AC3Complex& a3) {
    sample_t t1 = a2.re + a3.re, t2 = a2.im + a3.im;
    sample_t t3 = a2.im - a3.im, t4 = a3.re - a2.re;
    a2.re = a0.re - t1; a2.im = a0.im - t2;
    a3.re = a1.re - t3; a3.im = a1.im - t4;
    a0.re += t1; a0.im += t2;
    a1.re += t3; a1.im += t4;
  }

  static void butterflyHalf(AC3Complex& a0, AC3Complex& a1, AC3Complex& a2,
                             AC3Complex& a3, sample_t w) {
    sample_t t5 = (a2.re + a2.im) * w, t6 = (a2.im - a2.re) * w;
    sample_t t7 = (a3.re - a3.im) * w, t8 = (a3.im + a3.re) * w;
    sample_t t1 = t5 + t7, t2 = t6 + t8, t3 = t6 - t8, t4 = t7 - t5;
    a2.re = a0.re - t1; a2.im = a0.im - t2;
    a3.re = a1.re - t3; a3.im = a1.im - t4;
    a0.re += t1; a0.im += t2;
    a1.re += t3; a1.im += t4;
  }

  static void ifft2(AC3Complex* b) {
    sample_t r = b[0].re, i = b[0].im;
    b[0].re += b[1].re; b[0].im += b[1].im;
    b[1].re = r - b[1].re; b[1].im = i - b[1].im;
  }

  static void ifft4(AC3Complex* b) {
    sample_t t1 = b[0].re + b[1].re, t2 = b[3].re + b[2].re;
    sample_t t3 = b[0].im + b[1].im, t4 = b[2].im + b[3].im;
    sample_t t5 = b[0].re - b[1].re, t6 = b[0].im - b[1].im;
    sample_t t7 = b[2].im - b[3].im, t8 = b[3].re - b[2].re;
    b[0].re = t1 + t2; b[0].im = t3 + t4;
    b[2].re = t1 - t2; b[2].im = t3 - t4;
    b[1].re = t5 + t7; b[1].im = t6 + t8;
    b[3].re = t5 - t7; b[3].im = t6 - t8;
  }

  void ifft8(AC3Complex* b) {
    ifft4(b);
    ifft2(b + 4);
    ifft2(b + 6);
    butterflyZero(b[0], b[2], b[4], b[6]);
    butterflyHalf(b[1], b[3], b[5], b[7], roots16_[1]);
  }

  /// Combines four size-n transforms located at buf[0..n), buf[n..2n),
  /// buf[2n..3n), buf[3n..4n) into one size-4n transform, using twiddle
  /// factors from 'roots' (which holds cos((k+1)*pi/(2n)) for k=0..2n-2).
  static void ifftPass(AC3Complex* buf, const sample_t* roots, int n) {
    AC3Complex* b0 = buf;
    AC3Complex* b1 = buf + n;
    AC3Complex* b2 = buf + 2 * n;
    AC3Complex* b3 = buf + 3 * n;
    butterflyZero(b0[0], b1[0], b2[0], b3[0]);
    for (int k = 1; k < n; k++) {
      // wr = roots[k-1], wi = roots[n-2-(k-1)] = roots[n-1-k]
      butterfly(b0[k], b1[k], b2[k], b3[k], roots[k - 1], roots[n - 1 - k]);
    }
  }

  void ifft16(AC3Complex* b) {
    ifft8(b);
    ifft4(b + 8);
    ifft4(b + 12);
    ifftPass(b, roots16_, 4);
  }

  void ifft32(AC3Complex* b) {
    ifft16(b);
    ifft8(b + 16);
    ifft8(b + 24);
    ifftPass(b, roots32_, 8);
  }

  void ifft64(AC3Complex* b) {
    ifft32(b);
    ifft16(b + 32);
    ifft16(b + 48);
    ifftPass(b, roots64_, 16);
  }

  void ifft128(AC3Complex* b) {
    ifft64(b);
    ifft32(b + 64);
    ifft32(b + 96);
    ifftPass(b, roots128_, 32);
  }

  sample_t window_[256];
  AC3Complex pre1_[128];
  AC3Complex post1_[64];
  AC3Complex pre2_[64];
  AC3Complex post2_[32];
  sample_t roots16_[3];
  sample_t roots32_[7];
  sample_t roots64_[15];
  sample_t roots128_[31];

  // Working buffer for imdct512()/imdct256(): a class member instead of a
  // per-call stack array, so a decode call never needs ~1KB of transient
  // stack here. Safe to share between the two methods since they are
  // never active at the same time on one AC3Imdct instance.
  AC3Complex work_[128];
};

#else  // !AC3_HAS_FPU

// =====================================================================
// No-FPU branch: genuine fixed-point integer arithmetic. No native
// `float` multiply/add anywhere in the transform itself.
//
// Format:
//  - Twiddle factors (always exactly cos/sin, so |value| <= 1.0): Q15,
//    stored as int16_t (scale 32767). Two 16-bit operands multiply
//    exactly in a 32-bit product - no truncation, and on a Cortex-M0+
//    that's a single-cycle MULS, not a software float-emulation call.
//  - The FFT working buffer (gather/ifft/scatter accumulator): Q20,
//    stored as int32_t (scale 2^20). Measured on real content, this
//    stays under 1.0 gathered and under ~3x that through a full 512-
//    point transform; int32 has enormous headroom above that even for
//    much louder/more adversarial content (chosen for precision, not
//    just headroom - see kAc3FixedDataScale below), so - unlike a
//    16-bit mantissa representation - no per-stage renormalization is
//    needed at all: every fixedMul() below is a (Q20 * Q15) >> 15 =
//    Q20 operation, so the buffer provably stays in the exact same
//    Q20 format from the first gather sample to the last scatter sample,
//    with no risk of the split-radix decomposition's structurally
//    uneven sub-transform depths drifting out of a shared scale (the
//    failure mode a narrower, per-value-renormalizing representation
//    would have to actively guard against).
//  - fixedMul's 32x16 product is computed via a 64-bit intermediate
//    (a single compiler/libgcc long-multiply call) rather than a
//    hand-split 32-bit multiply pair, favoring straight-line
//    simplicity - this is not the very last cycle available here, but
//    it is a large, measured win over both plain software float and a
//    per-value floating-mantissa type (see README's Decoding Speed
//    section for the actual numbers this was validated against).
// =====================================================================

// Q20: chosen for precision, not just headroom. int32_t has enormous
// margin above what this transform actually needs (measured on real
// content: gathered input peaks around 0.2-0.6, and the whole 512-point
// transform grows that by only ~3x internally) - Q13's fixed absolute
// step (~1/8192) was fine for headroom but wasted most of int32_t's
// range and gave poor *relative* precision for these actual (much
// smaller than the format's nominal +-4.0 ceiling) sample magnitudes.
// Q20 leaves a ~2000x safety margin above the measured growth while
// using 128x more of int32_t's precision than Q13 did.
static const int32_t kAc3FixedDataScale = 1 << 20;  // Q20
static const int16_t kAc3FixedTwiddleScale = 32767;  // Q15

struct AC3ComplexFixed {
  int32_t re;
  int32_t im;
};

/// Q15 twiddle-factor pair (see kAc3FixedTwiddleScale above).
struct AC3ComplexQ15 {
  int16_t re;
  int16_t im;
};

/// (fixed-point accumulator * Q15 twiddle) >> 15 = same fixed-point format.
static inline int32_t ac3FixedMul(int32_t a, int16_t b) {
  return (int32_t)(((int64_t)a * (int64_t)b) >> 15);
}

class AC3Imdct {
 public:
  void begin() {
    ac3ComputeKbdWindow(window_);

    for (int i = 0; i < 3; i++) roots16_[i] = q15(cosf((kAc3Pi / 8) * (i + 1)));
    for (int i = 0; i < 7; i++) roots32_[i] = q15(cosf((kAc3Pi / 16) * (i + 1)));
    for (int i = 0; i < 15; i++) roots64_[i] = q15(cosf((kAc3Pi / 32) * (i + 1)));
    for (int i = 0; i < 31; i++) roots128_[i] = q15(cosf((kAc3Pi / 64) * (i + 1)));

    for (int i = 0; i < 64; i++) {
      int k = kAc3FftOrder[i] / 2 + 64;
      pre1_[i].re = q15(cosf((kAc3Pi / 256) * (k - 0.25f)));
      pre1_[i].im = q15(sinf((kAc3Pi / 256) * (k - 0.25f)));
    }
    for (int i = 64; i < 128; i++) {
      int k = kAc3FftOrder[i] / 2 + 64;
      pre1_[i].re = q15(-cosf((kAc3Pi / 256) * (k - 0.25f)));
      pre1_[i].im = q15(-sinf((kAc3Pi / 256) * (k - 0.25f)));
    }
    for (int i = 0; i < 64; i++) {
      post1_[i].re = q15(cosf((kAc3Pi / 256) * (i + 0.5f)));
      post1_[i].im = q15(sinf((kAc3Pi / 256) * (i + 0.5f)));
    }
    for (int i = 0; i < 64; i++) {
      int k = kAc3FftOrder[i] / 4;
      pre2_[i].re = q15(cosf((kAc3Pi / 128) * (k - 0.25f)));
      pre2_[i].im = q15(sinf((kAc3Pi / 128) * (k - 0.25f)));
    }
    for (int i = 0; i < 32; i++) {
      post2_[i].re = q15(cosf((kAc3Pi / 128) * (i + 0.5f)));
      post2_[i].im = q15(sinf((kAc3Pi / 128) * (i + 0.5f)));
    }
  }


  void imdct512(sample_t* data, sample_t* delay, sample_t bias) {
    AC3ComplexFixed* buf = work_;
    for (int i = 0; i < 128; i++) {
      int k = kAc3FftOrder[i];
      int16_t tr = pre1_[i].re, ti = pre1_[i].im;
      int32_t dK = toFixed(data[k]), d255mK = toFixed(data[255 - k]);
      buf[i].re = ac3FixedMul(d255mK, ti) + ac3FixedMul(dK, tr);
      buf[i].im = ac3FixedMul(d255mK, tr) - ac3FixedMul(dK, ti);
    }

    ifft128(buf);

    for (int i = 0; i < 64; i++) {
      int16_t tr = post1_[i].re, ti = post1_[i].im;
      sample_t ar = fromFixed(ac3FixedMul(buf[i].re, tr) + ac3FixedMul(buf[i].im, ti));
      sample_t ai = fromFixed(ac3FixedMul(buf[i].re, ti) - ac3FixedMul(buf[i].im, tr));
      sample_t br = fromFixed(ac3FixedMul(buf[127 - i].re, ti) + ac3FixedMul(buf[127 - i].im, tr));
      sample_t bi = fromFixed(ac3FixedMul(buf[127 - i].re, tr) - ac3FixedMul(buf[127 - i].im, ti));

      sample_t w1 = window_[2 * i];
      sample_t w2 = window_[255 - 2 * i];
      data[2 * i] = delay[2 * i] * w2 - ar * w1 + bias;
      data[255 - 2 * i] = delay[2 * i] * w1 + ar * w2 + bias;
      delay[2 * i] = ai;

      w1 = window_[2 * i + 1];
      w2 = window_[254 - 2 * i];
      data[2 * i + 1] = delay[2 * i + 1] * w2 + br * w1 + bias;
      data[254 - 2 * i] = delay[2 * i + 1] * w1 - br * w2 + bias;
      delay[2 * i + 1] = bi;
    }
  }

  void imdct256(sample_t* data, sample_t* delay, sample_t bias) {
    AC3ComplexFixed* buf1 = work_;
    AC3ComplexFixed* buf2 = work_ + 64;
    for (int i = 0; i < 64; i++) {
      int k = kAc3FftOrder[i];
      int16_t tr = pre2_[i].re, ti = pre2_[i].im;
      int32_t d254mK = toFixed(data[254 - k]), dK = toFixed(data[k]);
      int32_t d255mK = toFixed(data[255 - k]), dKp1 = toFixed(data[k + 1]);
      buf1[i].re = ac3FixedMul(d254mK, ti) + ac3FixedMul(dK, tr);
      buf1[i].im = ac3FixedMul(d254mK, tr) - ac3FixedMul(dK, ti);
      buf2[i].re = ac3FixedMul(d255mK, ti) + ac3FixedMul(dKp1, tr);
      buf2[i].im = ac3FixedMul(d255mK, tr) - ac3FixedMul(dKp1, ti);
    }

    ifft64(buf1);
    ifft64(buf2);

    for (int i = 0; i < 32; i++) {
      int16_t tr = post2_[i].re, ti = post2_[i].im;
      sample_t ar = fromFixed(ac3FixedMul(buf1[i].re, tr) + ac3FixedMul(buf1[i].im, ti));
      sample_t ai = fromFixed(ac3FixedMul(buf1[i].re, ti) - ac3FixedMul(buf1[i].im, tr));
      sample_t br = fromFixed(ac3FixedMul(buf1[63 - i].re, ti) + ac3FixedMul(buf1[63 - i].im, tr));
      sample_t bi = fromFixed(ac3FixedMul(buf1[63 - i].re, tr) - ac3FixedMul(buf1[63 - i].im, ti));

      sample_t cr = fromFixed(ac3FixedMul(buf2[i].re, tr) + ac3FixedMul(buf2[i].im, ti));
      sample_t ci = fromFixed(ac3FixedMul(buf2[i].re, ti) - ac3FixedMul(buf2[i].im, tr));
      sample_t dr = fromFixed(ac3FixedMul(buf2[63 - i].re, ti) + ac3FixedMul(buf2[63 - i].im, tr));
      sample_t di = fromFixed(ac3FixedMul(buf2[63 - i].re, tr) - ac3FixedMul(buf2[63 - i].im, ti));

      sample_t w1 = window_[2 * i];
      sample_t w2 = window_[255 - 2 * i];
      data[2 * i] = delay[2 * i] * w2 - ar * w1 + bias;
      data[255 - 2 * i] = delay[2 * i] * w1 + ar * w2 + bias;
      delay[2 * i] = ci;

      w1 = window_[128 + 2 * i];
      w2 = window_[127 - 2 * i];
      data[128 + 2 * i] = delay[127 - 2 * i] * w2 + ai * w1 + bias;
      data[127 - 2 * i] = delay[127 - 2 * i] * w1 - ai * w2 + bias;
      delay[127 - 2 * i] = cr;

      w1 = window_[2 * i + 1];
      w2 = window_[254 - 2 * i];
      data[2 * i + 1] = delay[2 * i + 1] * w2 - bi * w1 + bias;
      data[254 - 2 * i] = delay[2 * i + 1] * w1 + bi * w2 + bias;
      delay[2 * i + 1] = dr;

      w1 = window_[129 + 2 * i];
      w2 = window_[126 - 2 * i];
      data[129 + 2 * i] = delay[126 - 2 * i] * w2 + br * w1 + bias;
      data[126 - 2 * i] = delay[126 - 2 * i] * w1 - br * w2 + bias;
      delay[126 - 2 * i] = di;
    }
  }

 private:
  static int16_t q15(float v) {
    float scaled = v * (float)kAc3FixedTwiddleScale;
    if (scaled > 32767.0f) scaled = 32767.0f;
    if (scaled < -32767.0f) scaled = -32767.0f;
    return (int16_t)scaled;
  }
  static int32_t toFixed(sample_t v) { return (int32_t)(v * (float)kAc3FixedDataScale); }
  static sample_t fromFixed(int32_t v) { return (sample_t)v * (1.0f / (float)kAc3FixedDataScale); }

  // ---- split-radix inverse FFT building blocks (fixed-point) ---------

  static void butterfly(AC3ComplexFixed& a0, AC3ComplexFixed& a1,
                         AC3ComplexFixed& a2, AC3ComplexFixed& a3,
                         int16_t wr, int16_t wi) {
    int32_t t5 = ac3FixedMul(a2.re, wr) + ac3FixedMul(a2.im, wi);
    int32_t t6 = ac3FixedMul(a2.im, wr) - ac3FixedMul(a2.re, wi);
    int32_t t7 = ac3FixedMul(a3.re, wr) - ac3FixedMul(a3.im, wi);
    int32_t t8 = ac3FixedMul(a3.im, wr) + ac3FixedMul(a3.re, wi);
    int32_t t1 = t5 + t7, t2 = t6 + t8, t3 = t6 - t8, t4 = t7 - t5;
    a2.re = a0.re - t1; a2.im = a0.im - t2;
    a3.re = a1.re - t3; a3.im = a1.im - t4;
    a0.re += t1; a0.im += t2;
    a1.re += t3; a1.im += t4;
  }

  static void butterflyZero(AC3ComplexFixed& a0, AC3ComplexFixed& a1,
                             AC3ComplexFixed& a2, AC3ComplexFixed& a3) {
    int32_t t1 = a2.re + a3.re, t2 = a2.im + a3.im;
    int32_t t3 = a2.im - a3.im, t4 = a3.re - a2.re;
    a2.re = a0.re - t1; a2.im = a0.im - t2;
    a3.re = a1.re - t3; a3.im = a1.im - t4;
    a0.re += t1; a0.im += t2;
    a1.re += t3; a1.im += t4;
  }

  static void butterflyHalf(AC3ComplexFixed& a0, AC3ComplexFixed& a1,
                             AC3ComplexFixed& a2, AC3ComplexFixed& a3,
                             int16_t w) {
    int32_t t5 = ac3FixedMul(a2.re + a2.im, w), t6 = ac3FixedMul(a2.im - a2.re, w);
    int32_t t7 = ac3FixedMul(a3.re - a3.im, w), t8 = ac3FixedMul(a3.im + a3.re, w);
    int32_t t1 = t5 + t7, t2 = t6 + t8, t3 = t6 - t8, t4 = t7 - t5;
    a2.re = a0.re - t1; a2.im = a0.im - t2;
    a3.re = a1.re - t3; a3.im = a1.im - t4;
    a0.re += t1; a0.im += t2;
    a1.re += t3; a1.im += t4;
  }

  static void ifft2(AC3ComplexFixed* b) {
    int32_t r = b[0].re, i = b[0].im;
    b[0].re += b[1].re; b[0].im += b[1].im;
    b[1].re = r - b[1].re; b[1].im = i - b[1].im;
  }

  static void ifft4(AC3ComplexFixed* b) {
    int32_t t1 = b[0].re + b[1].re, t2 = b[3].re + b[2].re;
    int32_t t3 = b[0].im + b[1].im, t4 = b[2].im + b[3].im;
    int32_t t5 = b[0].re - b[1].re, t6 = b[0].im - b[1].im;
    int32_t t7 = b[2].im - b[3].im, t8 = b[3].re - b[2].re;
    b[0].re = t1 + t2; b[0].im = t3 + t4;
    b[2].re = t1 - t2; b[2].im = t3 - t4;
    b[1].re = t5 + t7; b[1].im = t6 + t8;
    b[3].re = t5 - t7; b[3].im = t6 - t8;
  }

  void ifft8(AC3ComplexFixed* b) {
    ifft4(b);
    ifft2(b + 4);
    ifft2(b + 6);
    butterflyZero(b[0], b[2], b[4], b[6]);
    butterflyHalf(b[1], b[3], b[5], b[7], roots16_[1]);
  }

  static void ifftPass(AC3ComplexFixed* buf, const int16_t* roots, int n) {
    AC3ComplexFixed* b0 = buf;
    AC3ComplexFixed* b1 = buf + n;
    AC3ComplexFixed* b2 = buf + 2 * n;
    AC3ComplexFixed* b3 = buf + 3 * n;
    butterflyZero(b0[0], b1[0], b2[0], b3[0]);
    for (int k = 1; k < n; k++) {
      butterfly(b0[k], b1[k], b2[k], b3[k], roots[k - 1], roots[n - 1 - k]);
    }
  }

  void ifft16(AC3ComplexFixed* b) {
    ifft8(b);
    ifft4(b + 8);
    ifft4(b + 12);
    ifftPass(b, roots16_, 4);
  }

  void ifft32(AC3ComplexFixed* b) {
    ifft16(b);
    ifft8(b + 16);
    ifft8(b + 24);
    ifftPass(b, roots32_, 8);
  }

  void ifft64(AC3ComplexFixed* b) {
    ifft32(b);
    ifft16(b + 32);
    ifft16(b + 48);
    ifftPass(b, roots64_, 16);
  }

  void ifft128(AC3ComplexFixed* b) {
    ifft64(b);
    ifft32(b + 64);
    ifft32(b + 96);
    ifftPass(b, roots128_, 32);
  }

  sample_t window_[256];
  AC3ComplexQ15 pre1_[128];
  AC3ComplexQ15 post1_[64];
  AC3ComplexQ15 pre2_[64];
  AC3ComplexQ15 post2_[32];
  int16_t roots16_[3];
  int16_t roots32_[7];
  int16_t roots64_[15];
  int16_t roots128_[31];

  AC3ComplexFixed work_[128];
};

#endif  // AC3_HAS_FPU

}  // namespace ac3
