# DVBStreamer5 V10.8.590 — DVB-T2: три канала на одной частоте, UDP + HTTP

## Сообщение оператора (20.11.2026)

При запуске третьего DVB-T2 канала на adapter2/frontend0:
`DVB frontend adapter2/frontend0 is already tuned to another transponder`.
Два UDP-канала запускались, HTTP-канал блокировался.

## Корневая причина, подтверждённая анализом кода

`src/media/SharedDvbInputPool.cpp` использует единственный физический reader на DVB frontend. До V590 его ключ транспондера сравнивал и наземные, и спутниковые параметры (FEC, LNB, DiSEqC, SR, modulation). Параметры спутниковой антенны и FEC не определяют настройку DVB-T2; terrestrial driver принудительно использует FEC_AUTO. Поэтому два канала одного DVB-T2 мультиплекса с разными историческими/импортированными параметрами могли получить разные ключи и ошибка ошибочно утверждала, что занят «другой транспондер».

Тип выхода HTTP или UDP **не участвует в идентичности DVB-транспондера**. Разница в конфигурации входа, а не принципиальная несовместимость HTTP и UDP.

## Исправление

Для DVB-T и DVB-T2 идентичность общего источника теперь определяется:
- физическим адаптером и frontend;
- частотой (kHz);
- шириной полосы (Hz);
- системой DVB-T или DVB-T2;
- PLP / `stream_id` в `dvb://` URI.

Незначимые спутниковые параметры, FEC и различия AUTO/явная модуляция не разделяют наземный мультиплекс на отдельные источники. Параметры вывода, service ID, PID, jitter и формат HTTP/UDP не влияют на ключ.

**Безопасность:** Разные частоты, полоса, DVB-T/DVB-T2, DVB-T2 PLP ID и разные тюнеры остаются разными ключами. PLP = -1 (автоматический выбор) и явный PLP = 0 тоже намеренно не объединяются: невозможно гарантировать, что драйвер выберет PLP 0. Спутниковый ключ сохранён без изменений, включая DiSEqC/Unicable.

При конфликте в журнал добавлены `SHARED DVB TUNE CONFLICT` с физическими параметрами текущего и запрошенного наземного мультиплекса — без Unicable PIN и секретов.

## Эмуляция CI

`tests/test_shared_dvb_t2_key_v590.cpp` проверяет последовательность двух UDP + одного HTTP на одном frontend, отличия несущественных полей, блокировки разных RF/BW/PLP и неизменность спутникового ключа. Эмуляция не заменяет реальный тест драйвера.

Из корня исходников:

    git switch main
    git pull --ff-only origin main
    cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
    cmake --build build-release --parallel 2
    ctest --test-dir build-release -R shared_dvb_t2_mixed_outputs --output-on-failure

## Проверка на реальном сервере с DVB-T2

1. Установить V10.8.590 на тестовый сервер.
2. Выключить три канала на adapter2/frontend0.
3. Сверить настройки входа: frequency_khz, bandwidth_hz, delivery_system и stream_id (PLP) должны совпадать.
4. Включить два UDP-канала, затем HTTP-канал — все должны оставаться одновременно активными.
5. Проверить журнал:
   `journalctl -u dvbstreamer5 --since '10 minutes ago' --no-pager | grep -E 'SHARED DVB (SOURCE|subscriber|TUNE CONFLICT)|NATIVE DVB TERRESTRIAL TUNE'`

Если в новых сообщениях `SHARED DVB TUNE CONFLICT` видна разница в PLP (-1 vs 0 или 0 vs 1), ширине полосы или частоте, исправить настройки входа на соответствующие реальному мультиплексу; такие конфликты не следует обходить.

## English

DVB-T/T2 shared input now keys only physical frontend, RF, bandwidth, delivery system and PLP. Two UDP channels plus HTTP output on one mux are accepted in emulation, satellite frontend isolation unchanged. Different RF/BW/PLP remain blocked. Hardware verification is still required.
