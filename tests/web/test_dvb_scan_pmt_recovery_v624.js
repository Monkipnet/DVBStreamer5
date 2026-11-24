const fs = require('fs');
const assert = require('assert');
const source = fs.readFileSync('src/DvbSatellite.cpp', 'utf8');
assert.match(source, /tune\.pids = "8192"/, 'scan must read all TS PIDs');
assert.match(source, /attemptScanner\.discoveredProgramCount\(\) > 0/, 'extend only after PAT discovery');
assert.match(source, /attemptScanner\.readyProgramCount\(\) < attemptScanner\.discoveredProgramCount\(\)/, 'extend for incomplete PMTs');
assert.match(source, /timeoutMs < 18000/, 'do not extend full-length scans');
assert.match(source, /attemptDeadline \+= std::chrono::milliseconds\(11500\)/, 'extend 6500 to 18000 ms once');
assert.match(source, /extendedForPmt = true/, 'extension must run only once');
assert.match(source, /result\["pmt_ready_count"\]/, 'report decoded PMT service count');
assert.match(source, /result\["pmt_ts_packet_counts"\]/, 'report received TS packets for each PMT PID');
assert.match(source, /\+\+pmtPackets\[pid\]/, 'count inbound PMT TS packets');
assert.match(source, /externalDvbFrontendOwners/, 'retain external frontend protection');
assert.match(source, /hasActiveFrontend/, 'retain live frontend protection');
// V627: a strong frontend LOCK without a PAT must be distinguishable from
// an empty Linux DVB DVR and from a parser that cannot assemble PSI sections.
for (const [field, label] of [
  ['ts_packets_received', 'TS packet count'],
  ['pat_ts_packet_count', 'PID 0 packet count'],
  ['pat_section_count', 'PAT section count'],
  ['tei_ts_packet_count', 'transport error count'],
  ['top_ts_pid_packet_counts', 'sampled PID histogram'],
  ['dvr_bytes_read', 'DVR byte count'],
  ['dvr_data_reads', 'DVR nonempty reads'],
]) {
  assert.ok(source.includes('result["' + field + '"]'), 'Missing ' + label);
}
assert.match(source, /DVB SCAN TS DIAG/, 'SSH-only scan must log TS/PAT/PMT stage');
assert.match(source, /\+\+pidPackets_\[pid\]/, 'all TS PIDs must be accounted');
assert.match(source, /\+\+patSections_/, 'decoded PAT sections must be accounted');
assert.match(source, /attemptBytesRead \+= received/, 'read byte count must come from real DVR reads');
assert.match(source, /attemptScanner\.feed\(buffer\.data\(\), received\)/, 'keep original PSI feed');
assert.match(source, /externalDvbFrontendOwners/, 'scan may not retune an external live owner');
// V628: DVR must have its 8 MiB ring before the first TS_TAP filter is
// started; large scan reads and nonblocking transport sampling protect PSI.
const nativeDvb = fs.readFileSync('src/media/LinuxDvbInput.cpp', 'utf8');
const openStart = nativeDvb.indexOf('bool LinuxDvbInput::open(');
const dvrOpen = nativeDvb.indexOf('const std::string dvr = devicePath(config, "dvr")', openStart);
const filterStart = nativeDvb.indexOf('const std::string demux = devicePath(config, "demux")', openStart);
const dvrResize = nativeDvb.indexOf('configureDvbDvrBuffer(', openStart);
assert.ok(openStart >= 0 && dvrOpen > openStart && filterStart > dvrOpen,
  'DVR must open before starting demux TS filters');
assert.ok(dvrResize > dvrOpen && dvrResize < filterStart,
  'DVR ring must resize before starting demux TS filters');
assert.match(source, /188 \* 2048/, 'scan must drain full-TS DVR in large packet-aligned buffers');
assert.match(source, /if \(!collectServices &&\s*\(/,
  'scan must not block DVR draining on repeated frontend ioctls');
console.log('PASS DVB PMT recovery, TS/PAT diagnostics and full-TS DVR drain guards');
