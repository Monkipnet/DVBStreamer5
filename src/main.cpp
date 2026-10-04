#include <iostream>
#include <boost/asio.hpp>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include "ConfigManager.h"
#include "CardManager.h"
#include "TelegramNotifier.h"
#include "StreamManager.h"
#include "HttpServer.h"
#include "AppVersion.h"

int main() {
#if defined(__GLIBC__)
    // Limit glibc allocator arenas for long-running network worker threads.
    // glibc otherwise creates many independent malloc arenas and keeps freed
    // pages cached in those arenas, which makes RSS look like a leak after
    // queue warm-up/reconnects.  A modest arena cap still gives enough allocator
    // concurrency for the actual CPU parallelism while reducing fragmentation.
    mallopt(M_ARENA_MAX, 16);
    mallopt(M_TRIM_THRESHOLD, 512 * 1024);
#endif
    std::cerr << dvbstreamer5::app::kProductName
              << " " << dvbstreamer5::app::kProgramVersion
              << " | support=" << dvbstreamer5::app::kSupportEmail << std::endl;
    std::cerr << "main() entered" << std::endl;

    std::cerr << "Native media engine initialized" << std::endl;

    ConfigManager configManager;
    if (!configManager.load()) {
        std::cerr << "Unable to load or create configuration." << std::endl;
        return 1;
    }

    std::cerr << "Config loaded: http_port=" << configManager.config.httpPort
              << " login=" << configManager.config.login << std::endl;

    CardManager::instance().configure(configManager.config.camClients);

    TelegramNotifier notifier(configManager);
    StreamManager streamManager(configManager, notifier);

    boost::asio::io_context ioc;
    HttpServer server(ioc, configManager, streamManager);

    if (!server.start()) {
        std::cerr << "HTTP server start failed" << std::endl;
        return 1;
    }

    std::cerr << "HTTP server started" << std::endl;
    for (const auto& stream : configManager.config.streams) {
        if (!stream.autoStart || stream.activationMode == "ondemand") {
            if (stream.autoStart && stream.activationMode == "ondemand") {
                std::cerr << "On-demand stream kept idle at startup: " << stream.id << std::endl;
            }
            continue;
        }
        std::cerr << "Auto-starting stream: " << stream.id << std::endl;
        try {
            std::string startError;
            if (!streamManager.startStream(stream, &startError)) {
                std::cerr << "Auto-start failed: stream=" << stream.id
                          << " error=" << (startError.empty() ? "unknown" : startError) << std::endl;
            }
        } catch (const std::exception& ex) {
            // Resource exhaustion (for example pthread_create -> EAGAIN) must
            // fail one stream, not terminate the complete headend process.
            std::cerr << "Auto-start exception contained: stream=" << stream.id
                      << " error=" << ex.what() << std::endl;
        } catch (...) {
            std::cerr << "Auto-start unknown exception contained: stream=" << stream.id << std::endl;
        }
    }
    std::cout << "DVBStreamer5 running on port " << configManager.config.httpPort << std::endl;
    std::cerr << "Calling ioc.run()" << std::endl;

    // A handler exception must not take down all 20+ active services.  Asio's
    // io_context remains usable after an exception escapes a handler, so keep
    // servicing the remaining descriptors and log the contained failure.
    while (!ioc.stopped()) {
        try {
            ioc.run();
        } catch (const std::exception& ex) {
            std::cerr << "io_context handler exception contained: " << ex.what() << std::endl;
        } catch (...) {
            std::cerr << "io_context unknown handler exception contained" << std::endl;
        }
    }
    return 0;
}
