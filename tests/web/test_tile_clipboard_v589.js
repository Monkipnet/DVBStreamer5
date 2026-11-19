'use strict';
// V589: emulate two servers' authenticated dashboards without a real browser.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const ui=fs.readFileSync('src/http/HttpServerWebUi.inc','utf8');
const helper=fs.readFileSync('src/http/TileClipboardTransfer.h','utf8');
const backend=fs.readFileSync('src/http/HttpServerConfigActions.inc','utf8');
const testStart=ui.indexOf('// V589: one-action paste');
const testEnd=ui.indexOf('function renderWisiEpgTiles(tiles)',testStart);
assert.ok(testStart>0&&testEnd>testStart);
const snippet=ui.slice(testStart,testEnd);
assert.ok(!snippet.includes('openTileClipboardPaste()'));
assert.ok(!snippet.includes('tileClipboardPreview()'));
assert.ok(!snippet.includes('openModal('),'Paste must never show a dialog');
assert.match(ui,/data-mpts-output-id="\$\{escapeHtmlValue\(output.id\)\}"/);
assert.match(helper,/else if \(kind == "mpts"\) output\["tile"\] = scrubMpts/);
assert.match(helper,/kind == "stream" \|\| kind == "wisi_epg" \|\| kind == "mpts"/);
assert.match(backend,/handleTileClipboardExport[\s\S]*kind == "mpts"/);
assert.match(backend,/handleTileClipboardPaste[\s\S]*kind == "mpts"/);
assert.match(backend,/output\.autoStart = false/);
const item={schema:'DVBStreamer5/tile',format_version:1,kind:'mpts',
  tile:{id:'',name:'Mux 01',output_host:'239.1.2.3',output_port:5000,
    auto_start:false,services:[{stream_id:'source-stream-1',service_id:7}]}};
let pending=[];
let addListeners={};
let notices=[];
let saved=[];
const elem={remove(){},set textContent(v){},style:{set cssText(v){} }};
const document={
  addEventListener(name,fn){addListeners[name]=fn;},
  getElementById(){return null;},
  createElement(){return elem;},
  body:{appendChild(){}}
};
const sandbox={document,
  navigator:{clipboard:{readText:async()=>JSON.stringify(item)}},
  window:{isSecureContext:true,setTimeout(){}},
  innerWidth:1200,innerHeight:900,
  wisiText:(ru,en,hr,de)=>ru,
  hideEpgHover(){},
  fetchJson:async(url,options)=>{
    if(url!=='/api/tile-clipboard/paste')throw Error('Unexpected URL '+url);
    const request=JSON.parse(options.body);
    assert.equal(request.action,'apply','Must apply directly without preview');
    saved.push(request);
    return {result:'ok',applied:true,kind:'mpts',warnings:[]};
  },
  fetchState:async()=>{},
  fetchMptsStatus:async()=>{},
  renderMptsList(){},
  uiError(message){throw Error('Unexpected UI error: '+message)},
  fallbackCopyText(){return true},
  console
};
vm.createContext(sandbox);
vm.runInContext(snippet,sandbox,{filename:'v589-clipboard.js'});
(async()=>{
  // Secure context: context-menu Paste reads clipboard, immediately creates tile.
  await vm.runInContext('pasteTileClipboardFromMenu()',sandbox);
  assert.equal(saved.length,1);
  assert.ok(saved[0].item.tile.services[0].stream_id==='source-stream-1');
  // On HTTP, Ctrl+V uses the trusted paste event without readText or a dialog.
  sandbox.window.isSecureContext=false;
  sandbox.navigator.clipboard={};
  let prevented=false;
  addListeners.paste({target:{closest:()=>null},
    clipboardData:{getData:()=>JSON.stringify(item)},preventDefault(){prevented=true;}});
  await new Promise(r=>setImmediate(r));
  assert.equal(prevented,true);
  assert.equal(saved.length,2);
  // Unrelated clipboard content must not be swallowed.
  prevented=false;
  addListeners.paste({target:{closest:()=>null},
    clipboardData:{getData:()=> 'regular text'},preventDefault(){prevented=true;}});
  assert.equal(prevented,false);
  assert.equal(saved.length,2);
  console.log('PASS V589: direct paste, HTTPS clipboard, HTTP Ctrl+V, MPTS, no modal');
})().catch(error=>{console.error(error);process.exitCode=1});
