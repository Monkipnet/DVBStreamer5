'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const cpp=fs.readFileSync(path.join(__dirname,'../../src/media/NativeTranscoderPipeline.cpp'),'utf8');
const header=fs.readFileSync(path.join(__dirname,'../../src/media/NativeTranscoderPipeline.h'),'utf8');
// This release deliberately instruments audio starvation without modifying
// established A/V timing bounds, queue depths, timestamps, or drop decisions.
for(const marker of [
  'NATIVE AV MUX PEER STALL',
  'diagnostic_only=1',
  'waited >= std::chrono::seconds(2)',
  'reportNow - lastMuxPeerStallReportAt_ >=',
  '" waiting=" << (haveVideo ? "video" : "audio")',
  '" missing=" << (haveVideo ? "audio" : "video")',
  'lastMuxPeerStallReportAt_ = reportNow',
  'video_producer_idle_ms=',
  'audio_producer_idle_ms=',
  'last_video_produced_pts=',
  'last_audio_produced_pts=',
  'queued_video_head_pts=',
  'queued_audio_head_pts=',
  'input_video_pts=',
  'input_audio_pts=',
  'encoded_audio_pts='
]){
  assert.ok(cpp.includes(marker),'Missing bounded mux peer-stall diagnostic: '+marker);
}
assert.ok(header.includes('std::chrono::steady_clock::time_point lastMuxPeerStallReportAt_{};'));
for(const field of ['lastMuxVideoProducedAt_','lastMuxAudioProducedAt_',
  'lastMuxVideoProducedPts90k_','lastMuxAudioProducedPts90k_']){
  assert.ok(header.includes(field),'Missing per-track producer heartbeat: '+field);
}
assert.ok(cpp.includes('lastMuxVideoProducedAt_ = sample.queuedAt;'));
assert.ok(cpp.includes('lastMuxAudioProducedAt_ = sample.queuedAt;'));
assert.ok(cpp.includes('lastMuxVideoProducedPts90k_ = muxClockOf(sample);'));
assert.ok(cpp.includes('lastMuxAudioProducedPts90k_ = muxClockOf(sample);'));

for(const marker of [
  'static constexpr std::size_t kMaxMuxVideoQueue = 64;',
  'static constexpr std::size_t kMaxMuxAudioQueue = 512;',
  'static constexpr std::uint64_t kMaxAvLead90k = 13500;',
  'static constexpr std::uint64_t kMaxAvHardLead90k = 45000;'
]){
  assert.ok(header.includes(marker),'Mux policy must be unchanged: '+marker);
}
assert.ok(cpp.includes('ready = withinLead || (holdExpired && withinHardLead);'));
assert.ok(cpp.includes('reason=queue_full keep_live=1'));
console.log('A/V peer-stall telemetry and unchanged queue bounds: OK');
