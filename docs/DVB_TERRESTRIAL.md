# DVBStreamer5 — DVB-T / DVB-T2 on Linux

Native DVB terrestrial reception uses the **same** shared MPEG-TS demux, PAT/PMT
service selection, output pipelines and optional transcoding as DVB-S/S2.
The program does **not** use FFmpeg or GStreamer to tune or ingest transport streams.

## Supported Linux DVB frontends

Any Linux DVBv5 frontend that advertises `SYS_DVBT` or `SYS_DVBT2` through
`DTV_ENUM_DELSYS` may be used; there is **no TBS model whitelist**. The kernel
driver and firmware must first create `/dev/dvb/adapterN/frontendM`.
The native application cannot install or replace kernel DVB drivers.

Manufacturer-listed examples (not an exhaustive certification list):

| Model | Advertised reception | Hardware |
|---|---|---|
| TBS6205 (pictured) | DVB-T/T2/C | PCIe, 4 tuners |
| TBS6205se | DVB-T/T2/C2/C and other standards | PCIe, 4 tuners |
| TBS6209 / TBS6209se | DVB-T/T2/C2/C and other standards | PCIe, 8 tuners |
| TBS6216 | DVB-T/T2/C2/C and other standards | PCIe, 16 tuners |
| TBS6281 / TBS6281SE | DVB-T/T2 | PCIe, 2 tuners |
| TBS6284 / TBS6285 | DVB-T/T2 | PCIe, 4 tuners |
| TBS6220 | DVB-T/T2 | PCIe, 1 tuner |

See https://www.tbsdtv.com/download/index.html?path=3_14 for current vendor
driver packages, exact card revision and supported host kernel ranges. The
manufacturer's offline driver package V20250428 lists Linux kernels 4.19–6.12.
Other manufacturers' cards are handled identically once Linux exposes a working
DVBv5 frontend with DVB-T/T2 capability.

**Scope:** DVB-T/T2 only. DVB-C/C2/ISDB-T/ATSC require separate tuning implementations,
even when the physical TBS6205 supports those delivery standards.

## Add channel

Choose **Add Channel → Reception type → DVB-T/T2**. Select an advertised
adapter/frontend, then DVB-T or DVB-T2, RF center frequency in MHz, channel
bandwidth, modulation (Auto recommended), and DVB-T2 PLP ID (or -1 for Auto).
DVB-T does not use a PLP ID; satellite LNB, DiSEqC, symbol-rate and Unicable
controls are inapplicable and are hidden.

Scan one entered RF multiplex for PAT/PMT services. Save selected channels
to the existing outputs (HTTP, HLS, SRT, UDP, etc.). Blind frequency-band auto
scanning is **not** included.

A sample, **not a locally verified frequency**:
```
dvb://terrestrial?adapter=0&frontend=0&frequency_khz=666000&bandwidth_hz=8000000&delivery_system=dvb-t2&modulation=auto&stream_id=-1&pids=8192
```
Frequency is persisted as kHz in the DVBStreamer5 URI, then passed to the
Linux DVB API as **Hz**; `bandwidth_hz` uses Hz directly. `stream_id` means
T2 PLP and is not the MPEG-TS service ID (SID).

## Safe diagnostics (does not retune a working frontend)

```bash
ls -la /dev/dvb/adapter*/frontend* 2>/dev/null
find /sys/class/dvb -maxdepth 2 -name '*frontend*' -print
dmesg | grep -Ei 'dvb|tbs|frontend|firmware' | tail -60
```

Check available delivery systems in the web **Add Channel** adapter list,
and test signal/scan on a **free** frontend, not one used by live services.
For a real hardware acceptance test verify: frontend detection, T2 and T
LOCK, scan/PMT services, 2+ services shared on one multiplex, PLP selection,
HTTP/HLS and UDP/SRT outputs, OnDemand, signal/quality, and DVB-S2 regression.

Hardware reception on a specific card/driver is **not considered verified**
until those checks pass on the machine with the adapter.
