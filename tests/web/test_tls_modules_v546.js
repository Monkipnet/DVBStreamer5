'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const cp=require('node:child_process');
const read=p=>fs.readFileSync(p,'utf8');
const ui=read('src/http/HttpServerWebUi.inc');
const transport=read('src/http/HttpServerTransport.inc');
const header=read('src/http/TlsCertificateModules.h');
const portable=read('scripts/make_portable_linux_bundle.sh');
const direct=read('install_dvbstreamer5_binary.sh');
const main=read('scripts/tls/manage.sh');
const common=read('scripts/tls/modules/common.sh');
const self=read('scripts/tls/modules/selfsigned.sh');
const le=read('scripts/tls/modules/letsencrypt.sh');
const start=ui.indexOf('function openNetworkModal()');
const end=ui.indexOf('function subscriberStreamClassFromIds',start);
assert(start>0 && end>start);
new vm.Script(ui.slice(start,end));
for(const file of ['scripts/tls/manage.sh','scripts/tls/modules/common.sh',
                   'scripts/tls/modules/selfsigned.sh','scripts/tls/modules/letsencrypt.sh']){
 const result=cp.spawnSync('bash',['-n',file],{encoding:'utf8'});
 assert.equal(result.status,0,file+': '+result.stderr);
}
for(const marker of [
 'createManagedSelfSignedTls','toggleManagedLetsEncrypt','refreshManagedTlsStatus',
 'tlsManagedAutoRenew','/api/tls/status','/api/tls/selfsigned',
 '/api/tls/letsencrypt','SSL / TLS сертификаты',
 'SSL / TLS certificates','SSL / TLS certifikati',
 'Получать и автоматически обновлять сертификаты',
 'Obtain and automatically renew','automatski obnavljaj'
])assert.ok(ui.includes(marker),marker);
for(const marker of [
 'target == "/api/tls/status"',
 'target=="/api/tls/selfsigned"',
 'target=="/api/tls/letsencrypt"',
 'isPanelSessionAuthorized(req)',
 'SelfSignedModule::generate','LetsEncryptModule::setEnabled',
 'apiKeyAttempt','X-Requested-With','configManager.config.httpPort'
])assert.ok(transport.includes(marker),marker);
for(const marker of [
 'posix_spawn','--no-block','--unit=dvbstreamer5-tls-',
 'validDomain','validEmail','/etc/dvbstreamer5/tls/state.json',
 'struct SelfSignedModule','struct LetsEncryptModule'
])assert.ok(header.includes(marker),marker);
assert(!header.includes('std::system('));
assert(!header.includes('popen('));
for(const marker of ['flock -n 9','source "$HERE/modules/selfsigned.sh"',
                      'source "$HERE/modules/letsencrypt.sh"','le-disable','le-renew'])
 assert.ok(main.includes(marker),marker);
for(const marker of ['openssl req -x509','subjectAltName=DNS:',
                      'activate_certificate','status ok selfsigned'])
 assert.ok(self.includes(marker),marker);
for(const marker of ['certbot certonly --config-dir "$ACME_CONFIG"', 'certbot certonly --config-dir', 'OnCalendar=daily',
                      'enable --now dvbstreamer5-le-renew.timer',
                      'disable --now dvbstreamer5-le-renew.timer',
                      'status warning letsencrypt acme_failed_retry_scheduled'])
 assert.ok(le.includes(marker),marker);
for(const marker of ['proxy_pass http://127.0.0.1:','proxy_set_header X-Real-IP',
                      'ssl_certificate','nginx -t','check_domain','check_email',
                      'nginx_config_unmanaged'])
 assert.ok(common.includes(marker),marker);
assert(common.includes('ACME_CONFIG=$TLS_DIR/acme-client'));
assert(common.includes('nginx_preflight'));
assert(common.includes('nginx_reload_failed'));
assert(le.includes('le_renew()'));
assert(le.includes('le_disable()'));
assert(le.includes('ACME_LOG'));
assert(portable.includes('"$STAGE/tls/manage.sh"'));
assert(portable.includes('bin lib web ca-plugins etc tls run.sh'));
assert(direct.includes('"$INSTALL_DIR/tls/manage.sh"'));
console.log('PASS V546 independent TLS modules, tri-locale UI, admin API, safe execution, disabled default renewal and bundle wiring');
