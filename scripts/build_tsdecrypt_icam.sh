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

python3 - "$WORK" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])

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
print('Patched isolated source: ECM-aware key scheduling + bitslice iCAM')
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
