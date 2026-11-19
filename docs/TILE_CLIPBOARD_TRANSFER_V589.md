# DVBStreamer5 V10.8.589 — прямая вставка плиток (без диалога)

Версия расширяет механизм V10.8.588.

## Русский

**Поддерживаются:** обычная MPEG-TS/IPTV-плитка (DVB, HTTP, SRT, UDP, HLS, транскодирование), WISI EPG, MPTS.

**Как перенести:**
1. В веб-интерфейсе исходного сервера правый клик по плитке → **Копировать плитку**.
2. На целевом сервере правый клик по свободному месту панели → **Вставить плитку**.
3. При доступном системном Clipboard API в HTTPS-панели плитка создаётся **сразу**, без окна импорта или подтверждения.
4. В HTTP-панелях браузер запрещает чтение буфера программой. Нажмите **Ctrl+V** по панели (когда курсор не в поле ввода), и плитка вставится непосредственно, без окна.
5. Новая плитка **остановлена**, автозапуск выключен. Откройте редактор, настройте интерфейс, тюнер, CAM, XMLTV и порты и только потом запустите.

**MPTS:** откройте меню MPTS на исходном сервере, нажмите правой кнопкой по строке нужного выхода. После переноса MPTS появляется в списке MPTS на целевом сервере. Не найденные локальные stream_id выделяются предупреждением в редакторе, запуск запрещён до перепривязки каналов.

**Безопасность:** передаются настройки конфигурации плитки, но не учётные данные, закрытые ключи, SRT passphrase/streamid, ключи HLS и глобальная конфигурация. Локальные интерфейсы и CAM сбрасываются. Из URL удаляются userinfo и query/fragment. Секреты в нестандартных URL-путях могут остаться — их необходимо исключить до копирования.

Работающие каналы и MPTS не останавливаются при переносе. У каждой вставленной плитки новый ID.

## English

Right-click a stream or WISI EPG tile, or an MPTS row in the MPTS dialog, and choose **Copy tile**. On the target dashboard, right-click free space and choose **Paste tile**. On HTTPS the new tile is created immediately without a dialog. On HTTP use **Ctrl+V** outside input fields. Imported tiles are stopped and autostart is disabled. Rebind local tuners, interfaces, CAM, XMLTV and MPTS service IDs before enabling.

## Hrvatski

Desni klik na pločicu (ili MPTS redak) → **Kopiraj pločicu**. Na drugom poslužitelju desni klik → **Zalijepi pločicu**. Na HTTPS-u se pločica odmah stvara bez dijaloga. Na HTTP-u pritisnite **Ctrl+V** izvan polja za unos. Nova pločica je zaustavljena; automatsko pokretanje je isključeno. Prije pokretanja prilagodite tuner, mrežu, CAM, XMLTV i MPTS kanale.

## Deutsch

Rechtsklick auf eine Kachel (oder MPTS-Zeile) → **Kachel kopieren**. Auf dem Zielserver Rechtsklick → **Kachel einfügen**. Mit HTTPS wird die Kachel ohne Dialog erstellt. Bei HTTP **Strg+V** außerhalb von Eingabefeldern drücken. Die neue Kachel bleibt gestoppt, Autostart ist deaktiviert. Tuner, Netzwerk, CAM, XMLTV und MPTS-Kanäle vor dem Start anpassen.

## Проверка

    git switch main
    git pull --ff-only origin main
    node tests/web/test_tile_clipboard_v588.js
    node tests/web/test_tile_clipboard_v589.js
    node tests/web/test_wisi_epg_tile_controls_v582.js
    cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
    cmake --build build-release --parallel 2
    ctest --test-dir build-release -R tile_clipboard_safe_transfer --output-on-failure
