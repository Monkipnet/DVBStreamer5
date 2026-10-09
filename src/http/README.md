# HttpServer implementation layout

`src/HttpServer.cpp` is now a small composition unit. The implementation is split by responsibility while intentionally remaining a single C++ translation unit. This preserves the existing anonymous-namespace helpers, static state, macro behavior, and linkage during the structural refactor.

- `HttpServerCommon.inc` — includes, shared helpers, URL/output helpers.
- `HttpServerTransport.inc` — listener/session handling, authentication, HTTP port management.
- `HttpServerState.inc` — interfaces, system metrics, runtime state serialization.
- `HttpServerDvbMedia.inc` — DVB/CA endpoints and HTTP/HLS media delivery.
- `HttpServerQuality.inc` — quality sampling, persistence, history API.
- `HttpServerConfigActions.inc` — config import/export/save, backup and stream/subscriber actions.
- `HttpServerWebUi.inc` — embedded Web UI returned by `renderIndexPage()`.

The include order is significant. A later cleanup can move individual groups to independent `.cpp` translation units after their shared helpers are given explicit interfaces.
