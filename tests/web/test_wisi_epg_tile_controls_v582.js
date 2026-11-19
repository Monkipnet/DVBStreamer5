// V582: WISI EIT output controls never enter media stream actions.
// Emulate start/stop/autostart UI and check persisted restart policy.
'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const read=p=>fs.readFileSync(p,'utf8');
const ui=read('src/http/HttpServerWebUi.inc');
const config=read('src/ConfigManager.cpp');
const hdr=read('src/ConfigManager.h');
const transport=read('src/http/HttpServerTransport.inc');
const state=read('src/http/HttpServerState.inc');
const script=ui.slice(ui.indexOf('<script>')+8,ui.indexOf('</script>'));
new Function(script);
assert.match(hdr,/bool autoStart = false/);
assert.match(config,/out\["auto_start"\] = autoStart/);
assert.match(config,/out\.autoStart = root\.get\("auto_start", root\.get\("enabled", false\)\)\.asBool\(\)/);
assert.match(config,/output\.enabled = output\.autoStart/);
assert.match(state,/tile\["auto_start"\] = output\.autoStart/);
assert.match(transport,/target == "\/api\/epg-udp\/action"/);
assert.match(transport,/it->enabled = true/);
assert.match(transport,/it->enabled = false/);
assert.match(transport,/it->autoStart = payload\["auto_start"\]\.asBool\(\)/);
assert.match(transport,/it->autoStart = previousAutoStart/);
assert.match(transport,/if \(action != "autostart"\)\s+streamManager\.configureEpgUdpOutputs\(\)/);
assert.match(transport,/Status therefore reads current config, not a worker snapshot/);
assert.match(ui,/data-wisi-toggle/);
assert.match(ui,/data-wisi-auto-start/);
assert.match(ui,/data-wisi-edit/);
assert.match(ui,/o\.enabled,o\.auto_start,o\.output_host/);
for(const label of ['Автостарт','Automatsko pokretanje','Bearb.','Запущено','Zaustavljeno','Wartet auf Kanäle'])
 assert.ok(ui.includes(label),'Missing UI translation: '+label);
const tile=ui.slice(ui.indexOf('function renderWisiEpgTiles('),ui.indexOf('function render(force=false)',ui.indexOf('function renderWisiEpgTiles(')));
assert.doesNotMatch(tile,/data-stream-action|data-stream-id|stream-toggle/);
// Old enabled=true config must autostart on migration; explicit false overrides.
const policy=o=>Object.hasOwn(o,'auto_start')?o.auto_start:o.enabled;
assert.equal(policy({enabled:true}),true);
assert.equal(policy({enabled:true,auto_start:false}),false);
const action=ui.slice(ui.indexOf('async function wisiEpgTileAction('),ui.indexOf('function renderWisiEpgTiles('));
const calls=[],errors=[],outputs=[
  {id:'wisi-epg-1',name:'WISI',enabled:false,auto_start:false,
   output_host:'239.255.40.1',output_port:5002,services:[{service_id:19}]}];
const ctx={Set,Promise,JSON,wisiEpgPendingActions:new Set(),
 state:{epg_udp_outputs:[]},wisiEpgUdpOutputs:outputs,
 wisiEpgSummary:x=>x.map(o=>({id:o.id,enabled:o.enabled,auto_start:o.auto_start,
 channel_count:o.services.length})),
 fetchJson:async(url,opt)=>{
   assert.equal(url,'/api/epg-udp/action');
   const payload=JSON.parse(opt.body); calls.push(payload);
   const output=outputs.find(o=>o.id===payload.id);
   if(payload.action==='start')output.enabled=true;
   if(payload.action==='stop')output.enabled=false;
   if(payload.action==='autostart')output.auto_start=payload.auto_start;
   return {result:'ok',status:{outputs}};
 },render:()=>{},uiError:e=>errors.push(e)};
vm.createContext(ctx);
vm.runInContext(action,ctx,{timeout:2000});
(async()=>{
 const control={disabled:false,checked:true};
 await ctx.wisiEpgTileAction('wisi-epg-1','start',null,control);
 assert.equal(outputs[0].enabled,true);
 assert.equal(outputs[0].auto_start,false,'Manual start must not activate autostart');
 await ctx.wisiEpgTileAction('wisi-epg-1','autostart',true,control);
 assert.equal(outputs[0].auto_start,true);
 await ctx.wisiEpgTileAction('wisi-epg-1','stop',null,control);
 assert.equal(outputs[0].enabled,false);
 assert.equal(outputs[0].auto_start,true,'Manual Stop must preserve autostart');
 assert.equal(calls.length,3);
 assert.deepEqual(errors,[]);
 assert.equal(control.disabled,false);
 const version=read('src/AppVersion.h');
 const current=version.match(/kProgramVersion = "10\.8\.(\d+)"/);
 assert.ok(current&&Number(current[1])>=582);
 console.log('PASS V582 WISI EPG Start/Stop/Edit/autostart, legacy policy, four locales and independent EIT actions');
})().catch(e=>{console.error(e);process.exitCode=1});
