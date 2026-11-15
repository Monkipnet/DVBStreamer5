'use strict';
// V539: browser-side TP editor under three locales and strict
// selected-TP identity, plus backend permission/stop/rollback invariants.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const vm=require('node:vm');
const root=path.join(__dirname,'../..');
const source=fs.readFileSync(path.join(root,'src/http/HttpServerWebUi.inc'),'utf8');
const api=fs.readFileSync(path.join(root,'src/http/HttpServerDvbMedia.inc'),'utf8');
const transport=fs.readFileSync(path.join(root,'src/http/HttpServerTransport.inc'),'utf8');
function slice(start,end){
 const a=source.indexOf(start),b=source.indexOf(end,a+start.length);
 assert.ok(a>=0&&b>a,start+' not found');
 return source.slice(a,b);
}
for(const marker of [
 "details.classList.add('tp-edit-subgroup')",
 "editRow.dataset.tpEditRow='1'",
    "editRow.append(heading)",
    "status.append(meters,count)",
    ".tp-spoiler-space{grid-column:2;",
 "editRow.addEventListener('click'",
 "event.preventDefault();",
 "event.stopPropagation();",
 "summary.append(editRow,spoilerSpace,status)",
 ".tp-spoiler-space:after{content:\"▼\";",
 ".tp-edit-subgroup[open] .tp-spoiler-space:after{content:\"▲\"",
 "const editGroup=editing ? findTransponderGroupForScan(",
 "initialTransponderTune.group_key,state.streams||[]",
 "group_key:initialTransponderTune.group_key",
 "stream_ids:group.streams.map",
 "id=\"satExistingTransponder\" disabled",
 "activeTransponderEdit.group_key",
 "loadSatelliteAdapters(true,initialTransponderTune)",
 "if(!preferredTune)applySelectedDvbFrontendTuneToForm();",
 "document.getElementById('satAdapter').disabled=true",
 "document.getElementById('satFrontend').disabled=true",
 "document.getElementById('satReceptionType').disabled=true",
 "satelliteModalGeneration",
 "id=\"satSaveTransponderButton\"",
 "onclick=\"saveExistingTransponderTune()\"",
 "oninput=\"setTileSearch(this.value)\"",
 "searchInput.placeholder=searchLabel"
])assert.ok(source.includes(marker),'Missing UI contract: '+marker);
for(const label of [
 'Редактировать транспондер после остановки каналов',
 'Edit transponder after stopping channels',
 'Uredi transponder nakon zaustavljanja kanala',
 'Выбранный транспондер из списка плиток',
 'Selected transponder from tile group',
 'Odabrani transponder iz grupe pločica',
 'Сохранить параметры TP','Save transponder settings','Spremi postavke transpondera',
 'Каналы останутся остановленными',
 'Channels will remain stopped',
 'Kanali će ostati zaustavljeni'
])assert.ok(source.includes(label),'Missing RU EN HR label '+label);
for(const marker of [
 'handleDvbTransponderEdit(', 'expected_input_uri',
 'configManager.config.streams=before',
 'streamManager.isStreamActive(id)',
 'streamManager.isStopPending(id)',
 'hasActiveFrontend(', 'Not all services from this transponder were selected',
 'streamManager.configureAutoStartRecovery()',
 'dvb_tp_editor::editUri('
])assert.ok(api.includes(marker),'Missing server invariant '+marker);
assert.ok(transport.includes('target == "/api/dvb-transponder/edit"'));
assert.ok(transport.includes('apiKeyAttempt ||'));
assert.ok(transport.includes('isPanelSessionAuthorized(req)'));
assert.ok(transport.includes('requestedWith->value()!="DVBStreamer5"'));
// Non-default frontend must be selected BEFORE the scanner could probe.
const loadScript=slice('async function loadSatelliteAdapters(', 'function renderSatelliteServices()');
let selected={adapter:'0',frontend:'0'},autoRestore=0,probes=0,requested;
const fields={
 satDeviceInfo:{textContent:''},satAdapter:{value:'0'},satFrontend:{value:'0'}
};
const devices=[{adapter:0,frontend:0},{adapter:3,frontend:2}];
const context={
 document:{getElementById:id=>fields[id]||null},
 dvbAdapters:[],dvbCapabilities:{},
 fetch:async()=>({json:async()=>({adapters:devices,native_dvb_available:true,
   native_stream_id_supported:true,native_diseqc_source_supported:true})}),
 loadCamClients:async()=>{},
 updateSatelliteCapabilityInfo:()=>{},
 dvbFrontendSupportsReception:()=>true,
 refreshSatelliteAdapterOptions:(adapter,frontend)=>{
   requested={adapter,frontend};fields.satAdapter.value=String(adapter);
   fields.satFrontend.value=String(frontend);
 },
 applySelectedDvbFrontendTuneToForm:()=>{autoRestore++;},
 updateSatelliteDeviceInfo:()=>{},
 updateSatelliteSignal:()=>{probes++;},
 Number,Array,Set,console
};
vm.createContext(context);vm.runInContext(loadScript,context,{timeout:2000});
async function test(){
 await context.loadSatelliteAdapters(true,{adapter:3,frontend:2});
 assert.deepEqual(requested,{adapter:3,frontend:2});
 assert.equal(fields.satAdapter.value,'3');
 assert.equal(fields.satFrontend.value,'2');
 assert.equal(autoRestore,0,'selected TP must not be overwritten with default adapter metadata');
 assert.equal(probes,0,'must not probe wrong frontend while selecting');
 await context.loadSatelliteAdapters();
 assert.equal(autoRestore,1,'normal Add Channel must retain old behavior');
 assert.equal(probes,1);

 const saveScript=slice('async function saveExistingTransponderTune()', 'function openAddChannelModal(');
 const requests=[],errors=[],alerts=[],calls=[];
 const editor={
  activeTransponderEdit:{
   label:'BetaDigital · TP 12383 H 27500 · A3/FE2',
   group_key:'tp:pci:card:TP-12383',
   streams:[
    {id:'sat-one',expected_input_uri:'dvb://satellite?pnr=101&frequency_khz=12383000'},
    {id:'sat-two',expected_input_uri:'dvb://satellite?pnr=202&frequency_khz=12383000'}]
  },satelliteScanning:false,
  document:{getElementById:id=>({
    satSaveTransponderButton:{disabled:false},
    satExistingTransponder:{value:'tp:pci:card:TP-12383'},
    satName:{value:'BetaDigital'}
  })[id]||null},
  satelliteTunePayload:()=>({adapter:3,frontend:2,frequency_mhz:12456,
    symbol_rate:30000,polarity:'V',delivery_system:'dvb-s2',
    modulation:'8psk',fec:'3/4',diseqc_source:1,stream_id:0,
    lnb_lof1_mhz:9750,lnb_lof2_mhz:10600,lnb_slof_mhz:11700,
    unicable_mode:'none',unicable_user_band:-1,
    unicable_frequency_mhz:0,unicable_position:0,unicable_pin:-1}),
  window:{confirm:()=>true,alert:msg=>alerts.push(msg)},
  subtitleUiTriText:(ru,en,hr)=>ru,fetchJson:async(url,opts)=>{
    requests.push({url,data:JSON.parse(opts.body),headers:opts.headers});
    return {result:'ok',changed:2};
  },closeModal:()=>calls.push('closed'),fetchState:async()=>calls.push('refreshed'),
  uiError:msg=>errors.push(msg),Number,String,console
 };
 vm.createContext(editor);vm.runInContext(saveScript,editor,{timeout:2000});
 assert.equal(await editor.saveExistingTransponderTune(),true);
 assert.equal(requests.length,1);
 assert.equal(requests[0].url,'/api/dvb-transponder/edit');
 assert.equal(requests[0].headers['X-Requested-With'],'DVBStreamer5');
 assert.equal(requests[0].data.tune.adapter,3);
 assert.equal(requests[0].data.tune.frontend,2);
 assert.equal(requests[0].data.tune.frequency_mhz,12456);
 assert.equal(requests[0].data.streams.length,2);
 assert.equal(requests[0].data.streams[0].id,'sat-one');
 assert.deepEqual(calls,['closed','refreshed']);
 assert.equal(errors.length,0);
 assert.equal(alerts.length,1);
 editor.window.confirm=()=>false;
 assert.equal(await editor.saveExistingTransponderTune(),false);
 assert.equal(requests.length,1,'cancel must not save');
 editor.window.confirm=()=>true;
 editor.fetchJson=async()=>({result:'error',error:'frontend is occupied'});
 assert.equal(await editor.saveExistingTransponderTune(),false);
 assert.ok(errors.includes('frontend is occupied'));
 console.log('PASS V539: TP row edit/arrow, exact A3/FE2 selection, 2-channel save/cancel/error, RU EN HR and server guards');
}
test().catch(error=>{console.error(error);process.exitCode=1});
