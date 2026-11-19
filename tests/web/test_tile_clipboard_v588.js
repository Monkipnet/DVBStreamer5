'use strict';
// V588: non-invasive single-tile transfer UI and backend routing contracts.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const read = path => fs.readFileSync(path, 'utf8');
const ui = read('src/http/HttpServerWebUi.inc');
const transport = read('src/http/HttpServerTransport.inc');
const backend = read('src/http/HttpServerConfigActions.inc');
const helper = read('src/http/TileClipboardTransfer.h');
const version = read('src/AppVersion.h');
const start = ui.indexOf('<script>\n');
assert.ok(start > 0,'Expected inline dashboard JS script');
const end = ui.indexOf('</script>',start);
assert.ok(end > start);
new Function(ui.slice(start+8,end));
assert.match(ui,/document\.addEventListener\('contextmenu',event=>/);
assert.match(ui,/\.tile\[data-stream-id\],\.tile\[data-wisi-output-id\]/);
assert.match(ui,/async function copyTileClipboard\(kind,id\)/);
assert.match(ui,/async function pasteTileClipboardFromMenu\(\)/);
assert.match(ui,/async function pasteTileClipboardDirect\(raw\)/);
assert.match(ui,/document.addEventListener\('paste',event=>/);
assert.match(ui,/tileClipboardFallback=contents/);
assert.match(ui,/navigator\.clipboard\?\.writeText/);
assert.match(ui,/navigator\.clipboard\?\.readText/);
assert.match(ui,/fallbackCopyText\(contents\)/);
assert.match(ui,/tileClipboardRecognized\(raw\)/);
assert.match(ui,/action:'apply',item/);
assert.match(transport,/target\.rfind\("\/api\/tile-clipboard\/export\?kind=", 0\)/);
assert.match(transport,/target == "\/api\/tile-clipboard\/paste"/);
assert.match(transport,/handleTileClipboardPaste\(req\.body\(\)\)/);
assert.match(transport,/apiKeyAttempt \|\|/);
assert.match(backend,/stream\.autoStart = false/);
assert.match(backend,/output\.enabled = false/);
assert.match(backend,/output\.autoStart = false/);
assert.match(backend,/configManager\.save\(\)/);
assert.match(backend,/validateHttpPortsForConfig\(candidate, portError\)/);
assert.match(backend,/refreshHttpPorts\(\)/);
assert.ok(!backend.slice(backend.indexOf('handleTileClipboardPaste('),
  backend.indexOf('std::string HttpServer::handleConfigExport(')).includes('handleSaveConfig('),
  'Paste should not use full-config mutation');
assert.match(helper,/kSchema = "DVBStreamer5\/tile"/);
assert.match(helper,/tile\["hls_access_key_value"\] = ""/);
assert.match(helper,/tile\["srt_input_passphrase"\] = ""/);
assert.match(helper,/tile\["conditional_access_client"\] = ""/);
assert.match(helper,/tile\["auto_start"\] = false/);
assert.match(helper,/tile\["hls_encryption"\] = "none"/);
assert.match(helper,/uri\.find_first_of\("\?#"/);
for(const label of ['Копировать плитку','Copy tile','Kopiraj pločicu','Kachel kopieren',
  'Вставить плитку','Paste tile','Zalijepi pločicu','Kachel einfügen']){
    assert.ok(ui.includes(label),'Missing RU/EN/HR/DE label: '+label);
}
assert.match(version,/kProgramVersion = "10\.8\.58[89]"/);
console.log('PASS V588/V589 tile clipboard: right-click, 4 languages, one-action paste, session gates');
