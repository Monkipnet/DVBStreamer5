'use strict';
const fs = require('fs');
const assert = require('assert');
const src = fs.readFileSync('src/OscamMiniManager.cpp', 'utf8');
const helper = fs.readFileSync('src/OscamMiniLogTail.h', 'utf8');
const test = fs.readFileSync('tests/test_oscam_mini_file_log_v630.cpp', 'utf8');

assert.match(src, /#include "OscamMiniLogTail\.h"/);
assert.match(src, /oscamlog::readServiceStdout\(pid\)/,
  'OSCam status must resolve its actual service stdout');
assert.match(src, /if \(fileLog\.fileBacked\)/,
  'file-backed stdout must take precedence over journald');
assert.match(src, /log = fileLog\.log/);
assert.match(src, /recentReaderLog = fileLog\.recent/);
assert.match(src, /result\["log"\] = log/);
assert.match(src, /journalctl -u /,
  'retain the ordinary systemd journal path for non-file services');
assert.match(helper, /read_symlink\(fs::path\("\/proc"\) \/ pid \/ "fd\/1"/);
assert.match(helper, /trustedLogPath\(path\)/);
assert.match(helper, /32000/);
assert.match(helper, /recentLines\(result\.log, std::time\(nullptr\)\)/);
assert.match(helper, /recentlyModified\(path\)/);
assert.match(test, /assert\(recent\.find\(oldLine\) == std::string::npos\)/);
assert.match(test, /assert\(text\.size\(\) <= 32000\)/);
console.log('PASS OSCam web status file log fallback, journal support and security limits');
