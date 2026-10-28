#pragma once

// Testable server-local clock matching for the global stream restart scheduler.
#include <algorithm>
#include <ctime>
#include <vector>

namespace dvbstreamer5::schedule {

inline bool matchesLocalTime(bool enabled,
                             const std::vector<int>& weekdays,
                             const std::vector<int>& hours,
                             int minute, const std::tm& local) {
    if (!enabled || minute < 0 || minute > 59) return false;
    const int weekday = local.tm_wday == 0 ? 7 : local.tm_wday;
    return std::find(weekdays.begin(), weekdays.end(), weekday) != weekdays.end() &&
           std::find(hours.begin(), hours.end(), local.tm_hour) != hours.end() &&
           local.tm_min == minute;
}

inline int dateKey(const std::tm& local) {
    return (local.tm_year + 1900) * 400 + local.tm_yday;
}

inline int minuteSlot(const std::tm& local) {
    return local.tm_hour * 60 + local.tm_min;
}

} // namespace dvbstreamer5::schedule
