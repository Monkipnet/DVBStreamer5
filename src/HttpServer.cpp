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
    // V383: optional metadata-only HTTP Backend controls. No stream restart.
    static constexpr const char* middlewareUi = R"AUTHUI(<style>
#dvbAuthBtn{position:fixed;right:16px;bottom:16px;z-index:1099;background:#266bc1;color:white;border:0;border-radius:9px;padding:9px 14px;cursor:pointer;font:13px system-ui}
#dvbAuthModal{display:none;position:fixed;inset:0;z-index:1100;background:#0009;align-items:center;justify-content:center}
#dvbAuthModal[open]{display:flex}
#dvbAuthBox{background:#172230;color:#e6f0ff;border:1px solid #405166;border-radius:12px;padding:20px;width:min(470px,calc(100vw - 40px));font:13px system-ui}
#dvbAuthBox label{display:block;margin:12px 0 4px}
#dvbAuthBox input[type=url],#dvbAuthBox input[type=number]{box-sizing:border-box;width:100%;padding:9px;background:#0e1723;color:white;border:1px solid #53637a;border-radius:6px}
#dvbAuthBox button{margin-top:12px;padding:8px 12px;border:0;border-radius:6px;cursor:pointer}
#dvbAuthBox small{display:block;color:#bbcadc;margin:7px 0}
</style>
<button type="button" id="dvbAuthBtn">Middleware Auth</button>
<div id="dvbAuthModal" role="dialog" aria-modal="true" aria-labelledby="dvbAuthHeading">
<section id="dvbAuthBox"><h3 id="dvbAuthHeading"></h3>
<label><input type="checkbox" id="dvbAuthOn"> <span id="dvbAuthEnabledLabel"></span></label>
<label for="dvbAuthUrl" id="dvbAuthUrlLabel"></label><input id="dvbAuthUrl" type="url" placeholder="https://auth.example.com/check" autocomplete="off">
<label for="dvbAuthTimeout" id="dvbAuthTimeoutLabel"></label><input type="number" id="dvbAuthTimeout" min="100" max="3000" step="100">
<small id="dvbAuthNote"></small><small id="dvbAuthIpNote"></small>
<div id="dvbAuthStatus" role="status"></div>
<button type="button" id="dvbAuthSave"></button>
<button type="button" id="dvbAuthClose"></button>
</section></div>
<script>
(function(){
const ru=(localStorage.getItem('dvbstreamer5-language')||document.documentElement.lang||'ru').startsWith('ru');
const t=ru?{
title:'Авторизация потоков — Cesbo Astra HTTP Backend',enabled:'Включить проверку токенов middleware',
url:'Адрес HTTP Backend',timeout:'Тайм-аут, мс',
note:'HTTP / HLS / SRT. Ответ 200 разрешает доступ. При недоступном middleware новые подключения запрещены.',
ip:'При включении локальная IP-фильтрация для HTTP/HLS/SRT автоматически игнорируется. Списки IP сохраняются.',
save:'Сохранить',close:'Закрыть',ok:'Сохранено без перезапуска потоков',error:'Не удалось сохранить настройки'
}:{
title:'Stream authorization — Cesbo Astra HTTP Backend',enabled:'Enable middleware token authorization',
url:'HTTP Backend URL',timeout:'Timeout, ms',
note:'HTTP / HLS / SRT. Status 200 allows access. New connections are denied if middleware is unavailable.',
ip:'When enabled, local IP filtering is bypassed for HTTP/HLS/SRT. IP lists are preserved.',
save:'Save',close:'Close',ok:'Saved without restarting streams',error:'Could not save settings'
};
for(const [key,id] of [['title','dvbAuthHeading'],['enabled','dvbAuthEnabledLabel'],['url','dvbAuthUrlLabel'],['timeout','dvbAuthTimeoutLabel'],['note','dvbAuthNote'],['ip','dvbAuthIpNote'],['save','dvbAuthSave'],['close','dvbAuthClose']])
document.getElementById(id).textContent=t[key];
const modal=document.getElementById('dvbAuthModal'),status=document.getElementById('dvbAuthStatus');
document.getElementById('dvbAuthBtn').addEventListener('click',async()=>{
status.textContent='';modal.setAttribute('open','');
try{const r=await fetch('/api/stream-auth',{credentials:'same-origin',cache:'no-store'});if(!r.ok)throw Error('HTTP '+r.status);const d=await r.json();
document.getElementById('dvbAuthOn').checked=!!d.enabled;
document.getElementById('dvbAuthUrl').value=d.backend_url||'';
document.getElementById('dvbAuthTimeout').value=d.timeout_ms||1200;
}catch(e){status.textContent=t.error+': '+e.message;}
});
document.getElementById('dvbAuthClose').addEventListener('click',()=>modal.removeAttribute('open'));
modal.addEventListener('click',e=>{if(e.target===modal)modal.removeAttribute('open')});
document.getElementById('dvbAuthSave').addEventListener('click',async()=>{
const button=document.getElementById('dvbAuthSave');button.disabled=true;status.textContent='';
try{
const r=await fetch('/api/stream-auth/save',{method:'POST',credentials:'same-origin',
headers:{'Content-Type':'application/json'},body:JSON.stringify({
enabled:document.getElementById('dvbAuthOn').checked,
backend_url:document.getElementById('dvbAuthUrl').value.trim(),
timeout_ms:Number(document.getElementById('dvbAuthTimeout').value)||1200})});
const d=await r.json();if(!r.ok||d.result!=='ok')throw Error(d.error||('HTTP '+r.status));
status.textContent=t.ok;
}catch(e){status.textContent=t.error+': '+e.message;}finally{button.disabled=false;}
});
})();
</script>)AUTHUI";
    const auto bodyEnd = html.rfind("</body>");
    if (bodyEnd != std::string::npos) html.insert(bodyEnd, middlewareUi);
    return html;
}
