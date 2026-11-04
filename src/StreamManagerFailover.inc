// V342: per-tile source failover supervisor and independent primary probes.
// Included from StreamManager.cpp, not a separate translation unit.

bool StreamManager::primaryStreamHealthy(const StreamConfig& primary) {
    const std::string uri = normalizeInputUri(primary.inputUri);
    const std::string lower = toLower(uri);
    const auto validTs = [](const std::uint8_t* data, std::size_t size) {
        if (!data || size < 188U) return false;
        for (std::size_t offset = 0; offset + 188U <= size; ++offset) {
            if (data[offset] != 0x47U) continue;
            if (size >= offset + 376U && data[offset + 188U] != 0x47U)
                continue;
            dvbstreamer5::media::mpegts::PacketInfo info;
            if (dvbstreamer5::media::mpegts::inspectPacket(
                    data + offset, 188U, info) &&
                info.pid != dvbstreamer5::media::mpegts::kNullPid &&
                info.hasPayload) return true;
        }
        return false;
    };

    // Checking a live connection is not enough: require real MPEG-TS data.
    if (startsWith(lower,"http://") || startsWith(lower,"https://")) {
        if (lower.find(".m3u8") == std::string::npos &&
            toLower(primary.inputMode) != "hls") {
            dvbstreamer5::http::RequestOptions options;
            options.connectTimeoutMs = 2000;
            options.readTimeoutMs = 2500;
            options.totalTimeoutMs = 3500;
            options.maxBodyBytes = 256 * 1024U;
            options.userAgent = primary.hlsUserAgent.empty()
                ? "Mozilla/5.0 DVBStreamer5" : primary.hlsUserAgent;
            if (primary.hlsAccessKeyMode == "header" &&
                !primary.hlsAccessKeyName.empty() &&
                !primary.hlsAccessKeyValue.empty() &&
                primary.hlsAccessKeyName.find_first_of("\r\n") == std::string::npos &&
                primary.hlsAccessKeyValue.find_first_of("\r\n") == std::string::npos)
                options.headers.emplace_back(
                    primary.hlsAccessKeyName, primary.hlsAccessKeyValue);
            std::string location = uri;
            if (primary.hlsAccessKeyMode == "query" &&
                !primary.hlsAccessKeyName.empty() &&
                !primary.hlsAccessKeyValue.empty()) {
                location += (location.find('?') == std::string::npos ? "?" : "&");
                location += dvbstreamer5::http::encodeQueryComponent(primary.hlsAccessKeyName);
                location += "=";
                location += dvbstreamer5::http::encodeQueryComponent(primary.hlsAccessKeyValue);
            }
            dvbstreamer5::http::Response response;
            std::string error;
            dvbstreamer5::media::mpegts::PacketFramer framer;
            std::vector<dvbstreamer5::media::mpegts::Packet> packets;
            unsigned validPackets = 0;
            (void)dvbstreamer5::http::get(
                location, options, response, error,
                [&](const std::uint8_t* bytes, std::size_t size) {
                    packets.clear();
                    framer.push(bytes,size,packets);
                    for (const auto& packet : packets) {
                        dvbstreamer5::media::mpegts::PacketInfo info;
                        if (dvbstreamer5::media::mpegts::inspectPacket(
                                packet.data(), packet.size(), info) &&
                            info.hasPayload &&
                            info.pid != dvbstreamer5::media::mpegts::kNullPid)
                            ++validPackets;
                    }
                    // Quit the probe as soon as enough real MPEG-TS packets
                    // arrive; do not pin the origin with a long-running client.
                    return validPackets < 10U;
                });
            return validPackets >= 10U && response.status >= 200 &&
                response.status < 300;
        }
    }

    auto probeStream = [&](auto& input, auto& config) {
        std::atomic<bool> haveMedia{false};
        std::string error;
        const bool started = input.start(
            config,
            [&](const std::uint8_t* data, std::size_t n) {
                if (validTs(data,n)) haveMedia.store(true, std::memory_order_release);
                return !haveMedia.load(std::memory_order_acquire);
            },
            [](const std::string&) {}, error);
        if (!started) return false;
        for (int i=0; i<30 && !haveMedia.load(std::memory_order_acquire); ++i) {
            if (failoverMonitorStop.load(std::memory_order_acquire)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        input.stop();
        return haveMedia.load(std::memory_order_acquire);
    };

    if (startsWith(lower,"srt://")) {
        dvbstreamer5::media::srt::EndpointConfig cfg;
        std::string error;
        if (!dvbstreamer5::media::srt::parseUri(uri,cfg,error)) return false;
        const auto mode = toLower(primary.inputMode);
        if (mode=="caller"||mode=="listener") cfg.mode=mode;
        cfg.latencyMs = primary.srtInputLatencyMs;
        if (!primary.srtInputPassphrase.empty()) cfg.passphrase=primary.srtInputPassphrase;
        if (!primary.srtInputStreamId.empty()) cfg.streamId=primary.srtInputStreamId;
        cfg.pbkeylen=primary.srtInputPbKeyLen;
        cfg.connectTimeoutMs=2000;
        cfg.ioTimeoutMs=500;
        dvbstreamer5::media::srt::NativeSrtInput input;
        return probeStream(input,cfg);
    }
    if (startsWith(lower,"rtsp://")) {
        dvbstreamer5::media::rtsp::InputConfig cfg;
        cfg.uri=uri;
        cfg.connectTimeoutMs=2000;
        cfg.ioTimeoutMs=500;
        dvbstreamer5::media::rtsp::NativeRtspInput input;
        return probeStream(input,cfg);
    }
    if (startsWith(lower,"rtmp://") || startsWith(lower,"rtmps://")) {
        dvbstreamer5::media::rtmp::EndpointConfig cfg;
        cfg.uri=uri;
        cfg.connectTimeoutMs=2000;
        cfg.ioTimeoutMs=500;
        dvbstreamer5::media::rtmp::NativeRtmpInput input;
        return probeStream(input,cfg);
    }
    if (toLower(primary.inputMode)=="hls" ||
        lower.find(".m3u8") != std::string::npos) {
        StreamConfig cfg=primary;
        dvbstreamer5::media::hls::NativeHlsInput input;
        return probeStream(input,cfg);
    }

    if (startsWith(lower,"udp://") || startsWith(lower,"rtp://")) {
        const auto schemeEnd=uri.find("://");
        const auto hostStart=schemeEnd+3U;
        const auto query=uri.find('?',hostStart);
        const auto end=query==std::string::npos?uri.size():query;
        const auto colon=uri.rfind(':',end);
        if (colon==std::string::npos || colon<hostStart) return false;
        std::string host=uri.substr(hostStart,colon-hostStart);
        if (!host.empty() && host.front()=='@') host.erase(host.begin());
        int port=0;
        try {port=std::stoi(uri.substr(colon+1,end-colon-1));}
        catch (...) {return false;}
        if(port<1||port>65535)return false;
        in_addr parsed{};
        bool multicast=false;
        if (::inet_pton(AF_INET,host.c_str(),&parsed)==1) {
            const auto addr=ntohl(parsed.s_addr);
            multicast=(addr & 0xf0000000U)==0xe0000000U;
        }
        dvbstreamer5::media::network::UdpSocket probe;
        std::string error;
        if(!probe.openReceiver("0.0.0.0",static_cast<std::uint16_t>(port),
                multicast?host:std::string(),cleanInterface(primary.inputInterfaceAddress),
                256*1024,error))return false;
        std::array<std::uint8_t,65536> packet{};
        for(int i=0;i<8;++i){
            if(failoverMonitorStop.load(std::memory_order_acquire))break;
            std::size_t n=0;
            if(!probe.receive(packet.data(),packet.size(),n,300,error))break;
            if(validTs(packet.data(),n)) return true;
        }
        return false;
    }
    // Never claim DVB frontend recovery based only on tuner lock or an
    // untested file path. An unsupported probe must not tear down a good backup.
    return false;
}

void StreamManager::monitorFailoverStreams() {
    constexpr auto kProbeInterval=std::chrono::seconds(5);
    constexpr auto kRetryInterval=std::chrono::seconds(10);
    while(!failoverMonitorStop.load(std::memory_order_acquire)){
        for(int i=0;i<10 && !failoverMonitorStop.load(std::memory_order_acquire);++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if(failoverMonitorStop.load(std::memory_order_acquire))break;

        struct Snapshot {std::string id;FailoverEntry entry;};
        std::vector<Snapshot> candidates;
        {
            std::lock_guard<std::mutex> lock(failoverMutex);
            for(const auto& [id,entry]:failoverEntries)
                if(!entry.switching)candidates.push_back({id,entry});
        }

        for(const auto& candidate:candidates){
            if(failoverMonitorStop.load(std::memory_order_acquire))break;
            bool active=false,stalled=false;
            {
                std::lock_guard<std::mutex> lock(managerMutex);
                const auto it=streams.find(candidate.id);
                if(it!=streams.end() && it->second){
                    active=it->second->active.load(std::memory_order_acquire);
                    stalled=it->second->inputStalled.load(std::memory_order_acquire);
                }
            }
            // A genuine source/relay crash can make the tile inactive before
            // the 5-second silence counter fires. The original registration
            // survives unexpected failure, but not a manual Stop.
            const auto now=std::chrono::steady_clock::now();
            if(!active && !candidate.entry.backupActive &&
                now-candidate.entry.firstStartedAt < std::chrono::seconds(5))
                continue;
            bool restore=false;
            if(candidate.entry.backupActive){
                if(now<candidate.entry.nextProbe)continue;
                const bool healthy=primaryStreamHealthy(candidate.entry.primary);
                std::lock_guard<std::mutex> lock(failoverMutex);
                const auto it=failoverEntries.find(candidate.id);
                if(it==failoverEntries.end()||it->second.switching||
                    !it->second.backupActive)continue;
                it->second.healthyChecks=healthy?it->second.healthyChecks+1:0;
                it->second.nextProbe=std::chrono::steady_clock::now()+kProbeInterval;
                if(it->second.healthyChecks<2)continue;
                restore=true;
            }else{
                if((active && !stalled) || now<candidate.entry.nextProbe)
                    continue;
            }

            // Mark the channel as transitioning before stopping its old
            // monitor. Manual stop never triggers an unrelated stream restart.
            {
                std::lock_guard<std::mutex> lock(failoverMutex);
                const auto it=failoverEntries.find(candidate.id);
                if(it==failoverEntries.end() || it->second.switching ||
                   it->second.backupActive!=candidate.entry.backupActive) continue;
                it->second.switching=true;
                it->second.backupActive=!restore;
            }

            StreamConfig next=candidate.entry.primary;
            if(!restore){
                next.inputUri=next.backupInputUri;
                if(toLower(next.backupInputType)=="file" &&
                   next.inputUri.find("://")==std::string::npos)
                    next.inputUri="file://"+next.inputUri;
                // Do not carry HLS/SRT input-mode forced by the primary URI
                // onto an unrelated replacement protocol.
                next.inputMode="auto";
            }
            std::cerr << "NATIVE INPUT FAILOVER"
                      << " stream_id=" << candidate.id
                      << " from=" << (restore?"backup":"primary")
                      << " to=" << (restore?"primary":"backup")
                      << " cause=" << (restore?"media_probe_healthy_twice":"input_no_media_5s")
                      << std::endl;
            // The transition itself is serialized against explicit user
            // Stop/Restart, not merely its registry update. If Stop won this
            // race, it removed the entry and we must never resurrect the tile.
            std::lock_guard<std::mutex> actionLock(failoverActionMutex);
            {
                std::lock_guard<std::mutex> lock(failoverMutex);
                const auto it=failoverEntries.find(candidate.id);
                if(it==failoverEntries.end() || !it->second.switching) continue;
            }
            stopStream(candidate.id);
            std::string error;
            const bool ok=startStream(next,&error);
            if(!ok){
                std::cerr << "NATIVE INPUT FAILOVER FAILED"
                          << " stream_id=" << candidate.id
                          << " direction=" << (restore?"primary":"backup")
                          << " reason=" << error << std::endl;
                // Recreate the previous source if the candidate failed.
                StreamConfig fallback=candidate.entry.primary;
                if(restore){
                    fallback.inputUri=fallback.backupInputUri;
                    if(toLower(fallback.backupInputType)=="file" &&
                       fallback.inputUri.find("://")==std::string::npos)
                        fallback.inputUri="file://"+fallback.inputUri;
                    fallback.inputMode="auto";
                }
                {
                    std::lock_guard<std::mutex> lock(failoverMutex);
                    const auto it=failoverEntries.find(candidate.id);
                    if(it!=failoverEntries.end())it->second.backupActive=restore;
                }
                std::string rollbackError;
                (void)startStream(fallback,&rollbackError);
            }
            {
                std::lock_guard<std::mutex> lock(failoverMutex);
                const auto it=failoverEntries.find(candidate.id);
                if(it!=failoverEntries.end()){
                    it->second.switching=false;
                    it->second.healthyChecks=0;
                    it->second.nextProbe=std::chrono::steady_clock::now()+
                        (ok?kProbeInterval:kRetryInterval);
                }
            }
        }
    }
}
