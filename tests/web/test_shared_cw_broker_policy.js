'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const base = path.join(__dirname, '../..');
const plugin = fs.readFileSync(path.join(base,
    'src/ca/backends/newcamd/NewcamdPlugin.cpp'), 'utf8');
const broker = fs.readFileSync(path.join(base,
    'src/ca/backends/newcamd/SharedCwBroker.h'), 'utf8');

assert.match(plugin, /DVBSTREAMER5_SHARED_CW_BROKER/);
assert.match(plugin, /std::strcmp\(p, "1"\) == 0/);
assert.match(plugin, /postReplay\(session, table, replayPair\)/);
assert.match(plugin, /replayWorker = std::thread/);
assert.match(plugin, /cwBroker\.lookup\(/);
assert.match(plugin, /cwBroker\.publish\(/);
assert.match(plugin, /cwBroker\.invalidateReader\(/);
assert.match(plugin, /session->client->set_key_update_callback/);
assert.match(plugin, /SHA256\(/);
assert.match(broker, /kTtlMs = 1500/);
assert.match(broker, /kMaxEntries = 128/);
assert.match(broker, /key\.serviceId/);
assert.match(broker, /key\.caid/);
assert.match(broker, /key\.ecmPid/);
assert.match(broker, /value\.usable\(\)/);
// External TSDecrypt is intentionally *not* connected to the in-process
// broker in this release. Do not accidentally claim or fake IPC support.
const ts = fs.readFileSync(path.join(base,
    'src/ca/backends/tsdecrypt/TsdecryptPlugin.cpp'), 'utf8');
assert.doesNotMatch(ts, /SharedCwBroker|shared_cw_cache_hits/);
console.log('PASS shared CW broker opt-in integration guards');
