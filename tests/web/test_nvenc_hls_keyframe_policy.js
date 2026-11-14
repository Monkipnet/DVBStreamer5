'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const hardware=fs.readFileSync(path.join(__dirname,'../../src/media/NativeHardwareCodec.cpp'),'utf8');
const zeroCopy=fs.readFileSync(path.join(__dirname,'../../src/media/NativeNvidiaZeroCopy.cpp'),'utf8');
const hls=fs.readFileSync(path.join(__dirname,'../../src/media/NativeHlsSegmenter.cpp'),'utf8');
// Plain NVENC I frames are not necessarily independently decodable. Both
// encoder paths must advertise only forced GOP IDRs or hardware-confirmed IDRs.
for(const [name,source,field,force] of [
  ['CPU-fed NVENC',hardware,'frame','forcedRandomAccess'],
  ['NVDEC/CUDA/NVENC zero-copy',zeroCopy,'encoded','forceRandomAccess']
]){
  const pattern=field+'.keyFrame = '+force+' ||\n            lock.pictureType == NV_ENC_PIC_TYPE_IDR;';
  assert.ok(source.includes(pattern),name+' must mark only true decoder-safe IDR pictures');
  assert.ok(!source.includes('lock.pictureType == NV_ENC_PIC_TYPE_I;'),
    name+' must not declare a non-IDR I frame decoder-safe');
  assert.ok(source.includes('NV_ENC_PIC_FLAG_FORCEIDR'),name+' must request periodic IDRs');
  assert.ok(source.includes('NV_ENC_PIC_FLAG_OUTPUT_SPSPPS'),name+' must repeat decoder config at IDR');
}
assert.ok(hls.includes('if (!(info.hasPcr && info.randomAccess)) return true;'),
  'First independent HLS segment must wait for a random-access PCR');
assert.ok(hls.includes('? info.randomAccess'),
  'Segment rotation must retain random-access boundary for transcoded streams');
console.log('NVENC IDR/HLS random-access flags: OK');
