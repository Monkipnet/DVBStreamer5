#!/usr/bin/env bash
set -Eeuo pipefail
umask 077
URL='https://tvstreammersat5.tech/download.php?id=5c15e7ab4ff1c26daef79d745d211c3d'
META='https://tvstreammersat5.tech/downloads/dvbstreamer5/latest.json'
MAX_BYTES=1073741824
UPDATE_STATUS='/var/lib/dvbstreamer5/update-status'
update_phase(){
  # Fixed, non-user-controlled phase strings; readable by the web status API.
  case "$1" in downloading|verifying|installing|restarting|done|error) ;;
    *) return 1 ;;
  esac
  printf '%s\n' "$1" > "$UPDATE_STATUS"
}
UPDATE_TXN_TMP=''
UPDATE_TXN_BACKUP=''
UPDATE_TXN_DIRTY=0
update_transaction_exit(){
  local exit_status=$?
  trap - EXIT ERR
  if (( exit_status != 0 )) && [[ "${UPDATE_INSTALL_ACTIVE:-0}" == 1 ]]; then
    update_phase error || true
    if [[ "$UPDATE_TXN_DIRTY" == 1 && -d "$UPDATE_TXN_BACKUP" ]]; then
      echo 'UPDATE: installation failed, restoring previous runtime and units' >&2
      systemctl stop dvbstreamer5.service || true
      systemctl stop oscam-mini.service || true
      local restored="/opt/DVBStreamer5.rollback-$" unit
      if [[ ! -e "$restored" ]] && cp -a -- "$UPDATE_TXN_BACKUP" "$restored"; then
        rm -rf -- /opt/DVBStreamer5
        if mv -T -- "$restored" /opt/DVBStreamer5; then
          for unit in dvbstreamer5.service oscam-mini.service; do
            if [[ -e "$UPDATE_TXN_TMP/units/$unit" ]]; then
              cp -a -- "$UPDATE_TXN_TMP/units/$unit" "/etc/systemd/system/$unit" || true
            else
              rm -f -- "/etc/systemd/system/$unit" || true
            fi
          done
          systemctl daemon-reload || true
          if [[ -x /opt/DVBStreamer5/oscam-mini/oscam-mini ]]; then
            systemctl start oscam-mini.service || true
          fi
          systemctl start dvbstreamer5.service || true
          if systemctl is-active --quiet dvbstreamer5.service; then
            echo 'UPDATE: rollback restored DVBStreamer5 service' >&2
          else
            echo 'UPDATE ERROR: rollback restored files but service failed to start' >&2
          fi
        else
          echo 'UPDATE ERROR: rollback rename failed' >&2
        fi
      else
        echo 'UPDATE ERROR: could not copy backup, current files kept' >&2
      fi
    fi
  fi
  if [[ -n "$UPDATE_TXN_TMP" ]]; then rm -rf -- "$UPDATE_TXN_TMP" || true; fi
  exit "$exit_status"
}
die(){
  if [[ "${UPDATE_INSTALL_ACTIVE:-0}" == 1 ]]; then update_phase error || true; fi
  echo "UPDATE ERROR: $*" >&2; exit 1
}
valid_version(){ [[ "$1" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; }
newer(){
  local a b c x y z
  IFS=. read -r a b c <<< "$1"
  IFS=. read -r x y z <<< "$2"
  (( 10#$a > 10#$x || (10#$a == 10#$x && 10#$b > 10#$y) || (10#$a == 10#$x && 10#$b == 10#$y && 10#$c > 10#$z) ))
}
site_header_version(){
  local page="$1" header="$2" versions
  curl -fsSL --proto '=https' --proto-redir '=https' --max-redirs 3 \
    --connect-timeout 10 --max-time 20 --max-filesize 262144 \
    'https://tvstreammersat5.tech/' -o "$page" 2>/dev/null || return 1
  # Only inspect the actual HTML header, never scripts or arbitrary body links.
  sed -n '/<[Hh][Ee][Aa][Dd][Ee][Rr][ >]/,/<\/[Hh][Ee][Aa][Dd][Ee][Rr]>/p' "$page" > "$header"
  [[ -s "$header" ]] || return 1
  # Remove tags while preserving text such as "DVBStreamer5 V10.8.437".
  versions="$(sed -E 's/<[^>]*>/ /g' "$header" |
    grep -Eo '[Vv]?[0-9]+\.[0-9]+\.[0-9]+' |
    sed -E 's/^[Vv]//' | sort -u)" || return 1
  [[ "$(printf '%s\n' "$versions" | grep -c .)" == 1 ]] || return 1
  printf '%s\n' "$versions"
}
# Read release version without executing the downloaded program or installer.
# Older portable bundles use a generic root folder and embed the version in ELF.
# Support generic archive folder names without executing untrusted binaries.
determine_archive_release(){
 local archive="$1" entries="$2" entry root='' part candidate binary='' version='' versions
 [[ -s "$entries" ]] || die 'Empty archive'
 while IFS= read -r entry; do
  [[ "$entry" != /* && "$entry" != *'..'* && "$entry" != *$'\n'* ]] || die 'Unsafe path'
  part="${entry%%/*}"
  [[ -n "$part" && "$part" != '.' ]] || die 'Invalid archive root'
  [[ -z "$root" || "$part" == "$root" ]] || die 'Multiple archive roots'
  root="$part"
 done < "$entries"
 ARCHIVE_ROOT="$root"
 if [[ "$root" =~ ^DVBStreamer5-([0-9]+\.[0-9]+\.[0-9]+)-linux-x86_64$ ]]; then
  RELEASE_VERSION="${BASH_REMATCH[1]}"
  return 0
 fi
 for candidate in "$root/VERSION" "$root/version.txt"; do
  if grep -Fxq "$candidate" "$entries"; then
   version="$(tar -xOzf "$archive" "$candidate" | tr -d '\r\n')"
   if valid_version "$version"; then RELEASE_VERSION="$version"; return 0; fi
  fi
 done
 for candidate in "$root/bin/DVBStreamer5" "$root/DVBStreamer5"; do
  if grep -Fxq "$candidate" "$entries"; then binary="$candidate"; break; fi
 done
 [[ -n "$binary" ]] || die 'No DVBStreamer5 executable'
 versions="$(tar -xOzf "$archive" "$binary" | strings -a | grep -E '^10[.]8[.][0-9]+$' | sort -u)" || true
 [[ "$(printf '%s\n' "$versions" | grep -c .)" == 1 ]] || die 'No unique embedded version'
 RELEASE_VERSION="$versions"
}
read_release(){
  local tmp="$1" manifest="$1/manifest.json" header="$1/header.txt" link
  RELEASE_SHA=''; RELEASE_SIZE=0; ARCHIVE_ROOT=''
  if curl -fsSL --proto '=https' --proto-redir '=https' --max-redirs 3 --max-time 20 --max-filesize 8192 "$META" -o "$manifest" 2>/dev/null; then
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
    # The homepage header advertises the current program release.
    # This is a quick check: no full tar.gz download is needed.
    local site_version
    if site_version="$(site_header_version "$tmp/site.html" "$tmp/site-header.html")" &&
       valid_version "$site_version"; then
      RELEASE_VERSION="$site_version"
      return 0
    fi
    # PHP download endpoints may answer HEAD with 404 while accepting GET.
    # HEAD failure must never abort the normal archive-download fallback.
    local name='' archive="$tmp/release.tar.gz"
    if curl -fsSLI --proto '=https' --proto-redir '=https' --max-redirs 3 \
        --max-time 20 -D "$header" "$URL" -o /dev/null 2>/dev/null; then
      name="$(tr -d '\r' < "$header" | sed -nE 's/^[Cc]ontent-[Dd]isposition:.*filename="?([^";]+)"?.*/\1/p' | tail -1)"
    fi
    if [[ "$name" =~ ^DVBStreamer5-([0-9]+\.[0-9]+\.[0-9]+)(-linux-x86_64)?\.tar\.gz$ ]]; then
      RELEASE_VERSION="${BASH_REMATCH[1]}"
    else
      # GET is authoritative for PHP attachment downloads; validate the
      # archive and infer the version from VERSION, directory or ELF.
      download_archive "$archive"
      tar -tzf "$archive" > "$tmp/entries" || die 'Downloaded file is not a valid tar.gz'
      [[ -s "$tmp/entries" ]] || die 'Downloaded archive is empty'
      determine_archive_release "$archive" "$tmp/entries"
      # install_release reuses the already-downloaded archive.
    fi
  fi
  valid_version "$RELEASE_VERSION" || die 'Bad release version'
}
download_archive(){
  local out="$1" details status effective size
  details="$(curl -sSL --show-error --proto '=https' --proto-redir '=https' --max-redirs 3 --connect-timeout 10 --max-time 600 --max-filesize "$MAX_BYTES" -o "$out" -w '%{http_code} %{url_effective}' "$URL")" || die 'Archive download failed'
  read -r status effective <<< "$details"
  if [[ "$status" == 404 ]]; then
    die "Archive not found (HTTP 404): verify the download.php file ID on tvstreammersat5.tech"
  fi
  [[ "$status" == 200 ]] || die "Download HTTP $status (final URL: $effective)"
  # download.php may point to an external HTTPS CDN. The source URL is fixed,
  # and curl permits only HTTPS redirect targets with verified TLS.
  # Never follow userinfo URLs or localhost/private-address redirects.
  local authority host
  [[ "$effective" =~ ^https://([^/?#]+)(/|[?]|$) ]] ||
    die "Unsafe non-HTTPS download redirect: $effective"
  authority="${BASH_REMATCH[1]}"
  [[ "$authority" != *@* ]] || die "Unsafe credential-bearing redirect: $effective"
  host="${authority%%:*}"
  host="${host,,}"
  case "$host" in
    localhost|localhost.*|*.localhost|127.*|10.*|192.168.*|169.254.*|0.*|::1|[[]::1[]]|*.local|*.internal)
      die "Refused local/private archive redirect: $effective" ;;
  esac
  if [[ "$host" =~ ^172[.](1[6-9]|2[0-9]|3[01])[.] ]] ||
     [[ "$host" =~ ^[[]?(fc|fd|fe80): ]]; then
    die "Refused private archive redirect: $effective"
  fi
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
  # Expand the temporary directory now: local variables are gone at shell EXIT.
  trap "rm -rf -- '$tmp'" EXIT
  read_release "$tmp"
  if newer "$RELEASE_VERSION" "$2"; then available=true; fi
  printf '{"result":"ok","current":"%s","latest":"%s","available":%s}\n' "$2" "$RELEASE_VERSION" "$available"
}
install_release(){
  (( EUID == 0 )) || die 'Installation requires root'
  exec 9>/run/lock/dvbstreamer5-update.lock
  flock -n 9 || die 'Update already running'
  local tmp archive root bundle backup name line expected_version
  tmp="$(mktemp -d -p /var/tmp dvb5-update.XXXXXXXX)"
  UPDATE_TXN_TMP="$tmp"
  trap 'update_transaction_exit' EXIT
  mkdir -p -m 0755 /var/lib/dvbstreamer5
  # Preserve a visible error state even on explicit exit calls.
  UPDATE_INSTALL_ACTIVE=1
  update_phase downloading
  read_release "$tmp"
  newer "$RELEASE_VERSION" "$2" || die 'No newer release'
  expected_version="$RELEASE_VERSION"
  archive="$tmp/release.tar.gz"
  if [[ ! -f "$archive" ]]; then download_archive "$archive"; fi
  tar -tzf "$archive" > "$tmp/names"
  update_phase verifying
  determine_archive_release "$archive" "$tmp/names"
  root="$ARCHIVE_ROOT"
  [[ "$RELEASE_VERSION" == "$expected_version" ]] || die 'Downloaded archive version mismatch'
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
  mkdir -p -m 0700 "$tmp/units"
  local unit
  for unit in dvbstreamer5.service oscam-mini.service; do
    if [[ -e "/etc/systemd/system/$unit" ]]; then
      cp -a -- "/etc/systemd/system/$unit" "$tmp/units/$unit"
    fi
  done
  UPDATE_TXN_BACKUP="$backup"
  update_phase installing
  echo "UPDATE: installing $RELEASE_VERSION; backup=$backup"
  UPDATE_TXN_DIRTY=1
  # EXIT trap restores the runtime and units if any operation fails.
  if ! (cd "$bundle" && /bin/bash ./install.sh) ||
     ! systemctl is-active --quiet dvbstreamer5.service; then
    die 'Installation failed; rollback requested'
  fi
  if [[ -x /opt/DVBStreamer5/oscam-mini/oscam-mini ]] &&
     ! systemctl is-active --quiet oscam-mini.service; then
    die 'OSCam-mini failed to start; rollback requested'
  fi
  [[ "$(cat /opt/DVBStreamer5/VERSION 2>/dev/null)" == "$RELEASE_VERSION" ]] ||
    die 'Runtime version verification failed; rollback requested'
  update_phase done
  UPDATE_TXN_DIRTY=0
  UPDATE_INSTALL_ACTIVE=0
  echo "UPDATE: completed $RELEASE_VERSION"
}
(( $# == 2 )) || die 'Usage: dvbstreamer5_update.sh check|install VERSION'
valid_version "$2" || die 'Invalid current version'
for command in curl tar strings sha256sum flock mktemp stat sed; do
  command -v "$command" >/dev/null || die "Missing dependency: $command"
done
case "$1" in check) check "$@" ;; install) install_release "$@" ;; *) die 'Unknown action';; esac
