// V531: panel program restart must escape the original systemd cgroup;
// OnDemand must stop even when a DVB tile was manually started or a
// manifest-only HLS probe continues after the real viewer disconnected.
// Source wiring regression complements systemd/VLC field verification.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const read = file => fs.readFileSync(path.join(__dirname, '../..', file), 'utf8');

const api = read('src/http/HttpServerTransport.inc');
const actions = read('src/http/HttpServerConfigActions.inc');
const header = read('src/HttpServer.h');
const parallel = read('src/StableUdpOutput.cpp');
const manager = read('src/StreamManager.cpp');
const sharedDvb = read('src/media/SharedDvbInputPool.cpp');
const version = read('src/AppVersion.h');

// A request that cannot schedule restart must never return success.
assert.ok(header.includes('bool handleRestartProgram(std::string* error);'));
assert.ok(api.includes('target == "/api/restart-program"'));
assert.ok(api.includes('if (!handleRestartProgram(&restartError))'));
assert.ok(api.includes('res.result(http::status::service_unavailable)'));
assert.ok(api.includes('res.body() = R"({"result":"restarting","scheduled":true,"delay_s":2})"'));
assert.ok(api.includes('apiKeyAttempt ||'), 'stream API keys cannot restart the daemon');
assert.ok(api.includes('isPanelSessionAuthorized(req) || isAuthorized(req)'));

// The program itself must not create a child that dies in its service cgroup.
// systemd-run --on-active creates a separate timer/oneshot owned by PID 1.
const restart = actions.slice(actions.indexOf('bool HttpServer::handleRestartProgram('),
                              actions.indexOf('void HttpServer::handleDeleteStream('));
assert.ok(restart.includes('/usr/bin/systemd-run --quiet --no-block --on-active=2s'));
assert.ok(restart.includes('/usr/bin/systemctl --no-block restart dvbstreamer5.service'));
assert.ok(restart.includes('if (rc != 0)'));
assert.ok(!restart.includes('std::thread('), 'old cgroup-bound restart thread must be gone');

// HLS manifest/playlist probes do not prove a subscriber remains connected.
// Successful media GETs provide the separate bounded last-activity signal.
const ensure = parallel.slice(parallel.indexOf('bool StreamManager::ensureOnDemandStreamParallel('),
                              parallel.indexOf('void StreamManager::prewarmOnDemandDvbInputs()'));
assert.ok(ensure.includes('const bool hlsProbe = source == "hls" || source == "hls-startup-wait"'));
assert.ok(ensure.includes('if (!activeAlready || !hlsProbe)'));
assert.ok(ensure.includes('onDemandLastActivity[id] = std::chrono::steady_clock::now()'));
assert.ok(ensure.includes('if (activeAlready) return true;'));

// The idle monitor must discover all running configured OnDemand tiles, even
// panel-started DVB services absent from the activation bookkeeping set.
const start = manager.indexOf('void StreamManager::monitorOnDemandStreams()');
const end = manager.indexOf('std::vector<std::string> StreamManager::activeStreams()',start);
assert.ok(start >= 0 && end > start);
const monitor = manager.slice(start,end);
assert.ok(monitor.includes('toLower(state->config.activationMode) != "ondemand"'));
assert.ok(monitor.includes('onDemandStartedStreams.insert(id)'));
assert.ok(monitor.includes('onDemandLastActivity.try_emplace(id,'));
assert.ok(monitor.includes('state->startedAt'), 'old active channels should not inherit a fresh lifetime');
assert.ok(monitor.includes('hasRecentHlsMedia') && monitor.includes('session.protocol == "hls"'));
assert.ok(monitor.includes('hasHttpClient || hasRecentHlsMedia'));
assert.ok(monitor.includes('stopStream(id)'));

// Idle teardown unsubscribes one stream. Neighboring services must remain
// attached to the shared transponder; cold DVB retune protection is intact.
assert.ok(manager.includes('sharedDvbInputs.unsubscribe(id)'));
assert.ok(sharedDvb.includes('bus->subscribers.erase(item);'));
assert.ok(sharedDvb.includes('if (bus->subscribers.empty())'));
assert.ok(sharedDvb.includes('keepWarmOnIdle'));
assert.ok(version.includes('kProgramVersion = "10.8.531"'));
console.log('PASS: V531 independent program restart and DVB/HLS OnDemand idle teardown');
