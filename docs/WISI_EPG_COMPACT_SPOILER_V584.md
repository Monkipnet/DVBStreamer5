# WISI EPG compact accordion — V10.8.584

## RU
Плитки WISI EPG теперь высотой 184 px вместо общих для медиа-плиток 252 px. Информация остаётся сверху, а кнопки Старт/Стоп и Ред. вместе с галочкой Автостарт прижаты к нижнему краю. При узкой ширине галочка размещается отдельной нижней строкой без обрезки кнопок.

Группа теперь оформлена как спутниковый спойлер: заголовок **WISI EPG**, крупная стрелка по центру (▼ / ▲), справа счётчик активных/всех EPG-выходов. Заголовок кликабелен, состояние раскрытия сохраняется в браузере при перерисовке и перезагрузке страницы. Во время поиска совпадающая группа автоматически раскрывается; после очистки поиска восстанавливается выбранное оператором состояние. Надпись «отдельные плитки» убрана со всех языков.

## EN / HR / DE
WISI EPG tiles use compact 184px height with Start/Stop, Edit and Autostart aligned to the bottom. A DVB-style collapsible WISI EPG group has a centered expand/collapse arrow, active/total counter, remembered state and search-aware opening. The former "separate tiles" heading is removed in all four languages.

No WISI output, UDP, EIT or service configuration logic was changed. GitHub Actions runs node tests/web/test_wisi_epg_compact_spoiler_v584.js and existing regression tests. Real-browser visual inspection is still recommended.
