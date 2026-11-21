// V613 regression: NVDEC diagnostics must identify the originating tile.
// No hardware decode in CI; this is a source-level wiring/policy invariant.
const assert = require('node:assert/strict');
const fs = require('node:fs');

const h = fs.readFileSync('src/media/NativeNvidiaZeroCopy.h', 'utf8');
const p = fs.readFileSync('src/media/NativeTranscoderPipeline.cpp', 'utf8');
const n = fs.readFileSync('src/media/NativeNvidiaZeroCopy.cpp', 'utf8');

assert.match(h, /std::string streamId;[^\n]*Per-instance/);
assert.match(p, /zeroCopyConfig\.streamId = config_\.streamId;/);
assert.match(n, /NATIVE NVDEC DECODE ERROR status=/);
assert.match(n, /stream_id=" << config_\.streamId/);
assert.match(n, /type=" << \(decodeStatus\.decodeStatus/);
assert.match(n, /error_concealed/);
assert.match(n, /action=forward_to_encoder/);
assert.match(n, /cuvidDecodeStatus_Error_Concealed/);
assert.match(n, /nvdecCorruptFrames_ % 25ULL/);
console.log('V613 NVDEC per-stream concealment diagnostic wiring: OK');
