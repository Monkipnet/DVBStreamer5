#include "../src/ca/backends/tsdecrypt/TsDecryptEcmPidPolicy.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    const time_t now = 1000;
    // No rotation history: preserve V471 stickiness semantics.
    assert(!dvb5_should_rotate_ecm_pid(0, 3, 0, 1, 6742, 6742, 1, 990, 0, now));
    assert(!dvb5_should_rotate_ecm_pid(0, 3, 0, 2, 6742, 6742, 1, 990, 0, now));
    assert(dvb5_should_rotate_ecm_pid(0, 3, 0, 3, 6742, 6742, 1, 990, 0, now));
    assert(dvb5_should_rotate_ecm_pid(0, 3, 0, 1, 6742, 7181, 1, 990, 0, now));
    assert(dvb5_should_rotate_ecm_pid(0, 3, 0, 1, 6742, 0, 0, 0, 0, now));
    assert(dvb5_should_rotate_ecm_pid(0, 3, 0, 1, 6742, 6742, 1, 969, 0, now));
    assert(dvb5_should_rotate_ecm_pid(0, 3, -1, 1, 6742, 6742, 1, 990, 0, now));
    // Rotations (including CAMD transport failures) are rate-limited.
    assert(!dvb5_should_rotate_ecm_pid(0, 3, 0, 99, 6742, 7181, 1, 990, 990, now));
    assert(!dvb5_should_rotate_ecm_pid(0, 3, -1, 99, 6742, 7181, 1, 990, 986, now));
    assert(dvb5_should_rotate_ecm_pid(0, 3, 0, 99, 6742, 7181, 1, 990, 985, now));
    assert(!dvb5_should_rotate_ecm_pid(0, 3, -1, 99, 6742, 7181, 1, 990, 1001, now));
    // User-provided -X is never overridden by automatic rotation.
    assert(!dvb5_should_rotate_ecm_pid(6742, 3, 0, 99, 6742, 6742, 1, 990, 0, now));
    assert(!dvb5_should_rotate_ecm_pid(0, 1, 0, 99, 6742, 6742, 1, 990, 0, now));
    puts("PASS TSDecrypt ECM PID rate-limit and sticky policy");
    return 0;
}
