#pragma once

#include "media/LinuxDvbInput.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dvbstreamer5::media::network {

class SharedDvbInputPool {
public:
    using DataCallback = std::function<bool(const std::uint8_t*, std::size_t)>;
    using FinishCallback = std::function<void(const std::string&)>;

    SharedDvbInputPool();
    ~SharedDvbInputPool();

    SharedDvbInputPool(const SharedDvbInputPool&) = delete;
    SharedDvbInputPool& operator=(const SharedDvbInputPool&) = delete;

    bool subscribe(
        const std::string& streamId,
        const LinuxDvbTuneConfig& config,
        DataCallback onData,
        FinishCallback onFinish,
        std::string& error);

    void unsubscribe(const std::string& streamId) noexcept;
    void stopAll() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dvbstreamer5::media::network

// Compatibility alias for StreamManager and older integrations that referenced
// SharedDvbInputPool directly from dvbstreamer5::media before the shared DVB
// implementation was moved under the network namespace.
namespace dvbstreamer5::media {
using SharedDvbInputPool = network::SharedDvbInputPool;
} // namespace dvbstreamer5::media
