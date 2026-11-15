# DVBStreamer5 V10.8.537 — Cluster failover, viewer balancing and middleware API

## Русский — настройка

### 1. Подготовка

Установите V10.8.537 на все серверы, которые участвуют в пиринге. Каждый сервер продолжает работать самостоятельно.
Разрешите соединения между узлами по служебному HTTP-порту через LAN или WireGuard, либо используйте HTTPS с действительным сертификатом. Не открывайте административную панель в Интернет без защиты.

На каждом сервере откройте **Меню → Мониторинг и сеть → Кластер / Пиринг** и скопируйте **Ключ этого сервера**. Это отдельный ключ для чтения состояния узла, не пароль администратора и не ключ middleware.

На сервере-маршрутизаторе добавьте соседей: имя, служебный URL (например, http://10.8.0.2:9000), собственный ключ удалённого сервера и, при необходимости, **Публичный адрес для зрителей** (например, https://edge.example.org:9443). Адрес для зрителей должен вести на действительный HTTP MPEG-TS или HLS выход нужного порта, доступный абонентам. Если поле не заполнено, используется служебный адрес узла. Для HTTP поддерживаются только частные IPv4, а для доменов — HTTPS с проверкой TLS.

Каждый сервер появляется в отдельной группе по имени, под спойлером как DVB-карта. Отображаются каналы, состояние и битрейт. Все подписи поддерживают RU/EN/HR.

### 2. Включение переключения и балансировки

В том же окне доступны независимые параметры:

- **Автоматически выбирать резервный сервер для новых зрителей** — включает маршрутизацию и приоритет локального канала, если он работает. При отказе локального источника новые запросы направляются к доступному соседу.
- **Балансировать новых зрителей по числу подключений** — включает распределение новых запросов на наименее загруженный здоровый узел (в том числе локальный). При одинаковой нагрузке выбор стабилен для IP зрителя.

Нажмите **Сохранить маршрутизацию**. По умолчанию оба параметра отключены. Настройки сохраняются в отдельном файле cluster-peers.json с правами 0600, без перезапуска или изменения плиток.

Соответствие программ: сначала сравнивается tvg-id, если он задан с обеих сторон; конфликтующие tvg-id не связываются. Иначе используется точный ID или точное название канала. На выбранном сервере должен быть включён соответствующий HTTP MPEG-TS или HLS выход. Работоспособность определяется активностью потока и ненулевым исходящим битрейтом. Состояние соседей кэшируется на 5 секунд.

### 3. Стабильные адреса просмотра

Для нового HTTP MPEG-TS подключения:

    http://ROUTER:9000/cluster/watch/CHANNEL_ID.ts

Для нового HLS подключения:

    http://ROUTER:9000/cluster/watch/CHANNEL_ID.m3u8

Замените CHANNEL_ID внутренним ID канала на маршрутизаторе. Ответ — временный HTTP 307 на прямой адрес выбранного сервера. Конечный узел повторно проверяет право доступа абонента. Исходные query-параметры просмотра передаются в Location, поэтому не используйте в них административные или кластерные ключи. При необходимости применяйте подписанные клиентские параметры middleware.

**Важное ограничение:** уже открытые HTTP MPEG-TS соединения не мигрируют между узлами. Переключение происходит при новом подключении/переподключении клиента. HLS-манифест перенаправляет зрителя на выбранный узел, где он скачивает сегменты. Прямые URL существующих каналов остаются прежними. UDP, RTP, SRT, RTMP, транскодирование и внутренний поток тюнера здесь не балансируются.

### 4. Внешняя middleware через API

Раздел **Авторизация потоков** содержит независимый API-ключ middleware и список IP, которым разрешено его использовать. Укажите фактический IP вашей middleware в разрешённых адресах, создайте/скопируйте её ключ (не ключ кластера) и запрашивайте маршрут:

    curl -fsS -H "X-API-Key: $MIDDLEWARE_KEY" \
      "http://ROUTER:9000/api/cluster/route?stream_id=CHANNEL_ID&format=ts&client_ip=198.51.100.22"

Для HLS используйте format=hls. Ответ JSON содержит result, stream_id, protocol, origin, local, url, policy и authorization_required. Middleware может выдать поле url зрителю или использовать его в собственном механизме разрешения сеанса.

API доступен также администратору с действующей сессией панели. Только GET; API-ключ middleware ограничен своим списком IP и **не** позволяет изменять конфигурацию, запускать/останавливать каналы или менять настройки кластера.

Если используется список разрешённых IP абонентов, проверка выполняется до выдачи маршрута. При включённой внешней авторизации запрос API — **только поиск адреса**, а не разрешение просмотра: внешний backend должен авторизовать клиента, и конечный сервер вновь проверяет доступ по своим правилам. Middleware должна корректно передавать собственные подписанные параметры сессии на конечный адрес. Не используйте cluster-peers.json или его ключи в запросах зрителей.

### 5. Тестирование и диагностика

GitHub Actions эмулирует 3 узла, HTTP/HLS-клиентов, синтетические MPEG-TS пакеты, падение и восстановление узлов, BACKUP, распределение нагрузки, ограничения адресов и права middleware. CI не заменяет испытания на двух реальных серверах с живым потоком, особенно при долгих TCP-сессиях и внешней middleware.

Проверяйте статус через **Кластер / Пиринг**; служебный API маршрутизатора возвращает ошибку при отсутствии рабочего выхода. Если маршрут не найден, проверьте совпадение tvg-id/ID/имени, исходящий битрейт, доступность viewer URL и авторизацию.

## English — configuration

Install V10.8.537 on participating peers. In **Menu → Monitoring & network → Cluster / Peering**, copy each node's **This server access token**. On the routing node add the peer's display name, private HTTP/WireGuard or HTTPS control origin, remote node token and optional **Viewer-facing address**, pointing at its actual media output port. Peers appear grouped by server name in collapsible DVB-style sections.

Enable **Automatically fail over new viewers to healthy servers** to prefer a healthy local output and redirect new sessions when it fails. Enable **Balance new viewers by active connection count** to choose the least loaded healthy HTTP/HLS output. Both are off by default; save with **Save routing**.

Clients can open /cluster/watch/CHANNEL_ID.ts (HTTP MPEG-TS) or /cluster/watch/CHANNEL_ID.m3u8 (HLS). HTTP 307 points to a direct media URL; the chosen server still enforces subscriber/middleware authorization. Existing TCP viewers are **not** transparently migrated, nor are UDP/SRT/RTMP outputs or transcoder workloads moved.

External middleware can use **GET /api/cluster/route?stream_id=CHANNEL_ID&format=ts|hls&client_ip=VIEWER_IP**, authenticating with the IP-allowlisted **X-API-Key** configured in Stream authorization. The JSON result includes origin, url, local and policy. Route discovery is not playback authorization; final media access must also be approved by the destination backend. Never expose the cluster node token to a viewer.

## Hrvatski — postavke

Instalirajte V10.8.537 na sve čvorove. Otvorite **Izbornik → Nadzor i mreža → Klaster / Peering** i kopirajte **pristupni ključ svakog poslužitelja**. Na glavnom čvoru dodajte naziv, kontrolnu HTTP adresu preko LAN/WireGuard mreže ili HTTPS, udaljeni ključ te neobveznu **adresu za gledatelje** koja mora voditi na stvarni medijski izlaz. Poslužitelji su grupirani po nazivu u sklopivim DVB grupama.

Uključite **Automatski preusmjeri nove gledatelje na dostupne poslužitelje** za failover ili **Raspodijeli nove gledatelje prema broju veza** za balansiranje. Postavke su zadano isključene. Spremite ih tipkom **Spremi usmjeravanje**.

Za gledatelje koristite /cluster/watch/CHANNEL_ID.ts ili /cluster/watch/CHANNEL_ID.m3u8. HTTP 307 vodi na izravnu adresu odabranog poslužitelja, koji ponovno provjerava prava pristupa. Postojeće TCP veze se **ne premještaju**, kao ni UDP/SRT/RTMP izlazi ili transkodiranje.

Vanjska middleware koristi **GET /api/cluster/route?stream_id=CHANNEL_ID&format=ts|hls&client_ip=VIEWER_IP** uz poseban ključ **X-API-Key** i popis dopuštenih IP adresa. API vraća URL odabranog čvora, ali ne daje dozvolu za gledanje: završni poslužitelj mora provjeriti prava korisnika. Ne otkrivajte ključeve klastera gledateljima.

---

## V10.8.548 — Управление спойлерами кластера / Cluster spoiler controls / Kontrole klastera

**Русский.** В разделе **Меню → Мониторинг и сеть → Кластер / Пиринг** группы серверов имеют большую стрелку **▼ / ▲ точно по центру заголовка**. Нажатие на стрелку, свободное место заголовка или индикатор статуса раскрывает/сворачивает каналы. **Только нажатие непосредственно на название сервера** открывает редактирование его настроек. Раскрытие группы не останавливает потоки, не открывает редактор и не изменяет конфигурацию. Поиск каналов может временно раскрыть группу, не перезаписывая сохранённое состояние.

**English.** In **Menu → Monitoring & network → Cluster / Peering**, the **▼ / ▲ disclosure arrow is centered** in each server header. Click the arrow, empty header space or status indicator to expand/collapse channels. **Only clicking the server's name** opens editing for that server. Expanding a group does not stop streams or modify its configuration. Search may expand groups temporarily without overwriting saved disclosure state.

**Hrvatski.** U **Izbornik → Nadzor i mreža → Klaster / Peering** strelica **▼ / ▲ nalazi se u sredini zaglavlja** poslužitelja. Klik na strelicu, prazni dio zaglavlja ili indikator stanja proširuje i sažima kanale. **Samo klik na naziv poslužitelja** otvara uređivanje tog poslužitelja. Otvaranje grupe ne zaustavlja tokove niti mijenja konfiguraciju. Pretraživanje može privremeno otvoriti grupu bez promjene spremljenog stanja.
