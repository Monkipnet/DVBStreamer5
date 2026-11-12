'use strict';
// Operator Stop must remain authoritative; UI cannot forge stopped state.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const m = fs.readFileSync('src/StreamManager.cpp','utf8');
const h = fs.readFileSync('src/StreamManager.h','utf8');
const a = fs.readFileSync('src/http/HttpServerConfigActions.inc','utf8');
const s = fs.readFileSync('src/http/HttpServerState.inc','utf8');
const ui = fs.readFileSync('src/http/HttpServerWebUi.inc','utf8');
for (const x of [
  'operatorStoppedStreams_', 'isStopPending(', 'isOperatorStopped(',
  'clearOperatorStop(', 'mutable std::mutex asyncStopMutex_'
]) assert.ok(h.includes(x), 'Missing shared stop policy: '+x);
for (const x of [
  'operatorStoppedStreams_.insert(id)',
  'asyncStopPending_.count(streamConfig.id)',
  'operatorStoppedStreams_.count(streamConfig.id)',
  'if (isOperatorStopped(id))',
  'NATIVE ASYNC STOP NEW GENERATION'
]) assert.ok(m.includes(x), 'Missing stream stop gating: '+x);
for (const x of [
  'streamManager.isStopPending(cfg.id)',
  'streamManager.clearOperatorStop(cfg.id)',
  'stream is still stopping; wait for stopped state before Start'
]) assert.ok(a.includes(x), 'Start must not race Stop: '+x);
assert.ok(a.includes('const bool stopPending = streamManager.isStopPending(id)'),
  'Duplicate Stop must keep reporting pending');
assert.ok(s.includes('item["stop_pending"] = streamManager.isStopPending(cfg.id)'),
  'State must reflect asynchronous Stop');
for (const x of [
  'stream.stop_pending ? streamStoppingLabel()',
  'toggleButton.disabled = false;',
  'stream.stop_force_requested',
  "toggleStream(id, true, null, true)",
  'current?.stop_pending',
  'Zaustavljanje...',
  'if (data.already_stopped)',
  'current.stop_pending = true',
  'Stop queued; waiting for completion'
]) assert.ok(ui.includes(x), 'UI cannot fake completed Stop: '+x);
assert.ok(!ui.includes("if (active) markStreamStoppedInUi(id, data.already_stopped ? 'stopped' : 'stopping');"),
  'Stop ack must not reset uptime');
console.log('PASS tile operator Stop policy');
