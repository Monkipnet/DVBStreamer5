#pragma once

#include "media/UdpSocket.h"
#include "media/UdpTsMux.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dvbstreamer5::media::network {

class UdpOutputScheduler;
class UdpTsOutput {
public:
    struct Destination {
        UdpSocket* socket;
        std::size_t index;
        std::shared_ptr<std::atomic<std::uint64_t>> sentBytes;
    };
    struct Stats {
        std::uint64_t datagrams=0, retries=0, lateMaxUs=0, clockResets=0;
        std::uint64_t queueBytes=0, queuePeakBytes=0, sendBufferBytes=0;
        std::uint64_t pendingBytes=0, observerBytes=0, nullDatagrams=0;
        bool cbr=false;
    };
    using Observer=std::function<void(const std::uint8_t*,std::size_t)>;
    using Failure=std::function<void(const std::string&)>;
    using Sender=std::function<UdpSocket::SendResult(UdpSocket&,std::size_t,
        const std::uint8_t*,std::size_t,std::string&)>;
    UdpTsOutput(bool cbr, std::uint64_t encodedBitrate, std::string streamId,
        std::vector<Destination> destinations, std::atomic<std::uint64_t>* totalBytes,
        Observer observer, Failure failure, Sender sender = {});
    ~UdpTsOutput();
    UdpTsOutput(const UdpTsOutput&)=delete;
    UdpTsOutput& operator=(const UdpTsOutput&)=delete;
    bool enqueue(const std::vector<mpegts::Packet>& packets);
    void finish();
    void stop();
    std::size_t queuedPackets() const;
    bool drained() const;
    Stats stats() const;
    std::string lastError() const;
private:
    friend class UdpOutputScheduler;
    struct State;
    std::shared_ptr<State> state_;
    unsigned shard_=0;
};

} // namespace dvbstreamer5::media::network
