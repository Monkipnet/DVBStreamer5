'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const os=require('node:os');
const path=require('node:path');
const {spawnSync}=require('node:child_process');
const script='scripts/publish_portable_release.sh';
const src=fs.readFileSync(script,'utf8');
for(const marker of ['tar -tzf "$archive"', 'internal_version=', 'sha256sum "$tmp_archive"',
                     'mv -T -- "$tmp_archive"', 'mv -T -- "$tmp_meta"',
                     '"sha256": "$digest"', '"version": "$version"', '"size": $size']){
  assert.ok(src.includes(marker),'Missing safe release publication: '+marker);
}
const root=fs.mkdtempSync(path.join(os.tmpdir(),'dvb5-publish-v599-'));
try {
  const ver='10.8.599';
  const base='DVBStreamer5-'+ver+'-linux-x86_64';
  const pkg=path.join(root,'fixture',base);
  fs.mkdirSync(pkg,{recursive:true});
  fs.writeFileSync(path.join(pkg,'VERSION'),ver+'\n');
  fs.writeFileSync(path.join(pkg,'SHA256SUMS'),'');
  const archive=path.join(root,base+'.tar.gz');
  let r=spawnSync('tar',['-czf',archive,'-C',path.join(root,'fixture'),base],{encoding:'utf8'});
  assert.equal(r.status,0,r.stderr);
  const webroot=path.join(root,'website');
  fs.mkdirSync(webroot);
  r=spawnSync('bash',[script,archive,webroot],{encoding:'utf8',timeout:12000});
  assert.equal(r.status,0,'Publish failed: '+r.stderr);
  const published=path.join(webroot,'downloads','dvbstreamer5');
  const metadata=JSON.parse(fs.readFileSync(path.join(published,'latest.json'),'utf8'));
  assert.equal(metadata.version,ver);
  assert.equal(metadata.url,'https://tvstreammersat5.tech/downloads/dvbstreamer5/'+base+'.tar.gz');
  assert.equal(metadata.size,fs.statSync(archive).size);
  const sha=spawnSync('sha256sum',[archive],{encoding:'utf8'});
  assert.equal(metadata.sha256,sha.stdout.slice(0,64));
  assert.ok(fs.statSync(path.join(published,base+'.tar.gz')).size>0);
  // Never publish mismatched archive and metadata.
  const mismatch=path.join(root,'DVBStreamer5-10.8.600-linux-x86_64.tar.gz');
  fs.copyFileSync(archive,mismatch);
  r=spawnSync('bash',[script,mismatch,webroot],{encoding:'utf8',timeout:12000});
  assert.notEqual(r.status,0,'Must reject mismatched release version');
  assert.equal(JSON.parse(fs.readFileSync(path.join(published,'latest.json'),'utf8')).version,ver);
}finally{fs.rmSync(root,{recursive:true,force:true});}
console.log('PASS V599: validated atomic website publication, digest, version and rollback-safe metadata');
