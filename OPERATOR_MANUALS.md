# DVBStreamer5 — Руководства оператора / Operator Manuals

Практические руководства по работе в **веб-панели DVBStreamer5**, подготовленные в редакции **V10.8.527 (14.11.2026)**. Обе инструкции находятся в **корне репозитория `main`** и открываются прямо на GitHub.

Both editions cover practical operations in the **DVBStreamer5 web interface**, preserving source revision **V10.8.527 (14 November 2026)**. Both guides are stored at the **root of the `main` branch**.

| Язык / Language | Открыть / Open |
|---|---|
| Русский | [DVBStreamer5_Operator_Manual_V10.8.527_RU.md](DVBStreamer5_Operator_Manual_V10.8.527_RU.md) |
| English | [DVBStreamer5_Operator_Manual_V10.8.527_EN.md](DVBStreamer5_Operator_Manual_V10.8.527_EN.md) |

## Что описано / Contents

1. Вход, навигация, плитки каналов / Sign in, navigation, channel tiles
2. Создание и изменение каналов / Adding and editing channels
3. IPTV и DVB-S/S2 / Network and satellite inputs
4. Основные и резервные источники / Primary and backup sources
5. HTTP, HLS, UDP, SRT и другие выходы / Streaming outputs
6. Кодирование видео и аудио / Video and audio transcoding
7. EPG, XMLTV и субтитры / EPG, XMLTV and subtitles
8. OSCam-mini, условный доступ / OSCam-mini, conditional access
9. Мониторинг и абоненты / Monitoring and subscribers
10. Настройки, обновление, ежедневная проверка / Settings, updates, daily checklist

## Актуальные особенности V10.8.531 / Notes for later builds

- **Online** + **Auto Start**: обычная плитка, автоматически запускается вместе с сервером, если разрешено / Normal tile with optional boot autostart.
- **OnDemand**: для HTTP/HLS зрителя; плитка запускается по запросу, а после ухода последнего зрителя останавливает сервисный конвейер / HTTP/HLS request activates the tile; after all viewers leave, the service pipeline shuts down after the idle period.
- **HLS**: в V530 для транскодируемого OnDemand с короткими чанками первоначальный плейлист появляется после трёх завершённых сегментов, чтобы у плеера был запас медиа / V530 pre-fills three closed segments for selected transcoded OnDemand HLS, reducing near-live-edge stalls.
- **DVB Warm Standby**: после остановки плитки общий фронтенд может оставаться настроенным; `STANDBY` — не запущенный канал / Warm standby may retain tuner lock without keeping the channel, transcoder, CA or HLS pipeline running.
- **Restart Program**: в V531 планируется отдельным заданием systemd и **перезапускает весь сервер**, прерывая все выходы / Full daemon restart; schedule only in a maintenance window.

Для технических подробностей см. [HLS V530](docs/V10_8_530_ONDEMAND_HLS_PREFILL_VLC.md) и [Restart/OnDemand V531](docs/V10_8_531_PROGRAM_RESTART_ONDEMAND_IDLE.md).

These Markdown files were transcribed from the previously prepared 14-page RU/EN PDF operator manuals. PDF originals remain separate source artifacts; these links point to the full textual manuals in Git.
