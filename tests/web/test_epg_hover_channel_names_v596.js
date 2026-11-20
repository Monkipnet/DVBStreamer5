'use strict';
// V596: EPG hover must not cover tile controls; XMLTV IDs stay internal.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const ui=fs.readFileSync('src/http/HttpServerWebUi.inc','utf8');
const cpp=fs.readFileSync('src/StreamManager.cpp','utf8');
const external=fs.readFileSync('src/ExternalXmltv.cpp','utf8');
const version=fs.readFileSync('src/AppVersion.h','utf8');
const start=ui.indexOf('<script>');
const end=ui.indexOf('</script>',start);
assert.ok(start>=0&&end>start);
new vm.Script(ui.slice(start+8,end));
assert.match(version,/kProgramVersion = "10\.8\.(\d+)"/);
assert.ok(Number(version.match(/10\.8\.(\d+)/)?.[1])>=596);
assert.match(ui,/positionEpgHoverBesideTile\(tile\)/);
assert.match(ui,/data-wisi-xmltv-display/);
assert.match(ui,/data-wisi-xmltv-channel/);
assert.match(ui,/wisiChannelDisplayName\(svc\.xmltv_source_id/);
assert.match(cpp,/externalXmltvManager_->channelName\(/);
assert.match(external,/ExternalXmltvManager::channelName\(/);
const helper=ui.slice(ui.indexOf('function positionEpgHoverBesideTile('),
  ui.indexOf('function showEpgHover(',ui.indexOf('function positionEpgHoverBesideTile(')));
const tooltip={offsetWidth:220,offsetHeight:110,
 style:{},classList:{removed:[],remove(s){this.removed.push(s)}},
 setAttribute(key,value){this[key]=value}};
const tile={getBoundingClientRect(){return this.rect}};
const ctx={epgHoverTooltip:tooltip,window:{innerWidth:1000,innerHeight:700}};
vm.createContext(ctx);vm.runInContext(helper,ctx);
tile.rect={left:100,right:300,top:100,bottom:240};
ctx.positionEpgHoverBesideTile(tile);
assert.equal(tooltip.style.left,'312px'); // right side, no action overlap
tile.rect={left:750,right:950,top:100,bottom:240};
ctx.positionEpgHoverBesideTile(tile);
assert.equal(tooltip.style.left,'518px'); // left side
ctx.window.innerWidth=300;
tile.rect={left:35,right:265,top:200,bottom:300};
ctx.positionEpgHoverBesideTile(tile);
assert.ok(Number.parseInt(tooltip.style.top,10)+tooltip.offsetHeight<=tile.rect.top);
ctx.window.innerHeight=310;
tile.rect={left:35,right:265,top:50,bottom:260};
ctx.positionEpgHoverBesideTile(tile);
assert.ok(tooltip.classList.removed.includes('visible')); // hide if nowhere safe
console.log('PASS V596: EPG hover beside tile, safe fallback, XMLTV channel names');
