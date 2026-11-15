'use strict';
// V541 synthetic cluster settings + TLS regression, no RF/tokens/network required.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const ui=fs.readFileSync('src/http/ClusterWebUiInject.h','utf8');
const api=fs.readFileSync('src/http/HttpServerCluster.inc','utf8');
const store=fs.readFileSync('src/http/ClusterPeerStore.h','utf8');
const tls=fs.readFileSync('scripts/setup_cluster_tls.sh','utf8');
const script=ui.slice(ui.indexOf('static const char* script='));
const js=script.slice(script.indexOf('<script>')+8,script.indexOf('</script>'));
new vm.Script(js);
for(const marker of [
 "openClusterSettings(key)","cluster-server-spoiler","details.open=!details.open",
 "clusterEditingOriginalUrl","original_url:clusterEditingOriginalUrl",
 "data-cluster-edit","selected?.name","selected?.url","selected?.viewer_url",
 "Сохранить сервер","Save server","Spremi poslužitelj",
 "Отмена редактирования","Cancel editing","Odustani od uređivanja",
 "Оставьте ключ пустым","Leave token empty","Ostavite ključ praznim",
 "clusterPeerVerifyTls","Проверять сертификат TLS","Verify TLS certificate",
 "Provjeri TLS certifikat","verify_tls,","selected.verify_tls!==false"
])assert.ok(ui.includes(marker),marker);
for(const marker of [
 'original_url','Invalid original cluster peer address','Unknown peer',
 'Cluster peer address already exists','token=found->token',
 'duplicate!=config.peers.end() && duplicate!=found',
 'options.verifyTlsPeer=peer.verifyTlsPeer', 'input.get("verify_tls"'
])assert.ok(api.includes(marker),marker);
assert.ok(!api.includes('result["token"]=peer.token'));
assert.ok(store.includes('bool verifyTlsPeer = true'));
assert.ok(store.includes('item["verify_tls"]=peer.verifyTlsPeer'));
for(const marker of [
 'certbot certonly --standalone','--keep-until-expiring',
 'openssl req -x509','subjectAltName=DNS:',
 'nginx -t','systemctl reload nginx',
 'dvbstreamer5-cluster-tls.timer','OnCalendar=daily',
 'proxy_pass http://127.0.0.1:','proxy_buffering off',
 'SELF-SIGNED fallback'
])assert.ok(tls.includes(marker),marker);
console.log('PASS V541 cluster row editing, original URL update, password preservation, RU EN HR, TLS LE/fallback/renewal wiring');
