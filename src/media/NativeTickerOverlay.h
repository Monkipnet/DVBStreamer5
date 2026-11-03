#pragma once
#include "media/NativeCodecRuntime.h"
#include <jsoncpp/json/json.h>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace dvbstreamer5::media::ticker {

// One overlay per stream, with independently editable local-time intervals.
// All drawing is performed before the encoder, never on the MPEG-TS/CBR path.
struct TimeWindow {
    std::string start;
    std::string end;
    std::vector<int> days;
};
struct Entry {
    std::string streamId;
    std::string text;
    std::string fontFile;
    std::string foreground = "#ffffff";
    std::string background = "#000000";
    int backgroundOpacity = 70; // percent; 0 = fully transparent
    int fontSize = 32;          // rendered pixels at the source/encoder raster
    int speed = 100;            // pixels per second, right-to-left
    int offsetPx = 0;           // positive moves the ticker up from the bottom
    std::vector<TimeWindow> windows;
};
class TickerOverlay {
public:
    static TickerOverlay& instance();
    static Json::Value fontList();
    static bool available() noexcept;
    // Validate user-supplied JSON before writing it to disk.
    static bool validate(const Json::Value& value, std::string& error);
    void configure(const Json::Value& value);
    bool enabledFor(const std::string& streamId) const;
    bool draw(const std::string& streamId, codec::RawVideoFrame& frame);
private:
    TickerOverlay() = default;
    bool active(const Entry& entry) const;
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    std::chrono::steady_clock::time_point animationStart_ = std::chrono::steady_clock::now();
};
} // namespace dvbstreamer5::media::ticker
