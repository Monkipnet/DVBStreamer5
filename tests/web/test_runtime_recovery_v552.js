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
const tsdecrypt=read('src/ca/backends/tsdecrypt/TsdecryptPlugin.cpp');
const service=read('scripts/make_portable_linux_bundle.sh');
const legacy=read('packaging/dvbstreamer5.service');
const standalone=read('packaging/universal/run-installer.sh');
assert(fs.statSync('install_dvbstreamer5_binary.sh').mode & 0o111,
 'legacy installer must retain its executable Git mode');
const version=read('src/AppVersion.h');
const patchVersion = Number(version.match(/kProgramVersion = "10\.8\.(\d+)"/)?.[1]);
assert(Number.isInteger(patchVersion) && patchVersion >= 552,
 'V552 recovery regression must support later patch releases');
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
assert(pcr.includes('lastResetWallGapMs()'));
assert(udp.includes('pcr_wall_gap_ms='));
assert(udp.includes('preserved_packets='));
assert(udp.includes('pending_pcr_epochs='));
assert(udp.includes('preservedEpochs_.clear()'));
assert(udp.includes('std::strcmp(clock_.lastResetCause()'));
assert(pcr.includes('takePreviousAnchors()'));
assert(pcr.includes('timeNsForAnchors('));
assert(tsdecrypt.includes('output_null_child=%llu'));
assert(tsdecrypt.includes('output_null_underrun=%llu'));
assert(tsdecrypt.includes('outputUnderrunNullPackets.fetch_add(1)'));
assert(tsdecrypt.includes('outputChildNullPackets.fetch_add(1)'));
assert(tsdecrypt.includes('output_null_underrun_packets'));
for(const content of [service,standalone,legacy]){
 assert(content.includes('TimeoutStopSec=20'));
 assert(content.includes('KillMode=control-group'));
 assert(content.includes('SendSIGKILL=yes'));
}
console.log('PASS V552 stalled input retry, operator/failover gates, PCR and systemd restart bounds');
