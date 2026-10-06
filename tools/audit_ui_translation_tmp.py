from pathlib import Path
import re

src = Path('src/HttpServer.cpp').read_text(encoding='utf-8')

# Build the exact Russian -> English runtime map already used by translateUiText().
map_match = re.search(r"const uiRuToEn = new Map\(\[(.*?)\n\]\);", src, re.S)
if not map_match:
    raise SystemExit('uiRuToEn map not found')
map_block = map_match.group(1)
known = set()
for m in re.finditer(r"\['((?:\\.|[^'])*)',\s*'((?:\\.|[^'])*)'\]", map_block):
    known.add(m.group(1).replace("\\'", "'"))

# Ignore the translation declarations themselves; audit the actual generated UI/runtime JS.
audit_src = src[:map_match.start()] + ('\n' * map_block.count('\n')) + src[map_match.end():]

# Ignore the canonical translations.ru declaration because it is intentionally Russian.
for pattern in [
    r"const translations = \{.*?\n\};",
    r"Object\.assign\(translations\.ru, \{.*?\n\}\);",
]:
    audit_src = re.sub(pattern, lambda m: '\n' * m.group(0).count('\n'), audit_src, flags=re.S)

cyr = re.compile(r'[А-Яа-яЁё]')
rows = []
for lineno, line in enumerate(audit_src.splitlines(), 1):
    if not cyr.search(line):
        continue
    # Keep only browser/UI source. Server-side Russian messages above the HTML page are not UI localization candidates.
    if lineno < 1000:
        continue
    compact = line.strip()
    if len(compact) > 600:
        compact = compact[:600] + ' ...'
    rows.append((lineno, compact))

out = []
out.append(f'known uiRuToEn exact entries: {len(known)}')
out.append(f'Cyrillic-bearing UI/source lines outside translation declarations: {len(rows)}')
out.append('')
for lineno, line in rows:
    out.append(f'{lineno}: {line}')
Path('UI_TRANSLATION_AUDIT.tmp').write_text('\n'.join(out) + '\n', encoding='utf-8')
print('\n'.join(out[:20]))
