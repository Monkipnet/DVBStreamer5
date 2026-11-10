#!/usr/bin/env python3
"""DVBStreamer5 portable updater using the project's fixed HTTPS download link.

This temporarily has NO publisher signature authentication. Only use with a trusted
website. Do not accept URLs or executable commands from the HTTP/API client.
"""
import email.message
import fcntl
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.parse
import urllib.request

HOST = 'tvstreammersat5.tech'
SITE = 'https://' + HOST
DOWNLOAD = SITE + '/download.php?id=5c15e7ab4ff1c26daef79d745d211c3d'
META = SITE + '/downloads/dvbstreamer5/latest.json'
INSTALL_DIR = pathlib.Path('/opt/DVBStreamer5')
LIMIT = 1024 * 1024 * 1024
VERSION_RE = re.compile(r'(?<!\d)(\d+\.\d+\.\d+)(?!\d)')

def fail(message):
    raise RuntimeError(message)

def version_tuple(value):
    if not isinstance(value, str) or not re.fullmatch(r'\d+\.\d+\.\d+', value):
        fail('Invalid release version')
    return tuple(int(part) for part in value.split('.'))

class TrustedSiteOnly(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, msg, headers, newurl):
        if not allowed_url(newurl):
            fail('Refused external/HTTP redirect')
        return super().redirect_request(request, fp, code, msg, headers, newurl)

def allowed_url(url):
    p = urllib.parse.urlsplit(url)
    return p.scheme == 'https' and p.hostname == HOST and p.port is None and not p.username and not p.password

OPENER = urllib.request.build_opener(TrustedSiteOnly())

def request(url, method='GET'):
    if not allowed_url(url):
        fail('Untrusted download URL')
    return OPENER.open(urllib.request.Request(url, method=method, headers={
        'User-Agent': 'DVBStreamer5-Updater/1',
        'Accept': 'application/octet-stream, application/json, */*'
    }), timeout=30)

def filename_from_headers(response):
    header = response.headers.get('Content-Disposition', '')
    message = email.message.Message()
    message['Content-Disposition'] = header
    filename = message.get_filename()
    if not filename:
        filename = urllib.parse.unquote(pathlib.PurePosixPath(
            urllib.parse.urlsplit(response.geturl()).path).name)
    return pathlib.PurePosixPath(filename).name

def release_from_filename(filename):
    match = re.fullmatch(r'DVBStreamer5-(\d+\.\d+\.\d+)(?:-[\w.-]+)?\.tar\.gz', filename, re.I)
    return match.group(1) if match else None

def release_metadata():
    # Prefer a small unsigned metadata file if the site publishes one. It must
    # point to exactly the fixed download.php ID, not to a client-controlled URL.
    try:
        with request(META) as response:
            raw = response.read(8193)
        if len(raw) > 8192:
            fail('Release metadata too large')
        data = json.loads(raw)
        version = data['version']
        version_tuple(version)
        url = data['url']
        digest = data.get('sha256', '')
        size = data.get('size', 0)
        if url != DOWNLOAD:
            fail('Manifest must reference the fixed download link')
        if digest and not re.fullmatch(r'[0-9a-fA-F]{64}', digest):
            fail('Malformed archive SHA256')
        if type(size) is not int or size < 0 or size > LIMIT:
            fail('Invalid download size')
        return {'version': version, 'sha256': digest.lower(), 'size': size}
    except urllib.error.HTTPError as error:
        if error.code != 404:
            fail('Release metadata HTTP error: ' + str(error.code))
    # If the website has no manifest, use the archive attachment filename.
    # A generic download.php name cannot provide a trustworthy version number.
    try:
        with request(DOWNLOAD, 'HEAD') as response:
            filename = filename_from_headers(response)
            size_raw = response.headers.get('Content-Length', '')
    except urllib.error.HTTPError as error:
        if error.code not in (405, 501):
            fail('Archive HEAD request failed: ' + str(error.code))
        # Some PHP sites disallow HEAD. Read only headers and close GET immediately.
        with request(DOWNLOAD) as response:
            filename = filename_from_headers(response)
            size_raw = response.headers.get('Content-Length', '')
    version = release_from_filename(filename)
    if not version:
        fail('Release version unavailable: publish latest.json or a versioned attachment filename')
    size = int(size_raw) if size_raw.isdecimal() else 0
    if size > LIMIT:
        fail('Archive exceeds download limit')
    return {'version': version, 'sha256': '', 'size': size}

def check(current):
    meta = release_metadata()
    print(json.dumps({
        'result': 'ok', 'current': current, 'latest': meta['version'],
        'available': version_tuple(meta['version']) > version_tuple(current),
        'size': meta['size'], 'integrity': 'sha256' if meta['sha256'] else 'https-only'
    }), flush=True)

def install(current):
    if os.geteuid() != 0:
        fail('Root privilege is required')
    with open('/run/lock/dvbstreamer5-update.lock', 'w') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            fail('An update is already running')
        meta = release_metadata()
        version = meta['version']
        if version_tuple(version) <= version_tuple(current):
            fail('No newer version available')
        with tempfile.TemporaryDirectory(prefix='dvb5-upd-', dir='/var/tmp') as temp:
            root = pathlib.Path(temp)
            archive = root / 'release.tar.gz'
            digest = hashlib.sha256()
            amount = 0
            with request(DOWNLOAD) as response, archive.open('wb') as output:
                attachment_version = release_from_filename(filename_from_headers(response))
                if attachment_version and attachment_version != version:
                    fail('Archive filename version differs from advertised release')
                while True:
                    block = response.read(1024 * 1024)
                    if not block:
                        break
                    amount += len(block)
                    if amount > LIMIT or (meta['size'] and amount > meta['size']):
                        fail('Archive exceeds allowed size')
                    digest.update(block)
                    output.write(block)
            if meta['size'] and amount != meta['size']:
                fail('Archive size mismatch')
            if meta['sha256'] and digest.hexdigest() != meta['sha256']:
                fail('Archive SHA256 mismatch')
            with tarfile.open(archive, 'r:gz') as tar:
                members = tar.getmembers()
                if not members or len(members) > 50000:
                    fail('Invalid archive contents')
                tops = set()
                total = 0
                for member in members:
                    path = pathlib.PurePosixPath(member.name)
                    if (path.is_absolute() or '..' in path.parts or
                        member.issym() or member.islnk() or member.isdev() or
                        member.isfifo() or member.mode & 0o6000 or
                        not (member.isfile() or member.isdir())):
                        fail('Unsafe archive entry')
                    if member.isfile():
                        total += member.size
                    if total > 3 * LIMIT:
                        fail('Archive expands beyond size limit')
                    tops.add(path.parts[0])
                if len(tops) != 1:
                    fail('Invalid archive layout')
                tar.extractall(root / 'extract', filter='data')
            bundle = root / 'extract' / next(iter(tops))
            required = ('bin/DVBStreamer5', 'run.sh', 'install.sh', 'SHA256SUMS')
            if not all((bundle / item).is_file() for item in required):
                fail('Incomplete DVBStreamer5 portable bundle')
            info = bundle / 'BUNDLE-INFO.txt'
            if info.exists():
                match = re.search(r'^version=(.+)$', info.read_text(errors='replace'), re.M)
                if match and match.group(1).strip() != version:
                    fail('Bundled version differs from release version')
            subprocess.run(['/usr/bin/sha256sum', '-c', 'SHA256SUMS'],
                           cwd=bundle, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, timeout=120, check=True)
            backups = pathlib.Path('/var/lib/dvbstreamer5/update-backups')
            backups.mkdir(parents=True, exist_ok=True, mode=0o700)
            backup = backups / ('before-' + version + '-' + current)
            if backup.exists() or not INSTALL_DIR.is_dir():
                fail('Backup collision or missing installed program')
            shutil.copytree(INSTALL_DIR, backup, symlinks=True)
            try:
                subprocess.run(['/bin/bash', str(bundle / 'install.sh')],
                               cwd=bundle, check=True, timeout=300)
                subprocess.run(['/usr/bin/systemctl', 'is-active', '--quiet',
                                'dvbstreamer5.service'], timeout=20, check=True)
            except Exception:
                subprocess.run(['/usr/bin/systemctl', 'stop', 'dvbstreamer5.service'],
                               timeout=60, check=False)
                if INSTALL_DIR.exists():
                    shutil.rmtree(INSTALL_DIR)
                shutil.copytree(backup, INSTALL_DIR, symlinks=True)
                subprocess.run(['/usr/bin/systemctl', 'restart', 'dvbstreamer5.service'],
                               timeout=60, check=False)
                raise
            print('Installed ' + version + '; runtime backup: ' + str(backup), flush=True)

def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ('check', 'install'):
        fail('Usage: updater.py check|install CURRENT_VERSION')
    action, current = sys.argv[1:]
    version_tuple(current)
    if action == 'check':
        check(current)
    else:
        install(current)

if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        if len(sys.argv) > 1 and sys.argv[1] == 'check':
            print(json.dumps({'result': 'error', 'error': str(error)[:250]}))
        else:
            print('UPDATE ERROR: ' + str(error), file=sys.stderr)
        sys.exit(1)
