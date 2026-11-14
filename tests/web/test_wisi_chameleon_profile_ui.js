// V518: WISI compatibility is automatic on transcoded UDP CBR/VBR outputs.
'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const source=fs.readFileSync(path.join(__dirname,'../../src/http/HttpServerWebUi.inc'),'utf8');
assert(!source.includes('id="streamWisiChameleonProfile"'),
    'WISI must have no manual checkbox');
assert(source.includes('id="streamWisiAutomaticProfileInfo"'),
    'automatic CBR/VBR policy information must appear in editor');
assert(!source.includes('wisi_chameleon_profile: document.getElementById('),
    'save must not depend on obsolete checkbox');
assert(!source.includes("'streamWisiChameleonProfile','wisi_chameleon_profile'"),
    'clipboard must no longer depend on obsolete checkbox');
// V517: stream manager must preserve previously selected 500ms–15s
// transport startup choices when enabling Source-PCR/20ms output PCR.
const manager=fs.readFileSync(
  path.join(__dirname,'../../src/StreamManager.cpp'),'utf8');
assert(manager.includes(
  'relay.wisiSourcePcrStrict = wisiCbrProfileActive || ipSourceClockAb;'),
  'automatic CBR WISI profile must preserve Source-PCR startup-buffer marker');
assert(manager.includes('WisiChameleonProfilePolicy::cbrEligible(') &&
       manager.includes('WisiChameleonProfilePolicy::vbrEligible('),
       'CBR and VBR automatic eligibility must be wired');
assert(manager.includes('relay.vbrPeakBitsPerSecond = wisiVbrPeakBitsPerSecond;'),
       'VBR peak pacer must be connected to the selected channel');
const start=source.indexOf('<script>');
const end=source.indexOf('</script>',start);
assert(start>=0 && end>start,'web UI script found');
new Function(source.slice(start+8,end));
console.log('V518 automatic WISI CBR/VBR editor and JS syntax: OK');
