'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const ui = fs.readFileSync(path.resolve(__dirname, '../../src/http/HttpServerWebUi.inc'), 'utf8');
const start = ui.indexOf('function outputHostIsAuto(');
const end = ui.indexOf('function outputBadgeText(', start);
assert.ok(start > 0 && end > start, 'V513 stream URL helpers must be present');
const browser = {location: {hostname: '203.0.113.7'}};
const context = {window: browser, URL, normalizedOutputType: () => 'http'};
const {streamLinks, advertisedOutputLink} = vm.runInNewContext(
  ui.slice(start, end) + '\n({streamLinks, advertisedOutputLink})', context);

const one = (type, host, url, mode='listener') =>
  streamLinks({vlc_links: [{output_type: type, output_mode: mode,
    output_host: host, url}]}).map(link => link.url)[0];

assert.equal(one('http', '0.0.0.0', 'http://10.0.0.5:9002/live/mpegts'),
  'http://203.0.113.7:9002/live/mpegts');
assert.equal(one('hls', '::', 'http://10.0.0.5:9000/live/video.m3u8'),
  'http://203.0.113.7:9000/live/video.m3u8');
assert.equal(one('srt', '0.0.0.0', 'srt://10.0.0.5:7001?mode=caller'),
  'srt://203.0.113.7:7001?mode=caller');
assert.equal(one('srt', '0.0.0.0', 'srt://10.0.0.5:7001?mode=listener', 'caller'),
  'srt://10.0.0.5:7001?mode=listener');
assert.equal(one('http', '192.168.200.3', 'http://192.168.200.3:9002/live/mpegts'),
  'http://192.168.200.3:9002/live/mpegts');
assert.equal(one('http', 'media.example.org', 'http://media.example.org:9002/live/mpegts'),
  'http://media.example.org:9002/live/mpegts');
assert.equal(one('http', '127.0.0.1', 'http://127.0.0.1:9002/live/mpegts'),
  'http://127.0.0.1:9002/live/mpegts');
assert.equal(one('udp-cbr', '239.9.0.7', 'udp://@239.9.0.7:1234'),
  'udp://@239.9.0.7:1234');
assert.equal(one('rtp', '224.0.10.2', 'rtp://224.0.10.2:1234'),
  'rtp://224.0.10.2:1234');
browser.location.hostname = 'admin.example.org';
assert.equal(one('http', '0.0.0.0', 'http://10.0.0.5:9002/live/mpegts'),
  'http://admin.example.org:9002/live/mpegts');
assert.equal(advertisedOutputLink({url: 'not a URL', output_type: 'hls', output_host: '0.0.0.0'}), 'not a URL');

// Switching the output interface must not rewrite manually configured hosts.
const ifStart = ui.indexOf('function outputInterfaceChanged(');
const ifEnd = ui.indexOf('function markOutputHostManual(', ifStart);
assert.ok(ifStart > 0 && ifEnd > ifStart);
assert.doesNotMatch(ui.slice(ifStart, ifEnd), /host\.value\s*=/);
assert.match(ui, /data-output-auto-host/);
const state = fs.readFileSync(path.resolve(__dirname, '../../src/http/HttpServerState.inc'), 'utf8');
assert.match(state, /std::string HttpServer::listInterfaces\(\)\s*\{[\s\S]*?enumerateNetworkInterfaces\(true\)/);
const relay = fs.readFileSync(path.resolve(__dirname, '../../src/media/NativeUdpRelay.cpp'), 'utf8');
assert.match(relay, /config_\.sourcePcrPacketPacing\s*\|\|\s*transcodeSourcePcrPacketPacing/);
assert.match(relay, /sourcePllEncodedTargetBitrate\(encodedCbrBitrate\)/);
console.log('V513 auto output host / explicit host / lo / PCR pacing gates: OK');
