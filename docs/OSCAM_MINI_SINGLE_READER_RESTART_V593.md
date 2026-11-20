# DVBStreamer5 V10.8.593 — отдельный перезапуск OSCam-mini reader

В разделе **OSCam-mini → Ридеры / Remote CA** у каждого ридера появилась кнопка **«Перезапустить ридер»**. Подтверждение предупреждает о возможной краткой задержке ECM для клиентов этого ридера.

Кнопка использует новый endpoint `POST /api/oscam-mini/reader-action` с JSON `{"action":"restart","label":"..."}`. Требуется обычная администраторская авторизация панели. В отличие от «Перезапуск» наверху страницы, **systemctl restart oscam-mini.service не вызывается**. Конфигурация `oscam.server` не перезаписывается и другие ридеры не перезапускаются.

Запрос проходит в OSCam-mini по локальному Unix SOCK_SEQPACKET `/run/dvbstreamer5-oscam-mini-reader.sock` (только root, 0600, SO_PEERCRED), без HTTP WebIF и открытия TCP-порта. Внутри OSCam выбранный ридер получает собственную штатную команду `ACTION_READER_RESTART`; ответ означает «команда поставлена в очередь», а не «карта уже готова».

**Условия:** поддерживается OSCam-mini, собранный с V593; обе службы `DVBStreamer5` и `oscam-mini` в штатной установке работают под root. При неустановленной новой версии OSCam сервер вернёт понятное сообщение об отсутствии сокета. Изменённые, но не сохранённые имена ридеров не используются: кнопка отправляет исходное сохранённое имя. Неизвестные, отключённые или содержащие опасные символы имена отвергаются.

Новые тексты и предупреждения кнопки доступны на RU, EN, HR, DE.

## Проверки

Команды из корня исходников:

    node tests/web/test_oscam_reader_restart_v593.js
    c++ -std=c++17 -Wall -Wextra -pthread -Isrc tests/test_oscam_reader_control_v593.cpp -o /tmp/oscam-reader-control-v593
    /tmp/oscam-reader-control-v593
    cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
    cmake --build build-release --parallel 2
    ctest --test-dir build-release -R oscam_reader_restart_ipc --output-on-failure

Проверить на реальном сервере: перезапустить один ридер, убедиться в соответствующем событии `DVBStreamer5 OSCam-mini reader ... restart queued` через `journalctl -u oscam-mini`, проверить ECM после восстановления, время отзыва и отсутствие перезапуска процесса и соседних ридеров.
