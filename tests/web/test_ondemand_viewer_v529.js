// V529: source-level regression for live OnDemand viewer accounting.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const read = file => fs.readFileSync(path.join(__dirname, '../..', file), 'utf8');

const manager = read('src/StreamManager.cpp');
const dvbMedia = read('src/http/HttpServerDvbMedia.inc');
const policy = read('src/media/OnDemandHlsStartupPolicy.h');
const unit = read('tests/media_core/test_ondemand_hls_startup_policy.cpp');

const start = manager.indexOf('void StreamManager::monitorOnDemandStreams()');
const end = manager.indexOf('std::vector<std::string> StreamManager::activeStreams()', start);
assert.ok(start >= 0 && end > start, 'OnDemand monitor missing');
const monitor = manager.slice(start, end);

assert.ok(monitor.includes('for (const auto& [fd, session] : httpClients)'), 'socket accounting missing');
assert.ok(monitor.includes('session.streamId == id'), 'socket stream isolation missing');
assert.ok(!monitor.includes('session.previewSession.empty()'), 'active preview must not be ignored');
assert.ok(monitor.includes('session.protocol == "hls"'), 'must scope media grace to HLS');
assert.ok(monitor.includes('session.lastActivity, now'), 'must use last media activity');
assert.ok(monitor.includes('hasHttpClient || hasRecentHlsMedia'), 'viewer presence must block idle stop');
assert.ok(monitor.includes('if (isStreamActive(id))'), 'idle shutdown guard missing');
assert.ok(monitor.includes('stopStream(id)'), 'OnDemand must still stop when idle');
assert.ok(dvbMedia.includes('streamManager.addStreamSession(id, clientIp, "hls")'),
  'successful HLS media must refresh logical session');
assert.ok(dvbMedia.includes('const bool mediaDelivered = res.result() == http::status::ok'),
  'HLS session cannot be created from a failed response');
assert.ok(dvbMedia.includes('segmentExt == ".ts"') && dvbMedia.includes('segmentExt == ".m4s"'),
  'must count TS and CMAF HLS media');
assert.ok(policy.includes('kHlsRecentMediaGrace = std::chrono::seconds(75)'),
  'missing bounded media grace');
assert.ok(policy.includes('now - lastMediaResponse < kHlsRecentMediaGrace'),
  'must expire viewer inactivity');
assert.ok(unit.includes('keepHlsOnDemandFromRecentMedia(media, media + seconds(74))'));
assert.ok(unit.includes('!keepHlsOnDemandFromRecentMedia(media, media + seconds(75))'));
console.log('PASS: OnDemand connected previews, bounded HLS viewer retention, idle stop');
