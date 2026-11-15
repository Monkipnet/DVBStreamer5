'use strict';
// V538: a synthetic DVB-S2 transponder + two services + neighboring
// DVB tuner and a concurrent frontend consumer; no RF hardware required.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const vm=require('node:vm');
const web=fs.readFileSync(path.join(__dirname,'../../src/http/HttpServerWebUi.inc'),'utf8');
function extract(begin,end){
 const a=web.indexOf(begin),b=web.indexOf(end,a+begin.length);
 assert.ok(a>=0&&b>a,'missing JS module '+begin);
 return web.slice(a,b);
}
const code=extract('// V538 TRANSPONDER SCAN ACTION BEGIN',
                   '// V538 TRANSPONDER SCAN ACTION END');
const prefill=extract('// V538: prepopulate the existing satellite scanner',
                      'function openAddChannelModal(');
assert.ok(web.includes("heading.dataset.tpScanTitle='1'"));
assert.ok(web.includes("editRow.addEventListener('click'"));
assert.ok(web.includes("editRow.dataset.tpEditRow='1'"));
assert.ok(web.includes("summary.append(editRow,spoilerSpace)"));
assert.ok(web.includes(".tp-edit-subgroup[open] .tp-spoiler-space:after{content:\"▲\";"));
assert.ok(web.includes(".tp-spoiler-space:after{content:\"▼\";"));
assert.ok(web.includes("editRow.type='button'"));
assert.ok(web.includes("event.preventDefault();"));
assert.ok(web.includes("event.stopPropagation();"));
assert.ok(web.includes("transponderScanPreparing=!!initialTransponderTune"));
assert.ok(web.includes("loadSatelliteAdapters(true).then"));
assert.ok(web.includes("modalGeneration!==satelliteModalGeneration"));
assert.ok(web.includes("if(!suppressInitialSignal)updateSatelliteSignal()"));
assert.ok(web.includes("if(!transponderScanPreparing)updateSatelliteSignal()"));
assert.ok(web.includes("searchInput.placeholder=searchLabel"));
for(const label of [
 'Поиск канала, устройства, IP, домена или подсети',
 'Search channel, device, IP, domain or subnet',
 'Traži kanal, uređaj, IP, domenu ili podmrežu',
 'Очистить поиск','Clear search','Očisti pretragu',
 'Каналы не найдены','No matching channels','Nema pronađenih kanala',
 'Остановить плитки и открыть сканирование',
 'Stop tiles and open scanning',
 'Zaustavi pločice i otvori skeniranje',
 'Вещание этих каналов прервётся.',
 'Their broadcast will be interrupted.',
 'Emitiranje će se prekinuti.'
])assert.ok(web.includes(label),'Missing locale '+label);
const uri='dvb://adapter1/frontend0?frequency_khz=12383000&symbol_rate=27500'+
 '&polarity=H&delivery_system=dvb-s2&modulation=8psk&fec=3/4'+
 '&stream_id=0&diseqc_source=1&satellite_name=BetaDigital'+
 '&lnb_lof1_khz=9750000&lnb_lof2_khz=10600000&lnb_slof_khz=11700000';
const first={id:'sat-1',name:'Discovery HD',dvb_input:true,dvb_adapter:1,dvb_frontend:0,
 tpKey:'TP-A',input_uri:uri,active:true,stop_pending:false};
const second={...first,id:'sat-2',name:'HISTORY Channel'};
const off={...first,id:'sat-3',name:'Sky Showcase',active:false};
const anotherTuner={...first,id:'sat-4',name:'Warner TV Comedy',dvb_adapter:2,dvb_frontend:0,
 tpKey:'TP-B',active:true};
const competing={...first,id:'sat-5',name:'Different TP',tpKey:'TP-B',
 input_uri:uri.replace('12383000','11912000'),active:true};
const groupKey='tp:pci:example:TP-A';
function simulator(options={}){
 const streams=[{...first},{...second},{...off},{...anotherTuner}];
 if(options.competitor)streams.push({...competing});
 const requests=[],errors=[],opened=[],prompts=[];
 let statePolls=0,frontendPolls=0;
 const context={
   URLSearchParams,Promise,Date,Number,Map,Set,console,
   state:{streams},
   window:{confirm:prompt=>{prompts.push(prompt);return options.confirm!==false;}},
   subtitleUiTriText:(ru,en,hr)=>options.language==='en'?en:options.language==='hr'?hr:ru,
   makeDvbCardGroups:values=>({cards:[{key:'pci:example',
       streams:values.filter(s=>Number(s.dvb_adapter)===1&&Number(s.dvb_frontend)===0)}]}),
   makeSatelliteTransponderGroups:values=>{
      const groups=new Map();
      for(const stream of values){
        const key=stream.tpKey;
        if(!groups.has(key))groups.set(key,{key,title:key==='TP-A'?'BetaDigital 19.2°E · TP 12383 H 27500 · A1/FE0':'Other TP',
          streams:[]});
        groups.get(key).streams.push(stream);
      }
      return {transponders:[...groups.values()]};
   },
   streamSatelliteTransponderInfo:stream=>({
     key:options.invalid?'unconfigured:sat-1':stream.tpKey,title:'BetaDigital TP'}),
   fetchJson:async(url,params)=>{
     requests.push({url,body:params?.body});
     if(url==='/api/stop-stream'){
       const id=JSON.parse(params.body).id;
       if(options.stopFailure===id)return {result:'error',error:'simulated stop error'};
       assert.ok(streams.some(s=>s.id===id));
       return {result:'ok',status:'stopping',already_stopped:false};
     }
     if(url==='/api/state'){
       statePolls++;
       if(!options.stuck && statePolls>=2){
         for(const stream of streams)
           if(['sat-1','sat-2'].includes(stream.id))
             {stream.active=false;stream.stop_pending=false;}
       }
       return {streams};
     }
     if(url==='/api/dvb-adapters'){
       frontendPolls++;
       return {adapters:options.absentFrontend?[]:[{
         adapter:1,frontend:0,
         frontend_active:options.externallyBusy?false:frontendPolls<2,
         external_in_use:!!options.externallyBusy,
         configured_streams:3,
         consumers:frontendPolls<2?1:0
       }]};
     }
     throw Error('Unexpected API '+url);
   },
   openAddChannelModal:tune=>{
     assert.ok(statePolls>=2,'Scan opened before stop verified');
     assert.ok(frontendPolls>=2,'Scan opened before frontend release');
     opened.push(tune);
   },
   fetchState:()=>{},
   uiError:e=>errors.push(String(e)),
   setTimeout:(fn)=>Promise.resolve().then(fn)
 };
 vm.createContext(context);
 vm.runInContext(code,context,{timeout:2000});
 return {context,streams,requests,errors,opened,prompts,
  polls:()=>({state:statePolls,frontend:frontendPolls})};
}
async function main(){
 const yes=simulator();
 assert.equal(await yes.context.beginTransponderGroupScan(groupKey),true);
 assert.equal(yes.prompts.length,1);
 assert.equal(yes.opened.length,1);
 assert.deepEqual(yes.requests.filter(r=>r.url==='/api/stop-stream')
   .map(r=>JSON.parse(r.body).id),['sat-1','sat-2']);
 assert.equal(yes.streams.find(s=>s.id==='sat-4').active,true);
 assert.equal(yes.opened[0].adapter,1);
 assert.equal(yes.opened[0].frontend,0);
 assert.equal(yes.opened[0].frequency_mhz,12383);
 assert.equal(yes.opened[0].symbol_rate,27500);
 assert.equal(yes.opened[0].polarity,'H');
 assert.equal(yes.opened[0].stream_id,0);
 assert.equal(yes.opened[0].diseqc_source,1);
 assert.equal(yes.opened[0].satellite_name,'BetaDigital');
 assert.equal(yes.opened[0].group_key,groupKey);
 assert.deepEqual([...yes.opened[0].stream_ids],['sat-1','sat-2','sat-3']);
 assert.ok(yes.polls().state>=2&&yes.polls().frontend>=2);
 const cancel=simulator({confirm:false,language:'en'});
 assert.equal(await cancel.context.beginTransponderGroupScan(groupKey),false);
 assert.equal(cancel.requests.length,0);
 assert.equal(cancel.opened.length,0);
 assert.ok(cancel.prompts[0].includes('broadcast will be interrupted'));
 const conflict=simulator({competitor:true});
 assert.equal(await conflict.context.beginTransponderGroupScan(groupKey),false);
 assert.equal(conflict.opened.length,0);
 assert.equal(conflict.prompts.length,0);
 assert.ok(conflict.errors.join('').includes('занят другими плитками'));
 const failed=simulator({stopFailure:'sat-2'});
 assert.equal(await failed.context.beginTransponderGroupScan(groupKey),false);
 assert.equal(failed.opened.length,0);
 assert.ok(failed.errors.join('').includes('simulated stop error'));
 const external=simulator({externallyBusy:true,language:'hr'});
 assert.equal(await external.context.beginTransponderGroupScan(groupKey),false);
 assert.equal(external.opened.length,0);
 assert.ok(external.errors.join('').includes('Tuner koristi druga aplikacija'));
 const missing=simulator({absentFrontend:true});
 assert.equal(await missing.context.beginTransponderGroupScan(groupKey),false);
 assert.equal(missing.opened.length,0);
 const invalid=simulator({invalid:true});
 assert.equal(await invalid.context.beginTransponderGroupScan(groupKey),false);
 assert.equal(invalid.requests.length,0);
 // Exact prefill into the existing scanner, with no scan POST or stream restart.
 const fields={};
 for(const id of [
  'satAdapter','satFrontend','satName','satFrequency','satSymbolRate',
  'satPolarity','satDeliverySystem','satModulation','satFec','satDiseqc',
  'satStreamId','satUnicableMode','satUnicableUserBand',
  'satUnicableFrequency','satUnicablePosition','satUnicablePin',
  'satLof1','satLof2','satSlof'])fields[id]={value:''};
 const setup={
  document:{getElementById:id=>fields[id]||null},
  dvbAdapters:[{adapter:1,frontend:0,frontend_active:false,external_in_use:false}],
  refreshSatelliteFrontendOptions:v=>{fields.satFrontend.value=String(v)},
  updateSatelliteUnicableUi:()=>{},updateSatelliteDeviceInfo:()=>{},
  Number,String,console
 };
 vm.createContext(setup);
 vm.runInContext(prefill,setup,{timeout:2000});
 assert.equal(setup.applyTransponderScanTune(yes.opened[0]),true);
 assert.equal(fields.satFrequency.value,'12383');
 assert.equal(fields.satSymbolRate.value,'27500');
 assert.equal(fields.satDeliverySystem.value,'dvb-s2');
 assert.equal(fields.satName.value,'BetaDigital');
 assert.equal(fields.satStreamId.value,'0');
 assert.equal(fields.satDiseqc.value,'1');
 assert.equal(fields.satLof1.value,'9750');
 setup.dvbAdapters[0].external_in_use=true;
 assert.equal(setup.applyTransponderScanTune(yes.opened[0]),false);
 // Test depends on actual operator-visible instructions and stable version.
 assert.match(fs.readFileSync(path.join(__dirname,'../../src/AppVersion.h'),'utf8'),
   /kProgramVersion = "10\.8\.(53[8-9]|5[4-9]\d|[6-9]\d\d)"/);
 console.log('PASS V538: TP editor row, centered spoiler arrow, cancel, queued Stop, frontend release, exact TP context and RU/EN/HR');
}
main().catch(e=>{console.error(e);process.exitCode=1;});
