#pragma once
// V536: Cluster view is appended to the existing Web UI, leaving the
// interactive DVB / IPTV tile rendering and all media pipelines untouched.
#include <string>

inline std::string addClusterWebUi(std::string html) {
    const auto before=[&](const std::string& anchor,const std::string& insertion) {
        const auto position=html.find(anchor);
        if(position!=std::string::npos)html.insert(position,insertion);
    };
    const auto after=[&](const std::string& anchor,const std::string& insertion) {
        const auto position=html.find(anchor);
        if(position!=std::string::npos)html.insert(position+anchor.size(),insertion);
    };
    static const char* styles=R"CLUSTERSTYLES(
<style>
#clusterDashboard{margin:16px 0 28px}
#clusterDashboard[hidden],#clusterDashboard [hidden]{display:none!important}
.cluster-heading{display:flex;align-items:center;gap:10px;font-size:14px;font-weight:700;color:#d9ebff;margin:0 0 10px}
.cluster-heading button{margin-left:auto}
.cluster-server-list{display:flex;flex-direction:column;gap:10px}
.cluster-server-list .cluster-server-group>summary{list-style:none;display:flex;align-items:center;gap:10px;cursor:pointer}
.cluster-server-list .cluster-server-group>summary::-webkit-details-marker{display:none}
.cluster-server-group>summary:before{content:"▸";color:#7dd1ff}
.cluster-server-group[open]>summary:before{content:"▾"}
.cluster-server-name{flex:1;min-width:0;overflow-wrap:anywhere;font-weight:700}
.cluster-server-indicator{color:#a1e8b9;border-radius:999px;background:rgba(40,175,105,.16);padding:3px 8px;font-size:11px;white-space:nowrap}
.cluster-server-indicator.offline{color:#ffb3b3;background:rgba(255,84,84,.15)}
.cluster-server-tiles{display:grid;grid-template-columns:repeat(auto-fill,minmax(min(100%,220px),1fr));gap:10px}
.cluster-channel{min-width:0;padding:10px;border-radius:10px;border:1px solid rgba(125,209,255,.17);background:rgba(22,32,46,.85);display:flex;flex-direction:column;gap:5px}
.cluster-channel-title{font-size:12px;font-weight:700;color:#e9f3ff;overflow-wrap:anywhere}
.cluster-channel-metrics{font-size:11px;color:#aabbce}
.cluster-channel-metrics.online{color:#8fe6ad}
.cluster-note{font-size:12px;color:#a0b3c9;padding:8px}
.cluster-peersetting{display:flex;justify-content:space-between;align-items:center;gap:9px;margin:9px 0;padding:8px;border:1px solid rgba(120,200,240,.18);border-radius:9px}
.cluster-peersetting>div{min-width:0;overflow-wrap:anywhere}
.cluster-peersetting small{display:block;color:#a4b8ca}
.cluster-config-form{display:grid;gap:8px;margin-top:16px}
.cluster-config-form label{font-size:12px;color:#b6d3ed}
.cluster-config-form input,#clusterLocalToken{display:block;width:100%;min-width:0;box-sizing:border-box;padding:9px;background:#101a28;border:1px solid #4f6582;border-radius:8px;color:#eef8ff}
.cluster-config-hint{font-size:12px;line-height:1.5;color:#aabfd1;margin:10px 0}
html[data-dashboard-theme="light"] .cluster-heading,html[data-dashboard-theme="light"] .cluster-channel-title{color:#243b55}
html[data-dashboard-theme="light"] .cluster-channel{background:#fff;border-color:#cbd9e8}
html[data-dashboard-theme="light"] .cluster-channel-metrics{color:#415d75}
html[data-dashboard-theme="light"] .cluster-channel-metrics.online{color:#178249}
html[data-dashboard-theme="light"] .cluster-server-indicator{color:#196d3d;background:#dff2e6}
html[data-dashboard-theme="light"] .cluster-server-indicator.offline{color:#a23b42;background:#ffe4e5}
html[data-dashboard-theme="light"] .cluster-config-form input,html[data-dashboard-theme="light"] #clusterLocalToken{background:#fff;border-color:#b6cadc;color:#243c57}
html[data-dashboard-theme="light"] .cluster-config-hint,html[data-dashboard-theme="light"] .cluster-note{color:#536b83}
html[data-dashboard-theme="light"] .cluster-config-form label{color:#37516b}
</style>
)CLUSTERSTYLES";
    static const char* menu=R"CLUSTERMENU(
<button type="button" class="system-menu-item" onclick="openClusterSettings();closeSystemMenu()"
data-ui-ru="Кластер / Пиринг" data-ui-en="Cluster / Peering" data-ui-hr="Klaster / Peering">Кластер / Пиринг</button>
)CLUSTERMENU";
    static const char* dashboard=R"CLUSTERDASH(
<section id="clusterDashboard" hidden aria-label="Кластер серверов">
<div class="cluster-heading"><span id="clusterDashboardTitle">Кластер серверов</span>
<button type="button" id="clusterRefreshButton" class="button-secondary" onclick="refreshClusterStatus(true)" title="Обновить кластер" aria-label="Обновить кластер">↻</button></div>
<div id="clusterServerList" class="cluster-server-list"></div>
</section>
)CLUSTERDASH";
    static const char* script=R"CLUSTERSCRIPT(
<script>
// V536 isolated peering monitor. Read-only remote streams; the server never
// sends start, stop, import, redirect, or failover operations to peers.
const clusterOpenStorageKey='dvbstreamer5-cluster-spoilers-v1';
let clusterGroupOpen={};
try {
  const saved=JSON.parse(localStorage.getItem(clusterOpenStorageKey)||'{}');
  if(saved&&typeof saved==='object'&&!Array.isArray(saved))clusterGroupOpen=saved;
}catch(_){}
let clusterPeersSnapshot=[],clusterLastPoll=0,clusterRequest=null;
function clusterText(ru,en,hr){return language==='ru'?ru:language==='hr'?hr:en;}
function clusterError(message){
  const text=String(message||'');
  const translated={
    'Cannot read or create private cluster configuration':
      ['Не удалось создать защищённую конфигурацию кластера','Cannot create secure cluster config','Nije moguće stvoriti sigurnu konfiguraciju klastera'],
    'Cannot read private cluster configuration':
      ['Не удалось прочитать защищённую конфигурацию','Cannot read secure cluster config','Nije moguće pročitati konfiguraciju klastera'],
    'Cluster peer configuration unreadable':
      ['Не удалось прочитать настройки кластера','Cluster peer configuration unreadable','Konfiguracija klastera nije dostupna'],
    'Invalid cluster peer request':
      ['Неверный запрос кластера','Invalid cluster peer request','Neispravan zahtjev klastera'],
    'Use HTTPS or a private-LAN IPv4 HTTP origin, without a path':
      ['Используйте HTTPS или HTTP с частным IPv4, без пути','Use HTTPS or private IPv4 HTTP without a path','Koristite HTTPS ili privatni IPv4 HTTP bez putanje'],
    'Peer name and access token are required':
      ['Необходимо имя и ключ сервера','Peer name and token required','Potreban je naziv i ključ poslužitelja'],
    'Invalid peer name, origin or 64-digit token':
      ['Проверьте имя, адрес или 64-значный ключ','Invalid peer name, address or 64-digit key','Neispravan naziv, adresa ili ključ od 64 znamenke'],
    'Maximum of 8 peers':
      ['Максимум 8 серверов в кластере','Maximum of 8 peers','Najviše 8 poslužitelja u klasteru'],
    'Unknown peer':
      ['Сервер не найден','Unknown peer','Nepoznat poslužitelj'],
    'Failed to write private cluster configuration':
      ['Ошибка сохранения настроек кластера','Failed to save cluster config','Nije moguće spremiti konfiguraciju klastera'],
    'Invalid cluster routing settings':
      ['Неверные настройки маршрутизации','Invalid routing settings','Neispravne postavke usmjeravanja'],
    'Cannot read cluster settings':
      ['Не удалось прочитать настройки маршрутизации','Cannot read cluster routing settings','Nije moguće pročitati postavke usmjeravanja'],
    'Cannot save cluster routing settings':
      ['Не удалось сохранить маршрутизацию','Cannot save cluster routing settings','Nije moguće spremiti usmjeravanje'],
    'Panel login and same-origin request required':
      ['Нужен вход администратора в панель','Panel administrator login required','Potrebna je prijava administratora'],
    'Cluster administration requires panel login':
      ['Нужен вход администратора в панель','Panel administrator login required','Potrebna je prijava administratora']
  };
  const values=translated[text];
  return values ? clusterText(values[0],values[1],values[2]) : text;
}
function updateClusterLabels(){
  const area=document.getElementById('clusterDashboard');
  if(area)area.setAttribute('aria-label',clusterText('Кластер серверов','Server cluster','Klaster poslužitelja'));
  const button=document.getElementById('clusterRefreshButton');
  if(button){
    const label=clusterText('Обновить состояние кластера','Refresh cluster status','Osvježi status klastera');
    button.title=label;button.setAttribute('aria-label',label);
  }
}

function persistClusterOpen(){
  try{localStorage.setItem(clusterOpenStorageKey,JSON.stringify(clusterGroupOpen));}catch(_){}
}
function clusterNormalized(value){return String(value||'').toLocaleLowerCase();}
function clusterVisibleChannel(channel,peer,query) {
  if(!query)return true;
  const fields=[
    channel.name,channel.id,peer.name,peer.remote_name,peer.url
  ].map(clusterNormalized).join(' ');
  return query.split(/\s+/).filter(Boolean).every(term=>fields.includes(term));
}
function filterClusterGroups(){
  const query=typeof tileSearchQuery==='string'?tileSearchQuery:'';
  for(const details of document.querySelectorAll('#clusterServerList .cluster-server-group')){
    const name=clusterNormalized(details.querySelector('.cluster-server-name')?.textContent);
    const ownMatch=query&&query.split(/\s+/).every(t=>
      (name+' '+clusterNormalized(details.dataset.clusterUrl)).includes(t));
    let visible=0;
    for(const card of details.querySelectorAll('.cluster-channel')){
      const matching=!query||ownMatch||query.split(/\s+/).every(t=>
        clusterNormalized(card.dataset.search).includes(t));
      card.hidden=!matching;
      if(matching)++visible;
    }
    // An offline server can still be found by server name.
    details.hidden=!!query&&!ownMatch&&visible===0;
    if(query&&!details.hidden)details.open=true;
    else if(!query)details.open=clusterGroupOpen[details.dataset.clusterUrl]===true;
  }
  const area=document.getElementById('clusterDashboard');
  if(area)area.hidden=clusterPeersSnapshot.length===0||
    (!!query&&!document.querySelector('#clusterServerList .cluster-server-group:not([hidden])'));
}
function renderClusterPeers(peers) {
  clusterPeersSnapshot=Array.isArray(peers)?peers:[];
  const list=document.getElementById('clusterServerList');
  if(!list)return;
  list.replaceChildren();
  const title=document.getElementById('clusterDashboardTitle');
  if(title)title.textContent=clusterText('Кластер серверов','Server cluster','Klaster poslužitelja')+
    ' ('+clusterPeersSnapshot.length+')';
  updateClusterLabels();
  const sorted=[...clusterPeersSnapshot].sort((a,b)=>
    String(a.name||'').localeCompare(String(b.name||''),language,{numeric:true,sensitivity:'base'}));
  for(const peer of sorted){
    const key=String(peer.url||''),label=String(peer.name||peer.remote_name||key);
    const online=peer.online===true,channels=Array.isArray(peer.channels)?peer.channels:[];
    const details=document.createElement('details');
    details.className='dvb-card-group cluster-server-group';
    details.dataset.clusterUrl=key;
    details.open=clusterGroupOpen[key]===true; // like DVB card: collapsed by default
    const summary=document.createElement('summary');
    const name=document.createElement('strong');
    name.className='cluster-server-name';
    name.textContent=label;
    name.title=key+(peer.remote_name&&peer.remote_name!==label?' · '+peer.remote_name:'');
    const indicator=document.createElement('span');
    indicator.className='cluster-server-indicator'+(online?'':' offline');
    indicator.textContent=online ?
      channels.filter(ch=>ch.active===true).length+'/'+channels.length+' '+
       clusterText('активно','active','aktivno') :
      clusterText('Нет связи','Offline','Nedostupno');
    summary.append(name,indicator);
    const grid=document.createElement('div');
    grid.className='dvb-card-tiles cluster-server-tiles';
    if(online) {
      for(const ch of channels){
        const card=document.createElement('div');
        card.className='cluster-channel';
        card.dataset.search=[ch.id,ch.name,label,key,peer.remote_name]
          .map(clusterNormalized).join(' ');
        const heading=document.createElement('div');
        heading.className='cluster-channel-title';
        heading.textContent=String(ch.name||ch.id||'—');
        const metrics=document.createElement('div');
        metrics.className='cluster-channel-metrics'+(ch.active===true?' online':'');
        metrics.textContent=ch.active===true?
          (ch.using_backup===true?clusterText('РЕЗЕРВ','BACKUP','REZERVA'):
            clusterText('ОНЛАЙН','ONLINE','ONLINE'))+' · '+
          (Number(ch.bitrate_out_kbps)||0)+' kbps':
          clusterText('ОФЛАЙН','OFFLINE','OFFLINE');
        card.append(heading,metrics);
        grid.appendChild(card);
      }
    }
    if(!online||channels.length===0){
      const note=document.createElement('div');
      note.className='cluster-note';
      note.textContent=!online?clusterText('Нет связи с сервером или неверный ключ',
        'Server unreachable or access key rejected',
        'Nema veze ili neispravan ključ'):
        clusterText('На сервере нет каналов','No channels on this server','Nema kanala');
      grid.appendChild(note);
    }
    details.append(summary,grid);
    details.addEventListener('toggle',()=>{
      if(tileSearchQuery)return;
      clusterGroupOpen[key]=details.open;
      persistClusterOpen();
    });
    list.appendChild(details);
  }
  filterClusterGroups();
}
async function refreshClusterStatus(force=false){
  if(clusterRequest)return clusterRequest;
  if(!force&&Date.now()-clusterLastPoll<15000)return;
  clusterLastPoll=Date.now();
  clusterRequest=fetchJson('/api/cluster/status',{},12000)
    .then(result=>{
      if(result.result!=='ok')throw Error(clusterError(result.error||clusterText('Ошибка мониторинга кластера','Cluster status failed','Pogreška nadzora klastera')));
      renderClusterPeers(result.peers);
    })
    .catch(error=>console.warn('Cluster monitor:',error))
    .finally(()=>{clusterRequest=null;});
  return clusterRequest;
}
async function openClusterSettings(){
  try{
    const data=await fetchJson('/api/cluster/config',{},8000);
    if(data.result!=='ok')throw Error(clusterError(data.error||clusterText('Настройки кластера недоступны','Cluster configuration unavailable','Konfiguracija klastera nije dostupna')));
    const peerRows=(data.peers||[]).map((peer,index)=>
      '<div class="cluster-peersetting"><div><strong>'+escapeHtmlValue(peer.name)+
      '</strong><small>'+escapeHtmlValue(peer.url)+(peer.viewer_url?' · '+escapeHtmlValue(peer.viewer_url):'')+'</small></div>'+
      '<button class="button-danger" type="button" data-cluster-remove="'+index+'">×</button></div>'
    ).join('');
    openModal('<h2>'+clusterText('Кластер / Пиринг','Cluster / Peering','Klaster / Peering')+'</h2>'+
      '<p class="cluster-config-hint">'+clusterText(
      'Мониторинг серверов; отдельная маршрутизация новых HTTP/HLS-зрителей. Существующее вещание не переключается. Используйте WireGuard или HTTPS.',
      'Peer monitoring with optional routing for new HTTP/HLS viewers. Existing sessions are not moved. Use WireGuard or HTTPS.',
      'Nadzor čvorova i usmjeravanje novih HTTP/HLS gledatelja. Postojeće veze se ne premještaju. Koristite WireGuard ili HTTPS.')+'</p>'+
      '<label>'+clusterText('Ключ этого сервера','This server access token','Pristupni ključ ovog poslužitelja')+'</label>'+
      '<input id="clusterLocalToken" readonly value="'+escapeHtmlValue(data.node_token)+'">'+
      '<button class="button-secondary" type="button" onclick="copyClusterNodeToken()">'+
      clusterText('Копировать ключ','Copy token','Kopiraj ključ')+'</button>'+
      '<h3>'+clusterText('Серверы','Servers','Poslužitelji')+'</h3>'+
      '<div id="clusterSettingsPeers">'+(peerRows||'<p class="cluster-config-hint">'+
        clusterText('Серверы пока не добавлены','No peers configured','Nema čvorova')+'</p>')+'</div>'+
      '<div class="cluster-config-form"><label>'+clusterText('Имя сервера','Server name','Naziv poslužitelja')+'</label>'+
      '<input id="clusterPeerName" maxlength="80" placeholder="home2" autocomplete="off">'+
      '<label>'+clusterText('IP или домен с портом','IP or domain and port','IP ili domena i port')+'</label>'+
      '<input id="clusterPeerUrl" maxlength="250" placeholder="http://10.8.0.2:9000" autocomplete="off">'+
      '<label>'+clusterText('Публичный адрес для зрителей (необязательно)','Viewer-facing address (optional)','Adresa za gledatelje (neobvezno)')+'</label>'+
      '<input id="clusterPeerViewerUrl" maxlength="250" autocomplete="off" placeholder="https://edge.example.com:9000">'+
      '<p class="cluster-config-hint">'+clusterText(
      'Укажите HTTP(S)-адрес удалённого сервера, доступный зрителям. Если пусто, используется служебный адрес. Не указывайте /api или /cluster/watch.',
      'Set the remote media HTTP(S) origin reachable by viewers. If empty, the control URL is used. Do not include /api or /cluster/watch.',
      'Upišite adresu udaljenog HTTP(S) medijskog poslužitelja dostupnu gledateljima. Ako je prazna, koristi se adresa za nadzor.')+'</p>'+
      '<label>'+clusterText('Ключ удалённого сервера','Remote server token','Ključ udaljenog poslužitelja')+'</label>'+
      '<input id="clusterPeerToken" type="password" maxlength="64" autocomplete="new-password" placeholder="'+clusterText('64 шестнадцатеричных символа','64 hexadecimal digits','64 heksadekadske znamenke')+'">'+
      '<button class="button-primary" type="button" onclick="addClusterPeer()">'+
      clusterText('Добавить сервер','Add server','Dodaj poslužitelj')+'</button></div>'+
      '<h3>'+clusterText('Маршрутизация HTTP / HLS','HTTP / HLS routing','HTTP / HLS usmjeravanje')+'</h3>'+
      '<div class="cluster-config-form">'+
      '<label><input type="checkbox" id="clusterAutoFailover" '+(data.auto_failover?'checked':'')+'> '+
      clusterText('Автоматически выбирать резервный сервер для новых зрителей',
        'Automatically fail over new viewers to healthy servers',
        'Automatski preusmjeri nove gledatelje na dostupne poslužitelje')+'</label>'+
      '<label><input type="checkbox" id="clusterBalanceViewers" '+(data.balance_viewers?'checked':'')+'> '+
      clusterText('Балансировать новых зрителей по числу подключений',
        'Balance new viewers by active connection count',
        'Raspodijeli nove gledatelje prema broju veza')+'</label>'+
      '<p class="cluster-config-hint">'+clusterText(
      'Работает только для /cluster/watch/ID.ts и /cluster/watch/ID.m3u8, а также API /api/cluster/route. Прямые адреса каналов не меняются. На конечном сервере действуют его правила доступа.',
      'Applies only to /cluster/watch/ID.ts, /cluster/watch/ID.m3u8 and /api/cluster/route. Direct stream URLs stay unchanged. Target-node authorization still applies.',
      'Vrijedi samo za /cluster/watch/ID.ts, /cluster/watch/ID.m3u8 i /api/cluster/route. Izravne adrese ostaju iste. Odredišni čvor provjerava pristup.')+'</p>'+
      '<button class="button-primary" type="button" onclick="saveClusterRouting()">'+
      clusterText('Сохранить маршрутизацию','Save routing','Spremi usmjeravanje')+'</button></div>');
    document.querySelectorAll('[data-cluster-remove]').forEach(button=>{
      button.addEventListener('click',()=>deleteClusterPeer(data.peers[Number(button.dataset.clusterRemove)]?.url));
    });
  }catch(error){uiError(error.message||String(error));}
}
async function copyClusterNodeToken(){
  const input=document.getElementById('clusterLocalToken');
  if(!input)return;
  try{await navigator.clipboard.writeText(input.value);}catch(_){
    input.select();document.execCommand('copy');input.setSelectionRange(0,0);
  }
}
async function addClusterPeer(){
  const name=document.getElementById('clusterPeerName')?.value?.trim();
  const url=document.getElementById('clusterPeerUrl')?.value?.trim();
  const token=document.getElementById('clusterPeerToken')?.value?.trim();
  const viewer_url=document.getElementById('clusterPeerViewerUrl')?.value?.trim()||'';
  if(!name||!url||!token){
    uiError(clusterText('Укажите имя, адрес и ключ','Enter name, URL and token','Unesite naziv, adresu i ključ'));
    return;
  }
  try{
    const data=await fetchJson('/api/cluster/add',{
      method:'POST',headers:{'Content-Type':'application/json','X-Requested-With':'DVBStreamer5'},
      body:JSON.stringify({name,url,token,viewer_url})
    },10000);
    if(data.result!=='ok')throw Error(clusterError(data.error||clusterText('Не удалось добавить сервер','Cannot add peer','Nije moguće dodati poslužitelj')));
    await openClusterSettings();await refreshClusterStatus(true);
  }catch(error){uiError(error.message||String(error));}
}
async function saveClusterRouting(){
  const auto_failover=!!document.getElementById('clusterAutoFailover')?.checked;
  const balance_viewers=!!document.getElementById('clusterBalanceViewers')?.checked;
  try{
    const data=await fetchJson('/api/cluster/routing/save',{
      method:'POST',headers:{
        'Content-Type':'application/json','X-Requested-With':'DVBStreamer5'
      },body:JSON.stringify({auto_failover,balance_viewers})
    },10000);
    if(data.result!=='ok')throw Error(clusterError(data.error||
      clusterText('Ошибка сохранения маршрутизации','Cannot save routing',
      'Nije moguće spremiti usmjeravanje')));
    await openClusterSettings();
    await refreshClusterStatus(true);
  }catch(error){uiError(error.message||String(error));}
}
async function deleteClusterPeer(url){
  if(!url||!confirm(clusterText('Удалить сервер из кластера?','Remove this cluster peer?','Ukloniti čvor?')))return;
  try{
    const data=await fetchJson('/api/cluster/delete',{
      method:'POST',headers:{'Content-Type':'application/json','X-Requested-With':'DVBStreamer5'},
      body:JSON.stringify({url})
    },10000);
    if(data.result!=='ok')throw Error(clusterError(data.error||clusterText('Не удалось удалить сервер','Cannot remove peer','Nije moguće ukloniti poslužitelj')));
    await openClusterSettings();await refreshClusterStatus(true);
  }catch(error){uiError(error.message||String(error));}
}
// Keep the existing local tile search unchanged; simply extend its scope.
const previousClusterTileFilter=applyTileSearch;
applyTileSearch=function(){previousClusterTileFilter();filterClusterGroups();};
const previousClusterLanguage=applyLanguage;
applyLanguage=function(){previousClusterLanguage();updateClusterLabels();if(clusterPeersSnapshot.length)renderClusterPeers(clusterPeersSnapshot);};
setInterval(()=>{if(!document.hidden)refreshClusterStatus();},15000);
refreshClusterStatus();
</script>
)CLUSTERSCRIPT";
    before("</head>",styles);
    after("<button class=\"system-menu-item\" onclick=\"openNetworkModal();closeSystemMenu()\" data-i18n=\"network\">Network</button>",menu);
    after("<div id=\"tiles\" class=\"tile-grid\"></div>",dashboard);
    before("</body>",script);
    return html;
}
