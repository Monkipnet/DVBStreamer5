'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const {spawnSync} = require('node:child_process');
const root = process.cwd();
const read = p => fs.readFileSync(p, 'utf8');
const vendor = read('scripts/vendor_native_codecs.sh');
const codecs = read('scripts/build_native_codecs.sh');
const decrypt = read('scripts/build_tsdecrypt_icam.sh');
const embedded = read('cmake/EmbedWebAssets.cmake');
const workflow = read('.github/workflows/build-main-linux.yml');
for (const script of [
  'scripts/vendor_native_codecs.sh',
  'scripts/build_native_codecs.sh',
  'scripts/build_tsdecrypt_icam.sh',
  'scripts/make_portable_linux_bundle.sh'
]) {
  const p = spawnSync('bash', ['-n', script], {cwd:root, encoding:'utf8'});
  assert.equal(p.status, 0, script + ' syntax: ' + p.stderr);
}
for (const marker of [
  'DVBSTREAMER5_VENDOR_FORCE',
  'Reusing pinned',
  'DVBSTREAMER5_VENDOR_INFO.txt',
  'grep -Fxq "Commit: $commit" "$manifest"'
])
  assert.ok(vendor.includes(marker), 'Missing pinned-source no-op: ' + marker);
for (const marker of [
  'DVBSTREAMER5_CODEC_FORCE_REBUILD',
  'CODEC_FINGERPRINT',
  'CODEC_CACHE_COMPLETE',
  'CODEC_STAMP',
  'reusing $PREFIX',
  'printf \'%s\\n\' "$CODEC_FINGERPRINT" > "$CODEC_STAMP"'
])
  assert.ok(codecs.includes(marker), 'Missing codec fingerprint/no-op: ' + marker);
assert.ok(codecs.includes('rm -rf "$BUILD_ROOT" "$PREFIX"'),
  'Explicit rebuild must still rebuild dependencies on changed pins/toolchain');
for (const marker of [
  'DVBSTREAMER5_TSDECRYPT_FORCE_REBUILD',
  'TSDECRYPT_FINGERPRINT',
  'reusing $OUT',
  'TSDECRYPT_STAMP'
]) assert.ok(decrypt.includes(marker), 'Missing pinned iCAM build cache: ' + marker);
assert.ok(embedded.includes('if(should_write)'),
  'Unchanged embedded assets must not be rewritten during CMake reconfigure');
for (const marker of [
  'actions/cache@v4',
  '-DCMAKE_C_COMPILER_LAUNCHER=ccache',
  '-DCMAKE_CXX_COMPILER_LAUNCHER=ccache',
  'CCACHE_DIR:',
  'dvbstreamer5-native-codecs-ubuntu2404-',
  'dvbstreamer5-tsdecrypt-ubuntu2404-',
  'ccache --show-stats'
]) assert.ok(workflow.includes(marker), 'Missing CI incremental cache: ' + marker);
assert.ok(!workflow.includes('cmake --build build-main-ci --clean-first'),
  'CI must not force clean builds');
console.log('Incremental build policy: OK');
