FROM ubuntu:24.04 AS build

ARG CODEC_JOBS=2
ARG BUILD_JOBS=2
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config python3 nodejs curl ca-certificates \
    binutils nasm autoconf automake libtool \
    libpcsclite-dev libssl-dev libcrypt-dev libmp3lame-dev \
    libcurl4-openssl-dev libxml2-dev zlib1g-dev \
    libva-dev libvpl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Recreate all locked third-party sources/prefixes so the Docker build does not
# depend on developer-machine artifacts.
RUN bash scripts/vendor_srt_source.sh \
    && bash scripts/vendor_native_codecs.sh \
    && DVBSTREAMER5_CODEC_JOBS="${CODEC_JOBS}" bash scripts/build_native_codecs.sh

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel "${BUILD_JOBS}"

FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive \
    DVBSTREAMER5_CONFIG_DIR=/etc/dvbstreamer5 \
    DVBSTREAMER5_CA_PLUGIN_DIR=/opt/DVBStreamer5/ca-plugins \
    DVBSTREAMER5_OSCAM_CONFIG_DIR=/etc/dvbstreamer5/oscam-mini \
    DVBSTREAMER5_OSCAM_BINARY=/opt/DVBStreamer5/oscam-mini/oscam-mini

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libssl3t64 libcrypt1 libcurl4t64 libxml2 zlib1g libmp3lame0 \
    libva2 libva-drm2 libvpl2 \
    libpcsclite1 pcscd libccid \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p \
    /opt/DVBStreamer5/bin \
    /opt/DVBStreamer5/ca-plugins \
    /opt/DVBStreamer5/oscam-mini/default-config \
    /etc/dvbstreamer5 \
    /var/lib/dvbstreamer5/archive

COPY --from=build /src/build/DVBStreamer5 /opt/DVBStreamer5/bin/DVBStreamer5
COPY --from=build /src/build/ca-plugins/dvbstreamer5-ca-newcamd.so /opt/DVBStreamer5/ca-plugins/
COPY --from=build /src/build/oscam-mini/oscam-mini /opt/DVBStreamer5/oscam-mini/oscam-mini
COPY --from=build /src/scripts/install_oscam_mini.sh /opt/DVBStreamer5/oscam-mini/install_oscam_mini.sh
COPY --from=build /src/packaging/oscam-mini/default-config/ /opt/DVBStreamer5/oscam-mini/default-config/

WORKDIR /opt/DVBStreamer5
VOLUME ["/etc/dvbstreamer5", "/var/lib/dvbstreamer5"]
EXPOSE 9000/tcp
STOPSIGNAL SIGTERM

ENTRYPOINT ["/opt/DVBStreamer5/bin/DVBStreamer5"]
