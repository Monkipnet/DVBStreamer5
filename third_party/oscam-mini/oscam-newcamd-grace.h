#ifndef DVBSTREAMER5_OSCAM_NEWCAMD_GRACE_H
#define DVBSTREAMER5_OSCAM_NEWCAMD_GRACE_H

// V563: bounded, opt-in Newcamd iCAM response grace.
// No CW/ECM transformation, replay, retry, caching or reader reconnect.
#include <stdint.h>
#include <stdlib.h>

// Fail-closed parsing: accept decimal 1..1500 only. unset/invalid => 0.
static inline uint16_t dvb5_newcamd_grace_parse(const char *value)
{
	if(!value || !*value) { return 0; }
	const char *p = value;
	for(; *p; ++p)
	{
		if(*p < '0' || *p > '9') { return 0; }
	}
	const unsigned long ms = strtoul(value, NULL, 10);
	return (ms >= 1 && ms <= 1500) ? (uint16_t)ms : 0;
}

static inline uint16_t dvb5_newcamd_grace_for_route(
		uint16_t caid, int inbound_newcamd, int outbound_newcamd,
		const char *value)
{
	if(caid != 0x098D || !inbound_newcamd || !outbound_newcamd)
		{ return 0; }
	return dvb5_newcamd_grace_parse(value);
}

static inline uint32_t dvb5_newcamd_effective_timeout_ms(
		uint32_t base_ms, uint16_t selected_grace_ms)
{
	// Defensive integer-overflow guard even for invalid external configs.
	if(base_ms > UINT32_MAX - selected_grace_ms) { return base_ms; }
	return base_ms + selected_grace_ms;
}

static inline uint32_t dvb5_newcamd_reader_expiry_seconds(uint32_t timeout_ms)
{
	// Retain OSCam's historic second-resolution safety margin, but base
	// it on the request's effective deadline so 5–6.5s replies survive.
	return (uint32_t)(((uint64_t)timeout_ms + 500ULL) / 1000ULL + 1ULL);
}

#endif
