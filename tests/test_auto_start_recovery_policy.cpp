#include "../src/AutoStartRecoveryPolicy.h"

#include <cassert>
#include <chrono>

int main() {
    using namespace dvbstreamer5::stream_recovery;
    using namespace std::chrono;
    assert(eligible(true, "online"));
    assert(!eligible(false, "online"));
    assert(!eligible(true, "ondemand"));
    assert(!eligible(true, "unknown"));
    assert(retryDelay(1) == seconds(10));
    assert(retryDelay(2) == seconds(20));
    assert(retryDelay(3) == seconds(40));
    assert(retryDelay(4) == seconds(60));
    assert(retryDelay(5) == seconds(120));
    assert(retryDelay(50) == seconds(120));
    return 0;
}
