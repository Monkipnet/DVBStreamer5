// V528: regression guards for DVB Unicable/JESS OnDemand identity
// and HEAD-first HLS players. Read-only source checks; no hardware needed.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const read = name => fs.readFileSync(path.join(__dirname, '../..', name), 'utf8');

const warm = read('src/StableUdpOutput.cpp');
const manager = read('src/StreamManager.cpp');
const pool = read('src/media/SharedDvbInputPool.cpp');
const http = read('src/http/HttpServerTransport.inc');
const routes = read('src/http/HttpServerDvbMedia.inc');

const warmStart = warm.indexOf('bool makeWarmDvbTune(');
const warmEnd = warm.indexOf('std::string warmFrontendKey(');
const liveStart = manager.indexOf('relay.dvbTuneConfig.adapter =');
const liveEnd = manager.indexOf('relay.dvbTuneConfig.pids =');
assert.ok(warmStart >= 0 && warmEnd > warmStart);
assert.ok(liveStart >= 0 && liveEnd > liveStart);
const warmFields = warm.slice(warmStart, warmEnd);
const liveFields = manager.slice(liveStart, liveEnd);

// Every transponder identity component must match the live subscription,
// or a busy frontend will be mistaken for another transponder.
const identityFields = [
  'adapter', 'frontend', 'frequencyKHz', 'symbolRateK', 'bandwidthHz',
  'polarity', 'deliverySystem', 'modulation', 'fec', 'diseqcSource',
  'lnbLof1KHz', 'lnbLof2KHz', 'lnbSlofKHz',
  'unicableMode', 'unicableUserBand', 'unicableFrequencyKHz',
  'unicablePosition', 'unicablePin', 'streamId'
];
for (const field of identityFields) {
  assert.ok(
    liveFields.includes('relay.dvbTuneConfig.' + field + ' = dvbParams.' + field + ';'),
    'Live DVB tune mapping missing: ' + field
  );
  assert.ok(
    warmFields.includes('tune.' + field + ' = params.' + field + ';'),
    'OnDemand warm tune mapping missing: ' + field
  );
}
assert.ok(pool.includes('has active service consumers'));
assert.ok(pool.includes('if (busy)'));

// HEAD should be a side-effect-free HLS manifest capability probe,
// even when OnDemand is idle and the manifest is not yet on disk.
assert.ok(http.includes('const bool idleOnDemandManifest ='));
assert.ok(http.includes('hlsCfg->activationMode == "ondemand"'));
assert.ok(http.includes('filePath.extension() == ".m3u8"'));
assert.ok(http.includes('!dynamicArchive && !idleOnDemandManifest'));
assert.ok(routes.includes('ensureOnDemandStreamParallel(id, "hls", &demandError)'));
assert.ok(routes.includes('ensureOnDemandStreamParallel(id, "http", &demandError)'));
// Keep explicit operator Stop authoritative; no silent resurrection.
assert.ok(manager.includes('operatorStoppedStreams_.count(streamConfig.id)'));
assert.ok(pool.includes('has active service consumers'));

console.log('PASS: OnDemand DVB tune parity, HEAD-first HLS and operator Stop safety');
