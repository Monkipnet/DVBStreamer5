#pragma once

#include "ca/CaBackendPluginApi.h"

// Exported entry point of the loadable Newcamd CA backend. The host resolves
// this symbol through dlsym using the versioned CA backend ABI.
extern "C" const dvbstreamer5_ca_backend_api_v1*
dvbstreamer5_ca_backend_get_api_v1(void);
