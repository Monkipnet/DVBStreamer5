#include "media/NativeTickerOverlay.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#ifdef DVBSTREAMER5_HAVE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

namespace dvbstreamer5::media::ticker {
namespace {
bool timeMinutes(const std::string& s, int& result) {
    if (s.size() != 5 || s[2] != ':' ||
        s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9' ||
        s[3] < '0' || s[3] > '9' || s[4] < '0' || s[4] > '9') return false;
    const int h = (s[0]-'0') * 10 + s[1]-'0';
    const int m = (s[3]-'0') * 10 + s[4]-'0';
    if (h > 23 || m > 59) return false;
    result = h * 60 + m;
    return true;
}

bool hexColor(const std::string& s) {
    if (s.size() != 7 || s[0] != '#') return false;
    for (std::size_t i=1; i<s.size(); ++i)
        if (!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')||
              (s[i]>='A'&&s[i]<='F'))) return false;
    return true;
}

int channel(const std::string& s, int pos) {
    return std::stoi(s.substr(static_cast<std::size_t>(pos), 2), nullptr, 16);
}
struct Yuv { int y, u, v; };
Yuv yuvOf(const std::string& hex) {
    const int r=channel(hex,1),g=channel(hex,3),b=channel(hex,5);
    return {std::clamp(16+((66*r+129*g+25*b+128)>>8),0,255),
            std::clamp(128+((-38*r-74*g+112*b+128)>>8),0,255),
            std::clamp(128+((112*r-94*g-18*b+128)>>8),0,255)};
}

std::vector<std::pair<std::string,std::string>> systemFonts() {
    std::vector<std::pair<std::string,std::string>> result;
    std::set<std::string> paths;
#if defined(__linux__)
    // Fixed command, never built from untrusted UI values.
    FILE* pipe = ::popen("fc-list -f '%{family[0]}\\t%{file}\\n' 2>/dev/null", "r");
    if (pipe) {
        char line[8192];
        while (std::fgets(line, sizeof(line), pipe) && result.size() < 256) {
            std::string record(line);
            const auto tab = record.find('\t');
            if (tab == std::string::npos) continue;
            std::string family=record.substr(0,tab);
            std::string path=record.substr(tab+1);
            while (!path.empty() && (path.back()=='\n'||path.back()=='\r')) path.pop_back();
            if (family.empty() || path.empty() || path[0] != '/' || !paths.insert(path).second) continue;
            result.emplace_back(std::move(family), std::move(path));
        }
        ::pclose(pipe);
    }
#endif
    if (result.empty()) {
        const std::string fallback="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
        FILE* f=std::fopen(fallback.c_str(),"rb");
        if (f) { std::fclose(f); result.emplace_back("DejaVu Sans",fallback); }
    }
    std::sort(result.begin(),result.end());
    return result;
}

const std::vector<std::pair<std::string,std::string>>& fonts() {
    static const auto result=systemFonts();
    return result;
}
std::string preferredFont() {
    const auto& found=fonts();
    for (const auto& f : found)
        if (f.first.find("DejaVu Sans") != std::string::npos &&
            f.second.find("DejaVuSans.ttf") != std::string::npos)
            return f.second; // covers Cyrillic without a custom font selection
    for (const auto& f : found)
        if (f.first.find("Noto Sans") != std::string::npos &&
            f.second.find("NotoSans-Regular") != std::string::npos)
            return f.second;
    return found.empty() ? std::string() : found.front().second;
}
bool knownFont(const std::string& file) {
    for(const auto& item : fonts()) if(item.second==file) return true;
    return false;
}

bool intervalActive(const TimeWindow& w, const std::tm& local) {
    int start=0,end=0;
    if (!timeMinutes(w.start,start) || !timeMinutes(w.end,end)) return false;
    // tm_wday: Sunday=0; UI ISO weekday: Monday=1 ... Sunday=7.
    const int today=local.tm_wday==0?7:local.tm_wday;
    const int previous=today==1?7:today-1;
    const auto hasDay=[&](int d) { return std::find(w.days.begin(),w.days.end(),d)!=w.days.end(); };
    const int minute=local.tm_hour*60+local.tm_min;
    if (start==end) return hasDay(today); // full day
    if (start<end) return hasDay(today) && minute>=start && minute<end;
    return (hasDay(today) && minute>=start) ||
           (hasDay(previous) && minute<end);
}

#ifdef DVBSTREAMER5_HAVE_FREETYPE
struct RasterCache {
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    std::string faceFile;
    std::string key;
    int w=0,h=0;
    std::vector<std::uint8_t> alpha;
    RasterCache() { if (FT_Init_FreeType(&library)) library=nullptr; }
    ~RasterCache() { if(face)FT_Done_Face(face); if(library)FT_Done_FreeType(library); }

    bool raster(const Entry& e) {
        if (!library) return false;
        const std::string file=e.fontFile.empty()?preferredFont():e.fontFile;
        if (file.empty() || !knownFont(file)) return false;
        const std::string newKey=file+"|"+std::to_string(e.fontSize)+"|"+e.text;
        if (newKey==key) return !alpha.empty();
        key.clear();
        if (faceFile!=file || !face) {
            if(face) FT_Done_Face(face);
            face=nullptr;
            if(FT_New_Face(library,file.c_str(),0,&face))return false;
            faceFile=file;
        }
        if(FT_Set_Pixel_Sizes(face,0,static_cast<FT_UInt>(e.fontSize)))return false;
        const int base=std::max(1,static_cast<int>(face->size->metrics.ascender >> 6));
        h=std::max(e.fontSize+12, static_cast<int>(face->size->metrics.height>>6)+8);
        h=std::clamp(h,20,140);
        // Decode UTF-8 into Unicode code points; FreeType handles Cyrillic glyphs.
        std::vector<std::uint32_t> chars;
        for (std::size_t i=0;i<e.text.size();) {
            const auto c=static_cast<unsigned char>(e.text[i++]);
            std::uint32_t cp=c;
            int extra=0;
            if (c>=0xF0&&c<=0xF4){ cp=c&7;extra=3; }
            else if(c>=0xE0&&c<=0xEF){cp=c&15;extra=2;}
            else if(c>=0xC2&&c<=0xDF){cp=c&31;extra=1;}
            else if(c>=0x80)continue;
            if(i+static_cast<std::size_t>(extra)>e.text.size())break;
            for(int j=0;j<extra;++j)cp=(cp<<6)|(static_cast<unsigned char>(e.text[i++])&63);
            chars.push_back(cp);
        }
        int advance=0;
        for(auto c:chars)if(!FT_Load_Char(face,c,FT_LOAD_DEFAULT))
            advance+=static_cast<int>(face->glyph->advance.x>>6);
        w=std::clamp(advance+8,1,16000);
        alpha.assign(static_cast<std::size_t>(w*h),0);
        int pen=4;
        for(auto c:chars) {
            if(FT_Load_Char(face,c,FT_LOAD_RENDER))continue;
            const auto& glyph=*face->glyph;
            const auto& bmp=glyph.bitmap;
            const int x0=pen+glyph.bitmap_left;
            const int y0=base+4-glyph.bitmap_top;
            for(int yy=0;yy<static_cast<int>(bmp.rows);++yy) {
                const int y=y0+yy;
                if(y<0||y>=h)continue;
                // FreeType bitmap pitch may be negative and monochrome rows
                // are bit-packed (row[xx] would overread a MONO glyph).
                const int pitch=static_cast<int>(bmp.pitch);
                const int physicalRow=pitch>=0?yy:static_cast<int>(bmp.rows)-1-yy;
                const auto* row=bmp.buffer+static_cast<std::ptrdiff_t>(physicalRow)*pitch;
                for(int xx=0;xx<static_cast<int>(bmp.width);++xx) {
                    const int x=x0+xx;
                    if(x<0||x>=w)continue;
                    std::uint8_t coverage=0;
                    if(bmp.pixel_mode==FT_PIXEL_MODE_GRAY)coverage=row[xx];
                    else if(bmp.pixel_mode==FT_PIXEL_MODE_MONO)
                        coverage=(row[xx/8]&(0x80U>>(xx%8)))?255:0;
                    else continue;
                    alpha[static_cast<std::size_t>(y*w+x)]=coverage;
                }
            }
            pen+=static_cast<int>(glyph.advance.x>>6);
            if(pen>=w)break;
        }
        key=newKey;
        return true;
    }
};
#endif

void blend(codec::RawVideoFrame& frame,int x,int y,const Yuv& color,int alpha) {
    if(alpha<=0||x<0||y<0||x>=frame.width||y>=frame.height)return;
    const int w=frame.width,h=frame.height;
    const std::size_t yIndex=static_cast<std::size_t>(y)*w+x;
    const std::size_t uvBase=static_cast<std::size_t>(w)*h;
    auto mix=[&](std::size_t at,int c) {
        const int old=frame.i420[at];
        frame.i420[at]=static_cast<std::uint8_t>((old*(255-alpha)+c*alpha+127)/255);
    };
    mix(yIndex,color.y);
    if(!(x&1) && !(y&1)) {
        const std::size_t uv=static_cast<std::size_t>(y/2)*(w/2)+(x/2);
        mix(uvBase+uv,color.u);
        mix(uvBase+uv+uvBase/4,color.v);
    }
}
} // namespace

TickerOverlay& TickerOverlay::instance() {
    static TickerOverlay overlay;
    return overlay;
}
bool TickerOverlay::available() noexcept {
#ifdef DVBSTREAMER5_HAVE_FREETYPE
    return true;
#else
    return false;
#endif
}
Json::Value TickerOverlay::fontList() {
    Json::Value out(Json::arrayValue);
    for(const auto& f:fonts()) {
        Json::Value v(Json::objectValue);
        v["name"]=f.first;
        v["file"]=f.second;
        out.append(v);
    }
    return out;
}
bool TickerOverlay::validate(const Json::Value& config,std::string& error) {
    error.clear();
    if(!config.isArray()||config.size()>64U) {
        error="Expected at most 64 ticker entries";return false;
    }
    std::set<std::string> ids;
    for(const auto& item:config) {
        if(item.isObject() && item.isMember("enabled") && !item["enabled"].isBool()) {
            error="Ticker enabled must be boolean";return false;
        }
        if(!item.isObject() || !item["stream_id"].isString() ||
           !item["text"].isString()) { error="Invalid ticker entry";return false; }
        const std::string id=item["stream_id"].asString();
        const std::string text=item["text"].asString();
        if(id.empty()||id.size()>256||!ids.insert(id).second||text.size()>8192) {
            error="Invalid ticker stream ID or text (8192 bytes max)";return false;
        }
        for (const char* key : {"font_file", "foreground", "background"})
            if(item.isMember(key) && !item[key].isString()) {
                error="Ticker font/colors must be strings";return false;
            }
        for (const char* key : {"font_size", "speed", "background_opacity", "offset_px"})
            if(item.isMember(key) && !item[key].isInt()) {
                error="Ticker numeric field is invalid";return false;
            }
        const std::string font=item.get("font_file","").asString();
        if(!font.empty()&&!knownFont(font)) {error="Font is not installed";return false;}
        if(!hexColor(item.get("foreground","#ffffff").asString())||
           !hexColor(item.get("background","#000000").asString())) {
            error="Invalid color";return false;
        }
        const int fontSize=item.get("font_size",32).asInt();
        const int speed=item.get("speed",40).asInt();
        const int opacity=item.get("background_opacity",70).asInt();
        const int offset=item.get("offset_px",0).asInt();
        if(fontSize<12||fontSize>96||speed<5||speed>600||opacity<0||opacity>100||offset<0||offset>2160) {
            error="Ticker appearance is out of range";return false;
        }
        const auto& windows=item["windows"];
        if(!windows.isArray()||windows.size()>32U) {
            error="Expected at most 32 schedule intervals";return false;
        }
        for(const auto& window:windows) {
            if(window.isObject() && window.isMember("enabled") && !window["enabled"].isBool()) {
                error="Schedule enabled must be boolean";return false;
            }
            if(!window.isObject() || !window["start"].isString() ||
               !window["end"].isString() || !window["days"].isArray()) {
                error="Invalid schedule interval";return false;
            }
            int s=0,e=0;
            if(!timeMinutes(window["start"].asString(),s)||
               !timeMinutes(window["end"].asString(),e)) {
                error="Time must be HH:MM";return false;
            }
            if(window["days"].size()>7U) {error="Too many weekdays";return false;}
            for(const auto& d:window["days"])if(!d.isInt()||d.asInt()<1||d.asInt()>7){
                error="Invalid weekday (1..7)";return false;
            }
        }
    }
    return true;
}
void TickerOverlay::configure(const Json::Value& config) {
    std::vector<Entry> next;
    if(config.isArray())for(const auto& item:config){
        Entry e;
        e.enabled=item.get("enabled",true).asBool();
        e.streamId=item["stream_id"].asString();
        e.text=item["text"].asString();
        e.fontFile=item.get("font_file","").asString();
        e.foreground=item.get("foreground","#ffffff").asString();
        e.background=item.get("background","#000000").asString();
        e.backgroundOpacity=item.get("background_opacity",70).asInt();
        e.fontSize=item.get("font_size",32).asInt();
        e.speed=item.get("speed",40).asInt();
        e.offsetPx=item.get("offset_px",0).asInt();
        for(const auto& w:item["windows"]) {
            TimeWindow t;
            t.enabled=w.get("enabled",true).asBool();
            t.start=w["start"].asString();
            t.end=w["end"].asString();
            for(const auto& d:w["days"])t.days.push_back(d.asInt());
            e.windows.push_back(std::move(t));
        }
        next.push_back(std::move(e));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    entries_=std::move(next);
    // Retain motion phase across edits; remove only obsolete channel clocks.
    for (auto it=motion_.begin();it!=motion_.end();) {
        const bool exists=std::any_of(entries_.begin(),entries_.end(),
            [&](const Entry& e){return e.streamId==it->first;});
        if(!exists)it=motion_.erase(it);
        else ++it;
    }
}
bool TickerOverlay::enabledFor(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for(const auto& e:entries_)if(e.streamId==id&&e.enabled&&!e.text.empty()&&!e.windows.empty())
        return true;
    return false;
}
bool TickerOverlay::renderRequestedFor(const std::string& id) const {
    Entry entry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it=std::find_if(entries_.begin(),entries_.end(),
            [&](const Entry& e){return e.streamId==id;});
        if(it==entries_.end()||!it->enabled||it->text.empty()||it->windows.empty())
            return false;
        entry=*it;
    }
    return available() && active(entry);
}
bool TickerOverlay::active(const Entry& e) const {
    const auto now=std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local,&now);
#else
    localtime_r(&now,&local);
#endif
    for(const auto& w:e.windows)if(w.enabled&&intervalActive(w,local))return true;
    return false;
}
bool TickerOverlay::draw(const std::string& streamId,codec::RawVideoFrame& frame,
                         double nominalFps) {
#ifdef DVBSTREAMER5_HAVE_FREETYPE
    Entry entry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it=std::find_if(entries_.begin(),entries_.end(),[&](const Entry& e){return e.streamId==streamId;});
        if(it==entries_.end()||!it->enabled||it->text.empty()||it->windows.empty())return false;
        entry=*it;
    }
    if(!active(entry))return false;
    const int w=frame.width,h=frame.height;
    if(w<16||h<16||(w&1)||(h&1)||frame.i420.size()<static_cast<std::size_t>(w)*h*3/2)
        return false;
    static thread_local RasterCache cache;
    if(!cache.raster(entry))return false;
    const int bandH=cache.h;
    const int top=std::clamp(h-bandH-entry.offsetPx,0,std::max(0,h-bandH));
    const Yuv bg=yuvOf(entry.background),fg=yuvOf(entry.foreground);
    const int bgAlpha=entry.backgroundOpacity*255/100;
    // Animate using video presentation timestamps, not encoder-worker time.
    // Invalid/discontinuous PTS advances one nominal frame, avoiding leaps.
    const double fps=std::isfinite(nominalFps) && nominalFps>=1.0 && nominalFps<=240.0
        ? nominalFps : 25.0;
    const double fallbackStep=1.0/fps;
    const double period=static_cast<double>(w+cache.w+80);
    double travelled=0.0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& motion=motion_[streamId];
        if(!motion.initialized || motion.lastSpeed!=entry.speed) {
            std::cerr << "NATIVE TICKER MOTION stream_id=" << streamId
                      << " clock=video_pts90k"
                      << " subpixel=1"
                      << " speed_px_s=" << entry.speed
                      << " fps_reference=" << fps
                      << std::endl;
            motion.lastSpeed=entry.speed;
        }
        if(!motion.initialized || motion.text!=entry.text ||
           motion.fontFile!=entry.fontFile || motion.fontSize!=entry.fontSize) {
            motion.initialized=true;
            motion.text=entry.text;
            motion.fontFile=entry.fontFile;
            motion.fontSize=entry.fontSize;
            motion.distancePx=0.0;
        } else {
            double deltaSeconds=fallbackStep;
            if(frame.hasPts && motion.lastHasPts) {
                constexpr std::uint64_t kPtsMask=(1ULL<<33U)-1ULL;
                const std::uint64_t delta=(frame.pts90k-motion.lastPts90k)&kPtsMask;
                if(delta==0)deltaSeconds=0.0;
                else if(delta<=22500U) {
                    // V332: preserve true PTS velocity for normal 25/50-fps
                    // cadence, but cap a one-off late PTS jump to 1.5 output
                    // frames. It avoids an eye-catching text teleport after
                    // brief input jitter without changing video PTS or FPS.
                    // Fractional positions remain subpixel-interpolated below.
                    deltaSeconds=std::min(
                        static_cast<double>(delta)/90000.0,
                        fallbackStep*1.5);
                }
                // Discontinuities >250ms do not have drawable intermediate
                // frames, so never skip a large visible portion of the text.
            }
            motion.distancePx=std::fmod(motion.distancePx+
                static_cast<double>(entry.speed)*deltaSeconds,period);
        }
        motion.lastHasPts=frame.hasPts;
        motion.lastPts90k=frame.pts90k;
        travelled=motion.distancePx;
    }
    const double textX=static_cast<double>(w)-travelled;
    for(int y=0;y<bandH && top+y<h;++y)
        for(int x=0;x<w;++x)
            if(bgAlpha)blend(frame,x,top+y,bg,bgAlpha);
    // Linear coverage interpolation preserves fractional pixel positions:
    // no whole-pixel snapping, including at slow (5-25 px/s) speed.
    const int first=std::max(0,static_cast<int>(std::floor(textX)));
    const int last=std::min(w,static_cast<int>(std::ceil(textX+cache.w)));
    if(first<last)for(int y=0;y<cache.h && top+y<h;++y) {
        const auto offset=static_cast<std::size_t>(y)*cache.w;
        for(int x=first;x<last;++x){
            const double at=static_cast<double>(x)-textX;
            const int x0=static_cast<int>(std::floor(at));
            const double frac=at-x0;
            const int a0=(x0>=0&&x0<cache.w) ? cache.alpha[offset+x0] : 0;
            const int a1=(x0+1>=0&&x0+1<cache.w) ? cache.alpha[offset+x0+1] : 0;
            const int alpha=std::clamp(static_cast<int>(
                std::lround((1.0-frac)*a0+frac*a1)),0,255);
            if(alpha)blend(frame,x,top+y,fg,alpha);
        }
    }
    return true;
#else
    (void)streamId;(void)frame;(void)nominalFps;return false;
#endif
}
} // namespace dvbstreamer5::media::ticker
