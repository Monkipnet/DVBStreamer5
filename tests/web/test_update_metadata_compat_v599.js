'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const os=require('node:os');
const path=require('node:path');
const {spawnSync}=require('node:child_process');
const src=fs.readFileSync('scripts/dvbstreamer5_update.sh','utf8');
const ui=fs.readFileSync('src/http/HttpServerWebUi.inc','utf8');
assert.match(src,/RELEASE_URL="\$URL"/);
assert.match(src,/elif command -v python3/);
assert.match(src,/json\.load\(source\)/);
assert.match(src,/canonical versioned HTTPS archive/);
assert.match(src,/\$RELEASE_URL"\)/);
assert.match(ui,/data-ui-de="Auf Updates prüfen"/);
assert.match(ui,/data-ui-de="Update installieren"/);
assert.match(ui,/language==='de'\?\(de\|\|en\)/);
const root=fs.mkdtempSync(path.join(os.tmpdir(),'dvb5-updater-v599-'));
try {
  const bin=path.join(root,'bin');
  fs.mkdirSync(bin);
  // Isolated PATH: all check dependencies except jq; Python3 fallback must work.
  for(const name of ['bash','curl','tar','strings','sha256sum','flock',
                     'mktemp','stat','sed','python3','rm','sort','grep',
                     'tr','cut']){
    if(name==='curl') continue;
    const resolved=spawnSync('which',[name],{encoding:'utf8'});
    assert.equal(resolved.status,0,'Missing runner tool '+name);
    fs.symlinkSync(resolved.stdout.trim(),path.join(bin,name));
  }
  const manifest=path.join(root,'manifest.json');
  const curl=path.join(bin,'curl');
  fs.writeFileSync(curl,[
    '#!/bin/bash',
    'dest=""',
    'while (( $# )); do',
    '  if [[ "$1" == "-o" ]]; then shift; dest="$1"; fi',
    '  shift',
    'done',
    '[[ -n "$dest" ]] || exit 22',
    '/bin/cp "$FAKE_RELEASE_MANIFEST" "$dest"',
    'exit 0'
  ].join('\n')+'\n',{mode:0o755});
  const version='10.8.599';
  function check(url,expectOk){
    fs.writeFileSync(manifest,JSON.stringify({
      version,url,sha256:'a'.repeat(64),size:32768
    }));
    const r=spawnSync('/bin/bash',
      ['scripts/dvbstreamer5_update.sh','check','10.8.589'],{
        env:{...process.env,PATH:bin,FAKE_RELEASE_MANIFEST:manifest},
        encoding:'utf8',timeout:12000
      });
    if(expectOk){
      assert.equal(r.status,0,'check failed: '+r.stderr);
      const report=JSON.parse(r.stdout);
      assert.equal(report.current,'10.8.589');
      assert.equal(report.latest,version);
      assert.equal(report.available,true);
      assert.equal(r.stderr.trim(),'');
    }else{
      assert.notEqual(r.status,0,'must reject unsafe URL '+url);
      assert.match(r.stderr,/Unexpected release URL/);
    }
  }
  check('https://tvstreammersat5.tech/downloads/dvbstreamer5/DVBStreamer5-10.8.599-linux-x86_64.tar.gz',true);
  check('https://tvstreammersat5.tech/download.php?id=5c15e7ab4ff1c26daef79d745d211c3d',true);
  check('https://malicious.example/DVBStreamer5-10.8.599-linux-x86_64.tar.gz',false);
  check('https://tvstreammersat5.tech/downloads/dvbstreamer5/DVBStreamer5-10.8.598-linux-x86_64.tar.gz',false);
}finally{fs.rmSync(root,{recursive:true,force:true});}
console.log('PASS V599: metadata checks without jq, fixed PHP or canonical versioned release, URL rejection, DE UI');
