'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const ui=fs.readFileSync('src/http/HttpServerWebUi.inc','utf8');
const api=fs.readFileSync('src/http/HttpServerDvbMedia.inc','utf8');
const uri=fs.readFileSync('src/http/DvbTransponderUriEditor.h','utf8');
const script=ui.slice(ui.indexOf('<script>')+8,ui.indexOf('</script>'));
new Function(script); // entire UI must parse
function section(start,end){
  const a=ui.indexOf(start),b=ui.indexOf(end,a+start.length);
  assert.ok(a>=0&&b>a,'Missing UI section: '+start);
  return ui.slice(a,b);
}
for(const mark of [
  'function streamTerrestrialMultiplexInfo(',
  'function makeTerrestrialMultiplexGroups(',
  'function editTerrestrialAdapterGroup(',
  'editTerrestrialAdapterGroup(group.key)',
  "grid.className='tile-grid dvb-unconfigured-tiles'",
  "details.dataset.subgroupKey.startsWith('tt:')",
  "key.startsWith('tp:')||key.startsWith('tt:')",
  "key.startsWith('tp:')&&!key.startsWith('tt:')",
  "satBandwidth',tune.bandwidth_hz",
  'initialTransponderTune.delivery_system',
  'editingTerrestrial',
  'beginTransponderGroupScan(',
  'activeTransponderEdit',
  "stream_ids:group.streams.map",
  "tileLayoutMode==='cesbo'",
  'data-cesbo-menu-toggle',
  'function closeCesboTileMenu(restoreFocus=false)',
  "addInputSubgroup(tiles,'tt:'+group.key+':'+mux.key",
  '#tiles .dvb-card-tiles.terrestrial-mux-container',
  "satReceptionType",
  "reception.value=['dvb-t','dvb-t2'].includes("

])assert.ok(ui.includes(mark),'Missing V602 UI integration: '+mark);
for(const mark of [
  'const bool terrestrial=system=="dvb-t" || system=="dvb-t2"',
  'validBandwidth()',
  'original.bandwidthHz!=reference.bandwidthHz',
  'updated.bandwidthHz=tune["bandwidth_hz"].asUInt()',
  'checkedUri.bandwidthHz!=check.bandwidthHz',
  'streamManager.isStreamActive(id)',
  'streamManager.isStopPending(id)',
  'Not all services from this transponder were selected',
  'hasActiveFrontend('
])assert.ok(api.includes(mark),'Missing V602 backend safety invariant: '+mark);
assert.ok(uri.includes('"frequency_khz","bandwidth_hz","symbol_rate"'));
// Each terrestrial MUX is a direct sibling of the satellite transponders.
const terrestrialRender=section('function render(force=false) {',
  '  if(grouped.network.length) {');
assertNonNested(terrestrialRender);
function assertNonNested(source){
  assert.ok(source.includes("addInputSubgroup(tiles,'tt:'"),
    'Terrestrial MUX should be a top-level DVB-S/S2-style spoiler');
  assert.ok(!source.includes("details.className='dvb-card-group'"),
    'A second physical-adapter spoiler must not wrap MUX groups');
}
const info=section('function streamTerrestrialMultiplexInfo(',
  'function makeSatelliteTransponderGroups(');
const terrestrial=s=>s.dvb_input===true&&/delivery_system=dvb-t2?(?:&|$)/.test(s.input_uri);
const builders=new Function('URLSearchParams','streamIsTerrestrialDvb',
  'sortDvbCardStreams','language',info+
  '\nreturn {streamTerrestrialMultiplexInfo,makeTerrestrialMultiplexGroups};')(
  URLSearchParams,terrestrial,(a,b)=>String(a.id).localeCompare(String(b.id)),'en');
const stream=(id,frequency,plp,bandwidth=8000000,delivery='dvb-t2')=>({
 id,dvb_input:true,dvb_adapter:3,dvb_frontend:1,
 input_uri:'dvb://terrestrial?adapter=3&frontend=1&frequency_khz='+frequency+
  '&bandwidth_hz='+bandwidth+'&delivery_system='+delivery+
  '&stream_id='+plp+'&service_id='+(100+Number(id.slice(-1)))
});
const a=stream('tile1',666000,5),b=stream('tile2',666000,5);
const c=stream('tile3',666000,6),d=stream('tile4',674000,5);
const invalid=stream('tile5',1200000,5);
const grouped=builders.makeTerrestrialMultiplexGroups([a,b,c,d,invalid]);
assert.equal(grouped.multiplexes.length,4);
const matched=grouped.multiplexes.find(m=>m.streams.length===2);
assert.ok(matched);
assert.equal(matched.valid,true);
assert.deepEqual(matched.streams.map(s=>s.id),['tile1','tile2']);
assert.match(matched.title,/MUX 666 MHz/);
assert.match(matched.title,/PLP 5/);
const tuneSource=section('function transponderScanTuneFromStream(',
  'function transponderFrontendEquals(');
const readTune=new Function('URLSearchParams','streamTerrestrialMultiplexInfo',
  'streamSatelliteTransponderInfo',tuneSource+'\nreturn transponderScanTuneFromStream;')(
  URLSearchParams,builders.streamTerrestrialMultiplexInfo,()=>null);
assert.equal(readTune(a).adapter,3);
assert.equal(readTune(a).frontend,1);
assert.equal(readTune(a).frequency_mhz,666);
assert.equal(readTune(a).bandwidth_hz,8000000);
assert.equal(readTune(a).stream_id,5);
assert.equal(readTune(invalid),null,'Invalid MUX must not tune any frontend');
const findSrc=section('function findTransponderGroupForScan(',
  'function transponderScanTuneFromStream(');
const card={key:'pci:test-dvb',streams:[a,b,c,d]};
const find=new Function('makeDvbCardGroups','makeSatelliteTransponderGroups',
  'makeTerrestrialMultiplexGroups',findSrc+
  '\nreturn findTransponderGroupForScan;')(
  ()=>({cards:[card]}),()=>({transponders:[]}),
  builders.makeTerrestrialMultiplexGroups);
const selection='tt:'+card.key+':'+matched.key;
assert.equal(find(selection,card.streams).streams.length,2);
assert.equal(find('tt:'+card.key+':missing',card.streams),null);
assert.equal(find('tp:'+card.key+':'+matched.key,card.streams),null);
console.log('PASS V602: DVB-T/T2 edit entry, exact RF/PLP grouping, editor preselection, backend invariants and compact coexistence');
