'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const vm=require('node:vm');
const source=fs.readFileSync(path.resolve(__dirname,'../../src/http/HttpServerWebUi.inc'),'utf8');
const start=source.indexOf('<script>');
const end=source.indexOf('</script>',start);
assert.ok(start>=0&&end>start,'Web UI inline script should exist');
assert.doesNotThrow(()=>new vm.Script(source.slice(start+8,end)), 'Inline dashboard JS must parse');
assert.match(source,/onclick="openTileViewModal\(\);closeSystemMenu\(\)"/);
for(const field of ['status','uptime','runtime','badge','signal','quality','output','activeInput','primary','backup','sid','frontend','tuning','ca','decode','bitrateIn','bitrateOut']){
 assert.ok(source.includes("['"+field+"',"),'Missing tile visibility option '+field);
}
assert.match(source,/localStorage\.setItem\(tileVisibilityStorageKey,JSON\.stringify\(tileVisibility\)\)/);
assert.match(source,/localStorage\.removeItem\(tileVisibilityStorageKey\)/);
assert.match(source,/applyTileVisibility\(tile\)/);
assert.match(source,/input\[data-tile-visibility\]/);
console.log('Tile visibility controls, persistence, defaults and JS syntax: OK');
