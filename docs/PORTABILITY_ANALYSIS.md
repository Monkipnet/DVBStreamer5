# TVStreamer5: анализ переноса и переносимости Linux-сборки

Дата анализа: 2026-09-28. Исходная база: `Monkipnet/Tvstreamer_sat`, commit
`f9af8f699455c9f16076734785b224d73eea5b23`.

## Результат сравнения

Текущий репозиторий уже был форком `Tvstreamer_sat`, поэтому полная замена
дерева исходниками upstream привела бы к потере более новой нативной реализации:

- Linux DVB input и сканирование без обязательного `dvbsrc`;
- нативные UDP/RTP input/output, CBR pacing и MPEG-TS remap;
- нативные HLS/MPTS transport paths;
- встроенный MP2 encoder на vendored TwoLAME;
- встроенный Newcamd backend без обязательного внешнего `.so`.

Вместо перезаписи выполнено слияние отсутствующих изменений upstream 203.75:
HEVC/H.265 transcoding, выбор NVENC/Intel/x265, HLS ABR renditions и master
playlist. Исправление CryptoWorks из upstream не копировалось поверх текущего
файла: текущая реализация уже содержит более полную проверку ошибок BIGNUM и
освобождение памяти через `card_done`.

Продукт, бинарник, CMake targets, пути установки и новые конфигурационные файлы
переименованы в `TVStreamer5` / `tvstreamer5`. Старые конфиги
`dvbstreamer5-*` и `tvstreammersat5-*`, каталоги CA plugins и старый ABI symbol
по-прежнему распознаются.

## Что означает «один бинарник для любого Linux»

Целевая платформа проекта — только Linux `x86_64` (`amd64`). Требуются достаточно
новое ядро, DVB/GPU/USB drivers и
совместимый userspace ABI. NVIDIA NVENC, Intel VA/QSV и PC/SC неизбежно зависят
от драйверов целевой машины.

Практическая цель проекта:

1. Один самодостаточный пакет для Linux `x86_64`.
2. Нативный passthrough/remap работает без GStreamer plugins.
3. Транскодирование подключается как опциональный bundled runtime.
4. На целевой машине остаются только kernel drivers, `/dev/dvb`, GPU driver и,
   при работе со smart-card reader, `pcscd`/CCID.

## Карта зависимостей

| Зависимость | Использование | Текущее решение | Рекомендуемая стратегия |
|---|---|---|---|
| libc, pthread, dl | базовый Linux runtime, потоки, optional plugins | системная | glibc baseline для bundle или отдельная musl-сборка |
| Boost headers | Asio/Beast, string helpers, circular buffer | header-only; binary Boost dependency удалена | оставить header-only, затем постепенно заменить небольшие helpers STL-кодом |
| libcrypt | только MD5-crypt для Newcamd login | удалена; алгоритм реализован через уже используемый OpenSSL EVP | готово |
| OpenSSL libcrypto | AES-256-GCM config, DES/Newcamd, HLS key crypto | динамическая | статически линковать либо класть совместимую `libcrypto` в bundle; не копировать отдельные crypto-функции вручную |
| JsonCpp 1.9.8 | config/API/state JSON | встроенный amalgamated static target | готово; системная `libjsoncpp` не нужна, лицензия сохранена в `third_party/jsoncpp/LICENSE` |
| cpp-httplib 0.58.0 | HTTP/HLS input и Telegram HTTPS | встроенная header-only библиотека поверх OpenSSL | готово; системная `libcurl` не нужна, лицензия сохранена в `third_party/cpp-httplib/LICENSE` |
| libdvbcsa 1.1.0 | MPEG-TS CSA descrambling | встроенный SSE2 static target для x86_64 | готово; GPL-2.0-or-later совместима с GPL-проектом, исходник и лицензия сохранены |
| GStreamer/GLib/GIO | transcoding и оставшиеся protocol paths | динамическая + runtime plugins | оставить optional; bundle core/plugins или постепенно заменить passthrough paths нативным кодом |
| TwoLAME | MP2 audio encoding | vendored static target | готово; соблюдать LGPL-2.1 |
| OSCam-mini | smart-card server | vendored отдельный процесс | оставить отдельным companion binary; прямое включение в основной процесс создаёт ABI, isolation и GPL boundary риски |
| hls.js/mpegts.js | browser preview | embedded web assets | готово; Apache-2.0 notices включены |

## Почему нельзя просто скопировать код GStreamer

GStreamer — framework с registry, динамическими plugins, codec modules и
аппаратными backends. Приложение использует не одну функцию, а граф элементов.
Копирование фрагментов исходников не создаст рабочий transcoder и усложнит
лицензирование. Рациональный путь — нативные модули для transport stream и
изолированный bundled GStreamer runtime только для decode/encode.

## Целевая схема поставки

- `TVStreamer5`: основной executable; web assets, transport core, TwoLAME и
  builtin Newcamd находятся внутри него.
- `runtime/`: приватные shared libraries и GStreamer plugins, запускаемые через
  wrapper с собственными `LD_LIBRARY_PATH`, `GST_PLUGIN_PATH` и
  `GST_PLUGIN_SCANNER`.
- `oscam-mini`: отдельный optional companion process.
- один пакет `linux-x86_64-glibc`; ARM/AArch64 не поддерживается.
- сборка на старом поддерживаемом glibc baseline; это надёжнее, чем собирать на
  новом дистрибутиве и ожидать обратной совместимости.

Полностью статическая musl-сборка возможна для режима без GStreamer, GPU и
PC/SC. Для полной версии с plugin-based transcoding предпочтителен relocatable
bundle, а не один статический ELF.

## Порядок дальнейшего удаления runtime-зависимостей

1. Отделить типы транспортных буферов от `GstBuffer` и сделать GStreamer опциональным (libcurl уже удалена).
2. Завершить нативные HTTP/HLS/SRT paths для passthrough и сделать GStreamer
   optional на уровне CMake.
3. Разделить `TVStreamer5-core` и `TVStreamer5-transcode` profiles.
4. Собирать и тестировать bundle в контейнерах с минимальным glibc baseline на
   целевой архитектуре `x86_64`.
5. Проверять каждый release через `scripts/audit_runtime_deps.sh`, smoke tests и
   запуск в чистых Debian/Ubuntu/Fedora/RHEL-compatible containers.

## Лицензии

В корне проекта находится GPL-3.0 text; OSCam-mini также GPL, TwoLAME —
LGPL-2.1, cpp-httplib и JsonCpp — MIT, browser libraries — Apache-2.0. Перед публичной бинарной поставкой
нужно дополнить единый `THIRD_PARTY_NOTICES` точными версиями и способами
линковки. Статическое включение LGPL-компонентов требует соблюдения условий о
релинковке/исходниках. Этот раздел фиксирует инженерные риски и не является
юридическим заключением.
