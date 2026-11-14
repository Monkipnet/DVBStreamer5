// V530: ensure only short-segment, transcoded OnDemand HLS uses the
// initial three-segment live prefill. Source-level guard complements the
// native segmenter CTest which verifies published media and playlist contents.
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const read = p => fs.readFileSync(path.join(__dirname,'../..',p),'utf8');

const config = read('src/media/NativeHlsSegmenter.h');
const segmenter = read('src/media/NativeHlsSegmenter.cpp');
const manager = read('src/StreamManager.cpp');
const server = read('src/http/HttpServerDvbMedia.inc');
const nativeTest = read('tests/media_core/test_native_hls_source_switch.cpp');
const version = read('src/AppVersion.h');

assert.ok(config.includes('std::size_t startupPrefillSegments = 0;'),
    'Other HLS modes must default to no prefill');
assert.ok(manager.includes('toLower(streamConfig.activationMode) == "ondemand"'),
    'The prefill needs an OnDemand-only guard');
assert.ok(manager.includes('hlsConfig.independentSegments &&'),
    'Passthrough must retain the existing startup path');
assert.ok(manager.includes('hlsConfig.targetDurationSeconds <= 4.0'),
    'Long custom segments must avoid unbounded cold-start waits');
assert.ok(manager.includes('hlsConfig.startupPrefillSegments = 3U'),
    'OnDemand short transcode must prefill three segments');
assert.ok(segmenter.includes('const std::size_t minimumReadySegments = std::max'),
    'Missing completed-segment startup gate');
assert.ok(segmenter.includes('config_.startupPrefillSegments'));
assert.ok(segmenter.includes('if (completedSegments_ < minimumReadySegments) return true;'));
assert.ok(segmenter.includes('NATIVE HLS STARTUP PREFILL READY'));
assert.ok(server.includes('prefilledOnDemandTranscode ? 200 : (cfg->transcodeEnabled ? 120 : 30)'),
    'Cold-start HTTP wait must cover the bounded prefill');
assert.ok(nativeTest.includes('prefillConfig.startupPrefillSegments=3U'));
assert.ok(nativeTest.includes('if(completed<3U && hasManifest)return 14;'));
assert.ok(nativeTest.includes('if(count<3U||playlist.find("#EXT-X-ENDLIST")'));
assert.ok(version.includes('kProgramVersion = "10.8.530"'));
console.log('PASS: V530 scoped OnDemand HLS three-segment startup prefill');
