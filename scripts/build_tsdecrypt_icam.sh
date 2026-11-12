#!/usr/bin/env bash
# Experimental ECM-aware TSDecrypt binary; isolated build, no production changes.
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
SRC_INPUT="${1:-/tmp/dvbstreamer5-tsdecrypt}"
BUILD_INPUT="${2:-$ROOT/build}"
SRC="$(cd -- "$SRC_INPUT" && pwd -P)"
mkdir -p -- "$BUILD_INPUT"
BUILD="$(cd -- "$BUILD_INPUT" && pwd -P)"
INC="$ROOT/third_party/libdvbcsa/src"
LIB="$BUILD/third_party/libdvbcsa/libdvbstreamer5_dvbcsa.a"
OUT="$BUILD/tsdecrypt-icam/tsdecrypt"

for cmd in cmake make cc python3 nm readelf; do
    command -v "$cmd" >/dev/null || { echo "Missing command: $cmd" >&2; exit 2; }
done
# Upstream TSDecrypt still records retired georgi.unixsol.org submodule URLs.
# Restore its exact pinned commits from the author's GitHub mirrors without
# editing the user's .gitmodules, switching to unpinned heads, or modifying
# installed TSDecrypt on test/production hosts.
if [[ ! -f "$SRC/libfuncs/libfuncs.h" || ! -f "$SRC/libtsfuncs/tsfuncs.h" ]]; then
    command -v git >/dev/null || { echo "Missing git for pinned TSDecrypt submodules" >&2; exit 2; }
    git -C "$SRC" rev-parse --is-inside-work-tree >/dev/null || {
        echo "TSDecrypt submodules are missing and source is not a Git checkout" >&2; exit 2;
    }
    echo "Restoring pinned TSDecrypt submodules from gfto GitHub mirrors"
    git -C "$SRC" \
      -c submodule.libfuncs.url=https://github.com/gfto/libfuncs.git \
      -c submodule.libtsfuncs.url=https://github.com/gfto/libtsfuncs.git \
      submodule update --init --recursive || {
        echo "Cannot restore pinned TSDecrypt submodule commits" >&2; exit 2;
    }
fi
[[ -f "$SRC/libfuncs/libfuncs.h" && -f "$SRC/libtsfuncs/tsfuncs.h" ]] || {
    echo "Pinned TSDecrypt libfuncs/libtsfuncs headers are still missing" >&2; exit 2;
}

for name in Makefile csa.c csa.h camd.c data.h; do
    [[ -f "$SRC/$name" ]] || { echo "Missing source: $SRC/$name" >&2; exit 2; }
done
[[ -f "$INC/dvbcsa/dvbcsa.h" ]] || { echo "Missing vendored dvbcsa header" >&2; exit 2; }
grep -q 'dvbcsa_bs_key_set_ecm' "$INC/dvbcsa/dvbcsa.h" || {
    echo "Missing ECM-aware dvbcsa header API" >&2; exit 2;
}
cmake --build "$BUILD" --target dvbstreamer5_dvbcsa -j"$(nproc)"
[[ -f "$LIB" ]] || { echo "Missing vendored library: $LIB" >&2; exit 2; }
nm -g --defined-only "$LIB" | grep ' dvbcsa_bs_key_set_ecm$' >/dev/null || {
    echo "Missing ECM-aware dvbcsa library implementation" >&2; exit 2;
}

WORK="$(mktemp -d "${TMPDIR:-/tmp}/dvbstreamer5-tsdecrypt-XXXXXX")"
trap 'rm -rf -- "$WORK"' EXIT
cp -a "$SRC"/. "$WORK"/

python3 - "$WORK" "$ROOT/src/ca/backends/tsdecrypt/TsDecryptEcmPidPolicy.h" <<'PY'
from pathlib import Path
import shutil
import sys
root = Path(sys.argv[1])
shutil.copyfile(sys.argv[2], root / 'dvb5_ecm_pid_policy.h')

def change(name, changes):
    p = root / name
    source = p.read_text()
    for old, new in changes:
        count = source.count(old)
        if count != 1:
            raise SystemExit(f"Unexpected upstream version: {name}: match count {count} for {old[:55]!r}")
        source = source.replace(old, new, 1)
    p.write_text(source)

change('data.h', [(
    'struct camd {\n',
    'struct camd {\n\tuint8_t ecm_mode; /* Mode of the current ECM request. */\n',
)])
change('csa.h', [
    ('struct csakey {\n',
     'struct csakey {\n\tuint8_t ecm_mode[2]; /* Key parity-specific mode. */\n'),
    ('void\t\t\tcsa_set_even_cw\t\t(csakey_t *csakey, uint8_t *even_cw);',
     'void\t\t\tcsa_set_even_cw\t\t(csakey_t *csakey, uint8_t *even_cw, uint8_t mode);'),
    ('void\t\t\tcsa_set_odd_cw\t\t(csakey_t *csakey, uint8_t *odd_cw);',
     'void\t\t\tcsa_set_odd_cw\t\t(csakey_t *csakey, uint8_t *odd_cw, uint8_t mode);'),
])
change('csa.c', [
    ('inline void csa_set_even_cw(csakey_t *csakey, uint8_t *even_cw) {\n'
     '\tstruct csakey *key = (struct csakey *)csakey;\n'
     '\tdvbcsa_key_set(even_cw, key->s_csakey[0]);\n'
     '\tdvbcsa_bs_key_set(even_cw, key->bs_csakey[0]);',
     'inline void csa_set_even_cw(csakey_t *csakey, uint8_t *even_cw, uint8_t mode) {\n'
     '\tstruct csakey *key = (struct csakey *)csakey;\n'
     '\tkey->ecm_mode[0] = mode;\n'
     '\tif (mode) {\n'
     '\t\tdvbcsa_key_set_ecm(mode, even_cw, key->s_csakey[0]);\n'
     '\t\tdvbcsa_bs_key_set_ecm(mode, even_cw, key->bs_csakey[0]);\n'
     '\t} else {\n'
     '\t\tdvbcsa_key_set(even_cw, key->s_csakey[0]);\n'
     '\t\tdvbcsa_bs_key_set(even_cw, key->bs_csakey[0]);\n'
     '\t}'),
    ('inline void csa_set_odd_cw(csakey_t *csakey, uint8_t *odd_cw) {\n'
     '\tstruct csakey *key = (struct csakey *)csakey;\n'
     '\tdvbcsa_key_set(odd_cw, key->s_csakey[1]);\n'
     '\tdvbcsa_bs_key_set(odd_cw, key->bs_csakey[1]);',
     'inline void csa_set_odd_cw(csakey_t *csakey, uint8_t *odd_cw, uint8_t mode) {\n'
     '\tstruct csakey *key = (struct csakey *)csakey;\n'
     '\tkey->ecm_mode[1] = mode;\n'
     '\tif (mode) {\n'
     '\t\tdvbcsa_key_set_ecm(mode, odd_cw, key->s_csakey[1]);\n'
     '\t\tdvbcsa_bs_key_set_ecm(mode, odd_cw, key->bs_csakey[1]);\n'
     '\t} else {\n'
     '\t\tdvbcsa_key_set(odd_cw, key->s_csakey[1]);\n'
     '\t\tdvbcsa_bs_key_set(odd_cw, key->bs_csakey[1]);\n'
     '\t}'),
    ('\t\tdvbcsa_decrypt(key->s_csakey[key_idx], ts_packet + payload_offset, 188 - payload_offset);',
     '\t\tif (payload_offset < 188) {\n'
     '\t\t\tif (key->ecm_mode[key_idx]) {\n'
     '\t\t\t\tstruct dvbcsa_bs_batch_s batch[2] = {\n'
     '\t\t\t\t\t{ ts_packet + payload_offset, 188 - payload_offset }, { NULL, 0 }\n'
     '\t\t\t\t};\n'
     '\t\t\t\tdvbcsa_bs_decrypt(key->bs_csakey[key_idx], batch, 184);\n'
     '\t\t\t} else {\n'
     '\t\t\t\tdvbcsa_decrypt(key->s_csakey[key_idx], ts_packet + payload_offset, 188 - payload_offset);\n'
     '\t\t\t}\n'
     '\t\t}'),
])
mode_fn = '''/* Mode selection matches DVBStreamer5's guarded VideoGuard ECM logic. */
static uint8_t icam_mode_for_ecm(const struct camd_msg *msg) {
    if (!msg || (msg->ca_id & 0xff00u) != 0x0900u || msg->data_len <= 0x15)
        return 0;
    if (msg->data[0] != 0x80 && msg->data[0] != 0x81)
        return 0;
    const int marker = msg->data[4];
    if (marker == 0 || ((int)msg->data[2] - marker) != 4)
        return 0;
    return msg->data[0x15];
}

'''
change('camd.c', [
    ('void camd_set_cw(struct ts *ts, uint8_t *new_cw, int check_validity) {',
     mode_fn + 'void camd_set_cw(struct ts *ts, uint8_t *new_cw, int check_validity) {'),
    ('csa_set_even_cw(c->key->csakey, new_cw);',
     'csa_set_even_cw(c->key->csakey, new_cw, c->ecm_mode);'),
    ('csa_set_odd_cw(c->key->csakey, new_cw + 8);',
     'csa_set_odd_cw(c->key->csakey, new_cw + 8, c->ecm_mode);'),
    ('static int camd_send_ecm(struct ts *ts, struct camd_msg *msg) {\n\tstruct camd *c = &ts->camd;\n',
     'static int camd_send_ecm(struct ts *ts, struct camd_msg *msg) {\n'
     '\tstruct camd *c = &ts->camd;\n'
     '\t/* ECM request and corresponding CW response are sequential. */\n'
     '\tc->ecm_mode = icam_mode_for_ecm(msg);\n'),
])
# V471: keep a recently proven ECM PID after transient negative CW
# replies instead of immediately bouncing between every PMT candidate.
change('data.h', [
    ('\tuint16_t\t\t\tecm_pids[MAX_ECM_PIDS];',
     '\tuint16_t\t\t\tecm_pids[MAX_ECM_PIDS];\n'
     '\tuint16_t\t\t\tlast_successful_ecm_pid;\n'
     '\tunsigned int\t\tecm_pid_consecutive_denials;\n'
     '\ttime_t\t\t\tlast_ecm_pid_rotation_time;'),
])
change('camd.c', [
    ('#include "camd.h"\n',
     '#include "camd.h"\n#include "dvb5_ecm_pid_policy.h"\n'),
    ('\t\t// get_cw returned error, lets try other ecm pids, we might be lucky...\n'
     '\t\tif (ts->n_ecm_pids > 1) {',
     '\t\tif (ret == 0) ++ts->ecm_pid_consecutive_denials;\n'
     '\t\telse ts->ecm_pid_consecutive_denials = 0;\n'
     '\t\tconst int should_rotate = dvb5_should_rotate_ecm_pid(\n'
     '\t\t\tts->forced_ecm_pid, ts->n_ecm_pids, ret,\n'
     '\t\t\tts->ecm_pid_consecutive_denials, ts->ecm_pid,\n'
     '\t\t\tts->last_successful_ecm_pid, c->key->is_valid_cw,\n'
     '\t\t\tc->key->ts, ts->last_ecm_pid_rotation_time, time(NULL));\n'
     '\t\tif (should_rotate) {\n'
     '\t\t\tts->last_ecm_pid_rotation_time = time(NULL);\n'
     '\t\t\tts->ecm_pid_consecutive_denials = 0;'),
    ('\t\t\tts_LOGf("ECM | Switching ECM pid to 0x%04x (%d) idx:%d\\n", ts->ecm_pid, ts->ecm_pid, ts->ecm_pid_idx);\n'
     '\t\t}\n'
     '\t\tif (ret == -1)',
     '\t\t\tts_LOGf("ECM | Switching ECM pid to 0x%04x (%d) idx:%d\\n", ts->ecm_pid, ts->ecm_pid, ts->ecm_pid_idx);\n'
     '\t\t} else if (ret == 0 && ts->n_ecm_pids > 1 && !ts->forced_ecm_pid) {\n'
     '\t\t\tts_LOGf("ECM | Holding recently valid ECM pid after transient card denial.\\n");\n'
     '\t\t}\n'
     '\t\tif (ret == -1)'),
    ('\tc->key->is_valid_cw = valid_cw;\n',
     '\tc->key->is_valid_cw = valid_cw;\n'
     '\tif (valid_cw) {\n'
     '\t\tts->last_successful_ecm_pid = ts->ecm_pid;\n'
     '\t\tts->ecm_pid_consecutive_denials = 0;\n'
     '\t}\n'),
])
print('Patched isolated TSDecrypt: ECM-aware CSA + sticky ECM PID policy')
PY

mkdir -p "$WORK/.vendor-lib" "$(dirname -- "$OUT")"
ln -s "$LIB" "$WORK/.vendor-lib/libdvbcsa.a"
make -C "$WORK" dvbcsa -j"$(nproc)" \
    CFLAGS="-O2 -g -pipe -fno-strict-aliasing -I$INC" \
    LDFLAGS="-Wl,--gc-sections -L$WORK/.vendor-lib"
[[ -x "$WORK/tsdecrypt" ]] || { echo "No executable produced" >&2; exit 3; }
nm -a "$WORK/tsdecrypt" | grep ' [Tt] dvbcsa_bs_key_set_ecm$' >/dev/null || {
    echo 'Built TSDecrypt lacks ECM-aware dvbcsa implementation' >&2; exit 3;
}
if readelf -d "$WORK/tsdecrypt" | grep 'libdvbcsa.so' >/dev/null; then
    echo "Dynamic libdvbcsa is not allowed in this portable build" >&2; exit 3;
fi
install -m0755 "$WORK/tsdecrypt" "$OUT"
echo "Experimental TSDecrypt compiled: $OUT"
sha256sum "$OUT"
echo "Do not replace a running production TSDecrypt before one-tile A/B validation."
