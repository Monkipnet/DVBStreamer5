'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const {spawnSync}=require('node:child_process');
const web=fs.readFileSync('src/http/HttpServerWebUi.inc','utf8');
const transport=fs.readFileSync('src/http/HttpServerTransport.inc','utf8');
const packageScript=fs.readFileSync('scripts/make_portable_linux_bundle.sh','utf8');
const updater=fs.readFileSync('scripts/dvbstreamer5_update.py','utf8');
for(const text of [
  'id="checkReleaseButton"',
  'id="installReleaseButton" hidden',
  'async function checkBinaryUpdates()',
  'async function installBinaryUpdate()',
  'latestVerifiedUpdate=data.latest',
  "'/api/update/check'",
  "'/api/update/install'"
]) assert.ok(web.includes(text),'Missing update UI: '+text);
for(const text of [
  'target == "/api/update/check"',
  'target == "/api/update/install"',
  '!isPanelSessionAuthorized(req)',
  'apiKeyAttempt',
  '"X-Requested-With"',
  '/opt/DVBStreamer5/updater.py'
]) assert.ok(transport.includes(text),'Missing secure update route: '+text);
for(const text of [
  'scripts/dvbstreamer5_update.py',
  '"$STAGE/updater.py"',
  'run.sh updater.py PORTABILITY.txt'
]) assert.ok(packageScript.includes(text),'Missing bundled updater: '+text);
for(const text of [
  'release signature',
  "'-verify'",
  "'sha256'",
  'Archive SHA256/size mismatch',
  "if ver(version)<=ver(current)",
  "update-backups",
  "tar.extractall",
  "shutil.copytree(backup,HOME"
]) assert.ok(updater.toLowerCase().includes(text.toLowerCase()),'Missing updater protection: '+text);
const py=spawnSync('python3',['-c','import ast,sys;ast.parse(sys.stdin.read())'],{
  input:updater,encoding:'utf8'
});
assert.equal(py.status,0,'Updater Python syntax: '+py.stderr);
console.log('PASS: signed binary updater UI, policy, packaging and Python syntax');
