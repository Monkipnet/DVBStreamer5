#!/usr/bin/env bash
# Publish an already built, versioned portable archive to the configured
# website document root. Run on the website host as its authorized owner.
# No credentials, SSH keys, or webserver configuration are managed here.
set -Eeuo pipefail
umask 022

(( $# == 2 )) || {
  echo 'Usage: bash scripts/publish_portable_release.sh ARCHIVE /absolute/site/public_html' >&2
  exit 2
}
archive="$1"
webroot="$2"
[[ -f "$archive" && -s "$archive" ]] || { echo "Missing archive: $archive" >&2; exit 2; }
[[ "$webroot" == /* && -d "$webroot" && ! -L "$webroot" ]] || {
  echo 'Webroot must be a real existing absolute directory' >&2; exit 2;
}
name="${archive##*/}"
if [[ ! "$name" =~ ^DVBStreamer5-([0-9]+\.[0-9]+\.[0-9]+)-linux-x86_64\.tar\.gz$ ]]; then
  echo 'Archive filename must contain DVBStreamer5 release version' >&2
  exit 2
fi
version="${BASH_REMATCH[1]}"
release_dir="$webroot/downloads/dvbstreamer5"
[[ ! -L "$webroot/downloads" && ! -L "$release_dir" ]] || {
  echo 'Refusing a symlink in release publish path' >&2; exit 2;
}
mkdir -p -- "$release_dir"
tar -tzf "$archive" >/dev/null || { echo 'Archive is not a valid tar.gz' >&2; exit 2; }
root="DVBStreamer5-$version-linux-x86_64"
internal_version="$(tar -xOzf "$archive" "$root/VERSION" 2>/dev/null | tr -d '\r\n')" || {
  echo 'Archive has no release VERSION' >&2; exit 2;
}
[[ "$internal_version" == "$version" ]] || {
  echo "Archive filename/version mismatch: $version / $internal_version" >&2
  exit 2
}
tmp_archive="$release_dir/.upload-$name-$$"
tmp_meta="$release_dir/.latest.json-$$"
cleanup() { rm -f -- "$tmp_archive" "$tmp_meta"; }
trap cleanup EXIT
install -m 0644 -- "$archive" "$tmp_archive"
digest="$(sha256sum "$tmp_archive" | cut -d ' ' -f1)"
size="$(stat -c %s "$tmp_archive")"
mv -T -- "$tmp_archive" "$release_dir/$name"
cat > "$tmp_meta" <<EOF
{
  "version": "$version",
  "url": "https://tvstreammersat5.tech/downloads/dvbstreamer5/$name",
  "sha256": "$digest",
  "size": $size
}
EOF
chmod 0644 "$tmp_meta"
# Only publish metadata after the complete verified archive exists.
mv -T -- "$tmp_meta" "$release_dir/latest.json"
printf 'Published %s: sha256=%s size=%s\n' "$version" "$digest" "$size"
printf 'Metadata: %s\n' "$release_dir/latest.json"
