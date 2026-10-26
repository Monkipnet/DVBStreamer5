#pragma once
/**
 * Detects, at compile time, whether the target has a hardware FPU. Used
 * by AC3Imdct.h to pick its internal arithmetic type: plain `float`
 * (unchanged/default) on FPU-equipped targets (ESP32/S3, desktop,
 * Cortex-M4F/M7, ...), or genuine fixed-point integer arithmetic on
 * FPU-less targets (the RP2040's Cortex-M0+, AVR, ESP32-S2/C3/C2/C6/H2,
 * ...), where native `float` arithmetic is emulated in software and is
 * much more expensive than integer math.
 *
 * Detection is per-architecture rather than per-board, since "ESP32" is
 * not one answer: the original ESP32 and S3 (Xtensa LX6/LX7) have a
 * hardware single-precision FPU, but S2 (Xtensa LX7) and the RISC-V
 * variants (C3/C2/C6/H2) do not.
 *
 * If this guesses wrong for your target, define AC3_HAS_FPU yourself (0
 * or 1) before including any header from this library.
 */

#ifndef AC3_HAS_FPU
#if defined(__riscv)
// RISC-V: __riscv_flen is defined (to 32 or 64) only when the F/D
// hardware floating-point extension is present - covers ESP32-C3/C2/C6/H2
// (no FPU) vs. any RISC-V target built with -march including F/D.
#if defined(__riscv_flen) && __riscv_flen > 0
#define AC3_HAS_FPU 1
#else
#define AC3_HAS_FPU 0
#endif
#elif defined(__XTENSA__) || defined(__xtensa__)
// Xtensa (ESP32 family): FPU presence is a core-configuration option,
// not something a predefined compiler macro states directly. The
// toolchain ships xtensa/config/core-isa.h with XCHAL_HAVE_FP describing
// this specific core - covers ESP32/S3 (has FPU) vs. ESP32-S2 (no FPU).
#if __has_include(<xtensa/config/core-isa.h>)
#include <xtensa/config/core-isa.h>
#if defined(XCHAL_HAVE_FP) && XCHAL_HAVE_FP
#define AC3_HAS_FPU 1
#else
#define AC3_HAS_FPU 0
#endif
#else
// Header unavailable (unusual build setup): assume FPU present, matching
// the original ESP32/S3, the more common case.
#define AC3_HAS_FPU 1
#endif
#elif defined(__SOFTFP__)
// GCC/Clang define this on ARM when the soft-float ABI is in use, i.e.
// no FPU instructions are emitted at all - the definitive signal.
#define AC3_HAS_FPU 0
#elif defined(__arm__) && !defined(__ARM_FP)
// An ARM target that doesn't report any FPU capability.
#define AC3_HAS_FPU 0
#elif defined(__AVR__)
// AVR-based boards never have a hardware FPU.
#define AC3_HAS_FPU 0
#else
// Desktop builds and other ARM cores with a hardware FPU (M4F/M7/A-class).
#define AC3_HAS_FPU 1
#endif
#endif
