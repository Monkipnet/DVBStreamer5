'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const vm=require('node:vm');
const source=fs.readFileSync(path.resolve(__dirname,
  '../../src/http/HttpServerWebUi.inc'),'utf8');
const start=source.indexOf('function decodeWisiCbrProfile(');
const end=source.indexOf('function streamBitrateMode(',start);
assert.ok(start>=0&&end>start);
const {decodeWisiCbrProfile:decode,encodeWisiCbrTargetBitrate:encode}=
  vm.runInNewContext(source.slice(start,end)+
    '\n({decodeWisiCbrProfile,encodeWisiCbrTargetBitrate});');
assert.equal(decode(9300809).bitrateKbps,9300);
assert.equal(decode(9300809).mode,'source');
assert.equal(decode(9300809).startupMs,5000);
assert.equal(encode(9300,9300809,'source',5000),9300809);
assert.equal(encode(9301,9300809,'source',5000),9301809);
assert.equal(encode(9300,9300809,'fixed',5000),9300709);
assert.equal(encode(9300,9300809,'source',3000),9300777);
assert.equal(encode(9300,9300809,'fixed',3000),9300000);
assert.equal(decode(9300799).startupMs,0);
assert.equal(decode(9300699).startupMs,0);
assert.equal(decode(9300729).startupMs,15000);
assert.equal(decode(9300829).startupMs,15000);
assert.equal(encode(9300,9300123,'fixed',3000),9300123);
assert.equal(encode(7500,7500000,'fixed',3000),7500000);
assert.throws(()=>encode(1,9300809,'source',5000),/Invalid CBR/);
console.log('V367 CBR PCR profile and startup marker persistence: OK');
