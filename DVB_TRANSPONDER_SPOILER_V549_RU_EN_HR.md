# DVBStreamer5 V10.8.549 — DVB transponder disclosure and editing (RU / EN / HR)

## Русский

**Меню → DVB / Транспондеры** и группы транспондеров на главной панели теперь используют ту же схему, что и **Server Cluster**:

- **Название транспондера слева** — единственное место, где клик запускает редактирование выбранного TP.
- **Большая стрелка ▼ / ▲ по центру строки** — раскрывает или сворачивает спойлер DVB. Клик на пустое место строки действует так же.
- **Уровень/качество сигнала и число активных каналов справа** — только показатели; клик по ним раскрывает/сворачивает группу, а не запускает редактор.

Перед редактированием действующего транспондера остаётся подтверждение остановки связанных плиток. После подтверждения останавливаются **только связанные с TP каналы**, сервер ожидает освобождения frontend, затем открывает редактор с выбранными транспондером, адаптером и frontend. Отмена не останавливает каналы. Раскрытие спойлера не отправляет запросов остановки, не вызывает сканирование и не меняет конфигурацию.

Поддерживаются RU / EN / HR и тёмная/светлая темы. Не меняются UDP/SRT/HTTP/HLS выходы, настройки CA/TSDecrypt, группы сетевых потоков и режимы вещания.

**Регрессионные проверки:** `tests/web/test_dvb_spoiler_v549.js`, `tests/web/test_transponder_editor_v539.js`, `tests/web/test_transponder_scan_v538.js`. GitHub Actions запускает все три теста, затем CMake/CTest и portable-сборку.

## English

DVB transponder groups use the same layout as **Server Cluster**. **Only the transponder name (left)** opens editing; the **large centered ▼ / ▲ arrow**, blank header area and right-side signal/quality/status indicators expand or collapse the group. Clicking the arrow or status never opens editing or stops streams.

Editing retains the existing confirmation to stop only services belonging to that transponder, waits for its frontend to become available, and opens the editor with the original TP, adapter and frontend selected. Cancel does not stop channels. HTTP/HLS/UDP/SRT streaming, CAM and network input group behavior remain unchanged. All labels are localized into Russian, English and Croatian.

## Hrvatski

DVB grupe transpondera koriste raspored kao **Server Cluster**. **Samo klik na naziv transpondera (lijevo)** otvara uređivanje. **Velika strelica ▼ / ▲ u sredini**, prazan dio zaglavlja i desni pokazatelji signala/kvalitete/stanja otvaraju ili zatvaraju grupu. Klik na strelicu ne otvara uređivanje niti zaustavlja tokove.

Pri uređivanju ostaje potvrda zaustavljanja samo programa povezanih s odabranim transponderom. Nakon oslobađanja frontenda otvara se uređivač s ispravnim TP-om, adapterom i frontendom. Otkazivanje ne zaustavlja kanale. HTTP/HLS/UDP/SRT, CAM i mrežne grupe ostaju nepromijenjeni.

## Сборка / Build / Izgradnja

Выполнять из корня исходников / Run from the source root / Pokrenuti iz korijena izvornog koda:

```bash
git switch main
git pull --ff-only
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
node tests/web/test_dvb_spoiler_v549.js
node tests/web/test_transponder_editor_v539.js
node tests/web/test_transponder_scan_v538.js
ctest --test-dir build --output-on-failure
bash scripts/make_portable_linux_bundle.sh --build-dir build --archive
ls -lh dist/DVBStreamer5-10.8.549-linux-x86_64.tar.gz
```

Установка после тестирования / Installation after tests / Instalacija nakon testiranja:

```bash
sudo bash install_dvbstreamer5_binary.sh --source "$PWD" --build-dir build --mode update --restart --skip-deps
systemctl status dvbstreamer5.service --no-pager
```

Обновление службы временно прерывает вещание. / Restart briefly interrupts streams. / Ponovno pokretanje nakratko prekida emitiranje.
