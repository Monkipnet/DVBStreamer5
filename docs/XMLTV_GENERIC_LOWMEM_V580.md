# DVBStreamer5 — Generic XMLTV only, low-memory EPG (V10.8.580)

**Scope:** XMLTV provider configuration, IPTV tile EPG and standalone WISI EIT PID 0x0012. No stream encoding, SRT, OSCam or tuner changes.

## RU
Убраны кнопка и код провайдерского плагина из окна «Источники XMLTV». Используются только пользовательские HTTP(S) XMLTV-источники, добавленные в приложении. Ранее сохранённые источники не удаляются автоматически, чтобы не потерять настройки пользователя: при необходимости их можно удалить из списка «Источники XMLTV».

В плитке и в WISI EPG UDP источник выбирается из ранее добавленных источников. Для WISI предусмотрены поиск по названию/ID, ограниченный список результатов и ручной XMLTV ID. В браузере хранится не более **двух** полных каталогов источников; остальные загружаются при поиске/выборе. Это предотвращает накопление всех XMLTV-каталогов в браузере.

В сервере XMLTV-файл читается последовательно через libxml2 и временный файл, без загрузки целиком в RAM. Для **каждого XMLTV-источника** хранятся события только каналов, на которые настроены плитки или включённые внешние EIT-выходы WISI. При одинаковом XMLTV ID разных провайдеров программа передач не дублируется, если выбран конкретный источник. В режиме плитки «Любой источник» сохранено прежнее правило поиска по всем активным источникам. При разборе ненужных программ парсер не извлекает строки заголовка/описания и не разбирает время. Каталог названий и ID остаётся для выбора каналов, включая провайдеров без отдельных `<channel>` элементов.

Кэш на диске сохраняет только выбранные EPG-события и полный каталог имён. Новое назначение программы передач может потребовать обновления источника; действующие плитки и выходы при этом не перезапускаются.

## EN
Removed the special TV provider plugin entry. Use only configured XMLTV URLs. Server retention is source-scoped to assigned channels, with streaming XML parsing and skipped descriptions for unassigned programmes. Browser caches at most two XMLTV catalogs for the WISI picker. Existing configured sources are not silently deleted.

## Validation
The GitHub Actions pipeline runs memory-retention policy checks, EIT external XMLTV tests and an emulated two-source/15k-channel selector test. Source-level assertions are not a measured RSS benchmark: verify RAM before/after on `iptv` under comparable workload.
