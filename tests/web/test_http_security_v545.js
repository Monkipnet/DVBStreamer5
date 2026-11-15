'use strict';
// V545: verify that the opt-in proxy and HTTP guard are wired into all paths.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const read=p=>fs.readFileSync(p,'utf8');
const web=read('src/http/HttpServerWebUi.inc');
const transport=read('src/http/HttpServerTransport.inc');
const media=read('src/http/HttpServerDvbMedia.inc');
const routing=read('src/http/HttpServerClusterRouting.inc');
const header=read('src/HttpServer.h');
const config=read('src/ConfigManager.cpp');
const policy=read('src/http/HttpSecurityPolicy.h');
const ci=read('.github/workflows/build-main-linux.yml');
for(const marker of [
 'trustLocalProxyRealIp', 'trust_local_proxy_real_ip',
 'saveTrustedProxySetting(this)', 'saveTrustedProxySetting(control)',
 'Доверять реальному IP', 'Trust real client IP', 'Vjeruj stvarnoj IP',
 '/api/security/proxy'
]) assert.ok(web.includes(marker)||transport.includes(marker)||config.includes(marker),marker);
for(const marker of [
 'parser.header_limit(16U*1024U)','parser.body_limit(maxBody)',
 'HTTP header deadline exceeded','HTTP body deadline exceeded',
 'panelLoginThrottle_.allowed','panelLoginThrottle_.record',
 'allowStateChangingBrowserRequest','SameSite=Strict',
 'X-Content-Type-Options','Referrer-Policy',
 'clientIpForRequest(socket,req)','effectiveClientIp('
])assert.ok(transport.includes(marker),marker);
assert.ok(!transport.includes('parser.body_limit(512ULL'));
assert.ok(transport.includes('isPanelSessionAuthorized(req)||isAuthorized(req)'));
assert.ok(media.includes('const std::string& effectiveClientIp'));
assert.ok(routing.includes('clientIpForRequest(socket,req)'));
assert.ok(header.includes('http_security::LoginThrottle'));
assert.ok(policy.includes('isLoopback(socketIp)'));
assert.ok(policy.includes('realIpHeader.find_first_of'));
assert.ok(ci.includes('test_http_security_v545.cpp'));
console.log('PASS V545 HTTP size/time limits, CSRF, throttling, tri-locale admin proxy and media IP wiring');
