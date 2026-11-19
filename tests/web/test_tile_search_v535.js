'use strict';
// V535 UI regression and deterministic, network-free search matching.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const path=require('node:path');
const web=fs.readFileSync(path.join(__dirname,'../../src/http/HttpServerWebUi.inc'),'utf8');
const begin='// V535 TILE SEARCH PURE LOGIC BEGIN';
const end='// V535 TILE SEARCH PURE LOGIC END';
assert.ok(web.includes(begin)&&web.includes(end),'Search matching module missing');
const pure=web.split(begin)[1].split(end)[0];
const sandbox={URL};
vm.runInNewContext(pure,sandbox,{timeout:2000});
const match=(stream,query,interfaces=[])=>sandbox.tileSearchMatchesStream(stream,query,interfaces);
const a={
 id:'stream-1',name:'Мир 24 HD',
 input_uri:'udp://239.21.32.44:1234',
 backup_input_uri:'http://backup.tv.example.org/live',
 input_interface_address:'192.168.10.18',
 outputs:[{output_host:'239.40.50.60',output_port:5001}],
 vlc_links:[{url:'https://relay.iptv.example.net/hls/index.m3u8'}]
};
const b={
 id:'stream-2',name:'Sport TV HD',
 input_uri:'srt://transcoder.network.example.net:9000?mode=caller',
 interface_address:'10.8.0.3',
 vlc_link:'http://streaming.other.example.com:9000/live'
};
const dvb={
 id:'sat-1',name:'DVB-S2 Football',dvb_input:true,dvb_adapter:1,dvb_frontend:0,
 dvb_card_key:'pci:0000:03:00.0',
 input_uri:'dvb://satellite?adapter=1&frontend=0&frequency_khz=11785000'
};
const iface=[{name:'enp0s25',address:'192.168.10.18'},
             {name:'enp1s0f0',address:'10.8.0.3'}];
// Case folding and partial channel search, with Cyrillic and Latin.
assert.equal(match(a,'МИР'),true);
assert.equal(match(a,'мир 24'),true);
assert.equal(match(b,'SPORT'),true);
assert.equal(match(b,'несуществующий'),false);
// Input, backup and output domains must be searchable.
assert.equal(match(a,'tv.example.org'),true);
assert.equal(match(a,'backup.tv'),true);
assert.equal(match(a,'iptv.example.net'),true);
assert.equal(match(b,'network.example.net'),true);
assert.equal(match(b,'other.example.com'),true);
assert.equal(match(b,'tv.example.org'),false);
// Partial IP matching, and real subnet membership via CIDR.
assert.equal(match(a,'239.21.32'),true);
assert.equal(match(a,'239.21.32.0/24'),true);
assert.equal(match(a,'239.21.0.0/16'),true);
assert.equal(match(a,'239.40.50.0/24'),true);
assert.equal(match(a,'239.21.33.0/24'),false);
assert.equal(match(b,'10.8.0.0/24'),true);
assert.equal(match(b,'10.8.1.0/24'),false);
assert.equal(match(b,'10.0.0.0/8'),true);
assert.equal(match(b,'10.0.0.0/33'),false);
assert.equal(match(a,'999.1.2.0/24'),false);
// Device and physical-interface names.
assert.equal(match(a,'enp0s25',iface),true);
assert.equal(match(b,'enp1s0f0',iface),true);
assert.equal(match(dvb,'adapter1'),true);
assert.equal(match(dvb,'pci:0000:03:00.0'),true);
assert.equal(match(dvb,'/dev/dvb/adapter1/frontend0'),true);
assert.equal(match(b,'enp0s25',iface),false);
// Clearing must restore matches regardless of search state.
assert.equal(match(a,''),true);
assert.equal(match(b,'  '),true);
// UI wiring, localisation and behaviour must remain in the main dashboard.
for(const marker of [
 'id="tileSearchInput"','id="tileSearchClear"',
 'data-placeholder-ru="Например: Мир 24, 239.1.1.1"',
 'data-placeholder-en="e.g. Sport TV, 239.1.1.1"',
 'data-placeholder-hr="Npr. Sport TV, 239.1.1.1"',
 'data-placeholder-de="Z. B. Sport TV, 239.1.1.1"',
 'oninput="setTileSearch(this.value)"',
 'onclick="clearTileSearch()"',
 'tile.hidden=!matched;',
 'applyTileSearch();',
 'if(!tileSearchQuery)rememberInputSubgroup(key,details.open);',
 'if(tileSearchQuery)return;',
 'section.hidden=filtering',
 'if(!filtering){',
 'details.open=inputSubgroupOpen[details.dataset.subgroupKey]!==false',
 'details.open=dvbCardGroupOpen[details.dataset.cardKey]===true'
])assert.ok(web.includes(marker),'Missing search UI contract: '+marker);
const version=fs.readFileSync(path.join(__dirname,'../../src/AppVersion.h'),'utf8');
const found=version.match(/kProgramVersion = "10[.]8[.]([0-9]+)"/);
assert.ok(found&&Number(found[1])>=535);
console.log('PASS: V535 dashboard instant filter, domains, CIDR, devices, clear and grouping');
