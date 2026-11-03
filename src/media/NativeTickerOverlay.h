#pragma once
#include "media/NativeCodecRuntime.h"
#include <jsoncpp/json/json.h>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dvbstreamer5::media::ticker {

// One overlay per stream, with independently editable local-time intervals.
// All drawing is performed before the encoder, never on the MPEG-TS/CBR path.
struct TimeWindow {
    bool enabled = true;       // temporarily pause without losing schedule
    std::string start;
    std::string end;
    std::vector<int> days;
};
struct Entry {
    bool enabled = true;       // immediately show/hide without removing text
    std::string streamId;
    std::string text;
    std::string fontFile;
    std::string foreground = "#ffffff";
    std::string background = "#000000";
    int backgroundOpacity = 70; // percent; 0 = fully transparent
    int fontSize = 32;          // rendered pixels at the source/encoder raster
    int speed = 40;             // pixels per second, right-to-left (smooth default)
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
    // Only true during an enabled interval; polled by the active video worker.
    bool renderRequestedFor(const std::string& streamId) const;
    bool draw(const std::string& streamId, codec::RawVideoFrame& frame,
              double nominalFps = 25.0);
private:
    TickerOverlay() = default;
    bool active(const Entry& entry) const;
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    // Phase is tracked independently per output stream using presentation PTS;
    // saving settings for one channel never restarts other channels' motion.
    struct MotionClock {
        bool initialized = false;
        bool lastHasPts = false;
        std::uint64_t lastPts90k = 0;
        double distancePx = 0.0;
        std::string text;
        std::string fontFile;
        int fontSize = 0;
    };
    std::unordered_map<std::string, MotionClock> motion_;
};
} // namespace dvbstreamer5::media::ticker
