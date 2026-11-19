# Localization and responsive dialogs — version unchanged 10.8.580

The user-facing language selector is a native dropdown: RU, EN, HR, DE. It always displays the current selected language, persists in localStorage, and uses the existing server preference update. RU/EN/HR catalogs remain intact. German is supplied for core navigation, common actions, networking, subscribers, stream controls and search; untranslated technical strings intentionally fall back to English.

The runtime localization pipeline includes dynamically created dialog text and bilingual attributes. Responsive CSS covers header controls, modal widths, form grid reflow, scrollable tables, long labels, and 560/900px breakpoints.

Removed temporary Newcamd/ECM latency telemetry from four OSCam-mini source modules (without changing protocol handling or the default 8000ms 098D grace). Existing regular error logs and standalone historic diagnostic utilities are preserved.

Tests include JavaScript syntax compilation, source checks of selector/locales/CSS and no verbose tracing. This does **not** replace visual browser review of every dialog at 320/768/1280px; no screenshot-based browser run has been performed here.

## V10.8.581 — compact language picker and localized search examples

Permanent translation policy: every new or modified operator-facing text, label, help string, placeholder, modal, menu and notification must be supplied in **RU / EN / HR / DE**. Historical German translations still use the English fallback when an explicit translation is absent; this is not full translated coverage.

The header selector is 68px wide with EN/RU/HR/DE labels. The accessible language name stays available to screen readers. Each search placeholder displays a short example in the selected language, refreshed immediately without changing the active filter. The full description of searchable channels, device names, IPs, domains and subnets remains in the localized accessible search label.

Runtime localization must not overwrite explicit language-specific placeholders. CI includes an emulated language switch checking all four search examples, accessible names and persisted selection. Browser layout and live operator interaction still require separate testing.
