# DVBStreamer5 Operator Manual

**Edition:** V10.8.527 · November 14, 2026  
**Project:** [Monkipnet/DVBStreamer5](https://github.com/Monkipnet/DVBStreamer5) · `main`

Practical broadcast-operator guide transcribed from the corresponding 14-page PDF. This document preserves the V10.8.527 edition; individual UI labels may differ in later builds. The diagrams in the PDF are conceptual, not screenshots.

## 1. Sign in and navigate

*Where to find it: Browser / DVBStreamer5 web panel*

DVBStreamer5 is managed in a browser. Operators do not need a terminal: channels, sources, outputs, and additional modules are available through the web interface.

### How to use it

1. Open the panel address provided by your administrator. A typical installation uses http://SERVER-ADDRESS:9000/.
2. Enter your assigned account and password. Change any initial/default password when you first sign in.
3. Open the dashboard with channel tiles. Use the menu to access DVB, network monitoring, EPG, OSCam-mini, and updates.

### Controls and fields

| Control / field | What it does |
|---|---|
| Dashboard | Channel tiles, statuses, and start/stop controls. |
| Edit channel | Source, output, backup, and encoding settings for an individual channel. |
| DVB / Transponders | Satellite reception settings and shared tuner resources. |
| Monitoring | Channel metrics, network connections, and subscribers. |
| System sections | EPG/XMLTV, optional modules, settings, and updates. |

> Available sections depend on your account permissions. Do not change another operator’s channels without approval.

## 2. Dashboard and channel tiles

*Where to find it: Dashboard / the relevant channel tile*

Each tile represents a channel or stream. It provides start/stop and edit actions and shows the currently selected source.

### How to use it

1. Find the tile by channel name. If you manage many IP channels, use their displayed ordering to navigate.
2. Review the channel status, active source, and input/output bitrate. DVB channels may also show LOCK, level, and quality.
3. Select Edit to open channel settings, or Start / Stop to control only that tile.
4. After a change, confirm the tile is active and that the viewer output plays both picture and sound.

### Controls and fields

| Control / field | What it does |
|---|---|
| Start / Stop | Start or stop the selected tile. |
| Edit | Open source, output, and transcoding settings. |
| Auto Start | Start the tile automatically with the server; for online tiles, limited retry after unexpected stop may be enabled. |
| Input / output bitrate | Indicate incoming and outgoing data activity. |
| Active source | Identify primary, backup, or replacement-file input. |

> Stopping one tile is not the same as stopping DVBStreamer5. First verify the channel you selected.

## 3. Add or edit a channel

*Where to find it: Dashboard / Add channel / Edit tile*

The channel editor groups basic channel details, input, backup, outputs, and transcoding. Fields depend on the selected input type.

### How to use it

1. Open Add Channel or select Edit on an existing tile.
2. Enter a recognizable channel name and select the input type.
3. Fill in the address or reception parameters. For an IP input, enter a URL/address and port as required; for DVB, select a transponder and service.
4. Optionally configure a backup source, output protocols, and video/audio processing.
5. Select Save. If a setting requires a tile restart, apply it to that tile at an appropriate time.

### Controls and fields

| Control / field | What it does |
|---|---|
| Channel name | Name displayed on the dashboard. |
| Input type | DVB, UDP/RTP, HTTP/HTTPS, HLS, SRT, RTSP, RTMP, or local file, depending on availability. |
| URL / address / port | Where to connect for an IP feed. |
| Enable transcoding | Process video and/or audio, or use copy mode. |
| Outputs | Where viewers and receiving devices obtain the stream. |

> Saving a setting does not always mean every parameter is immediately applied to an active stream.

## 4. IPTV and network inputs

*Where to find it: Edit channel / Source / Input*

Input sources may include UDP/RTP, HTTP/HTTPS TS, HLS, SRT, RTSP, RTMP, and local files if the relevant mode is enabled in your build and configuration.

### How to use it

1. Choose the input type that matches your upstream provider.
2. Enter the exact URL or address and port. For multicast, ensure the relevant network is reachable.
3. For SRT, select a compatible connection mode and settings. Sender and receiver must agree on mode and port.
4. Save the channel, start the tile, and verify the intended source is active.
5. If you use backup input, keep its address separate from the primary source.

### Controls and fields

| Control / field | What it does |
|---|---|
| UDP / RTP | Receive MPEG-TS over the network; multicast may be used. |
| HTTP TS | Receive a continuous TS over HTTP/HTTPS. |
| HLS | Use a playlist and media segments as the input. |
| SRT | Caller/Listener connection with compatible latency and other settings. |
| RTSP / RTMP | Receive media via the corresponding protocol. |
| File | Read a locally accessible media file. |

> The source URL and the destination/output address are separate settings.

## 5. DVB-S/S2 satellite channels

*Where to find it: DVB / Transponders / tuner and service*

Satellite reception uses hardware tuner and transponder settings. Multiple services on one transponder may share a frontend.

### How to use it

1. Open DVB and choose an available tuner/frontend.
2. Select a satellite or transponder and enter frequency, polarization, symbol rate (SR), and other required parameters.
3. If applicable, set DiSEqC, Unicable/JESS with the correct user band (UB) and frequency, or MIS/Stream ID.
4. Select the required TV service and link it to a channel tile.
5. Save and review the LOCK, quality, and signal level indicators in the panel.

### Controls and fields

| Control / field | What it does |
|---|---|
| Frontend | Physical DVB receiving device. |
| Frequency / SR / polarization | Basic transponder settings. |
| DiSEqC | Switch satellite receiving equipment inputs. |
| Unicable / JESS | User band and UB frequency. |
| MIS / Stream ID | Select a stream within a multi-stream DVB-S2 transponder. |
| LOCK / level / quality | Reception status indicators. |

> Changing a shared frontend may affect other channels using the same tuner.

## 6. Primary and backup sources

*Where to find it: Edit channel / Primary / Backup source*

Backup input helps maintain service when the primary source is unavailable. A local replacement file may be offered for supported source/output combinations.

### How to use it

1. Set the primary input and verify that it works.
2. Add a backup URL or select a local replacement file when that option is available.
3. Set the failover waiting time to avoid switching on every short network fluctuation.
4. Configure return to primary (failback) after recovery if required.
5. Save. Check the active source on the tile and verify playback on a client device.

### Controls and fields

| Control / field | What it does |
|---|---|
| Primary source | First-choice feed. |
| Backup source | Alternative URL when the primary is unavailable. |
| Replacement file | Local media file in supported modes. |
| Failover threshold | How long the panel waits before using backup. |
| Return to primary | Controls how the stream switches back after recovery. |

> A very short waiting period can cause repeated switches. Verify actual picture and sound, not just the tile status.

## 7. Configure outputs

*Where to find it: Edit channel / Outputs*

A channel can be delivered through one or more supported output protocols. Choose a format compatible with the receiving equipment.

### How to use it

1. Open Outputs for the relevant channel.
2. Add or enable the required protocol: UDP/RTP, HTTP TS, SRT, HLS, or another available mode.
3. For address-based outputs, enter the destination IP and port; for SRT, select a matching connection mode and settings.
4. For HLS, make sure your player uses the correct playlist URL.
5. Enable CBR or adjust MPEG-TS settings if required by your receiver.
6. Save and check playback on at least one viewer device.

### Controls and fields

| Control / field | What it does |
|---|---|
| UDP / RTP | Deliver MPEG-TS over a local or multicast network. |
| HTTP TS | Provide a continuous TS over HTTP. |
| SRT | Deliver with agreed connection mode and latency. |
| HLS | Segmented output delivered using a playlist. |
| CBR | Maintain a configured transport bitrate on supported outputs. |
| SID / PID remapping | Set transport identifiers when receiver compatibility requires it. |

> Use the correct destination address and port. Multicast forwarding must be enabled in your network.

## 8. Video and audio transcoding

*Where to find it: Edit channel / Transcoding*

Video can be passed through (Copy) or transcoded to a different codec, size, frame rate, or bitrate. Audio can also be copied or transcoded.

### How to use it

1. Open the selected tile’s transcoding settings.
2. If the input is compatible and no video changes are needed, choose Video Copy. Otherwise select H.264 or another supported codec.
3. Set output resolution, FPS, video bitrate, and optionally profile/preset.
4. Select an encoder: Auto, CPU, Intel QSV/VAAPI, or NVIDIA NVENC, depending on the installed hardware and drivers.
5. For audio, choose Audio Copy or an available codec, and adjust channel layout/bitrate if needed.
6. Save, restart only the test tile if required, and check picture and sound.

### Controls and fields

| Control / field | What it does |
|---|---|
| Video Copy | Pass compressed video without re-encoding. |
| Audio Copy | Pass compressed audio without re-encoding. |
| CPU | Encode in software on the processor. |
| Intel QSV / VAAPI | Hardware encoding on supported Intel GPUs. |
| NVIDIA NVENC | Hardware encoding on supported NVIDIA GPUs. |
| Resolution / FPS / bitrate | Control the output picture and data rate. |

> Auto chooses an available backend. For a deliberate choice, explicitly select CPU, VAAPI/QSV, or NVENC. Availability is system-dependent.

## 9. EPG, XMLTV, and subtitles

*Where to find it: Menu / EPG / XMLTV; Edit channel / EPG / Subtitles*

Electronic program guides can come from DVB EIT or an external XMLTV feed. Availability and carriage of guide data depend on the output type.

### How to use it

1. Open EPG / XMLTV.
2. For an external guide, add the HTTP URL and download settings for the XMLTV feed.
3. Where channel mapping is available, match each XMLTV channel to the corresponding tile.
4. Use Update Now when you need to refresh the guide immediately.
5. For DVB services, optionally enable EIT carriage on supported outputs.
6. If the service has DVB subtitles and the output supports them, enable the relevant option.

### Controls and fields

| Control / field | What it does |
|---|---|
| DVB EIT | Program information from a DVB stream. |
| XMLTV HTTP | External program guide source. |
| Channel mapping | Associate guide entries with a channel tile. |
| Update Now | Fetch refreshed program information manually. |
| DVB subtitles | Subtitles on supported services and outputs. |

> Check the guide on the receiving device. Having XMLTV in the panel does not guarantee it is carried by every output.

## 10. OSCam-mini and conditional access

*Where to find it: Menu / OSCam-mini / CAM*

Conditional access settings are intended for licensed cards and authorized CA modules. The equipment owner must have rights to receive the protected services.

### How to use it

1. Open OSCam-mini or the relevant CAM section.
2. Review service state and the list of available readers.
3. An administrator may add a local Phoenix/PCSC reader or an authorized remote Newcamd reader.
4. Set groups, CAID, and Provider/Ident as required by your authorized card and subscription.
5. Save and select the appropriate CA source for the affected tile.
6. An experimental TSDecrypt mode may be available for an individual tile; use it only when approved.

### Controls and fields

| Control / field | What it does |
|---|---|
| Readers | Attached smartcard readers and authorized remote sources. |
| Newcamd users | Accounts for authorized clients. |
| Group | Associate readers with access groups. |
| CAID / Provider / Ident | Conditional access system and provider settings. |
| Keepalive | Maintain a connection when supported. |
| TSDecrypt | Optional experimental alternative module, where installed. |

> Never share access passwords, secret keys, or exported CA configurations. CA setup should be handled by an authorized administrator.

## 11. Monitoring, subscribers, and notifications

*Where to find it: Dashboard / Monitoring / Network / Subscribers*

Operators can review stream and subscriber activity in the web panel, without connecting to a server console.

### How to use it

1. Open monitoring for the selected channel or the general network section.
2. Review incoming/outgoing bitrate, source state, and active outputs.
3. Where a subscriber list is available, choose the channel and inspect current connections.
4. If notifications are enabled, work with the administrator to configure them in the corresponding section.
5. Return to the tile when you need to change settings for that specific channel.

### Controls and fields

| Control / field | What it does |
|---|---|
| Input / output | Incoming and outgoing traffic indicators. |
| Online subscribers | Current connections on supported outputs. |
| Network interfaces | Input/output paths and filters when permitted. |
| Notifications | Status notifications if configured. |
| Active source | Current primary, backup, or replacement input. |

> Indicators depend on the installed version and settings. Also use viewer playback when deciding whether a channel is operating correctly.

## 12. Panel settings and updates

*Where to find it: Main menu / Settings / Check for updates*

The web panel may offer language/theme preferences and version checks. Applying an update is an administrator-level operation.

### How to use it

1. Choose your preferred language in panel settings if the switch is available.
2. Select a light or dark theme if desired.
3. Open Check for updates to compare the current version with the available version.
4. If Update is offered, coordinate with an administrator because installation may interrupt broadcasting.
5. Return to the dashboard and confirm that your channel tiles and settings remain accessible.

### Controls and fields

| Control / field | What it does |
|---|---|
| Russian / English | Panel language where translations are available. |
| Light / dark theme | Web panel appearance. |
| Check for updates | View information about newer versions. |
| Update | Start installation with the required permissions and supported mechanism. |

> Checking a version does not install anything. Never start an update without an agreed maintenance window.

## 13. Daily operator checklist

*Where to find it: Dashboard / channels / Edit when needed*

This short routine helps operators use DVBStreamer5 consistently and safely.

### How to use it

1. Sign in and review the tiles for channels under your responsibility.
2. Confirm the expected source is active and important channels have input and output activity.
3. When a setting must change, open the correct channel editor and save only the intended adjustments.
4. After starting, stopping, or editing a channel, verify playback on a client device.
5. Coordinate shared tuner, conditional access, and update operations with the administrator.

### Controls and fields

| Control / field | What it does |
|---|---|
| Add channel | Create a tile with its input and output settings. |
| Edit channel | Adjust one channel without changing unrelated tiles. |
| Start / Stop | Control the selected channel’s state. |
| Backup input | Choose primary/backup sources and return policy. |
| Check for updates | View a new version without automatically installing it. |

> Main rule: change only the channel and parameter you opened the editor for. Hand over important changes to the next operator shift.

