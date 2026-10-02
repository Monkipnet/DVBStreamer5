# Installable Linux bundle

DVBStreamer5 can be packaged as a self-contained x86_64 Linux bundle whose
user-space shared libraries stay beside the application.

## Build and package

From the project root:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel 2

bash scripts/make_portable_linux_bundle.sh \
  --build-dir build-release \
  --with-oscam \
  --archive
```

Output:

```text
dist/DVBStreamer5-linux-x86_64/
dist/DVBStreamer5-linux-x86_64.tar.gz
```

## Install on another Linux host

Copy/extract the archive, then run:

```bash
sudo ./install.sh
```

The installer requires Linux x86_64 and systemd, but does not use apt/dnf/pacman
for the bundled runtime libraries.

Installed layout:

```text
/opt/DVBStreamer5/
  bin/DVBStreamer5
  lib/
  lib/ossl-modules/
  web/
  ca-plugins/
  oscam-mini/
  run.sh

/etc/dvbstreamer5/
  dvbstreamer5-config.json
  dvbstreamer5-subscribers.json
  dvbstreamer5-ui.key
  oscam-mini/
    oscam.conf
    oscam.server
    oscam.user

/var/lib/dvbstreamer5/
  archive/
```

Systemd units:

```text
/etc/systemd/system/dvbstreamer5.service
/etc/systemd/system/oscam-mini.service
```

Both services are enabled for boot when OSCam-mini is present in the bundle.

The main service exports:

```text
DVBSTREAMER5_CONFIG_DIR=/etc/dvbstreamer5
DVBSTREAMER5_OSCAM_CONFIG_DIR=/etc/dvbstreamer5/oscam-mini
DVBSTREAMER5_OSCAM_BINARY=/opt/DVBStreamer5/oscam-mini/oscam-mini
DVBSTREAMER5_CA_PLUGIN_DIR=/opt/DVBStreamer5/ca-plugins
```

Existing legacy configuration stored directly under `/opt/DVBStreamer5` is
migrated into `/etc/dvbstreamer5` only when the target configuration file does
not already exist. The UI-password encryption key is migrated together with the
main JSON configuration.

## Libraries

The packager recursively resolves ELF dependencies for DVBStreamer5, optional
external CA plugins and OSCam-mini. It copies the dynamic loader and shared
libraries into `lib/`. Installed services execute through the bundled loader
with `--library-path /opt/DVBStreamer5/lib`.

Driver libraries that belong to the host GPU/kernel stack are not a portable
replacement for the corresponding kernel driver. The destination host still
needs compatible DVB, VAAPI/QSV/NVIDIA, USB and firmware support. PC/SC reader
use additionally needs a working `pcscd` service on the target host.

For the widest compatibility, build the bundle on the oldest Linux distribution
you intend to support.

## Uninstall

Keep configuration:

```bash
sudo ./uninstall.sh
```

Remove runtime and configuration:

```bash
sudo ./uninstall.sh --purge-config
```
