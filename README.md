### V10.8.263 — экспериментальный AV-mux hard lead 150 мс для software H.264

- Для вручную выбранных Ittiam/OpenH264 и программного AVC в Auto включена такая же ограничивающая политика 150 мс, какая ранее работала для FFmpeg. Цель: устранить подтверждённые ~480 мс опережения видео и непрерывный рост late_audio на Ittiam.
- Не изменены исходные PTS, PAL cadence, PCR, аудио, hardware zero-copy или fallback ручных декодеров. GPU zero-copy сохраняет 500 мс.
- Это *тестовый* патч: на одном канале обязательно проверить зависания/недостаток пакетов, mux queue, late_audio и воспринимаемую синхронизацию; при ухудшении откатиться на V262. В main не сливать до проверки.

### V10.8.262 — строгий ручной выбор AVC-декодера и аппаратный Auto

- Ручной выбор OpenH264, Ittiam или FFmpeg не переключается на другой программный декодер при ошибке, паузе декодирования или H.264 field coding; вместо этого ошибка остаётся видимой в журнале.
- Фабрика OpenH264 больше не подменяет выбранный OpenH264 на Ittiam скрытым диагностическим fallback.
- AVC Auto: существующие NVIDIA/Intel zero-copy проверки выполняются раньше; если совместимый аппаратный zero-copy путь не запущен, пробуется независимый Intel HW decoder, затем FFmpeg → Ittiam → OpenH264.
- Для выбранных вручную NVIDIA/Intel аппаратных декодеров не предусмотрена скрытая замена на программный.
- PCR, общий mux, audio timing, видеоэнкодеры и FFmpeg PAL PTS не изменены. Проверить полную сборку и live-регрессию перед слиянием в main.

### V10.8.261 — программные H.264 декодеры и переход PTS через 33-битную границу

- **База V260 сохранена:** успешная реальная синхронизация аудио/видео в VLC, PCR decode preroll 500 мс, аудио PTS, VAAPI/QSV и NVIDIA NVENC/NVDEC не менялись; ручного аудио-смещения нет.
- **Ittiam libavc:** прежний wrapper без проверки присваивал каждому interlaced output 25-fps PTS от первого кадра, даже при другой реальной частоте или обрыве GOP. Теперь `NativeIttiamAvc.cpp` использует проверяемый `FfmpegPalPtsClock` с обнаружением только устойчивой PAL 25-fps cadence и повторной привязкой при discontinuity, без изменения самой сборки libavc.
- **OpenH264 и Ittiam при interlaced broadcast AVC:** `NativeTsDemux` передаёт по каждому AU признак фактически распознанного `field_pic_flag`. Если он появился у программного OpenH264/Ittiam, только этот поток переключается на проверенный FFmpeg/libavcodec, поскольку OpenH264 не гарантирует корректное декодирование H.264 field-coded picture. Для прогрессивного H264 сохраняется выбранный декодер. Если FFmpeg не собран, выбранный декодер остаётся активным, а причина отказа записывается в журнал.
- **Автовосстановление:** если программный AVC декодер возвращает ошибку или 150 входных AU подряд не выдаёт кадры, попытаться один раз переключиться на FFmpeg без изменения видеоэнкодера, звука или других каналов. Логи: `NATIVE AVC SOFTWARE FALLBACK stream_id=... from=... to=ffmpeg reason=verified_h264_field_coding|no_decoded_frames|decoder_error`. При неуспехе журнала `unavailable` исходный тракт сохраняется для случаев без фатальной ошибки.
- **Потоки и очереди:** при динамическом выборе FFmpeg сигнал в независимый mux worker передаётся через атомарный `ffmpegAvcMuxPolicy_` (без гонки при чтении `selectedAvcDecoder_`). Действующие ограничения 150/500 мс сохраняются.
- **33-bit PTS/DTS wrap:** выявлен затык `muxAudioQueue_=512`, `drop_audio>47000`, `av_delta_ms~-94500000` когда видео прошло `2^33` 90-kHz ticks, а аудио перешло к нулю. Общие сравнения, выбор пакетов, late/drop и журнал используют модульные функции `src/media/PtsClock33.h`; `NativeMpegTsMux` продолжает отсчёт внутреннего времени сквозь wrap и сохраняет разницу B-frame PTS−DTS. Дополнительный header-клон удалён.
- **Регрессия:** `testPtsClock33()` добавлен в `tests/test_media_core.cpp`. Проверяет события непосредственно до/после wrap, совместимость wrapped/unwrapped меток и регресс `video=2^33+60000, audio=55000`. Небольшой отдельный C++17 smoke test логики clock прошёл локально; полный `media_core_tests`, программные декодеры и live GPU/OSCam после V261 **должны быть проверены на сервере**.
- Команды проверки: `cmake -S tests/media_core -B build-test-media && cmake --build build-test-media --parallel 2 && ctest --test-dir build-test-media --output-on-failure`. Сначала тестировать V261 на одном канале с FFT/FFmpeg, затем Ittiam/OpenH264; сохранять рабочий V260 для отката. Метрики: `NATIVE AVC SOFTWARE FALLBACK`, `NATIVE AVC ITTIAM PAL cadence`, `NATIVE AV MUX`, `NATIVE AV MUX DROP`.

### V10.8.260 — исправлены тесты HTTP live reconnect (media_core)

- После прохождения CBR/UDP/RTP частей тестов V259 `media_core_tests` упал в `testNativeHttpCbrRelay(): assert(!relay.isRunning())`. По серверному журналу `NATIVE HTTP INPUT ... status=200 ok=1 bytes=1504` и следующая `attempt=2`: исходная функция `NativeUdpRelay::runHttpInput()` намеренно повторяет подключение после EOF обычного конечного HTTP ответа; live relay НЕ обязан завершаться сам.
- Исправлены тесты HTTP CBR, HTTP query access key и HTTP VBR/RTP fanout: после получения полезных пакетов проверяем, что соединение/live relay остаётся работающим, затем явно выполняем `relay.stop()` и ожидаем корректное завершение одноразового тестового HTTP-сервера. Устаревшая проверка пустого `lastError()` после transient EOF удалена, так как состояние `reconnecting` является нормальным.
- Проверки HTTP 404 и HTTP 302 с авторизационным ключом теперь ожидают соответствующие сообщения ошибки **при работающем relay**, затем `stop()`. Продолжаем проверять, что секретный заголовок присутствовал только в первоначальном запросе и перенаправление не было выполнено. Никакие дополнительные подключения от имени тестового пользователя не выполняются.
- Изменён исключительно `tests/test_media_core.cpp`; production `NativeUdpRelay.cpp`, WISI CBR pacing, remapper, mux, видеодекодеры и выходные протоколы **не изменялись**. PCR preroll 500 мс из V256 оставлен без изменений.
- Запуск в Ubuntu: `git pull --ff-only origin main && cmake -S tests/media_core -B build-test-media && cmake --build build-test-media --parallel 2 && ctest --test-dir build-test-media --output-on-failure`. Полный тестовый набор на рабочем сервере ещё не подтверждён.

### V10.8.259 — обновлены тесты CBR пейсера и UDP/RTP/HTTP/File CBR

- После исправлений тестов remapper на V258 `media_core_tests` на Ubuntu остановился в `testCbrTsPacer(): assert(datagram[0] == mediaPacket)`. Это **устаревшее предположение**: текущий WISI CBR pacer использует токены полезного потока (первоначально 75% от TS bitrate) и распределяет NULL-пакеты между полезными слотами, а не только после них.
- В `tests/test_media_core.cpp` тест прямого pacer проверяет точное присутствие **одного медиа-пакета в любом слоте 7×188**, валидность всех пакетов и последовательный NULL continuity. Лимит FIFO теперь берётся из `CbrTsPacer::maximumQueuedBytes()` (8 MiB), а не устаревшей константы 2 MiB. Минимальный CBR bitrate также проверяется по действующей нижней границе 100 kbps (а не старой 512 kbps).
- Исправлены связанные интеграционные тесты UDP CBR, UDP+RTP+CBR fanout, File CBR, HTTP CBR, HTTP query key + CBR. Помощник `collectCbrMedia` проверяет размер каждой полученной датаграммы 7×188, синхробайт TS, NULL continuity, состав и порядок **всех полезных пакетов** независимо от смещения среди NULL-пакетов. Тестовые короткие потоки теперь используют `CbrTsPacer::kNoStartupBufferMarker` (+699), чтобы не ждать штатный 3-секундный накопительный буфер WISI; реальный transport target остается 512 kbps.
- Производственный `CbrTsPacer`, `WisiCbrOutputWorker`, `MpegTsRemapper`, `NativeMpegTsMux` и все декодеры/энкодеры **не менялись**. PCR preroll 500ms V256 сохранён для последующей проверки lip-sync на настоящем VLC.
- Проверка: `git pull --ff-only origin main && cmake -S tests/media_core -B build-test-media && cmake --build build-test-media --parallel 2 && ctest --test-dir build-test-media --output-on-failure`. Эти обновлённые тесты ещё должны быть запущены на Linux — локально полный набор нативных зависимости/ввода не тестировался.

### V10.8.258 — тест восстановления после фрагментированного PAT

- На Ubuntu `ctest` V257 завершался в `testMpegTsRemapper()` на старом ожидании `!remapper.process(fragmentedPat,...)`: оно не соответствует текущему `collectSections()`, где неполная PAT секция буферизуется между TS пакетами вместо немедленного завершения канала.
- Обновлён только `tests/test_media_core.cpp`: неполный PAT при continuity=4 должен быть принят без ошибки, следующий корректный PAT при continuity=5 должен восстановить PSI сборщик, а сервис SID=42 и ремаппинг video PID=0x0200 остаться действующими. Тест не зависит от случайного момента фоновой генерации PAT/PMT/SDT.
- Производственные `MpegTsRemapper.cpp` и `NativeMpegTsMux.cpp` не менялись; PCR decode preroll 500ms из V256 сохранён. Исправление только регрессионного теста, не новая проверка реальной синхронизации VLC.
- Команды: `git pull --ff-only origin main`, `cmake -S tests/media_core -B build-test-media`, `cmake --build build-test-media --parallel 2`, `ctest --test-dir build-test-media --output-on-failure`. Если тест проходит, собрать основное приложение и portable архив V258.
- `__int128` -Wpedantic и предупреждение об unused-function не вызвали предыдущего падения теста.

### V10.8.257 — исправление устаревшего теста MPEG-TS Remapper

- В V256 `media_core_tests` собирался, но `testMpegTsRemapper()` останавливался на `output.size() == 2`. Это был **не провал нового PCR теста**: прежняя проверка ожидала выдачу PAT/PMT сразу при получении PMT, хотя актуальный remapper делает `remapReady_=true` и выдаёт PAT/PMT/SDT на следующем периодическом `tick()` / входном TS-пакете.
- Обновлён только регрессионный `tests/test_media_core.cpp`: после PMT ожидается пустой вывод; `tick()` выдаёт PAT, PMT, SDT; проверяются PID, SID, remapped audio/video PID и CRC; также CAT, EMM, video PID и очередная PAT после 120 мс. Исходный PSI-continuity принадлежит remapper, а не входному TS.
- `src/media/MpegTsRemapper.cpp` **не изменён**. Исправление PCR=video DTS−500 мс из V256 в `NativeMpegTsMux` сохранено. Не менялись Nvenc/Nvdec, VAAPI/QSV, FFmpeg/Ittiam, потоковые выходы и интерфейс.
- Проверка на сервере: `cmake -S tests/media_core -B build-test-media && cmake --build build-test-media --parallel 2 && ctest --test-dir build-test-media --output-on-failure`. Если CTest проходит, пересобрать основное приложение с версией V257 и переносимый архив. Сборка теста после GitHub-фикса локально пока не подтверждена, нужна проверка на Ubuntu.

### V10.8.256 — MPEG-TS PCR decode preroll для всех декодеров (A/V sync)

- Пользователь подтвердил, что звук звучит раньше картинки при **любом доступном видеодекодере**, а не только FFmpeg. Поэтому настройка ручного аудио-офсета V255 **полностью отменена** из кода, UI, JSON конфигурации, clipboard и тестов: она не требуется, пользователь хочет автоматическую синхронизацию.
- Выявлен общий дефект: `NativeMpegTsMux::packetizePes()` записывал `PCR_base = video DTS`. При live-транскодировании без B-кадров обычно `PTS=DTS=PCR`, то есть MPEG-TS приемнику не предоставляется времени декодировать и отобразить кадр до его presentation timestamp. Аудио может начать звучать раньше видимого видео независимо от декодера или кодировщика.
- Исправление в общем MPEG-TS mux: `PCR_base=(video DTS − 45000) mod 2^33`, то есть **500 мс decode preroll**, без изменения исходных PTS/DTS видео и аудио и без принудительного lip-sync offset. Один PCR clock используется всеми выходными элементарными дорожками. Частота выдачи PCR не меняется (на каждом видео PES).
- Это **гипотеза общего корня**, основанная на анализе кода и симптомах на всех декодерах, пока не подтвержденная реальным VLC. Реальная A/V синхронизация и размер буфера могут также зависеть от приемника и mux pacing. Не меняли NVENC/NVDEC, VAAPI/QSV, Ittiam, FFmpeg PAL clock, абонентские output настройки, системный аудиодекодер и mux queue policy V254.
- В `tests/test_media_core.cpp` добавлены проверки 500-мс отставания PCR от video DTS и корректности 33-bit wrap. Запуск: `cmake -S tests/media_core -B build-test-media && cmake --build build-test-media --parallel 2 && ctest --test-dir build-test-media --output-on-failure`; при необходимости запуск `build-test-media/media_core_tests` напрямую.
- После установки V256 проверять VLC с любым декодером, начиная с рабочего FFmpeg→VAAPI, отсутствие `NATIVE AV MUX DROP` и `NATIVE ASYNC VIDEO RESYNC`. Если lip-sync не восстановился, измерять PTS/PCR фактического output TS, а не добавлять ручной сдвиг аудио. На фоне V254 `av_delta_ms` может оставаться 50–130 мс: эта метрика сравнивает дорожки, новый PCR-lag ее не изменяет.

### V10.8.254 — FFmpeg AVC: максимальное опережение видеопотока на mux 150 мс

- Диагностика V253 для канала `stream-1793338874706`: `input_av_ms` примерно +2064..2152 мс по последним меткам producer, `decoded_video_vs_input_ms` −240..−360 мс, `encoded_audio_vs_input_ms` около 0..45 мс; очередь видео 28..34 кадра и `mux_av_ms` +442..456 мс. Продолжительная близость к 500-мс пределу указывает на то, что планировщик выпускает видео почти на пределе допуска относительно уже отправленного аудио.
- Для **явно выбранного H.264 декодера `ffmpeg`** условие `withinHardLead` и ожидание lagging peer теперь ограничены тем же **150-мс preferred lead**, вместо прежнего 500-мс hard escape. Это ограничивает mux-time video ahead of audio без изменения медиатегов, декодирования, FPS-gate или audio PTS. Ожидаем `hard_lead_ms=150` в `NATIVE AV MUX`, `av_delta_ms` ближе к 150 мс (значение может колебаться при очередях).
- Для других выбранных декодеров, `auto`, Intel/VAAPI, NVENC/NVDEC и MPEG-TS output protocol сохраняется прежний 500-мс hard-lead. **Аудио PTS намеренно не сдвигаются**: Source PTS watermark 2.1s может отражать пакетную подачу/буферизацию; mux AV gap не доказывает 450ms воспринимаемого lip-sync.
- Проверить на live сервере VLC и `NATIVE AV MUX` + `NATIVE AV MUX DROP`: более строгое ожидание видео увеличит очередь примерно на несколько кадров. Если очередь достигает 64 или появляются `queue_full`, вернуться к V253 и анализировать источник. При отключенном аудио использовать `auto`/другой декодер, поскольку A/V lead-policy не нужен.
- Это рабочая гипотеза и целевое исправление планировщика, не проверенное на аппаратуре и не гарантирующее устранение полного source PTS mismatch. V251 обеспечила чистое изображение на FFmpeg, V252–253 стабилизировали PAL FPS/PTS; эти пути не изменены.

### V10.8.253 — FFmpeg PAL AV sync: убран искусственный PTS lead; стадийная диагностика

- По реальному журналу V252 `NATIVE AVC FFMPEG PTS cadence ... source_delta90k=-10800`, что показывает: старая коррекция привязывала дальнейшее видео к аномальному самому первому timestamp и добавляла 120 мс опережения. Новый lock на 25-м кадре привязывается к **текущему исходному FFmpeg PTS**, а следующие display-order кадры идут с PAL шагом 3600/90k. На момент переключения и при реальном discontinuity сбрасывается FPS-gate clock для предотвращения дополнительного пропуска кадров.
- Добавлена ограниченная режимом `FFmpeg` строка `NATIVE AV CLOCK TRACE` для сравнения последних **producer watermark** меток между источником, decoder, audio encoder и mux (`input_av_ms`, `decoded_video_vs_input_ms`, `encoded_audio_vs_input_ms`, `mux_av_ms`, `mux_video_vs_input_ms`, `mux_audio_vs_input_ms`). Это разные моменты обработки и НЕ измерение физической lip-sync задержки на экране; диагностические значения нужно оценивать вместе с очередями.
- При отсутствии меток числовая диагностика показывает `999999` как признак неизвестного значения. Для других выбранных видеодекодеров новый диагностический лог не выводится. Скорость аудио и исходные аудио PTS не меняются. Mux lead window 150/500 мс, Intel VAAPI encoder и NVIDIA NVENC/NVDEC оставлены неизменными.
- Добавлен регрессионный тест на начальную PTS аномалию -10800 тиков. `tests/test_ffmpeg_pal_pts_clock.cpp` компилируется отдельно командой из комментария в начале файла.
- **Важно:** фикс убирает доказанные искусственные 120 мс, но не гарантирует устранение полного AV-skew: V252 уже показывала около +450 мс на последних записанных mux PTS; необходимо сравнить новые `NATIVE AV CLOCK TRACE` и VLC после установки.

### V10.8.252 — FFmpeg 25fps PAL PTS clock stabilization

- В логах V251 на канале 8 FFmpeg чисто декодирует interlaced 720×576 25fps, но generic `NATIVE VIDEO FPS GATE` отбросил более 1000 изображений при входном PAL 25fps. Выходной AV mux показывал стабильные +300…+500ms (`av_delta_ms`), без переполнений mux.
- Только для **явно выбранного FFmpeg H.264**, интерлейсного выходного кадра и целевого **ровно 25fps** введён отдельный clock. После 25 последовательно выданных FFmpeg-кадров он сравнивает реальный PTS-диапазон с PAL cadence и включается лишь при соответствии (±120ms). Переупорядоченные/дрожащие PTS заменяются display-order PTS c шагом 3600/90k, до legacy `FPS GATE`; H264 пиксели и реальные sample count не меняются.
- Нестабильный/30/50 fps источник не проходит PAL eligibility и продолжает использовать стандартный FPS gate. При PTS discontinuity >1sec clock перенастраивается; лог `NATIVE AVC FFMPEG PTS discontinuity`. При lock лог `NATIVE AVC FFMPEG PTS cadence ... locked=1`.
- Есть отдельный тест `tests/test_ffmpeg_pal_pts_clock.cpp` для PAL jitter, 30fps bypass, 33-bit PTS wrap и discontinuity; компиляция теста: `g++ -std=c++17 -Isrc tests/test_ffmpeg_pal_pts_clock.cpp -o /tmp/test_ffmpeg_pal_pts_clock && /tmp/test_ffmpeg_pal_pts_clock`.
- Изменение не применяется к Ittiam, OpenH264, Intel oneVPL, NVIDIA NVDEC/NVENC, progressive AVC, AV mux, AAC и encoder. Дальнейшая runtime-проверка обязательна: `NATIVE VIDEO FPS GATE dropped` должен перестать расти после lock, а `av_delta_ms` требуется сравнить с V251, поскольку аудио PTS отдельно не сдвигались.

### V10.8.251 — независимый FFmpeg H.264 decoder для interlaced IPTV

- По скриншоту V250 видны сохраняющиеся вертикальные блочные артефакты на вещательном AVC 720×576 25 fps. Дамп того же канала успешно декодируется независимым FFmpeg, а NativeTsDemux уже правильно собирает пары полей. Поэтому добавлен отдельный selectable software H.264 decoder на libavcodec без замены Ittiam/OpenH264 или аппаратных энкодеров.
- В редакторе плитки: **Декодер H.264 → FFmpeg libavcodec** (также в английском UI). Новый декодер использует один worker для стабильной обработки PAFF/MBAFF, принимает H.264 Annex-B access units через FFmpeg send/receive API, копирует stride Y/U/V planes в I420, сохраняет frame PTS и interlaced-флаг, передаёт обычному CPU/VAAPI/QSV/NVENC encoder.
- Опциональные зависимости: `sudo apt-get install -y libavcodec-dev libavutil-dev pkg-config` и чистая переконфигурация CMake. При отсутствии dev-файлов старая сборка остаётся доступна, FFmpeg-пункт disabled. Проверить строку `Optional H.264 software decoder: FFmpeg/libavcodec enabled`.
- Portable bundle `scripts/make_portable_linux_bundle.sh` рекурсивно собирает ELF-библиотеки; в архиве также проверить libavcodec и libavutil.
- Устранение артефактов нужно подтвердить в VLC на канале 8 после установки: `FFmpeg libavcodec` decoder + `Intel VAAPI direct` encoder, 720×576, 25 fps без deinterlace. Ожидаются логи `NATIVE AVC DECODER backend=ffmpeg-libavcodec`, `NATIVE AVC FFMPEG OUTPUT`.
- NVENC/NVDEC, Intel zero-copy, VAAPI encoder, Ittiam, OpenH264, MPEG-TS output и остальные каналы не менялись. Режим явно выбирается пользователем и не становится глобальным.

### V10.8.250 — исправлена сборка interlaced AVC field pairs из IPTV MPEG-TS

- Диагностика канала 8 (`stream-1793338874706`) по дампу из V249 выявила, что поток выдаёт 50 отдельных top/bottom field NAL в секунду, причём каждый field предваряется собственными AUD/PPS. FFmpeg независимо декодировал видео с нормальными кадрами. Старый `NativeTsDemux::queueVideoSample()` немедленно резал AU по повторному AUD/PPS, до уже существующей проверки `complementaryField`; поэтому Ittiam получал отдельные field-AU вместо пары.
- В `NativeTsDemux.cpp` AUD/SPS/PPS после первого slice теперь являются отложенной границей. Если следующий slice — комплементарное поле с тем же `frame_num`/PPS и противоположным `bottom_field_flag`, оба поля и промежуточные NAL передаются Ittiam вместе. Если следующий slice — новое изображение, граница ставится перед его AUD/SPS/PPS, так что NAL не теряются.
- Учитываются GOP, где первое поле — IDR (NAL 5), а второе — non-IDR (NAL 1). Если оба IDR, проверка `idr_pic_id` сохраняется. В предоставленном дампе после появления SPS: 1749 полей; 874 последовательные пары соответствуют признакам пары, в том числе 17 переходов IDR/non-IDR.
- Логи `NATIVE AVC AU FIELD_PAIR` и `NATIVE AVC AU FIELD_CLOCK` позволяют подтвердить эффект на живом потоке: частота `FIELD_PAIR` должна вырасти, а `FIELD_CLOCK` почти исчезнуть.
- **Не менялись** NVENC/NVDEC, oneVPL, Intel VAAPI encoding, CPU scaler, прочие протоколы и структура сохранённого конфига. Сборка на реальном сервере и устранение артефактов требуют проверки после установки.

### V10.8.249 — адресный дамп H.264 для диагностики артефактов Ittiam

- Исходный interlaced AVC 720×576 продолжает содержать артефакты даже без deinterlace и масштабирования; цветной тестовый I420-кадр проходит CPU scaler → Intel VAAPI encoder без повреждений. Поэтому нельзя считать причиной Intel encoder до проверки входных AVC байтов.
- Ittiam error `0x00000091` соответствует `ERROR_START_CODE_NOT_FOUND`; одного этого кода недостаточно, чтобы объявить входное видео повреждённым.
- Ранее `DVBSTREAMER5_DUMP_H264_ES=/tmp/dvb-8.h264` писал до 64 MiB для каждого AVC потока. Для безопасности теперь требуется ещё `DVBSTREAMER5_DUMP_H264_ES_STREAM_ID=stream-1793338874706`. Без точного stream_id дамп не выполняется. Файлы включают stream_id, PID, индекс с PTS/DTS и не обрезают access unit на границе лимита.
- Пример systemd drop-in: `Environment=DVBSTREAMER5_DUMP_H264_ES=/var/tmp/dvb8.h264` и `Environment=DVBSTREAMER5_DUMP_H264_ES_STREAM_ID=stream-1793338874706`. После перезапуска службы файл `/var/tmp/dvb8.<stream-id>.<hash>.pid-*.h264` можно проверить независимым декодером ffmpeg/VLC.
- **Это инструмент диагностики, а не исправление артефактов.** Потоковый путь Ittiam, Intel VAAPI, NVIDIA NVENC/NVDEC и база V241 остались неизменными.

### V10.8.248 — VAAPI VPP bypass guard и диагностика PAL upscaling

- По runtime-логу 720×576 → 1920×1080 обнаружено, что выбранный OpenH264 не выдаёт кадры и автоматически переключается на Ittiam после 150 samples; это не чистое сравнение двух software decoders.
- Добавлена дополнительная защита прямо в `VaapiH264Encoder::encode`: когда входной размер отличается от выходного, по умолчанию выполняется CPU I420 scaling и только потом аппаратное VAAPI H.264 encode, даже если вызывающий код не выполнил scaler negotiation. Внутренний VAAPI VPP для I420 применяется только с явным `DVBSTREAMER5_ENABLE_VAAPI_SOFTWARE_SCALE=1`.
- Добавлен обязательный startup-лог `NATIVE VAAPI ENCODER scale-policy` с реальным source/output и выбранным CPU/VPP режимом, чтобы отличить обновлённый бинарник от старого.
- Существующие Intel hardware decode и NVIDIA NVDEC/NVENC реализации не изменены. Настройку `OpenH264` пока сохраняем с fallback для обратной совместимости, но реальный decoder надо проверять по `NATIVE AVC DECODER`.
- Само исчезновение артефактов пока не подтверждено тестом на сервере. Для диагностики есть уже существующая per-stream подмена raw-I420 кадра через `DVBSTREAMER5_DIAG_COLOR_STREAM_ID`.

### V10.8.247 — исправлен обход CPU scaler в legacy VAAPI encoder

- При использовании Intel `VAAPI direct` c программным H.264 decoder прежний `supportsNativeScaling()` ошибочно включал встроенный VAAPI VPP при изменении разрешения, обходя исправление V243. Теперь по умолчанию CPU I420 scaler выполняется до VAAPI encoder, а H.264 кодируется аппаратно.
- Для сравнения допускается явное включение старого GPU VPP: `DVBSTREAMER5_ENABLE_VAAPI_SOFTWARE_SCALE=1`. На Haswell/i965 с наблюдаемыми артефактами включать его не рекомендуется.
- Сохранена диагностика V246: Intel oneVPL HW decoder отключается в UI, если нет аппаратной реализации API 2.1+. Для Haswell/i965 отдельный VAAPI VLD decoder пока не реализован. Доступность Intel VAAPI encode не зависит от oneVPL decode.
- NVIDIA NVDEC/NVENC код не менялся; база V241 и Intel GPU metrics сохранены.

### V10.8.246 — исправлена диагностика возможностей Intel HW decoder

- `Intel HW / oneVPL` доступен в UI только при подтверждённой аппаратной реализации oneVPL API 2.1+ (обязательной для `FrameInterface::Map/Unmap`). Наличие отдельного Intel VAAPI encoder больше не выдаётся за доступность oneVPL decoder.
- На legacy Intel Haswell с рабочим `i965` аппаратное кодирование `VAAPI direct` остаётся доступно. Ручной oneVPL decoder отключён, поскольку для старого i965 нужен отдельный native VAAPI VLD decoder, который здесь ещё не реализован.
- Если oneVPL недоступен, вручную выбранный `intel_hw` выдаёт понятную ошибку вместо неясного `OFFLINE`; сохранённые конфигурации автоматически не переписываются.
- Режимы NVIDIA/NVDEC/NVENC не менялись. Исправление Map/Unmap из V245 сохранено для поддерживаемых oneVPL 2.1+ GPU.

### V10.8.245 — oneVPL Intel HW decoder: корректное отображение поверхности

- Исправлен ручной Intel oneVPL hardware decoder: после `DecodeFrameAsync` decoded surface синхронизируется и обязательно отображается через `FrameInterface->Map(MFX_MAP_READ)` перед чтением `Data.Y/Data.UV`. После копирования I420 выполняются `Unmap` и `Release`.
- Проверяются формат NV12, coded/visible geometry, чётность crop, Y/UV pointers и pitch. При ошибке выводится подробная диагностика вместо общего сообщения.
- Это исправляет чтение неотображённой oneVPL поверхности в V244. Не устраняет отсутствующий Intel hardware runtime/driver: для такого случая нужен полный журнал `Intel HW decoder...`.
- NVIDIA NVDEC/NVENC и штатный Intel zero-copy pipeline не изменены.

### V10.8.244 — настоящий encoder-independent Intel HW decoder

- Ручной режим `Intel HW / oneVPL` больше не требует encoder `QSV/Auto`. Добавлен отдельный oneVPL hardware decoder, который синхронизирует decoded surface, учитывает реальный NV12 pitch и crop, преобразует видимую область в I420 и передаёт кадр выбранному encoder-у.
- Поддерживаются комбинации Intel HW decode → CPU, QSV, VAAPI или NVENC encode. Это устраняет прежний OFFLINE с сообщением `Intel HW decoder...` при несовместимой жёсткой привязке к zero-copy transcoder.
- Автоматические Intel/NVIDIA zero-copy пути сохранены как отдельные оптимизации. Ручной NVIDIA HW пока остаётся связан с NVDEC → CUDA → NVENC.
- Сохранены исправления V243: CPU scaler по умолчанию для software-decoded I420 и startup gate по реальным random-access NAL.

### V10.8.243 — software scaler corruption fix и QSV startup fix

- Исправлен общий путь software-decoded I420 кадров: автоматический VAAPI VPP scaler больше не используется по умолчанию. OpenH264/Ittiam/обычный software decode теперь масштабируются встроенным CPU I420 scaler-ом перед CPU/NVENC/QSV encode. Это изолирует наблюдавшиеся блочные/полосовые артефакты от Intel VAAPI VPP. Для A/B VAAPI software scaling можно вернуть только явно: `DVBSTREAMER5_ENABLE_VAAPI_SOFTWARE_SCALE=1`.
- Dedicated hardware paths не отключены: `NVIDIA HW / NVDEC` по-прежнему использует NVDEC → CUDA → NVENC, `Intel HW / oneVPL` — Intel oneVPL decode/VPP/encode zero-copy.
- Исправлен startup gate QSV/oneVPL: готовность видео теперь определяется по фактическим H.264/H.265 random-access NAL и накопленным parameter sets, а не требует encoder `FrameType` и SPS/PPS/IDR одновременно в одном AU. Добавлены логи `NATIVE VIDEO STARTUP waiting/ready`.
- База V241 и Intel GPU header metrics сохранены.

### V10.8.242 — ручной выбор аппаратного H.264 decoder на базе V241

- База V241 сохранена: Intel GPU process Video/Render metrics, автоматические transcode-настройки V240 и остальные изменения ветки `intel-gpu-metrics-v10.8.241` не откатывались.
- В редактор транскодирования добавлены режимы `NVIDIA HW / NVDEC` и `Intel HW / oneVPL` рядом с `Auto`, `Ittiam libavc` и `OpenH264`.
- `NVIDIA HW / NVDEC` принудительно использует существующий NVDEC → CUDA → NVENC zero-copy путь и требует encoder `NVENC` или `Auto`.
- `Intel HW / oneVPL` принудительно использует существующий Intel oneVPL decode/VPP/encode zero-copy путь и требует encoder `QSV` или `Auto`.
- При недоступном hardware runtime ручной режим завершается явной ошибкой вместо скрытого fallback на software decoder. Реализации NVENC/NVDEC и Intel encoder не переписывались.

### V10.8.241 — Intel GPU utilization in the header (QSV / VAAPI)

**Test branch:** `intel-gpu-metrics-v10.8.241`, based on `transcode-auto-v10.8.240`. `main` remains untouched.

- **Root cause:** NVIDIA NVML supplies a card-wide `utilization_percent`, while Intel i915/Xe DRM GPU cards often expose no compatible global `gpu_busy_percent`; the old header therefore displayed "N/A" despite ongoing Intel hardware transcoding.
- **Intel live fix:** when card-wide GPU percentage is unavailable, calculate DVBStreamer5 **process-owned** utilization from DRM per-engine counters and display `GPU 45% · Intel (app)` in the header, with an explicit app-only scope and engine name in the tooltip. This is a percentage of the busiest measured GPU engine and is **not** whole-card utilization. Do not invent 0% when counters are unavailable.
- **Hardware compatibility:** Intel `i915` uses `/proc/self/fdinfo` `drm-engine-video/render` nanoseconds; Intel `xe` uses `drm-cycles-*` divided by `drm-total-cycles-*` over the sample interval. Both use device PCI ID matching. The 3-second header sampler and 30-second history sampler have separate baselines. No external `intel_gpu_top`, `nvidia-smi`, root privileges or shell commands are required for the sampling itself.
- **NVIDIA unchanged:** NVML card-wide utilization keeps priority when present; mixed Intel/NVIDIA systems choose the largest available figure and identify Intel's process-specific fallback.
- **System load panel:** the per-device percentage also falls back to app DRM GPU busy with clear source labeling; existing GPU graphs and separate per-process engine histories remain available.
- **No transcoder changes:** CPU, NVENC, NVDEC, QSV, VAAPI, mux, OSCam and DVB source/CA pipelines are unchanged. All automatic transcoding configuration from V10.8.240 is retained.
- **Tests:** `node tests/web/test_intel_gpu_header.js`, plus `bash scripts/test_auto_transcode_policy.sh`. The CI performs a CPU-only CMake compile. Intel hardware/transcode output must still be verified on the actual server.

Ubuntu 22.04 / 24.04 source update from a separate test checkout:

```bash
git fetch origin
git switch intel-gpu-metrics-v10.8.241 || git switch -c intel-gpu-metrics-v10.8.241 --track origin/intel-gpu-metrics-v10.8.241
git pull --ff-only
node tests/web/test_intel_gpu_header.js
bash scripts/test_auto_transcode_policy.sh
bash scripts/vendor_srt_source.sh
bash scripts/vendor_native_codecs.sh
DVBSTREAMER5_CODEC_JOBS=2 bash scripts/build_native_codecs.sh
cmake -S . -B build-v241 -DCMAKE_BUILD_TYPE=Release \
  -DDVBSTREAMER5_ENABLE_NVENC=ON \
  -DDVBSTREAMER5_ENABLE_INTEL_QSV=ON \
  -DDVBSTREAMER5_ENABLE_VAAPI=ON
cmake --build build-v241 --parallel 2
bash scripts/make_portable_linux_bundle.sh --build-dir build-v241 --binary build-v241/DVBStreamer5 --output-dir dist/DVBStreamer5-linux-x86_64 --with-oscam --archive
```

While an Intel QSV/VAAPI channel is transcoding, examine API metrics without changing the running daemon:

```bash
curl -fsS http://127.0.0.1:9000/api/system-metrics | jq '{gpus,process_gpus}'
```

If the API requires session authentication, use your normal authenticated browser/network route and inspect the JSON response. Wait for at least two live metrics polls; the first has no previous DRM counter baseline. Unsupported kernel DRM statistics legitimately display N/A rather than a fabricated percentage.

---

### V10.8.240 — automatic video-transcoding controls (CPU / NVIDIA / Intel)

**Test branch:** `transcode-auto-v10.8.240`, based on `dvb-vbr-null-v10.8.239`; `main` is not modified.

- **Simplified stream editor:** Video codec (including Copy), encoder (Auto/CPU/NVIDIA NVENC/Intel QSV/Intel VAAPI), H.264 decoder, output resolution, video bitrate, FPS, deinterlace, audio controls and HLS ABR remain user-selectable.
- **Advanced controls removed:** no manual Preset, Profile, Level, GOP, reference frames, AQ strength, B-frames or Open GOP. `Profile=auto` and `Level=auto` are enforced for all backends; the encoder negotiates the effective hardware-supported profile/level.
- **Automatic policy:** the explicitly configured **transcoding video bitrate** (not the TS/UDP-CBR bitrate), output resolution and FPS determine a supported real-time speed/quality preset and adaptive quantization; GOP is approximately 2 seconds (`round(fps * 2)`). Live video uses B-frames 0, refs auto, Open GOP off. On high-throughput 1080p50/60 use a faster preset, and CPU 4K50/60 uses fastest. This does **not** change the user-specified bitrate, frame rate, resolution, audio or encoder choice.
- **CBR-independent:** automatic preset/GOP/AQ use the bitrate entered under **Transcoding → Video bitrate**, even if a separate MPEG-TS CBR output limit constrains the effective encoder bitrate. Transport CBR remains independent. Each lower HLS ABR rendition is automatically tuned using its own configured rendition video bitrate.
- **Migrates older configs:** legacy manually saved advanced properties are ignored by config load and stream API save; the new automatic values are stored for consistent UI/API state. Existing transcoding copy/paste transfers only user-visible controls. Original copy/pass-through and non-transcoded streams are unaffected.
- **Tests:** `bash scripts/test_auto_transcode_policy.sh`; also part of the experimental branch's CI. A successful CI build, actual NVIDIA/Intel encoding and hardware playback must be verified independently.

Ubuntu 22.04 / 24.04 — new source checkout, CPU and any detected GPU backends:

```bash
git clone --recurse-submodules -b transcode-auto-v10.8.240 https://github.com/Monkipnet/DVBStreamer5.git DVBStreamer5-v240
cd DVBStreamer5-v240
bash scripts/vendor_srt_source.sh
bash scripts/vendor_native_codecs.sh
DVBSTREAMER5_CODEC_JOBS=2 bash scripts/build_native_codecs.sh
bash scripts/test_auto_transcode_policy.sh
cmake -S . -B build-v240 -DCMAKE_BUILD_TYPE=Release \
  -DDVBSTREAMER5_ENABLE_NVENC=ON \
  -DDVBSTREAMER5_ENABLE_INTEL_QSV=ON \
  -DDVBSTREAMER5_ENABLE_VAAPI=ON
cmake --build build-v240 --parallel 2
bash scripts/make_portable_linux_bundle.sh --build-dir build-v240 --binary build-v240/DVBStreamer5 --output-dir dist/DVBStreamer5-linux-x86_64 --with-oscam --archive
```

If the build host has Intel QSV or VAAPI headers/drivers, install `libvpl-dev` and `libva-dev` beforehand. GPU backends require their respective runtime drivers and hardware. To update a **separate test clone**:

```bash
git fetch origin
git switch transcode-auto-v10.8.240 || git switch -c transcode-auto-v10.8.240 --track origin/transcode-auto-v10.8.240
git pull --ff-only
```

Inspect startup logs for `NATIVE TRANSCODE AUTO stream=...` to confirm `configured_video_kbps`, `effective_video_kbps`, FPS, preset, GOP and automatic profile. Do not replace a working production binary or restart its service until compiling and testing this branch.

---

### V10.8.239 (ISOLATED DVB VBR NULL-PACKET FIX)

**Branch:** `dvb-vbr-null-v10.8.239`, directly based on experimental `ca-irdeto-v10.8.238`; stable `main` is unchanged.

- **Fix:** The shared DVB selected-service prefilter always strips *source-transponder* NULL packets (PID `0x1FFF`), regardless of CBR/VBR mode. Previously it stripped NULL packets only for CBR, so selecting UDP-VBR could send tens of Mbit/s for a ~2.7 Mbit/s service.
- **CBR unchanged:** The CBR worker generates its own NULL packets to meet the configured target bitrate. The fix does not affect full-transponder/MPTS passthrough, other inputs, OSCam, Irdeto CW buffering or DVB-CSA.
- **WISI Chameleon:** For controlled comparison, keep working CBR output at its known rate first. Then switch **one** selected DVB service to UDP-VBR and verify its measured *output* rate is close to its non-NULL service rate (not the full transponder rate). Watch WISI input-buffer/overflow alarms and RF DVB-C BER separately. VBR can still have bursts; CBR remains preferable if the downstream receiver requires a paced CBR input.
- **Status:** GitHub branch source patch; compile/CI results and live WISI results must be verified independently.

Build on Ubuntu (from source):

```bash
git clone --recurse-submodules -b dvb-vbr-null-v10.8.239 https://github.com/Monkipnet/DVBStreamer5.git DVBStreamer5-v239
cd DVBStreamer5-v239
bash scripts/vendor_srt_source.sh
bash scripts/vendor_native_codecs.sh
DVBSTREAMER5_CODEC_JOBS=2 bash scripts/build_native_codecs.sh
cmake -S . -B build-v239 -DCMAKE_BUILD_TYPE=Release
cmake --build build-v239 --parallel 2
bash scripts/make_portable_linux_bundle.sh --build-dir build-v239 --binary build-v239/DVBStreamer5 --output-dir dist/DVBStreamer5-linux-x86_64 --with-oscam --archive
```

Update existing clone:

```bash
git fetch origin
git switch dvb-vbr-null-v10.8.239 || git switch -c dvb-vbr-null-v10.8.239 --track origin/dvb-vbr-null-v10.8.239
git pull --ff-only
```

Confirm VBR filtering in application logs: `SHARED DVB service prefilter enabled ... drop_source_null=1`. That line must appear for a DVB service with CBR **off**, as well as for CBR **on**.

---
### V10.8.238 (ISOLATED DIAGNOSTIC BRANCH): simultaneous Irdeto / TS integrity monitoring

**Branch:** `ca-irdeto-v10.8.238` is based directly on `ca-irdeto-v10.8.236` (NOT V237). `main` is unchanged. It does not include V237 CHID preference caching. This version is for diagnosing occasional picture breakup on eight concurrent satellite services sharing one physical Irdeto card; it does **not** claim to repair the corruption.

**Opt-in:** add `DVBSTREAMER5_CA_TS_MONITOR=1` only for a controlled test of this build. Disabled by default; neither the CW algorithm nor service lifecycle is modified by the monitor. The independent V236 `DVBSTREAMER5_IRDETO_CW_BUFFER=1` remains available; when comparing old/new monitoring conditions, keep that flag at a consistent setting.

- **Input TS before decryption:** per-selected PID continuity-counter gaps, repeats, TEI, invalid adaptation fields, PCR backwards and >1-second jumps (heuristics), explicit discontinuity indicators, scrambled-packet totals and EVEN/ODD TSC changes. Independent continuity state per PID prevents conflating interleaved elementary streams.
- **Immediately after in-place DVB-CSA (still inside CA plugin):** packets whose TSC remains set, missing-key packets, video PES start code/header sanity and H.264/H.265 Annex-B NAL header syntax. A malformed NAL header is a *heuristic*, not proof that an entire frame is incorrect. PES NAL scan is limited to AVC/HEVC PIDs identified in PMT and does not perform complete access-unit or decoder validation.
- **ECM/CW correlation:** completed valid Newcamd CW response latency, responses slower than 500 ms, peak latency and no-CW response counts; log per successful transaction (`CA V238 CW EVENT`) with SID, CHID, parity, duration, action and **never the CW bytes**. CA V236 normal CHID selection/rejection events remain available.
- One journal line per monitored service per second: `CA V238 MONITOR: stream=... sid=... in_cc_gaps=... post_pes_bad=... nal_bad=... ecm_over_500ms=...`. Counters are interval deltas (except `ecm_peak_ms_total`). The aggregate is per-service, allowing all eight channels to be observed without eight TVs.
- **Limitations:** the CA plugin sits *after* the transport source's selection and *before* final remuxing, rate pacing, SRT/UDP/HTTP output and the client decoder. It cannot prove the original RF input has no errors, determine packet loss on a viewer's network, verify every encrypted byte, or assert that all detected NAL anomalies are real picture breakup. Correlate with DVB tuner signal readings and a client-side TS capture when investigating an anomaly. A CC repeat can be a harmless transport duplicate.
- **CPU impact:** while enabled, every selected 188-byte TS packet is inspected twice (pre/post CSA) with small per-PID state; write volume ~8 summary lines per second, plus CW events on eight active services. Disable for continuous production operation.
- **Unit/CI tests:** `bash scripts/test_irdeto_cw_policy.sh`; `bash scripts/test_ts_integrity_monitor.sh`. Existing `.github/workflows/irdeto-v236-ci.yml` workflow is extended to cover V238 and attempt CPU-only full CMake compilation. CI cannot emulate a physical Irdeto smartcard or DVB-S2 receiver.
- Build on a separate Ubuntu host, or in a separate directory on `iptv` without touching installed production files:

```bash
git clone -b ca-irdeto-v10.8.238 https://github.com/Monkipnet/DVBStreamer5.git DVBStreamer5-v238
cd DVBStreamer5-v238
bash scripts/test_irdeto_cw_policy.sh
bash scripts/test_ts_integrity_monitor.sh
bash scripts/vendor_srt_source.sh
bash scripts/vendor_native_codecs.sh
DVBSTREAMER5_CODEC_JOBS=2 bash scripts/build_native_codecs.sh
cmake -S . -B build-v238 -DCMAKE_BUILD_TYPE=Release
cmake --build build-v238 --parallel 2
bash scripts/make_portable_linux_bundle.sh --build-dir build-v238 --binary build-v238/DVBStreamer5 --output-dir dist/DVBStreamer5-linux-x86_64 --with-oscam --archive
```

- **Read-only monitor retrieval after explicitly authorized deployment and one controlled service restart:**

```bash
journalctl -u dvbstreamer5.service --since '-20 min' -o short-iso --no-pager |
  grep -E 'CA V238 MONITOR:|CA V238 CW EVENT:|NEWCAMD IRDETO CHID' > /tmp/v238-ca-monitor.log
python3 scripts/analyze_ca_ts_monitor.py /tmp/v238-ca-monitor.log
```

**DO NOT copy the test binary/plugin over the live `/opt/DVBStreamer5` files or restart `dvbstreamer5.service` on `iptv` during routine build.** For a maintenance deployment, back up the binary, newcamd .so, config and service drop-ins and define rollback before restarting; only the operator should authorize the restart. Both CA plugin and matching main binary must come from the same build.

---

### V10.8.236 (TEST BRANCH) — per-service Irdeto CW buffer for single-card / multi-channel setups

- **Not a production release.** Developed separately on `ca-irdeto-v10.8.236`; `main` (V10.8.235) is unchanged. No reboot or restart of the production `iptv` host is required.
- An OSCam-mini instance using **one physical Irdeto smartcard** can serve multiple distinct encrypted services over per-service Newcamd TCP connections. Even when their reader is shared, EVEN/ODD control words are **never** shared between different service IDs. This implementation keeps a separate key pair and next-key buffer for each `ServiceBinding`.
- There was already a one-entry per-service exact ECM signature de-duplication and an in-flight ECM queue. A cache cannot predict future unknown CWs; the card still needs to process each new ECM. The ~153–176 ms observed at OSCam is normal for this deployment and not by itself proof of a CA problem.
- New **opt-in test mode** `DVBSTREAMER5_IRDETO_CW_BUFFER=1`: retain an active parity's current key while an updated CW is received; stage its next CW and activate after a TSC parity change returns to that parity. Flush outstanding CSA bitslice batches before switching keys. This is a *hypothesis* to diagnose brief 1–2 second visual breakup and **must be compared with the baseline**: some ECM/CAS timing may require different switching semantics.
- In opt-in mode, ignore all-zero Irdeto CW halves instead of replacing a previously working key. Skip unchanged CWs for a valid key. Non-Irdeto CAS paths and V10.8.235 behavior are untouched when the switch is unset.
- OSCam-mini UI activity indicator fix (independent of the experimental CW buffer): a local reader such as `Pervy1` is marked active when a successful `found (...) by Pervy1` ECM appears in a separate **last-45-second** journal slice. Previously the UI relied only on the startup-only `ready for requests` message, which rolls out of the last 240 lines on a busy eight-channel reader and falsely shows `Ожидание карты` even while decrypting. No OSCam settings or CA decryption logic are changed by this UI-only correction.
- With `DVBSTREAMER5_CA_DIAGNOSTICS=1`, log `NEWCAMD IRDETO CW BUFFER` and `NEWCAMD IRDETO CW ACTIVATE`, plus cumulative `irdeto_staged`, `irdeto_activated`, `irdeto_zero_skips`, and `irdeto_unchanged_skips` counters in `CA TS DIAG`. Only parity, SID, timing, and counters are logged; **never** control-word bytes.
- Autonomous regression test for the exact policy integrated into the Newcamd plugin: `bash scripts/test_irdeto_cw_policy.sh`. Tests are CPU-only, synthesize parity/CW transitions, need no smartcard/OSCam or tuner, and explicitly verify that CW caches from two services cannot overwrite each other. The standalone test was compiled and passed locally with GCC; this **does not** constitute a full DVBStreamer5 binary build or real-stream Irdeto validation.
- Isolated GitHub Actions workflow: `.github/workflows/irdeto-v236-ci.yml` runs the standalone policy test and separately attempts a CPU-only full DVBStreamer5 CMake build on this test branch. Its result is **not yet verified** here.
- **UI-only reader status fix:** check recent successful ECMs for a reader even when startup `ready for requests` rolled out of a short journal window. This fixes misleading `Ожидание карты` for the active `Pervy1` reader; no reader reset, OSCam restart or change of CA logic.
- Build on an isolated test server: `git fetch origin && git switch ca-irdeto-v10.8.236 && git pull --ff-only`, then use the project CMake and portable-bundle commands below. For test A/B set environment variables **only for the test process**. Do not install the test build or add systemd overrides to production `iptv`.
- Expected verification: parity transitions, PES validation, continuity counters, normal video and audio, same 8 services, and no new regressions. **No completed build or runtime video test is claimed for V10.8.236.**

### V10.8.235 — исправлена цветность NVDEC 1080i H.264 при сохранении GPU zero-copy

- **Подтверждено пользователем на GTX 1050, DAZN 1/2:** в V10.8.234 режим `NVDEC → CUDA(NV12) → NVENC`, `bob`, `second_field=0` устранил зелёный оттенок и цветные горизонтальные полосы. Журнал показал `host_video_copies=0`, `gpu_surface_copies=0`.
- Рабочая комбинация теперь **выбирается автоматически** при наличии чересстрочного H.264 и включённого деинтерлейсинга: `NVDEC Bob` + первое поле (`CUVIDPROCPARAMS.second_field=0`). В отличие от V10.8.234, `DVBSTREAMER5_NVDEC_FIRST_FIELD_TEST=1` и `DVBSTREAMER5_NVDEC_VPP_MODE=bob` **больше не нужны для этого сценария**.
- Для прогрессивного контента и других кодеков сохранён прежний режим Adaptive по умолчанию. Явный `DVBSTREAMER5_NVDEC_VPP_MODE=adaptive|bob|weave` имеет приоритет; выключенный деинтерлейсинг в плитке принудительно сохраняет Weave. Выбор первого поля по умолчанию активен для чересстрочных H.264-кадров при Bob (включая явно выбранный Bob).
- Логи: `NATIVE NVIDIA ZERO-COPY active ... deinterlace=bob auto_h264_bob=1 ... host_video_copies=0 gpu_surface_copies=0`. При `DVBSTREAMER5_NVDEC_VPP_PROBE=1` дополнительно `field_policy=first_h264_bob second_field=0` для чересстрочных H.264 кадров.
- **После установки удалить временные systemd overrides** из прошлых диагностических тестов (после сохранения их резервной копии): `30-nvdec-safe-copy.conf`, `40-nvdec-vpp-test.conf`, `90-nvdec-uv-debug.conf`, `98-h264-capture.conf`, `99-nvenc-input-test.conf`, `zz-nvdec-field-ab.conf` в `/etc/systemd/system/dvbstreamer5.service.d`; затем `systemctl daemon-reload && systemctl restart dvbstreamer5.service`. Это возвращает обычную работу и исключает случайное включение диагностического `NVDEC→CPU(NV12)→NVENC`.
- Пути Ittiam → NVENC, Intel VAAPI/QSV, обычный NVENC, OSCam-mini, EPG, subtitles и TS mux не менялись. Итоговую сборку V10.8.235 и воспроизведение **без** overrides следует отдельно проверить на сервере.

### V10.8.234 — A/B-проверка поля для NVDEC Bob по образцу FFmpeg CUVID

- Сравнение одного и того же H.264 DAZN 1 (PID 2047) через `ffmpeg -c:v h264_cuvid -deint bob -drop_second_field 1 -resize 1280x720` и программный FFmpeg показало близкие RGB: общая средняя абсолютная разница 8.80/255, средние цвета по 10 горизонтальным зонам почти одинаковы. Это не гарантирует отсутствия узких полос, но аппаратный CUVID способен вывести изображения с почти той же средней цветностью.
- Анализ исходников установил конкретную разницу: FFmpeg при `-drop_second_field 1` подаёт в `cuvidMapVideoFrame` `CUVIDPROCPARAMS.second_field=0`, а штатный путь DVBStreamer5 (по примеру NVIDIA NvDecoder) — `repeat_first_field + 1` (обычно 1). Это **гипотеза**, а не подтверждённое исправление дефекта.
- Добавлен безопасный отключаемый A/B режим `DVBSTREAMER5_NVDEC_FIRST_FIELD_TEST=1`: только при выбранном deinterlace и VPP режиме Bob/Adaptive принудительно `second_field=0`. По умолчанию поведение не изменено. Проверка осуществляется на одном канале, а в логе `NATIVE NVIDIA ZERO-COPY active` будет `nvdec_first_field_test=1`.
- Чтобы тест был чистым, одновременно установите `DVBSTREAMER5_NVENC_SOFTWARE_INPUT_TEST=0`, `DVBSTREAMER5_NVDEC_SAFE_COPY=0`, `DVBSTREAMER5_NVDEC_DIAG_CHROMA=off`, `DVBSTREAMER5_NVDEC_VPP_MODE=bob`; временно отключите захват H.264. После визуального A/B сравнения первого поля удалите тестовый systemd override и перезапустите сервис.
- Обычные Ittiam → NVENC, Intel, OSCam-mini, EPG, subtitles и оптимизация mux не менялись. Успешность полного CMake build и исправления цвета на GTX 1050 требует проверки на сервере.

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
