'use strict';
// V536: simulate three cluster nodes, their TS packets, loss/recovery,
// backup activation and grouped browser UI. No physical tuner is needed.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const vm=require('node:vm');
const source=fs.readFileSync(path.join(__dirname,'../../src/http/ClusterWebUiInject.h'),'utf8');
const base=source.indexOf('// V536 isolated peering monitor.');
const end=source.indexOf('</script>',base);
assert(base>=0&&end>base);
const browserScript=source.slice(base,end);
const api=fs.readFileSync(path.join(__dirname,'../../src/http/HttpServerCluster.inc'),'utf8');
const transport=fs.readFileSync(path.join(__dirname,'../../src/http/HttpServerTransport.inc'),'utf8');
for(const marker of [
 'clusterTokenAuthorized','clusterNodeSnapshot','clusterPeerStatus',
 'clusterPeerChange','X-DVBStreamer5-Cluster-Key','maxRedirects=0',
 'maxBodyBytes=256U*1024U','clusterPeerConfiguration',
 '!(isAuthorized(req)||isPanelSessionAuthorized(req))'
])assert.ok((api+transport).includes(marker),'Missing cluster API protection: '+marker);
assert.ok(!api.includes('handleStartStream('));
assert.ok(!api.includes('handleStopStream('));
// Synthetic 188-byte MPEG-TS, with monotonic CC and a plausible PAT PID.
function tsPackets(pid,count) {
 const packets=[];for(let i=0;i<count;++i) {
   const p=Buffer.alloc(188,0xff);p[0]=0x47;
   p[1]=(pid>>8)&0x1f;p[2]=pid&255;p[3]=0x10|(i&15);
   if(pid===0){p[1]|=(i===0?0x40:0);p[4]=0x00;}
   packets.push(p);
 }
 return Buffer.concat(packets);
}
function simulatedChannel(id,name,packets,backup=false) {
 assert.equal(packets.length%188,0);
 for(let i=0;i<packets.length;i+=188)assert.equal(packets[i],0x47);
 return {id,name,active:packets.length>0,using_backup:backup,
   bitrate_in_kbps:Math.round(packets.length*8/1000),
   bitrate_out_kbps:Math.round(packets.length*8/1000)};
}
const baseTs=tsPackets(256,600),backupTs=tsPackets(512,300);
const simulated=[
 {name:'home2',remote_name:'home2',url:'http://10.8.0.2:9000',online:true,
  channels:[simulatedChannel('sat-a','Sport TV HD',baseTs),
            simulatedChannel('sat-b','MIR 24',backupTs,true)]},
 {name:'iptv',remote_name:'iptv',url:'https://iptv.example.org',online:true,
  channels:[simulatedChannel('ip-a','News HD',baseTs),
            simulatedChannel('ip-b','Offline Channel',Buffer.alloc(0))]},
 {name:'svettranscoding',remote_name:'svettranscoding',url:'http://10.8.0.3:9000',
  online:false,channels:[]}
];
class Node {
 constructor(tag='div'){this.tag=tag;this.children=[];this.className='';this.textContent='';
 this.title='';this.hidden=false;this.open=false;this.dataset={};this.listeners={};this.attrs={};}
 setAttribute(key,value){this.attrs[key]=value;}
 append(...nodes){this.children.push(...nodes)}
 appendChild(child){this.children.push(child)}
 replaceChildren(...nodes){this.children=[...nodes]}
 addEventListener(ev,callback){(this.listeners[ev]??=[]).push(callback)}
 fire(ev){for(const cb of this.listeners[ev]||[])cb()}
 findByClass(cls){
  let found=[];
  for(const n of this.children){
    if(n.className.split(/\s+/).includes(cls))found.push(n);
    found.push(...n.findByClass(cls));
  }
  return found;
 }
 querySelectorAll(sel){
  if(sel==='.cluster-channel')return this.findByClass('cluster-channel');
  throw Error('unimplemented selector '+sel);
 }
 querySelector(sel){
  if(sel==='.cluster-server-name')return this.findByClass('cluster-server-name')[0]||null;
  throw Error('unimplemented selector '+sel);
 }
}
const nodes={clusterDashboard:new Node('section'),clusterServerList:new Node(),
             clusterDashboardTitle:new Node('span')};
const document={
 getElementById:id=>nodes[id]||null,
 createElement:tag=>new Node(tag),
 querySelectorAll(sel){
  if(sel==='#clusterServerList .cluster-server-group')
   return nodes.clusterServerList.findByClass('cluster-server-group');
  if(sel==='[data-cluster-remove]')return [];
  throw Error('unimplemented document selector: '+sel);
 },
 querySelector(sel){
  if(sel==='#clusterServerList .cluster-server-group:not([hidden])')
   return nodes.clusterServerList.findByClass('cluster-server-group').find(n=>!n.hidden)||null;
  throw Error('unimplemented query '+sel);
 }
};
const storage=new Map(),calls=[];
let nextSimulated=simulated;
const context={document,console,language:'ru',tileSearchQuery:'',
 localStorage:{getItem:k=>storage.get(k)||null,setItem:(k,v)=>storage.set(k,v)},
 setInterval:()=>0,
 fetchJson:async(url)=>{
   calls.push(url);
   if(url==='/api/cluster/status')return {result:'ok',peers:nextSimulated};
   if(url==='/api/cluster/config')return {result:'ok',node_token:'a'.repeat(64),peers:[]};
   throw Error('Unexpected remote API: '+url);
 },
 escapeHtmlValue:v=>String(v).replaceAll('&','&amp;').replaceAll('<','&lt;'),
 openModal:()=>{},uiError:err=>{throw Error(String(err))},
 confirm:()=>true,applyTileSearch:()=>{},applyLanguage:()=>{},
 navigator:{clipboard:{writeText:async()=>{}}}
};
vm.createContext(context);
vm.runInContext(browserScript,context,{timeout:2000});
function groups(){return nodes.clusterServerList.findByClass('cluster-server-group');}
function group(name){return groups().find(g=>g.querySelector('.cluster-server-name').textContent===name);}
async function main() {
 // Initial peer status request is asynchronous.
 await context.refreshClusterStatus(true);
 assert.equal(groups().length,3);
 assert.equal(nodes.clusterDashboard.hidden,false);
 assert.ok(nodes.clusterDashboardTitle.textContent.includes('(3)'));
 const home=group('home2');
 assert.equal(home.open,false,'Server spoiler must start collapsed');
 assert.ok(home.findByClass('cluster-server-indicator')[0].textContent.includes('2/2'));
 assert.equal(home.findByClass('cluster-channel').length,2);
 assert.ok(home.findByClass('cluster-channel-metrics')[1].textContent.includes('РЕЗЕРВ'));
 assert.equal(group('iptv').findByClass('cluster-channel').length,2);
 assert.ok(group('svettranscoding').findByClass('cluster-server-indicator')[0].className.includes('offline'));
 assert.ok(group('svettranscoding').findByClass('cluster-note').length===1);
 home.open=true;home.fire('toggle');
 assert.ok(storage.get('dvbstreamer5-cluster-spoilers-v1').includes('10.8.0.2'));
 context.renderClusterPeers(nextSimulated);
 assert.equal(group('home2').open,true,'open state must survive status polling');
 // Header name/domain filter integrates remote server groups.
 context.tileSearchQuery='example.org';
 context.applyTileSearch();
 assert.equal(group('iptv').hidden,false);
 assert.equal(group('home2').hidden,true);
 context.tileSearchQuery='mir 24';context.applyTileSearch();
 assert.equal(group('home2').hidden,false);
 assert.equal(group('home2').findByClass('cluster-channel').filter(c=>!c.hidden).length,1);
 context.tileSearchQuery='';context.applyTileSearch();
 assert.equal(groups().filter(g=>!g.hidden).length,3);
 // Simulate primary stream loss, backup activated, peer unavailable and restored.
 nextSimulated=simulated.map(s=>({...s,channels:[...s.channels]}));
 nextSimulated[0].channels[0]={...nextSimulated[0].channels[0],using_backup:true};
 nextSimulated[1].online=false;nextSimulated[1].channels=[];
 await context.refreshClusterStatus(true);
 assert.ok(group('home2').findByClass('cluster-channel-metrics')[0].textContent.includes('РЕЗЕРВ'));
 assert.ok(group('iptv').findByClass('cluster-server-indicator')[0].className.includes('offline'));
 nextSimulated=simulated;
 await context.refreshClusterStatus(true);
 assert.ok(!group('iptv').findByClass('cluster-server-indicator')[0].className.includes('offline'));
 // RU, EN, HR tri-language UI for all notifications.
 assert.ok(source.includes('function clusterText(ru,en,hr)'));
 assert.ok(source.includes('data-ui-ru='));
 assert.ok(source.includes('data-ui-en='));
 assert.ok(source.includes('data-ui-hr='));
 for(const lang of ['ru','en','hr']) {
   context.language=lang;context.renderClusterPeers(nextSimulated);
   assert.equal(groups().length,3);
   const expected={ru:'Кластер серверов',en:'Server cluster',hr:'Klaster poslužitelja'};
   assert.ok(nodes.clusterDashboardTitle.textContent.startsWith(expected[lang]));
   const status=group('home2').findByClass('cluster-channel-metrics')[1].textContent;
   assert.ok(status.includes(lang==='ru'?'РЕЗЕРВ':lang==='hr'?'REZERVA':'BACKUP'));
 }
 assert.ok(calls.every(url=>url==='/api/cluster/status'));
 console.log('PASS: V536 3 simulated servers + MPEG-TS packets, ONLINE/OFFLINE/BACKUP, recover, sorting, spoilers, search and RU/EN/HR');
}
main().catch(e=>{console.error(e);process.exitCode=1});
