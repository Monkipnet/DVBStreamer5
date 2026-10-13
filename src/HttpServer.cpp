// HttpServer implementation is intentionally partitioned into focused
// implementation fragments. They remain one translation unit so this structural
// refactor cannot change anonymous-namespace visibility, static state, or linkage.
// Keep the include order synchronized with src/http/README.md.
#include "http/HttpServerCommon.inc"
#include "http/HttpServerTransport.inc"
#include "http/HttpServerState.inc"
#include "http/HttpServerDvbMedia.inc"
#include "http/HttpServerQuality.inc"
#include "http/HttpServerConfigActions.inc"

// V10.8.176: preserve the existing large WebUI implementation byte-for-byte and
// wrap only its rendered HTML. The private sub-kbps carrier remains compatible
// with V175 +777 source-PCR configs and now also carries the per-stream WISI CBR
// startup reservoir without widening the StreamConfig/StreamManager hot path.
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
    (void)replaceWebUiOnce(
        html,
        "value=\"${Math.round((stream.target_bitrate||2000000)/1000)}\"",
        "value=\"${(()=>{const v=Number(stream.target_bitrate||2000000);const r=v%1000;const encoded=(r===777)||(r>=700&&r<=729)||(r>=800&&r<=829);return Math.round((encoded?v-r:v)/1000);})()}\"");

    const std::string bitrateRowAnchor =
        "        <div class=\"form-row full\"><label>Target bitrate (кбит/с, для CBR)</label>";
    const auto rowStart = html.find(bitrateRowAnchor);
    if (rowStart != std::string::npos) {
        const auto rowEnd = html.find('\n', rowStart);
        if (rowEnd != std::string::npos) {
            const std::string bufferRow =
                "        <div class=\"form-row full\"><label>CBR буфер старта, мс</label>"
                "<input id=\"streamCbrBufferMs\" type=\"number\" min=\"500\" max=\"15000\" step=\"500\" "
                "value=\"${(()=>{const r=Number(stream.target_bitrate||0)%1000;if(r>=700&&r<=729)return (r-700+1)*500;if(r>=800&&r<=829)return (r-800+1)*500;return 3000;})()}\" />"
                "<small>Стартовый запас useful TS перед запуском UDP-CBR. 500–15000 мс, шаг 500 мс; большее значение повышает устойчивость к VBR burst, но увеличивает задержку старта.</small></div>\n";
            const std::string timelineRow =
                "        <div class=\"form-row full\"><label>Медиатаймлайн по PCR источника</label>"
                "<div class=\"checkbox-inline\"><input id=\"streamPcrSourcePll\" type=\"checkbox\" "
                "${(()=>{const r=Number(stream.target_bitrate||0)%1000;return (r===777||(r>=800&&r<=829))?'checked':'';})()} />"
                "<span>Медиатаймлайн по PCR источника</span></div>"
                "<small>Для проблемных CBR-каналов: useful-пакеты идут по скорости source PCR, PCR/PTS/DTS остаются в одном исходном clock domain.</small></div>\n";
            html.insert(rowEnd + 1U, bufferRow + timelineRow);
        }
    }

    // Persist both controls in the existing private sub-kbps carrier. Physical
    // CBR remains the exact whole-kbps value because CbrTsPacer strips it before
    // all transport timing calculations.
    (void)replaceWebUiOnce(
        html,
        "    target_bitrate: Number(document.getElementById('streamBitrate').value) * 1000,",
        "    target_bitrate: (() => { const base=Math.max(100,Math.round(Number(document.getElementById('streamBitrate').value)||2000))*1000; const requested=Number(document.getElementById('streamCbrBufferMs')?.value||3000); const bufferMs=Math.min(15000,Math.max(500,Math.round(requested/500)*500)); const code=Math.min(29,Math.max(0,Math.round(bufferMs/500)-1)); const source=document.getElementById('streamPcrSourcePll')?.checked===true; return base+(source?800:700)+code; })(),");

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
        "  ['Стартовый запас useful TS перед запуском UDP-CBR. 500–15000 мс, шаг 500 мс; большее значение повышает устойчивость к VBR burst, но увеличивает задержку старта.', 'Useful-TS startup reservoir before UDP-CBR transmission. 500–15000 ms in 500-ms steps; larger values tolerate VBR bursts better but increase startup delay.'],\n"
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
