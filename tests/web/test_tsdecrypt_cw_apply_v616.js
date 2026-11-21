// V616 source invariant: track only numeric even/odd CW-set events.
// No raw ECM, CW, fingerprints, credentials, or child stderr in user logs.
const assert = require('node:assert/strict');
const fs = require('node:fs');

const build = fs.readFileSync('scripts/build_tsdecrypt_icam.sh', 'utf8');
const backend = fs.readFileSync('src/ca/backends/tsdecrypt/TsdecryptPlugin.cpp', 'utf8');
const parser = fs.readFileSync('src/ca/backends/tsdecrypt/TsDecryptTelemetry.h', 'utf8');

for (const marker of [
  "change('camd.c', [(",
  'DVB5 | CW_APPLIED mode=%u even=%u odd=%u',
  'memcmp(c->key->cw, invalid_cw, 8)',
  'memcmp(c->key->cw + 8, invalid_cw, 8)'
]) assert.ok(build.includes(marker), 'missing pinned TSDecrypt CW marker: ' + marker);
for (const marker of [
  'parseCwAppliedReport', 'TSDECRYPT CW APPLY stream=%s',
  'cw_applied=%llu cw_even_applied=%llu',
  'cw_odd_applied=%llu cw_nonzero_mode_applied=%llu',
  'cw_last_applied_age_ms=%llu',
  'DVBSTREAMER5_TS_AV_TRACE_STREAM_ID'
]) assert.ok(backend.includes(marker), 'missing bounded per-stream CW diagnostic: ' + marker);
assert.ok(parser.includes('if (std::sscanf('), 'strict numeric parser');
assert.ok(parser.includes('mode > 255U') && parser.includes('even > 1U') && parser.includes('odd > 1U'));
assert.ok(!build.includes('ts_hex_dump_buf(cw_dump'), 'never add raw CW dump to patch');
console.log('V616 nonsecret even/odd CW application diagnostic: OK');
