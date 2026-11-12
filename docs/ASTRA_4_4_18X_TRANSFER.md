# Cesbo Astra 4.4.18x (Lua) configuration transfer — V10.8.484

Astra **4.4.18x**, including the published 4.4.182 syntax, uses Lua:
`adapter_0 = dvb_tune({ ... })` and `make_channel({ ... })`.
This is **different** from the modern Cesbo Astra 5.x
`make_stream` JSON file.

## Web UI

**Menu → Administration → Configuration**

Export → format:
- DVBStreamer5 full backup JSON
- Cesbo Astra 5.x JSON
- **Cesbo Astra 4.4.18x Lua (.conf)**
- Flussonic (.conf)

Import → format: choose **Cesbo Astra 4.4.18x Lua (.conf)**, select
a file, click Preview Import, inspect warnings and imported/duplicate
counts, and only then Apply Import.

This version imports and exports literal `dvb_tune({ ... })` and
`make_channel({ ... })` objects. The parser deliberately **never
executes Lua** or interprets expressions, variables, loops, function
calls, external files, Newcamd/BISS keys or generated configuration.
Nonliteral calls are skipped with an explicit warning.

Supported migration:
- SPTS channels: names, primary input and one backup URL;
- DVB-S/S2 adapter/frontend/tuning (TP, frequency, polarization,
  symbol rate, standard LNB/DiSEqC, explicit SID `#pnr`);
- DVB-T/T2 basic frequency with bandwidth/modulation review warning;
- UDP/RTP, HTTP(S), file-based backup, and supported IP inputs;
- UDP/RTP outputs, including IPv4 `interface@multicast:port`, plus
  secondary UDP/RTP destinations;
- limited SDT service name/provider metadata.

Unsupported or manual review:
- DVB-C/ATSC/ASI, SAT>IP, certain custom DVB tuning options,
  advanced DVB-T2 bandwidth, Unicable, custom PID mapping,
  scrambled access and keys, complex failover, HTTP playback,
  monitoring, EPG, recording, transcoding and dynamic Lua scripts.
- Server-wide options and credentials are **not** imported.
- Unsupported outputs and parameters are reported; there is no
  promise of lossless full-server migration.

**Safety and continuity:** imports append only new tiles with
`auto_start=false`; they never overwrite or restart existing
pipelines, CAM clients or subscribers. Sources with no supported
literal input are skipped, never launched automatically.

## Example legacy file

```lua
adapter_0 = dvb_tune({
  adapter = "0",
  device = "0",
  type = "S2",
  tp = "11836:H:27500",
  lnb = "9750:10600:11700",
})

make_channel({
  name = "Example TV",
  input = {
    "dvb://adapter_0#pnr=142",
    "http://backup.example.net/channel.ts",
  },
  output = {
    "udp://192.168.1.5@239.11.12.13:5000",
  },
  enable = false,
})
```

This is a structural example, not a validated broadcast frequency or a
ready-to-use production channel.

### Export

Legacy export emits a parseable Lua configuration with the appropriate
`dvb_tune` blocks for representable DVB-S/S2 inputs and
`make_channel` blocks for IP/DVB channels. Lua strings are escaped,
and existing Astra CAM passwords, middleware and licensing settings
are not generated. Check adapter numbers, tune settings, audio/video
selection, output routes and CBR characteristics before deploying
the result to an Astra instance.

### Build and validation (source root)

```bash
git switch main
git pull --ff-only origin main
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build -R '^external_config_transfer$' --output-on-failure
node tests/web/test_external_config_transfer_ui.js
ctest --test-dir build --output-on-failure
```

The CTest case includes round-trip conversion, DVB-S2/PNR, secondary
IP outputs, interface binding and a dynamic-Lua rejection test.

## Source documentation

- Legacy Astra 4.4.182 reference:
  https://cdn.cesbo.com/astra/4.4.182-free/
- Modern Cesbo Astra stream API:
  https://astra.cesbo.com/en/api/stream/
