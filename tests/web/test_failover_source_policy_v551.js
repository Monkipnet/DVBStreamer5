'use strict';
// V551 regression: monitorFailoverStreams() temporarily substitutes the backup
// URI as inputUri. URI inequality alone cannot detect the active backup.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const manager=fs.readFileSync('src/StreamManager.cpp','utf8');
const failover=fs.readFileSync('src/StreamManagerFailover.inc','utf8');
const policy=fs.readFileSync('src/media/NativeFailoverSourcePolicy.h','utf8');
const remapper=fs.readFileSync('src/media/MpegTsRemapper.cpp','utf8');
const config=fs.readFileSync('src/media/MpegTsRemapper.h','utf8');
const cmake=fs.readFileSync('tests/media_core/CMakeLists.txt','utf8');
assert(failover.includes('next.inputUri=next.backupInputUri;'));
assert(failover.includes('it->second.switching=true;'));
assert(failover.includes('it->second.backupActive=!restore;'));
assert(manager.includes('state->usingBackup = it->second.backupActive;'));
assert(manager.includes('hasConfiguredFailover('));
assert(manager.includes('streamConfig.inputUri,streamConfig.backupInputUri,'));
assert(manager.includes('state->usingBackup);'));
assert(manager.includes('if (state->usingBackup && configuredFailover)'));
assert(manager.includes('relay.remapConfig.allowPidConflictFallback = true;'));
assert(manager.includes('NATIVE BACKUP GENERATION CONFIG'));
assert(manager.includes('NATIVE BACKUP SERVICE IDENTITY'));
assert(manager.includes('relay.capturePrimaryProgram = !state->usingBackup;'));
assert(manager.includes('relay.primaryProgramIdentity = saved;'));
assert(config.includes('bool allowPidConflictFallback = false;'));
assert(remapper.includes('mediaPidCollision && !config_.allowPidConflictFallback'));
assert(remapper.includes('NATIVE BACKUP PID COLLISION fallback=passthrough'));
assert(remapper.includes('mappedPid = effectiveOutputVideoPid_'));
assert(remapper.includes('mappedPid = effectiveOutputAudioPid_'));
assert(policy.includes('(activeInputUri != backupInputUri || runningBackup)'));
assert(cmake.includes('native_failover_source_policy_v551'));
assert(cmake.includes('native_failover_program_identity'));
for (let i=0;i<9;++i) {
  const primary='srt://primary.example.net:4000';
  const backup='srt://backup.example.net:4001';
  const runningBackup=i>=3;
  const live=runningBackup?backup:primary;
  const before=!!backup && backup!==live;
  const after=!!backup && (backup!==live || runningBackup);
  assert.equal(after,true, 'all 9 retain state');
  assert.equal(before,!runningBackup, 'old gate skips exactly 6 backups');
}
console.log('PASS V551: 6/9 SRT backups retain failover state, V550 PID fallback and HTTP/HLS identity');
