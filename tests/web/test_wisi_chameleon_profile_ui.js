// V516: per-tile WISI Chameleon profile persists through UI and clipboard.
'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const source=fs.readFileSync(path.join(__dirname,'../../src/http/HttpServerWebUi.inc'),'utf8');
const id='streamWisiChameleonProfile';
assert.equal(source.split('id="'+id+'"').length-1,1,'WISI checkbox must be unique');
assert(source.includes('stream.wisi_chameleon_profile===true'),'stored WISI switch must load');
assert(source.includes('wisi_chameleon_profile: document.getElementById(\'streamWisiChameleonProfile\')?.checked === true'),
    'save request must persist explicit WISI switch');
assert(source.includes("['streamWisiChameleonProfile','wisi_chameleon_profile','checkbox']"),
    'copy/paste must retain WISI switch');
assert(source.includes('wisi_chameleon_profile:false'),
    'new tiles must stay off');
assert(source.includes("if (wisiProfile) wisiProfile.disabled = Boolean(copy);"),
    'video copy must disable WISI encoder switch');
const start=source.indexOf('<script>');
const end=source.indexOf('</script>',start);
assert(start>=0 && end>start,'web UI script found');
new Function(source.slice(start+8,end));
console.log('V516 WISI editor switch and JS syntax: OK');
