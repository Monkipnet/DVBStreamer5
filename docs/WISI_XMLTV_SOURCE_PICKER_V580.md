# WISI EIT external XMLTV source picker (V10.8.580, version unchanged)

## RU
**Вещание → EPG UDP для WISI → + Выход → + Внешний EPG-канал**.

Выберите **Источник XMLTV** из настроенных источников по имени (как в редакторе плитки). Введите часть названия или XMLTV ID в поле **Поиск по названию или ID** и выберите совпадение в списке (до 150 результатов одновременно); выбранный XMLTV ID будет записан в отдельное поле **XMLTV ID канала**. При необходимости ID можно ввести вручную, даже когда канал не появился в каталоге. Укажите реальные SID/TSID/ONID мультиплекса WISI и сохраните.

Каналы выбранного источника теперь запрашиваются отдельно через `GET /api/xmltv/channels?source_id=<id>`. Существующий `GET /api/xmltv/channels` сохранён без изменений для других окон. Это предотвращает ситуацию, при которой первые 15 000 каналов других источников вытесняют нужный источник. Источник может иметь до 15 000 каналов в действующей реализации парсера; просмотр/поиск осуществляется по всем загруженным каналам источника, но на экране показываются максимум 150 совпадений. Кнопка «Обновить список» перечитывает каталог из DVBStreamer5; скачивание нового XMLTV-файла выполняется отдельно в окне «Источники XMLTV».

Если источник ещё не загружен, можно выбрать его из списка и вручную указать ID канала. Это не создаёт IPTV-плитку. Отправляется только EIT PID 0x0012 (18), с возможным дополнением null-пакетами, в режиме Actual либо Barker/Other. Для работы в WISI должны совпадать SID/TSID/ONID, а прошивка должна поддерживать внешний EIT.

## EN
Choose an XMLTV **source by name**, then search its **channel name or ID** and pick a matching result. You can enter channel ID manually. Scoped source loading avoids losing channels when there are multiple large XMLTV feeds. The selection and form labels are translated into RU/EN/HR/DE.

## Verification
CI includes a JavaScript emulation with **two separate 15,000-channel XMLTV feeds**, checks searching for a channel near the end of the second feed, source isolation, bounded DOM option count, four-language strings and UI script syntax. A live WISI hardware test remains necessary.
