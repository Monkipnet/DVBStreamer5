'use strict';
// V552 regression: a long-dead *network* input can be restarted without
// restarting the full daemon; OnDemand, operator Stop and failover remain gated.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const read=p=>fs.readFileSync(p,'utf8');
const manager=read('src/StreamManager.cpp');
const header=read('src/StreamManager.h');
const pcr=read('src/media/PcrVbrSourceClock.h');
const udp=read('src/media/PcrVbrUdpOutputWorker.cpp');
const service=read('scripts/make_portable_linux_bundle.sh');
const legacy=read('packaging/dvbstreamer5.service');
const standalone=read('packaging/universal/run-installer.sh');
const version=read('src/AppVersion.h');
assert(version.includes('"10.8.552"'));
assert(header.includes('std::atomic<std::int64_t> inputStallSinceMs{0}'));
assert(manager.includes('if (bufferedNetworkInput && inputHealth.stalled)'));
assert(manager.includes('state->inputStallSinceMs.store(0, std::memory_order_release)'));
assert(manager.includes('nowMs - stallSince >= 120000'));
assert(manager.includes('if (active && !stalledLongEnough)'));
assert(manager.includes('if (failoverEntries.count(id)) continue;'));
assert(manager.includes('std::try_to_lock'));
assert(manager.includes('if (isOperatorStopped(id) || isStopPending(id)) continue;'));
assert(manager.includes('if (stalledLongEnough) {\n                    // A live relay'));
assert(manager.includes('(void)stopStream(id);'));
assert(manager.includes('operator stop superseded recovery'));
assert(pcr.includes('last.pcr-pcr<kHz/1000ULL'));
assert(pcr.includes('"discontinuity_flag"'));
assert(pcr.includes('"backward_jump"'));
assert(pcr.includes('"forward_jump"'));
assert(udp.includes('dropped_packets='));
for(const content of [service,standalone,legacy]){
 assert(content.includes('TimeoutStopSec=20'));
 assert(content.includes('KillMode=control-group'));
 assert(content.includes('SendSIGKILL=yes'));
}
console.log('PASS V552 stalled input retry, operator/failover gates, PCR and systemd restart bounds');
