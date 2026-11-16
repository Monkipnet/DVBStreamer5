#pragma once
#include <stdint.h>
#include <time.h>

// Hold a recently successful PID after isolated negative replies. Even if
// several alternatives fail, rate-limit rotation to avoid an ECM storm on a
// shared reader; persistent denials can still try other PMT candidates.
static inline int dvb5_should_rotate_ecm_pid(
    unsigned int forced_pid, unsigned int candidate_count, int reply,
    unsigned int consecutive_denials, uint16_t current_pid,
    uint16_t last_success_pid, int has_valid_cw,
    time_t last_success_time, time_t last_rotation_time, time_t now)
{
    if (forced_pid || candidate_count < 2) return 0;
    // No more than one PID rotation every 15 seconds, including timeouts
    // and transport failures. A real recovery can still stay on its PID.
    if (last_rotation_time && (now < last_rotation_time ||
        now - last_rotation_time < 15)) return 0;
    // A Newcamd transport timeout says nothing about the selected ECM PID.
    // Switching away from a recently proven PID on every timeout adds reader
    // work during upstream overload and can lose a previously valid mapping.
    // Never suppress rotation for an unproven/stale PID: it still needs AUTO
    // discovery after the CW evidence window has expired.
    if (reply == -1 && current_pid && current_pid == last_success_pid &&
        has_valid_cw && last_success_time &&
        now >= last_success_time && now - last_success_time <= 30)
        return 0;
    if (reply != 0) return 1; // Stale/unproven PID: allow bounded AUTO retry.
    if (!current_pid || current_pid != last_success_pid ||
        !has_valid_cw || !last_success_time || now < last_success_time ||
        now - last_success_time > 30)
        return 1;
    return consecutive_denials >= 3;
}
