#pragma once
// V598: isolate a *stopped* non-archived HLS directory before removing it.
// The caller must have joined all producers and stopped TS, ABR and CMAF
// segmenters. Hold StreamManager's startingStreamsMutex_ across quarantine:
// a concurrent Start must never share the path being moved.
#include <atomic>
#include <chrono>
#include <fstream>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>

namespace dvbstreamer5::media::hls {

inline bool safeTemporaryHlsId(const std::string& id) {
    if (id.empty() || id.size() > 180 || id == "." || id == "..")
        return false;
    for (unsigned char c : id) {
        if (!((c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.'))
            return false;
    }
    return true;
}

// Restart-time scavenging is intentionally conservative: only an ended
// MPEG-TS HLS playlist whose contents have not changed for at least 24 hours
// can identify a crash/old-version tail. Unknown, hot or CMAF-only directories
// remain untouched. Check every file, not only the playlist's mtime.
inline bool isStaleCompletedTemporaryHls(
    const std::filesystem::path& dir,
    std::chrono::hours minimumAge = std::chrono::hours(24)) {
    namespace fs = std::filesystem;
    std::error_code error;
    const auto status = fs::symlink_status(dir,error);
    if (error || !fs::is_directory(status)) return false;
    const auto manifest = dir / "video.m3u8";
    const auto manifestStatus = fs::symlink_status(manifest,error);
    if (error || !fs::is_regular_file(manifestStatus)) return false;
    const auto size = fs::file_size(manifest,error);
    if (error || size == 0 || size > 65536U) return false;
    const auto cutoff = fs::file_time_type::clock::now() - minimumAge;
    if (fs::last_write_time(manifest,error) > cutoff || error) return false;
    std::ifstream in(manifest, std::ios::binary);
    if (!in) return false;
    std::string contents(static_cast<std::size_t>(size), '\\0');
    in.read(&contents[0],static_cast<std::streamsize>(size));
    if (!in || contents.find("#EXT-X-ENDLIST") == std::string::npos)
        return false;
    fs::recursive_directory_iterator it(dir,fs::directory_options::none,error);
    const fs::recursive_directory_iterator end;
    if (error) return false;
    std::size_t inspected = 0;
    for (; it != end; it.increment(error)) {
        if (error || ++inspected > 1024) return false;
        const auto entryStatus=it->symlink_status(error);
        if (error || fs::is_symlink(entryStatus)) return false;
        if (fs::is_regular_file(entryStatus) &&
            (fs::last_write_time(it->path(),error)>cutoff || error))
            return false;
    }
    return !error;
}

// Return an empty path when nothing needs cleanup. Never follow a symlink
// at the root or per-stream level. Archive paths are never passed here.
inline std::filesystem::path quarantineStoppedTemporaryHls(
    const std::filesystem::path& root, const std::string& id,
    std::error_code& error) {
    error.clear();
    if (!safeTemporaryHlsId(id)) {
        error = std::make_error_code(std::errc::invalid_argument);
        return {};
    }
    const auto rootStatus = std::filesystem::symlink_status(root, error);
    if (error == std::errc::no_such_file_or_directory) {
        error.clear();
        return {};
    }
    if (error) return {};
    if (!std::filesystem::is_directory(rootStatus)) {
        error = std::make_error_code(std::errc::not_a_directory);
        return {};
    }
    // Require a directory private to the service; reject world-writable
    // parent paths where another user could replace entries during rename.
    if ((rootStatus.permissions() & std::filesystem::perms::others_write) !=
        std::filesystem::perms::none) {
        error = std::make_error_code(std::errc::permission_denied);
        return {};
    }
    const auto dir = root / id;
    const auto dirStatus = std::filesystem::symlink_status(dir, error);
    if (error == std::errc::no_such_file_or_directory) {
        error.clear();
        return {};
    }
    if (error) return {};
    if (!std::filesystem::is_directory(dirStatus)) {
        error = std::make_error_code(std::errc::not_a_directory);
        return {};
    }
    static std::atomic<unsigned long long> sequence{0};
    for (int attempt = 0; attempt < 16; ++attempt) {
        const auto nonce = ++sequence;
        const auto trash = root /
            (".dvbstreamer5-hls-stopped-" +
             std::to_string(static_cast<unsigned long long>(::getpid())) +
             "-" + std::to_string(nonce));
        std::error_code existsError;
        if (std::filesystem::exists(trash, existsError)) continue;
        if (existsError) { error = existsError; return {}; }
        std::filesystem::rename(dir, trash, error);
        if (!error) return trash;
        return {};
    }
    error = std::make_error_code(std::errc::file_exists);
    return {};
}

inline std::uintmax_t removeQuarantinedTemporaryHls(
    const std::filesystem::path& trash, std::error_code& error) {
    error.clear();
    if (trash.empty() ||
        trash.filename().string().rfind(".dvbstreamer5-hls-stopped-",0) != 0) {
        error = std::make_error_code(std::errc::invalid_argument);
        return 0;
    }
    return std::filesystem::remove_all(trash,error);
}

} // namespace dvbstreamer5::media::hls
