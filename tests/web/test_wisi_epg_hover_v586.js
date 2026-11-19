'use strict';
// V586: WISI EPG passive hover: bounded source, lazy cache and escaped UI.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const read=p=>fs.readFileSync(p,'utf8');
const ui=read('src/http/HttpServerWebUi.inc');
const header=read('src/StreamManager.h');
const stream=read('src/StreamManager.cpp');
const transport=read('src/http/HttpServerTransport.inc');
new Function(ui.slice(ui.indexOf('<script>')+8,ui.indexOf('</script>')));
assert.match(header,/epgUdpPreview\(const std::string& outputId\) const/);
assert.match(stream,/constexpr std::size_t kMaxHoverServices = 8/);
assert.match(stream,/externalXmltvManager_->snapshot\(/);
assert.match(stream,/snapshot = epgSnapshot\(svc\.streamId\)/);
assert.match(stream,/entry\["now"\] = shortEvent/);
assert.match(stream,/entry\["next"\] = shortEvent/);
assert.match(stream,/substr\(0, 180\)/);
assert.match(transport,/target\.rfind\("\/api\/epg-udp\/preview\?output_id=", 0\)/);
assert.match(transport,/streamManager\.epgUdpPreview\(outputId\)/);
assert.match(ui,/tile\.addEventListener\('mouseenter',\(\)=>\{\s*void showWisiEpgHover/);
assert.match(ui,/tile\.addEventListener\('mouseleave',hideEpgHover\)/);
assert.match(ui,/wisiEpgHoverCache\.get\(outputId\)/);
assert.match(ui,/Date\.now\(\)-cached\.time<30000/);
assert.match(ui,/token!==wisiEpgHoverToken\|\|epgHoverTile!==tile/);
for(const txt of ['Нет программы','No programme','Nema programa','Kein Programm',
 'Сейчас','Now','Sada','Jetzt','Далее','Next','Sljedeće','Danach',
 'EPG недоступен','EPG unavailable','EPG nije dostupan','EPG nicht verfügbar'])
 assert.ok(ui.includes(txt),'Missing hover translation '+txt);
const part=ui.slice(ui.indexOf('function wisiEpgHoverHtml('),
 ui.indexOf('function wisiEpgHoverShowContent('));
const ctx={wisiText:(ru,en,hr,de)=>en,formatEpgClock:()=> '12:00',
 escapeHtmlValue:x=>String(x??'').replace(/[&<>"']/g,c=>({
 '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))};
vm.createContext(ctx);
vm.runInContext(part,ctx);
const html=ctx.wisiEpgHoverHtml({name:'WISI & EPG',channel_count:5,
 channels:[{name:'<img src=x onerror=alert(1)>',
 now:{start_utc:123,title:'<script>attack</script>'},
 next:{start_utc:456,title:'Sport TV'}}]});
assert.ok(html.includes('&lt;script&gt;attack&lt;/script&gt;'));
assert.ok(html.includes('&lt;img src=x onerror=alert(1)&gt;'));
assert.ok(!html.includes('<script>'));
assert.ok(html.includes('+4 channels'));
console.log('PASS V586 WISI EPG hover: Now/Next, four locales, escaped, bounded');
