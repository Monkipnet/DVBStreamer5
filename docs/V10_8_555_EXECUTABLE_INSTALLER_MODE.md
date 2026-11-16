# DVBStreamer5 V10.8.555 — restore direct executable permission on legacy Linux installer

The atomic V552 tree update accidentally wrote `install_dvbstreamer5_binary.sh`
with Git mode `100644` rather than the original `100755`. This commit
restores the executable bit without changing the installer body, keeps the
V552 recovery and systemd stop guarantees, and adds an executable-bit
regression assertion to the V552 Node test. Portable `install.sh` keeps its
own runtime `chmod` behavior.
