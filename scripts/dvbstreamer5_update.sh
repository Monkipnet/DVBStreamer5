#!/usr/bin/env bash
set -Eeuo pipefail
umask 077
URL='https://tvstreammersat5.tech/download.php?id=5c15e7ab4ff1c26daef79d745d211c3d'
META='https://tvstreammersat5.tech/downloads/dvbstreamer5/latest.json'
MAX_BYTES=1073741824
die(){ echo "UPDATE ERROR: $*" >&2; exit 1; }
valid_version(){ [[ "$1" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; }
newer(){
  local a b c x y z
  IFS=. read -r a b c <<< "$1"
  IFS=. read -r x y z <<< "$2"
  (( 10#$a > 10#$x || (10#$a == 10#$x && 10#$b > 10#$y) || (10#$a == 10#$x && 10#$b == 10#$y && 10#$c > 10#$z) ))
}
read_release(){
  local tmp="$1" manifest="$1/manifest.json" header="$1/header.txt" link
  RELEASE_SHA=''; RELEASE_SIZE=0
  if curl -fsSL --proto '=https' --proto-redir '=https' --max-redirs 3 --max-time 20 --max-filesize 8192 "$META" -o "$manifest"; then
    command -v jq >/dev/null || die 'jq is required to parse release metadata'
    RELEASE_VERSION="$(jq -er '.version | strings' "$manifest")" || die 'Missing release version'
    link="$(jq -er '.url | strings' "$manifest")" || die 'Missing release URL'
    [[ "$link" == "$URL" ]] || die 'Unexpected release URL'
    RELEASE_SHA="$(jq -er '.sha256 // "" | strings' "$manifest")"
    RELEASE_SIZE="$(jq -er '.size // 0 | numbers' "$manifest")"
    [[ -z "$RELEASE_SHA" || "$RELEASE_SHA" =~ ^[0-9a-fA-F]{64}$ ]] || die 'Bad SHA256'
    [[ "$RELEASE_SIZE" =~ ^[0-9]+$ ]] || die 'Bad archive size'
    (( RELEASE_SIZE <= MAX_BYTES )) || die 'Archive too large'
  else
    curl -fsSLI --proto '=https' --proto-redir '=https' --max-redirs 3 --max-time 20 -D "$header" "$URL" -o /dev/null || die 'Cannot inspect website download'
    local name
    name="$(tr -d '\r' < "$header" | sed -nE 's/^[Cc]ontent-[Dd]isposition:.*filename="?([^";]+)"?.*/\1/p' | tail -1)"
    [[ "$name" =~ ^DVBStreamer5-([0-9]+\.[0-9]+\.[0-9]+)(-linux-x86_64)?\.tar\.gz$ ]] || die 'No versioned filename; publish latest.json'
    RELEASE_VERSION="${BASH_REMATCH[1]}"
  fi
  valid_version "$RELEASE_VERSION" || die 'Bad release version'
}
download_archive(){
  local out="$1" details status effective size
  details="$(curl -sSL --show-error --proto '=https' --proto-redir '=https' --max-redirs 3 --connect-timeout 10 --max-time 600 --max-filesize "$MAX_BYTES" -o "$out" -w '%{http_code} %{url_effective}' "$URL")" || die 'Archive download failed'
  read -r status effective <<< "$details"
  [[ "$status" == 200 && "$effective" == https://tvstreammersat5.tech/* ]] || die 'Untrusted download redirect'
  size="$(stat -c %s "$out")"
  (( size > 0 && size <= MAX_BYTES )) || die 'Invalid archive size'
  if (( RELEASE_SIZE > 0 )); then (( size == RELEASE_SIZE )) || die 'Archive size mismatch'; fi
  if [[ -n "$RELEASE_SHA" ]]; then
    [[ "$(sha256sum "$out" | cut -d ' ' -f1)" == "${RELEASE_SHA,,}" ]] || die 'Archive SHA256 mismatch'
  fi
}
check(){
  local tmp available=false
  tmp="$(mktemp -d)"
  trap 'rm -rf -- "$tmp"' EXIT
  read_release "$tmp"
  if newer "$RELEASE_VERSION" "$2"; then available=true; fi
  printf '{"result":"ok","current":"%s","latest":"%s","available":%s}\n' "$2" "$RELEASE_VERSION" "$available"
}
install_release(){
  (( EUID == 0 )) || die 'Installation requires root'
  exec 9>/run/lock/dvbstreamer5-update.lock
  flock -n 9 || die 'Update already running'
  local tmp archive root bundle backup name line
  tmp="$(mktemp -d -p /var/tmp dvb5-update.XXXXXXXX)"
  trap 'rm -rf -- "$tmp"' EXIT
  read_release "$tmp"
  newer "$RELEASE_VERSION" "$2" || die 'No newer release'
  archive="$tmp/update.tar.gz"
  download_archive "$archive"
  root="DVBStreamer5-$RELEASE_VERSION-linux-x86_64"
  tar -tzf "$archive" > "$tmp/names"
  [[ -s "$tmp/names" ]] || die 'Empty archive'
  while IFS= read -r name; do
    [[ "$name" == "$root" || "$name" == "$root/"* ]] || die 'Archive contains unexpected root'
    [[ "$name" != *'..'* && "$name" != *$'\n'* ]] || die 'Unsafe archive path'
  done < "$tmp/names"
  tar -tvzf "$archive" > "$tmp/types"
  while IFS= read -r line; do
    case "${line:0:1}" in -|d) ;; *) die 'Unsafe archive member type';; esac
  done < "$tmp/types"
  mkdir "$tmp/unpack"
  tar -xzf "$archive" -C "$tmp/unpack" --no-same-owner --no-same-permissions
  bundle="$tmp/unpack/$root"
  for file in bin/DVBStreamer5 run.sh install.sh SHA256SUMS; do
    [[ -f "$bundle/$file" ]] || die "Missing $file"
  done
  (cd "$bundle" && sha256sum -c SHA256SUMS >/dev/null) || die 'Bundle checksums failed'
  mkdir -p -m 0700 /var/lib/dvbstreamer5/update-backups
  backup="/var/lib/dvbstreamer5/update-backups/before-$RELEASE_VERSION-from-$2"
  [[ -d /opt/DVBStreamer5 && ! -e "$backup" ]] || die 'Missing runtime or backup collision'
  cp -a /opt/DVBStreamer5 "$backup"
  echo "UPDATE: installing $RELEASE_VERSION; backup=$backup"
  if ! (cd "$bundle" && /bin/bash ./install.sh) || ! systemctl is-active --quiet dvbstreamer5.service; then
    systemctl stop dvbstreamer5.service || true
    rm -rf -- /opt/DVBStreamer5
    cp -a "$backup" /opt/DVBStreamer5
    systemctl restart dvbstreamer5.service || true
    die 'Installation failed; rolled back runtime'
  fi
  echo "UPDATE: completed $RELEASE_VERSION"
}
(( $# == 2 )) || die 'Usage: dvbstreamer5_update.sh check|install VERSION'
valid_version "$2" || die 'Invalid current version'
for command in curl tar sha256sum flock mktemp stat sed; do
  command -v "$command" >/dev/null || die "Missing dependency: $command"
done
case "$1" in check) check "$@" ;; install) install_release "$@" ;; *) die 'Unknown action';; esac
