# DVBStreamer5 V10.8.586 — WISI EPG CBR and hover preview

## RU — постоянный битрейт WISI

Причина прерываний: старый EIT-only UDP выход передавал датаграммы только при появлении новых EIT-секций; в паузах WISI не получал транспортных пакетов и показывал нулевой битрейт.

В V586 выход для включенной EPG-плитки с каналами отправляет непрерывный MPEG-TS CBR: 7×188 байт на UDP датаграмму с равномерным темпом и заполнением NULL-пакетами PID 0x1FFF, сохраняя EIT PID 0x0012 и его CRC. Генерация EIT выполняется в отдельном потоке, чтобы загрузка XMLTV не прерывала транспорт. Ограниченная очередь сохраняет цельность EIT-порций. Нажатие Стоп прекращает и EIT, и NULL.

Настройка «Постоянный битрейт TS, кбит/с» в меню Вещание → EPG UDP для WISI: 512–12000, по умолчанию 3000. Значение отображается на плитке. Переводы RU/EN/HR/DE.

## RU — EPG при наведении

При наведении на WISI EPG-плитку появляется EPG-подсказка с Сейчас и Далее для первых четырёх назначенных каналов, в том числе внешних XMLTV. Большее число каналов показывается счетчиком. Серверный endpoint GET /api/epg-udp/preview?output_id=... отдаёт только первые восемь каналов, их SID, имена и две передачи без XMLTV URL или секретов. Данные запрашиваются только при наведении и кешируются в браузере на 30 секунд; при уходе курсора устаревший запрос не раскрывает подсказку.

## EN / HR / DE

Continuous CBR WISI EPG input with configurable bitrate, NULL padding, bounded EIT queue and hover Now/Next programme previews. Localized labels: English, Croatian and German in addition to Russian.

## Границы проверки

CI эмулирует UDP-получатель WISI на 127.0.0.1 и проверяет отсутствие длинных пауз, PID, continuity и Stop. Реальную совместимость с WISI Chameleon требуется проверять на оборудовании. EIT-only TS не содержит PAT/PMT/SDT/PCR, поэтому стабильная входная скорость не гарантирует отображение EPG.

## Команды из корня исходников

~~~bash
git switch main
git status --short
git fetch origin
git pull --ff-only origin main
grep kProgramVersion src/AppVersion.h
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel 2
node tests/web/test_wisi_epg_hover_v586.js
node tests/web/test_wisi_epg_cbr_config_v586.js
ctest --test-dir build-release -R 'epg_udp_(eit|cbr)_wisi' --output-on-failure
bash scripts/make_portable_linux_bundle.sh --build-dir build-release --with-oscam --archive
~~~
