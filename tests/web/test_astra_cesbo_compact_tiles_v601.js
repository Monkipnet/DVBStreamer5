'use strict';
// V601: both compact display and menu behavior are browser-only.
// Test without DVB cards, hardware, servers, or network.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const ui=fs.readFileSync('src/http/HttpServerWebUi.inc','utf8');
const version=fs.readFileSync('src/AppVersion.h','utf8');
// Versions after V609 must keep testing the original compact UI.
const release=version.match(/kProgramVersion = "([0-9]+)[.]([0-9]+)[.]([0-9]+)"/);
assert.ok(release && Number(release[1])===10 && Number(release[2])===8 &&
          Number(release[3])>=601,
          'Compact tiles must remain valid on releases from 10.8.601');

const start=ui.indexOf('<script>'), end=ui.indexOf('</script>',start);
assert.ok(start>0&&end>start,'Embedded dashboard JavaScript');
new Function(ui.slice(start+8,end)); // test whole dashboard JS syntax
const css=ui.slice(0,start);
for(const required of [
  "const tileLayoutStorageKey='dvbstreamer5-tile-layout-v601'",
  "let tileLayoutMode='standard'",
  "if(localStorage.getItem(tileLayoutStorageKey)==='cesbo')",
  "localStorage.setItem(tileLayoutStorageKey,mode)",
  "data-tile-layout=\"standard\"",
  "data-tile-layout=\"cesbo\"",
  "tiles.classList.toggle('tiles-cesbo',tileLayoutMode==='cesbo')",
  "tileLayoutMode,",
  'data-cesbo-menu-toggle',
  'data-role="cesbo-compact-status"',
  'class="cesbo-compact-status"',
  "button.dataset.streamId=id",
  "closeCesboTileMenu();",
  "document.body.appendChild(popup)",
  "window.addEventListener('scroll',closeCesboTileMenu,true)",
  "window.addEventListener('resize',closeCesboTileMenu)",
  'data-ui-de="Kachelansicht"'
]){
  assert.ok(ui.includes(required),'Missing optional compact UI: '+required);
}
for(const required of [
  '#tiles.tiles-cesbo .tile-grid:not(.wisi-epg-tiles)',
  '#tiles.tiles-cesbo .tile[data-stream-id].active',
  '#tiles.tiles-cesbo .tile[data-stream-id] .controls',
  '#tiles.tiles-cesbo .tile[data-stream-id] .tile-actions .delete-button',
  '.cesbo-tile-menu-popup',
  '.cesbo-tile-menu-popup[hidden]',
  '#tiles.tiles-cesbo .wisi-epg-tile'
])assert.ok(css.includes(required),'Missing compact tile style: '+required);
// Dark palette must apply only in compact mode while keeping the light
// compact palette and original standard tiles untouched.
for(const required of [
  'html:not([data-dashboard-theme="light"]) #tiles.tiles-cesbo .tile[data-stream-id]',
  'html:not([data-dashboard-theme="light"]) #tiles.tiles-cesbo .wisi-epg-tile',
  'html:not([data-dashboard-theme="light"]) .cesbo-tile-menu-popup',
  '#tiles.tiles-cesbo .tile[data-stream-id]{'
])assert.ok(css.includes(required),'Missing Astra/Cesbo theme rule: '+required);
assert.ok(!css.includes('html:not([data-dashboard-theme="light"]) .tile[data-stream-id]{'));
// Check that standard mode remains the default: compact styles are always
// scoped to #tiles.tiles-cesbo and both controls already exist for standard.
assert.ok(!css.includes('.tile{height:60px'));
// V602: a running but silent/empty stream shows red rather than healthy green.
assert.match(ui,/tile\.classList\.toggle\('cesbo-no-output'/);
assert.ok(css.includes('.tile[data-stream-id].cesbo-no-output'),
  'Running stream without outgoing bitrate needs a red warning border');
// Tile-display field labels have four explicit translations, including DE.
const fieldBegin=ui.indexOf('const tileVisibilityFields=[');
const fieldEnd=ui.indexOf('];',fieldBegin)+2;
assert.ok(fieldBegin>0 && fieldEnd>fieldBegin);
const tileFields=new Function('return '+ui.slice(fieldBegin,fieldEnd)
  .replace('const tileVisibilityFields=','')+';')();
assert.equal(tileFields.length,17);
assert.ok(tileFields.every(field=>field.length===5&&field.every(value=>typeof value==='string'&&value.length)),
  'All tile-display labels must have RU EN HR DE translations');
assert.ok(ui.includes("language==='de'?field[4]:field[2]"),
  'German tile-display labels must not fall back to English');

assert.match(ui,/function setTileLayoutMode\(mode\)\s*\{/);
assert.match(ui,/function updateStreamTile\(tile, stream\)/);
assert.match(ui,/function renderWisiEpgTiles\(tiles\)/);
// Menu logic is extracted and executed with a fake DOM for both tile kinds.
const menuStart=ui.indexOf('let cesboMenuTrigger=null;');
const menuEnd=ui.indexOf("document.addEventListener('click',event=>",menuStart);
assert.ok(menuStart>0&&menuEnd>menuStart);
const elements=[];
const document={
  getElementById(id){return elements.find(e=>e.id===id)||null;},
  createElement(tag){
    const obj={
      tagName:tag,children:[],dataset:{},style:{},attributes:{},
      offsetWidth:196,offsetHeight:185,
      setAttribute(k,v){this.attributes[k]=v;},
      appendChild(el){this.children.push(el);},
      addEventListener(k,cb){this.listeners??={};this.listeners[k]=cb;},
      querySelector(selector){return this.children[0]||null;},
      querySelectorAll(selector){return this.children;},
      remove(){const i=elements.indexOf(this);if(i>=0)elements.splice(i,1);},
      focus(){this.focused=true;document.activeElement=this;}
    };
    return obj;
  },
  body:{appendChild(node){elements.push(node);}}
};
const window={innerWidth:1280,innerHeight:720};
const stream={id:'channel-1',active:true,bitrate_out_kbps:2200};
const wisi={id:'epg-1',enabled:true,auto_start:false};
const callLog=[];
const tile=(isWisi)=>({
  classList:{contains:()=>isWisi},dataset:isWisi?{wisiOutputId:'epg-1'}:{streamId:'channel-1'},
  querySelector(){return {textContent:'Stop'};}
});
let activeTile=tile(false);
const trigger={
  closest(){return activeTile;},
  attributes:{},setAttribute(k,v){this.attributes[k]=v;},
  getBoundingClientRect(){return {left:1080,right:1104,top:500,bottom:524};},
  focus(){this.focused=true;document.activeElement=this;}
};
const makeMenu=new Function('document','window','streamById','state',
  'hideEpgHover','tileLayoutText','t','wisiEpgTileAction','wisiEpgOpenTileEditor','tileLayoutMode',
  ui.slice(menuStart,menuEnd)+'\nreturn {openCesboTileMenu,closeCesboTileMenu};');
const funcs=makeMenu(document,window,()=>stream,{epg_udp_outputs:[wisi]},
  ()=>callLog.push('hide-epg'),(ru,en)=>en,k=>k,
  (...args)=>callLog.push(args),(...args)=>callLog.push(['edit-wisi',...args]),'cesbo');
funcs.openCesboTileMenu(trigger);
assert.equal(elements.length,1);
assert.equal(elements[0].children.length,5);
assert.deepEqual(elements[0].children.map(b=>b.dataset.streamAction),
 ['toggle','edit','quality','copy','delete']);
assert.ok(Number(elements[0].style.left.replace('px',''))<=window.innerWidth-196-8);
assert.ok(Number(elements[0].style.top.replace('px',''))<=window.innerHeight-185-8);
assert.equal(trigger.attributes['aria-expanded'],'true');
const popup=elements[0];
const pressed=key=>popup.listeners.keydown({
  key,preventDefault(){},stopPropagation(){}
});
pressed('ArrowDown');
assert.equal(document.activeElement,popup.children[1]);
pressed('End');
assert.equal(document.activeElement,popup.children[4]);
pressed('Home');
assert.equal(document.activeElement,popup.children[0]);
pressed('ArrowUp');
assert.equal(document.activeElement,popup.children[4]);
pressed('Escape');
assert.equal(elements.length,0);
assert.equal(trigger.attributes['aria-expanded'],'false');
assert.equal(document.activeElement,trigger,'Escape returns focus to the tile menu button');
// EPG output uses dedicated callback actions, not stream action handlers.
activeTile=tile(true);
funcs.openCesboTileMenu(trigger);
assert.equal(elements[0].children.length,3);
assert.ok(elements[0].children.every(b=>!b.dataset.streamAction));
elements[0].children[2].listeners.click();
assert.deepEqual(callLog.at(-1).slice(0,3),['epg-1','autostart',true]);
assert.equal(elements.length,0);
console.log('PASS V601: default standard, compact tile settings, popup menu actions, WISI EPG isolation');
