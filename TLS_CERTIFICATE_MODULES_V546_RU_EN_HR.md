# DVBStreamer5 V10.8.546 — Certificate Manager (RU / EN / HR)

## Русский

### Два независимых модуля в программе

Откройте **Меню → Мониторинг и сеть → SSL / TLS сертификаты**.

**Самоподписанный SSL** — введите домен сервера (например, `blumen.zapto.org`) и HTTPS-порт (обычно `9443`), нажмите **Создать самоподписанный сертификат**. Модуль `SelfSignedModule` создаёт RSA-3072/SHA-256 сертификат на 365 дней с SAN, закрытый ключ с правами 0600 и настраивает защищённый Nginx-прокси к действующему HTTP-порту DVBStreamer5. Переключение на самоподписанный сертификат **отключает** автопродление Let’s Encrypt. Самоподписанный сертификат не является публично доверенным — браузер или другой сервер предупредит о нём, пока он не добавлен в доверенные.

**Let’s Encrypt** — заполните домен, HTTPS-порт и действительный email, затем включите галочку **«Получать и автоматически обновлять сертификаты Let’s Encrypt»**. Независимый модуль `LetsEncryptModule` устанавливает недостающий Certbot, использует HTTP-01 webroot, создаёт сертификат и включает системный таймер ежедневной проверки. При неудаче выпуска остаётся действующий сертификат (при первом запуске автоматически создаётся самоподписанный) и назначается повторная попытка; таймер продолжает работу. Снятие галочки выключает автоматическое продление, не удаляя текущий сертификат.

Требования Let’s Encrypt: публичная DNS-запись, указывающая на сервер, входящий **TCP/80**, доступные Certbot и Nginx, отсутствие конфликтующего веб-сервера. Порт `9443` (либо другой выбранный HTTPS-порт) должен быть открыт для клиентов. HTTPS обслуживает Nginx; HTTP MPEG-TS/HLS и существующий порт приложения не переустанавливаются. Модули запускаются в отдельном systemd-процессе, не блокируя веб-панель. Состояние, режим и срок сертификата показываются в окне настроек.

Начиная с V546, скрипты размещаются в переносимом архиве как `tls/manage.sh`, `tls/modules/selfsigned.sh`, `tls/modules/letsencrypt.sh`, `tls/modules/common.sh`. В `/etc/dvbstreamer5/tls` сохраняются настройки и ключи и **никогда не включаются в portable-архив**. Требуются права root для настройки Nginx и systemd. При отсутствии Nginx или Certbot система может установить пакеты через `apt-get` (Ubuntu/Debian). Это происходит только после явного действия администратора, не при запуске DVBStreamer5.

**Если ранее применяли `scripts/setup_cluster_tls.sh` на том же HTTPS-порту**, новый модуль откажется менять прокси и покажет `legacy_tls_proxy_port_conflict`. Сначала нужно аккуратно перенести существующую конфигурацию (не удаляйте действующий сертификат на работающем сервере). Рекомендуется проверить в окно обслуживания и резервировать файлы Nginx.

### Проверка

```bash
# На сервере, из корня исходников
git switch main
git pull --ff-only
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
node tests/web/test_tls_modules_v546.js
c++ -std=c++17 -Isrc -UNDEBUG tests/test_tls_certificate_v546.cpp \
  -ljsoncpp -o /tmp/dvbstreamer5-tls-test
/tmp/dvbstreamer5-tls-test
ctest --test-dir build --output-on-failure
bash scripts/make_portable_linux_bundle.sh --build-dir build --archive
ls -lh dist/DVBStreamer5-10.8.546-linux-x86_64.tar.gz

# После установки portable и включения Let’s Encrypt в панели:
systemctl status dvbstreamer5-le-renew.timer --no-pager
systemctl list-timers --all | grep dvbstreamer5
sudo nginx -t
curl -kI https://blumen.zapto.org:9443/
echo | openssl s_client -connect blumen.zapto.org:9443 \
  -servername blumen.zapto.org 2>/dev/null | \
  openssl x509 -noout -subject -issuer -dates
```

Отдельная галочка **«Доверять реальному IP от локального HTTPS-прокси»** из V545 остаётся независимой, выключена по умолчанию и нужна для IP-фильтрации абонентов через Nginx. Не путайте её с включением Let’s Encrypt.

## English

Go to **Menu → Monitoring & network → SSL / TLS certificates**.

The **Self-signed SSL** module independently creates a 365-day RSA-3072 certificate with DNS SAN, stores its private key as root-only, and configures a local Nginx HTTPS reverse proxy. Activating this module disables Let’s Encrypt auto-renewal.

The independent **Let’s Encrypt** module is **off by default**. Enter a public DNS name, email and HTTPS port, then check **Obtain and automatically renew Let’s Encrypt certificates**. It uses Certbot HTTP-01 webroot and a daily systemd timer. On failure it retains the active certificate, bootstraps self-signed TLS if necessary, and retries daily. Disabling the checkbox stops the timer without deleting the current certificate. Public TCP/80 must be reachable and the chosen HTTPS port must be open. Existing DVBStreamer5 HTTP/UDP/SRT/HLS functionality is unchanged.

The two certificate modules ship with portable archives. Private keys and settings remain under `/etc/dvbstreamer5/tls`, never inside the archive. The panel runs the modules asynchronously through systemd. Nginx/Certbot may be installed via apt on supported systems after an explicit admin action. If the legacy TLS script already owns the chosen HTTPS port, the new module reports a conflict without replacing it. The separate *Trust real client IP from local HTTPS proxy* checkbox continues to work independently and is off by default.

## Hrvatski

Otvorite **Izbornik → Nadzor i mreža → SSL / TLS certifikati**.

Neovisni modul **Samopotpisani SSL** stvara 365-dnevni RSA-3072 certifikat s DNS SAN-om, čuva privatni ključ s dopuštenjem samo za root i postavlja HTTPS putem lokalnog Nginx proxyja. Uključivanje samopotpisanog certifikata isključuje automatsku obnovu Let’s Encrypt.

Zasebni modul **Let’s Encrypt** zadano je isključen. Unesite javnu DNS domenu, email i HTTPS priključak te označite **Izdaj i automatski obnavljaj Let’s Encrypt certifikate**. Koristi Certbot HTTP-01 webroot i dnevni systemd timer. U slučaju neuspjeha zadržava postojeći certifikat ili prvi put stvara samopotpisani certifikat, a provjera se ponavlja svakodnevno. Isključivanje opcije zaustavlja automatsku obnovu bez brisanja postojećeg certifikata. TCP/80 mora biti dostupan izvana; odabrani HTTPS priključak mora biti otvoren.

Oba modula uključena su u portable paket, dok privatni ključevi i postavke ostaju u `/etc/dvbstreamer5/tls`. DVBStreamer5 zadržava postojeće HTTP/HLS/UDP/SRT tokove. Ako stari TLS proxy koristi isti HTTPS priključak, modul prijavljuje sukob i ne prepisuje konfiguraciju. Opcija za vjerovanje stvarnoj IP adresi iz lokalnog HTTPS proxyja zasebna je i zadano isključena.
