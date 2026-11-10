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
  'run.sh updater.sh VERSION PORTABILITY.txt'
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
for(const marker of [
  'id="binaryUpdateProgress"',
  'async function watchBinaryUpdate(version)',
  "updateProgress('checking')",
  "updateProgress('downloading',version)",
  "updateProgress('error'",
  "fetch('/api/update/status'",
  '.update-action-busy::before',
  'id="binaryUpdateClose"',
  'onclick="closeBinaryUpdateProgress()"',
  'id="binaryUpdateClose" onclick="closeBinaryUpdateProgress()" aria-label="Закрыть уведомление"',
  "#binaryUpdateClose::before{content:'×';",
  "updateClose.textContent='';",
  '#binaryUpdateClose{position:absolute;top:8px;right:8px;width:18px;height:18px;',

  'function closeBinaryUpdateProgress()',
  'let updateProgressDismissed=false;',
  'if(!updateProgressDismissed)surface.hidden=false;',
  '#binaryUpdateProgress[hidden]{display:none}',
  'aria-live="polite"'
]) assert.ok(web.includes(marker),'Missing visible updater progress: '+marker);
for(const marker of [
  'target == "/api/update/status"',
  '/var/lib/dvbstreamer5/update-status',
  'phase != "downloading"'
]) assert.ok(transport.includes(marker),'Missing status API protection: '+marker);
for(const marker of [
  'update_phase downloading',
  'update_phase verifying',
  'update_phase installing',
  'update_phase done',
  'UPDATE_INSTALL_ACTIVE=1'
]) assert.ok(updater.includes(marker),'Missing real update lifecycle phase: '+marker);
for(const marker of [
  'run.sh updater.sh VERSION PORTABILITY.txt',
  '[[ -s "$INSTALL_DIR/updater.sh" && -s "$INSTALL_DIR/VERSION" ]]',
  'chmod 0755 "$INSTALL_DIR/updater.sh"'
]) assert.ok(packageScript.includes(marker),'Missing installer updater guard: '+marker);
for(const marker of [
  'Updater missing: install /opt/DVBStreamer5/updater.sh',
  'Updater check failed: ',
  'failure["exit_status"] = exitCode',
  'Json::writeString(writer, failure)',
  '" 2>&1"',
  'Json::parseFromStream(reader, jsonInput, &parsed, &parseErrors)'
]) assert.ok(transport.includes(marker),'Missing update diagnostics: '+marker);
assert.ok(!transport.includes('" 2>/dev/null"'), 'Updater stderr must not be discarded');
for(const marker of [
  'Unsafe non-HTTPS download redirect',
  'Unsafe credential-bearing redirect',
  'Refused local/private archive redirect',
  "Download HTTP $status"
]) assert.ok(updater.includes(marker),'Missing HTTPS redirect guard: '+marker);
assert.ok(!web.includes('data-ui-ru="Закрыть уведомление"'),'Close icon must not be replaced by localized button text');
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

// Exercise the successful check path with a fake HTTP header (no network).
// A scoped tmp cleanup trap must not reference an unset local at shell exit.
const os=require('node:os');
const path=require('node:path');
const tempRoot=fs.mkdtempSync(path.join(os.tmpdir(),'dvb5-updater-test-'));
try {
  const curl=path.join(tempRoot,'curl');
  fs.writeFileSync(curl, [
    '#!/usr/bin/env bash',
    'while (( $# )); do',
    '  if [[ "$1" == "-D" ]]; then',
    '    shift',
    "    printf 'HTTP/2 200\\r\\nContent-Disposition: attachment; filename=\"DVBStreamer5-10.8.411-linux-x86_64.tar.gz\"\\r\\n' > \"$1\"",
    '    exit 0',
    '  fi',
    '  shift',
    'done',
    'exit 22'
  ].join('\n')+'\n',{mode:0o755});
  const result=spawnSync('bash',['scripts/dvbstreamer5_update.sh','check','10.8.440'],{
    encoding:'utf8',timeout:15000,
    env:{...process.env,PATH:tempRoot+path.delimiter+process.env.PATH}
  });
  assert.equal(result.status,0,'check/EXIT must succeed: '+result.stderr);
  assert.equal(result.stderr.trim(),'','No error may appear after the JSON check');
  const report=JSON.parse(result.stdout.trim());
  assert.equal(report.latest,'10.8.411');
  assert.equal(report.available,false);
  // Regression: PHP download.php may reject HEAD but accept normal GET.
  const fixtureDir=path.join(tempRoot,'fixture','DVBStreamer5-10.8.411-linux-x86_64');
  fs.mkdirSync(fixtureDir,{recursive:true});
  fs.writeFileSync(path.join(fixtureDir,'VERSION'),'10.8.411\n');
  const archive=path.join(tempRoot,'fixture.tar.gz');
  const packed=spawnSync('tar',['-czf',archive,'-C',path.join(tempRoot,'fixture'),
    'DVBStreamer5-10.8.411-linux-x86_64'],{encoding:'utf8'});
  assert.equal(packed.status,0,'Fixture archive creation: '+packed.stderr);
  fs.writeFileSync(curl,[
    '#!/usr/bin/env bash',
    'for arg in "$@"; do',
    '  case "$arg" in',
    '    *latest.json*|https://tvstreammersat5.tech/) exit 22 ;;',
    '    -fsSLI|-I|--head) exit 22 ;;',
    '  esac',
    'done',
    'dest=""',
    'while (( $# )); do',
    '  if [[ "$1" == "-o" ]]; then shift; dest="$1"; fi',
    '  shift',
    'done',
    '[[ -n "$dest" ]] || exit 22',
    'cp '+JSON.stringify(archive)+' "$dest"',
    "printf '200 https://tvstreammersat5.tech/download.php?id=5c15e7ab4ff1c26daef79d745d211c3d'",
    'exit 0'
  ].join('\n')+'\n',{mode:0o755});
  const fallback=spawnSync('bash',['scripts/dvbstreamer5_update.sh','check','10.8.440'],{
    encoding:'utf8',timeout:15000,
    env:{...process.env,PATH:tempRoot+path.delimiter+process.env.PATH}
  });
  assert.equal(fallback.status,0,'HEAD 404 must fall back to GET: '+fallback.stderr);
  assert.equal(JSON.parse(fallback.stdout).latest,'10.8.411');
} finally {
  fs.rmSync(tempRoot,{force:true,recursive:true});
}
// Light/dark dashboard styling stays browser-local and does not touch stream state.
for(const marker of [
  'id="dashboardThemeButton"',
  'function toggleDashboardTheme()',
  'function updateDashboardThemeButton()',
  'dvbstreamer5-dashboard-theme',
  'document.documentElement.dataset.dashboardTheme=dashboardTheme',
  'localStorage.setItem(dashboardThemeKey,dashboardTheme)',
  'data-ui-ru="Светлая тема"',
  'data-ui-en="Light theme"',
  'data-ui-hr="Svijetla tema"',
  'html[data-dashboard-theme="light"] .tile',
  'html[data-dashboard-theme="light"] .modal-content',
  'html[data-dashboard-theme="light"] header'
]) assert.ok(web.includes(marker),'Missing light theme integration: '+marker);
assert.match(web,/function applyLanguage\(\)\s*\{[\s\S]*?updateDashboardThemeButton\(\);\s*\}/,
  'Language reapply must refresh the theme switch label');
const syntax=spawnSync('bash',['-n'],{input:updater,encoding:'utf8'});
assert.equal(syntax.status,0,'Bash updater syntax: '+syntax.stderr);
console.log('PASS: Bash update UI, API, portable packaging and syntax');
