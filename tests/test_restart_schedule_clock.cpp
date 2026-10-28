// Run: g++ -std=c++17 -Isrc tests/test_restart_schedule_clock.cpp -o /tmp/test_restart_schedule_clock
#include "RestartScheduleClock.h"

#include <cassert>
#include <ctime>
#include <set>
#include <iostream>

int main() {
    using namespace dvbstreamer5::schedule;
    // 2026-10-28 is Wednesday (tm_wday=3), server-local 17:00.
    std::tm wed{};
    wed.tm_year=126;
    wed.tm_yday=300;
    wed.tm_wday=3;
    wed.tm_hour=17;
    wed.tm_min=0;

    assert(matchesLocalTime(true, {1,2,3,4,5}, {5,17}, 0, wed));
    assert(!matchesLocalTime(false,{1,2,3,4,5}, {5,17}, 0, wed));
    assert(!matchesLocalTime(true, {1,2,4,5}, {5,17}, 0, wed));
    assert(!matchesLocalTime(true, {1,2,3,4,5}, {5}, 0, wed));
    assert(!matchesLocalTime(true, {1,2,3,4,5}, {5,17}, 30, wed));
    assert(!matchesLocalTime(true, {1,2,3,4,5}, {5,17}, -1, wed));
    assert(!matchesLocalTime(true, {1,2,3,4,5}, {5,17}, 60, wed));
    assert(minuteSlot(wed)==1020);
    const int wedDate=dateKey(wed);

    std::tm sun=wed;
    sun.tm_wday=0; // ISO Sunday=7
    assert(matchesLocalTime(true,{7},{17},0,sun));

    // A repeated wall clock minute on DST fall-back has the same date and
    // minute slot: a per-date set suppresses the second restart.
    std::set<int> fired;
    assert(fired.insert(minuteSlot(wed)).second);
    assert(!fired.insert(minuteSlot(wed)).second);
    wed.tm_yday++;
    assert(dateKey(wed)!=wedDate);
    fired.clear();
    assert(fired.insert(minuteSlot(wed)).second);

    std::cout << "Global restart schedule clock tests passed\n";
}
