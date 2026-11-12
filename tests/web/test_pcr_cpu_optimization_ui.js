'use strict';
// V10.8.485: protect broadcast UI and realtime CPU/PCR fixes from regression.
const fs=require('node:fs');
const assert=require('node:assert/strict');
const path=require('node:path');
const root=path.resolve(__dirname,'../..');
const read=(p)=>fs.readFileSync(path.join(root,p),'utf8');
const web=read('src/http/HttpServerWebUi.inc');
const server=read('src/HttpServer.cpp');
const codec=read('src/media/NativeCodecVendored.cpp');
const mux=read('src/media/NativeMpegTsMux.cpp');
const pipeline=read('src/media/NativeTranscoderPipeline.cpp');
const clock=read('src/media/PcrVbrSourceClock.h');
const pcm=read('src/media/NativeVideoProcessing.cpp');
const mediaTest=read('tests/test_media_core.cpp');

for(const obsolete of [
  'id="streamCbrPcrMode"',
  'id="streamDvbSourcePcrPacing"',
  'id="streamIpSourcePcrEnabled"',
  'id="streamIpSourcePcrRow"',
  'id="streamPcrSourcePll"'
])assert.ok(!web.includes(obsolete),'obsolete tile PCR control: '+obsolete);
assert.ok(!server.includes('injectSourcePcrTimelineUi('),
  'the HTTP server must not inject deprecated PCR editor fields');
for(const retained of [
  'id="streamBitrate"','id="streamCbrStartupMs"',
  'decodeWisiCbrProfile(existingCbrStream?.target_bitrate).mode',
  'existingCbrStream?.ip_source_pcr_passthrough === true',
  'existingCbrStream?.dvb_source_pcr_pacing === true'
])assert.ok(web.includes(retained),'missing safe persistence: '+retained);
assert.ok(mux.includes('previous + 1ULL'),
  'duplicated source PTS/DTS must not add a nominal frame');
assert.ok(mediaTest.includes('repeatMux'),
  'native MPEG-TS mux duplicate/backwards PTS regression test missing');
assert.ok(pipeline.includes('retained_gop=') &&
          pipeline.includes('canKeepRecentGop') &&
          pipeline.includes('std::prev(latestIdr.base())'),
  'PES overflow must retain a decodable IDR-anchored suffix');
assert.ok(clock.includes('std::upper_bound') &&
          !clock.includes('right-1'),
  'PCR lookup must be logarithmic with valid deque iterators');
assert.ok(codec.includes('highComplexity = options_.preset == "quality"'),
  'fast/balanced OpenH264 presets should not unconditionally encode in high complexity');
assert.ok(pcm.includes('if (input.sampleRate == sampleRate && input.channels == channels)'),
  'PCM identity conversion should avoid floating-point resampling');
console.log('PASS realtime PCR/CPU and simplified per-tile CBR editor regression policy');
