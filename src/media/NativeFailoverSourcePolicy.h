#pragma once
// V551: a backup restart has its input URI replaced with backupInputUri.
// Comparing only those two URIs would incorrectly disable all failover
// state preservation and backup-only PID remapping during the transition.
#include <string>

namespace dvbstreamer5::media::network {

inline bool hasConfiguredFailover(
    const std::string& activeInputUri,
    const std::string& backupInputUri,
    bool runningBackup) {
    return !backupInputUri.empty() &&
        (activeInputUri != backupInputUri || runningBackup);
}

} // namespace dvbstreamer5::media::network
