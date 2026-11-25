'use strict';
const fs=require('fs'), assert=require('assert'), vm=require('vm');
const src=fs.readFileSync('src/OscamMiniManager.cpp','utf8');
const js=src.slice(src.indexOf('const newcamdNetworkText='),src.indexOf('const oscamRuToEn='));
const context={localStorage:{getItem:()=>context.language},language:'en'};
vm.createContext(context);
vm.runInContext(js+'\nthis.text=networkText;',context);
for(const lang of ['en','ru','hr','de']) {
    context.language=lang;
    assert.equal(context.text().length,6);
    assert.ok(context.text().every(Boolean));
}
assert.match(src,/result\["bind_addresses"\] = newcamdBindAddresses\(\)/);
assert.match(src,/settings\.clientTimeout = previous\.clientTimeout/);
assert.match(src,/settings\.allowedIps = allowed/);
assert.match(src,/oscamnetwork::validBind\(settings\.bindIp, addresses\)/);
assert.match(src,/if\(!settings\.allowedIps\.empty\(\)\)|if \(!settings\.allowedIps\.empty\(\)\)/);
assert.match(src,/allowed_ips:bind_ip\.value==='127\.0\.0\.1'/);
console.log('PASS OSCam-mini managed listener UI and configuration wiring');
