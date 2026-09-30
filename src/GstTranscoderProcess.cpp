#include "GstTranscoderProcess.h"

bool GstTranscoderProcess::isAvailable(std::string* error) {
    if (error) {
        *error = "external gst-launch transcoder is disabled; using in-process transcoder";
    }
    return false;
}

bool GstTranscoderProcess::start(const StreamConfig& config, std::string& error) {
    (void)config;
    error = "external gst-launch transcoder is disabled; use the in-process transcoder";
    return false;
}

void GstTranscoderProcess::stop() {
    // No child process exists in Stage 1.
}

bool GstTranscoderProcess::isRunning() {
    return false;
}

std::string GstTranscoderProcess::description() const {
    return "external gst-launch transcoder disabled";
}

std::vector<pid_t> GstTranscoderProcess::childPids() const {
    return {};
}
