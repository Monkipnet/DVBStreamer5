#pragma once
/**
 * Constant tables mandated by the ATSC A/52 (AC-3) standard: frame-size
 * table, differential-exponent decode tables, mantissa dequantization
 * tables, the psychoacoustic bit-allocation tables (hearing threshold,
 * band boundaries, log-addition table, bap lookup) and the dither LUT.
 * These numeric values are defined by the public standard (and are
 * therefore identical across independent, interoperable decoders); they
 * were cross-checked against a reference open-source decoder for
 * transcription accuracy.
 */

#include <stdint.h>
#include "AC3Config.h"

namespace ac3 {

// ---------------------------------------------------------------------
// Frame size / bit rate table (indexed by frmsizecod >> 1, i.e. bitrate
// code 0..18), and per-sample-rate word-count tables (indexed by
// frmsizecod, 0..37) as defined in A/52 Table 5.18.
// ---------------------------------------------------------------------
static const int kAc3BitRateKbps[19] = {32,  40,  48,  56,  64,  80,  96, 112,
                                         128, 160, 192, 224, 256, 320, 384, 448,
                                         512, 576, 640};

// Frame size in 16-bit words, indexed [fscod][frmsizecod]. fscod: 0=48k,1=44.1k,2=32k
static const uint16_t kAc3FrameSizeWords[3][38] = {
    // 48 kHz : frmsize = 4 * bitrate(kbps) / 8 ... precomputed = 2*bitrate/4? use spec table directly
    {64,   64,   80,   80,   96,   96,   112,  112,  128,  128,
     160,  160,  192,  192,  224,  224,  256,  256,  320,  320,
     384,  384,  448,  448,  512,  512,  640,  640,  768,  768,
     896,  896,  1024, 1024, 1152, 1152, 1280, 1280},
    // 44.1 kHz
    {69,   70,   87,   88,   104,  105,  121,  122,  139,  140,
     174,  175,  208,  209,  243,  244,  278,  279,  348,  349,
     417,  418,  487,  488,  557,  558,  696,  697,  835,  836,
     975,  976,  1114, 1115, 1253, 1254, 1393, 1394},
    // 32 kHz
    {96,   96,   120,  120,  144,  144,  168,  168,  192,  192,
     240,  240,  288,  288,  336,  336,  384,  384,  480,  480,
     576,  576,  672,  672,  768,  768,  960,  960,  1152, 1152,
     1344, 1344, 1536, 1536, 1728, 1728, 1920, 1920}};

static inline int ac3SampleRateForFscod(int fscod) {
  static const int rates[4] = {48000, 44100, 32000, 0};
  return rates[fscod & 3];
}

/// Frame size in bytes for a given fscod (0/1/2) and frmsizecod (0..37).
static inline int ac3FrameSizeBytes(int fscod, int frmsizecod) {
  if (fscod > 2 || frmsizecod > 37) return 0;
  return kAc3FrameSizeWords[fscod][frmsizecod] * 2;
}

// ---------------------------------------------------------------------
// Differential exponent decode tables (A/52 7.1.3). Each 7-bit exponent
// group codes 3 successive exponent differences (-2..+2), or run codes
// 25 (all-equal-2 x3) which is unused for delta but appears in the plain
// grouped tables below (values 0..24 map through exp_1/2/3, 25 marks the
// "reuse previous" padding rows and never occurs for a valid 7-bit code
// since 5^3=125 > 128; codes 125..127 are unused/invalid).
// ---------------------------------------------------------------------
static const int8_t kExpDelta1[128] = {
    -2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
     2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    25,25,25};
static const int8_t kExpDelta2[128] = {
    -2,-2,-2,-2,-2,-1,-1,-1,-1,-1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2,
    -2,-2,-2,-2,-2,-1,-1,-1,-1,-1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2,
    -2,-2,-2,-2,-2,-1,-1,-1,-1,-1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2,
    -2,-2,-2,-2,-2,-1,-1,-1,-1,-1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2,
    -2,-2,-2,-2,-2,-1,-1,-1,-1,-1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2,
    25,25,25};
static const int8_t kExpDelta3[128] = {
    -2,-1, 0, 1, 2,-2,-1, 0, 1, 2,-2,-1, 0, 1, 2,-2,-1, 0, 1, 2,-2,-1, 0, 1, 2,
    -2,-1, 0, 1, 2,-2,-1, 0, 1, 2,-2,-1, 0, 1, 2,-2,-1, 0, 1, 2,-2,-1, 0, 1, 2,
    -2,-1, 0, 1, 2,-2,-1, 0, 1, 2,-2,-1, 0, 1, 2,-2,-1, 0, 1, 2,-2,-1, 0, 1, 2,
    -2,-1, 0, 1, 2,-2,-1, 0, 1, 2,-2,-1, 0, 1, 2,-2,-1, 0, 1, 2,-2,-1, 0, 1, 2,
    -2,-1, 0, 1, 2,-2,-1, 0, 1, 2,-2,-1, 0, 1, 2,-2,-1, 0, 1, 2,-2,-1, 0, 1, 2,
    25,25,25};

// ---------------------------------------------------------------------
// Mantissa dequantization tables for the low-bit-count (asymmetric,
// vector-quantized) cases bap = 1,2,4 (3 or 5 mantissas packed per
// codeword) and bap = 3,5 (single symmetric quantizers), A/52 7.4.4.
// Values are pre-scaled to a 16-bit-domain fixed point (matching the
// convention used by the exponent scale_factor table below).
// ---------------------------------------------------------------------
namespace detail {
static inline sample_t Q(int num, float den) {
  return (num * 32768.0f) / den;
}
}  // namespace detail

// bap==1 : 3 mantissas per 5-bit codeword, levels {-2,0,2}/3
static const sample_t kQuant1_0[32] = {
    detail::Q(-2,3),detail::Q(-2,3),detail::Q(-2,3),detail::Q(-2,3),detail::Q(-2,3),detail::Q(-2,3),detail::Q(-2,3),detail::Q(-2,3),detail::Q(-2,3),
    0,0,0,0,0,0,0,0,0,
    detail::Q(2,3),detail::Q(2,3),detail::Q(2,3),detail::Q(2,3),detail::Q(2,3),detail::Q(2,3),detail::Q(2,3),detail::Q(2,3),detail::Q(2,3),
    0,0,0,0,0};
static const sample_t kQuant1_1[32] = {
    detail::Q(-2,3),detail::Q(-2,3),detail::Q(-2,3),0,0,0,detail::Q(2,3),detail::Q(2,3),detail::Q(2,3),
    detail::Q(-2,3),detail::Q(-2,3),detail::Q(-2,3),0,0,0,detail::Q(2,3),detail::Q(2,3),detail::Q(2,3),
    detail::Q(-2,3),detail::Q(-2,3),detail::Q(-2,3),0,0,0,detail::Q(2,3),detail::Q(2,3),detail::Q(2,3),
    0,0,0,0,0};
static const sample_t kQuant1_2[32] = {
    detail::Q(-2,3),0,detail::Q(2,3),detail::Q(-2,3),0,detail::Q(2,3),detail::Q(-2,3),0,detail::Q(2,3),
    detail::Q(-2,3),0,detail::Q(2,3),detail::Q(-2,3),0,detail::Q(2,3),detail::Q(-2,3),0,detail::Q(2,3),
    detail::Q(-2,3),0,detail::Q(2,3),detail::Q(-2,3),0,detail::Q(2,3),detail::Q(-2,3),0,detail::Q(2,3),
    0,0,0,0,0};

// bap==2 : 3 mantissas per 7-bit codeword, levels {-4,-2,0,2,4}/5
#define AC3_Q2(n) detail::Q(n,5)
static const sample_t kQuant2_0[128] = {
    AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),
    AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),
    AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),
    0,0,0};
static const sample_t kQuant2_1[128] = {
    AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),0,0,0,0,0,AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),
    AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),0,0,0,0,0,AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),
    AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),0,0,0,0,0,AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),
    AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),0,0,0,0,0,AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),
    AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-4),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),AC3_Q2(-2),0,0,0,0,0,AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(2),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),AC3_Q2(4),
    0,0,0};
static const sample_t kQuant2_2[128] = {
    AC3_Q2(-4),AC3_Q2(-2),0,AC3_Q2(2),AC3_Q2(4),AC3_Q2(-4),AC3_Q2(-2),0,AC3_Q2(2),AC3_Q2(4),AC3_Q2(-4),AC3_Q2(-2),0,AC3_Q2(2),AC3_Q2(4),AC3_Q2(-4),AC3_Q2(-2),0,AC3_Q2(2),AC3_Q2(4),AC3_Q2(-4),AC3_Q2(-2),0,AC3_Q2(2),AC3_Q2(4),
    AC3_Q2(-4),AC3_Q2(-2),0,AC3_Q2(2),AC3_Q2(4),AC3_Q2(-4),AC3_Q2(-2),0,AC3_Q2(2),AC3_Q2(4),AC3_Q2(-4),AC3_Q2(-2),0,AC3_Q2(2),AC3_Q2(4),AC3_Q2(-4),AC3_Q2(-2),0,AC3_Q2(2),AC3_Q2(4),AC3_Q2(-4),AC3_Q2(-2),0,AC3_Q2(2),AC3_Q2(4),
    AC3_Q2(-4),AC3_Q2(-2),0,AC3_Q2(2),AC3_Q2(4),AC3_Q2(-4),AC3_Q2(-2),0,AC3_Q2(2),AC3_Q2(4),AC3_Q2(-4),AC3_Q2(-2),0,AC3_Q2(2),AC3_Q2(4),AC3_Q2(-4),AC3_Q2(-2),0,AC3_Q2(2),AC3_Q2(4),AC3_Q2(-4),AC3_Q2(-2),0,AC3_Q2(2),AC3_Q2(4),
    AC3_Q2(-4),AC3_Q2(-2),0,AC3_Q2(2),AC3_Q2(4),AC3_Q2(-4),AC3_Q2(-2),0,AC3_Q2(2),AC3_Q2(4),AC3_Q2(-4),AC3_Q2(-2),0,AC3_Q2(2),AC3_Q2(4),AC3_Q2(-4),AC3_Q2(-2),0,AC3_Q2(2),AC3_Q2(4),AC3_Q2(-4),AC3_Q2(-2),0,AC3_Q2(2),AC3_Q2(4),
    AC3_Q2(-4),AC3_Q2(-2),0,AC3_Q2(2),AC3_Q2(4),AC3_Q2(-4),AC3_Q2(-2),0,AC3_Q2(2),AC3_Q2(4),AC3_Q2(-4),AC3_Q2(-2),0,AC3_Q2(2),AC3_Q2(4),AC3_Q2(-4),AC3_Q2(-2),0,AC3_Q2(2),AC3_Q2(4),AC3_Q2(-4),AC3_Q2(-2),0,AC3_Q2(2),AC3_Q2(4),
    0,0,0};
#undef AC3_Q2

// bap==3 : single 3-bit symmetric quantizer, 7 levels
static const sample_t kQuant3[8] = {
    detail::Q(-6,7), detail::Q(-4,7), detail::Q(-2,7), 0,
    detail::Q(2,7),  detail::Q(4,7),  detail::Q(6,7),  0};

// bap==4 : 2 mantissas per 7-bit codeword, 11 levels each, range +-10/11
#define AC3_Q4(n) detail::Q(n,11)
static const sample_t kQuant4_0[128] = {
    AC3_Q4(-10),AC3_Q4(-10),AC3_Q4(-10),AC3_Q4(-10),AC3_Q4(-10),AC3_Q4(-10),AC3_Q4(-10),AC3_Q4(-10),AC3_Q4(-10),AC3_Q4(-10),AC3_Q4(-10),
    AC3_Q4(-8),AC3_Q4(-8),AC3_Q4(-8),AC3_Q4(-8),AC3_Q4(-8),AC3_Q4(-8),AC3_Q4(-8),AC3_Q4(-8),AC3_Q4(-8),AC3_Q4(-8),AC3_Q4(-8),
    AC3_Q4(-6),AC3_Q4(-6),AC3_Q4(-6),AC3_Q4(-6),AC3_Q4(-6),AC3_Q4(-6),AC3_Q4(-6),AC3_Q4(-6),AC3_Q4(-6),AC3_Q4(-6),AC3_Q4(-6),
    AC3_Q4(-4),AC3_Q4(-4),AC3_Q4(-4),AC3_Q4(-4),AC3_Q4(-4),AC3_Q4(-4),AC3_Q4(-4),AC3_Q4(-4),AC3_Q4(-4),AC3_Q4(-4),AC3_Q4(-4),
    AC3_Q4(-2),AC3_Q4(-2),AC3_Q4(-2),AC3_Q4(-2),AC3_Q4(-2),AC3_Q4(-2),AC3_Q4(-2),AC3_Q4(-2),AC3_Q4(-2),AC3_Q4(-2),AC3_Q4(-2),
    0,0,0,0,0,0,0,0,0,0,0,
    AC3_Q4(2),AC3_Q4(2),AC3_Q4(2),AC3_Q4(2),AC3_Q4(2),AC3_Q4(2),AC3_Q4(2),AC3_Q4(2),AC3_Q4(2),AC3_Q4(2),AC3_Q4(2),
    AC3_Q4(4),AC3_Q4(4),AC3_Q4(4),AC3_Q4(4),AC3_Q4(4),AC3_Q4(4),AC3_Q4(4),AC3_Q4(4),AC3_Q4(4),AC3_Q4(4),AC3_Q4(4),
    AC3_Q4(6),AC3_Q4(6),AC3_Q4(6),AC3_Q4(6),AC3_Q4(6),AC3_Q4(6),AC3_Q4(6),AC3_Q4(6),AC3_Q4(6),AC3_Q4(6),AC3_Q4(6),
    AC3_Q4(8),AC3_Q4(8),AC3_Q4(8),AC3_Q4(8),AC3_Q4(8),AC3_Q4(8),AC3_Q4(8),AC3_Q4(8),AC3_Q4(8),AC3_Q4(8),AC3_Q4(8),
    AC3_Q4(10),AC3_Q4(10),AC3_Q4(10),AC3_Q4(10),AC3_Q4(10),AC3_Q4(10),AC3_Q4(10),AC3_Q4(10),AC3_Q4(10),AC3_Q4(10),AC3_Q4(10),
    0,0,0,0,0,0,0};
static const sample_t kQuant4_1[128] = {
    AC3_Q4(-10),AC3_Q4(-8),AC3_Q4(-6),AC3_Q4(-4),AC3_Q4(-2),0,AC3_Q4(2),AC3_Q4(4),AC3_Q4(6),AC3_Q4(8),AC3_Q4(10),
    AC3_Q4(-10),AC3_Q4(-8),AC3_Q4(-6),AC3_Q4(-4),AC3_Q4(-2),0,AC3_Q4(2),AC3_Q4(4),AC3_Q4(6),AC3_Q4(8),AC3_Q4(10),
    AC3_Q4(-10),AC3_Q4(-8),AC3_Q4(-6),AC3_Q4(-4),AC3_Q4(-2),0,AC3_Q4(2),AC3_Q4(4),AC3_Q4(6),AC3_Q4(8),AC3_Q4(10),
    AC3_Q4(-10),AC3_Q4(-8),AC3_Q4(-6),AC3_Q4(-4),AC3_Q4(-2),0,AC3_Q4(2),AC3_Q4(4),AC3_Q4(6),AC3_Q4(8),AC3_Q4(10),
    AC3_Q4(-10),AC3_Q4(-8),AC3_Q4(-6),AC3_Q4(-4),AC3_Q4(-2),0,AC3_Q4(2),AC3_Q4(4),AC3_Q4(6),AC3_Q4(8),AC3_Q4(10),
    AC3_Q4(-10),AC3_Q4(-8),AC3_Q4(-6),AC3_Q4(-4),AC3_Q4(-2),0,AC3_Q4(2),AC3_Q4(4),AC3_Q4(6),AC3_Q4(8),AC3_Q4(10),
    AC3_Q4(-10),AC3_Q4(-8),AC3_Q4(-6),AC3_Q4(-4),AC3_Q4(-2),0,AC3_Q4(2),AC3_Q4(4),AC3_Q4(6),AC3_Q4(8),AC3_Q4(10),
    AC3_Q4(-10),AC3_Q4(-8),AC3_Q4(-6),AC3_Q4(-4),AC3_Q4(-2),0,AC3_Q4(2),AC3_Q4(4),AC3_Q4(6),AC3_Q4(8),AC3_Q4(10),
    AC3_Q4(-10),AC3_Q4(-8),AC3_Q4(-6),AC3_Q4(-4),AC3_Q4(-2),0,AC3_Q4(2),AC3_Q4(4),AC3_Q4(6),AC3_Q4(8),AC3_Q4(10),
    AC3_Q4(-10),AC3_Q4(-8),AC3_Q4(-6),AC3_Q4(-4),AC3_Q4(-2),0,AC3_Q4(2),AC3_Q4(4),AC3_Q4(6),AC3_Q4(8),AC3_Q4(10),
    AC3_Q4(-10),AC3_Q4(-8),AC3_Q4(-6),AC3_Q4(-4),AC3_Q4(-2),0,AC3_Q4(2),AC3_Q4(4),AC3_Q4(6),AC3_Q4(8),AC3_Q4(10),
    0,0,0,0,0,0,0};
#undef AC3_Q4

// bap==5 : single 4-bit symmetric quantizer, 15 levels
static const sample_t kQuant5[16] = {
    detail::Q(-14,15), detail::Q(-12,15), detail::Q(-10,15), detail::Q(-8,15),
    detail::Q(-6,15),  detail::Q(-4,15),  detail::Q(-2,15),  0,
    detail::Q(2,15),   detail::Q(4,15),   detail::Q(6,15),   detail::Q(8,15),
    detail::Q(10,15),  detail::Q(12,15),  detail::Q(14,15),  0};

/// Exponent scale factors: scale_factor[exp] = 2^(-exp-1) in the 16-bit
/// mantissa domain used above (A/52 7.4.3, dequantization Eq 7.9).
static const sample_t kExpScaleFactor[25] = {
    0.000030517578125f,      0.0000152587890625f,      0.00000762939453125f,
    0.000003814697265625f,   0.0000019073486328125f,   0.00000095367431640625f,
    0.000000476837158203125f,0.0000002384185791015625f,0.00000011920928955078125f,
    0.000000059604644775390625f,0.0000000298023223876953125f,0.00000001490116119384765625f,
    0.000000007450580596923828125f,0.0000000037252902984619140625f,0.00000000186264514923095703125f,
    0.000000000931322574615478515625f,0.0000000004656612873077392578125f,
    0.00000000023283064365386962890625f,0.000000000116415321826934814453125f,
    0.0000000000582076609134674072265625f,0.00000000002910383045673370361328125f,
    0.000000000014551915228366851806640625f,0.0000000000072759576141834259033203125f,
    0.00000000000363797880709171295166015625f,0.000000000001818989403545856475830078125f};

static const sample_t kLevel3Db = 0.7071067811865476f;
static const sample_t kLevel45Db = 0.5946035575013605f;
static const sample_t kLevel6Db = 0.5f;
static const sample_t kLevelPlus3Db = 1.4142135623730951f;
static const sample_t kLevelPlus6Db = 2.0f;

static const sample_t kCenterMixLevel[4] = {kLevel3Db, kLevel45Db, kLevel6Db, kLevel45Db};
static const sample_t kSurroundMixLevel[4] = {kLevel3Db, kLevel6Db, 0.0f, kLevel6Db};

// 16-bit pseudo-random dither LUT (A/52 7.4.4 dither for bap==0 bands).
static const uint16_t kDitherLut[256] = {
    0x0000,0xa011,0xe033,0x4022,0x6077,0xc066,0x8044,0x2055,
    0xc0ee,0x60ff,0x20dd,0x80cc,0xa099,0x0088,0x40aa,0xe0bb,
    0x21cd,0x81dc,0xc1fe,0x61ef,0x41ba,0xe1ab,0xa189,0x0198,
    0xe123,0x4132,0x0110,0xa101,0x8154,0x2145,0x6167,0xc176,
    0x439a,0xe38b,0xa3a9,0x03b8,0x23ed,0x83fc,0xc3de,0x63cf,
    0x8374,0x2365,0x6347,0xc356,0xe303,0x4312,0x0330,0xa321,
    0x6257,0xc246,0x8264,0x2275,0x0220,0xa231,0xe213,0x4202,
    0xa2b9,0x02a8,0x428a,0xe29b,0xc2ce,0x62df,0x22fd,0x82ec,
    0x8734,0x2725,0x6707,0xc716,0xe743,0x4752,0x0770,0xa761,
    0x47da,0xe7cb,0xa7e9,0x07f8,0x27ad,0x87bc,0xc79e,0x678f,
    0xa6f9,0x06e8,0x46ca,0xe6db,0xc68e,0x669f,0x26bd,0x86ac,
    0x6617,0xc606,0x8624,0x2635,0x0660,0xa671,0xe653,0x4642,
    0xc4ae,0x64bf,0x249d,0x848c,0xa4d9,0x04c8,0x44ea,0xe4fb,
    0x0440,0xa451,0xe473,0x4462,0x6437,0xc426,0x8404,0x2415,
    0xe563,0x4572,0x0550,0xa541,0x8514,0x2505,0x6527,0xc536,
    0x258d,0x859c,0xc5be,0x65af,0x45fa,0xe5eb,0xa5c9,0x05d8,
    0xae79,0x0e68,0x4e4a,0xee5b,0xce0e,0x6e1f,0x2e3d,0x8e2c,
    0x6e97,0xce86,0x8ea4,0x2eb5,0x0ee0,0xaef1,0xeed3,0x4ec2,
    0x8fb4,0x2fa5,0x6f87,0xcf96,0xefc3,0x4fd2,0x0ff0,0xafe1,
    0x4f5a,0xef4b,0xaf69,0x0f78,0x2f2d,0x8f3c,0xcf1e,0x6f0f,
    0xede3,0x4df2,0x0dd0,0xadc1,0x8d94,0x2d85,0x6da7,0xcdb6,
    0x2d0d,0x8d1c,0xcd3e,0x6d2f,0x4d7a,0xed6b,0xad49,0x0d58,
    0xcc2e,0x6c3f,0x2c1d,0x8c0c,0xac59,0x0c48,0x4c6a,0xec7b,
    0x0cc0,0xacd1,0xecf3,0x4ce2,0x6cb7,0xcca6,0x8c84,0x2c95,
    0x294d,0x895c,0xc97e,0x696f,0x493a,0xe92b,0xa909,0x0918,
    0xe9a3,0x49b2,0x0990,0xa981,0x89d4,0x29c5,0x69e7,0xc9f6,
    0x0880,0xa891,0xe8b3,0x48a2,0x68f7,0xc8e6,0x88c4,0x28d5,
    0xc86e,0x687f,0x285d,0x884c,0xa819,0x0808,0x482a,0xe83b,
    0x6ad7,0xcac6,0x8ae4,0x2af5,0x0aa0,0xaab1,0xea93,0x4a82,
    0xaa39,0x0a28,0x4a0a,0xea1b,0xca4e,0x6a5f,0x2a7d,0x8a6c,
    0x4b1a,0xeb0b,0xab29,0x0b38,0x2b6d,0x8b7c,0xcb5e,0x6b4f,
    0x8bf4,0x2be5,0x6bc7,0xcbd6,0xeb83,0x4b92,0x0bb0,0xaba1};

// ---------------------------------------------------------------------
// Bit-allocation (psychoacoustic model) tables, A/52 7.2.
// ---------------------------------------------------------------------

/// Absolute hearing threshold, per sample rate (fscod 0/1/2), 50 bands.
static const int16_t kHearingThreshold[3][50] = {
    {0x730,0x730,0x7c0,0x800,0x820,0x840,0x850,0x850,0x860,0x860,
     0x860,0x860,0x860,0x870,0x870,0x870,0x880,0x880,0x890,0x890,
     0x8a0,0x8a0,0x8b0,0x8b0,0x8c0,0x8c0,0x8d0,0x8e0,0x8f0,0x900,
     0x910,0x910,0x910,0x910,0x900,0x8f0,0x8c0,0x870,0x820,0x7e0,
     0x7a0,0x770,0x760,0x7a0,0x7c0,0x7c0,0x6e0,0x400,0x3c0,0x3c0},
    {0x710,0x710,0x7a0,0x7f0,0x820,0x830,0x840,0x850,0x850,0x860,
     0x860,0x860,0x860,0x860,0x870,0x870,0x870,0x880,0x880,0x880,
     0x890,0x890,0x8a0,0x8a0,0x8b0,0x8b0,0x8c0,0x8c0,0x8e0,0x8f0,
     0x900,0x910,0x910,0x910,0x910,0x900,0x8e0,0x8b0,0x870,0x820,
     0x7e0,0x7b0,0x760,0x770,0x7a0,0x7c0,0x780,0x5d0,0x3c0,0x3c0},
    {0x680,0x680,0x750,0x7b0,0x7e0,0x810,0x820,0x830,0x840,0x850,
     0x850,0x850,0x860,0x860,0x860,0x860,0x860,0x860,0x860,0x860,
     0x870,0x870,0x870,0x870,0x880,0x880,0x880,0x890,0x8a0,0x8b0,
     0x8c0,0x8d0,0x8e0,0x8f0,0x900,0x910,0x910,0x910,0x900,0x8f0,
     0x8d0,0x8b0,0x840,0x7f0,0x790,0x760,0x7a0,0x7c0,0x7b0,0x720}};

/// Upper mantissa index bound of each of the 50 threshold/band pairs
/// (the "band table"), used to walk exponents band-by-band above band 6.
static const int16_t kBandTab[30] = {
    21, 22,  23,  24,  25,  26,  27,  28,  31,  34,
    37, 40,  43,  46,  49,  55,  61,  67,  73,  79,
    85, 97, 109, 121, 133, 157, 181, 205, 229, 253};

/// Log-addition table for combining power spectral density values that
/// fall within the same band (A/52 7.2.3.1).
static const int8_t kLogAddTab[256] = {
    -64,-63,-62,-61,-60,-59,-58,-57,-56,-55,-54,-53,
    -52,-52,-51,-50,-49,-48,-47,-47,-46,-45,-44,-44,
    -43,-42,-41,-41,-40,-39,-38,-38,-37,-36,-36,-35,
    -35,-34,-33,-33,-32,-32,-31,-30,-30,-29,-29,-28,
    -28,-27,-27,-26,-26,-25,-25,-24,-24,-23,-23,-22,
    -22,-21,-21,-21,-20,-20,-19,-19,-19,-18,-18,-18,
    -17,-17,-17,-16,-16,-16,-15,-15,-15,-14,-14,-14,
    -13,-13,-13,-13,-12,-12,-12,-12,-11,-11,-11,-11,
    -10,-10,-10,-10,-10, -9, -9, -9, -9, -9, -8, -8,
     -8, -8, -8, -8, -7, -7, -7, -7, -7, -7, -6, -6,
     -6, -6, -6, -6, -6, -6, -5, -5, -5, -5, -5, -5,
     -5, -5, -4, -4, -4, -4, -4, -4, -4, -4, -4, -4,
     -4, -3, -3, -3, -3, -3, -3, -3, -3, -3, -3, -3,
     -3, -3, -3, -2, -2, -2, -2, -2, -2, -2, -2, -2,
     -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -1, -1,
     -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
     -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
     -1, -1, -1, -1, -1, -1,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0};

/// bap lookup table: given a clamped (mask + 4*exp) index in [-156,148],
/// returns the number of quantization levels code (bap, 0..15, or a
/// negative code selecting one of the vector-quantized tables above).
/// Index this array as kBapTab[156 + value].
static const int8_t kBapTab[305] = {
    16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,
    16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,
    16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,
    16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,
    16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,
    16,16,16,16,16,16,16,16,16,16,16,16,16,

    16,16,16,16,16,16,16,16,16,14,14,14,14,14,14,14,
    14,12,12,12,12,11,11,11,11,10,10,10,10, 9, 9, 9,
     9, 8, 8, 8, 8, 7, 7, 7, 7, 6, 6, 6, 6, 5, 5, 5,
     5, 4, 4,-3,-3, 3, 3, 3,-2,-2,-1,-1,-1,-1,-1, 0,

     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0};
static const int kBapTabZeroOffset = 156;

/// Coupling sub-band boundary table (indexed by cplbegf, 0..15), A/52 7.6.
static const uint8_t kCouplingBandTab[16] = {
    31, 35, 37, 39, 41, 42, 43, 44, 45, 45, 46, 46, 47, 47, 48, 48};

/// Rematrixing band boundaries (stereo mode only), A/52 7.5.4.
static const int kRematrixBand[4] = {25, 37, 61, 253};

}  // namespace ac3
