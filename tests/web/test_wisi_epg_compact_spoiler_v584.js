// V584: compact WISI EPG tiles inside a persistent DVB-style spoiler.
// Source-level contracts + JS syntax, independent from real WISI hardware.
'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const ui=fs.readFileSync('src/http/HttpServerWebUi.inc','utf8');
const css=ui.slice(ui.indexOf('<style>'),ui.indexOf('</style>'));
const script=ui.slice(ui.indexOf('<script>')+8,ui.indexOf('</script>'));
new Function(script);
const render=ui.slice(ui.indexOf('function renderWisiEpgTiles(tiles)'),
  ui.indexOf('function render(force=false)',ui.indexOf('function renderWisiEpgTiles(tiles)')));
const filter=ui.slice(ui.indexOf('function applyTileSearch()'),
  ui.indexOf('function updateLiveTiles()',ui.indexOf('function applyTileSearch()')));
assert.match(css,/\.wisi-epg-group\{[^}]*grid-column:1\/-1/);
assert.match(css,/\.wisi-epg-tile\{[^}]*height:184px;min-height:184px/);
assert.match(css,/\.wisi-epg-tile \.controls\{margin-top:auto;flex:0 0 auto;display:grid;/);
assert.match(css,/\.wisi-epg-tile \.controls \.wisi-epg-auto-start\{grid-column:1\/-1/);
assert.match(css,/\.wisi-epg-spoiler-space:after\{content:"▼"/);
assert.match(css,/\.wisi-epg-group\[open\] \.wisi-epg-spoiler-space:after\{content:"▲"/);
assert.match(css,/\.wisi-epg-group>summary:focus-visible/);
assert.match(css,/@media\(max-width:560px\)\{[\s\S]*?\.wisi-epg-group>summary/);
assert.match(render,/const group=document\.createElement\('details'\)/);
assert.match(render,/const summary=document\.createElement\('summary'\)/);
assert.match(render,/heading\.textContent='WISI EPG'/);
assert.doesNotMatch(render,/отдельные плитки|separate tiles|zasebne pločice|separate Kacheln/);
assert.match(render,/group\.open=inputSubgroupOpen\['wisi-epg'\]!==false/);
assert.match(render,/rememberInputSubgroup\('wisi-epg',group\.open\)/);
assert.match(render,/if\(tileSearchQuery\)return;/);
assert.match(render,/summary\.append\(heading,arrow,count\)/);
assert.match(render,/group\.appendChild\(grid\)/);
assert.match(render,/tiles\.appendChild\(group\)/);
for(const action of ['data-wisi-toggle','data-wisi-edit','data-wisi-auto-start'])
  assert.ok(render.includes(action),'Missing tile action: '+action);
assert.match(filter,/for\(const group of tiles\.querySelectorAll\('\.wisi-epg-group'\)\)/);
assert.match(filter,/if\(filtering&&hasMatches\)group\.open=true/);
assert.match(filter,/group\.open=inputSubgroupOpen\['wisi-epg'\]!==false/);
const version=fs.readFileSync('src/AppVersion.h','utf8');
assert.match(version,/kProgramVersion = "10\.8\.585"/);
console.log('PASS V584: compact 184px WISI tiles, bottom controls, centered spoiler arrow, search and persistence');
