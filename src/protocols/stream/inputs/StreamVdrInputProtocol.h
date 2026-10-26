#pragma once

#include <string>

namespace dvbstreamer5::stream_protocols::inputs {

// CI+ bridge input backed by a VDR streamdev-server instance.
//
// URI syntax:
//   vdr://host[:port]/<channel-number-or-vdr-channel-id>
//
// The default streamdev HTTP port is 3000. The URI is resolved to a TS
// endpoint (http://host:port/TS/<channel>) before entering the existing native
// HTTP MPEG-TS path. VDR remains responsible for tuner ownership and CI+ CAM
// handling; DVBStreamer5 only consumes the resulting transport stream.
bool isVdrInput(const std::string& input, const std::string& mode, bool testPattern);

bool resolveVdrStreamdevUri(
    const std::string& input,
    std::string& resolvedHttpUri,
    std::string& error);

} // namespace dvbstreamer5::stream_protocols::inputs
