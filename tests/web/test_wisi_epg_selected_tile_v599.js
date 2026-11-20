'use strict';
// V599: WISI EPG XMLTV metadata must refresh the *open modal*, not the first
// dashboard tile. Exercise the asynchronous path with two distinct outputs.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');

const ui=fs.readFileSync('src/http/HttpServerWebUi.inc','utf8');
const version=fs.readFileSync('src/AppVersion.h','utf8');
const script=ui.slice(ui.indexOf('<script>')+8,ui.indexOf('</script>'));
new vm.Script(script);
assert.match(version,/kProgramVersion = "10\.8\.(\d+)"/);
assert.ok(Number(version.match(/10\.8\.(\d+)/)[1])>=599);
const start=ui.indexOf('async function wisiEpgLoadSource(');
const end=ui.indexOf('function wisiEpgExternalSourceChanged(',start);
assert.ok(start>=0&&end>start,'XMLTV loader extraction');

const firstTile={getAttribute:()=> 'wisi-first'};
const secondModalList={getAttribute:name=>{
 assert.equal(name,'data-wisi-output-id');
 return 'wisi-second';
}};
let modalIsOpen=true;
const selectors=[],shown=[],errors=[],loaded=[];
const ctx={
  Map,Set,Promise,Array,JSON,String,
  wisiXmltvRequests:new Map(),
  wisiXmltvCatalogBySource:new Map(),
  wisiXmltvDisplayNames:new Map(),
  wisiEpgUdpOutputs:[
    {id:'wisi-first',services:[]},
    {id:'wisi-second',services:[
      {xmltv_source_id:'xmltv-two',xmltv_channel_id:'news-two'}
    ]}
  ],
  wisiEpgChannelEditContext:null,
  fetchJson:async url=>{
    assert.equal(url,'/api/xmltv/channels?source_id=xmltv-two');
    loaded.push(url);
    return [{id:'news-two',name:'News Two'}];
  },
  wisiEpgUpdateMatches:()=>{},
  wisiEpgShowChannelList:id=>shown.push(id),
  uiError:value=>errors.push(value),
  document:{
    // A broad query always hits the first dashboard tile.
    querySelector:selector=>{
      if(selector==='[data-wisi-output-id]')return firstTile;
      throw Error('Unexpected broad selector: '+selector);
    },
    getElementById:id=>{
      assert.equal(id,'modalContent');
      if(!modalIsOpen)return null;
      return {querySelector:selector=>{
        selectors.push(selector);
        return selector==='.wisi-epg-channel-list[data-wisi-output-id]'
          ?secondModalList:null;
      }};
    }
  }
};
vm.createContext(ctx);
vm.runInContext(ui.slice(start,end),ctx);
(async()=>{
  await ctx.wisiEpgLoadSource('xmltv-two',true);
  assert.deepEqual(shown,['wisi-second'],'Metadata completion switched to first tile');
  assert.deepEqual(selectors,['.wisi-epg-channel-list[data-wisi-output-id]']);
  assert.equal(ctx.wisiXmltvDisplayNames.get('xmltv-two'+String.fromCharCode(92)+'0news-two'),'News Two');
  assert.equal(loaded.length,1);
  // An edited channel or closed modal must never be overwritten.
  shown.length=0;
  ctx.wisiEpgChannelEditContext={outputId:'wisi-second',index:0};
  await ctx.wisiEpgLoadSource('xmltv-two',true);
  assert.deepEqual(shown,[]);
  ctx.wisiEpgChannelEditContext=null;
  modalIsOpen=false;
  await ctx.wisiEpgLoadSource('xmltv-two',true);
  assert.deepEqual(shown,[]);
  assert.deepEqual(errors,[]);
  console.log('PASS V599: second WISI EPG tile remains selected after XMLTV load; closed/editor modals protected');
})().catch(error=>{console.error(error);process.exitCode=1});
