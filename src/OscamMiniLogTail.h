#pragma once

// Read only the OSCam-mini service's own file-backed stdout when journald is
// unavailable. Do not open arbitrary /proc/<pid>/fd targets or read whole logs.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

namespace dvbstreamer5::oscamlog {
namespace fs = std::filesystem;

inline bool trustedLogPath(const fs::path& input) {
    if (!input.is_absolute()) return false;
    const auto path = input.lexically_normal();
    const fs::path root("/var/log/dvbstreamer5");
    const auto relative = path.lexically_relative(root);
    if (relative.empty() || relative.is_absolute() ||
        *relative.begin() == "..") return false;

    const auto name = path.filename().string();
    const bool validName = name == "oscam-mini.log" ||
        (name.rfind("oscam-mini-", 0) == 0 && name.size() > 15 &&
         name.substr(name.size() - 4) == ".log");
    if (!validName) return false;

    // Canonicalize before opening so a symlink cannot point outside the log
    // directory. Systemd's stdout log path is managed by root.
    std::error_code ec;
    const auto canonical = fs::canonical(path, ec);
    if (ec) return false;
    const auto canonicalRelative = canonical.lexically_relative(root);
    return !canonicalRelative.empty() && !canonicalRelative.is_absolute() &&
        *canonicalRelative.begin() != "..";
}

inline std::string serviceStdoutLogPath(const std::string& pid) {
    if (pid.empty() || pid.size() > 20 ||
        !std::all_of(pid.begin(), pid.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
        })) return {};

    std::error_code ec;
    const auto path = fs::read_symlink(fs::path("/proc") / pid / "fd/1", ec);
    if (ec || !trustedLogPath(path)) return {};
    return path.string();
}

inline std::string readTail(const fs::path& path, std::size_t limit = 32000) {
    if (!limit) return {};
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const auto end = in.tellg();
    if (end <= 0) return {};
    const auto n = static_cast<std::streamoff>(
        std::min<std::uintmax_t>(static_cast<std::uintmax_t>(end), limit));
    const bool truncated = end > n;
    in.seekg(end - n);
    std::string value(static_cast<std::size_t>(n), '\0');
    in.read(value.data(), n);
    value.resize(static_cast<std::size_t>(in.gcount()));
    if (truncated) {
        const auto newline = value.find('\n');
        if (newline != std::string::npos) value.erase(0, newline + 1);
    }
    return value;
}

// OSCam writes timestamps as HH:MM:SS in local server time. Keep only events
// from the last 45 seconds; stale "found" events must never mark a card ready.
inline std::string recentLines(const std::string& log, std::time_t now,
                               int maxAgeSeconds = 45) {
    std::tm current{};
#ifdef __linux__
    if (!localtime_r(&now, &current)) return {};
#else
    const auto* currentPtr = std::localtime(&now);
    if (!currentPtr) return {};
    current = *currentPtr;
#endif
    std::string output;
    std::size_t begin = 0;
    while (begin < log.size()) {
        const auto end = log.find('\n', begin);
        const auto length = (end == std::string::npos ? log.size() : end) - begin;
        const auto line = log.substr(begin, length);
        int hh = -1, mm = -1, ss = -1;
        if (std::sscanf(line.c_str(), "%2d:%2d:%2d", &hh, &mm, &ss) == 3 &&
            hh >= 0 && hh <= 23 && mm >= 0 && mm <= 59 &&
            ss >= 0 && ss <= 59) {
            std::tm when = current;
            when.tm_hour = hh;
            when.tm_min = mm;
            when.tm_sec = ss;
            when.tm_isdst = -1;
            std::time_t event = std::mktime(&when);
            if (event > now) event -= 24 * 3600;  // Midnight crossing.
            if (event <= now && std::difftime(now, event) <= maxAgeSeconds) {
                output += line;
                output += '\n';
            }
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return output;
}

inline bool recentlyModified(const fs::path& path, int seconds = 45) {
    std::error_code ec;
    const auto written = fs::last_write_time(path, ec);
    if (ec) return false;
    const auto elapsed = fs::file_time_type::clock::now() - written;
    return elapsed >= std::chrono::seconds(-2) &&
        elapsed <= std::chrono::seconds(seconds);
}

struct Snapshot {
    bool fileBacked = false;
    std::string log;
    std::string recent;
};

inline Snapshot readServiceStdout(const std::string& pid) {
    const auto path = serviceStdoutLogPath(pid);
    if (path.empty()) return {};
    std::ifstream probe(path, std::ios::binary);
    if (!probe) return {};

    Snapshot result;
    result.fileBacked = true;
    result.log = readTail(path);
    if (recentlyModified(path)) {
        result.recent = recentLines(result.log, std::time(nullptr));
    }
    return result;
}

} // namespace dvbstreamer5::oscamlog
