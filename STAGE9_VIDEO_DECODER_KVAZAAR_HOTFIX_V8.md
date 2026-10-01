# Stage 9 V8 — H.264 live decode + Kvazaar init

## H.264
OpenH264 decoder now uses live error concealment instead of ERROR_CON_DISABLE.
The decoder requests the highest dependency layer and I420 output. This avoids
suppressing incomplete live MPEG-TS/PES pictures indefinitely.

## H.265 / Kvazaar
Kvazaar initialization is reduced to the supported minimal embed configuration:
config_init defaults + width/height + framerate + target bitrate + P420/8-bit.
The previous direct overrides of GOP/OWF/ref/thread fields could leave dependent
configuration values inconsistent and encoder_open() could reject the config.

Kvazaar 2.3.x requires coded width and height divisible by 8. V8 reports the
actual requested geometry if this requirement is violated instead of the opaque
"encoder_open failed" error.

No FFmpeg/GStreamer/libav dependency is introduced.
