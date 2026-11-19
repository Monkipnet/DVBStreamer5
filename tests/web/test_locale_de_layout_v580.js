const fs = require('fs');
const assert = require('assert');
const page = fs.readFileSync('src/http/HttpServerWebUi.inc', 'utf8');
assert.match(page, /<select id="languageSelect"[^>]*onchange="selectInterfaceLanguage\(this\.value\)"/);
for (const locale of ['ru','en','hr','de']) assert.match(page, new RegExp('<option value="'+locale+'">'));
assert.match(page, /language === 'de' \? 'Sprache'/);
assert.match(page, /localStorage\.setItem\('dvbstreamer5-language', language\)/);
assert.match(page, /translations\.de = Object\.assign/);
assert.match(page, /function translateEnglishToGerman/);
assert.match(page, /data-ui-de=/);
assert.match(page, /data-placeholder-de=/);
assert.match(page, /\.modal-content :is\(input,select,textarea\)/);
assert.match(page, /\.stream-modal \.input-main-row\{grid-template-columns:repeat\(auto-fit/);
assert.doesNotMatch(page, /id="languageButton"/);
const start = page.indexOf('<script>') + '<script>'.length;
const end = page.indexOf('</script>', start);
assert.ok(start > 0 && end > start);
new Function(page.slice(start, end));
for(const path of ['oscam-work.c','oscam-reader.c','module-newcamd.c','oscam-ecm.c']){
  const source = fs.readFileSync('third_party/oscam-mini/'+path,'utf8');
  assert.doesNotMatch(source,/DVB5 ECM LATENCY/, path+' still contains verbose diagnostic logging');
}
const versionMatch = fs.readFileSync('src/AppVersion.h','utf8').match(/kProgramVersion\s*=\s*"10\.8\.(\d+)"/);
assert.ok(versionMatch && Number(versionMatch[1]) >= 580, 'V580 baseline regression');
console.log('PASS four-language selector, responsive UI, JavaScript syntax, ECM trace cleanup and unchanged version');
