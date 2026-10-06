from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one match, got {count}")
    return text.replace(old, new, 1)

# Version bump.
p = Path('src/AppVersion.h')
s = p.read_text(encoding='utf-8')
s = replace_once(s, 'kProgramVersion = "10.8.112"', 'kProgramVersion = "10.8.113"', 'version')
p.write_text(s, encoding='utf-8')

p = Path('src/HttpServer.cpp')
s = p.read_text(encoding='utf-8')

# Fill gaps found by the translation audit. Runtime localization translates newly
# inserted DOM text/attributes, so keeping these in the central map also covers
# modal content generated after the user switches language.
anchor = "  ['Подключение к HTTP-потоку…', 'Connecting to HTTP stream…']\n]);"
extra = """  ['Подключение к HTTP-потоку…', 'Connecting to HTTP stream…'],
  ['Имя абонента, IP адрес, Статус', 'Subscriber name, IP address, Status'],
  ['HD ПОТОКИ', 'HD STREAMS'],
  ['HD + SD ПОТОКИ', 'HD + SD STREAMS'],
  ['SD ПОТОКИ', 'SD STREAMS'],
  ['БЕЗ МЕТКИ HD/SD', 'WITHOUT HD/SD LABEL'],
  ['Нативный DVB-S/S2 frontend доступен только в Linux.', 'Native DVB-S/S2 frontend is available only on Linux.'],
  ['Выходной протокол', 'Output protocol'],
  ['Режим каналов транспондера', 'Transponder channel mode'],
  ['Формировать CBR MPEG-TS', 'Generate CBR MPEG-TS'],
  ['Глобально для всех выбранных каналов. OnDemand запускает поток и CAM только при HTTP/HLS клиенте; UDP/RTP/SRT/RTSP/RTMP не могут определить пассивного получателя.',
   'Applies to all selected channels. OnDemand starts the stream and CAM only when an HTTP/HLS client connects; UDP/RTP/SRT/RTSP/RTMP cannot detect a passive receiver.'],
  ['Первый RTP порт', 'First RTP port'],
  ['Первый SRT порт', 'First SRT port'],
  ['Первый RTSP порт', 'First RTSP port'],
  ['Предпросмотр должен выдаваться веб-сервером DVBStreamer (same-origin)', 'Preview must be served by the DVBStreamer web server (same-origin)']
]);"""
s = replace_once(s, anchor, extra, 'translation map extension')

# Tile status is operator-facing. Keep the complete backend status for logs,
# diagnostics, Telegram and quality history, but hide the implementation-detail
# word "native" from the compact tile line.
translate_tail = """  return lead + text + tail;
}

function localizeUiTextNode(node) {"""
helper = """  return lead + text + tail;
}

function tileRuntimeStatus(value) {
  return String(value ?? '')
    .replace(/\\bnative\\s+/gi, '')
    .replace(/\\s{2,}/g, ' ')
    .trim();
}

function localizeUiTextNode(node) {"""
s = replace_once(s, translate_tail, helper, 'tile status helper')

old_update = """  const runtimeStatus = tile.querySelector('[data-role=\"runtime-status\"]');
  if (runtimeStatus) {
    const value = String(stream.status || '').trim();
    runtimeStatus.textContent = value;
    runtimeStatus.title = value;
  }"""
new_update = """  const runtimeStatus = tile.querySelector('[data-role=\"runtime-status\"]');
  if (runtimeStatus) {
    const value = tileRuntimeStatus(stream.status);
    runtimeStatus.textContent = value;
    runtimeStatus.title = value;
  }"""
s = replace_once(s, old_update, new_update, 'live tile runtime status')

old_template = """<span data-role=\"runtime-status\" class=\"runtime-status\" title=\"${escapeHtmlValue(stream.status || '')}\">${escapeHtmlValue(stream.status || '')}</span>"""
new_template = """<span data-role=\"runtime-status\" class=\"runtime-status\" title=\"${escapeHtmlValue(tileRuntimeStatus(stream.status))}\">${escapeHtmlValue(tileRuntimeStatus(stream.status))}</span>"""
s = replace_once(s, old_template, new_template, 'initial tile runtime status')

# One dynamic fragment is not an exact text node because the level number is appended.
s = replace_once(
    s,
    "    ['нет данных', 'no data'], ['звук включается в плеере', 'audio can be enabled in the player']\n",
    "    ['нет данных', 'no data'], ['звук включается в плеере', 'audio can be enabled in the player'], ['уровень ', 'level ']\n",
    'dynamic level translation')

p.write_text(s, encoding='utf-8')
print('V10.8.113 UI transform applied')
