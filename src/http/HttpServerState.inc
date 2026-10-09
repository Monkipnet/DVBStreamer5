std::string HttpServer::listInterfaces() {
    Json::Value root;
    auto interfaces = enumerateNetworkInterfaces();
    for (auto& iface : interfaces) {
        Json::Value item;
        item["name"] = iface.name;
        item["address"] = iface.address;
        root.append(item);
    }
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, root);
}

  std::string HttpServer::systemMetrics() {
    uint64_t cpuTotal = 0;
    uint64_t cpuIdle = 0;
    {
      std::ifstream statFile("/proc/stat");
      std::string label;
      uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
      if (statFile >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal && label == "cpu") {
        cpuIdle = idle + iowait;
        cpuTotal = user + nice + system + idle + iowait + irq + softirq + steal;
      }
    }

    uint64_t memoryTotal = 0;
    uint64_t memoryAvailable = 0;
    {
      std::ifstream meminfo("/proc/meminfo");
      std::string label;
      uint64_t value = 0;
      std::string unit;
      while (meminfo >> label >> value >> unit) {
        if (label == "MemTotal:") memoryTotal = value;
        if (label == "MemAvailable:") memoryAvailable = value;
      }
    }

    // Lightweight in-process resource telemetry.  This makes a future
    // TasksMax/OOM diagnosis visible in the web UI without requiring shell
    // scripts on a production headend.
    uint64_t processThreads = 0;
    uint64_t processRssKb = 0;
    uint64_t processAnonKb = 0;
    uint64_t processDataKb = 0;
    {
      std::ifstream status("/proc/self/status");
      std::string line;
      while (std::getline(status, line)) {
        if (line.rfind("Threads:", 0) == 0) {
          std::istringstream value(line.substr(8));
          value >> processThreads;
        } else if (line.rfind("VmRSS:", 0) == 0) {
          std::istringstream value(line.substr(6));
          value >> processRssKb;
        } else if (line.rfind("RssAnon:", 0) == 0) {
          std::istringstream value(line.substr(8));
          value >> processAnonKb;
        } else if (line.rfind("VmData:", 0) == 0) {
          std::istringstream value(line.substr(7));
          value >> processDataKb;
        }
      }
    }

    uint64_t mallocArenaBytes = 0;
    uint64_t mallocInUseBytes = 0;
    uint64_t mallocFreeBytes = 0;
    uint64_t mallocMmapBytes = 0;
#if defined(__GLIBC__)
    {
      const struct mallinfo2 info = mallinfo2();
      mallocArenaBytes = static_cast<uint64_t>(info.arena);
      mallocInUseBytes = static_cast<uint64_t>(info.uordblks);
      mallocFreeBytes = static_cast<uint64_t>(info.fordblks);
      mallocMmapBytes = static_cast<uint64_t>(info.hblkhd);
    }
#endif

    uint64_t qualitySampleCount = 0;
    uint64_t qualitySampleObjectBytes = 0;
    uint64_t qualityStringCapacityBytes = 0;
    uint64_t qualityStreamKeyCapacityBytes = 0;
    {
      std::lock_guard<std::mutex> qualityLock(qualityMutex);
      for (const auto& entry : qualitySamples) {
        qualitySampleCount += entry.second.size();
        qualitySampleObjectBytes +=
            static_cast<uint64_t>(entry.second.size()) * sizeof(QualitySample);
        qualityStreamKeyCapacityBytes += static_cast<uint64_t>(entry.first.capacity() + 1);
        for (const auto& sample : entry.second) {
          qualityStringCapacityBytes += static_cast<uint64_t>(sample.status.capacity() + 1);
          qualityStringCapacityBytes += static_cast<uint64_t>(sample.level.capacity() + 1);
          qualityStringCapacityBytes += static_cast<uint64_t>(sample.message.capacity() + 1);
        }
      }
    }

    uint64_t mptsQueueBytes = 0;
    const Json::Value mptsMemory = streamManager.mptsSnapshot();
    for (const auto& output : mptsMemory["outputs"]) {
      mptsQueueBytes += output.get("queue_bytes", Json::UInt64(0)).asUInt64();
    }
    uint64_t processFds = 0;
    {
      std::error_code ec;
      for (std::filesystem::directory_iterator it("/proc/self/fd", ec), end; !ec && it != end; it.increment(ec)) {
        ++processFds;
      }
    }
    uint64_t cgroupPidsCurrent = 0;
    std::string cgroupPidsMax;
    {
      std::ifstream current("/sys/fs/cgroup/pids.current");
      current >> cgroupPidsCurrent;
      std::ifstream maximum("/sys/fs/cgroup/pids.max");
      maximum >> cgroupPidsMax;
    }

    Json::Value root;
    std::lock_guard<std::mutex> lock(metricsMutex);
    double cpuPercent = 0.0;
    const auto now = std::chrono::steady_clock::now();
    if (previousCpuTotal > 0 && cpuTotal > previousCpuTotal && cpuIdle >= previousCpuIdle) {
      const uint64_t totalDelta = cpuTotal - previousCpuTotal;
      const uint64_t idleDelta = cpuIdle - previousCpuIdle;
      cpuPercent = 100.0 * static_cast<double>(totalDelta - std::min(totalDelta, idleDelta)) / totalDelta;
    }
    previousCpuTotal = cpuTotal;
    previousCpuIdle = cpuIdle;
    const double elapsedSeconds = previousMetricsSample.time_since_epoch().count() == 0
      ? 0.0
      : std::chrono::duration<double>(now - previousMetricsSample).count();
    previousMetricsSample = now;

    root["cpu_percent"] = cpuPercent;
    root["ram_percent"] = memoryTotal == 0
      ? 0.0
      : 100.0 * static_cast<double>(memoryTotal - std::min(memoryTotal, memoryAvailable)) / memoryTotal;
    root["process_threads"] = Json::UInt64(processThreads);
    root["process_fds"] = Json::UInt64(processFds);
    root["process_rss_mb"] = static_cast<double>(processRssKb) / 1024.0;
    root["process_anon_mb"] = static_cast<double>(processAnonKb) / 1024.0;
    root["process_data_mb"] = static_cast<double>(processDataKb) / 1024.0;
    root["malloc_arena_mb"] = static_cast<double>(mallocArenaBytes) / (1024.0 * 1024.0);
    root["malloc_inuse_mb"] = static_cast<double>(mallocInUseBytes) / (1024.0 * 1024.0);
    root["malloc_free_mb"] = static_cast<double>(mallocFreeBytes) / (1024.0 * 1024.0);
    root["malloc_mmap_mb"] = static_cast<double>(mallocMmapBytes) / (1024.0 * 1024.0);
    root["quality_samples"] = Json::UInt64(qualitySampleCount);
    root["quality_live_mb"] = static_cast<double>(
        qualitySampleObjectBytes + qualityStringCapacityBytes + qualityStreamKeyCapacityBytes) /
        (1024.0 * 1024.0);
    root["http_state_requests"] = Json::UInt64(
        httpStateRequestCount.load(std::memory_order_relaxed));
    root["http_state_last_kb"] = static_cast<double>(
        httpStateLastResponseBytes.load(std::memory_order_relaxed)) / 1024.0;
    root["http_state_total_mb"] = static_cast<double>(
        httpStateResponseBytes.load(std::memory_order_relaxed)) / (1024.0 * 1024.0);
    root["http_metrics_requests"] = Json::UInt64(
        httpMetricsRequestCount.load(std::memory_order_relaxed));
    root["mpts_queue_mb"] = static_cast<double>(mptsQueueBytes) / (1024.0 * 1024.0);
    root["cgroup_pids_current"] = Json::UInt64(cgroupPidsCurrent);
    root["cgroup_pids_max"] = cgroupPidsMax;

    if (lastMemoryDiagLog.time_since_epoch().count() == 0 ||
        now - lastMemoryDiagLog >= std::chrono::seconds(60)) {
      lastMemoryDiagLog = now;
      // 202.67: allocator/HTTP attribution. No media-path state is changed.
      const uint64_t stateRequests = httpStateRequestCount.load(std::memory_order_relaxed);
      const uint64_t stateResponseBytes = httpStateResponseBytes.load(std::memory_order_relaxed);
      const uint64_t stateLastResponseBytes = httpStateLastResponseBytes.load(std::memory_order_relaxed);
      const uint64_t metricsRequests = httpMetricsRequestCount.load(std::memory_order_relaxed);
      const char* stateBypassEnv = std::getenv("DVBSTREAMER5_STATE_API_DIAG_BYPASS");
      const bool stateBypassEnabled =
          stateBypassEnv && *stateBypassEnv && std::strcmp(stateBypassEnv, "0") != 0;
      uint64_t stateWorkerCount = 0;
      {
        std::lock_guard<std::mutex> diagLock(httpDiagMutex);
        stateWorkerCount = static_cast<uint64_t>(httpStateWorkerThreads.size());
      }

      // Optional one-minute trim probe. Disabled by default because malloc_trim()
      // is process-wide and can briefly contend with media allocator activity.
      // Enable only for diagnosis with DVBSTREAMER5_MEMORY_TRIM_DIAG=1.
      bool trimEnabled = false;
      int trimResult = -1;
      uint64_t postTrimRssKb = processRssKb;
      uint64_t postTrimAnonKb = processAnonKb;
      uint64_t postTrimMallocArenaBytes = mallocArenaBytes;
      uint64_t postTrimMallocInUseBytes = mallocInUseBytes;
      uint64_t postTrimMallocFreeBytes = mallocFreeBytes;
#if defined(__GLIBC__)
      const char* trimEnv = std::getenv("DVBSTREAMER5_MEMORY_TRIM_DIAG");
      trimEnabled = trimEnv && *trimEnv && std::strcmp(trimEnv, "0") != 0;
      if (trimEnabled) {
        trimResult = malloc_trim(0);
        const struct mallinfo2 postTrimInfo = mallinfo2();
        postTrimMallocArenaBytes = static_cast<uint64_t>(postTrimInfo.arena);
        postTrimMallocInUseBytes = static_cast<uint64_t>(postTrimInfo.uordblks);
        postTrimMallocFreeBytes = static_cast<uint64_t>(postTrimInfo.fordblks);
        std::ifstream postTrimStatus("/proc/self/status");
        std::string postTrimLine;
        while (std::getline(postTrimStatus, postTrimLine)) {
          if (postTrimLine.rfind("VmRSS:", 0) == 0) {
            std::istringstream value(postTrimLine.substr(6));
            value >> postTrimRssKb;
          } else if (postTrimLine.rfind("RssAnon:", 0) == 0) {
            std::istringstream value(postTrimLine.substr(8));
            value >> postTrimAnonKb;
          }
        }
      }
#endif

      // Collect live native queue occupancy only once per minute. Doing
      // this on every /api/system-metrics request would add avoidable pipeline
      // locking to the media server hot path.
      const Json::Value nativeQueueMemory = streamManager.queueMemorySnapshot();
      const uint64_t mediaQueueBytes = nativeQueueMemory.get("bytes", Json::UInt64(0)).asUInt64();
      const uint64_t mediaQueueCount = nativeQueueMemory.get("queue_count", Json::UInt64(0)).asUInt64();
      const uint64_t mediaQueueMaxBytes = nativeQueueMemory.get("max_queue_bytes", Json::UInt64(0)).asUInt64();
      const std::string mediaQueueMaxName = nativeQueueMemory.get("max_queue_name", "").asString();
      const std::string mediaQueueMaxStream = nativeQueueMemory.get("max_stream_id", "").asString();
      const uint64_t telemetryScratchUsedBytes =
          nativeQueueMemory.get("telemetry_scratch_used_bytes", Json::UInt64(0)).asUInt64();
      const uint64_t telemetryScratchCapacityBytes =
          nativeQueueMemory.get("telemetry_scratch_capacity_bytes", Json::UInt64(0)).asUInt64();
      const uint64_t telemetryScratchMaxCapacityBytes =
          nativeQueueMemory.get("telemetry_scratch_max_capacity_bytes", Json::UInt64(0)).asUInt64();
      const uint64_t telemetryRemainderCapacityBytes =
          nativeQueueMemory.get("telemetry_remainder_capacity_bytes", Json::UInt64(0)).asUInt64();
      const std::string telemetryScratchMaxName =
          nativeQueueMemory.get("telemetry_scratch_max_name", "").asString();
      const std::string telemetryScratchMaxStream =
          nativeQueueMemory.get("telemetry_scratch_max_stream", "").asString();
      const uint64_t managedPipelineCount = nativeQueueMemory.get("managed_pipeline_count", Json::UInt64(0)).asUInt64();
      const uint64_t mediaElementCount = nativeQueueMemory.get("media_element_count", Json::UInt64(0)).asUInt64();
      const uint64_t mediaEndpointCount = nativeQueueMemory.get("media_endpoint_count", Json::UInt64(0)).asUInt64();
      const uint64_t pipelineCreated = nativeQueueMemory.get("pipeline_created", Json::UInt64(0)).asUInt64();
      const uint64_t pipelineFinalized = nativeQueueMemory.get("pipeline_finalized", Json::UInt64(0)).asUInt64();
      const uint64_t sharedDvbBusDropped =
          nativeQueueMemory.get("shared_dvb_bus_dropped", Json::UInt64(0)).asUInt64();
      const uint64_t externalSrtBusDropped =
          nativeQueueMemory.get("external_srt_bus_dropped", Json::UInt64(0)).asUInt64();
      const uint64_t httpRelayForcedDisconnects =
          nativeQueueMemory.get("http_relay_forced_disconnects", Json::UInt64(0)).asUInt64();
      const uint64_t sourceOnlyRestarts = nativeQueueMemory.get("source_only_restarts", Json::UInt64(0)).asUInt64();
      const uint64_t fullPipelineRestarts = nativeQueueMemory.get("full_pipeline_restarts", Json::UInt64(0)).asUInt64();
      const uint64_t sourceReconnectStarted = nativeQueueMemory.get("source_reconnect_started", Json::UInt64(0)).asUInt64();
      const uint64_t sourceReconnectCompleted = nativeQueueMemory.get("source_reconnect_completed", Json::UInt64(0)).asUInt64();
      const uint64_t sourceReconnectSuppressed = nativeQueueMemory.get("source_reconnect_suppressed", Json::UInt64(0)).asUInt64();
      const uint64_t sourceReconnectTimeouts = nativeQueueMemory.get("source_reconnect_timeouts", Json::UInt64(0)).asUInt64();
      const uint64_t sourceReconnectFailed = nativeQueueMemory.get("source_reconnect_failed", Json::UInt64(0)).asUInt64();
      const uint64_t sourceReconnectInflight = nativeQueueMemory.get("source_reconnect_inflight", Json::UInt64(0)).asUInt64();
      const uint64_t autoCbrRaises = nativeQueueMemory.get("auto_cbr_raises", Json::UInt64(0)).asUInt64();
      const std::string autoCbrLastStream = nativeQueueMemory.get("auto_cbr_last_stream", "").asString();
      const uint64_t autoCbrLastMeasuredBitrate = nativeQueueMemory.get("auto_cbr_last_measured_bitrate", Json::UInt64(0)).asUInt64();
      const uint64_t autoCbrLastOldTarget = nativeQueueMemory.get("auto_cbr_last_old_target", Json::UInt64(0)).asUInt64();
      const uint64_t autoCbrLastNewTarget = nativeQueueMemory.get("auto_cbr_last_new_target", Json::UInt64(0)).asUInt64();
      const uint64_t autoCbrConfigSaves = nativeQueueMemory.get("auto_cbr_config_saves", Json::UInt64(0)).asUInt64();
      const uint64_t autoCbrConfigSaveFailed = nativeQueueMemory.get("auto_cbr_config_save_failed", Json::UInt64(0)).asUInt64();
      const uint64_t streamStoppingCount = nativeQueueMemory.get("stream_stopping_count", Json::UInt64(0)).asUInt64();
      const uint64_t streamStartingCount = nativeQueueMemory.get("stream_starting_count", Json::UInt64(0)).asUInt64();
      const uint64_t streamStartWaits = nativeQueueMemory.get("stream_start_waits", Json::UInt64(0)).asUInt64();
      const uint64_t streamStartWaitTimeouts = nativeQueueMemory.get("stream_start_wait_timeouts", Json::UInt64(0)).asUInt64();
      const uint64_t streamFinalizeTimeouts = nativeQueueMemory.get("stream_finalize_timeouts", Json::UInt64(0)).asUInt64();
      const uint64_t streamForcedRetires = nativeQueueMemory.get("stream_forced_retires", Json::UInt64(0)).asUInt64();
      const uint64_t teardownRestartRequests = nativeQueueMemory.get("teardown_restart_requests", Json::UInt64(0)).asUInt64();
      const bool serviceRestartPending = nativeQueueMemory.get("service_restart_pending", false).asBool();
      const uint64_t hlsRebuilds = nativeQueueMemory.get("hls_rebuilds", Json::UInt64(0)).asUInt64();
      const uint64_t hlsRecoverySuppressed = nativeQueueMemory.get("hls_recovery_suppressed", Json::UInt64(0)).asUInt64();
      const uint64_t remapCreated = nativeQueueMemory.get("remap_created", Json::UInt64(0)).asUInt64();
      const uint64_t remapDestroyed = nativeQueueMemory.get("remap_destroyed", Json::UInt64(0)).asUInt64();
      const auto stableUdpMemory = StableUdpOutput::memoryStats();
      std::cerr << "MEMORY DIAG 202.70: rss_mb=" << (static_cast<double>(processRssKb) / 1024.0)
                << " anon_mb=" << (static_cast<double>(processAnonKb) / 1024.0)
                << " data_mb=" << (static_cast<double>(processDataKb) / 1024.0)
                << " malloc_inuse_mb=" << (static_cast<double>(mallocInUseBytes) / (1024.0 * 1024.0))
                << " malloc_free_mb=" << (static_cast<double>(mallocFreeBytes) / (1024.0 * 1024.0))
                << " malloc_arena_mb=" << (static_cast<double>(mallocArenaBytes) / (1024.0 * 1024.0))
                << " malloc_mmap_mb=" << (static_cast<double>(mallocMmapBytes) / (1024.0 * 1024.0))
                << " quality_samples=" << qualitySampleCount
                << " quality_live_mb="
                << (static_cast<double>(qualitySampleObjectBytes + qualityStringCapacityBytes +
                                        qualityStreamKeyCapacityBytes) / (1024.0 * 1024.0))
                << " http_state_calls=" << stateRequests
                << " http_state_workers=" << stateWorkerCount
                << " http_state_last_kb=" << (static_cast<double>(stateLastResponseBytes) / 1024.0)
                << " http_state_total_mb=" << (static_cast<double>(stateResponseBytes) / (1024.0 * 1024.0))
                << " http_metrics_calls=" << metricsRequests
                << " state_bypass=" << (stateBypassEnabled ? 1 : 0)
                << " trim_enabled=" << (trimEnabled ? 1 : 0)
                << " trim_result=" << trimResult
                << " post_trim_rss_mb=" << (static_cast<double>(postTrimRssKb) / 1024.0)
                << " post_trim_anon_mb=" << (static_cast<double>(postTrimAnonKb) / 1024.0)
                << " post_trim_malloc_inuse_mb="
                << (static_cast<double>(postTrimMallocInUseBytes) / (1024.0 * 1024.0))
                << " post_trim_malloc_free_mb="
                << (static_cast<double>(postTrimMallocFreeBytes) / (1024.0 * 1024.0))
                << " post_trim_malloc_arena_mb="
                << (static_cast<double>(postTrimMallocArenaBytes) / (1024.0 * 1024.0))
                << " mpts_queue_mb=" << (static_cast<double>(mptsQueueBytes) / (1024.0 * 1024.0))
                << " media_queue_mb=" << (static_cast<double>(mediaQueueBytes) / (1024.0 * 1024.0))
                << " media_queue_count=" << mediaQueueCount
                << " media_queue_max_mb=" << (static_cast<double>(mediaQueueMaxBytes) / (1024.0 * 1024.0))
                << " media_queue_max=" << (mediaQueueMaxStream.empty() ? "-" : mediaQueueMaxStream)
                << ":" << (mediaQueueMaxName.empty() ? "-" : mediaQueueMaxName)
                << " stable_ring_cap_mb="
                << (static_cast<double>(stableUdpMemory.packetRingCapacityBytes) / (1024.0 * 1024.0))
                << " stable_chunk_mb="
                << (static_cast<double>(stableUdpMemory.queuedChunkPayloadBytes) / (1024.0 * 1024.0))
                << " stable_chunk_cap_mb="
                << (static_cast<double>(stableUdpMemory.queuedChunkCapacityBytes) / (1024.0 * 1024.0))
                << " stable_chunk_count=" << stableUdpMemory.queuedChunkCount
                << " stable_chunk_max_mb="
                << (static_cast<double>(stableUdpMemory.queuedChunkMaxCapacityBytes) / (1024.0 * 1024.0))
                << " stable_remainder_cap_mb="
                << (static_cast<double>(stableUdpMemory.inputRemainderCapacityBytes) / (1024.0 * 1024.0))
                << " stable_cleanstart_cap_mb="
                << (static_cast<double>(stableUdpMemory.cleanStartCapacityBytes) / (1024.0 * 1024.0))
                << " telemetry_scratch_used_mb="
                << (static_cast<double>(telemetryScratchUsedBytes) / (1024.0 * 1024.0))
                << " telemetry_scratch_cap_mb="
                << (static_cast<double>(telemetryScratchCapacityBytes) / (1024.0 * 1024.0))
                << " telemetry_scratch_max_mb="
                << (static_cast<double>(telemetryScratchMaxCapacityBytes) / (1024.0 * 1024.0))
                << " telemetry_scratch_max="
                << (telemetryScratchMaxStream.empty() ? "-" : telemetryScratchMaxStream)
                << ":" << (telemetryScratchMaxName.empty() ? "-" : telemetryScratchMaxName)
                << " telemetry_remainder_cap_mb="
                << (static_cast<double>(telemetryRemainderCapacityBytes) / (1024.0 * 1024.0))
                << " stable_senders=" << stableUdpMemory.senderCount
                << " stable_created=" << stableUdpMemory.senderCreated
                << " stable_destroyed=" << stableUdpMemory.senderDestroyed
                << " pipelines_live=" << managedPipelineCount
                << " pipelines_created=" << pipelineCreated
                << " pipelines_finalized=" << pipelineFinalized
                << " shared_dvb_bus_dropped=" << sharedDvbBusDropped
                << " external_srt_bus_dropped=" << externalSrtBusDropped
                << " http_relay_forced_disconnects=" << httpRelayForcedDisconnects
                << " pipelines_unaccounted="
                << ((pipelineCreated >= pipelineFinalized + managedPipelineCount)
                        ? pipelineCreated - pipelineFinalized - managedPipelineCount : 0)
                << " media_elements=" << mediaElementCount
                << " media_endpoints=" << mediaEndpointCount
                << " remap_live=" << (remapCreated >= remapDestroyed ? remapCreated - remapDestroyed : 0)
                << " remap_created=" << remapCreated
                << " remap_destroyed=" << remapDestroyed
                << " source_restarts=" << sourceOnlyRestarts
                << " full_restarts=" << fullPipelineRestarts
                << " source_reconnect_started=" << sourceReconnectStarted
                << " source_reconnect_completed=" << sourceReconnectCompleted
                << " source_reconnect_suppressed=" << sourceReconnectSuppressed
                << " source_reconnect_timeouts=" << sourceReconnectTimeouts
                << " source_reconnect_failed=" << sourceReconnectFailed
                << " source_reconnect_inflight=" << sourceReconnectInflight
                << " auto_cbr_raises=" << autoCbrRaises
                << " auto_cbr_last_stream=" << (autoCbrLastStream.empty() ? "-" : autoCbrLastStream)
                << " auto_cbr_last_measured_kbps=" << (autoCbrLastMeasuredBitrate / 1000ULL)
                << " auto_cbr_last_old_target_kbps=" << (autoCbrLastOldTarget / 1000ULL)
                << " auto_cbr_last_new_target_kbps=" << (autoCbrLastNewTarget / 1000ULL)
                << " auto_cbr_config_saves=" << autoCbrConfigSaves
                << " auto_cbr_config_save_failed=" << autoCbrConfigSaveFailed
                << " stream_stopping=" << streamStoppingCount
                << " stream_starting=" << streamStartingCount
                << " stream_start_waits=" << streamStartWaits
                << " stream_start_wait_timeouts=" << streamStartWaitTimeouts
                << " stream_finalize_timeouts=" << streamFinalizeTimeouts
                << " stream_forced_retires=" << streamForcedRetires
                << " teardown_restart_requests=" << teardownRestartRequests
                << " service_restart_pending=" << (serviceRestartPending ? 1 : 0)
                << " hls_rebuilds=" << hlsRebuilds
                << " hls_recovery_suppressed=" << hlsRecoverySuppressed
                << " threads=" << processThreads
                << " fds=" << processFds
                << std::endl;
    }

    Json::Value interfaces(Json::arrayValue);
    const auto interfaceAddresses = enumerateNetworkInterfaces();
    std::ifstream netdev("/proc/net/dev");
    std::string line;
    while (std::getline(netdev, line)) {
      const auto colon = line.find(':');
      if (colon == std::string::npos) continue;
      std::string name = line.substr(0, colon);
      name.erase(0, name.find_first_not_of(" \t"));
      name.erase(name.find_last_not_of(" \t") + 1);
      if (name == "lo" || name.empty()) continue;
      std::istringstream values(line.substr(colon + 1));
      uint64_t rxBytes = 0, txBytes = 0;
      if (!(values >> rxBytes)) continue;
      for (int i = 0; i < 7; ++i) {
        uint64_t ignored = 0;
        values >> ignored;
      }
      if (!(values >> txBytes)) continue;
      double rxMbps = 0.0;
      double txMbps = 0.0;
      const auto previous = previousNetworkBytes.find(name);
      if (elapsedSeconds > 0.0 && previous != previousNetworkBytes.end()) {
        rxMbps = static_cast<double>(rxBytes - std::min(rxBytes, previous->second.first)) * 8.0 / elapsedSeconds / 1000000.0;
        txMbps = static_cast<double>(txBytes - std::min(txBytes, previous->second.second)) * 8.0 / elapsedSeconds / 1000000.0;
      }
      previousNetworkBytes[name] = {rxBytes, txBytes};
      Json::Value item;
      item["name"] = name;
      const auto address = std::find_if(interfaceAddresses.begin(), interfaceAddresses.end(), [&name](const NetworkInterface& iface) {
        return iface.name == name;
      });
      item["address"] = address == interfaceAddresses.end() ? "" : address->address;
      item["udp_input_filter"] = address != interfaceAddresses.end() &&
          dvbstreamer5::network::UdpInputFirewall::enabledFor(
              configManager.config, address->address);
      item["rx_mbps"] = rxMbps;
      item["tx_mbps"] = txMbps;
      interfaces.append(item);
    }
    root["interfaces"] = interfaces;
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, root);
  }

std::string HttpServer::currentState() {
    Json::Value root;
    root["login"] = configManager.config.login;
    root["server_name"] = configManager.config.serverName;
    root["http_port"] = configManager.config.httpPort;
    root["language"] = configManager.config.language;
    root["telegram_token"] = configManager.config.telegramToken;
    root["telegram_chat_id"] = configManager.config.telegramChatId;
    root["srt_vps_vds_optimization"] = configManager.config.srtVpsVdsOptimization;
    Json::Value camClients(Json::arrayValue);
    for (const auto& client : configManager.config.camClients) camClients.append(client.toJson());
    root["cam_clients"] = camClients;
    root["program_version"] = kProgramVersion;
    Json::Value interfaces(Json::arrayValue);
    for (const auto& iface : enumerateNetworkInterfaces()) {
        Json::Value item;
        item["name"] = iface.name;
        item["address"] = iface.address;
        interfaces.append(item);
    }
    root["interfaces"] = interfaces;
    Json::Value inputInterfaces(Json::arrayValue);
    for (const auto& iface : enumerateNetworkInterfaces(true)) {
        Json::Value item;
        item["name"] = iface.name;
        item["address"] = iface.address;
        inputInterfaces.append(item);
    }
    root["input_interfaces"] = inputInterfaces;
    root["stream_count"] = Json::UInt(configManager.config.streams.size());
    root["active_count"] = Json::UInt(streamManager.activeStreams().size());
    root["ca_manager"] = CardManager::instance().snapshot();
    root["subscriber_filtering_enabled"] = configManager.subscribers.filteringEnabled;
    Json::Value blockedIps(Json::arrayValue);
    for (const auto& ip : configManager.subscribers.blockedIps) blockedIps.append(ip);
    root["blocked_ips"] = blockedIps;
    static const auto transcoderCapabilities = TranscoderModule::inspectCapabilities();
    Json::Value transcoder;
    transcoder["available"] = transcoderCapabilities.available;
    transcoder["video_encoder"] = transcoderCapabilities.videoEncoder;
    transcoder["x264_available"] = transcoderCapabilities.x264Available;
    transcoder["nvenc_available"] = transcoderCapabilities.nvencAvailable;
    transcoder["intel_available"] = transcoderCapabilities.intelAvailable;
    transcoder["intel_encoder"] = transcoderCapabilities.intelEncoder;
    transcoder["hevc_video_encoder"] = transcoderCapabilities.hevcVideoEncoder;
    transcoder["x265_available"] = transcoderCapabilities.x265Available;
    transcoder["nvenc_hevc_available"] = transcoderCapabilities.nvencHevcAvailable;
    transcoder["intel_hevc_available"] = transcoderCapabilities.intelHevcAvailable;
    transcoder["intel_hevc_encoder"] = transcoderCapabilities.intelHevcEncoder;
    transcoder["audio_encoder"] = transcoderCapabilities.audioEncoder;
    transcoder["aac_encoder"] = transcoderCapabilities.aacEncoder;
    transcoder["mp3_encoder"] = transcoderCapabilities.mp3Encoder;
    transcoder["mp2_encoder_available"] = transcoderCapabilities.mp2EncoderAvailable;
    transcoder["deinterlace"] = transcoderCapabilities.deinterlaceAvailable;
    transcoder["message"] = transcoderCapabilities.message;
    Json::Value missing(Json::arrayValue);
    for (const auto& element : transcoderCapabilities.missingElements) missing.append(element);
    transcoder["missing_elements"] = missing;
    root["transcoder"] = transcoder;
    const auto activeStreamSessions = streamManager.activeStreamSessions();
    std::map<std::string, size_t> streamNumbers;
    std::map<std::string, std::string> streamNames;
    std::set<std::string> configuredStreamIds;
    for (size_t index = 0; index < configManager.config.streams.size(); ++index) {
      const auto& stream = configManager.config.streams[index];
      streamNumbers[stream.id] = index + 1;
      streamNames[stream.id] = stream.name.empty() ? stream.id : stream.name;
      configuredStreamIds.insert(stream.id);
    }
    // Also purge history/UI state for IDs removed by a full config update or
    // rename, not only by the dedicated delete endpoint.
    {
      std::lock_guard<std::mutex> lock(qualityMutex);
      for (auto it = qualitySamples.begin(); it != qualitySamples.end();) {
        if (!configuredStreamIds.count(it->first)) {
          qualityLastCompaction.erase(it->first);
          it = qualitySamples.erase(it);
        } else {
          ++it;
        }
      }
      for (auto it = qualityLastCompaction.begin(); it != qualityLastCompaction.end();) {
        if (!configuredStreamIds.count(it->first)) it = qualityLastCompaction.erase(it);
        else ++it;
      }
    }
    {
      std::lock_guard<std::mutex> lock(caDecodeUiMutex());
      auto& memory = caDecodeUiMemory();
      for (auto it = memory.begin(); it != memory.end();) {
        if (!configuredStreamIds.count(it->first)) it = memory.erase(it);
        else ++it;
      }
    }
    auto sessionToJson = [&](const ActiveStreamSession& session) {
      Json::Value item;
      item["client_ip"] = session.clientIp;
      item["stream_id"] = session.streamId;
      item["stream_number"] = Json::UInt64(streamNumbers[session.streamId]);
      item["stream_name"] = streamNames.count(session.streamId) ? streamNames[session.streamId] : session.streamId;
      item["protocol"] = session.protocol == "mpegts" ? "http" : session.protocol;
      item["connections"] = Json::UInt64(session.connections);
      return item;
    };

    auto subscriberStreamClass = [&](const SubscriberConfig& subscriber) -> std::string {
      bool hasHd = false;
      bool hasSd = false;
      for (const auto& streamId : subscriber.streamIds) {
        const auto found = streamNames.find(streamId);
        if (found == streamNames.end()) continue;
        const std::string channelName = toLower(found->second);
        if (channelName.find("hd") != std::string::npos) hasHd = true;
        if (channelName.find("sd") != std::string::npos) hasSd = true;
      }
      if (hasHd && hasSd) return "HD+SD";
      if (hasHd) return "HD";
      if (hasSd) return "SD";
      return "OTHER";
    };
    auto subscriberStreamRank = [](const std::string& streamClass) {
      if (streamClass == "HD") return 0;
      if (streamClass == "HD+SD") return 1;
      if (streamClass == "SD") return 2;
      return 3;
    };

    std::set<std::string> registeredIps;
    for (const auto& subscriber : configManager.subscribers.subscribers) {
      const std::string primaryIp = normalizeIpAddress(subscriber.primaryIp);
      const std::string backupIp = normalizeIpAddress(subscriber.backupIp);
      if (!primaryIp.empty()) registeredIps.insert(primaryIp);
      if (!backupIp.empty()) registeredIps.insert(backupIp);
    }

    Json::Value allSessions(Json::arrayValue);
    Json::Value unknownSessions(Json::arrayValue);
    for (const auto& session : activeStreamSessions) {
      allSessions.append(sessionToJson(session));
      if (!session.clientIp.empty() && registeredIps.count(normalizeIpAddress(session.clientIp)) == 0) {
        unknownSessions.append(sessionToJson(session));
      }
    }
    root["stream_sessions"] = allSessions;
    root["unknown_stream_sessions"] = unknownSessions;

    Json::Value subscribers(Json::arrayValue);
    std::vector<const SubscriberConfig*> sortedSubscribers;
    sortedSubscribers.reserve(configManager.subscribers.subscribers.size());
    for (const auto& subscriber : configManager.subscribers.subscribers) {
      sortedSubscribers.push_back(&subscriber);
    }
    std::stable_sort(sortedSubscribers.begin(), sortedSubscribers.end(),
      [&](const SubscriberConfig* left, const SubscriberConfig* right) {
        return subscriberStreamRank(subscriberStreamClass(*left)) <
               subscriberStreamRank(subscriberStreamClass(*right));
      });
    for (const SubscriberConfig* subscriberPtr : sortedSubscribers) {
      const auto& subscriber = *subscriberPtr;
      Json::Value item = subscriber.toJson();
      item["stream_class"] = subscriberStreamClass(subscriber);
      const size_t activeSessions = streamManager.activeSubscriberSessions(subscriber);
      item["active_sessions"] = Json::UInt64(activeSessions);
      item["session_active"] = activeSessions > 0;
      Json::Value subscriberStreams(Json::arrayValue);
      const std::string primaryIp = normalizeIpAddress(subscriber.primaryIp);
      const std::string backupIp = normalizeIpAddress(subscriber.backupIp);
      for (const auto& session : activeStreamSessions) {
        const std::string sessionIp = normalizeIpAddress(session.clientIp);
        if (sessionIp == primaryIp || (!backupIp.empty() && sessionIp == backupIp)) {
          subscriberStreams.append(sessionToJson(session));
        }
      }
      item["active_streams"] = subscriberStreams;
      subscribers.append(item);
    }
    root["subscribers"] = subscribers;
    Json::Value streams(Json::arrayValue);
    auto snap = streamManager.snapshot();
    Json::Value listeners(Json::arrayValue);
    std::set<std::string> listenerKeys;
    auto appendListener = [&](const std::string& protocol, const std::string& transport,
                              const std::string& bindAddress, int port, const std::string& owner,
                              const std::string& stateText) {
        const std::string key = protocol + ":" + transport + ":" + bindAddress + ":" + std::to_string(port);
        if (!listenerKeys.insert(key).second) return;
        Json::Value entry;
        entry["protocol"] = protocol;
        entry["transport"] = transport;
        entry["bind"] = bindAddress;
        entry["port"] = port;
        entry["owner"] = owner;
        entry["state"] = stateText;
        listeners.append(entry);
    };
    std::vector<std::pair<int, std::shared_ptr<tcp::acceptor>>> acceptorSnapshot;
    {
        std::lock_guard<std::mutex> lock(acceptorsMutex);
        for (const auto& item : acceptors) acceptorSnapshot.push_back(item);
    }
    for (const auto& [port, listener] : acceptorSnapshot) {
        if (!listener || !listener->is_open()) continue;
        bool hasHttp = port == configManager.config.httpPort;
        bool hasHls = false;
        for (const auto& stream : configManager.config.streams) {
            for (const auto& output : streamOutputs(stream)) {
                const std::string type = normalizedOutputType(output);
                if ((type == "http" || type == "hls") &&
                    streamHttpPort(output, configManager.config.httpPort) == port) {
                    if (type == "http") hasHttp = true;
                    if (type == "hls") hasHls = true;
                }
            }
        }
        if (hasHttp) appendListener("HTTP", "TCP", "0.0.0.0", port, "HTTP server", "LISTEN");
        if (hasHls) appendListener("HLS", "TCP", "0.0.0.0", port, "HLS server", "LISTEN");
    }
    for (const auto& cfg : configManager.config.streams) {
        const auto it = snap.find(cfg.id);
        const bool active = it != snap.end() && it->second && it->second->active.load();
        if (!active) continue;
        for (const auto& output : streamOutputs(cfg)) {
            if (normalizedOutputType(output) != "srt" || toLower(output.outputMode) == "caller") continue;
            const int port = validPortOrDefault(output.outputPort, 7001);
            const std::string bindAddress = output.interfaceAddress.empty() ? "0.0.0.0" : output.interfaceAddress;
            appendListener("SRT", "UDP", bindAddress, port,
                cfg.name.empty() ? cfg.id : cfg.name, "LISTEN");
        }
    }
    root["listeners"] = listeners;
    // Reading FE status is cheap and does not retune the frontend, but several
    // channel tiles may share the exact same DVB input/transponder. Cache one
    // ioctl snapshot per live DVB URI for each /api/state response.
    std::map<std::string, Json::Value> dvbSignalCache;
    for (const auto& cfg : configManager.config.streams) {
        Json::Value item = cfg.toJson();
        if (snap.count(cfg.id)) {
            auto* streamState = snap.at(cfg.id);
            item["active"] = streamState->active.load();
            item["status"] = streamState->statusMessage;
            item["using_backup"] = streamState->usingBackup;
            item["active_input_uri"] = cfg.testPattern
                ? "test://bars"
                : (streamState->activeInputUri.empty() ? cfg.inputUri : streamState->activeInputUri);
            item["active_input_label"] = cfg.testPattern
                ? "Тест"
                : (streamState->usingBackup
                    ? (toLower(cfg.backupInputType) == "file" ? "Файл замены" : "Резерв")
                    : "Основной");
            item["bitrate_in_kbps"] = Json::UInt64(streamState->inputBitrate.load() / 1000);
            item["bitrate_out_kbps"] = Json::UInt64(streamState->outputBitrate.load() / 1000);
            item["payload_out_kbps"] = Json::UInt64(streamState->outputPayloadBitrate.load() / 1000);
            item["input_cc_errors"] = Json::UInt64(streamState->inputCcErrorsDelta.load());
            item["output_cc_errors"] = Json::UInt64(streamState->outputCcErrorsDelta.load());
            item["input_cc_errors_total"] = Json::UInt64(streamState->inputCcErrors.load());
            item["output_cc_errors_total"] = Json::UInt64(streamState->outputCcErrors.load());
            const uint64_t caPayloadPackets = streamState->outputTsPayloadPacketsDelta.load();
            const uint64_t caScrambledPackets = streamState->outputTsScrambledPacketsDelta.load();
            const uint64_t caClearPesStarts = streamState->outputTsClearPesStartsDelta.load();
            item["ca_output_payload_packets"] = Json::UInt64(caPayloadPackets);
            item["ca_output_scrambled_packets"] = Json::UInt64(caScrambledPackets);
            item["ca_output_clear_pes_starts"] = Json::UInt64(caClearPesStarts);
            item["ca_output_scrambled_percent"] = caPayloadPackets > 0
                ? (100.0 * static_cast<double>(caScrambledPackets) / static_cast<double>(caPayloadPackets))
                : 0.0;
            std::string rawCaDecodeState;
            const bool streamActive = streamState->active.load();
            if (cfg.conditionalAccessClient.empty()) {
                rawCaDecodeState = "not_applicable";
            } else if (!streamActive) {
                rawCaDecodeState = "offline";
            } else if (streamState->outputBitrate.load() == 0 || caPayloadPackets < 50) {
                rawCaDecodeState = "waiting";
            } else if (caScrambledPackets > 0) {
                rawCaDecodeState = "scrambled";
            } else if (caClearPesStarts == 0) {
                rawCaDecodeState = "invalid";
            } else {
                rawCaDecodeState = "clear";
            }
            item["ca_decode_state"] = smoothCaDecodeState(cfg.id, rawCaDecodeState, streamActive);
            item["cc_errors"] = item["input_cc_errors"];
            item["cc_errors_total"] = item["input_cc_errors_total"];
        } else {
            item["active"] = false;
            item["status"] = "stopped";
            item["using_backup"] = false;
            item["active_input_uri"] = cfg.testPattern ? "test://bars" : cfg.inputUri;
            item["active_input_label"] = cfg.testPattern ? "Тест" : "Основной";
            item["bitrate_in_kbps"] = Json::UInt64(0);
            item["bitrate_out_kbps"] = Json::UInt64(0);
            item["input_cc_errors"] = Json::UInt64(0);
            item["output_cc_errors"] = Json::UInt64(0);
            item["input_cc_errors_total"] = Json::UInt64(0);
            item["output_cc_errors_total"] = Json::UInt64(0);
            item["ca_output_payload_packets"] = Json::UInt64(0);
            item["ca_output_scrambled_packets"] = Json::UInt64(0);
            item["ca_output_clear_pes_starts"] = Json::UInt64(0);
            item["ca_output_scrambled_percent"] = 0.0;
            item["ca_decode_state"] = cfg.conditionalAccessClient.empty() ? "not_applicable" : "offline";
            item["cc_errors"] = Json::UInt64(0);
            item["cc_errors_total"] = Json::UInt64(0);
        }
        const bool configuredDvb = DvbSatellite::isDvbUri(cfg.inputUri);
        item["dvb_input"] = configuredDvb;
        if (configuredDvb) {
            DvbSatelliteParams configuredDvbParams;
            std::string configuredDvbError;
            if (DvbSatellite::parseUri(cfg.inputUri, configuredDvbParams, configuredDvbError)) {
                item["dvb_adapter"] = configuredDvbParams.adapter;
                item["dvb_frontend"] = configuredDvbParams.frontend;
                item["dvb_stream_id"] = configuredDvbParams.streamId;
                item["dvb_diseqc_source"] = configuredDvbParams.diseqcSource;
            }
        }
        item["dvb_signal_available"] = false;
        item["dvb_locked"] = false;
        item["dvb_signal"] = 0;
        item["dvb_quality"] = 0;
        if (configuredDvb && item.get("active", false).asBool() &&
            !item.get("using_backup", false).asBool() && !cfg.testPattern) {
            const std::string liveDvbUri = item.get("active_input_uri", cfg.inputUri).asString();
            if (DvbSatellite::isDvbUri(liveDvbUri)) {
                auto cached = dvbSignalCache.find(liveDvbUri);
                if (cached == dvbSignalCache.end()) {
                    cached = dvbSignalCache.emplace(
                        liveDvbUri, DvbSatellite::signalFromUri(liveDvbUri)).first;
                }
                const Json::Value& dvbStats = cached->second;
                item["dvb_signal_available"] = dvbStats.get("available", false).asBool();
                item["dvb_locked"] = dvbStats.get("locked", false).asBool();
                item["dvb_signal"] = dvbStats.get("signal", 0).asInt();
                item["dvb_quality"] = dvbStats.get("quality", 0).asInt();
                item["dvb_frontend_name"] = dvbStats.get("frontend_name", "");
                item["dvb_frontend_model"] = dvbStats.get("frontend_model", "");
                item["dvb_driver_mode"] = dvbStats.get("driver_mode", -1).asInt();
                item["dvb_driver_mode_name"] = dvbStats.get("driver_mode_name", "unknown");
                if (dvbStats.isMember("signal_db")) item["dvb_signal_db"] = dvbStats["signal_db"];
                if (dvbStats.isMember("cnr_db")) item["dvb_cnr_db"] = dvbStats["cnr_db"];
            }
        }

        item["ca"] = CardManager::instance().streamState(cfg.id);

        Json::Value links(Json::arrayValue);
        for (const auto& output : streamOutputs(cfg)) {
            Json::Value link;
            link["output_type"] = normalizedOutputType(output);
            link["output_mode"] = output.outputMode;
            link["output_host"] = output.outputHost;
            link["output_port"] = output.outputPort;
            link["interface_address"] = output.interfaceAddress;
            link["url"] = streamLink(output, configManager.config.httpPort);
            links.append(link);
        }
        item["vlc_links"] = links;
        item["vlc_link"] = links.size() == 0 ? "" : links[0].get("url", "").asString();
        recordQualitySample(cfg, item);
        streams.append(item);
    }
    root["streams"] = streams;
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, root);
}

