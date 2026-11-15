# DVBStreamer5 V10.8.542 — TLS certificates and cluster server editing

## RU — настройка

Нажмите на **строку сервера** в группе кластера, чтобы открыть редактирование. Стрелка справа раскрывает или сворачивает список его каналов. В модальном окне можно изменить имя, адрес узла, публичный адрес для зрителей и проверку TLS. Оставьте поле удалённого ключа пустым для сохранения текущего; новый ключ вводится только при замене. Изменение адреса заменяет существующий узел, не создаёт дубликат.

Флажок **Проверять сертификат TLS** по умолчанию включён. Отключайте его только для доверенного сервера с самоподписанным сертификатом: соединение остаётся зашифрованным, но подлинность удалённого сервера не подтверждается, поэтому возможно перехватить кластерный ключ атакой посредника. Предпочтительнее импортировать CA в доверенные сертификаты либо использовать Let's Encrypt. Начиная с V10.8.544, при снятом флажке разрешён также **http://blumen.zapto.org:9000**. В отличие от HTTPS, HTTP не шифрует трафик или кластерный ключ. Настоятельно рекомендуется использовать только в доверенной сети.

### Получение TLS на удалённом сервере

```bash
git switch main
git pull --ff-only
sudo bash scripts/setup_cluster_tls.sh \
 --domain blumen.zapto.org \
 --backend-port 9000 \
 --https-port 9443 \
 --email YOUR_REAL_EMAIL@example.org
```

Этот сценарий выполняется на **самом удалённом сервере** и не запускается автоматически при одном лишь сохранении узла в панели. Он создаёт Nginx HTTPS-прокси к существующему HTTP порту, пытается получить сертификат через Let's Encrypt HTTP-01 (домен должен указывать на сервер и входящий TCP/80 должен быть доступен) и при неудаче использует самоподписанный сертификат. Ежедневный systemd timer повторяет попытку и обновляет настройки прокси. Не используйте порт HTTPS, занятый другим процессом. Существующие HTTP/UDP/SRT выходы не изменяются.

Проверка:

```bash
sudo nginx -t
systemctl status nginx dvbstreamer5-cluster-tls.timer --no-pager
curl -Iv https://blumen.zapto.org:9443/
```

В настройках кластера используйте адрес `https://blumen.zapto.org:9443`. При самоподписанном сертификате можно снять флажок проверки TLS только для этого узла. **Не вводите ключ кластера в публичную страницу и не публикуйте файл cluster-peers.json.**

## EN — setup

Click the **server row** to edit its details; the right arrow independently expands or collapses its channel list. The original URL identifies the record so saving a changed URL updates the peer instead of adding another one. Leave the remote token field blank to preserve the existing secret.

**Verify TLS certificate** is enabled by default for every peer. Disable it only when explicitly accepting the impersonation and token-interception risk for a trusted self-signed node. HTTPS remains encrypted but server identity is not authenticated. From V10.8.544, explicitly unchecking the box also permits public-domain HTTP (for example http://blumen.zapto.org:9000). Plain HTTP does not encrypt traffic or the shared cluster token.

On the remote server run `sudo bash scripts/setup_cluster_tls.sh --domain YOUR_DOMAIN --backend-port 9000 --https-port 9443 --email YOU@example.org`. The script attempts Let's Encrypt HTTP-01 using reachable TCP/80, falls back to a self-signed certificate, configures an Nginx reverse proxy and installs a daily renewal timer. The script must be run on each relevant node; it does not run automatically from the cluster editor. Use `https://YOUR_DOMAIN:9443` in cluster settings.

## HR — postavljanje

Kliknite **redak poslužitelja** za uređivanje postavki; desna strelica zasebno proširuje ili sažima popis kanala. Promjena URL-a ažurira odabrani čvor, bez duplikata. Ostavite polje ključa praznim ako ne mijenjate postojeći ključ.

**Provjeri TLS certifikat** zadano je uključeno za svaki čvor. Isključite samo ako svjesno prihvaćate rizik lažnog poslužitelja i presretanja ključa kod samopotpisanog certifikata. HTTPS šifrira promet, ali bez provjere ne potvrđuje identitet poslužitelja. Od V10.8.544 isključena provjera omogućuje i HTTP preko javne domene (primjerice http://blumen.zapto.org:9000). Obični HTTP ne šifrira promet niti ključ klastera.

Na udaljenom poslužitelju pokrenite `sudo bash scripts/setup_cluster_tls.sh --domain YOUR_DOMAIN --backend-port 9000 --https-port 9443 --email YOU@example.org`. Skripta pokušava izdati Let's Encrypt certifikat (TCP/80 mora biti dostupan), inače koristi samopotpisani certifikat i postavlja Nginx i dnevni timer za obnovu. Postavljanje se mora pokrenuti na svakom čvoru odvojeno.
