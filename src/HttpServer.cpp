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

// V10.8.174: preserve the existing large WebUI implementation byte-for-byte and
// wrap only its rendered HTML. This keeps the PCR experiment isolated from the
// already working dashboard/editor JavaScript.
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

void injectSourcePcrPllUi(std::string& html) {
    // Decode the private +777 bps carrier before presenting bitrate to the user.
    // Normal configured bitrates are whole kbps and therefore remain unchanged.
    (void)replaceWebUiOnce(
        html,
        "value=\"${Math.round((stream.target_bitrate||2000000)/1000)}\"",
        "value=\"${Math.round(((((stream.target_bitrate||2000000)%1000)===777 ? (stream.target_bitrate||2000000)-777 : (stream.target_bitrate||2000000)))/1000)}\"");

    const std::string bitrateRowAnchor =
        "        <div class=\"form-row full\"><label>Target bitrate (кбит/с, для CBR)</label>";
    const auto rowStart = html.find(bitrateRowAnchor);
    if (rowStart != std::string::npos) {
        const auto rowEnd = html.find('\n', rowStart);
        if (rowEnd != std::string::npos) {
            const std::string pllRow =
                "        <div class=\"form-row full\"><label>Синхронизация PCR с источником (PLL)</label>"
                "<div class=\"checkbox-inline\"><input id=\"streamPcrSourcePll\" type=\"checkbox\" "
                "${(((stream.target_bitrate||0)%1000)===777)?'checked':''} />"
                "<span>Синхронизация PCR с источником (PLL)</span></div>"
                "<small>Экспериментальный режим только для проблемных CBR-каналов; остальные остаются fixed-CBR.</small></div>\n";
            html.insert(rowEnd + 1U, pllRow);
        }
    }

    // Persist opt-in without widening StreamConfig/StreamManager in this release.
    // CbrTsPacer removes the marker before every transport timing calculation.
    (void)replaceWebUiOnce(
        html,
        "    target_bitrate: Number(document.getElementById('streamBitrate').value) * 1000,",
        "    target_bitrate: Number(document.getElementById('streamBitrate').value) * 1000 + (document.getElementById('streamPcrSourcePll')?.checked ? 777 : 0),");

    // Add translations only if the current phrase table still contains the
    // existing target-bitrate entry. Missing anchors leave the base UI usable.
    const std::string translationAnchor =
        "  ['Target bitrate (кбит/с, для CBR)', 'Target bitrate (kbps, for CBR)'],";
    const std::string translationReplacement =
        translationAnchor + "\n"
        "  ['Синхронизация PCR с источником (PLL)', 'Source PCR synchronization (PLL)'],\n"
        "  ['Экспериментальный режим только для проблемных CBR-каналов; остальные остаются fixed-CBR.', 'Experimental mode only for problematic CBR channels; all others remain fixed-CBR.'],";
    (void)replaceWebUiOnce(html, translationAnchor, translationReplacement);
}

} // namespace

std::string HttpServer::renderIndexPage() {
    std::string html = renderIndexPageBase();
    injectSourcePcrPllUi(html);
    return html;
}
