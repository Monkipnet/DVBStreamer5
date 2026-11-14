# DVBStreamer5 V10.8.524 — incremental builds, main only

## Policy

Never delete or recreate the CMake build directory during routine updates.
CMake's generated dependency graph recompiles only changed translation units
and their actual dependents. Linking changed targets and building a new fully
self-contained portable archive are still required.

Local `cmake --build build-release --parallel 2` already supports incremental
rebuilds. The regressions were dependency recipes which previously deleted
`third_party/native-codecs-prefix` and downloaded vendored codec sources on
every CI run.

## Changes

- `scripts/vendor_native_codecs.sh`: pinned source tree reuse when the
  exact upstream repo and commit match its checked manifest; redownload only on
  missing/mismatched tree or `DVBSTREAMER5_VENDOR_FORCE=1`.
- `scripts/build_native_codecs.sh`: validated binary-cache stamp tied to
  the recipe, vendored pinned commits, compiler and architecture. Reuse
  `third_party/native-codecs-prefix` if all required headers/static archives
  exist and the fingerprint matches; explicit
  `DVBSTREAMER5_CODEC_FORCE_REBUILD=1` invalidates it.
- `scripts/build_tsdecrypt_icam.sh`: reuse the validated executable when
  TSDecrypt source/submodules, ECM PID policy, CSA static library, build script
  and compiler fingerprint are unchanged; override with
  `DVBSTREAMER5_TSDECRYPT_FORCE_REBUILD=1`. No installed service is affected.
- `cmake/EmbedWebAssets.cmake`: write the generated translation unit only
  when content differs, to preserve timestamps across CMake configure.
- `.github/workflows/build-main-linux.yml`: enable C/C++ ccache, restore
  caches for pinned SRT sources, pinned codec sources and static archives,
  iCAM TSDecrypt source and executable, and report cache statistics.
  GitHub-hosted runners remain ephemeral: ccache may compile fresh the first
  time or after eviction; it is not a persistent build filesystem.
- `tests/web/test_incremental_build_policy.js`: checks guardrails and shell
  script syntax; run in Actions before actual compilation.

## Commands from source root

```bash
git switch main
git pull --ff-only origin main

# First setup only if no configured CMake build directory exists:
test -s build-release/CMakeCache.txt ||
  cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release

# Every subsequent update: reuse the existing build directory.
cmake --build build-release --parallel 2
ctest --test-dir build-release --output-on-failure
node tests/web/test_incremental_build_policy.js

# Complete portable archive, version in its name:
bash scripts/make_portable_linux_bundle.sh --build-dir build-release --with-oscam --archive
VERSION="$(sed -n 's/.*kProgramVersion = "\([^"]*\)";.*/\1/p' src/AppVersion.h)"
(cd dist && sha256sum -c "DVBStreamer5-${VERSION}-linux-x86_64.tar.gz.sha256")
```

To enable ccache on the local machine, install `ccache` then configure
the existing CMake tree with `-DCMAKE_C_COMPILER_LAUNCHER=ccache` and
`-DCMAKE_CXX_COMPILER_LAUNCHER=ccache` without deleting the tree; use
`ccache -s` to view cache hit rate.

When codec dependencies are genuinely changed, one full rebuild of the
affected native dependency tree is required. When a common header changes,
all translation units depending on it must be rebuilt to remain correct.
A fully portable Linux .tar.gz always contains all runtime dependencies
regardless of how few source files changed.

GitHub Actions build completion and actual ccache hit rate were not confirmed
during this change; verify the new run before moving production.
