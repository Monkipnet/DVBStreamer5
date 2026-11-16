# DVBStreamer5 V10.8.550 — Backup IP/SRT PID collision recovery (RU / EN / HR)

## Русский — исправление «из 9 IP-каналов работают только 3»

**Источник диагностики:** журнал сервера `social` от 16.11.2026. Шесть независимых входов SRT caller (`transcode=0, cbr=0`) многократно останавливались с ошибкой `native remap output PID conflicts with another selected-program PID`, затем повторно запускались как `backup to backup / backup_relay_stopped_retry`. Причина подтверждена сообщением из `MpegTsRemapper.cpp`: выходной PID видео/аудио, закреплённый за основным каналом, уже использовался другим PID резервного single-program MPEG-TS.

**MPTS не используется и не требуется.** Каждый IP/SRT-канал имеет собственный вход, PMT и выходы. Общий сетевой интерфейс и общий ресурс не означают один TCP-сокет для девяти независимых SRT-каналов.

### Исправление V550

- Только резервному входу в `StreamManager.cpp` назначается `allowPidConflictFallback=true`. Основной вход, DVB и обычное строгие переназначение PID работают как раньше.
- Если PID видео/аудио резервного источника конфликтует с заданным выходным PID, вместо остановки резервного relay используется **временная передача исходных PID медиа**. Выходные SID и PMT PID остаются заданными, PMT объявляет **фактически передаваемые** PID, а пакеты TS используют те же PID.
- При восстановлении источника без конфликта автоматически возвращается заданное переназначение PID. Изменение состава PMT вызывает публикацию новой версии PMT и discontinuity для обновления декодеров.
- Добавлены диагностические строки `NATIVE BACKUP PID COLLISION fallback=passthrough` и `NATIVE BACKUP PID COLLISION recovered=1`.
- Новые автоматические проверки симулируют девять **независимых single-program SRT/IP входов**, из которых шесть имеют конфликт. Проверяются остановка старого строгого варианта, беспрерывная работа нового remapper, соответствие PAT/PMT фактическим PID, возвращение закреплённых PID после изменения резервного источника. Существующий CTest `native_failover_program_identity` запускается также для проверки первичного failover.

**Внимание:** при резервном источнике с конфликтом PID клиенты получат PMT с временно изменёнными видео/аудио PID. Клиенты, игнорирующие PMT при переключении, могут потребовать переподключения. Если требуется абсолютно неизменный PID при failover, резервный источник должен использовать совместимые PID или потребуется более сложная перенумерация всех конфликтующих ES. Код не делает рискованного смешивания двух потоков в одном PID.

### Сборка и диагностика

```bash
git switch main
git pull --ff-only
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
cmake -S tests/media_core -B build-test-media -DCMAKE_BUILD_TYPE=Release
cmake --build build-test-media -j"$(nproc)"
ctest --test-dir build-test-media -R native_failover_program_identity --output-on-failure
ctest --test-dir build --output-on-failure
bash scripts/make_portable_linux_bundle.sh --build-dir build --archive
ls -lh dist/DVBStreamer5-10.8.550-linux-x86_64.tar.gz
```

После обновления сервера `social` проверить:

```bash
journalctl -u dvbstreamer5.service --since '-15 min' --no-pager -o cat |
  grep -E 'NATIVE BACKUP PID COLLISION|NATIVE STREAM RELAY STOP|NATIVE INPUT FAILOVER' |
  tail -100
```

Наличие `fallback=passthrough` при исчезновении повторных `NATIVE STREAM RELAY STOP` подтвердит срабатывание восстановления. Итог **9/9 реально работающих каналов** требует проверки на сервере `social`; тестовая эмуляция не заменяет эту проверку.

## English

V550 addresses six independent single-program SRT/IP backup relays repeatedly terminating with `native remap output PID conflicts with another selected-program PID`. **No MPTS fan-out is introduced.** Only backup remappers may temporarily pass through source media PIDs when requested primary-compatible output PIDs overlap another selected-program PID. The configured output service ID and PMT PID remain stable, and PMT media PIDs always match actual TS packet PIDs. When a new source PMT no longer conflicts, normal pinned output media PID remapping resumes.

Regression tests emulate 3 unaffected and 6 conflicting independent channels, validate PMT/video/audio, and verify recovery after a source PMT transition. The fallback may change media PIDs during backup; downstream clients must process PMT updates. Main input and DVB remapping remain strict. Production success must be verified on `social`.

## Hrvatski

V550 ispravlja šest neovisnih SRT/IP rezervnih tokova koji se stalno zaustavljaju zbog `native remap output PID conflicts with another selected-program PID`. **MPTS se ne koristi.** Samo rezervni izvor smije privremeno koristiti izvorne audio/video PID-ove ako su traženi izlazni PID-ovi već zauzeti. Izlazni servisni ID i PMT PID ostaju isti, dok PMT točno opisuje stvarne PID-ove paketa. Kad sukob nestane, vraćaju se zadani izlazni PID-ovi. Osnovni ulazi i DVB ostaju strogi.

Automatizirani test simulira devet neovisnih kanala, od kojih šest ima sukob PID-a, te provjerava oporavak nakon promjene PMT-a. Klijenti moraju pratiti ažuriranja PMT-a tijekom rezervnog rada. Stvarni prijam 9/9 mora se potvrditi na poslužitelju `social`.
