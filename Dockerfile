FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config libpcsclite-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel --target DVBStreamer5

FROM ubuntu:24.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libssl3t64 libpcsclite1 pcscd libccid \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/DVBStreamer5 /app/DVBStreamer5
RUN mkdir -p /opt/DVBStreamer5/ca-plugins
WORKDIR /data
EXPOSE 9000/tcp
STOPSIGNAL SIGTERM
ENTRYPOINT ["/app/DVBStreamer5"]
