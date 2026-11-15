'use strict';
// V549: DVB transponder controls match the centered Server Cluster spoiler.
// Simulated browser DOM: editing requires a click on the TP name only.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const source=fs.readFileSync('src/http/HttpServerWebUi.inc','utf8');
function section(begin,end){
  const i=source.indexOf(begin),j=source.indexOf(end,i+begin.length);
  assert(i>=0 && j>i,'missing UI section: '+begin);
  return source.slice(i,j);
}
const code=section('function addInputSubgroup(', 'function updateInputSubgroupCounts(');
const css=section('/* V549: like Server Cluster', '.tp-scan-context{');
new vm.Script(code);
for(const marker of [
  'grid-template-columns:minmax(0,1fr) 56px minmax(0,1fr)',
  '.tp-spoiler-space{grid-column:2;',
  'justify-self:center',
  'pointer-events:none',
  '.tp-spoiler-status{grid-column:3;',
  '.tp-edit-subgroup[open] .tp-spoiler-space:after{content:"▲"}',
  '.tp-spoiler-space:after{content:"▼";'
])assert(css.includes(marker),marker);
for(const label of [
  'Нажмите на название для редактирования',
  'Click the title to edit',
  'Kliknite naziv za uređivanje',
  'Редактировать транспондер после остановки каналов',
  'Edit transponder after stopping channels',
  'Uredi transponder nakon zaustavljanja kanala'
])assert(code.includes(label),label);
class Node {
  constructor(tag){
    this.tag=tag;this.children=[];this.dataset={};this.attributes={};
    this.listeners={};this.classes=new Set();this.className='';
    this.classList={add:name=>this.classes.add(name)};
    this.open=false;this.textContent='';this.title='';
  }
  append(...items){this.children.push(...items);}
  appendChild(node){this.children.push(node);}
  setAttribute(k,v){this.attributes[k]=v;}
  addEventListener(type,fn){this.listeners[type]=fn;}
}
function create(groupKey,language='ru') {
  const parent=new Node('main'),parents=new Map(),ordered=[];
  const scans=[],saved=[];
  const state={
    document:{createElement:tag=>new Node(tag)},
    inputSubgroupOpen:{},tileSearchQuery:'',language,
    t:k=>k==='cardActive'?'active':k,
    subtitleUiTriText:(ru,en,hr)=>language==='ru'?ru:language==='hr'?hr:en,
    beginTransponderGroupScan:key=>scans.push(key),
    rememberInputSubgroup:(key,opened)=>saved.push([key,opened]),
  };
  vm.createContext(state);
  vm.runInContext(code+'\nthis.createGroup=addInputSubgroup;',state);
  state.createGroup(parent,groupKey,'BetaDigital TP 12383 H 27500',
    [{id:'a',active:true},{id:'b',active:false}],parents,ordered);
  return {parent,parents,ordered,scans,saved,state};
}
const key='tp:card:A1:F0:12383';
for(const language of ['ru','en','hr']){
  const ctx=create(key,language);
  const detail=ctx.parent.children[0];
  assert(detail.classes.has('tp-edit-subgroup'));
  assert.equal(detail.tag,'details');
  const summary=detail.children[0];
  assert.equal(summary.tag,'summary');
  const [edit,arrow,status]=summary.children;
  assert.equal(summary.children.length,3);
  assert.equal(edit.tag,'button');
  assert.equal(edit.dataset.tpEditRow,'1');
  assert.equal(edit.children.length,1,'only title may be clickable to edit');
  assert.equal(edit.children[0].textContent,'BetaDigital TP 12383 H 27500');
  assert.equal(arrow.tag,'span');
  assert.equal(arrow.className,'tp-spoiler-space');
  assert.equal(arrow.attributes['aria-hidden'],'true');
  assert.equal(arrow.listeners.click,undefined,'arrow is not an edit button');
  assert.equal(status.tag,'span');
  assert.equal(status.className,'tp-spoiler-status');
  assert.equal(status.children.length,2,'signal metrics and active channel count stay visible');
  assert.equal(status.children[0].dataset.tpMeters,'1');
  assert.equal(status.children[1].dataset.subgroupCount,'1');
  assert.equal(status.listeners.click,undefined,'status is not an edit button');
  assert.equal(detail.open,true);
  assert.equal(ctx.scans.length,0);
  let prevent=false,stop=false;
  edit.listeners.click({preventDefault:()=>prevent=true,stopPropagation:()=>stop=true});
  assert(prevent&&stop,'edit button must not toggle group');
  assert.deepEqual(ctx.scans,[key]);
  assert.equal(detail.open,true);
  detail.open=false;
  detail.listeners.toggle();
  assert.equal(ctx.scans.length,1,'collapse must never begin TP editor');
  detail.open=true;
  detail.listeners.toggle();
  assert.equal(ctx.scans.length,1,'expand must never begin TP editor');
  assert.deepEqual(ctx.saved,[[key,false],[key,true]]);
  assert.equal(ctx.parents.get('a'),detail.children[1]);
  assert.equal(ctx.ordered.length,2);
}
// IP subgroups remain unchanged: network grouping is disclosure-only.
const ip=create('net:192.168.1.0/24');
assert(!ip.parent.children[0].classes.has('tp-edit-subgroup'));
assert.equal(ip.parent.children[0].children[0].children.length,2);
assert.equal(ip.scans.length,0);
console.log('PASS V549 centered DVB arrows, name-only TP editing, meters/status disclosure, RU EN HR, no IP regression');
