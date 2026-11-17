# DVBStreamer5 V10.8.573 — Yandex TV EPG as a separate plugin

Standalone Python 3.6+ HTML-to-XMLTV process. Does **not** load into the
main DVBStreamer5 executable and cannot call tuner, CA, OSCam or
transcoder functions. Portable ships it at
\`/opt/DVBStreamer5/scripts/epg-yandex-tv\` without enabling it.

## RU — установка
1. В окне обслуживания установите архив DVBStreamer5 V569; установка
   может останавливать сервисы, а плагин сам по себе этого не делает.
2. Установите только EPG-плагин:
   \`sudo bash /opt/DVBStreamer5/scripts/epg-yandex-tv/install_plugin.sh\`
3. Отредактируйте \`/etc/dvbstreamer5/epg-yandex-tv.json\`.
   Включайте \`enabled=true\` и \`permission_confirmed=true\` только
   если у вас есть разрешение на использование и ретрансляцию телепрограммы.
   По умолчанию всё отключено.
4. Автообнаружение каналов через главную страницу экспериментально.
   Надёжнее задать известные HTML-страницы в \`channel_pages\`:
   \`\`\`json
   {"id":"ntv-11","name":"НТВ","url":"https://tv.yandex.by/channel/ntv-11"}
   \`\`\`
5. \`sudo systemctl enable --now epg-yandex-tv.service\`
6. \`curl -s http://127.0.0.1:19785/health\`
7. В DVBStreamer5: **Вещание → Источники XMLTV → + Плагин Яндекс ТВ**.
   Включите источник галочкой, сохраните и обновите. Программа из
   плагина появится в общем EPG-каталоге, и её можно привязывать к плиткам.
   Модуль EPG UDP для WISI берёт данные из уже привязанных плиток.

Парсер распознаёт \`h2\` и \`li\` с \`h3\`, время \`HH:MM\`, а
при наличии \`time datetime\` использует полную дату и UTC-смещение.
Время завершения определяется по **следующей** передаче. Последняя
передача без известного окончания исключается, чтобы не придумывать
длительность. При отсутствии расписания показывает ошибку и
сохраняет предыдущий успешно полученный XMLTV.

Ограничения: разметка страниц Яндекса может измениться. CAPTCHA
не обходится. Доступ разрешён только по HTTPS к \`tv.yandex.by\`,
до 4 МиБ на страницу, не более 32 каналов и 512 событий на канал,
пауза 2 секунды между загрузками. Публичный API Яндекса не
используется. Проверка на реальном сервере с согласованными правами
в настоящий момент не проведена.

## EN
Independent loopback-only Python XMLTV plugin. Install and configure
the separate systemd service first, then enable its existing XMLTV
source in DVBStreamer5. Rights for downstream reuse are required.
No CAPTCHA bypass or guarantees about the current HTML markup.

## HR
Zaseban Python dodatak koji pruža XMLTV samo na 127.0.0.1.
Prvo konfigurirajte i pokrenite njegov systemd servis, zatim
omogućite XMLTV izvor u DVBStreamer5. Potrebna je dozvola za
daljnje korištenje podataka. CAPTCHA se ne zaobilazi.

## Rollback
\`sudo systemctl disable --now epg-yandex-tv.service\`
Disabling the plugin does not restart DVBStreamer5 or OSCam.


## V573: new index discovery and on-demand refresh

Config remains disabled by default. Supported fields:
- \`index_url\`: HTTPS \`tv.yandex.by\` page (default homepage)
- \`crawl_links\`: discover same-origin \`/channel/\` links on index (default true)
- \`crawl_delay\`: 0.5..30 seconds between channel requests (default 2.0)
- \`max_channels\`: up to 32
- \`cache_ttl_seconds\`: 60..86400, default 3600; an old cache is served
  while an asynchronous refresh is queued.
- \`channel_pages\`: explicit ID/name/URL records (skip index discovery)

The plugin keeps the old \`/epg.xml\` endpoint, adds \`/xmltv\` and \`/\`
aliases, and exposes \`/health\` JSON with last success/error and counts.
\`GET /refresh\` queues a nonblocking refresh and returns 202 when
accepted; repeat requests within 60 seconds return 200 (rate-limited).
\`/epg.xml?refresh=1\` requests the refresh and immediately serves
last-good XMLTV. Never publish an empty XMLTV on parser failure.
No optional browser or CAPTCHA bypass dependencies are introduced.

RU: в настройках XMLTV включайте источник только после разрешённого
запуска отдельного EPG-плагина; затем привяжите созданные каналы к плиткам.
EN: the plugin remains disabled until separately configured.
HR: dodatak ostaje isključen dok ga administrator zasebno ne konfigurira.
