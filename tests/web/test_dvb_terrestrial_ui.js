'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const root = path.resolve(__dirname, '../..');
const ui = fs.readFileSync(path.join(root, 'src/http/HttpServerWebUi.inc'), 'utf8');
const source = fs.readFileSync(path.join(root, 'src/DvbSatellite.cpp'), 'utf8');
const input = fs.readFileSync(path.join(root, 'src/media/LinuxDvbInput.cpp'), 'utf8');
const pool = fs.readFileSync(path.join(root, 'src/media/SharedDvbInputPool.cpp'), 'utf8');
const tuneKey = fs.readFileSync(path.join(root, 'src/media/SharedDvbTuneKey.h'), 'utf8');
const relay = fs.readFileSync(path.join(root, 'src/StreamManager.cpp'), 'utf8');
const warm = fs.readFileSync(path.join(root, 'src/StableUdpOutput.cpp'), 'utf8');

const begin = ui.indexOf('<script>');
const end = ui.indexOf('</script>', begin);
assert.ok(begin >= 0 && end > begin, 'Web UI JavaScript script tag missing');
assert.doesNotThrow(() => new vm.Script(ui.slice(begin + 8, end),
  { filename: 'DVBStreamer5-WebUi.js' }), 'Web UI JavaScript must parse');

for (const marker of [
  'id="satReceptionType"', 'id="satBandwidth"', 'id="satDeliverySystem"',
  'id="satStreamId"', 'id="streamDvbBandwidth"', 'id="streamDvbDelivery"',
  'function setDvbReceptionMode(', 'function updateDvbInputSystemUi(',
  'function dvbReceptionIsTerrestrial(', 'function dvbFrontendSupportsReception(',
  'bandwidth_hz:Number(', 'dvb://terrestrial', 'MUX '
]) assert.ok(ui.includes(marker), 'Missing DVB-T/T2 UI feature: ' + marker);

for (const marker of [
  'SYS_DVBT', 'SYS_DVBT2', 'DTV_ENUM_DELSYS', 'bandwidth_hz',
  'validTerrestrialBandwidth', 'dvb://terrestrial'
]) assert.ok(source.includes(marker), 'Missing DVB delivery support: ' + marker);

for (const marker of [
  'SYS_DVBT', 'SYS_DVBT2', 'DTV_BANDWIDTH_HZ',
  'DTV_FREQUENCY', 'DTV_STREAM_ID', 'tuneTerrestrialFrontend('
]) assert.ok(input.includes(marker), 'Missing native frontend tuning: ' + marker);

assert.ok(pool.includes('sharedDvbTransponderKey(tune)'),
  'Shared DVB subscriber and warm standby must use the canonical tune key');
assert.ok(tuneKey.includes('config.bandwidthHz'),
  'Shared DVB-T2 key must include bandwidth');
assert.ok(tuneKey.includes('config.frequencyKHz'),
  'Shared DVB-T2 key must include RF frequency');
assert.ok(tuneKey.includes('config.streamId'),
  'Shared DVB-T2 key must retain PLP identity');
assert.ok(tuneKey.includes('isTerrestrialDvbSystem(config.deliverySystem)'),
  'DVB-T/T2 key must normalize terrestrial-only tune parameters');
assert.ok(relay.includes('relay.dvbTuneConfig.bandwidthHz = dvbParams.bandwidthHz'),
  'Live DVB input must propagate bandwidth');
assert.ok(warm.includes('tune.bandwidthHz = params.bandwidthHz'),
  'OnDemand DVB input must propagate bandwidth');
console.log('DVB-T/T2 UI syntax, Linux DVBv5 tuning, shared + OnDemand checks: OK');
