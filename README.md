### V10.8.233 — исправление компиляции захвата H.264

- Исправлена регрессия V10.8.232 в `src/media/NativeTranscoderPipeline.cpp`: восстановлен расчёт `videoDuration90k` (`90000.0 / fps`), удалены посторонние токены после диагностической функции захвата. Эта ошибка ранее приводила к синтаксическим ошибкам и множеству вторичных сообщений C++ компилятора.
- Сохранён раздельный захват H.264 по `streamId` и PID без обрезки отдельного access unit.
- Обычные NVDEC/NVENC/Ittiam, OSCam-mini, паковка TS и A/V mux этим коммитом не изменялись.
- Проверка: `git pull --ff-only origin main`, затем `cmake --build build-release --parallel 2` и `strings build-release/DVBStreamer5 | grep 'split_per_stream=1'`. После подтверждения компиляции собирайте portable обычным скриптом.

### V10.8.233 — исправление многоканального захвата H.264 для FFmpeg

- `DVBSTREAMER5_DUMP_H264_ES=/tmp/dazn-input.h264` теперь создаёт **отдельные** файлы для каждой пары `streamId + video PID` (например, `/tmp/dazn-input.sat-....pid-2047.h264`), не смешивая два канала в один некорректный H.264 elementary stream. В журнале показывается точный путь `NATIVE AVC ES DUMP open path=...`.
- Ограничение **64 МиБ на канал**, без обрыва посередине H.264 access unit. Файл `*.h264.index.csv` сохраняет метки PTS/DTS и идентификатор канала/PID.
- Диагностический режим по умолчанию выключен и не меняет обработку NVDEC, NVENC, Ittiam, MPEG-TS или A/V. Предыдущий 64 МиБ файл, записанный при двух активных каналах в V10.8.231, **не подходит для заключения о повреждении исходного H.264**, поскольку мог содержать перемешанные access units разных каналов.
- Проверка на сервере: `ls -lh /tmp/dazn-input*.h264`; затем `ffmpeg -hide_banner -loglevel warning -f h264 -i "<один файл PID>" -an -f null -`. Начало захвата может приходиться на середину GOP, поэтому отдельные ошибки в начале файла возможны; оценивайте ошибки после первого IDR.
- Точная ссылка на HLS-выход канала отличается от исходного DVB/IPTV; для независимой диагностики важно сравнить *входные* Annex-B bytes с выходом NVENC и исключить запись двух источников в один файл.


### V10.8.233 — сравнение входного NVENC буфера с программным путём

- Для диагностики зелёных/красных полос: `DVBSTREAMER5_NVENC_SOFTWARE_INPUT_TEST=1` оставляет аппаратный NVDEC/deinterlace/scale, но загружает Y и UV плоскости NV12 в RAM и заполняет NVENC-owned `NV_ENC_CREATE_INPUT_BUFFER` построчно по реальному `nvEncLockInputBuffer.pitch`. Это тот же способ подачи кадра в NVENC, который использует рабочий Ittiam/I420→NV12 путь; входной decoder при этом **не переключается на Ittiam**.
- По умолчанию режим отключён, основной NVDEC→CUDA→NVENC остаётся прежним. Включать только для A/B теста одного канала: передача GPU→RAM повышает загрузку CPU/PCIe и **не является production-оптимизацией**. Выключить после теста.
- Если цвет становится нормальным, исследовать CUDA resource registration/mapped NV12 pitch в zero-copy. Если полосы сохраняются, исследовать NVDEC-decode status=9, входные H.264 access units и исходную NV12 цветность. Без результатов теста исправление цветности **не подтверждено**.

# DVBStreamer5

**DVBStreamer5** — Linux-приложение для приёма, обработки, декодирования, транскодирования и раздачи DVB/IPTV-потоков с собственным native media engine и встроенным Web UI.

Текущая версия: **10.8.233**

> Проект не использует FFmpeg/libav или GStreamer как media framework. Транспорт, MPEG-TS mux/demux, HLS/CMAF, SRT, RTSP, RTMP, remap, EPG и основная логика транскодинга работают внутри DVBStreamer5. Кодеки подключаются как зафиксированные project-local библиотеки.

---

## Содержание

- [Основные возможности](#основные-возможности)
- [Поддерживаемые входы и выходы](#поддерживаемые-входы-и-выходы)
- [DVB-S/S2](#dvb-ss2)
- [Транскодирование](#транскодирование)
- [HLS / CMAF / ABR / DVR](#hls--cmaf--abr--dvr)
- [EPG и DVB EIT](#epg-и-dvb-eit)
- [CA / Newcamd / OSCam-mini](#ca--newcamd--oscam-mini)
- [Web UI и эксплуатация](#web-ui-и-эксплуатация)
- [Быстрая сборка из исходников](#быстрая-сборка-из-исходников)
- [Сборка на Ubuntu 24.04](#сборка-на-ubuntu-2404)
- [Сборка переносимого бинарника на Ubuntu 22.04](#сборка-переносимого-бинарника-на-ubuntu-2204)
- [Сборка на Debian / build-сервере](#сборка-на-debian--build-сервере-без-установки-gpu-драйверов)
- [Установка на обычную Linux-систему](#установка-на-обычную-linux-систему)
- [Создание portable-пакета](#создание-portable-пакета)
- [Docker](#docker)
- [Docker Compose](#docker-compose)
- [Обновление из Git](#обновление-из-git)
- [Тесты и диагностика](#тесты-и-диагностика)

---

## Основные возможности

### DVB и транспорт

- Linux DVB-S / DVB-S2.
- Сканирование адаптеров, транспондеров и сервисов.
- LOCK, уровень сигнала, качество и CNR.
- Несколько сервисов одного транспондера через общий DVB frontend.
- DiSEqC.
- Настраиваемые LNB LOF1/LOF2/SLOF.
- Unicable EN50494 и JESS/EN50607.
- DVB-S2 Stream ID / MIS.
- PID-фильтрация или полный transport stream.
- Remap SID/PID.
- Генерация PAT/PMT/SDT.
- Отдельные MPTS-выходы с выбранными сервисами.
- CBR pacing и целевой MPEG-TS bitrate.
- Основной и резервный input.
- Online и OnDemand режимы.

### IPTV

- UDP MPEG-TS.
- RTP MPEG-TS.
- HTTP/HTTPS MPEG-TS.
- HLS MPEG-TS.
- HLS fMP4/CMAF.
- SRT caller/listener.
- RTSP input/output.
- RTMP/RTMPS input/output.
- File input.
- VDR input.
- Встроенный test pattern.
- Дополнительные выходы для одного потока.

### Native transcoding

- H.264/AVC.
- H.265/HEVC.
- Copy mode.
- CPU codecs.
- NVIDIA NVENC.
- Intel QSV / oneVPL.
- Intel/generic VAAPI.
- NVIDIA NVDEC -> CUDA -> NVENC zero-copy, когда поддерживается системой.
- Для чересстрочного NVIDIA-видео обработка полей в NVDEC исправлена в V10.8.233; при проблемах с конкретным драйвером можно отключить GPU decode, сохранив NVENC encoding: `DVBSTREAMER5_DISABLE_NVIDIA_ZERO_COPY=1` в окружении сервиса.
- При `transcode_video_decoder=ittiam` или `openh264` выбранный CPU decoder имеет приоритет даже с `video_encoder=nvenc` (диагностический fallback).
- Экспериментальная A/B диагностика NVIDIA: `DVBSTREAMER5_NVDEC_SAFE_COPY=1` включает копирование NV12 между двумя CUDA device surfaces (без RAM/CPU), чтобы исключить проблему прямой регистрации NVDEC mapping в NVENC; по умолчанию отключена. `NATIVE NVDEC DECODE ERROR` показывает, что GPU сообщил о повреждённом входном кадре.
- Диагностика V10.8.233 для 1080i: `DVBSTREAMER5_NVDEC_VPP_MODE=adaptive|bob|weave` задаёт NVDEC VPP deinterlace (по умолчанию `adaptive`); `DVBSTREAMER5_NVDEC_VPP_PROBE=1` выводит флаги полей в журнал. При выключенном деинтерлейсе в настройках плитки используется `weave`, независимо от переменной. На уже работающих системах `bob` и `weave` следует проверять только в тестовом потоке, сравнивая артефакты и плавность движения.
- Дополнительный тест V10.8.233 для зелёных кадров: `DVBSTREAMER5_NVDEC_DIAG_CHROMA=neutral` принудительно включает GPU safe copy и заменяет только NV12 UV-плоскость на нейтральные значения U=V=128 **на GPU**. Изображение в этом режиме намеренно чёрно-белое; если цветные полосы исчезают, вероятно повреждена UV-плоскость. Режим не исправляет цвет и по умолчанию отключён; удалите переменную после диагностического теста.

- Исправление V10.8.233: планировщик `NativeTranscoderPipeline::muxWorkerLoop` больше не делает повторные короткие/просроченные `pthread_cond_clockwait`, когда один поток A/V превышает допустимое опережение в 500 мс. Вместо этого ожидает поступления данных от отстающего потока; для допустимого временного удержания ждёт точного срока. Ограничения 150/500 мс, логика сброса очередей и порядок mux остаются прежними. Это исправление нагрузки CPU, не исправление цветных полос NVIDIA; NVDEC/UV ещё требуют отдельной проверки.
- Диагностика V10.8.233: `DVBSTREAMER5_NVDEC_UV_PROBE=1` выводит статистику первых четырёх и каждого 500-го отображённого кадра: небольшие (не более 128 байт на область) пробы Y и двух UV-строк с CUDA device surface; с `DVBSTREAMER5_NVDEC_SAFE_COPY=1` также сверяет UV в целевом CUDA-буфере. Используйте вместе с `DVBSTREAMER5_NVDEC_DIAG_CHROMA` **отключённым**, иначе результат намеренно обесцвечен. Пробник включать временно: он создаёт редкие малые GPU→CPU чтения для диагностики и может слегка влиять на производительность.
- Native scaling.
- Deinterlace.
- Resolution, FPS, bitrate, GOP, B-frames, reference frames.
- Preset, profile, level и AQ.
- Выбор H.264 decoder для каждой плитки:
  - Auto;
  - Ittiam libavc;
  - OpenH264.
- Копирование/вставка настроек транскодирования между плитками.

### Audio

Native pipeline включает:

- AAC-LC через FDK-AAC;
- MPEG-1 Layer II / MP2;
- MP3 encode через LAME, если development package был доступен при сборке;
- AC-3 decode;
- audio copy для совместимых потоков.

### EPG

- DVB EIT ingest.
- External XMLTV.
- Несколько XMLTV-источников.
- Ручное обновление.
- Автоматическое daily/weekly обновление.
- Выбор XMLTV channel для каждой плитки.
- Название и описание передач.
- Режимы Auto / DVB / XMLTV.
- Опциональная генерация DVB EIT PID 0x12 в выходной MPEG-TS.
- Present/Following и schedule EIT.
- До 7 дней EPG для выходного EIT.

### DVB subtitles

- Опциональный passthrough DVB bitmap subtitles.
- Выбор языка.
- Выбор конкретного PMT subtitle track.
- Сохранение subtitle PES и PMT descriptors при транскодировании.

### Управление и мониторинг

- Встроенный Web UI.
- Русский и английский интерфейс.
- Плитки потоков и перезапуск отдельной плитки.
- Группировка DVB-плиток и отображение сигнала.
- Input/Output bitrate.
- CPU/GPU и runtime telemetry.
- Scheduled restart активных потоков.
- Telegram notifications.
- Subscriber/IP filtering.
- Опциональная фильтрация UDP/RTP ingress по выбранным интерфейсам.
- HLS access key через HTTP header или query parameter.
- HLS archive/timeshift.
- Multibitrate/ABR HLS.

---

## Поддерживаемые входы и выходы

| Протокол / источник | Input | Output |
|---|:---:|:---:|
| DVB-S/S2 | ✅ | — |
| UDP MPEG-TS | ✅ | ✅ |
| RTP MPEG-TS | ✅ | ✅ |
| HTTP/HTTPS MPEG-TS | ✅ | HTTP |
| HLS MPEG-TS | ✅ | ✅ |
| HLS CMAF/fMP4 | ✅ | ✅ |
| SRT | ✅ | ✅ |
| RTSP | ✅ | ✅ |
| RTMP/RTMPS | ✅ | ✅ |
| File | ✅ | — |
| VDR | ✅ | — |
| Test Pattern | ✅ | — |
| MPTS | source/remap | ✅ |

SRT используется из **vendored SRT 1.5.7**, который собирается статически с OpenSSL EVP.

---

## DVB-S/S2

Пример URI, который формирует Web UI:

```text
dvb://satellite?adapter=0&frontend=0&frequency_khz=12383000&symbol_rate=27500&polarity=H&delivery_system=dvb-s2&modulation=8psk&fec=3%2F4
```

Дополнительные DVB параметры:

```text
diseqc_source
lnb_lof1_khz
lnb_lof2_khz
lnb_slof_khz
unicable_mode
unicable_user_band
unicable_frequency_khz
unicable_position
unicable_pin
stream_id
pids
```

Поддерживаемые Unicable modes:

```text
none
en50494
en50607
```

---

## Транскодирование

### H.264 decoder

```text
auto
ittiam
openh264
```

### Video encoder

```text
auto
cpu
nvenc
qsv
vaapi
```

Hardware backend доступен только если:

1. нужные headers/libs были найдены во время CMake configure;
2. на runtime-сервере установлен совместимый driver;
3. GPU поддерживает выбранный codec/profile.

### Native codec sources

`scripts/vendor_native_codecs.sh` загружает pinned source snapshots:

- OpenH264;
- Ittiam libavc;
- libde265;
- Kvazaar;
- FDK-AAC;
- Ittiam libmpeg2;
- PL_MPEG.

Static prefix:

```text
third_party/native-codecs-prefix
```

Production-сборка не использует system FFmpeg/GStreamer codecs.

---

## HLS / CMAF / ABR / DVR

Поддерживаются:

- MPEG-TS HLS;
- fMP4/CMAF;
- segment duration;
- live segment count;
- AES-128;
- SAMPLE-AES;
- CMAF cbcs;
- `EXT-X-MAP`;
- `init.mp4`;
- `.m4s`;
- multibitrate/ABR HLS;
- HLS DVR/archive;
- timeshift;
- retention в часах;
- отдельный archive path.

Default archive path:

```text
/var/lib/dvbstreamer5/archive
```

---

## EPG и DVB EIT

EPG может использовать:

1. DVB EIT;
2. XMLTV;
3. Auto — DVB с fallback на XMLTV.

XMLTV source поддерживает:

```text
daily
weekly
```

При включении DVB EIT output приложение формирует EIT present/following и schedule для итогового output SID/TSID/ONID.

---

## CA / Newcamd / OSCam-mini

Проект собирает loadable CA backend:

```text
dvbstreamer5-ca-newcamd.so
```

Стандартный каталог:

```text
/opt/DVBStreamer5/ca-plugins
```

Также собирается vendored **OSCam-mini** для сценариев с локальным card-server/reader.

Стандартные пути:

```text
/opt/DVBStreamer5/oscam-mini/oscam-mini
/etc/dvbstreamer5/oscam-mini
```

OSCam-mini не требуется для обычных незашифрованных IPTV/DVB потоков.

---

## Web UI и эксплуатация

Default Web UI:

```text
port: 9000
login: admin
password: admin
```

После первой установки **обязательно смените пароль**.

При systemd deployment:

```text
/etc/dvbstreamer5/dvbstreamer5-config.json
/etc/dvbstreamer5/dvbstreamer5-subscribers.json
/etc/dvbstreamer5/dvbstreamer5-ui.key
```

Runtime layout:

```text
/opt/DVBStreamer5/bin/DVBStreamer5
/opt/DVBStreamer5/ca-plugins/
/opt/DVBStreamer5/oscam-mini/
/etc/dvbstreamer5/
/var/lib/dvbstreamer5/
```

---

# Сборка из исходников

## Быстрая сборка из исходников

```bash
git clone https://github.com/Monkipnet/DVBStreamer5.git
cd DVBStreamer5

./install_deps.sh

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build --parallel 2
```

Результат:

```text
build/DVBStreamer5
build/ca-plugins/dvbstreamer5-ca-newcamd.so
build/oscam-mini/oscam-mini
```

Проверка версии:

```bash
grep kProgramVersion src/AppVersion.h
```

---

## Сборка на Ubuntu 24.04

Рекомендуемый вариант для production-сервера.

```bash
sudo apt update
sudo apt install -y git

git clone https://github.com/Monkipnet/DVBStreamer5.git
cd DVBStreamer5

./install_deps.sh
```

`install_deps.sh` устанавливает build dependencies, подготавливает locked SRT/native codecs и пытается включить доступные hardware backends. На Ubuntu с NVIDIA скрипт может установить рекомендуемый driver.

После первой установки NVIDIA driver может потребоваться reboot:

```bash
sudo reboot
```

Сборка:

```bash
cd DVBStreamer5

rm -rf build

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build --parallel "$(nproc)"
```

На сервере с небольшим количеством RAM:

```bash
cmake --build build --parallel 2
```

Во время configure проверяйте нужные строки:

```text
Native Intel VAAPI backend: enabled
Native Intel QSV/VAAPI backend: enabled via oneVPL
Native NVIDIA NVENC backend: enabled
Native NVIDIA NVDEC->CUDA->NVENC zero-copy backend: enabled
```

---

## Сборка переносимого бинарника на Ubuntu 22.04

Если release должен запускаться на Ubuntu 22.04 и более новых совместимых Ubuntu, собирайте его на **самой старой целевой системе**.

Бинарник, собранный на Ubuntu 24.04, может требовать более новую glibc и не запускаться на Ubuntu 22.04.

Установка обязательных build packages:

```bash
sudo apt update

sudo apt install -y \
  git build-essential cmake pkg-config python3 nodejs \
  curl ca-certificates binutils nasm autoconf automake libtool \
  libpcsclite-dev libssl-dev libcrypt-dev libmp3lame-dev \
  libcurl4-openssl-dev libxml2-dev zlib1g-dev
```

VAAPI/oneVPL headers, если доступны:

```bash
sudo apt install -y libva-dev || true

if apt-cache show libvpl-dev >/dev/null 2>&1; then
  sudo apt install -y libvpl-dev
fi
```

Подготовка source tree:

```bash
git clone https://github.com/Monkipnet/DVBStreamer5.git
cd DVBStreamer5

./scripts/vendor_srt_source.sh
./scripts/vendor_native_codecs.sh

DVBSTREAMER5_CODEC_JOBS=2 \
  ./scripts/build_native_codecs.sh
```

Сборка:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build --parallel 2
```

Проверка:

```bash
./scripts/audit_runtime_deps.sh build/DVBStreamer5
ldd build/DVBStreamer5
```

### Важно

DVBStreamer5 не является полностью статическим Linux executable.

В проект статически входят SRT и native codec libraries, но остаются обычные system runtime dependencies: glibc, libstdc++, OpenSSL, libcurl, libxml2, zlib и optional hardware/audio runtimes.

Для максимальной совместимости:

> Собирайте release на самой старой системе, на которой он должен запускаться.

---

## Сборка на Debian / build-сервере без установки GPU-драйверов

Для отдельного build-server можно не запускать автоматическую установку GPU driver.

```bash
sudo apt update

sudo apt install -y \
  git build-essential cmake pkg-config python3 nodejs \
  curl ca-certificates binutils nasm autoconf automake libtool \
  libpcsclite-dev libssl-dev libcrypt-dev libmp3lame-dev \
  libcurl4-openssl-dev libxml2-dev zlib1g-dev

for p in libva-dev libvpl-dev; do
  if apt-cache show "$p" >/dev/null 2>&1; then
    sudo apt install -y "$p"
  fi
done
```

Затем:

```bash
git clone https://github.com/Monkipnet/DVBStreamer5.git
cd DVBStreamer5

./scripts/vendor_srt_source.sh
./scripts/vendor_native_codecs.sh

DVBSTREAMER5_CODEC_JOBS=2 \
  ./scripts/build_native_codecs.sh

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build --parallel 2
```

Если требуется только проверить hardware dependency setup без автоматической установки GPU driver:

```bash
sudo ./scripts/install_native_hw_backends.sh --no-driver-install
```

После изменения dev packages выполняйте clean configure:

```bash
rm -rf build

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build --parallel 2
```

---

## WSL / Windows workspace

Если source tree находится на `/mnt/c` или `/mnt/d`, CMake build tree лучше держать в Linux filesystem:

```bash
cmake -S /mnt/d/PROJECTS/DVBStreamer5 \
  -B "$HOME/DVBStreamer5-build" \
  -DCMAKE_BUILD_TYPE=Release

cmake --build "$HOME/DVBStreamer5-build" --parallel 2
```

---

# Установка

## Установка на обычную Linux-систему

После успешной Release-сборки:

```bash
sudo mkdir -p \
  /opt/DVBStreamer5 \
  /etc/dvbstreamer5 \
  /var/lib/dvbstreamer5/archive

sudo cmake --install build \
  --prefix /opt/DVBStreamer5 \
  --strip
```

Установить основной systemd unit:

```bash
sudo install -m 0644 \
  packaging/dvbstreamer5.service \
  /etc/systemd/system/dvbstreamer5.service

sudo systemctl daemon-reload
sudo systemctl enable --now dvbstreamer5.service
```

Проверка:

```bash
systemctl status dvbstreamer5.service --no-pager

journalctl -u dvbstreamer5.service \
  -n 100 -o cat --no-pager
```

Web UI:

```text
http://SERVER_IP:9000
```

### OSCam-mini

Если используется локальный OSCam-mini/card-reader:

```bash
sudo /opt/DVBStreamer5/oscam-mini/install_oscam_mini.sh

sudo systemctl status oscam-mini.service --no-pager
```

Не включайте OSCam-mini, если он не нужен.

---

## Переменные окружения

| Variable | Назначение |
|---|---|
| `DVBSTREAMER5_CONFIG_DIR` | каталог основной конфигурации |
| `DVBSTREAMER5_CA_PLUGIN_DIR` | каталог CA plugins |
| `DVBSTREAMER5_OSCAM_CONFIG_DIR` | каталог OSCam-mini configs |
| `DVBSTREAMER5_OSCAM_BINARY` | путь к OSCam-mini executable |

Systemd unit использует:

```text
DVBSTREAMER5_CONFIG_DIR=/etc/dvbstreamer5
DVBSTREAMER5_CA_PLUGIN_DIR=/opt/DVBStreamer5/ca-plugins
DVBSTREAMER5_OSCAM_CONFIG_DIR=/etc/dvbstreamer5/oscam-mini
DVBSTREAMER5_OSCAM_BINARY=/opt/DVBStreamer5/oscam-mini/oscam-mini
```

---

# Portable package

## Создание portable-пакета

После Release-сборки:

```bash
VERSION=$(sed -n 's/.*kProgramVersion = "\([^"]*\)".*/\1/p' src/AppVersion.h)
ARCH=$(uname -m)

STAGE="$PWD/dist/rootfs"
OUT="$PWD/dist/DVBStreamer5-${VERSION}-linux-${ARCH}.tar.gz"

rm -rf "$STAGE"
mkdir -p "$STAGE"

DESTDIR="$STAGE" \
  cmake --install build \
  --prefix /opt/DVBStreamer5 \
  --strip

install -Dm644 \
  packaging/dvbstreamer5.service \
  "$STAGE/etc/systemd/system/dvbstreamer5.service"

mkdir -p \
  "$STAGE/etc/dvbstreamer5" \
  "$STAGE/var/lib/dvbstreamer5/archive"

tar -C "$STAGE" -czf "$OUT" .

echo "$OUT"
```

Проверка пакета:

```bash
tar -tzf "$OUT" | head -n 100

./scripts/audit_runtime_deps.sh build/DVBStreamer5
ldd build/DVBStreamer5
```

### Установка portable package

```bash
sudo tar -xzf DVBStreamer5-*-linux-*.tar.gz -C /

sudo mkdir -p \
  /etc/dvbstreamer5 \
  /var/lib/dvbstreamer5/archive

sudo systemctl daemon-reload
sudo systemctl enable --now dvbstreamer5.service
```

Проверка missing libraries:

```bash
ldd /opt/DVBStreamer5/bin/DVBStreamer5 |
grep 'not found'
```

---

# Docker

Обновлённый Dockerfile использует Ubuntu 24.04 и самостоятельно:

- ставит build dependencies;
- загружает locked SRT;
- загружает pinned codec sources;
- собирает native codec prefix;
- собирает DVBStreamer5;
- собирает Newcamd plugin;
- собирает OSCam-mini;
- создаёт отдельный runtime image.

## Docker build

```bash
git clone https://github.com/Monkipnet/DVBStreamer5.git
cd DVBStreamer5

docker build \
  --build-arg CODEC_JOBS=2 \
  --build-arg BUILD_JOBS=2 \
  -t dvbstreamer5:10.8.233 .
```

Persistent directories:

```bash
mkdir -p docker/config docker/archive
```

## IPTV-only container

На Linux для multicast UDP/RTP/SRT рекомендуется host network:

```bash
docker run -d \
  --name dvbstreamer5 \
  --restart unless-stopped \
  --network host \
  -v "$PWD/docker/config:/etc/dvbstreamer5" \
  -v "$PWD/docker/archive:/var/lib/dvbstreamer5" \
  dvbstreamer5:10.8.233
```

## Docker + DVB adapter

Пример adapter0:

```bash
docker run -d \
  --name dvbstreamer5 \
  --restart unless-stopped \
  --network host \
  --device /dev/dvb/adapter0/frontend0 \
  --device /dev/dvb/adapter0/demux0 \
  --device /dev/dvb/adapter0/dvr0 \
  -v "$PWD/docker/config:/etc/dvbstreamer5" \
  -v "$PWD/docker/archive:/var/lib/dvbstreamer5" \
  dvbstreamer5:10.8.233
```

Посмотреть device nodes:

```bash
find /dev/dvb -maxdepth 3 -type c -ls
```

Если конкретный драйвер создаёт дополнительные `ca*`, `net*` или `demux*`, передайте и их.

## Docker + Intel VAAPI/QSV

Проверка host:

```bash
vainfo --display drm --device /dev/dri/renderD128
```

Запуск:

```bash
docker run -d \
  --name dvbstreamer5 \
  --restart unless-stopped \
  --network host \
  --device /dev/dri/renderD128 \
  --device /dev/dri/card0 \
  -v "$PWD/docker/config:/etc/dvbstreamer5" \
  -v "$PWD/docker/archive:/var/lib/dvbstreamer5" \
  dvbstreamer5:10.8.233
```

## Docker + NVIDIA NVENC

На host нужны proprietary NVIDIA driver и NVIDIA Container Toolkit.

```bash
nvidia-smi
```

Проверка Docker GPU:

```bash
docker run --rm --gpus all \
  nvidia/cuda:12.6.0-base-ubuntu24.04 \
  nvidia-smi
```

Запуск:

```bash
docker run -d \
  --name dvbstreamer5 \
  --restart unless-stopped \
  --network host \
  --gpus all \
  -e NVIDIA_DRIVER_CAPABILITIES=video,compute,utility \
  -v "$PWD/docker/config:/etc/dvbstreamer5" \
  -v "$PWD/docker/archive:/var/lib/dvbstreamer5" \
  dvbstreamer5:10.8.233
```

DVB + NVIDIA: добавьте к этой команде нужные `--device /dev/dvb/...`.

---

## Docker Compose

В репозитории есть `docker-compose.yml`.

```bash
mkdir -p docker/config docker/archive

docker compose up -d --build
```

Логи:

```bash
docker compose logs -f dvbstreamer5
```

Остановка:

```bash
docker compose down
```

Для DVB/Intel/NVIDIA включите нужные device/GPU options в compose-файле.

---

# Обновление из Git

```bash
cd /path/to/DVBStreamer5

git switch main
git fetch origin
git pull --ff-only origin main
```

После обновления:

```bash
rm -rf build

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build --parallel 2
```

Переустановка:

```bash
sudo systemctl stop dvbstreamer5.service

sudo cmake --install build \
  --prefix /opt/DVBStreamer5 \
  --strip

sudo install -m 0644 \
  packaging/dvbstreamer5.service \
  /etc/systemd/system/dvbstreamer5.service

sudo systemctl daemon-reload
sudo systemctl start dvbstreamer5.service
```

`/etc/dvbstreamer5` при этом не удаляется.

---

# Тесты и диагностика

## Native tests

```bash
cmake -S . -B build-tests \
  -DCMAKE_BUILD_TYPE=Release \
  -DDVBSTREAMER5_BUILD_MEDIA_CORE_TESTS=ON \
  -DDVBSTREAMER5_BUILD_MP2_ENCODER_TESTS=ON

cmake --build build-tests --parallel 2

ctest \
  --test-dir build-tests \
  --output-on-failure
```

## Runtime dependency audit

```bash
./scripts/audit_runtime_deps.sh build/DVBStreamer5
```

Audit проверяет отсутствие случайной runtime dependency на FFmpeg/libav, GStreamer, external libsrt и system codec libraries.

## Service

```bash
systemctl status dvbstreamer5.service --no-pager

journalctl -u dvbstreamer5.service \
  -f -o cat
```

## DVB

```bash
find /dev/dvb -maxdepth 3 -type c -ls
```

## NVIDIA

```bash
nvidia-smi
```

## Intel VAAPI

```bash
vainfo --display drm --device /dev/dri/renderD128
```

## oneVPL

```bash
vpl-inspect
```

## Dynamic libraries

```bash
ldd /opt/DVBStreamer5/bin/DVBStreamer5
```

---

# Архитектура native media engine

Production media path включает собственные компоненты DVBStreamer5:

- MPEG-TS framing;
- PAT/PMT/SDT;
- PES;
- PTS/DTS/PCR;
- remap;
- CBR pacing;
- HLS segmenter;
- CMAF;
- SAMPLE-AES;
- SRT;
- RTP;
- RTSP;
- RTMP;
- native transcoder;
- hardware codec backends;
- EPG/DVB EIT;
- DVB subtitle passthrough;
- CA plugin interface.

Дополнительная техническая информация:

```text
NATIVE_MEDIA_ENGINE.md
third_party/NATIVE_CODEC_SOURCE_LOCK.txt
```

---

## Рекомендации для deployment

- **Обычная systemd installation** — лучший вариант для DVB PCIe/USB, PC/SC readers и прямого hardware access.
- **Docker** — удобен для IPTV/SRT/HLS и контейнерной инфраструктуры; DVB/GPU devices нужно явно передавать контейнеру.
- **Portable package с Ubuntu 22.04 build host** — удобен для переноса одного release между несколькими совместимыми Ubuntu-серверами.

Перед обновлением production headend рекомендуется проверить новый release на отдельном сервере или тестовом потоке.
