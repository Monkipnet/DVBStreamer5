'use strict';
const fs=require('node:fs');
const assert=require('node:assert/strict');
const ui=fs.readFileSync('src/http/HttpServerWebUi.inc','utf8');
const api=fs.readFileSync('src/http/HttpServerConfigActions.inc','utf8');
const transport=fs.readFileSync('src/http/HttpServerTransport.inc','utf8');
const manager=fs.readFileSync('src/StreamManager.cpp','utf8');
const header=fs.readFileSync('src/StreamManager.h','utf8');
const conversion=fs.readFileSync('src/http/ExternalConfigTransfer.h','utf8');
for(const text of [
 'id="configImportFormat"', 'id="configExportFormat"',
 'value="astra"', 'value="flussonic"', 'configTransferFormatChanged()',
 'new_streams', 'data.warnings',
 'sourceKind = sourceKind || document.getElementById',
 "kind === 'flussonic' ? 'text/plain",
 "toggleStream(id, true, null, true)",'stream.stop_force_requested'
]) assert.ok(ui.includes(text),'Missing UI import/export or force-stop control: '+text);
for(const text of [
 'external_config::parseAstra', 'external_config::parseFlussonic',
 'external_config::exportAstra', 'external_config::exportFlussonic',
 'external_config::mergeNewOnly', 'candidate = configManager.config',
 'auto_start=false', 'action == "preview"'
]) assert.ok(api.includes(text) || conversion.includes(text),
             'Missing backend converter safety gate: '+text);
assert.ok(transport.includes('handleConfigExport(req.body())'),
          'Export format must be forwarded from control API');
for(const text of [
 'stopStreamAsync(const std::string& id, bool force)',
 'forceCancelStreamIo', 'requestStopInputs', 'forceStopRequested_',
 'asyncStopThreads_', 'operatorStoppedStreams_.insert(id)',
 'NATIVE FORCE STOP SIGNAL'
]) assert.ok((manager+'\n'+header).includes(text),'Missing safe forced-stop support: '+text);
assert.ok(!manager.includes('pthread_cancel'),
  'Never cancel threads abruptly in-process');
assert.ok(!manager.includes('pthread_kill'),
  'Never kill a native thread in-process');
console.log('PASS Cesbo/Flussonic format dropdowns and per-tile force-stop policy');
