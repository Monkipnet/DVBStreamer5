# Local browser-preview libraries

The browser preview uses the vendored `mpegts.min.js` and `hls.min.js` files. CMake embeds these JavaScript libraries and their license texts into the DVBStreamer5 executable, which serves them at `/preview/` and `/licenses/`. The deployed executable does not need a `web/` directory or access to a CDN.

If a library is not already present, `bash scripts/vendor_preview_libs.sh` fetches the pinned version and license on an Internet-connected build machine. Reconfigure/rebuild after changing these source assets so the executable embeds the updated content.

Keep the vendor license files alongside the source assets; they are embedded and served by the application as well.
