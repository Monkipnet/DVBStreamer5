#include "media/NativeSrtTransport.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

int main() {
    using namespace dvbstreamer5::media::srt;
    std::string detail;
    if (!runtimeAvailable(&detail)) {
        std::cerr << "SRT runtime unavailable: " << detail << "\n";
        return 1;
    }
    if (detail.find("embedded:libsrt-1.5") == std::string::npos) {
        std::cerr << "SRT runtime is not using the embedded payload: " << detail << "\n";
        return 6;
    }
    EndpointConfig listener;
    listener.host = "0.0.0.0";
    listener.bindAddress = "127.0.0.1";
    listener.port = 19091;
    listener.mode = "listener";
    listener.latencyMs = 60;
    listener.ioTimeoutMs = 300;

    NativeSrtOutput output;
    std::string error;
    if (!output.start(listener, {}, {}, {}, error)) {
        std::cerr << "output start failed: " << error << "\n";
        return 1;
    }

    EndpointConfig caller;
    caller.host = "127.0.0.1";
    caller.port = listener.port;
    caller.mode = "caller";
    caller.latencyMs = 60;
    caller.ioTimeoutMs = 300;

    std::vector<std::uint8_t> received;
    std::atomic<bool> got{false};
    NativeSrtInput input;
    if (!input.start(caller,
        [&](const std::uint8_t* data, std::size_t size) {
            received.insert(received.end(), data, data + size);
            if (received.size() >= 1316) got = true;
            return true;
        }, {}, error)) {
        std::cerr << "input start failed: " << error << "\n";
        output.stop();
        return 2;
    }

    std::vector<std::uint8_t> block(1316, 0xff);
    for (std::size_t i = 0; i < block.size(); i += 188) block[i] = 0x47;
    for (int i = 0; i < 50 && !output.connected(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (!output.push(block.data(), block.size())) {
        std::cerr << "push failed\n";
        input.stop(); output.stop(); return 3;
    }
    for (int i = 0; i < 60 && !got; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    input.stop(); output.stop();
    if (!got || received.size() < block.size()) {
        std::cerr << "did not receive SRT payload, received=" << received.size()
                  << " input_error=" << input.lastError() << " output_error=" << output.lastError() << "\n";
        return 4;
    }
    if (!std::equal(block.begin(), block.end(), received.begin())) {
        std::cerr << "payload mismatch\n";
        return 5;
    }
    std::cout << "PASS: native SRT loopback " << detail << "\n";
    return 0;
}
