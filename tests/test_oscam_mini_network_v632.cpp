#include "OscamMiniNetwork.h"
#include <cassert>
#include <iostream>
int main() {
    using namespace oscamnetwork;
    const std::set<std::string> addresses{"192.0.2.10", "127.0.0.1"};
    assert(validBind("127.0.0.1", addresses));
    assert(validBind("192.0.2.10", addresses));
    assert(!validBind("0.0.0.0", addresses));
    assert(!validBind("192.0.2.11", addresses));
    assert(!validBind("192.0.2.10\n", addresses));
    std::string out;
    assert(normalizeAllowed("127.0.0.1", "", out) && out.empty());
    assert(!normalizeAllowed("127.0.0.1", "192.0.2.11", out));
    assert(normalizeAllowed("192.0.2.10", "192.0.2.11, 192.0.2.11", out));
    assert(out == "192.0.2.10,192.0.2.11");
    for (const auto* bad : {"", "0.0.0.0", "255.255.255.255", "224.0.0.1", "192.0.2.0/24", "192.0.2.11,", "192.0.2.11\nkey=abc", ",192.0.2.11"})
        assert(!normalizeAllowed("192.0.2.10", bad, out));
    std::cout << "PASS managed OSCam-mini listener policy\n";
}
