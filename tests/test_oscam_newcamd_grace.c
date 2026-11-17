#include "../third_party/oscam-mini/oscam-newcamd-grace.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <limits.h>

int main(void)
{
	assert(dvb5_newcamd_grace_parse(NULL) == 0);
	assert(dvb5_newcamd_grace_parse("") == 0);
	assert(dvb5_newcamd_grace_parse("0") == 0);
	assert(dvb5_newcamd_grace_parse("1") == 1);
	assert(dvb5_newcamd_grace_parse("1250") == 1250);
	assert(dvb5_newcamd_grace_parse("1500") == 1500);
	assert(dvb5_newcamd_grace_parse("1501") == 0);
	assert(dvb5_newcamd_grace_parse("-1") == 0);
	assert(dvb5_newcamd_grace_parse("1500ms") == 0);
	assert(dvb5_newcamd_grace_parse(" 1500") == 0);
	assert(dvb5_newcamd_grace_parse("999999999999999999999") == 0);
	assert(dvb5_newcamd_grace_for_route(0x098D, 1, 1, "1500") == 1500);
	assert(dvb5_newcamd_grace_for_route(0x098D, 0, 1, "1500") == 0);
	assert(dvb5_newcamd_grace_for_route(0x098D, 1, 0, "1500") == 0);
	assert(dvb5_newcamd_grace_for_route(0x0100, 1, 1, "1500") == 0);
	assert(dvb5_newcamd_effective_timeout_ms(5000, 0) == 5000);
	assert(dvb5_newcamd_effective_timeout_ms(5000, 1500) == 6500);
	assert(dvb5_newcamd_effective_timeout_ms(UINT32_MAX, 1500) == UINT32_MAX);
	assert(dvb5_newcamd_reader_expiry_seconds(5000) == 6);
	assert(dvb5_newcamd_reader_expiry_seconds(6500) == 8);
	assert(dvb5_newcamd_reader_expiry_seconds(6108) == 7);
	puts("PASS Newcamd iCAM bounded grace scope, parser, deadline and reader expiry");
	return 0;
}
