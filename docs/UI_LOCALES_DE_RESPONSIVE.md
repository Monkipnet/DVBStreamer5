# Localization and responsive dialogs — version unchanged 10.8.580

The user-facing language selector is a native dropdown: RU, EN, HR, DE. It always displays the current selected language, persists in localStorage, and uses the existing server preference update. RU/EN/HR catalogs remain intact. German is supplied for core navigation, common actions, networking, subscribers, stream controls and search; untranslated technical strings intentionally fall back to English.

The runtime localization pipeline includes dynamically created dialog text and bilingual attributes. Responsive CSS covers header controls, modal widths, form grid reflow, scrollable tables, long labels, and 560/900px breakpoints.

Removed temporary Newcamd/ECM latency telemetry from four OSCam-mini source modules (without changing protocol handling or the default 8000ms 098D grace). Existing regular error logs and standalone historic diagnostic utilities are preserved.

Tests include JavaScript syntax compilation, source checks of selector/locales/CSS and no verbose tracing. This does **not** replace visual browser review of every dialog at 320/768/1280px; no screenshot-based browser run has been performed here.
