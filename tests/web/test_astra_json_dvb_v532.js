'use strict';
// V532: production import code-path regression. Native C++ test
// tests/test_external_config_transfer.cpp covers actual JSON conversion.
// This guard also verifies that browser upload does not corrupt CP1251
// before the payload reaches C++ and that imported tvg-id survives Edit.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const read=p=>fs.readFileSync(path.join(__dirname,'../..',p),'utf8');
const importer=read('src/http/ExternalConfigTransfer.h');
const tuner=read('src/http/AstraJsonDvbTune.h');
const config=read('src/ConfigManager.cpp');
const header=read('src/ConfigManager.h');
const playlist=read('src/http/HttpServerEpg.inc');
const ui=read('src/http/HttpServerWebUi.inc');
const test=read('tests/test_external_config_transfer.cpp');
const version=read('src/AppVersion.h');

for (const fragment of [
  'astra_json::tunerIndex(root,result.warnings)',
  'astra_json::convertDvb(source,tunes,sourceSid,result.warnings)',
  'stream.inputServiceId=firstSid',
  'stream.tvgId=tvgId',
  'applyAstraHttpOutput',
  'cfg.outputType="http"',
  'cfg.outputPort=0',
  'cfg.autoStart = false',
  'if (sources.empty()) {++result.ignored;continue;}',
  'T2MI input skipped: native T2-MI PLP/TS depacketizer is unavailable',
  'astra_json::jsonUtf8(data,document,result.warnings)'
]) assert.ok(importer.includes(fragment),'Missing importer property: '+fragment);

for(const fragment of [
  'iconv_open("UTF-8","WINDOWS-1251")',
  'Astra JSON invalid satellite symbol rate skipped',
  'Astra JSON invalid DVB frequency skipped',
  'Astra JSON DVB tuner reference missing',
  '"bandwidth_hz="<<bandwidth',
  '"&delivery_system=dvb-"<<type',
  '"&stream_id="<<plp',
  'Astra JSON nonempty Unicable setting needs manual mapping'
]) assert.ok(tuner.includes(fragment),'Missing bounded tuner decoder: '+fragment);
assert.ok(header.includes('std::string tvgId;'));
assert.ok(config.includes('root["tvg_id"] = tvgId;'));
assert.ok(config.includes('config.tvgId = root.get("tvg_id", "").asString().substr(0, 128);'));
assert.ok(playlist.includes('return !cfg.tvgId.empty() ? cfg.tvgId : cfg.id;'));
assert.ok(ui.includes("readForeignConfigText(mainFile, sourceKind)"));
assert.ok(ui.includes("new TextDecoder('utf-8', {fatal:true})"));
assert.ok(ui.includes("new TextDecoder('windows-1251', {fatal:true})"));
assert.ok(ui.includes('id="streamTvgId"'));
assert.ok(ui.includes("const channelId = attr(stream.tvg_id || stream.id || '')"));
assert.ok(ui.includes("tvg_id: String(document.getElementById('streamTvgId')"));
assert.ok(test.includes('frequency_khz=11785020'));
assert.ok(test.includes('bandwidth_hz=7000000'));
assert.ok(test.includes('hasT2miWarning&&hasCaWarning&&hasHttpWarning'));
assert.ok(test.includes('assert(sat.tvgId=="a005")'));
assert.ok(test.includes('cp1251Out.streams[0].tvgId=="a12"'));
assert.ok(version.includes('kProgramVersion = "10.8.532"'));
console.log('PASS: V532 Astra JSON DVB-S/S2/T2, HTTP MPEG-TS, CP1251, tvg-id and bounded T2MI warning');
