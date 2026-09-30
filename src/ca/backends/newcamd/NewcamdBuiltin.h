#pragma once

#include "ca/CaBackendPluginApi.h"

// The Newcamd backend is linked into DVBStreamer5.  Keeping the versioned C
// function table lets the built-in backend and optional third-party backends
// share the same host-side implementation without dlopen for the default path.
extern "C" const dvbstreamer5_ca_backend_api_v1*
dvbstreamer5_ca_backend_get_api_v1(void);
