# DVBStreamer5 Remote 1.4 — Official Android app

[Скачать APK / Download APK](https://github.com/Monkipnet/DVBStreamer5/raw/refs/heads/main/android-client/release/DVBStreamer5-Remote-1.4.apk)

[Исходники / Source code](https://github.com/Monkipnet/DVBStreamer5/raw/refs/heads/main/android-client/release/DVBStreamer5-Remote-1.4-source.zip)

[Инструкция](release/Инструкция.md) · [SHA-256](release/SHA256SUMS.txt)

Официальное приложение DVBStreamer5 для Android 8.0+ и планшетов. Интерфейс: Русский / English / Deutsch. Язык выбирается в настройках приложения и включается в экспорт настроек. Для обновления версий 1.2/1.3 установите APK поверх существующего приложения: подпись и идентификатор сохранены.

Official DVBStreamer5 app for phones and tablets running Android 8.0+. Select Language in App settings to choose System default, Русский, English or Deutsch. The web panel's own language is selected separately and depends on your server version.

Offizielle DVBStreamer5-App für Smartphones und Tablets mit Android 8.0 oder höher. Unter App-Einstellungen → Sprache stehen Systemsprache, Русский, English und Deutsch zur Auswahl. Die Sprache der Weboberfläche wird dort selbst gewählt und hängt von der Serverversion ab.

Includes server selection, separate port settings, auto login with Android Keystore protection, encrypted settings import/export and the original user-provided icon.

Validated: complete 116-string translations for all three languages; 18 language and backup compatibility assertions; existing address, archive encryption and mock-login tests; APK signatures and alignment. Real-device UI and live server operation have not yet been tested.

## Исправление меню в версии 1.4

Меню веб-панели ограничено видимой областью экрана и прокручивается. Пересчёт при раскрытии подменю, повороте экрана, изменении масштаба текста и появлении клавиатуры. В нативном меню длинные названия переносятся; формы входа и пароля архива прокручиваются.

Проверены 30 браузерных сценариев с актуальной разметкой сервера: телефон и планшет, портретная и альбомная ориентация, увеличенный текст, раскрытые подменю и формы. Исправление встроено в APK и не требует изменения установленного сервера. Проверка на физическом Android-устройстве пока не выполнена.

Menu sizing and scrolling fixed for small screens, landscape, larger text and keyboard visibility. Native menu labels wrap, and credential dialogs scroll.

Menüs bleiben im sichtbaren Bildschirmbereich und sind scrollbar. Lange Menütexte werden umgebrochen; Anmelde- und Passwortdialoge lassen sich scrollen.
