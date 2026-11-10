'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const {spawnSync}=require('node:child_process');
const web=fs.readFileSync('src/http/HttpServerWebUi.inc','utf8');
const transport=fs.readFileSync('src/http/HttpServerTransport.inc','utf8');
const packageScript=fs.readFileSync('scripts/make_portable_linux_bundle.sh','utf8');
const updater=fs.readFileSync('scripts/dvbstreamer5_update.sh','utf8');
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
  '/opt/DVBStreamer5/updater.sh'
]) assert.ok(transport.includes(text),'Missing secure update route: '+text);
for(const text of [
  'scripts/dvbstreamer5_update.sh',
  '"$STAGE/updater.sh"',
  'run.sh updater.sh PORTABILITY.txt'
]) assert.ok(packageScript.includes(text),'Missing bundled updater: '+text);
for(const marker of [
  'download.php?id=5c15e7ab4ff1c26daef79d745d211c3d',
  'latest.json',
  'site_header_version',
  'https://tvstreammersat5.tech/',
  'RELEASE_VERSION="$site_version"',
  'sha256sum',
  'flock -n',
  'tar -tzf',
  'tar -tvzf',
  'determine_archive_release',
  'No unique embedded version',
  'ARCHIVE_ROOT',
  'if [[ ! -f "$archive" ]]; then download_archive "$archive"; fi',
  'update-backups',
  'cp -a "$backup" /opt/DVBStreamer5'
]) assert.ok(updater.includes(marker),'Missing Bash updater protection: '+marker);
assert.ok(!packageScript.includes('updater.py'),'Python updater must not be bundled');
assert.ok(packageScript.includes('"$STAGE/VERSION"'),'Bundle must record explicit version');
const versionHeader=fs.readFileSync('src/AppVersion.h','utf8');
const sedLine=packageScript.split('\n').find(line=>line.startsWith('VERSION="$(sed -n '));
assert.ok(sedLine,'Portable bundle version parser missing');
const sedExpression=sedLine.match(/sed -n '([^']+)'/);
assert.ok(sedExpression,'Missing portable version sed expression');
const parsed=spawnSync('sed',['-n',sedExpression[1]],{
  input:versionHeader,encoding:'utf8'
});
assert.equal(parsed.status,0,'Portable VERSION extraction command failed');
assert.match(parsed.stdout.trim(),/^\d+\.\d+\.\d+$/,
  'Portable VERSION extraction must return a semantic version');

const syntax=spawnSync('bash',['-n'],{input:updater,encoding:'utf8'});
assert.equal(syntax.status,0,'Bash updater syntax: '+syntax.stderr);
console.log('PASS: Bash update UI, API, portable packaging and syntax');
