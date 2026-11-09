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

    // V401: integrate Stream Auth in the existing Access & Integrations submenu;
    // never use a floating button over the broadcast monitoring dashboard.
    static constexpr const char* authMenuAnchor =
        "<button class=\"system-menu-item\" onclick=\"window.location.href='/oscam-mini';closeSystemMenu()\">OSCam-mini</button>";
    const auto authMenuPos = html.find(authMenuAnchor);
    if (authMenuPos != std::string::npos) {
        static constexpr const char* authMenuItems = R"AUTHMENU(
<button type="button" class="system-menu-item" onclick="openDvbAuthSettings();closeSystemMenu()"
 data-ui-ru="Авторизация потоков" data-ui-en="Stream authorization" data-ui-hr="Autorizacija tokova">Авторизация потоков</button>
<button type="button" class="system-menu-item" onclick="openDvbAuthSessions();closeSystemMenu()"
 data-ui-ru="Активные сессии middleware" data-ui-en="Active middleware sessions" data-ui-hr="Aktivne middleware sesije">Активные сессии middleware</button>)AUTHMENU";
        html.insert(authMenuPos + std::char_traits<char>::length(authMenuAnchor), authMenuItems);
    }
    static constexpr const char* middlewareUi = R"AUTHUI(
<style>
/* Must cover the fixed main header (z-index:100000) and its menus.
   This modal is injected at body level, independent of tile stacking. */
#dvbAuthModal,#dvbAuthSessionsModal{display:none;position:fixed;inset:0;z-index:220000;background:rgba(0,0,0,.75);align-items:center;justify-content:center;padding:16px;box-sizing:border-box}
#dvbAuthModal[open],#dvbAuthSessionsModal[open]{display:flex}
#dvbAuthBox,#dvbAuthSessionsBox{box-sizing:border-box;color:#e6f0ff;background:#172230;border:1px solid #405166;border-radius:12px;padding:20px;font:13px system-ui,Arial,sans-serif;box-shadow:0 20px 70px rgba(0,0,0,.45);width:min(520px,100%);max-height:92dvh;overflow:auto}
#dvbAuthSessionsBox{width:min(980px,100%);height:min(720px,90dvh);display:flex;flex-direction:column;overflow:hidden;gap:12px}
/* The session list scrolls independently; the dialog heading/close button
   must stay visible even with hundreds of connected viewers. */
#dvbAuthSessionsBox>.dvb-auth-head{position:sticky;top:0;z-index:2;flex:0 0 auto;background:#172230;padding-bottom:8px}
#dvbAuthSessionsToolbar,#dvbAuthSessionsTotal{flex:0 0 auto}
.dvb-auth-head{display:flex;align-items:center;justify-content:space-between;gap:12px}
.dvb-auth-head h3{margin:0;font-size:17px}
.dvb-auth-head button,.dvb-auth-actions button,#dvbAuthSessionsRefresh{cursor:pointer;border:1px solid #526f92;border-radius:8px;padding:8px 12px;background:#24384f;color:#edf5ff}
.dvb-auth-head button{padding:6px 11px}
#dvbAuthBox label{display:block;margin:12px 0 5px;color:#b8dfff;font-weight:600}
#dvbAuthBox label.dvb-auth-checkbox{color:#e6f0ff;font-weight:400}
#dvbAuthBox input[type=url],#dvbAuthBox input[type=number],#dvbAuthBox input[type=text],#dvbAuthSessionsFilter{box-sizing:border-box;width:100%;min-height:36px;padding:9px;background:#0e1723;color:#e6f0ff;border:1px solid #53637a;border-radius:7px}
#dvbAuthBox small{display:block;color:#b2c4db;margin:9px 0;line-height:1.4}
#dvbAuthApiKey{font:12px ui-monospace,SFMono-Regular,Consolas,monospace;letter-spacing:.02em}
#dvbAuthApiControls{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
#dvbAuthApiControls button{cursor:pointer;border:1px solid #526f92;border-radius:7px;padding:8px 11px;background:#24384f;color:#edf5ff;transition:background .16s ease,border-color .16s ease,box-shadow .16s ease}
#dvbAuthApiControls #dvbAuthApiCopy{background:#146ad0;border:1px solid #52a8ff;color:#fff;font-weight:700;box-shadow:0 0 0 1px rgba(68,158,255,.18)}
#dvbAuthApiControls #dvbAuthApiCopy:not(:disabled):hover{background:#2581e9;border-color:#8dccff;box-shadow:0 0 10px rgba(53,143,243,.45)}
#dvbAuthApiControls #dvbAuthApiCopy[data-copy-state="copied"]{background:#167447;border-color:#5be2a0;color:#fff;box-shadow:0 0 0 2px rgba(31,198,111,.18)}
#dvbAuthApiControls #dvbAuthApiCopy[data-copy-state="error"]{background:#a33839;border-color:#ff8484;color:#fff;box-shadow:0 0 0 2px rgba(228,71,71,.18)}
#dvbAuthApiControls #dvbAuthApiCopy:disabled{background:#283a4c;border-color:#495c70;color:#8797a9;cursor:not-allowed;box-shadow:none}
#dvbAuthApiControls #dvbAuthApiCopy:focus-visible{outline:2px solid #a9d8ff;outline-offset:2px}
#dvbAuthApiKeyStatus[data-copy-state="copied"]{color:#61e5a7;font-weight:600}
#dvbAuthApiKeyStatus[data-copy-state="error"]{color:#ff9a9a;font-weight:600}
#dvbAuthApiKeyStatus{color:#c0d7ee;min-height:1.2em}
.dvb-auth-actions{display:flex;gap:9px;justify-content:flex-end;flex-wrap:wrap;margin-top:16px}
#dvbAuthSave{background:#266bc1;color:#fff}
#dvbAuthStatus,#dvbAuthSessionsStatus{min-height:1.4em;color:#c0d7ee;overflow-wrap:anywhere}
#dvbAuthSessionsToolbar{display:flex;gap:9px;align-items:center;flex-wrap:wrap}
#dvbAuthSessionsFilter{flex:1;min-width:180px}
#dvbAuthSessionsList{flex:1;min-height:0;overflow-y:auto;overscroll-behavior:contain;scrollbar-width:thin;border:1px solid #43556d;border-radius:8px;padding:8px}
.dvb-auth-session{display:grid;grid-template-columns:minmax(90px,1fr) minmax(100px,2fr) minmax(90px,1.2fr) minmax(140px,1.5fr) auto;gap:9px;align-items:center;padding:9px 7px;border-bottom:1px solid #314358;word-break:break-word}
.dvb-auth-session:last-child{border-bottom:0}
.dvb-auth-session .dvb-auth-protocol{font-weight:700;color:#80cbff;text-transform:uppercase}
.dvb-auth-session .dvb-auth-meta{color:#b6c6dc;font-size:11px}
.dvb-auth-session button{border:1px solid #b06464;background:#4d252c;color:#ffe8e8;border-radius:7px;padding:7px 9px;cursor:pointer}
.dvb-auth-session button:disabled{opacity:.5;cursor:wait}
#dvbAuthSessionsTotal{color:#b6c6dc;font-size:12px}
@media(max-width:720px){
 .dvb-auth-session{grid-template-columns:1fr 1fr}
 .dvb-auth-session button{grid-column:1/-1}
 #dvbAuthBox,#dvbAuthSessionsBox{padding:13px}
}
</style>
<div id="dvbAuthModal" role="dialog" aria-modal="true" aria-labelledby="dvbAuthHeading">
<section id="dvbAuthBox">
 <div class="dvb-auth-head"><h3 id="dvbAuthHeading"></h3><button type="button" id="dvbAuthCloseTop" aria-label="Close">×</button></div>
 <label class="dvb-auth-checkbox"><input type="checkbox" id="dvbAuthOn"> <span id="dvbAuthEnabledLabel"></span></label>
 <label for="dvbAuthUrl" id="dvbAuthUrlLabel"></label><input id="dvbAuthUrl" type="url" placeholder="https://auth.example.com/check" autocomplete="off">
 <label for="dvbAuthTimeout" id="dvbAuthTimeoutLabel"></label><input type="number" id="dvbAuthTimeout" min="100" max="3000" step="100">
 <label for="dvbAuthRecheck" id="dvbAuthRecheckLabel"></label><input type="number" id="dvbAuthRecheck" min="1" max="60" step="1">
 <label for="dvbAuthEnd" id="dvbAuthEndLabel"></label><input id="dvbAuthEnd" type="url" placeholder="https://auth.example.com/stop" autocomplete="off">
 <div id="dvbAuthApiControls">
  <label for="dvbAuthApiKey" id="dvbAuthApiKeyLabel"></label>
  <input type="text" id="dvbAuthApiKey" readonly spellcheck="false" autocomplete="off" aria-describedby="dvbAuthApiKeyHelp">
  <button type="button" id="dvbAuthApiGenerate"></button>
  <button type="button" id="dvbAuthApiCopy" disabled></button>
 </div>
 <small id="dvbAuthApiKeyHelp"></small>
 <label for="dvbAuthApiIps" id="dvbAuthApiIpsLabel"></label>
 <input type="text" id="dvbAuthApiIps" autocomplete="off" spellcheck="false" placeholder="127.0.0.1,::1,192.168.200.9">
 <small id="dvbAuthApiIpsHelp"></small>
 <div id="dvbAuthApiKeyStatus" role="status" aria-live="polite"></div>
 <small id="dvbAuthNote"></small><small id="dvbAuthIpNote"></small>
 <div id="dvbAuthStatus" role="status" aria-live="polite"></div>
 <div class="dvb-auth-actions"><button type="button" id="dvbAuthSessions"></button><button type="button" id="dvbAuthSave"></button><button type="button" id="dvbAuthClose"></button></div>
</section>
</div>
<div id="dvbAuthSessionsModal" role="dialog" aria-modal="true" aria-labelledby="dvbAuthSessionsHeading">
<section id="dvbAuthSessionsBox">
 <div class="dvb-auth-head"><h3 id="dvbAuthSessionsHeading"></h3><button type="button" id="dvbAuthSessionsClose" aria-label="Close">×</button></div>
 <div id="dvbAuthSessionsToolbar">
  <input id="dvbAuthSessionsFilter" type="search" autocomplete="off">
  <button type="button" id="dvbAuthSessionsRefresh"></button>
 </div>
 <div id="dvbAuthSessionsTotal"></div>
 <div id="dvbAuthSessionsList" tabindex="0" role="region" aria-label="Active sessions"></div>
 <div id="dvbAuthSessionsStatus" role="status" aria-live="polite"></div>
</section>
</div>
<script>
(function(){
'use strict';
const $=id=>document.getElementById(id);
const modal=$('dvbAuthModal'),sessionsModal=$('dvbAuthSessionsModal');
const words={
ru:{
title:'Авторизация потоков — Cesbo Astra HTTP Backend',enabled:'Включить авторизацию через middleware',
url:'URL сервера авторизации',timeout:'Тайм-аут, мс',recheck:'Повторная проверка токена, мин',
end:'URL уведомления о завершении просмотра (необязательно)',
note:'HTTP / HLS / SRT. Ответ 200 разрешает доступ. При недоступности backend новые подключения запрещены.',
ip:'Когда middleware включён, локальная IP-фильтрация HTTP/HLS/SRT не применяется. Списки IP сохраняются.',
save:'Сохранить',close:'Закрыть',sessions:'Активные сессии',refresh:'Обновить',filter:'Поиск по IP, каналу или протоколу',
disconnect:'Отключить',empty:'Активных сессий нет',ok:'Сохранено без перезапуска потоков',error:'Ошибка',
loading:'Загрузка сессий…',shown:'Показано {shown} из {total}',sessionClosed:'Сессия отключена',more:'Показаны первые 500 записей — уточните поиск',
apiKey:'Ключ API сессий (Telik Server)',apiGenerate:'Сгенерировать новый',apiCopy:'Копировать ключ',
apiIps:'Разрешённые IP для ключа',apiIpsHelp:'Список IP через запятую. Проверяется адрес TCP-клиента, не X-Forwarded-For. По умолчанию доступ только с самого сервера.',
apiKeyHelp:'Ключ разрешает только список и отключение сессий. Старый ключ после замены перестанет работать. Новый ключ показывается один раз.',
apiKeyStored:'Ключ создан (скрыт)',apiKeyMissing:'Ключ ещё не создан',apiKeyCreated:'Сохраните ключ в Telik Server сейчас: повторно показать его невозможно.',
apiKeyPrompt:'Сгенерировать новый ключ? Старый ключ немедленно перестанет работать, а новый будет показан только один раз.',
apiKeyCopied:'✓ Ключ скопирован в буфер обмена',apiKeyCopyError:'Не удалось скопировать автоматически. Ключ выделен: нажмите Ctrl+C.',
apiCopyDone:'✓ Скопировано',apiCopyFailed:'Не скопировано',
apiKeySaveHint:'Сохраните разрешённые IP кнопкой «Сохранить»'
},
en:{
title:'Stream authorization — Cesbo Astra HTTP Backend',enabled:'Enable middleware authorization',
url:'Authorization server URL',timeout:'Timeout, ms',recheck:'Token recheck interval, min',
end:'Session-end notification URL (optional)',
note:'HTTP / HLS / SRT. Status 200 grants access. When backend is down, new connections are denied.',
ip:'When middleware is enabled, local HTTP/HLS/SRT IP filtering is bypassed. IP lists remain saved.',
save:'Save',close:'Close',sessions:'Active sessions',refresh:'Refresh',filter:'Search IP, channel or protocol',
disconnect:'Disconnect',empty:'No active sessions',ok:'Saved without restarting streams',error:'Error',
loading:'Loading sessions…',shown:'Showing {shown} of {total}',sessionClosed:'Session disconnected',more:'Showing first 500 matches — refine your search',
apiKey:'Sessions API key (Telik Server)',apiGenerate:'Generate new',apiCopy:'Copy key',
apiIps:'Allowed IP addresses for API key',apiIpsHelp:'Comma-separated IP addresses. Uses actual TCP peer IP, never X-Forwarded-For. Defaults to local host only.',
apiKeyHelp:'Key grants only session listing and disconnect. Rotation revokes the old key. New key is shown once.',
apiKeyStored:'API key configured (hidden)',apiKeyMissing:'API key not configured',apiKeyCreated:'Store this key in Telik Server now. It cannot be displayed again.',
apiKeyPrompt:'Generate a new key? The old key will be revoked immediately and the new key shown only once.',
apiKeyCopied:'✓ API key copied to clipboard',apiKeyCopyError:'Automatic copy failed. Key is selected: press Ctrl+C.',
apiCopyDone:'✓ Copied',apiCopyFailed:'Copy failed',
apiKeySaveHint:'Save allowed IP addresses with Save'
},
hr:{
title:'Autorizacija tokova — Cesbo Astra HTTP Backend',enabled:'Uključi autorizaciju putem middlewarea',
url:'URL poslužitelja za autorizaciju',timeout:'Vremensko ograničenje, ms',recheck:'Ponovna provjera tokena, min',
end:'URL obavijesti o završetku sesije (neobavezno)',
note:'HTTP / HLS / SRT. Status 200 dopušta pristup. Ako backend nije dostupan, novi pristupi su odbijeni.',
ip:'Kada je middleware uključen, lokalno IP filtriranje za HTTP/HLS/SRT se zaobilazi. IP popisi ostaju spremljeni.',
save:'Spremi',close:'Zatvori',sessions:'Aktivne sesije',refresh:'Osvježi',filter:'Traži IP, kanal ili protokol',
disconnect:'Prekini',empty:'Nema aktivnih sesija',ok:'Spremljeno bez ponovnog pokretanja tokova',error:'Pogreška',
loading:'Učitavanje sesija…',shown:'Prikazano {shown} od {total}',sessionClosed:'Sesija prekinuta',more:'Prikazano prvih 500 zapisa — suzite pretragu',
apiKey:'API ključ sesija (Telik Server)',apiGenerate:'Generiraj novi',apiCopy:'Kopiraj ključ',
apiIps:'Dopuštene IP adrese za ključ',apiIpsHelp:'IP adrese odvojene zarezima. Provjerava se stvarni TCP IP, ne X-Forwarded-For. Zadano je dopušten samo lokalni pristup.',
apiKeyHelp:'Ključ dopušta samo popis i prekid sesija. Novi ključ opoziva stari. Prikazuje se samo jednom.',
apiKeyStored:'API ključ je postavljen (skriven)',apiKeyMissing:'API ključ nije postavljen',apiKeyCreated:'Odmah spremite ključ u Telik Server. Ne može se ponovno prikazati.',
apiKeyPrompt:'Generirati novi ključ? Stari će odmah prestati vrijediti, a novi se prikazuje samo jednom.',
apiKeyCopied:'✓ Ključ je kopiran u međuspremnik',apiKeyCopyError:'Automatsko kopiranje nije uspjelo. Ključ je označen: pritisnite Ctrl+C.',
apiCopyDone:'✓ Kopirano',apiCopyFailed:'Kopiranje nije uspjelo',
apiKeySaveHint:'Spremite dopuštene IP adrese gumbom Spremi'
}
};
let currentSessions=[];
function labels(){
 const lang=localStorage.getItem('dvbstreamer5-language')||document.documentElement.lang||'ru';
 return words[lang]||words.en;
}
function paintLabels(){
 const t=labels();
 const targets={dvbAuthHeading:t.title,dvbAuthEnabledLabel:t.enabled,dvbAuthUrlLabel:t.url,
 dvbAuthTimeoutLabel:t.timeout,dvbAuthRecheckLabel:t.recheck,dvbAuthEndLabel:t.end,
 dvbAuthNote:t.note,dvbAuthIpNote:t.ip,dvbAuthSave:t.save,dvbAuthClose:t.close,
 dvbAuthSessions:t.sessions,dvbAuthSessionsHeading:t.sessions,
 dvbAuthSessionsRefresh:t.refresh,dvbAuthApiKeyLabel:t.apiKey,
 dvbAuthApiGenerate:t.apiGenerate,dvbAuthApiCopy:t.apiCopy,
 dvbAuthApiIpsLabel:t.apiIps,dvbAuthApiKeyHelp:t.apiKeyHelp,
 dvbAuthApiIpsHelp:t.apiIpsHelp};
 for(const id in targets)$(id).textContent=targets[id];
 $('dvbAuthSessionsFilter').placeholder=t.filter;
 $('dvbAuthCloseTop').setAttribute('aria-label',t.close);
 $('dvbAuthSessionsClose').setAttribute('aria-label',t.close);
 $('dvbAuthSessionsList').setAttribute('aria-label',t.sessions);
}
function openDialog(dialog){
 paintLabels();
 dialog.setAttribute('open','');
 const focus=dialog.querySelector('input,button');
 if(focus)focus.focus();
}
function closeDialog(dialog){
 dialog.removeAttribute('open');
 if(dialog===modal){
  // Never retain the one-time raw API key in the DOM after closing.
  $('dvbAuthApiKey').value='';
  $('dvbAuthApiCopy').disabled=true;
  $('dvbAuthApiCopy').removeAttribute('data-copy-state');
  $('dvbAuthApiCopy').textContent=labels().apiCopy;
  $('dvbAuthApiKeyStatus').removeAttribute('data-copy-state');
  $('dvbAuthApiKeyStatus').textContent='';
 }
}
window.openDvbAuthSettings=async function(){
 closeDialog(sessionsModal);
 $('dvbAuthStatus').textContent='';
 openDialog(modal);
 try{
  const response=await fetch('/api/stream-auth',{credentials:'same-origin',cache:'no-store'});
  if(!response.ok)throw Error('HTTP '+response.status);
  const data=await response.json();
  $('dvbAuthOn').checked=!!data.enabled;
  $('dvbAuthUrl').value=data.backend_url||'';
  $('dvbAuthTimeout').value=data.timeout_ms||1200;
  $('dvbAuthRecheck').value=data.recheck_minutes||5;
  $('dvbAuthEnd').value=data.end_url||'';
  $('dvbAuthApiIps').value=data.api_allowed_ips||'127.0.0.1,::1';
  $('dvbAuthApiKey').value='';
  $('dvbAuthApiKey').placeholder=data.api_key_configured?labels().apiKeyStored:labels().apiKeyMissing;
  $('dvbAuthApiCopy').disabled=true;
  $('dvbAuthApiCopy').removeAttribute('data-copy-state');
  $('dvbAuthApiCopy').textContent=labels().apiCopy;
  $('dvbAuthApiKeyStatus').removeAttribute('data-copy-state');
  $('dvbAuthApiKeyStatus').textContent='';
 }catch(error){$('dvbAuthStatus').textContent=labels().error+': '+error.message;}
};
$('dvbAuthClose').addEventListener('click',()=>closeDialog(modal));
$('dvbAuthCloseTop').addEventListener('click',()=>closeDialog(modal));
$('dvbAuthApiGenerate').addEventListener('click',async function(){
 if(!window.confirm(labels().apiKeyPrompt))return;
 this.disabled=true;
 $('dvbAuthApiKeyStatus').textContent='';
 try{
  const r=await fetch('/api/stream-auth/api-key/rotate',{
   method:'POST',credentials:'same-origin',
   headers:{'Content-Type':'application/json','X-Requested-With':'DVBStreamer5'},
   body:'{}',cache:'no-store'
  });
  const data=await r.json();
  if(!r.ok||data.result!=='ok'||!data.api_key)throw Error(data.error||'HTTP '+r.status);
  $('dvbAuthApiKey').value=data.api_key;
  $('dvbAuthApiKey').select();
  $('dvbAuthApiCopy').disabled=false;
  $('dvbAuthApiCopy').removeAttribute('data-copy-state');
  $('dvbAuthApiCopy').textContent=labels().apiCopy;
  $('dvbAuthApiKeyStatus').removeAttribute('data-copy-state');
  $('dvbAuthApiKeyStatus').textContent=labels().apiKeyCreated;
 }catch(error){$('dvbAuthApiKeyStatus').textContent=labels().error+': '+error.message;}
 finally{this.disabled=false;}
});
$('dvbAuthApiCopy').addEventListener('click',async()=>{
 const input=$('dvbAuthApiKey');
 if(!input.value)return;
 const button=$('dvbAuthApiCopy');
 const status=$('dvbAuthApiKeyStatus');
 // Legacy clipboard API works on plain HTTP and must be called directly
 // inside the user's click. A read-only input alone may not be copyable in
 // some browsers, so select a short-lived offscreen textarea instead.
 let copied=false;
 const transient=document.createElement('textarea');
 transient.value=input.value;
 transient.setAttribute('readonly','');
 transient.setAttribute('aria-hidden','true');
 transient.style.cssText='position:fixed;left:-9999px;top:0;opacity:0;pointer-events:none';
 document.body.appendChild(transient);
 try{
  transient.focus();
  transient.select();
  transient.setSelectionRange(0,transient.value.length);
  copied=!!document.execCommand('copy');
 }catch(error){copied=false;}
 finally{transient.remove();}
 // Modern Clipboard API is a second choice when legacy copying is denied.
 if(!copied && window.isSecureContext && navigator.clipboard &&
    typeof navigator.clipboard.writeText==='function'){
  try{
   await navigator.clipboard.writeText(input.value);
   copied=true;
  }catch(error){copied=false;}
 }
 const t=labels();
 if(copied){
  button.setAttribute('data-copy-state','copied');
  button.textContent=t.apiCopyDone;
  status.setAttribute('data-copy-state','copied');
  status.textContent=t.apiKeyCopied;
 }else{
  input.focus();
  input.select();
  input.setSelectionRange(0,input.value.length);
  button.setAttribute('data-copy-state','error');
  button.textContent=t.apiCopyFailed;
  status.setAttribute('data-copy-state','error');
  status.textContent=t.apiKeyCopyError;
 }
});
$('dvbAuthApiIps').addEventListener('input',()=>{
 $('dvbAuthApiKeyStatus').textContent=labels().apiKeySaveHint;
});
$('dvbAuthSave').addEventListener('click',async function(){
 this.disabled=true;
 $('dvbAuthStatus').textContent='';
 try{
  const response=await fetch('/api/stream-auth/save',{
   method:'POST',credentials:'same-origin',
   headers:{'Content-Type':'application/json','X-Requested-With':'DVBStreamer5'},
   body:JSON.stringify({
    enabled:$('dvbAuthOn').checked,
    backend_url:$('dvbAuthUrl').value.trim(),
    timeout_ms:Number($('dvbAuthTimeout').value)||1200,
    recheck_minutes:Number($('dvbAuthRecheck').value)||5,
    end_url:$('dvbAuthEnd').value.trim(),
    api_allowed_ips:$('dvbAuthApiIps').value.trim()
   })
  });
  const data=await response.json();
  if(!response.ok||data.result!=='ok')throw Error(data.error||'HTTP '+response.status);
  $('dvbAuthStatus').textContent=labels().ok;
 }catch(error){$('dvbAuthStatus').textContent=labels().error+': '+error.message;}
 finally{this.disabled=false;}
});
function cell(text,className){
 const div=document.createElement('div');
 if(className)div.className=className;
 div.textContent=String(text??'');
 return div;
}
function renderSessions(){
 const t=labels();
 const filter=$('dvbAuthSessionsFilter').value.trim().toLowerCase();
 const matching=currentSessions.filter(s=>
   [s.ip,s.channel_id,s.protocol,s.viewer].some(v=>String(v||'').toLowerCase().includes(filter)));
 const list=$('dvbAuthSessionsList');
 list.replaceChildren();
 for(const s of matching.slice(0,500)){
  const row=document.createElement('div');row.className='dvb-auth-session';
  row.append(cell(s.protocol||'','dvb-auth-protocol'),
             cell(s.channel_id||''),cell(s.ip||''),
             cell(new Date(Number(s.started_at||0)*1000).toLocaleString()+
                  ' · '+Number(s.connections||0)+' conn.','dvb-auth-meta'));
  const kick=document.createElement('button');kick.type='button';kick.textContent=t.disconnect;
  kick.addEventListener('click',async()=>{
   kick.disabled=true;
   try{
    const r=await fetch('/api/stream-auth/sessions/disconnect',{
      method:'POST',credentials:'same-origin',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({session_id:s.session_id})
    });
    const data=await r.json();
    if(!r.ok||data.result!=='ok')throw Error(data.error||'HTTP '+r.status);
    currentSessions=currentSessions.filter(x=>x.session_id!==s.session_id);
    renderSessions();
    $('dvbAuthSessionsStatus').textContent=t.sessionClosed;
   }catch(error){
    $('dvbAuthSessionsStatus').textContent=t.error+': '+error.message;
    kick.disabled=false;
   }
  });
  row.append(kick);list.append(row);
 }
 if(!matching.length)list.append(cell(t.empty));
 $('dvbAuthSessionsTotal').textContent=t.shown.replace('{shown}',String(Math.min(500,matching.length))).replace('{total}',String(matching.length))
   +(matching.length>500?' · '+t.more:'');
}
async function reloadSessions(){
 $('dvbAuthSessionsStatus').textContent=labels().loading;
 try{
  const response=await fetch('/api/stream-auth/sessions',{credentials:'same-origin',cache:'no-store'});
  if(!response.ok)throw Error('HTTP '+response.status);
  const data=await response.json();
  currentSessions=Array.isArray(data.sessions)?data.sessions:[];
  currentSessions.sort((a,b)=>Number(a.started_at||0)-Number(b.started_at||0));
  renderSessions();
  $('dvbAuthSessionsStatus').textContent='';
 }catch(error){$('dvbAuthSessionsStatus').textContent=labels().error+': '+error.message;}
}
window.openDvbAuthSessions=function(){
 closeDialog(modal);
 $('dvbAuthSessionsFilter').value='';
 currentSessions=[];
 openDialog(sessionsModal);
 reloadSessions();
};
$('dvbAuthSessions').addEventListener('click',window.openDvbAuthSessions);
$('dvbAuthSessionsRefresh').addEventListener('click',reloadSessions);
$('dvbAuthSessionsFilter').addEventListener('input',renderSessions);
$('dvbAuthSessionsClose').addEventListener('click',()=>closeDialog(sessionsModal));
for(const dialog of [modal,sessionsModal]){
 dialog.addEventListener('click',event=>{if(event.target===dialog)closeDialog(dialog);});
}
document.addEventListener('keydown',event=>{
 if(event.key!=='Escape')return;
 if(sessionsModal.hasAttribute('open')){closeDialog(sessionsModal);event.stopPropagation();}
 else if(modal.hasAttribute('open')){closeDialog(modal);event.stopPropagation();}
});
})();
</script>)AUTHUI";
    const auto bodyEnd = html.rfind("</body>");
    if (bodyEnd != std::string::npos) html.insert(bodyEnd, middlewareUi);
    return html;
}
