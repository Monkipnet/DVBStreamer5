'use strict';
// V547: simulate both modal entry points and verify independent TLS settings.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');

const web=fs.readFileSync('src/http/HttpServerWebUi.inc','utf8');
const cluster=fs.readFileSync('src/http/ClusterWebUiInject.h','utf8');
const start=web.indexOf('function openNetworkModal()');
const end=web.indexOf('async function saveTrustedProxySetting(control)',start);
assert(start>0 && end>start);
const source=web.slice(start,end);
assert(source.includes('function openTlsCertificateModal()'));
new vm.Script(source);

for(const marker of [
  'onclick="openTlsCertificateModal();closeSystemMenu()"',
  'data-ui-ru="SSL / TLS сертификаты"',
  'data-ui-en="SSL / TLS certificates"',
  'data-ui-hr="SSL / TLS certifikati"'
])assert(cluster.includes(marker),marker);
assert(cluster.includes('openNetworkModal();closeSystemMenu()'));
assert(cluster.indexOf('onclick="openClusterSettings();closeSystemMenu()"')>=0);
assert(cluster.indexOf('onclick="openTlsCertificateModal();closeSystemMenu()"')>
       cluster.indexOf('onclick="openClusterSettings();closeSystemMenu()"'));

for(const language of ['ru','en','hr']){
  const modalContent={innerHTML:'',className:''};
  const modal={classList:{add(name){assert.equal(name,'active');}}};
  let certificateRefreshes=0;
  let networkRefreshes=0;
  const requests=[];
  const context={
    language,document:{
      getElementById(id){
        if(id==='modalContent')return modalContent;
        if(id==='modal')return modal;
        return null;
      }
    },
    t:key=>key==='close'?'Close':key==='networkLoad'?'Network load':key,
    modalCloseButton:()=>'<button class="modal-close">×</button>',
    fetchSystemMetrics:()=>{networkRefreshes++;},
    refreshManagedTlsStatus:()=>{certificateRefreshes++;},
    fetchJson:(url)=>{requests.push(url);return Promise.resolve({trust_local_proxy_real_ip:false});},
    console,
  };
  vm.createContext(context);
  vm.runInContext(source,context);

  context.openNetworkModal();
  assert(modalContent.innerHTML.includes('networkTableBody'));
  assert(modalContent.innerHTML.includes('trustLocalProxyRealIp'));
  assert(!modalContent.innerHTML.includes('tlsManagedAutoRenew'));
  assert(!modalContent.innerHTML.includes('tlsSelfSignedButton'));
  assert.equal(certificateRefreshes,0);
  assert.equal(networkRefreshes,1);

  context.openTlsCertificateModal();
  assert(modalContent.innerHTML.includes('tlsManagedAutoRenew'));
  assert(modalContent.innerHTML.includes('tlsSelfSignedButton'));
  assert(modalContent.innerHTML.includes('tlsManagedStatus'));
  assert(!modalContent.innerHTML.includes('networkTableBody'));
  assert(!modalContent.innerHTML.includes('trustLocalProxyRealIp'));
  assert.equal(certificateRefreshes,1);
  assert.equal(networkRefreshes,1);
  assert.equal(modalContent.className,'modal-content network-modal tls-modal');
  assert(modalContent.innerHTML.includes(
    language==='ru'?'SSL / TLS сертификаты':
    language==='hr'?'SSL / TLS certifikati':'SSL / TLS certificates'));
  assert(requests.includes('/api/security/proxy'));
}
console.log('PASS V547 menu ordering, RU/EN/HR translations, independent Network and SSL modals');
