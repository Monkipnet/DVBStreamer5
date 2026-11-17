# WISI Chameleon EPG UDP V10.8.568 — RU / EN / HR

## RU
Меню **Вещание → EPG UDP для WISI**: создайте выход, задайте IP, порт и интерфейс.
Выберите Actual (EIT p/f 0x4E, schedule 0x50–0x5F) или Barker/Other
(EIT p/f 0x4F, schedule 0x60–0x6F). Выберите плитки и укажите
**реальные выходные** SID/TSID/ONID соответствующих каналов WISI.
В плитке включите EPG и выберите DVB EIT или XMLTV; внешний источник
настройте через «Источники XMLTV». Настройки отключены по умолчанию и
не перезапускают плитки, тюнеры и OSCam при изменении.

Выход — отдельный UDP MPEG-TS с PID 0x0012 (EIT), пакет 188 байт,
7 TS пакетов/датаграмму с заполнением null-пакетами. **Важно:**
это EIT-only поток, без PAT/PMT/SDT/TDT/TOT и без аудио/видео.
Не все прошивки WISI могут принять EIT-only как Barker TS:
на устройстве должен быть доступен режим внешнего источника EIT.
Подтверждение на настоящем Chameleon ещё не выполнено.

Кнопка «Яндекс ТВ (XMLTV)» создаёт отдельный **отключённый**
источник XMLTV, для которого требуется указать разрешённый URL
предоставления программы. Публичный API Яндекса закрыт; прямой
парсер сайта tv.yandex.by в этой версии **не реализован**.
После загрузки разрешённого XMLTV каналы появятся в каталоге и
их можно привязать к плиткам.

## EN
Broadcast → WISI EPG UDP: standalone opt-in EIT PID 0x0012,
Actual (0x4E/0x50) or Barker/Other (0x4F/0x60).
Configure UDP IPv4, destination port, network interface, tiles and
their effective WISI SID/TSID/ONID. EPG comes from the existing per-tile
DVB EIT/XMLTV selection. This is an EIT-only TS without PAT/PMT/SDT;
WISI firmware must support standalone EIT input; device testing is pending.
The Yandex TV entry needs an authorized XMLTV feed URL, not direct scraping.

## HR
Emitiranje → EPG UDP za WISI: zaseban opcionalni izlaz s EIT PID 0x0012.
Načini: Actual (0x4E/0x50) ili Barker/Other (0x4F/0x60).
Odaberite IPv4 adresu, UDP port, sučelje, kanale i izlazni
SID/TSID/ONID sustava WISI. EPG dolazi iz postojećeg DVB EIT/XMLTV
izvora. Izlaz nema PAT/PMT/SDT; potrebna je podrška za vanjski EIT.
Za Yandex TV treba dopušten XMLTV URL; izravno čitanje stranice
nije implementirano.
