'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const vm=require('node:vm');
const source=fs.readFileSync(path.resolve(__dirname,'../../src/http/HttpServerWebUi.inc'),'utf8');
const script=source.slice(source.indexOf('<script>')+8,source.indexOf('</script>',source.indexOf('<script>')));
assert.doesNotThrow(()=>new vm.Script(script,{filename:'DVBStreamer5-WebUi.js'}));
const begin=source.indexOf('function streamDvbCardKey(');
const end=source.indexOf('function dvbCardFrontendEntries(',begin);
assert.ok(begin>=0&&end>begin,'group helper block missing');
const sandbox={URL,URLSearchParams,language:'en',
 subtitleUiTriText:(ru,en)=>en};
const grouping=vm.runInNewContext(source.slice(begin,end)+
 '\n({makeDvbCardGroups,makeSatelliteTransponderGroups,streamSatelliteTransponderInfo,makeNetworkInputGroups,streamInputNetworkGroup});',sandbox);
const sat=(id,{freq='11836000',pol='H',sr='27500',adapter=1,frontend=0,isi=-1,diseqc=0,delivery='dvb-s2'}={})=>({
 id:'sat'+id,name:'SAT '+id,dvb_input:true,dvb_adapter:adapter,dvb_frontend:frontend,
 dvb_card_key:'pci:0000:01:00.0',
 input_uri:'dvb://adapter'+adapter+'/frontend'+frontend+'?'+new URLSearchParams({
 frequency_khz:freq,polarity:pol,symbol_rate:sr,stream_id:isi,
 diseqc_source:diseqc,delivery_system:delivery,satellite_name:'Astra'
 }).toString()
});
const s1=sat(1),s2=sat(2),s3=sat(3,{pol:'V'}),s4=sat(4,{adapter:2}),s5=sat(5,{isi:5});
const ter=sat(6,{delivery:'dvb-t2'});
const card=grouping.makeSatelliteTransponderGroups([s1,s2,s3,s4,s5,ter]);
assert.equal(card.transponders.length,4,'polarity, physical FE and ISI must be distinct');
assert.equal(card.transponders.find(g=>g.streams.length===2).streams.length,2);
assert.equal(card.terrestrial.length,1,'terrestrial channels remain in the card');
assert.equal(grouping.streamSatelliteTransponderInfo(s1).key,
 grouping.streamSatelliteTransponderInfo(s2).key);
assert.notEqual(grouping.streamSatelliteTransponderInfo(s1).key,
 grouping.streamSatelliteTransponderInfo(s3).key);
const missing=sat(8);
missing.input_uri='dvb://adapter1/frontend0';
assert.notEqual(grouping.streamSatelliteTransponderInfo(missing).key,
 grouping.streamSatelliteTransponderInfo(s1).key);
const ip=(id,uri,backup='')=>({id,name:id,dvb_input:false,input_uri:uri,backup_input_uri:backup});
const inputs=[
 ip('udp1','udp://@225.2.2.1:5000','udp://10.55.5.1:5000'),
 ip('srt1','srt://225.2.2.99:6000'),
 ip('other','http://225.2.3.8/live.ts'),
 ip('v6a','http://[2001:db8:abcd:1234::1]:8080/live'),
 ip('v6b','srt://[2001:db8:abcd:1234::2]:9000'),
 ip('dns','https://stream.example.org/live.m3u8'),
 ip('file','file:///var/videos/backup.ts')
];
const groups=grouping.makeNetworkInputGroups(inputs);
assert.equal(groups.length,5,'two IP subnets, one IPv6 subnet, DNS, other');
const sameV4=groups.find(g=>g.key==='ipv4:225.2.2.0/24');
assert.ok(sameV4&&sameV4.streams.length===2,'IPv4 /24 uses primary source');
assert.equal(groups.find(g=>g.key==='ipv6:2001:db8:abcd:1234::/64').streams.length,2);
assert.equal(groups.find(g=>g.key==='dns:stream.example.org').streams.length,1);
assert.equal(groups.find(g=>g.key==='other').streams.length,1);
assert.equal(grouping.makeDvbCardGroups([s1,inputs[0]]).cards.length,1);
assert.equal(grouping.makeDvbCardGroups(inputs).network.length,inputs.length);
for(const marker of ['inputSubgroupStorageKey','updateInputSubgroupCounts(tiles);',
  "addInputSubgroup(tiles,'tp:'","addInputSubgroup(list,'net:'",
  "tiles.classList.toggle('grouped-tiles', streams.length > 0)"])
 assert.ok(source.includes(marker),'missing grouping UI integration: '+marker);
assert.ok(!source.includes("addInputSubgroup(nested,'tp:'"),
  'satellite TP groups must not remain nested inside a DVB card');
for(const marker of ["meters.dataset.tpMeters='1'","[data-tp-meters]","renderDvbFrontendMeters(container,{streams}","function renderDvbFrontendMeters("])
 assert.ok(source.includes(marker),'missing TP signal/quality meter integration: '+marker);
console.log('PASS: satellite transponder and IPv4/IPv6 input subnet tile grouping');
