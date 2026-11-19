'use strict';
// V586: WISI EPG UDP CBR config, pacing and four-language UI regression.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const read=p=>fs.readFileSync(p,'utf8');
const ui=read('src/http/HttpServerWebUi.inc');
const config=read('src/ConfigManager.cpp');
const hdr=read('src/ConfigManager.h');
const state=read('src/http/HttpServerState.inc');
const sender=read('src/EpgUdpOutput.cpp');
new Function(ui.slice(ui.indexOf('<script>')+8,ui.indexOf('</script>')));
assert.match(hdr,/int bitrateKbps = 3000/);
assert.match(config,/out\["bitrate_kbps"\] = bitrateKbps/);
assert.match(config,/std::clamp\(root\.get\("bitrate_kbps", 3000\)\.asInt\(\), 512, 12000\)/);
assert.match(state,/tile\["bitrate_kbps"\] = output\.bitrateKbps/);
assert.match(ui,/data-wisi-field="bitrate"/);
assert.match(ui,/Number\(get\('bitrate'\)\?\.value\|\|3000\)/);
for(const txt of ['Постоянный битрейт TS','Constant TS bitrate',
 'Stalna brzina TS-a','Konstante TS-Bitrate',
 'Битрейт CBR','CBR bitrate','CBR brzina','CBR-Bitrate'])
 assert.ok(ui.includes(txt),'Missing WISI bitrate translation: '+txt);
assert.match(sender,/std::thread producer/);
assert.match(sender,/std::deque<Packet> pending/);
assert.match(sender,/wake_\.wait_until\(lock, deadline/);
assert.match(sender,/Packet packet = nullPacket\(\)/);
assert.match(sender,/target->pending\.size\(\) \+ packets\.size\(\) >\s*kMaxBufferedPackets/);
assert.match(sender,/target->continuity\+\+ & 0x0fU/);
const version=read('src/AppVersion.h');
const current=version.match(/kProgramVersion = "10\.8\.(\d+)"/);
assert.ok(current&&Number(current[1])>=586,'Requires at least V586');
console.log('PASS V586 WISI EPG UDP CBR pacing, queue bounds, four locales');
