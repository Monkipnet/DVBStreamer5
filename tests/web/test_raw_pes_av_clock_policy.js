'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const cpp = fs.readFileSync('src/media/NativeTsDemux.cpp', 'utf8');
const header = fs.readFileSync('src/media/NativeTsDemux.h', 'utf8');
const pipeline = fs.readFileSync('src/media/NativeTranscoderPipeline.cpp', 'utf8');

for (const marker of [
  'NATIVE DEMUX RAW AV CLOCK',
  'raw_audio_vs_raw_video_ms=',
  'raw_video_vs_assembled_ms=',
  'raw_audio_vs_assembled_ms=',
  'raw_video_pes_pts=',
  'raw_audio_pes_pts=',
  'assembled_video_au_pts=',
  'raw_video_age_ms=',
  'raw_audio_age_ms=',
  'assembled_video_age_ms=',
  'service_id=',
  'diagnostic_only=1',
  'now - rawAvClockReportAt_ < std::chrono::seconds(5)',
  'const std::uint64_t forward = (a - b) & kMask;',
  'std::int64_t',
  'void NativeTsDemux::reportRawAvClock()',
  'if (sample.hasPts)',
  'rawVideoPesPts90k_ = sample.pts90k;',
  'rawAudioPesPts90k_ = sample.pts90k;',
  'assembledVideoAuPts90k_ = ready.pts90k;',
  'assembledVideoAuPts90k_ = sample.pts90k;'
]) assert.ok(cpp.includes(marker), 'Missing original PES timestamp audit: ' + marker);

for (const field of [
  'rawClockVideoPid_', 'rawClockAudioPid_',
  'rawVideoPesPts90k_', 'rawAudioPesPts90k_', 'assembledVideoAuPts90k_',
  'rawVideoPesAt_', 'rawAudioPesAt_', 'assembledVideoAuAt_',
  'rawAvClockReportAt_'
]) assert.ok(header.includes(field) && cpp.includes(field),
  'Missing isolated raw clock tracking/reset: ' + field);

// Prevent a false conclusion about source skew from post-assembler input
// fields, and prevent a generic mux timing change in this diagnostic release.
assert.ok(cpp.indexOf('rawVideoPesPts90k_ = sample.pts90k;') <
          cpp.indexOf('queueVideoSample(std::move(sample));'));
assert.ok(cpp.includes('if (sampleCallback_) sampleCallback_(std::move(sample));'));
assert.ok(pipeline.includes('ready = withinLead || (holdExpired && withinHardLead);'));
assert.ok(pipeline.includes('reason=queue_full keep_live=1'));

// Verify the wrap-safe PTS arithmetic independently for known cases.
const wrap = (a,b) => {
  const mask = (1n << 33n) - 1n;
  const half = 1n << 32n;
  const d = (BigInt(a)-BigInt(b)) & mask;
  return Number((d >= half ? d-(mask+1n) : d)/90n);
};
assert.equal(wrap(1800000n,0n), 20000);
assert.equal(wrap(0n,1800000n), -20000);
assert.equal(wrap(27000n, (1n<<33n)-18000n), 500);
assert.equal(wrap((1n<<33n)-18000n, 27000n), -500);
console.log('Raw source PES vs assembled H.264 AU clock diagnostics: OK');
