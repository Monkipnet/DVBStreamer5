#include "OscamMiniLogTail.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace dvbstreamer5::oscamlog;

std::string atTime(std::time_t t) {
    std::tm local{};
    assert(localtime_r(&t, &local));
    char buffer[16]{};
    assert(std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local));
    return buffer;
}

int main() {
    // Never follow service stdout to an arbitrary host path or process pipe.
    assert(!trustedLogPath("/etc/shadow"));
    assert(!trustedLogPath("/var/log/dvbstreamer5/../private/oscam-mini.log"));
    assert(!trustedLogPath("/var/log/dvbstreamer5/secret.log"));
    assert(serviceStdoutLogPath("not-a-pid").empty());
    assert(serviceStdoutLogPath("../../1").empty());

    const auto now = std::time(nullptr);
    const std::string oldLine = atTime(now - 95) +
        " (ecm) TEST (098D): found (100 ms) by ICAM\n";
    const std::string freshLine = atTime(now - 12) +
        " (ecm) TEST (098D): found (90 ms) by ICAM\n";
    const std::string recent = recentLines(oldLine + freshLine, now);
    assert(recent.find(freshLine) != std::string::npos);
    assert(recent.find(oldLine) == std::string::npos);
    assert(recentLines(oldLine, now).empty());

    // File tails must be bounded (do not load entire active OSCam logs) and
    // start at a line boundary when the ring is already large.
    const auto path = fs::temp_directory_path() /
        ("dvbstreamer5-oscam-tail-" + std::to_string(::getpid()) + ".log");
    {
        std::ofstream out(path);
        for (int i = 0; i < 10000; ++i)
            out << "012345678901234567890123456789\n";
        out << freshLine;
    }
    const auto text = readTail(path, 32000);
    assert(!text.empty());
    assert(text.size() <= 32000);
    assert(text.find(freshLine) != std::string::npos);
    assert(text.rfind("012345678901234567890123456789", 0) == 0);
    fs::remove(path);

    // A real process stdout redirected to a temp path is never trusted.
    // Production status resolves only systemd MainPID and root-owned log dir.
    assert(serviceStdoutLogPath(std::to_string(::getpid())).empty());

    std::puts("PASS OSCam-mini bounded file log fallback and recent ECM activity");
    return 0;
}
