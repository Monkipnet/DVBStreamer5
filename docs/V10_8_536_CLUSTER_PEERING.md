# DVBStreamer5 V10.8.536 — Cluster Peering (Stage 1)

## Русский

V536 добавляет **добровольное подключение соседних серверов** для общего мониторинга. Каждый сервер работает автономно; существующие DVB-тюнеры, CAM, транскодирование и выходные протоколы не меняются.

Веб-панель → Меню → Мониторинг и сеть → **Кластер / Пиринг**.

1. На каждом сервере откройте настройки кластера и скопируйте локальный ключ (64 шестнадцатеричных символа).
2. На управляющем сервере добавьте узел: **имя**, адрес HTTP частной сети (например, http://10.8.0.2:9000) или HTTPS-домен и ключ именно удалённого сервера.
3. Каждый сервер отображается **под отдельным спойлером, по имени**, как DVB-группа. Все группы сначала закрыты. Состояние раскрытия сохраняется в браузере.
4. Внутри — каналы ONLINE/OFFLINE/BACKUP, исходящий битрейт. Поиск из шапки работает и по удалённым каналам/именам серверов.

Stage 1 — **только мониторинг**. Распределённое переключение источников, балансировка зрителей, репликация конфигураций и дистанционный Start/Stop пока не реализованы. Пиринг не влияет на текущие потоки, при недоступности другого сервера вещание продолжается.

Безопасность: отдельный токен для чтения; список и ключи в файле cluster-peers.json с правами 0600. HTTP допускается только по частным IPv4 (LAN/WireGuard); для доменов требуется HTTPS с проверкой TLS. Редиректы запрещены. До 8 узлов и 512 каналов в снимке, до 256 KiB ответа, таймаут 1,5 с на узел. Не публикуйте административный HTTP-порт в Интернет.

CI: GitHub Actions проверяет конфигурацию, ключи, три эмулированных сервера с синтетическими MPEG-TS пакетами, OFFLINE/BACKUP, отказ и восстановление, поиск, спойлеры и все три языка.

## English

V536 introduces **optional read-only cluster monitoring**. Use Menu → Monitoring & network → **Cluster / Peering**. Add each peer with its **name, URL and its own access token**.

Remote servers are grouped **by server name in collapsible DVB-style sections**. Groups start collapsed and preserve their state in browser storage. See remote channels, online/offline/backup status and output bitrate; the dashboard search also covers peer names and channels.

This first stage does **not** provide distributed stream failover, viewer balancing, configuration synchronization or remote start/stop. Existing media pipelines remain independent. Use private IPv4/WireGuard for HTTP or trusted TLS-verified HTTPS for domain names. Tokens are stored in a mode-0600 private file. CI tests three simulated servers and MPEG-TS with peer loss/recovery and RU/EN/HR user interface.

## Hrvatski

V536 dodaje **neobvezni nadzor klastera samo za čitanje**. Otvorite Izbornik → Nadzor i mreža → **Klaster / Peering**. Dodajte svaki poslužitelj s **nazivom, adresom i vlastitim pristupnim ključem**.

Poslužitelji su grupirani **po nazivu u sklopivim grupama poput DVB kartica**. Grupe su isprva zatvorene i pamte stanje u pregledniku. Prikazuju se kanali, statusi online/offline/rezerva i izlazni bitrate. Pretraživanje obuhvaća nazive poslužitelja i kanala.

Prva faza **ne uključuje** automatsko prebacivanje između poslužitelja, balansiranje gledatelja, sinkronizaciju konfiguracija ni daljinski start/stop. Svaki poslužitelj radi samostalno. Za HTTP koristite privatni IPv4 ili WireGuard, a za domene HTTPS s provjerom TLS-a. Pristupni ključevi ostaju u privatnoj datoteci s pravima 0600. GitHub Actions provjerava tri emulirana poslužitelja, MPEG-TS pakete, prekid i povrat veze te RU/EN/HR sučelje.
