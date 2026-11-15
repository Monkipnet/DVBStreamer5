# DVBStreamer5 V10.8.545 — HTTP security, sessions, trusted proxy and CI audit

## RU — инструкция оператора

### Изменения

- Запросы обрабатываются с ограничением заголовков (16 КиБ), размера тела и времени приёма: 8 с на заголовки, обычные POST до 15 с; резервные файлы до 300 с. Соединение с непрерывно медленной передачей разрывается, чтобы не занимать HTTP-worker.
- Пределы POST: вход в панель 4 КиБ; обычные API 1 МиБ; DOCX 3 МиБ и импорт конфигурации 16 МиБ **только после аутентификации**; загрузка резервного файла по прежнему допускает до 512 МиБ **после аутентификации**. У анонимного клиента большой запрос отклоняется после заголовков, без загрузки всего тела в память.
- После 8 ошибочных попыток входа с одного IP включается ограничение на 5 минут. Проверяются и форма входа, и HTTP Basic. Настройки действуют только в памяти текущего процесса.
- Для изменяющих конфигурацию POST-запросов браузера проверяются Origin/Sec-Fetch-Site, для старых браузеров без Origin требуется X-Requested-With: DVBStreamer5 при использовании cookie. У HTTP API-клиентов с Basic без Origin сохраняется совместимость.
- Сессионная cookie получила SameSite=Strict и Secure при входе через доверенное локальное HTTPS-терминирование. Добавлены X-Content-Type-Options, Referrer-Policy, X-Frame-Options.
- **Мониторинг и сеть → «Доверять реальному IP от локального HTTPS-прокси»**: флажок по умолчанию выключен. После включения DVBStreamer5 использует **только** X-Real-IP, переданный с TCP-адреса 127.0.0.1 или ::1. Все прямые клиентские X-Real-IP/X-Forwarded-For игнорируются. Настройка влияет на middleware, абонентскую IP-фильтрацию, кластер и учёт HTTP/HLS; UDP/SRT не меняются. Сохраняется при редактировании каналов и импорте конфигурации.

### Настройка

Если HTTP сервер находится за **локальным** Nginx с TLS, прокси должен подключаться к `http://127.0.0.1:9000`, переопределять `X-Real-IP` значением `$remote_addr`, а `Host` — значением `$http_host`, сохраняя внешний порт. Убедитесь, что прокси не пропускает от пользователя подложный X-Real-IP. Отметьте флажок и затем проверьте реальные IP в списке абонентов и внешней middleware.

Если прокси на другом компьютере или используется TCP-туннель, **не** включайте эту опцию: доверие ограничено loopback. Старый режим по умолчанию использует адрес TCP-соединения.

При миграции существующих скриптов POST без браузерного Origin и с cookie используйте заголовок `X-Requested-With: DVBStreamer5`. Для API-клиентов с HTTP Basic обработка без Origin сохраняется. Предпочитайте TLS на административном порту. При выключенной опции IP-фильтрация за Nginx может видеть 127.0.0.1.

### Эмуляция в GitHub Actions

- `tests/test_http_security_v545.cpp` — IP spoofing, Origin, лимиты тела, блокировка входа.
- `tests/web/test_http_security_v545.js` — связывание API, сохранения настройки и HTTP/HLS путей.
- `build-main-linux.yml` — тест под ASan/UBSan до полной сборки.
- `security-audit.yml` — CodeQL C/C++/JavaScript и OSV/CVE анализ закреплённых/вендорных зависимостей, при push в main, вручную и еженедельно. Найденные OSV уязвимости приводят к ошибке задания; применимость к реальным серверам требует сверки версий установленных библиотек.

Проверка и portable из **корня исходников**:

```bash
git switch main
git pull --ff-only
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
c++ -std=c++17 -O1 -g -UNDEBUG -Wall -Wextra -Isrc tests/test_http_security_v545.cpp -o /tmp/dvbstreamer5-http-security
/tmp/dvbstreamer5-http-security
node tests/web/test_http_security_v545.js
ctest --test-dir build --output-on-failure
bash scripts/make_portable_linux_bundle.sh --build-dir build --archive
ls -lh dist/DVBStreamer5-10.8.545-linux-x86_64.tar.gz
```

До установки на рабочие серверы проверьте среднее время загрузки резервного MP4, вход в админку, middleware API и IP-фильтр через Nginx. Ограничения и CSRF могут повлиять на нестандартные инструменты API. Схема безопасности не исправляет все ранее выявленные проблемы: стартовый admin/admin, запуск демона от root и цифровая подпись релизов требуют отдельной работы.

## EN — operator notes

HTTP headers are capped at 16 KiB and incoming data has absolute timeouts. The login body is limited to 4 KiB, ordinary POST requests to 1 MiB, configuration import to 16 MiB, DOCX to 3 MiB and authenticated backup uploads retain the previous 512 MiB cap. Large privileged uploads are rejected before reading their bodies when not authenticated. After 8 failed logins per IP, a 5-minute lock is applied to both the JSON login and legacy Basic authentication.

State-changing browser POST requests are checked for same-origin and Fetch Metadata; cookie-authenticated requests without an Origin must include the X-Requested-With: DVBStreamer5 header. Session cookies use SameSite=Strict and Secure on the local HTTPS-proxy path. The new **Monitoring & network → Trust real client IP from local HTTPS proxy** checkbox is **off by default**. When enabled, only a validated X-Real-IP from the directly connected loopback address is trusted; never enable it for a remote, untrusted proxy. This changes HTTP/HLS subscriber attribution, middleware IP allowlists and cluster viewer selection, not SRT/UDP.

The security-audit GitHub Actions workflow runs CodeQL and OSV scanning on main, on demand and weekly; malformed-input and sanitizer checks run alongside the normal Linux build. Existing runtime instances are not changed by a Git commit.

## HR — upute

HTTP zaglavlja ograničena su na 16 KiB, a čitanje zahtjeva ima vremenska ograničenja. Prijava dopušta 4 KiB, obični POST 1 MiB, uvoz konfiguracije 16 MiB i DOCX 3 MiB; autentificirani prijenos rezervnih datoteka zadržava 512 MiB. Veliki neautorizirani zahtjevi odbijaju se prije primanja cijelog tijela. Nakon osam pogrešnih prijava s iste IP adrese slijedi petominutna blokada, uključujući HTTP Basic.

Preglednikove POST operacije provjeravaju Origin i Fetch Metadata; kod prijave kolačićem bez Origin potreban je X-Requested-With: DVBStreamer5. Kolačići sesije koriste SameSite=Strict i Secure putem lokalnog HTTPS proxyja. Nova opcija **Nadzor i mreža → Vjeruj stvarnoj IP adresi iz lokalnog HTTPS proxyja** zadano je isključena. Kad se uključi, vjeruje se samo provjerenom X-Real-IP s lokalnih adresa 127.0.0.1 ili ::1; izravni klijenti ne mogu lažirati IP pomoću HTTP zaglavlja. Opcija vrijedi za HTTP/HLS, vanjski middleware, klaster i filtriranje pretplatnika, ali ne za SRT/UDP.

GitHub Actions pokreće CodeQL, OSV/CVE te ASan/UBSan testove i provjere neispravnih ulaznih podataka. Prije rada u produkciji provjerite middleware, filtriranje po IP-u i upload rezervnih datoteka.
