# WISI EPG tile controls — V10.8.582

## RU — эксплуатация

Отдельная WISI EPG-плитка теперь оформлена как потоковая: цветовое состояние, кнопки Старт/Стоп и Ред., галочка Автостарт.

Старт включает UDP EIT-генератор для назначенных каналов, Стоп выключает. Медиа-потоки DVB/IPTV не меняются. Ред. открывает существующий редактор списка EPG-каналов с XMLTV и SID/TSID/ONID. Автостарт определяет запуск после рестарта программы, отдельно от ручного Старт/Стоп.

Для пустой плитки после запуска отображается Ожидание каналов; без каналов UDP EIT-пакеты не передаются.

Старые конфигурации без auto_start мигрируют из enabled: прежде активные EPG-выходы продолжат автоматически запускаться. После этого флаги независимы.

Все новые подписи переведены на русский, английский, хорватский и немецкий (RU/EN/HR/DE).

## Implementation

- Config persists runtime enabled and restart auto_start separately; load applies enabled = auto_start with legacy fallback.
- POST /api/epg-udp/action updates one WISI output (start, stop, autostart), preserves channel mappings, validates action, saves with rollback on failure.
- GET /api/epg-udp/status returns current config, so checkbox updates do not interrupt running UDP outputs.
- /api/state contains compact auto_start/enable/channel_count summary. WISI controls do not call media-stream actions.
- Start/Stop reconfigures the existing shared EIT worker; real packet delivery to WISI still requires hardware verification.
- Emulation regression: node tests/web/test_wisi_epg_tile_controls_v582.js (main Linux GitHub Actions).

## Build commands from repository root

git switch main
git status --short
git fetch origin
git pull --ff-only origin main
grep kProgramVersion src/AppVersion.h
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel 2
node tests/web/test_wisi_epg_tile_controls_v582.js
bash scripts/make_portable_linux_bundle.sh --build-dir build-release --with-oscam --archive
VERSION=$(sed -n 's/.*kProgramVersion = "\([^"]*\)";.*/\1/p' src/AppVersion.h)
(cd dist && sha256sum -c "DVBStreamer5-${VERSION}-linux-x86_64.tar.gz.sha256")
