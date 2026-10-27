// HttpServer implementation is intentionally partitioned into focused
// implementation fragments. They remain one translation unit so this structural
// refactor cannot change anonymous-namespace visibility, static state, or linkage.
// Keep the include order synchronized with src/http/README.md.
#include "http/HttpServerCommon.inc"
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
    return html;
}
