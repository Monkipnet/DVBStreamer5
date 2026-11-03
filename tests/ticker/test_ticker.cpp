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
        // V323: motion follows presentation PTS; same PTS produces exactly
        // the same picture even when encoder-thread timing is irregular.
        auto moving=list;
        moving[0]["text"]="ПЛАВНАЯ БЕГУЩАЯ СТРОКА";
        moving[0]["speed"]=600;
        overlay.configure(moving);
        dvbstreamer5::media::codec::RawVideoFrame base;
        base.width=320;
        base.height=180;
        base.hasPts=true;
        base.i420=pristine;
        dvbstreamer5::media::codec::RawVideoFrame shown;
        for(int i=0;i<=16;++i) {
            auto current=base;
            current.pts90k=static_cast<std::uint64_t>(i)*3600U;
            require(overlay.draw("ticker-test",current,25.0),"PTS ticker draw");
            if(i==16)shown=std::move(current);
        }
        require(shown.i420!=pristine,"moving ticker visible after 16 frames");
        auto repeated=base;
        repeated.pts90k=16U*3600U;
        require(overlay.draw("ticker-test",repeated,25.0),"same-PTS ticker draw");
        require(repeated.i420==shown.i420,
                "repeated PTS must not advance ticker motion");
        moving[0]["speed"]=5;
        require(TickerOverlay::validate(moving,error),"slow 5 px/s valid");
        overlay.configure(moving);
        repeated=base;
        repeated.pts90k=16U*3600U;
        require(overlay.draw("ticker-test",repeated,25.0),"speed edit live");
        require(repeated.i420==shown.i420,
                "changing speed must not reset ticker position");
    } else {
        std::cout << "Ticker FreeType/fonts unavailable; render test skipped\n";
    }
    overlay.configure(Json::Value(Json::arrayValue));
    require(!overlay.enabledFor("ticker-test"), "!overlay.enabledFor('ticker-test')");
    std::cout << "Ticker smoke tests OK\n";
    return 0;
}
