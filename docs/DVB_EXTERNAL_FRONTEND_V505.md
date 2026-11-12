# DVBStreamer5 V10.8.505 — external DVB frontend owners

The DVB adapter list previously treated configured/idle OnDemand assignments
as "in use", but did not see external applications (TVHeadend, Astra, dvblast).
Signal/scan and native DVB input could retune a frontend held externally.

V505 checks /proc/PID/fd symlinks and fdinfo read-only. Only O_RDWR/O_WRONLY
frontends count as external owners; O_RDONLY signal observers are listed but
are not marked busy. The application's own PID is excluded. No command line
or credentials are exposed, only PID and short process comm.

API adds external_in_use, external_processes, and usage_state
(free, assigned, active, external, conflict). The UI distinguishes these
states and reports the owning external process. Scan, signal-tune and native
DVB open reject a detected external writer. Never terminate other programs,
change driver mode or alter existing streams.

Best-effort snapshot: procfs restrictions, aliases, and races can conceal an
owner. OS EBUSY still applies. No change to CA, preview, outputs or Unicable.
