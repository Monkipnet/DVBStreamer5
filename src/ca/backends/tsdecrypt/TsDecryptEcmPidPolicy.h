#pragma once
#include <stdint.h>
#include <time.h>

// Keep an ECM PID after isolated negative CW replies if it has produced
// a valid CW recently. Persistent denials can still rotate to alternatives.
static inline int dvb5_should_rotate_ecm_pid(
    unsigned int forced_pid, unsigned int candidate_count, int reply,
    unsigned int consecutive_denials, uint16_t current_pid,
    uint16_t last_success_pid, int has_valid_cw,
    time_t last_success_time, time_t now)
{
    if (forced_pid || candidate_count < 2) return 0;
    if (reply != 0) return 1; // Preserve CAMD transport-error behavior.
    if (!current_pid || current_pid != last_success_pid ||
        !has_valid_cw || !last_success_time || now < last_success_time ||
        now - last_success_time > 30)
        return 1;
    return consecutive_denials >= 3;
}
