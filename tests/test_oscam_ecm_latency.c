#include "../third_party/oscam-mini/oscam-ecm-latency.h"
#include <stdio.h>
#include <stdlib.h>

static int check(int condition, const char *label)
{
	if(!condition) { fprintf(stderr, "FAIL: %s\n", label); }
	return condition;
}

int main(void)
{
	int ok = 1;
	unsetenv("DVBSTREAMER5_OSCAM_ECM_LATENCY");
	ok &= check(!dvb5_ecm_latency_enabled(), "disabled by default");
	setenv("DVBSTREAMER5_OSCAM_ECM_LATENCY", "0", 1);
	ok &= check(!dvb5_ecm_latency_enabled(), "explicit 0 disables");
	setenv("DVBSTREAMER5_OSCAM_ECM_LATENCY", "true", 1);
	ok &= check(!dvb5_ecm_latency_enabled(), "unexpected value disables");
	setenv("DVBSTREAMER5_OSCAM_ECM_LATENCY", "1", 1);
	ok &= check(dvb5_ecm_latency_enabled(), "explicit 1 enables");
	const uint64_t now = dvb5_ecm_latency_now_ms();
	ok &= check(now > 0, "CLOCK_MONOTONIC works");
	ok &= check(dvb5_ecm_latency_delta_ms(100, 157) == 57,
		"correct delta");
	ok &= check(dvb5_ecm_latency_delta_ms(0, 150) == 0,
		"missing evidence cannot become fake latency");
	ok &= check(dvb5_ecm_latency_delta_ms(300, 150) == 0,
		"clock rollback must not underflow");
	unsetenv("DVBSTREAMER5_OSCAM_ECM_LATENCY");
	if(ok) puts("PASS OSCam ECM latency timing and opt-in");
	return ok ? 0 : 1;
}
