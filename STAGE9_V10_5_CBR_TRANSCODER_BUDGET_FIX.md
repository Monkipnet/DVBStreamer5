# Stage 9 V10.5 — CBR transcoder bitrate-budget fix

V10.4 fixed the shared PacketFramer corruption and live H.264 decoding became stable
(bitErr=0, refLost=0, decoded frames continuously increasing).

The remaining freeze/audio-stutter issue came from an impossible bitrate budget:
the default native H.264 encoder target is 6000 kbit/s while the complete CBR TS
target can also be 6000 kbit/s. Audio, TS/PES overhead, PSI/SI and encoder overshoot
then push useful TS payload to ~6.0–6.1 Mbit/s, leaving no room for a 6 Mbit/s pacer.
This causes backpressure/bursts and visible playback stalls.

V10.5 leaves the configured/UI video bitrate unchanged, but for a CBR transcoded
output computes an effective encoder target that fits inside the transport target.
It reserves max(500 kbit/s, audio bitrate + 5% of TS target) and caps only the runtime
video encoder target. A diagnostic line reports target/requested/effective/reserve.

No decoder, PacketFramer, audio codec, mux packetization, SRT protocol, FFmpeg,
GStreamer or libav dependency is added or changed.
