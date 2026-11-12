# V10.8.504 — softer green for TSDecrypt PES evidence

A successfully parsed output PES on TSDecrypt is a useful positive signal,
but is not as strong as actual video-frame presentation in the browser.

- Display `ДЕКОД: PES` using a **lighter, mint-green** pill (`pes` class)
  instead of the earlier amber waiting style.
- Keep `ДЕКОД: ОК` in the existing darker green class; this still requires
  actual recent preview frame presentation or other non-TSDecrypt state.
- Keep `ДЕКОД: ЧАСТ.` in amber and `ДЕКОД: НЕТ` in red.
- Add matching light-dashboard styling with legible, darker green text.
- No changes to CA transport, ECM/CW handling, preview functionality,
  video decode, backend states, or output protocols.

Added CI assertions for both dark/light PES CSS and state mapping.
