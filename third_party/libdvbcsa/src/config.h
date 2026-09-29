#pragma once

/* TVStreamer5 vendored libdvbcsa configuration: Linux x86_64 + SSE2. */
#define STDC_HEADERS 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_MEMORY_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_ASSERT_H 1
#define DVBCSA_USE_SSE 1
#define DVBCSA_ENDIAN_LITTLE 1

#if defined(_WIN32)
#define HAVE_STRINGS_H 0
#else
#define HAVE_STRINGS_H 1
#define HAVE_POSIX_MEMALIGN 1
#endif
