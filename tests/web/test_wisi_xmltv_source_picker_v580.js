// Emulated large multi-source XMLTV catalogs: the WISI picker must not hide
// channels in later sources or require thousands of <option> DOM nodes.
const fs=require('fs');
const vm=require('vm');
const assert=require('assert');
const page=fs.readFileSync('src/http/HttpServerWebUi.inc','utf8');
const head=fs.readFileSync('src/ExternalXmltv.h','utf8');
const impl=fs.readFileSync('src/ExternalXmltv.cpp','utf8');
const transport=fs.readFileSync('src/http/HttpServerTransport.inc','utf8');
const manager=fs.readFileSync('src/StreamManager.cpp','utf8');
const script=page.slice(page.indexOf('<script>')+8,page.indexOf('</script>'));
new Function(script);
assert.match(page,/data-wisi-xmltv-search/);
assert.match(page,/data-wisi-xmltv-matches/);
assert.match(page,/maxlength="256" data-wisi-xmltv-channel/);
assert.match(page,/function wisiEpgExternalSourceChanged/);
assert.match(page,/\/api\/xmltv\/channels\?source_id=/);
assert.match(page,/wisiXmltvSources=Array\.isArray\(sources\.sources\)/);
assert.doesNotMatch(page,/addLicensedYandexXmltvSource|Yandex TV|Яндекс ТВ|epg-yandex-tv/);
assert.match(page,/wisiXmltvCatalogBySource\.size >= 2/);
assert.match(page,/if\(external&&sourceId\)void wisiEpgLoadSource\(sourceId\)/);
assert.match(page,/!wisiXmltvCatalogBySource\.has\(source\)/);
assert.doesNotMatch(page,/let wisiXmltvCatalog = \[\]/);
assert.match(head,/channels\(const std::string& sourceId = \{\}\) const/);
assert.match(impl,/sourceId != id\) continue/);
assert.match(transport,/target\.rfind\("\/api\/xmltv\/channels\?source_id=", 0\)/);
assert.match(transport,/streamManager\.externalXmltvChannels\(sourceId\)/);
assert.match(manager,/externalXmltvManager_->channels\(sourceId\)/);
for (const label of ['Поиск по названию или ID','Search by name or ID',
                     'Pretraga po nazivu ili ID-u','Nach Name oder ID suchen'])
  assert.ok(page.includes(label), 'Missing locale label: '+label);

const getFunction=(name,next)=>{
  const start=page.indexOf('function '+name+'(');
  const end=page.indexOf('\nfunction '+next+'(',start);
  assert.ok(start>=0 && end>start, 'Cannot extract '+name);
  return page.slice(start,end);
};
const a=Array.from({length:15000},(_,i)=>({id:'a-'+i,name:'Alpha '+i,source_id:'source-a'}));
const b=Array.from({length:15000},(_,i)=>({id:'b-'+i,name:'Beta '+i,source_id:'source-b'}));
const cache=new Map([['source-a',a],['source-b',b]]);
const ctx={wisiXmltvCatalogBySource:cache,escapeHtmlValue:s=>String(s).replace(/&/g,'&amp;')
  .replace(/"/g,'&quot;').replace(/</g,'&lt;').replace(/>/g,'&gt;')};
vm.createContext(ctx);
vm.runInContext(getFunction('wisiEpgMatchOptions','wisiEpgFilterExternal'),ctx);
let html=vm.runInContext("wisiEpgMatchOptions('source-b','Beta 14999','b-14999')",ctx);
assert.ok(html.includes('value="b-14999"'),'Last channel of second source is searchable');
assert.ok(!html.includes('a-14999'),'Source isolation failed');
html=vm.runInContext("wisiEpgMatchOptions('source-a','','')",ctx);
assert.strictEqual((html.match(/<option/g)||[]).length,150,'No more than 150 DOM options');
html=vm.runInContext("wisiEpgMatchOptions('source-a','a-14998','')",ctx);
assert.ok(html.includes('value="a-14998"'),'ID lookup must find late entries');

const add=getFunction('wisiEpgAddExternal','wisiEpgRemoveExternal');
const ctx2={selected:null,wisiEpgEditChannel:(id,index)=>{ctx2.selected={id,index};}};
vm.createContext(ctx2);
vm.runInContext(add+"\nwisiEpgAddExternal('wisi-output-1')",ctx2);
assert.strictEqual(ctx2.selected.id,'wisi-output-1');
assert.strictEqual(ctx2.selected.index,-1,
  'New XMLTV channel must open an empty editor after the tile exists');
assert.match(fs.readFileSync('src/AppVersion.h','utf8'),/kProgramVersion = "10\.8\.580"/);
console.log('PASS WISI XMLTV source picker: 2 x 15,000 channels, search/name/ID, manual entry, four locales, JS syntax');
