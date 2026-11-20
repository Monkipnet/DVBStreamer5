#pragma once
// V598: isolate a *stopped* non-archived HLS directory before removing it.
// The caller must have joined all producers and stopped TS, ABR and CMAF
// segmenters. Hold StreamManager's startingStreamsMutex_ across quarantine:
// a concurrent Start must never share the path being moved.
#include <atomic>
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
