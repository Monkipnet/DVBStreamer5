'use strict';
// V548 simulated DOM: server name is the only editor target; centered spoiler
// arrow, status badge and empty header retain native summary expand behavior.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const source=fs.readFileSync('src/http/ClusterWebUiInject.h','utf8');
const start=source.indexOf('function renderClusterPeers(peers) {');
const end=source.indexOf('async function refreshClusterStatus(',start);
assert(start>0&&end>start);
const renderSource=source.slice(start,end);
const style=source.slice(source.indexOf('<style>'),source.indexOf('</style>'));
const script=source.slice(source.indexOf('<script>')+8,source.indexOf('</script>'));
new vm.Script(script);
for(const marker of [
 '.cluster-server-list .cluster-server-group>summary{list-style:none;display:grid;',
 'grid-template-columns:minmax(0,1fr) 56px minmax(0,1fr)',
 '.cluster-server-spoiler{grid-column:2;justify-self:center',
 'pointer-events:none',
 '.cluster-server-indicator{grid-column:3;justify-self:end',
 '.cluster-server-group>summary:before,.cluster-server-group[open]>summary:before{content:none!important}'
])assert(style.includes(marker),'missing centered header style: '+marker);
assert(!renderSource.includes('editRow.append(name,indicator)'));
assert(!renderSource.includes("arrow.addEventListener('click'"));
assert(renderSource.includes("arrow.setAttribute('aria-hidden','true')"));
assert(renderSource.includes('summary.append(editRow,arrow,indicator)'));
class Element {
  constructor(tag){
    this.tag=tag;
    this.children=[];
    this.listeners={};
    this.dataset={};
    this.attributes={};
    this.className='';
    this.open=false;
    this.hidden=false;
    this.textContent='';
  }
  append(...children){this.children.push(...children);}
  appendChild(child){this.children.push(child);}
  replaceChildren(...children){this.children=[...children];}
  addEventListener(type,fn){this.listeners[type]=fn;}
  setAttribute(key,value){this.attributes[key]=value;}
}
const list=new Element('div');
const title=new Element('span');
const edits=[],openState={},saves=[];
const context={
  document:{
    getElementById(id){
      return id==='clusterServerList'?list:id==='clusterDashboardTitle'?title:null;
    },
    createElement(tag){return new Element(tag);}
  },
  language:'ru',
  clusterPeersSnapshot:[],
  clusterGroupOpen:openState,
  tileSearchQuery:'',
  clusterText:(ru,en,hr)=>ru,
  updateClusterLabels:()=>{},
  filterClusterGroups:()=>{},
  persistClusterOpen:()=>saves.push(JSON.stringify(openState)),
  clusterNormalized:v=>String(v||'').toLowerCase(),
  openClusterSettings:key=>edits.push(key)
};
vm.createContext(context);
vm.runInContext(renderSource+'\nthis.renderClusterPeers=renderClusterPeers;',context);
const key='https://blumen.zapto.org:9443';
context.renderClusterPeers([{
  name:'Хорватия',url:key,online:true,channels:[]
}]);
assert.equal(list.children.length,1);
const details=list.children[0],summary=details.children[0];
assert.equal(details.tag,'details');
assert.equal(summary.tag,'summary');
assert.equal(summary.children.length,3);
const [editButton,arrow,indicator]=summary.children;
assert.equal(editButton.tag,'button');
assert.equal(editButton.children.length,1);
assert.equal(editButton.children[0].textContent,'Хорватия');
assert.equal(arrow.tag,'span');
assert.equal(arrow.className,'cluster-server-spoiler');
assert.equal(arrow.attributes['aria-hidden'],'true');
assert.equal(arrow.textContent,'▼');
assert.equal(indicator.tag,'span');
assert(indicator.className.includes('cluster-server-indicator'));
assert.equal(edits.length,0);
let prevented=false,stopped=false;
editButton.listeners.click({
 preventDefault(){prevented=true;},
 stopPropagation(){stopped=true;}
});
assert(prevented&&stopped);
assert.equal(edits.length,1);
assert.equal(edits[0],key);
assert.equal(details.open,false,'name editing must not toggle spoiler');
function nativeToggle(next) {
  details.open=next;
  details.listeners.toggle();
}
nativeToggle(true);
assert.equal(arrow.textContent,'▲');
assert.equal(edits.length,1,'expansion must not open editor');
assert.equal(openState[key],true);
nativeToggle(false);
assert.equal(arrow.textContent,'▼');
assert.equal(edits.length,1,'collapse must not open editor');
assert.equal(openState[key],false);
assert.equal(saves.length,2);
console.log('PASS V548 cluster centered ▲/▼ spoiler, name-only editor, status/blank expand, persistence');
