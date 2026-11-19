// WISI EIT-only outputs are configuration tiles. They must be creatable with
// zero channels, preserve existing services in the configuration menu and
// expose add/edit/delete operations only after the tile has been created.
const assert=require('assert');
const fs=require('fs');
const vm=require('vm');
const read=name=>fs.readFileSync(name,'utf8');
const ui=read('src/http/HttpServerWebUi.inc');
const api=read('src/http/HttpServerTransport.inc');
const state=read('src/http/HttpServerState.inc');
const sender=read('src/EpgUdpOutput.cpp');
const settings=read('src/ConfigManager.cpp');
const script=ui.slice(ui.indexOf('<script>')+8,ui.indexOf('</script>'));
new Function(script);

assert.match(state,/root\["epg_udp_outputs"\]/);
assert.match(state,/tile\["channel_count"\] = Json::UInt\(output\.services\.size\(\)\)/);
assert.doesNotMatch(state,/wisiEpgTiles\.append\(output\.toJson\(\)\)/);
assert.doesNotMatch(api,/if \(cfg\.services\.empty\(\)\)/);
assert.match(sender,/output\.enabled && !output\.services\.empty\(\)/);
assert.match(settings,/Json::Value EpgUdpOutputConfig::toJson\(\)/);
assert.match(settings,/out\["services"\] = arr/);
assert.match(ui,/function renderWisiEpgTiles\(tiles\)/);
assert.match(ui,/renderWisiEpgTiles\(tiles\);/);
assert.match(ui,/wisi_epg: \(state\.epg_udp_outputs \|\| \[\]\)/);
assert.match(ui,/function wisiEpgShowChannelList\(outputId\)/);
assert.match(ui,/async function wisiEpgOpenTileEditor\(outputId\)/);
assert.match(ui,/function wisiEpgEditChannel\(outputId,index\)/);
assert.match(ui,/async function wisiEpgSaveChannel\(\)/);
assert.match(ui,/async function wisiEpgDeleteChannel\(outputId,index\)/);
assert.match(ui,/wisiEpgCommitOutputs\(outputs\)/);
assert.match(ui,/services:old\.get\(id\)\?\.services\|\|\[\]/);
assert.match(ui,/function wisiEpgAddExternal\(outputId\)/);
assert.match(ui,/function wisiEpgRemoveExternal\(outputId,index\)/);
assert.match(ui,/class="button-danger" type="button" onclick="wisiEpgRemoveExternal/);
assert.match(ui,/class="button-secondary" type="button" onclick="wisiEpgEditChannel/);
assert.doesNotMatch(ui,/!o\.services\.length/);
assert.match(ui,/Каналы добавляются после сохранения через/);
assert.match(ui,/EPG-Kanal bearbeiten/);
assert.match(ui,/Dodaj kanal/);
assert.match(ui,/WISI EPG — отдельные плитки/);

// Emulate a configuration-menu save. A preexisting 2-service output is
// edited without channel fields in the menu; no channels may be lost.
function extract(start,next){
  const a=ui.indexOf('function '+start+'(');
  const b=ui.indexOf('function '+next+'(',a);
  assert.ok(a>=0&&b>a,start+' extraction');
  return ui.slice(a,b);
}
const previous=[{id:'wisi-epg-1',name:'Old',enabled:true,
 output_host:'239.255.1.1',output_port:5002,interface_address:'',
 mode:'barker',services:[
  {stream_id:'',xmltv_source_id:'src1',xmltv_channel_id:'channel-a',
   service_id:100,transport_stream_id:5,original_network_id:10},
  {stream_id:'',xmltv_source_id:'src1',xmltv_channel_id:'channel-b',
   service_id:101,transport_stream_id:5,original_network_id:10}
 ]}];
const inputFields={
 id:{value:'wisi-epg-1'},name:{value:'New WISI name'},
 enabled:{checked:true},host:{value:'239.255.1.9'},
 port:{value:'5500'},iface:{value:'192.168.200.3'},mode:{value:'actual'}
};
const card={querySelector:s=>{
 const match=s.match(/^\[data-wisi-field="([^"]+)"\]$/);
 return match?inputFields[match[1]]:null;
}};
const ctx={wisiEpgUdpOutputs:previous,document:{
 querySelectorAll:q=>q==='[data-wisi-output]'?[card]:[]}};
vm.createContext(ctx);
vm.runInContext(extract('wisiEpgSync','wisiEpgRender')+'\nwisiEpgSync()',ctx);
assert.strictEqual(ctx.wisiEpgUdpOutputs[0].name,'New WISI name');
assert.strictEqual(ctx.wisiEpgUdpOutputs[0].output_port,5500);
assert.strictEqual(ctx.wisiEpgUdpOutputs[0].services.length,2);
assert.strictEqual(ctx.wisiEpgUdpOutputs[0].services[1].xmltv_channel_id,'channel-b');

// No virtual WISI EPG output may enter stream.start/stop or client-preview paths.
const tileFn=ui.slice(ui.indexOf('function renderWisiEpgTiles('),
  ui.indexOf('function render(force=false)',ui.indexOf('function renderWisiEpgTiles(')));
assert.doesNotMatch(tileFn,/data-stream-id|data-stream-action|stream-toggle/);
assert.match(read('src/AppVersion.h'),/kProgramVersion = "10\.8\.580"/);
console.log('PASS virtual WISI EPG tiles: zero-service persistence, separate lifecycle, menu retention, channel editor/list/delete, four locales');
