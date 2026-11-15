'use strict';
// V546: exercise actual modules in an isolated filesystem with mocked services.
const assert=require('node:assert/strict');
const fs=require('node:fs'),os=require('node:os'),path=require('node:path');
const child=require('node:child_process');
const root=fs.mkdtempSync(path.join(os.tmpdir(),'dvb5-tls-lifecycle-'));
const files=path.join(root,'tls'),bin=path.join(root,'bin');
const dst=file=>path.join(root,file);
function ensure(p){fs.mkdirSync(p,{recursive:true});}
function run(args,env={}){
  const proc=child.spawnSync('bash',[path.join(files,'manage.sh'),...args],{
    env:{...process.env,PATH:bin+':'+process.env.PATH,...env},
    encoding:'utf8',timeout:30000
  });
  assert.equal(proc.status,0,'TLS action '+args[0]+' failed: '+proc.stderr);
  return JSON.parse(fs.readFileSync(dst('etc/dvbstreamer5/tls/state.json'),'utf8'));
}
try{
  ensure(files+'/modules');ensure(bin);
  for(const x of ['etc/nginx/conf.d','etc/systemd/system','run/lock'])ensure(dst(x));
  const replacements=[
    ['/etc/dvbstreamer5/tls',dst('etc/dvbstreamer5/tls')],
    ['/var/lib/dvbstreamer5/acme-client-work',dst('var/lib/dvbstreamer5/acme-client-work')],
    ['/var/lib/dvbstreamer5/acme',dst('var/lib/dvbstreamer5/acme')],
    ['/var/log/dvbstreamer5/acme-client',dst('var/log/dvbstreamer5/acme-client')],
    ['/etc/nginx/conf.d',dst('etc/nginx/conf.d')],
    ['/etc/systemd/system',dst('etc/systemd/system')],
    ['/run/lock/dvbstreamer5-tls.lock',dst('run/lock/dvbstreamer5-tls.lock')]
  ];
  for(const file of ['manage.sh','modules/common.sh','modules/selfsigned.sh','modules/letsencrypt.sh']){
    let source=fs.readFileSync('scripts/tls/'+file,'utf8');
    for(const [from,to] of replacements)source=source.split(from).join(to);
    if(file==='manage.sh')source=source.replace(
      "(( EUID == 0 )) || { echo 'TLS operations require root' >&2; exit 2; }",
      '# Disposable CI sandbox; no real host operations');
    fs.writeFileSync(path.join(files,file),source,{mode:0o700});
  }
  fs.writeFileSync(path.join(bin,'nginx'),'#!/usr/bin/env bash\nexit 0\n',{mode:0o755});
  fs.writeFileSync(path.join(bin,'systemctl'),
    '#!/usr/bin/env bash\necho "$*" >> "'+dst('systemctl-calls')+'"\nexit 0\n',{mode:0o755});
  const fakeCertbot=[
    '#!/usr/bin/env bash','set -Eeuo pipefail',
    'if [[ "${DVB5_TEST_CERTBOT_FAIL:-0}" == "1" ]]; then exit 2; fi',
    "config=''; domain=''",'while (( $# > 0 )); do',
    '  case "$1" in',
    '    --config-dir) config="$2"; shift 2 ;;',
    '    -d) domain="$2"; shift 2 ;;',
    '    *) shift ;;','  esac','done',
    '[[ -n "$config" && -n "$domain" ]]',
    'mkdir -p "$config/live/$domain"',
    'openssl req -x509 -newkey rsa:2048 -sha256 -nodes -days 90 \\',
    '  -keyout "$config/live/$domain/privkey.pem" \\',
    '  -out "$config/live/$domain/fullchain.pem" \\',
    '  -subj "/CN=$domain" -addext "subjectAltName=DNS:$domain" >/dev/null 2>&1'
  ].join('\n')+'\n';
  fs.writeFileSync(path.join(bin,'certbot'),fakeCertbot,{mode:0o755});
  let state=run(['selfsigned','blumen.zapto.org','9000','9443']);
  assert.equal(state.phase,'ok');
  assert.equal(state.auto_renew,false);
  const cert=dst('etc/dvbstreamer5/tls/active.crt');
  const old=fs.readlinkSync(cert);
  state=run(['le-enable','blumen.zapto.org','admin@example.org','9000','9443'],
            {DVB5_TEST_CERTBOT_FAIL:'1'});
  assert.equal(state.phase,'warning');
  assert.equal(state.auto_renew,true);
  assert.equal(fs.readlinkSync(cert),old,'ACME failure must retain valid cert');
  assert(fs.existsSync(dst('etc/systemd/system/dvbstreamer5-le-renew.timer')));
  state=run(['le-renew']);
  assert.equal(state.phase,'ok');
  assert.equal(state.mode,'letsencrypt');
  assert.equal(state.auto_renew,true);
  assert(fs.readlinkSync(cert).includes('/acme-client/live/blumen.zapto.org/'));
  const current=fs.readlinkSync(cert);
  state=run(['le-disable']);
  assert.equal(state.phase,'ok');
  assert.equal(state.auto_renew,false);
  assert.equal(fs.readlinkSync(cert),current);
  assert(!fs.existsSync(dst('etc/dvbstreamer5/tls/letsencrypt.env')));
  const calls=fs.readFileSync(dst('systemctl-calls'),'utf8');
  assert(calls.includes('enable --now dvbstreamer5-le-renew.timer'));
  assert(calls.includes('disable --now dvbstreamer5-le-renew.timer'));
  const nginx=fs.readFileSync(dst('etc/nginx/conf.d/dvbstreamer5-managed-tls.conf'),'utf8');
  assert(nginx.includes('proxy_pass http://127.0.0.1:9000;'));
  assert(nginx.includes('proxy_set_header X-Real-IP $remote_addr;'));
  console.log('PASS V546 sandbox: self-signed, ACME failure, recovery, renewal and opt-out');
}finally{
  fs.rmSync(root,{recursive:true,force:true});
}
