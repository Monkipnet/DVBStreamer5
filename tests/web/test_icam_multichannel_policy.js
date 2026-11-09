'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const file = fs.readFileSync(path.resolve(__dirname,
  '../../src/ca/backends/newcamd/NewcamdPlugin.cpp'), 'utf8');
// Keep iCAM key selection and session architecture unchanged.
assert.match(file, /constexpr size_t kMaxSessionsPerReader = 0/);
assert.match(file, /mode=independent-tcp session=/);
assert.match(file, /dvbcsa_bs_key_set_ecm\(ecmMode, cw, slot\.bsKey\)/);
// Do not hold a per-service CA mutex across routine stderr/journald logs.
assert.match(file, /if \(fastEcm && caDiagnosticsEnabled\(\)\) \{\s*std::cerr << "NEWCAMD FASTECM CW:/);
assert.match(file, /if \(diagnostics\) \{\s*std::cerr << "NEWCAMD FASTECM ACTIVATE:/);
for (const key of ['fast_ecm_ignored_halves', 'fast_ecm_stages',
                   'fast_ecm_activations']) {
  assert.ok(file.includes(key), 'Status metric missing: ' + key);
}
console.log('iCAM multichannel normal-mode logging and CW compatibility: OK');
