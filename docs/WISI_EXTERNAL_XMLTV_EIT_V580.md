# WISI Chameleon standalone XMLTV EIT services (version 10.8.580 unchanged)

Open **Broadcast / WISI EPG UDP**. Add an output, set destination multicast/unicast IPv4, UDP port, output network interface and select **Actual** or **Barker/Other** EIT. In the output, choose **External EPG channel**, select the XMLTV provider and channel ID, and enter the effective **SID, TSID, ONID from the receiving WISI multiplex**. Save and enable the output. Multiple external XMLTV services and existing DVBStreamer5 tile EPG services may share one output (up to 64 services per output, 8 outputs).

External services do **not** require a streaming tile, a decoder or a transcoder. The emitted transport stream contains PID **0x0012 (18) EIT** plus optional MPEG-TS null padding; no PAT/PMT, audio, video, SDT or time tables. The receiving WISI firmware must support external EIT/Barker input; otherwise the external EIT PID must be remultiplexed separately with the correct table type and service IDs.

The XMLTV manager's memory-conscious retention set now includes channels referenced by **enabled** external WISI output services. Saving a new external service queues a source refresh if the selected channel was not retained previously; EPG availability may lag the refresh. The full XMLTV channel catalog remains available for selection. Disabled XMLTV sources cannot be bound by the save endpoint. Existing tile-based EIT output stays compatible.

Translations for added controls: RU, EN, HR, DE. Media pipelines and OSCam are untouched. A CI regression checks the wiring, source retention, UI syntax and version. **Hardware validation with the target WISI Chameleon is still required.**
