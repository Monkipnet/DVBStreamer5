// HttpServer implementation is intentionally partitioned into focused
// implementation fragments. They remain one translation unit so this structural
// refactor cannot change anonymous-namespace visibility, static state, or linkage.
// Keep the include order synchronized with src/http/README.md.
#include "media/NativeTickerOverlay.h"
#include "http/HttpServerCommon.inc"
#include "http/HttpServerTickerImport.inc"
#include "http/HttpServerEpg.inc"
#include "http/HttpServerTransport.inc"
#include "http/HttpServerState.inc"
#include "http/HttpServerDvbMedia.inc"
#include "http/HttpServerQuality.inc"
#include "http/HttpServerConfigActions.inc"

// V10.8.180: preserve the existing large WebUI implementation byte-for-byte and
// wrap only its rendered HTML. The private sub-kbps carrier remains compatible
// with V175 +777 source-PCR configs and V176 startup-buffer configs; +699/+799
// now select a zero-ms startup reservoir without widening StreamConfig or the
// StreamManager hot path.
#define renderIndexPage renderIndexPageBase
#include "http/HttpServerWebUi.inc"
#undef renderIndexPage

namespace {

bool replaceWebUiOnce(std::string& text,
                      const std::string& from,
                      const std::string& to) {
    const auto pos = text.find(from);
    if (pos == std::string::npos) return false;
    text.replace(pos, from.size(), to);
    return true;
}

void injectSourcePcrTimelineUi(std::string& html) {
    // Decode every recognised private carrier before presenting bitrate to the
    // user. Old +777 configs stay source-PCR with the default 3000-ms buffer.
    // V10.8.180 adds +699/+799 as fixed/source-PCR zero-buffer carriers.
    (void)replaceWebUiOnce(
        html,
        "value=\"${Math.round((stream.target_bitrate||2000000)/1000)}\"",
        "value=\"${(()=>{const v=Number(stream.target_bitrate||2000000);const r=v%1000;const encoded=(r===699)||(r===777)||(r===799)||(r>=700&&r<=729)||(r>=800&&r<=829);return Math.round((encoded?v-r:v)/1000);})()}\"");

    const std::string bitrateRowAnchor =
        "        <div class=\"form-row full\"><label>Target bitrate (кбит/с, для CBR)</label>";
    const auto rowStart = html.find(bitrateRowAnchor);
    if (rowStart != std::string::npos) {
        const auto rowEnd = html.find('\n', rowStart);
        if (rowEnd != std::string::npos) {
            const std::string bufferRow =
                "        <div class=\"form-row full\"><label>CBR буфер старта, мс</label>"
                "<input id=\"streamCbrBufferMs\" type=\"number\" min=\"0\" max=\"15000\" step=\"500\" "
                "value=\"${(()=>{const r=Number(stream.target_bitrate||0)%1000;if(r===699||r===799)return 0;if(r>=700&&r<=729)return (r-700+1)*500;if(r>=800&&r<=829)return (r-800+1)*500;return 3000;})()}\" />"
                "<small>0 = отключить стартовое накопление; 500–15000 мс — стартовый запас useful TS перед запуском CBR, шаг 500 мс. Для нестабильных спутниковых/HLS входов можно проверить режим 0 мс без изменения физического CBR.</small></div>\n";
            const std::string timelineRow =
                "        <div class=\"form-row full\"><label>Медиатаймлайн по PCR источника</label>"
                "<div class=\"checkbox-inline\"><input id=\"streamPcrSourcePll\" type=\"checkbox\" "
                "${(()=>{const r=Number(stream.target_bitrate||0)%1000;return (r===777||r===799||(r>=800&&r<=829))?'checked':'';})()} />"
                "<span>Медиатаймлайн по PCR источника</span></div>"
                "<small>Для проблемных CBR-каналов: useful-пакеты идут по скорости source PCR, PCR/PTS/DTS остаются в одном исходном clock domain.</small></div>\n";
            html.insert(rowEnd + 1U, bufferRow + timelineRow);
        }
    }

    // Persist both controls in the existing private sub-kbps carrier. Keep the
    // ordinary default (3000 ms, source-PCR off) as a plain whole-kbps value so
    // existing non-experimental streams remain byte-for-byte compatible.
    // Zero ms uses +699 (fixed PCR) or +799 (source PCR).
    (void)replaceWebUiOnce(
        html,
        "    target_bitrate: Number(document.getElementById('streamBitrate').value) * 1000,",
        "    target_bitrate: (() => { const base=Math.max(100,Math.round(Number(document.getElementById('streamBitrate').value)||2000))*1000; const requested=Number(document.getElementById('streamCbrBufferMs')?.value||3000); const source=document.getElementById('streamPcrSourcePll')?.checked===true; if(requested<=0)return base+(source?799:699); const bufferMs=Math.min(15000,Math.max(500,Math.round(requested/500)*500)); const code=Math.min(29,Math.max(0,Math.round(bufferMs/500)-1)); if(!source&&bufferMs===3000)return base; if(source&&bufferMs===3000)return base+777; return base+(source?800:700)+code; })(),");

    // Disable the new controls together with the existing bitrate input when
    // the selected output is not using CBR.
    const std::string cbrControlAnchor =
        "    const bitrateInput = document.getElementById('streamBitrate');\n"
        "    if (bitrateInput) bitrateInput.disabled = !cbrSupported || !cbrInput.checked;";
    const std::string cbrControlReplacement =
        cbrControlAnchor + "\n"
        "    const bufferInput = document.getElementById('streamCbrBufferMs');\n"
        "    if (bufferInput) bufferInput.disabled = !cbrSupported || !cbrInput.checked;\n"
        "    const sourcePcrInput = document.getElementById('streamPcrSourcePll');\n"
        "    if (sourcePcrInput) sourcePcrInput.disabled = !cbrSupported || !cbrInput.checked;";
    (void)replaceWebUiOnce(html, cbrControlAnchor, cbrControlReplacement);

    const std::string translationAnchor =
        "  ['Target bitrate (кбит/с, для CBR)', 'Target bitrate (kbps, for CBR)'],";
    const std::string translationReplacement =
        translationAnchor + "\n"
        "  ['CBR буфер старта, мс', 'CBR startup buffer, ms'],\n"
        "  ['0 = отключить стартовое накопление; 500–15000 мс — стартовый запас useful TS перед запуском CBR, шаг 500 мс. Для нестабильных спутниковых/HLS входов можно проверить режим 0 мс без изменения физического CBR.', '0 disables startup buffering; 500–15000 ms is the useful-TS startup reservoir before CBR transmission in 500-ms steps. For unstable satellite/HLS inputs, 0 ms can be tested without changing the physical CBR rate.'],\n"
        "  ['Медиатаймлайн по PCR источника', 'Source-PCR media timeline'],\n"
        "  ['Для проблемных CBR-каналов: useful-пакеты идут по скорости source PCR, PCR/PTS/DTS остаются в одном исходном clock domain.', 'For problematic CBR channels: useful packets follow the source-PCR rate and PCR/PTS/DTS stay in one source clock domain.'],";
    (void)replaceWebUiOnce(html, translationAnchor, translationReplacement);
}

} // namespace

std::string HttpServer::renderIndexPage() {
    std::string html = renderIndexPageBase();
    injectSourcePcrTimelineUi(html);
    // V10.8.375: self-contained favicon for Web UI and portable builds.
    // Embedded data URI requires neither a static route nor a separate file.
    static constexpr const char* favicon =
        R"FAVICON(<link rel="icon" type="image/png" sizes="32x32" href="data:image/png;base64,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">)FAVICON";
    const auto headEnd = html.find("</head>");
    if (headEnd != std::string::npos) html.insert(headEnd, favicon);
    return html;
}
