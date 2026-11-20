'use strict';
// V598: static integration checks complement the standalone C++ filesystem emulator.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const stream=fs.readFileSync('src/StreamManager.cpp','utf8');
const header=fs.readFileSync('src/media/HlsTemporaryCleanup.h','utf8');
const workflow=fs.readFileSync('.github/workflows/build-main-linux.yml','utf8');
const version=fs.readFileSync('src/AppVersion.h','utf8');
for(const expected of [
  'media/HlsTemporaryCleanup.h',
  '!automaticFailoverTransition && !state->config.hlsArchiveEnabled',
  'startingStreamsMutex_',
  'startingStreamIds_.count(id)',
  'streams.find(id) == streams.end()',
  'quarantineStoppedTemporaryHls(',
  'removeQuarantinedTemporaryHls(',
  'isStaleCompletedTemporaryHls(',
  'configured.id == id && configured.hlsArchiveEnabled',
  'NATIVE HLS CLEANUP stream_id=',
  'NATIVE HLS STALE CLEANUP stream_id='
]) assert.ok(stream.includes(expected),'Missing HLS lifecycle guard: '+expected);
for(const expected of [
  '#EXT-X-ENDLIST',
  'std::chrono::hours(24)',
  'safeTemporaryHlsId(',
  'symlink_status(',
  'others_write',
  'std::filesystem::rename(dir, trash, error)'
]) assert.ok(header.includes(expected),'Missing cleanup safety check: '+expected);
assert.ok(workflow.includes('tests/test_hls_temporary_cleanup_v598.cpp'));
assert.ok(workflow.includes('test_hls_tail_cleanup_v598.js'));
const match=version.match(/kProgramVersion = "10[.]8[.](\d+)"/);
assert.ok(match && Number(match[1])>=598,'Version must be V10.8.598+');
console.log('PASS V598 HLS lifecycle stop, restart exclusion, startup stale sweep, archive/failover guards');
