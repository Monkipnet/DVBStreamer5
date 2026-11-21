// V614 source wiring regression: audio/video (including AC-3 private_stream_1)
// is observable at both sides of CA and within the TSDecrypt async FIFO.
// Deliberately no test credentials, ECMs, control words or decrypted payload.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const relay = fs.readFileSync('src/media/NativeUdpRelay.cpp', 'utf8');
const ca = fs.readFileSync('src/ca/backends/tsdecrypt/TsdecryptPlugin.cpp', 'utf8');
const trace = fs.readFileSync('src/media/TsAvStageTrace.h', 'utf8');
for (const stage of ['pre_ca', 'post_ca', 'pre_output']) {
  assert.match(relay, new RegExp('av(?:BeforeCa|AfterCa|PreOutput)\\(\\s*config_\\.streamId, "' + stage + '"'));
}
for (const name of [
  'DVBSTREAMER5_TS_AV_TRACE_STREAM_ID', 'DVBSTREAMER5_TS_AV_VIDEO_PID',
  'DVBSTREAMER5_TS_AV_AUDIO_PID', 'avBeforeCa.inspect',
  'avAfterCa.inspect', 'avPreOutput.inspect'
]) assert.ok(relay.includes(name), name);
for (const name of [
  'TSDECRYPT FIFO DIAG', 'child_last_output_age_ms=',
  'child_scrambled_suppressed=', 'private1_pes=', 'input_queued_bytes=',
  'output_queued_packets=', 'peakInputBytes', 'peakOutputPackets',
  'childScrambledSuppressed.fetch_add', 'outputPrivate1Pes.fetch_add'
]) assert.ok(ca.includes(name), name);
assert.match(trace, /stream == 0xbdU/);
assert.match(trace, /pts_forward_over200=/);
assert.match(trace, /bad_pes_delta=/);
console.log('V614 TSDecrypt FIFO / A-V stage diagnostic wiring: OK');
