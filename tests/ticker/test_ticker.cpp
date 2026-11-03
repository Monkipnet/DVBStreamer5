#include "media/NativeTickerOverlay.h"
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>

static void require(bool condition, const char* description) {
    if (!condition) {
        std::cerr << "Ticker test failed: " << description << "\n";
        std::exit(1);
    }
}

int main() {
    using dvbstreamer5::media::ticker::TickerOverlay;
    Json::Value entry(Json::objectValue);
    entry["stream_id"] = "ticker-test";
    entry["text"] = "Проверка бегущей строки DVBStreamer5";
    entry["foreground"] = "#FFFFFF";
    entry["background"] = "#0000FF";
    entry["background_opacity"] = 80;
    entry["speed"] = 80;
    entry["font_size"] = 28;
    entry["offset_px"] = 0;
    Json::Value window(Json::objectValue);
    window["start"] = "00:00";
    window["end"] = "00:00";
    Json::Value days(Json::arrayValue);
    for (int d=1;d<=7;++d) days.append(d);
    window["days"] = days;
    entry["windows"] = Json::Value(Json::arrayValue);
    entry["windows"].append(window);
    Json::Value list(Json::arrayValue);
    list.append(entry);
    std::string error;
    require(TickerOverlay::validate(list,error), "TickerOverlay::validate(list,error)");
    // Master and per-window switches keep their configuration yet suppress
    // the live overlay without stream or encoder restarts.
    auto paused=list;
    paused[0]["enabled"]=false;
    require(TickerOverlay::validate(paused,error), "valid master pause");
    auto& overlay=TickerOverlay::instance();
    overlay.configure(paused);
    require(!overlay.enabledFor("ticker-test"), "master pause disabled");
    require(!overlay.renderRequestedFor("ticker-test"), "master pause invisible");
    paused=list;
    paused[0]["windows"][0]["enabled"]=false;
    require(TickerOverlay::validate(paused,error), "valid window pause");
    overlay.configure(paused);
    require(overlay.enabledFor("ticker-test"), "window pause retained entry");
    require(!overlay.renderRequestedFor("ticker-test"), "window pause invisible");
    paused[0]["windows"][0]["enabled"]="on";
    require(!TickerOverlay::validate(paused,error), "invalid window boolean rejected");
    auto malformed=list;
    malformed[0]["windows"][0]["start"] = "25:00";
    require(!TickerOverlay::validate(malformed,error), "!TickerOverlay::validate(malformed,error)");
    malformed=list;
    malformed[0]["background"] = "red";
    require(!TickerOverlay::validate(malformed,error), "!TickerOverlay::validate(malformed,error)");
    malformed=list;
    malformed[0]["font_size"] = 0;
    require(!TickerOverlay::validate(malformed,error), "!TickerOverlay::validate(malformed,error)");

    overlay.configure(list);
    require(overlay.enabledFor("ticker-test"), "overlay.enabledFor('ticker-test')");
    require(!overlay.enabledFor("other-channel"), "!overlay.enabledFor('other-channel')");
    dvbstreamer5::media::codec::RawVideoFrame frame;
    frame.width=320;
    frame.height=180;
    frame.i420.assign(320*180*3/2,static_cast<std::uint8_t>(128));
    const auto pristine=frame.i420;
    require(!overlay.draw("other-channel",frame), "!overlay.draw('other-channel',frame)");
    require(frame.i420==pristine, "frame.i420==pristine");
    if (TickerOverlay::available() && TickerOverlay::fontList().size()>0) {
        require(overlay.draw("ticker-test",frame), "overlay.draw('ticker-test',frame)");
        require(frame.i420!=pristine, "frame.i420!=pristine");
    } else {
        std::cout << "Ticker FreeType/fonts unavailable; render test skipped\n";
    }
    overlay.configure(Json::Value(Json::arrayValue));
    require(!overlay.enabledFor("ticker-test"), "!overlay.enabledFor('ticker-test')");
    std::cout << "Ticker smoke tests OK\n";
    return 0;
}
