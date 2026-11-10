#!/usr/bin/env python3
"""DVBStreamer5 signed portable updater. Requires a locally provisioned Ed25519 public key."""
import fcntl, hashlib, json, os, pathlib, re, shutil, subprocess, sys, tarfile, tempfile, urllib.parse, urllib.request
SITE='https://tvstreammersat5.tech'
META=SITE+'/downloads/dvbstreamer5/latest.json'
KEY='/etc/dvbstreamer5/update-public.pem'
HOME=pathlib.Path('/opt/DVBStreamer5')
MAX=1024*1024*1024
def fail(s): raise RuntimeError(s)
def ver(s):
 if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+',s): fail('Invalid version')
 return tuple(map(int,s.split('.')))
def fetch(url,limit):
 p=urllib.parse.urlsplit(url)
 if p.scheme!='https' or p.netloc!='tvstreammersat5.tech': fail('Untrusted update URL')
 op=urllib.request.build_opener(urllib.request.HTTPSHandler())
 with op.open(urllib.request.Request(url,headers={'User-Agent':'DVBStreamer5-Updater'}),timeout=20) as r:
  if urllib.parse.urlsplit(r.geturl()).netloc!='tvstreammersat5.tech': fail('Untrusted redirect')
  b=r.read(limit+1)
  if len(b)>limit: fail('Download limit exceeded')
  return b
def manifest():
 if not os.path.isfile(KEY): fail('Missing trusted key: '+KEY)
 data=fetch(META,8192); sig=fetch(META+'.sig',1024)
 with tempfile.TemporaryDirectory() as d:
  a=pathlib.Path(d)/'m'; b=pathlib.Path(d)/'s'
  a.write_bytes(data); b.write_bytes(sig)
  if subprocess.run(['/usr/bin/openssl','pkeyutl','-verify','-pubin','-inkey',KEY,'-rawin','-in',str(a),'-sigfile',str(b)],capture_output=True,timeout=10).returncode: fail('Invalid release signature')
 m=json.loads(data)
 version=m['version']; digest=m['sha256']; url=m['url']; size=m['size']
 ver(version)
 if not re.fullmatch('[a-fA-F0-9]{64}',digest) or type(size)!=int or not 0<size<=MAX: fail('Invalid release metadata')
 if url!=SITE+'/downloads/dvbstreamer5/DVBStreamer5-'+version+'-linux-x86_64.tar.gz': fail('Untrusted archive name')
 return version,digest.lower(),url,size
def install(current):
 if os.geteuid(): fail('Root privilege is required')
 with open('/run/lock/dvbstreamer5-update.lock','w') as lock:
  try: fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
  except BlockingIOError: fail('Update already running')
  version,digest,url,size=manifest()
  if ver(version)<=ver(current): fail('No newer version')
  with tempfile.TemporaryDirectory(prefix='dvb5-upd-',dir='/var/tmp') as tmp:
   root=pathlib.Path(tmp); archive=root/'release.tar.gz'
   data=fetch(url,MAX)
   if len(data)!=size or hashlib.sha256(data).hexdigest()!=digest: fail('Archive SHA256/size mismatch')
   archive.write_bytes(data)
   with tarfile.open(archive,'r:gz') as tar:
    members=tar.getmembers()
    if not members or len(members)>50000: fail('Invalid archive')
    tops=set()
    for member in members:
     path=pathlib.PurePosixPath(member.name)
     if path.is_absolute() or '..' in path.parts or member.islnk() or member.issym() or member.isdev() or member.isfifo() or member.mode&0o6000 or not (member.isfile() or member.isdir()): fail('Unsafe archive content')
     tops.add(path.parts[0])
    if len(tops)!=1: fail('Invalid archive layout')
    tar.extractall(root/'extract',filter='data')
   bundle=root/'extract'/next(iter(tops))
   if not all((bundle/x).is_file() for x in ('bin/DVBStreamer5','install.sh','run.sh','SHA256SUMS')): fail('Incomplete bundle')
   subprocess.run(['/usr/bin/sha256sum','-c','SHA256SUMS'],cwd=bundle,stdout=subprocess.DEVNULL,timeout=90,check=True)
   backups=pathlib.Path('/var/lib/dvbstreamer5/update-backups'); backups.mkdir(parents=True,exist_ok=True)
   backup=backups/('before-'+version+'-'+current)
   if backup.exists() or not HOME.is_dir(): fail('Backup collision or missing installation')
   shutil.copytree(HOME,backup,symlinks=True)
   try:
    subprocess.run(['/bin/bash',str(bundle/'install.sh')],cwd=bundle,check=True,timeout=300)
    subprocess.run(['/usr/bin/systemctl','is-active','--quiet','dvbstreamer5.service'],timeout=20,check=True)
   except Exception:
    subprocess.run(['/usr/bin/systemctl','stop','dvbstreamer5.service'],timeout=60,check=False)
    if HOME.exists(): shutil.rmtree(HOME)
    shutil.copytree(backup,HOME,symlinks=True)
    subprocess.run(['/usr/bin/systemctl','restart','dvbstreamer5.service'],timeout=60,check=False)
    raise
   print('Installed '+version+'; backup '+str(backup),flush=True)
def main():
 if len(sys.argv)!=3 or sys.argv[1] not in ('check','install'): fail('Usage: updater.py check|install CURRENT_VERSION')
 action,current=sys.argv[1:]
 ver(current)
 if action=='check':
  version,digest,url,size=manifest()
  print(json.dumps({'result':'ok','current':current,'latest':version,'available':ver(version)>ver(current),'size':size}),flush=True)
 else: install(current)
if __name__=='__main__':
 try: main()
 except Exception as ex:
  if len(sys.argv)>1 and sys.argv[1]=='check': print(json.dumps({'result':'error','error':str(ex)[:250]}))
  else: print('UPDATE ERROR: '+str(ex),file=sys.stderr)
  sys.exit(1)
