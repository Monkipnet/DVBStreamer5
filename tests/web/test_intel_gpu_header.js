// Standalone regression test for the actual UI's Intel GPU-load selection.
// Run: node tests/web/test_intel_gpu_header.js
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(
  __dirname, '../../src/http/HttpServerWebUi.inc'), 'utf8');
const start = source.indexOf('function loadProcessGpuForCard(metrics, gpu) {');
const end = source.indexOf('// Stable chart colors identify', start);
assert.ok(start >= 0 && end > start, 'expected GPU display helpers missing');
const context = vm.createContext({});
vm.runInContext(source.slice(start, end), context);
const pick = context.gpuDisplayLoad;

const intel = {name:'Intel UHD 630',vendor:'Intel',
               source:'DRM',card:'card0',pci_bus_id:'0000:00:02.0'};
const nvidia = {name:'NVIDIA GTX 1050',vendor:'NVIDIA',
                utilization_percent:63,pci_bus_id:'0000:01:00.0'};

let reading=pick({process_gpus:[{
  pci_bus_id:'0000:00:02.0',engines:{video:48,render:7,'video-enhance':12}
}]},intel);
assert.equal(reading.percent,48);
assert.equal(reading.scope,'app');
assert.equal(reading.engine,'video');

reading=pick({process_gpus:[{
  pci_bus_id:'0000:00:02.0',engines:{vcs:37,rcs:14}
}]},intel);
assert.equal(reading.percent,37);
assert.equal(reading.scope,'app');

reading=pick({process_gpus:[{
  pci_bus_id:'0000:00:02.0',engines:{video:0,render:0}
}]},intel);
assert.equal(reading.percent,0);
assert.equal(reading.scope,'app');

reading=pick({process_gpus:[{
  pci_bus_id:'0000:03:00.0',engines:{video:99}
}]},intel);
assert.equal(reading.percent,null);
assert.equal(reading.scope,'unavailable');

reading=pick({process_gpus:[]},nvidia);
assert.equal(reading.percent,63);
assert.equal(reading.scope,'host');

const intelWithGlobal={...intel,utilization_percent:12};
reading=pick({process_gpus:[{
  pci_bus_id:'0000:00:02.0',engines:{video:91}
}]},intelWithGlobal);
assert.equal(reading.percent,12);
assert.equal(reading.scope,'host');

// A missing driver counter is not 0% or the utilization of another card.
reading=pick({process_gpus:[]},intel);
assert.equal(reading.percent,null);

console.log('V10.8.241 Intel i915/Xe GPU header fallback PASS');
