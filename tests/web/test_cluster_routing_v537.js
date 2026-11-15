'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const root=path.join(__dirname,'../..');
const read=p=>fs.readFileSync(path.join(root,p),'utf8');
const routing=read('src/http/HttpServerClusterRouting.inc');
const peers=read('src/http/HttpServerCluster.inc');
const store=read('src/http/ClusterPeerStore.h');
const transport=read('src/http/HttpServerTransport.inc');
const apiKey=read('src/StreamAuthApiKey.h');
const web=read('src/http/ClusterWebUiInject.h');
const header=read('src/HttpServer.h');
const cmake=read('CMakeLists.txt');
// Middleware must be able to ask where to send its own authorized viewer.
// An IP-restricted middleware key must never edit cluster config/streams.
assert.match(apiKey,/path==\/?"\/api\/cluster\/route"/);
assert.match(apiKey,/method=="GET"/);
assert.match(transport,/target\.rfind\("\/api\/cluster\/route\?",0\)==0/);
assert.match(transport,/apiKeyGranted \|\| isAuthorized\(req\)/);
assert.match(transport,/queryValue\(target,"client_ip"\)/);
assert.match(transport,/queryValue\(target,"stream_id"\)/);
assert.match(transport,/format!="ts" && format!="hls"/);
assert.match(transport,/normalizeIpAddress\(ip\)/);
assert.match(transport,/isClientAllowedForStream\(id,ip\)/);
assert.match(transport,/dvbstreamer5::stream_auth::enabled\(configManager\.config,id\)/);
assert.match(transport,/res\.result\(http::status::forbidden\)/);
assert.match(transport,/\/api\/cluster\/routing\/save/);
assert.match(transport,/X-Requested-With/);
assert.match(transport,/apiKeyAttempt \|\|/);
assert.match(header,/clusterViewerAllowed/);
// Viewer entrypoint checks authorization BEFORE sending any redirect.
const watch=transport.slice(transport.indexOf('// V537: opt-in public viewer entrypoint'),
  transport.indexOf('const bool panelProtected = requiresAuthentication(target);'));
assert.ok(watch.indexOf('clusterViewerAllowed(')>=0);
assert.ok(watch.indexOf('clusterViewerAllowed(')<watch.indexOf('clusterChooseStream('));
assert.ok(watch.includes('http::status::temporary_redirect'));
assert.ok(watch.includes('http::field::location'));
assert.ok(watch.includes('http::status::forbidden'));
assert.ok(watch.includes('No healthy cluster output'));
assert.ok(!watch.includes('X-DVBStreamer5-Cluster-Key'));
assert.ok(!watch.includes('X-API-Key'));
// The actual downstream node still enforces per-client and middleware rules.
assert.ok(routing.includes('SessionRegistry::instance().check'));
assert.ok(routing.includes('isClientAllowedForStream(id,ip)'));
assert.ok(routing.includes('result["authorization_required"]'));
assert.ok(routing.includes('result["url"]'));
assert.ok(routing.includes('result["policy"]'));
assert.ok(routing.includes('clusterPeerStatus()'));
assert.ok(routing.includes('peer["channels"]'));
assert.ok(routing.includes('peer.get("viewer_url"'));
assert.ok(routing.includes('remoteTvgId'));
assert.ok(routing.includes('remoteId==id'));
assert.ok(routing.includes('localCfg->name==remoteName'));
assert.ok(routing.includes('ch.get("viewers"'));
assert.ok(routing.includes('!ch.get("active",false).asBool()'));
assert.ok(routing.includes('ch["bitrate_out_kbps"].asUInt64()==0'));
assert.ok(routing.includes('safeChannelId(slug)'));
assert.ok(routing.includes('streamLink(output,configManager.config.httpPort)'));
assert.ok(routing.includes('config.autoFailover='));
// Advertised remote streams must not expose keys or passwords.
for(const marker of ['channel["tvg_id"]','channel["slug"]',
 'channel["http"]','channel["hls"]','channel["viewers"]'])
 assert.ok(peers.includes(marker),marker);
assert.ok(!peers.includes('channel["token"]'));
assert.ok(store.includes('mode 0600'));
assert.ok(store.includes('auto_failover'));
assert.ok(store.includes('balance_viewers'));
assert.ok(store.includes('viewer_url'));
assert.ok(store.includes('validOrigin(peer.viewerUrl)'));
// GUI texts must be present in Russian, English, Croatian.
for(const marker of [
 'id="clusterAutoFailover"','id="clusterBalanceViewers"',
 'id="clusterPeerViewerUrl"','saveClusterRouting()',
 'Автоматически выбирать резервный сервер',
 'Automatically fail over new viewers',
 'Automatski preusmjeri nove gledatelje',
 'Балансировать новых зрителей',
 'Balance new viewers',
 'Raspodijeli nove gledatelje',
 'Сохранить маршрутизацию','Save routing','Spremi usmjeravanje',
 '/cluster/watch/ID.ts','/cluster/watch/ID.m3u8','/api/cluster/route',
 'data-ui-ru=','data-ui-en=','data-ui-hr='
 ])assert.ok(web.includes(marker),'Missing UI marker: '+marker);
assert.ok(cmake.includes('dvbstreamer5_cluster_test'));
assert.ok(cmake.includes('dvbstreamer5_cluster_routing_test'));
// All earlier media output handlers must still exist intact.
for(const marker of [
 'resolveHttpMpegTsTarget(configManager.config.streams, target, httpMpegTsId)',
 'resolveHlsTarget(configManager.config.streams, target, hlsId, hlsFile)',
 'handleHttpStream(socket, target)',
 'sendHlsFileZeroCopy'
 ])assert.ok(transport.includes(marker),'Media path regression: '+marker);
console.log('PASS V537 middleware API route, viewer auth, IP filtering, HTTP redirects, tri-locale controls and legacy media guards');
