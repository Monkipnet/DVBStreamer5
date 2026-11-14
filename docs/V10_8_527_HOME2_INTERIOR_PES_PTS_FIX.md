# DVBStreamer5 V10.8.527 — H.264 source PTS carried across an incomplete AU

## home2 V526 failure, November 14, 2026

The user installed V10.8.526 on home2, but the issue was still present:
- HISTORY, SID 113: `raw_video_vs_assembled_ms` approximately 201,720–201,920
  ms, with `raw_audio_vs_raw_video_ms` around -1 to -2 seconds.
- Sky Sport News, SID 108: `raw_video_vs_assembled_ms` around 55,380–55,560 ms.
- HISTORY audio mux full at 512 samples, cumulative `NATIVE AV MUX DROP`
  count at least 97,100.
- Hardware video producer was active (producer idle generally below 150 ms).
- The original video PES timestamps were fresh; the drift arose in
  `NativeTsDemux` before NVENC. Audio Auto / PID 772 did not repair it.
- At the end of the excerpt the video clock was near raw PES again, with
  HISTORY audio PID now 257 instead of 772. This could indicate channel
  reinitialization; do not infer self-recovery.

## Why V526 failed

`H264PesPtsRecovery::completeAu` only accepted PTS anchors whose recorded
PES byte offset was **exactly zero** at the start of an emitted AU.
It then discarded every anchor with `offset < consumedBytes`. If a
new PES starts **inside the end of the previous incomplete AVC frame**,
the header PTS belongs to the first AU starting in that PES (the
following AU). V526 discarded that PTS as the previous AU was emitted.
If PES boundaries commonly fall inside an AU, *no reanchor occurs*,
and the old erroneous 1800/3600-tick predictor can accumulate minutes
of A/V skew.

## V527 surgical correction

- Keep the newest PES timestamp from inside an emitted AU and **carry
  it to the start of the next assembled AU**; never attach it to the
  preceding unfinished frame.
- Prefer an actual PES PTS at the exact next AU boundary over an
  older interior-carried anchor, if both are present.
- Retain the V526 500-ms drift-detection threshold, source-clock lock,
  33-bit wrap-safe signed arithmetic, and DTS semantics.
- `NATIVE DEMUX RAW AV CLOCK` now includes
  `h264_interior_pes_carried_total` so home2 can verify whether this
  code path is actually being exercised.
- Added regression cases to `h264_pes_pts_recovery`:
  1000 consecutive 25-fps AUs whose source PES always begins halfway
  through the previous AU, with an incorrect 1800-tick frame predictor;
  and a case where exact-boundary PES PTS must supersede a carried
  interior anchor.
- Local C++17 standalone recovery helper regression passed with
  GCC `-O2 -DNDEBUG -Wall -Wextra -Wpedantic -Werror` and the same
  flags without NDEBUG. Field logs dated 2026-11-14 now confirm short
  stable runs on home2 and the Intel VAAPI / CPU OpenH264 tests on
  svettranscoding (details below). Full CI result, output TS/PCR/CC
  inspection, long-term playback and all-encoder regression are **not
  yet confirmed**.
- No changes to DVB CA, OSCam, TSDecrypt, NVDEC/NVENC, A/V mux lead
  or depth limits, audio codecs, EPG, HLS/SRT/UDP/HTTP outputs.

## Build and CTest from source root

```bash
git switch main
git pull --ff-only origin main
bash scripts/vendor_srt_source.sh
bash scripts/vendor_native_codecs.sh
DVBSTREAMER5_CODEC_JOBS=2 bash scripts/build_native_codecs.sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel 2
ctest --test-dir build-release --output-on-failure
ctest --test-dir build-release -R h264_pes_pts_recovery --output-on-failure
bash scripts/make_portable_linux_bundle.sh \
  --build-dir build-release --with-oscam --archive
(cd dist && sha256sum -c DVBStreamer5-10.8.527-linux-x86_64.tar.gz.sha256)
```

GitHub Actions `build-main-linux.yml` packages the versioned portable
bundle including OSCam and TSDecrypt. Confirm the exact SHA/run and
successful tests before downloading/deploying. For Ubuntu 18.04 check
glibc compatibility; the hosted runner builds on Ubuntu 24.04.

## home2 after installing V527

Do not install without an OSCam/config backup and a maintenance window:
the portable installer explicitly stops DVBStreamer5 and OSCam-mini.
Run:

```bash
cat /opt/DVBStreamer5/VERSION
PID=$(systemctl show -p MainPID --value dvbstreamer5.service)
journalctl -u dvbstreamer5.service --since '-15 min' \
  --no-pager -o short-iso-precise |
grep -F "run.sh[$PID]:" |
grep -E 'NATIVE AVC PES PTS RESYNC|NATIVE DEMUX RAW AV CLOCK|NATIVE AV MUX stream_id=|NATIVE AV MUX PEER STALL|NATIVE AV MUX DROP' \
  > /tmp/dvb-v527-home2-avclock.log

grep 'NATIVE DEMUX RAW AV CLOCK' /tmp/dvb-v527-home2-avclock.log | tail -n 20
grep 'NATIVE AVC PES PTS RESYNC' /tmp/dvb-v527-home2-avclock.log | tail -n 20
grep 'NATIVE AV MUX stream_id=' /tmp/dvb-v527-home2-avclock.log | tail -n 20
grep 'NATIVE AV MUX DROP' /tmp/dvb-v527-home2-avclock.log | tail -n 20
```

Expected in a fresh process: `h264_interior_pes_carried_total` is
nonzero/increasing when PES starts in the body of unfinished AUs,
`raw_video_vs_assembled_ms` stays bounded rather than growing toward
minutes, and `drop_audio` remains zero. Confirm with live audio and
picture on actual HLS/SRT/UDP outputs. If `carried_total` stays zero
and drift grows, investigate PES header-to-AU mapping further, rather
than increasing queues.

## Rollback

Before touching services, preserve known-good `/opt/DVBStreamer5`
and `/etc/dvbstreamer5`, and retain the V525/V526 portable tar.gz.
If the new release fails, stop services and restore your verified
previous installation plus `systemctl daemon-reload` and restart.

---

# Полевые испытания V527 на home2 и svettranscoding — 14.11.2026

> Это обновление **документации**, не новый релиз: `kProgramVersion` остаётся
> `10.8.527`. Не менять рабочие кодировщики других плиток ради теста.
> Сборку и запуск GitHub Actions после правки документа проверять отдельно.

## A. Проверка на home2 — подтверждённые факты

После установки V527:
- HISTORY `service_id=113`: `raw_video_vs_assembled_ms` от 0 до
  +320 мс в показанных окнах; до V527 на V526 отставание достигало
  ~202 **секунд**.
- Sky Sport News `service_id=108`: обычно от -240 до +280 мс в
  показанных окнах; до V527 отставание было ~55 секунд.
- `NATIVE AV MUX DROP=0`, `NATIVE AV MUX PEER STALL=0`;
  в показанных mux-окнах `aq=0`.
- `h264_interior_pes_carried_total=0` **на обоих каналах**. В показанных
  запусках новый перенос внутренних PES-якорей не активировался,
  поэтому строго приписывать улучшение **именно** переносу V527 нельзя.
- На SID 108 `NATIVE AVC PES PTS RESYNC` достиг порядка 4 700
  событий; коррекция порядка -120…+160 мс встречалась часто.
  Это не равнозначно доказанной плавности видеокартинки.
- Последовательные журналы охватывали лишь около трёх минут.
  До полного подтверждения причины и устойчивости нужны непрерывные
  более длительные прогоны и реальное воспроизведение.

**Вывод:** на предоставленных фрагментах V527 не демонстрирует прежнего
накопления дрейфа и потерь аудио, но 24-часовая стабильность, HLS/SRT/UDP
воспроизведение и отсутствие микро-рывков пока не установлены.

## B. svettranscoding — реальный Intel 4-го поколения

Фактически проверенная конфигурация:
- Сервер `svettranscoding`, DVBStreamer5 `10.8.527`;
- Intel Core i5-4430S (4 ядра), Intel HD Graphics 4600 `8086:0412`;
- `/dev/dri/renderD128`, VAAPI-драйвер `i965` версии 2.4.1;
- `vainfo`: VAProfileH264Main / High `VAEntrypointEncSlice` доступен;
- H.264 1920×1080 @25 FPS, видеобитрейт 6000 кбит/с;
- испытанные ID: `stream-1794627918823`, `stream-1794629815825`.

### Тест A — Intel VAAPI

Действительный старт кодировщика подтверждён строкой от
`2026-11-14 15:45:31+03:00`:
```text
NATIVE HW ENCODER backend=vaapi-direct codec=h264 size=1920x1080 fps=25 bitrate_kbps=6000 preset=quality profile=high gop_frames=50 refs=2 rc=cbr strict_hrd=1 hrd_window_ms=400
```

В выбранных ранних окнах `NATIVE VIDEO ENCODE PERF`:
- ~24–25 FPS; `encode_avg_ms` 8–9 мс, `encode_max_ms` до
  16 мс в выбранных ранних окнах;
- `raw_drop_total=0`, `raw_drop_window=0`, `aq=0`,
  `drop_audio=0`, `drop_video=0`;
- общий журнал проверен на `PEER STALL`, `MUX DROP`,
  `PRIMARY RAW DROP`: 0 найденных событий в запрошенном интервале;
- на отдельных выходных H.264 `peak_100ms_kbps` до 16560 кбит/с,
  `peak_1s_kbps` порядка 5600–6156 кбит/с. Нулевые внутренние
  потери **не доказывают** отсутствие перегрузки downstream CBR/WISI.

**Ограничение:** сохранённый журнал Intel за
`16:00:24–16:11:00 +03:00` частично пересекается с подготовкой
CPU-теста, начатой в `16:10:53`. В поздних окнах встречаются
`encode_avg_ms` 16–21 мс, которые нельзя без временной и PID-привязки
приписывать только VAAPI. Сравнение 8–9 мс — **выборка**, не строгая
средняя за 10 минут.

### Тест B — CPU OpenH264

Одна плитка `stream-1794627918823` переключена на
`transcode_video_encoder=cpu`; подтверждены строки:
```text
NATIVE TRANSCODE AUTO stream=stream-1794627918823 encoder=cpu fps=25 configured_video_kbps=6000 effective_video_kbps=6000 preset=balanced profile=high
NATIVE CPU ENCODER backend=openh264 codec=h264 size=1920x1080 fps=25 bitrate_kbps=6000 preset=balanced profile=high
```

- Отдельное CPU-наблюдение: `16:17:16–16:27:16 +03:00`, 600 секунд;
- последние 15 окон: 23–26 FPS, `encode_avg_ms` 20–23 мс,
  `encode_max_ms` до 60 мс;
- `raw_drop_total=0`, `raw_drop_window=0`;
  `NATIVE AV MUX DROP=0`, `PEER STALL=0`,
  `NATIVE PRIMARY RAW DROP=0` в собранном журнале;
- `aq=0`, `av_delta_ms` 89–147 мс при `hard_lead_ms=150`;
- `ps -p 383251`: `%CPU=148`, `RSS=403780 KiB` (~394 МиБ).
  Это **нагрузка всего процесса**, а не одного OpenH264.

Сравнение аппаратных 8–9 мс и CPU ~21–22 мс означает, что VAAPI
быстрее в указанных окнах примерно в 2,4–2,7 раза. Но настройки
не идентичны: VAAPI `preset=quality`, CPU `preset=balanced`,
поэтому сравнение качества и полной энергоэффективности не выполнено.

## C. Инструкция повторяемого A/B-теста без перезапуска сервера

**Никакой скрипт с аргументом `cpu` или `intel` не переключает
сам энкодер.** Режим выбирается в настройках **одной тестовой плитки**.
Сохранить одинаковыми источник, H.264, 1920×1080, 25 FPS,
6000 кбит/с, аудиокодек и видеодекодер. Изменить только
`Видеокодировщик: VAAPI`, затем `CPU`, перезапуская **только
тестовую плитку** между прогонами. Вторую плитку не трогать.

**Подтверждение фактического backend** (после старта плитки):
```bash
journalctl -u dvbstreamer5.service --since '-10 min' --no-pager -o cat |
grep -E 'NATIVE HW ENCODER backend=vaapi-direct|NATIVE CPU ENCODER backend=openh264|NATIVE TRANSCODE AUTO stream=' |
tail -n 30
```
Для Intel нужно видеть `backend=vaapi-direct`, для CPU —
`backend=openh264`. `encoder_requested=vaapi` в строке
`NATIVE VIDEO BURST DIAG` сам по себе **недостаточен**.

**Один 600-секундный замер** после проверки backend:
```bash
MODE=intel                         # ВО ВТОРОМ ПРОГОНЕ: MODE=cpu
STREAM=stream-1794627918823       # ID единственной тестовой плитки
PID=$(systemctl show -p MainPID --value dvbstreamer5.service)
test "$(cat /opt/DVBStreamer5/VERSION)" = "10.8.527" || exit 1
START=$(date --iso-8601=seconds)
OUT="/tmp/v527-svet-${MODE}-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
printf 'mode=%s stream=%s pid=%s start=%s\n' "$MODE" "$STREAM" "$PID" "$START" | tee "$OUT/meta.txt"
sleep 600
test "$PID" = "$(systemctl show -p MainPID --value dvbstreamer5.service)" || {
  echo "Service restarted: test invalid"; exit 1;
}
END=$(date --iso-8601=seconds)
printf 'end=%s\n' "$END" >> "$OUT/meta.txt"
journalctl -u dvbstreamer5.service --since "$START" --until "$END"   --no-pager -o cat > "$OUT/journal.log"
grep -F "stream_id=$STREAM " "$OUT/journal.log" |
  grep 'NATIVE VIDEO ENCODE PERF' > "$OUT/encode.log" || true
grep -F "stream_id=$STREAM " "$OUT/journal.log" |
  grep 'NATIVE VIDEO DECODE PERF' > "$OUT/decode.log" || true
grep -F "stream_id=$STREAM " "$OUT/journal.log" |
  grep 'NATIVE AV MUX stream_id=' > "$OUT/mux.log" || true
grep -E 'NATIVE AV MUX PEER STALL|NATIVE AV MUX DROP|NATIVE PRIMARY RAW DROP'   "$OUT/journal.log" > "$OUT/drop-stall.log" || true
printf 'encoder_windows=%s\nmux_windows=%s\ndrop_stall_lines=%s\n'   "$(wc -l < "$OUT/encode.log")"   "$(wc -l < "$OUT/mux.log")"   "$(wc -l < "$OUT/drop-stall.log")"
tail -n 8 "$OUT/encode.log"
tail -n 8 "$OUT/mux.log"
echo "RESULTS=$OUT"
```

При сравнении брать **один и тот же stream ID** и только интервалы
после окончательного старта соответствующего backend. Сохранить полный
`journal.log`, включая строки, где сообщения разных потоков
перемешались: такие повреждённые строки исключить из числовой выборки,
а не исправлять их значения предположениями.

**Обязательные критерии:** `fps` около 25;
`raw_drop_total=0` и `drop_video=drop_audio=0`;
нет `PEER STALL`; `aq` не накапливается; `av_delta_ms` не растёт
с течением времени; исходный TS не содержит увеличивающихся PTS/PCR
ошибок; подтверждено плавное реальное воспроизведение.

## D. Остаточные риски и дальнейшая валидация

1. Изменение H.264 PTS находится в общем `NativeTsDemux`, который
   используется разными источниками/выходами, в том числе
   `videoCodec=copy`. Кодировщики не менялись, но их входные
   временные метки потенциально затрагиваются.
2. Отдельно проверить H.264 Video Copy, HEVC/MPEG-2 как контроль,
   HLS/SRT/HTTP/UDP и CMAF/RTMP, реальные interlaced/B-frame сервисы.
3. Провести продолжительный (от 15 минут и дольше) home2-тест со
   звуком, FPS, PCR/PTS/DTS, CC errors на фактическом выходном TS.
4. Для CPU vs VAAPI обязательно фиксировать применённые пресеты:
   `quality` у Intel и `balanced` у CPU — не равные условия качества.
5. `h264_interior_pes_carried_total=0` в home2 доказывает только,
   что новая ветка переноса ещё не использовалась на наблюдаемых входах;
   это не подтверждение и не опровержение её эффективности в другом TS.
6. Не повышать очереди, не отключать drop safety и не менять
   A/V mux thresholds без причины, подтверждённой логами.

**Статус:** реальный VAAPI и CPU кодировщики V527 работали без
зарегистрированных внутренних потерь в предоставленных интервалах.
Полный регрессионный набор и долговременная проверка внешнего
воспроизведения остаются открыты. **Не объявлять все энкодеры
проверенными по этим двум тестам.**
