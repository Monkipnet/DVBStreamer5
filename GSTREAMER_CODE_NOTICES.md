# Adapted GStreamer code

No GStreamer library is linked by the native media-core target.

`src/media/RtpMpegTs.cpp` follows the incomplete-trailing-packet handling in
GStreamer's `gst-plugins-good/gst/rtp/gstrtpmp2tdepay.c`: an RTP/MP2T payload
is reduced to its integral 188-byte MPEG-TS packet prefix before depayloading.
The upstream implementation is copyright Wim Taymans, is licensed under the
GNU Lesser General Public License version 2.1 or (at your option) any later
version, and is available at:

<https://github.com/GStreamer/gstreamer/blob/main/subprojects/gst-plugins-good/gst/rtp/gstrtpmp2tdepay.c>

The adapted routine is distributed under the upstream license terms. The rest
of the native media-core implementation remains under this repository's
license.
