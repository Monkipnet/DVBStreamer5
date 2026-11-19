# WISI EPG configuration tiles — V10.8.580 (version unchanged)

## RU — порядок работы

**Вещание → EPG UDP для WISI → + Выход**. Укажите имя будущей плитки, включите UDP EPG, заполните **IP назначения**, **UDP-порт**, **интерфейс** и **режим таблиц** (Actual или Barker/Other). **На этом этапе каналы выбирать не требуется.** Нажмите «Сохранить».

На панели появится отдельная плитка WISI EPG (не IPTV/DVB-медиапоток). До назначения первого канала состояние — «Ожидание каналов». В ней отображаются IP:порт, интерфейс, режим таблиц, количество EPG-каналов и кнопка **Редактировать**.

Нажмите **Редактировать**. Откроется список назначенных каналов с их XMLTV-источником и ID, SID/TSID/ONID, кнопками **Редактировать** и **Удалить**. Нажмите **Добавить канал**, выберите ранее настроенный XMLTV-источник, найдите канал по имени или ID (или введите ID вручную), задайте действующие в WISI SID/TSID/ONID и сохраните. Изменение/удаление канала немедленно записывается в общий конфиг EPG UDP. Для удаления выводится подтверждение. В меню настройки выхода можно изменить IP/порт/интерфейс/имя/режим или удалить выход — без потери его существующих назначений при обычном сохранении.

Плитка хранится в `epg_udp_outputs`, не создаёт запись в `streams` и не запускает тюнер, демультиплексор, транскодирование, OSCam и прочие медиаконвейеры. Пустая плитка не запускает даже EIT UDP thread; после добавления первого канала включённого выхода запускается уже имеющийся генератор PID **18 / 0x0012**. Запрос `/api/state` передаёт только краткие метаданные WISI-плиток (без полного списка каналов) для экономии трафика и RAM. Полная конфигурация загружается лишь при редактировании.

Если в старой конфигурации были привязаны EPG плиток DVBStreamer5, они сохраняются и отображаются в списке; редактирование сохраняет прежний stream ID и позволяет изменить SID/TSID/ONID.

## EN / HR / DE
Create an empty WISI EPG tile from Broadcast → EPG UDP for WISI by selecting name, enabled, destination, port, interface, EIT mode and Save. Add XMLTV channels only via **Edit** on that tile. Individual channel actions are **Edit** and **Delete** with confirmation. A virtual tile does not start a media stream. RU/EN/HR/DE labels are included.

## Verification
The CI regression emulates saving an existing 2-channel output via the configuration menu and checks both mappings remain intact, asserts empty-output backend support, confirms separate tile rendering and verifies channel edit/delete routes. Hardware validation on the WISI Chameleon remains necessary.
