#ifndef DVBSTREAMER5_OSCAM_ECM_LATENCY_H
#define DVBSTREAMER5_OSCAM_ECM_LATENCY_H

// V10.8.560: optional metadata-only latency trace for OSCam Newcamd readers.
// No ECM payloads, hashes, CWs, credentials, IPs, or network headers.
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

static inline int dvb5_ecm_latency_enabled(void)
{
	const char *value = getenv("DVBSTREAMER5_OSCAM_ECM_LATENCY");
	return value && value[0] == '1' && value[1] == '\0';
}

static inline uint64_t dvb5_ecm_latency_now_ms(void)
{
	struct timespec now;
	if(clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		{ return 0; }
	return (uint64_t)now.tv_sec * 1000ULL + (uint64_t)now.tv_nsec / 1000000ULL;
}

static inline uint64_t dvb5_ecm_latency_delta_ms(uint64_t started, uint64_t finished)
{
	return started && finished >= started ? finished - started : 0;
}

#endif
