#include "TranscoderModule.h"
#include "TranscodeVideoGeometry.h"
#include "media/GstMp2Encoder.h"
#include "media/NativeMpegTsMux.h"
#include "utils.h"

#include <algorithm>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <iostream>
#include <filesystem>
#include <cstdlib>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <memory>
#include <mutex>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>

namespace {





bool factoryAvailable(const char* name) {
    GstElementFactory* factory = gst_element_factory_find(name);
    if (!factory) return false;
    gst_object_unref(factory);
    return true;
}

struct EncoderProbeResult {
    bool ok = false;
    bool timedOut = false;
    int exitCode = -1;
    int signal = 0;
};

EncoderProbeResult probeVideoEncoderFactory(const std::string& factory) {
    EncoderProbeResult result;
    if (!factoryAvailable(factory.c_str())) return result;

    const pid_t pid = ::fork();
    if (pid < 0) {
        std::cerr << "Intel encoder probe 203.08: factory=" << factory
                  << " result=fork-failed errno=" << errno << std::endl;
        return result;
    }

    if (pid == 0) {
        // A broken qsvh264enc on older Intel generations can abort in libmfx.
        // Probe it in an isolated child and suppress core dumps so the main
        // DVBStreamer5 process is never affected by the driver assertion.
        struct rlimit coreLimit {};
        coreLimit.rlim_cur = 0;
        coreLimit.rlim_max = 0;
        ::setrlimit(RLIMIT_CORE, &coreLimit);

        const int nullFd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (nullFd >= 0) {
            ::dup2(nullFd, STDOUT_FILENO);
            ::dup2(nullFd, STDERR_FILENO);
            if (nullFd > STDERR_FILENO) ::close(nullFd);
        }

        // Keep the potentially unsafe Intel driver probe isolated from the main
        // process, but execute our own binary instead of the external
        // gst-launch-1.0 utility.
        ::execl("/proc/self/exe", "DVBStreamer5",
                "--transcoder-encoder-probe", factory.c_str(),
                static_cast<char*>(nullptr));
        ::_exit(127);
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        int status = 0;
        const pid_t done = ::waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            if (WIFEXITED(status)) {
                result.exitCode = WEXITSTATUS(status);
                result.ok = result.exitCode == 0;
            } else if (WIFSIGNALED(status)) {
                result.signal = WTERMSIG(status);
            }
            return result;
        }
        if (done < 0 && errno != EINTR) return result;
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timedOut = true;
            ::kill(pid, SIGKILL);
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

std::string intelVideoEncoderFactory() {
    return TranscoderModule::workingIntelVideoEncoderFactory();
}

std::string intelHevcVideoEncoderFactory() {
    return TranscoderModule::workingIntelHevcEncoderFactory();
}

bool isNvidiaVideoEncoder(const std::string& factory) {
    return factory == "nvh264enc" || factory == "nvh265enc";
}

bool isIntelVideoEncoder(const std::string& factory) {
    return factory == "qsvh264enc" || factory == "vah264enc" || factory == "vaapih264enc" ||
           factory == "qsvh265enc" || factory == "vah265enc" || factory == "vaapih265enc";
}

std::string selectedVideoEncoderFactory(const StreamConfig& config) {
    const bool hevc = toLower(config.transcodeVideoCodec) == "hevc";
    const std::string requested = toLower(config.transcodeVideoEncoder);
    if (requested == "nvenc") {
        const char* name = hevc ? "nvh265enc" : "nvh264enc";
        return factoryAvailable(name) ? name : std::string();
    }
    if (requested == "intel") {
        return hevc ? intelHevcVideoEncoderFactory() : intelVideoEncoderFactory();
    }
    if (requested == "x264") {
        const char* name = hevc ? "x265enc" : "x264enc";
        return factoryAvailable(name) ? name : std::string();
    }
    if (hevc) {
        if (factoryAvailable("nvh265enc")) return "nvh265enc";
        if (const std::string intel = intelHevcVideoEncoderFactory(); !intel.empty()) return intel;
        if (factoryAvailable("x265enc")) return "x265enc";
    } else {
        if (factoryAvailable("nvh264enc")) return "nvh264enc";
        if (const std::string intel = intelVideoEncoderFactory(); !intel.empty()) return intel;
        if (factoryAvailable("x264enc")) return "x264enc";
    }
    return {};
}

void setUIntIfPresent(GstElement* element, const char* property, guint value) {
    if (element && g_object_class_find_property(G_OBJECT_GET_CLASS(element), property)) {
        g_object_set(element, property, value, nullptr);
    }
}

void setBoolIfPresent(GstElement* element, const char* property, gboolean value) {
    if (element && g_object_class_find_property(G_OBJECT_GET_CLASS(element), property)) {
        g_object_set(element, property, value, nullptr);
    }
}

void setEnumIfPresent(GstElement* element, const char* property, const char* value) {
    if (element && g_object_class_find_property(G_OBJECT_GET_CLASS(element), property)) {
        gst_util_set_object_arg(G_OBJECT(element), property, value);
    }
}

void configureVideoEncoder(GstElement* encoder, const std::string& factory, guint bitrateKbps) {
    if (!encoder) return;
    if (isNvidiaVideoEncoder(factory)) {
        setUIntIfPresent(encoder, "bitrate", bitrateKbps);
        setUIntIfPresent(encoder, "gop-size", 50);
        setUIntIfPresent(encoder, "bframes", 0);
        setBoolIfPresent(encoder, "aud", TRUE);
        setBoolIfPresent(encoder, "zerolatency", TRUE);
        setBoolIfPresent(encoder, "repeat-sequence-header", TRUE);
        setBoolIfPresent(encoder, "strict-gop", TRUE);
        setUIntIfPresent(encoder, "vbv-buffer-size", std::max<guint>(bitrateKbps, 500u));
        setEnumIfPresent(encoder, "rc-mode", "cbr");
        return;
    }
    if (isIntelVideoEncoder(factory)) {
        setUIntIfPresent(encoder, "bitrate", bitrateKbps);
        setUIntIfPresent(encoder, "b-frames", 0);
        setBoolIfPresent(encoder, "aud", TRUE);
        if (factory == "qsvh264enc" || factory == "qsvh265enc") {
            setUIntIfPresent(encoder, "gop-size", 50);
            setUIntIfPresent(encoder, "idr-interval", 0);
            setEnumIfPresent(encoder, "rate-control", "cbr");
        } else if (factory == "vah264enc" || factory == "vah265enc") {
            setUIntIfPresent(encoder, "key-int-max", 50);
            setEnumIfPresent(encoder, "rate-control", "cbr");
            setUIntIfPresent(encoder, "target-usage", 4);
        } else {
            // Legacy vaapih264enc names differ across GStreamer releases; only
            // set properties when the installed element actually exposes them.
            setUIntIfPresent(encoder, "keyframe-period", 50);
            setUIntIfPresent(encoder, "key-int-max", 50);
            setEnumIfPresent(encoder, "rate-control", "cbr");
        }
        return;
    }

    if (factory == "x265enc") {
        setUIntIfPresent(encoder, "bitrate", bitrateKbps);
        setUIntIfPresent(encoder, "key-int-max", 50);
        setEnumIfPresent(encoder, "speed-preset", "superfast");
        setEnumIfPresent(encoder, "tune", "zerolatency");
        return;
    }

    g_object_set(encoder,
        "bitrate", bitrateKbps,
        "key-int-max", 50,
        "bframes", 2,
        "byte-stream", TRUE,
        "aud", TRUE,
        "vbv-buf-capacity", 1000u,
        nullptr);
    gst_util_set_object_arg(G_OBJECT(encoder), "speed-preset", "veryfast");
    gst_util_set_object_arg(G_OBJECT(encoder), "tune", "zerolatency");
    g_object_set(encoder, "option-string",
        "nal-hrd=cbr:force-cfr=1:repeat-headers=1:scenecut=0", nullptr);
}

struct TimestampNormalizer {
    std::mutex mutex;
    GstClockTime lastPts = GST_CLOCK_TIME_NONE;
    GstClockTime lastDts = GST_CLOCK_TIME_NONE;
    GstClockTime fallbackDuration = 20 * GST_MSECOND;
};

GstPadProbeReturn normalizeEncodedTimestamps(GstPad*, GstPadProbeInfo* info, gpointer userData) {
    if (!(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)) return GST_PAD_PROBE_OK;
    auto* state = static_cast<TimestampNormalizer*>(userData);
    GstBuffer* input = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!state || !input) return GST_PAD_PROBE_OK;

    GstBuffer* buffer = gst_buffer_make_writable(input);
    if (!buffer) return GST_PAD_PROBE_OK;
    GST_PAD_PROBE_INFO_DATA(info) = buffer;

    std::lock_guard<std::mutex> lock(state->mutex);
    GstClockTime duration = GST_BUFFER_DURATION(buffer);
    if (!GST_CLOCK_TIME_IS_VALID(duration) || duration == 0) duration = state->fallbackDuration;

    GstClockTime pts = GST_BUFFER_PTS(buffer);
    GstClockTime dts = GST_BUFFER_DTS(buffer);

    if (!GST_CLOCK_TIME_IS_VALID(pts)) {
        pts = GST_CLOCK_TIME_IS_VALID(state->lastPts) ? state->lastPts + duration : 0;
    } else if (GST_CLOCK_TIME_IS_VALID(state->lastPts) && pts <= state->lastPts) {
        pts = state->lastPts + duration;
    }

    if (!GST_CLOCK_TIME_IS_VALID(dts)) {
        dts = GST_CLOCK_TIME_IS_VALID(state->lastDts) ? state->lastDts + duration : pts;
    } else if (GST_CLOCK_TIME_IS_VALID(state->lastDts) && dts <= state->lastDts) {
        dts = state->lastDts + duration;
    }

    if (dts > pts) pts = dts;
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DTS(buffer) = dts;
    GST_BUFFER_DURATION(buffer) = duration;
    state->lastPts = pts;
    state->lastDts = dts;
    return GST_PAD_PROBE_OK;
}

void attachTimestampNormalizer(GstElement* element, GstClockTime fallbackDuration) {
    if (!element) return;
    GstPad* srcPad = gst_element_get_static_pad(element, "src");
    if (!srcPad) return;
    auto* state = new TimestampNormalizer();
    state->fallbackDuration = fallbackDuration;
    gst_pad_add_probe(srcPad, GST_PAD_PROBE_TYPE_BUFFER, normalizeEncodedTimestamps, state,
        [](gpointer data) { delete static_cast<TimestampNormalizer*>(data); });
    gst_object_unref(srcPad);
}

struct TranscodeContext {
    GstElement* bin = nullptr;
    GstElement* outputAppSrc = nullptr;
    dvbstreamer5::media::mpegts::NativeMpegTsMux mux;
    std::mutex muxMutex;
    GstClockTime lastOutputPts = GST_CLOCK_TIME_NONE;
    StreamConfig config;
    bool videoLinked = false;
    bool audioLinked = false;
};

bool add(GstElement* bin, GstElement* element) {
    return bin && element && gst_bin_add(GST_BIN(bin), element);
}

void sync(GstElement* element) {
    if (element) gst_element_sync_state_with_parent(element);
}


bool gstValueCanContainInt(const GValue* value, gint expected) {
    if (!value) return false;
    if (G_VALUE_HOLDS_INT(value)) {
        return g_value_get_int(value) == expected;
    }
    if (GST_VALUE_HOLDS_INT_RANGE(value)) {
        return expected >= gst_value_get_int_range_min(value) &&
               expected <= gst_value_get_int_range_max(value);
    }
    if (GST_VALUE_HOLDS_LIST(value) || GST_VALUE_HOLDS_ARRAY(value)) {
        const guint count = GST_VALUE_HOLDS_LIST(value)
            ? gst_value_list_get_size(value)
            : gst_value_array_get_size(value);
        for (guint i = 0; i < count; ++i) {
            const GValue* item = GST_VALUE_HOLDS_LIST(value)
                ? gst_value_list_get_value(value, i)
                : gst_value_array_get_value(value, i);
            if (gstValueCanContainInt(item, expected)) return true;
        }
    }
    return false;
}

bool structureFieldCanContainInt(const GstStructure* structure, const char* field, gint expected) {
    return structure && gstValueCanContainInt(gst_structure_get_value(structure, field), expected);
}

bool structureFieldStringCanContain(const GstStructure* structure, const char* field, const char* expected) {
    if (!structure || !field || !expected) return false;
    const GValue* value = gst_structure_get_value(structure, field);
    if (!value) return false;
    if (G_VALUE_HOLDS_STRING(value)) {
        return g_strcmp0(g_value_get_string(value), expected) == 0;
    }
    if (GST_VALUE_HOLDS_LIST(value) || GST_VALUE_HOLDS_ARRAY(value)) {
        const guint count = GST_VALUE_HOLDS_LIST(value)
            ? gst_value_list_get_size(value)
            : gst_value_array_get_size(value);
        for (guint i = 0; i < count; ++i) {
            const GValue* item = GST_VALUE_HOLDS_LIST(value)
                ? gst_value_list_get_value(value, i)
                : gst_value_array_get_value(value, i);
            if (G_VALUE_HOLDS_STRING(item) && g_strcmp0(g_value_get_string(item), expected) == 0) {
                return true;
            }
        }
    }
    return false;
}

struct AudioEncoderSelection {
    GstElement* element = nullptr;
    std::string factory;
};

AudioEncoderSelection makeAudioEncoder(const std::string& codec) {
    const char* const* factories = nullptr;
    static const char* aacFactories[] = {"fdkaacenc", "voaacenc", "avenc_aac", nullptr};
    static const char* mp3Factories[] = {"lamemp3enc", "avenc_mp3", nullptr};
    static const char* mp2Factories[] = {"dvbstreamer5mp2enc", nullptr};
    factories = codec == "mp3" ? mp3Factories :
        (codec == "mp2" ? mp2Factories : aacFactories);
    for (const char* const* name = factories; *name; ++name) {
        GstElementFactory* factory = gst_element_factory_find(*name);
        if (factory) {
            gst_object_unref(factory);
            return {gst_element_factory_make(*name, nullptr), *name};
        }
    }
    return {};
}

void configureAudioBitrate(GstElement* encoder, const std::string& factory, uint64_t bitrate) {
    if (!encoder) return;
    bitrate = std::clamp<uint64_t>(bitrate, 64000, 320000);

    if (factory == "lamemp3enc") {
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(encoder), "target")) {
            gst_util_set_object_arg(G_OBJECT(encoder), "target", "bitrate");
        }
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(encoder), "cbr")) {
            g_object_set(encoder, "cbr", TRUE, nullptr);
        }
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(encoder), "bitrate")) {
            g_object_set(encoder, "bitrate", static_cast<gint>(bitrate / 1000), nullptr);
        }
        return;
    }

    if (factory == "dvbstreamer5mp2enc") {
        g_object_set(encoder, "bitrate", static_cast<guint>(bitrate), nullptr);
        return;
    }

    if (g_object_class_find_property(G_OBJECT_GET_CLASS(encoder), "bitrate")) {
        g_object_set(encoder, "bitrate", static_cast<gint>(bitrate), nullptr);
    }
}

struct NativeMuxSinkBinding {
    TranscodeContext* context = nullptr;
    dvbstreamer5::media::mpegts::ElementaryKind kind =
        dvbstreamer5::media::mpegts::ElementaryKind::Video;
};

GstFlowReturn onNativeMuxSample(GstAppSink* sink, gpointer userData) {
    auto* binding = static_cast<NativeMuxSinkBinding*>(userData);
    if (!binding || !binding->context || !binding->context->outputAppSrc) return GST_FLOW_ERROR;
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_EOS;
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (!buffer) {
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    GstMapInfo map {};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    dvbstreamer5::media::mpegts::ElementarySample nativeSample;
    nativeSample.data = map.data;
    nativeSample.size = map.size;
    const GstClockTime pts = GST_BUFFER_PTS(buffer);
    const GstClockTime dts = GST_BUFFER_DTS(buffer);
    const GstClockTime duration = GST_BUFFER_DURATION(buffer);
    nativeSample.hasPts = GST_CLOCK_TIME_IS_VALID(pts);
    nativeSample.hasDts = GST_CLOCK_TIME_IS_VALID(dts);
    if (nativeSample.hasPts) nativeSample.pts90k = gst_util_uint64_scale(pts, 90000, GST_SECOND);
    if (nativeSample.hasDts) nativeSample.dts90k = gst_util_uint64_scale(dts, 90000, GST_SECOND);
    if (GST_CLOCK_TIME_IS_VALID(duration) && duration > 0) {
        nativeSample.duration90k = gst_util_uint64_scale(duration, 90000, GST_SECOND);
    }
    nativeSample.randomAccess = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);

    std::vector<dvbstreamer5::media::mpegts::Packet> packets;
    std::string error;
    GstClockTime outputPts = GST_CLOCK_TIME_NONE;
    {
        std::lock_guard<std::mutex> lock(binding->context->muxMutex);
        if (!binding->context->mux.write(binding->kind, nativeSample, packets, error)) {
            std::cerr << "Native transcoder MPEG-TS mux failed: " << error << std::endl;
            gst_buffer_unmap(buffer, &map);
            gst_sample_unref(sample);
            return GST_FLOW_ERROR;
        }
        outputPts = GST_CLOCK_TIME_IS_VALID(pts) ? pts : dts;
        if (!GST_CLOCK_TIME_IS_VALID(outputPts)) {
            outputPts = GST_CLOCK_TIME_IS_VALID(binding->context->lastOutputPts)
                ? binding->context->lastOutputPts +
                    (GST_CLOCK_TIME_IS_VALID(duration) && duration > 0 ? duration : GST_MSECOND)
                : 0;
        } else if (GST_CLOCK_TIME_IS_VALID(binding->context->lastOutputPts) &&
                   outputPts <= binding->context->lastOutputPts) {
            outputPts = binding->context->lastOutputPts + 1;
        }
        binding->context->lastOutputPts = outputPts;
    }

    GstFlowReturn result = GST_FLOW_OK;
    if (!packets.empty()) {
        const gsize bytes = packets.size() * dvbstreamer5::media::mpegts::kPacketSize;
        GstBuffer* output = gst_buffer_new_allocate(nullptr, bytes, nullptr);
        if (!output) {
            result = GST_FLOW_ERROR;
        } else {
            gst_buffer_fill(output, 0, packets.data(), bytes);
            GST_BUFFER_PTS(output) = outputPts;
            GST_BUFFER_DTS(output) = outputPts;
            GST_BUFFER_DURATION(output) = duration;
            result = gst_app_src_push_buffer(
                GST_APP_SRC(binding->context->outputAppSrc), output);
            if (result == GST_FLOW_FLUSHING) result = GST_FLOW_OK;
        }
    }

    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);
    return result;
}

bool linkElementToNativeMux(
    GstElement* source,
    TranscodeContext* context,
    dvbstreamer5::media::mpegts::ElementaryKind kind,
    dvbstreamer5::media::mpegts::ElementaryCodec codec,
    const char* capsText) {
    if (!source || !context || !context->bin || !context->outputAppSrc) return false;
    std::string error;
    {
        std::lock_guard<std::mutex> lock(context->muxMutex);
        if (!context->mux.setCodec(kind, codec, error)) {
            std::cerr << "Native transcoder MPEG-TS codec setup failed: " << error << std::endl;
            return false;
        }
    }

    GstElement* sink = gst_element_factory_make("appsink", nullptr);
    if (!sink || !add(context->bin, sink)) {
        if (sink && !GST_OBJECT_PARENT(sink)) gst_object_unref(sink);
        return false;
    }
    g_object_set(sink,
        "emit-signals", FALSE,
        "sync", FALSE,
        "async", FALSE,
        "max-buffers", 32u,
        "drop", FALSE,
        nullptr);
    if (capsText && *capsText) {
        GstCaps* caps = gst_caps_from_string(capsText);
        if (!caps) return false;
        gst_app_sink_set_caps(GST_APP_SINK(sink), caps);
        gst_caps_unref(caps);
    }
    auto* binding = new NativeMuxSinkBinding{context, kind};
    GstAppSinkCallbacks callbacks {};
    callbacks.new_sample = onNativeMuxSample;
    gst_app_sink_set_callbacks(
        GST_APP_SINK(sink), &callbacks, binding,
        [](gpointer p) { delete static_cast<NativeMuxSinkBinding*>(p); });
    if (!gst_element_link(source, sink)) return false;
    sync(sink);
    return true;
}

void drainPad(GstElement* bin, GstPad* pad) {
    if (!bin || !pad || gst_pad_is_linked(pad)) return;
    GstElement* queue = gst_element_factory_make("queue", nullptr);
    GstElement* sink = gst_element_factory_make("fakesink", nullptr);
    if (!queue || !sink || !add(bin, queue) || !add(bin, sink) || !gst_element_link(queue, sink)) {
        if (queue && !GST_OBJECT_PARENT(queue)) gst_object_unref(queue);
        if (sink && !GST_OBJECT_PARENT(sink)) gst_object_unref(sink);
        return;
    }
    g_object_set(sink, "sync", FALSE, "async", FALSE, nullptr);
    GstPad* queueSink = gst_element_get_static_pad(queue, "sink");
    if (queueSink) {
        gst_pad_link(pad, queueSink);
        gst_object_unref(queueSink);
    }
    sync(queue);
    sync(sink);
}

void onDecodedPadAdded(GstElement*, GstPad* pad, gpointer userData) {
    auto* context = static_cast<TranscodeContext*>(userData);
    if (!context || !context->bin || !context->outputAppSrc) return;

    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    if (!caps || gst_caps_is_empty(caps)) {
        if (caps) gst_caps_unref(caps);
        drainPad(context->bin, pad);
        return;
    }
    const GstStructure* structure = gst_caps_get_structure(caps, 0);
    const std::string media = gst_structure_get_name(structure);

    if (media.rfind("video/x-raw", 0) == 0 && !context->videoLinked) {
        dvbstreamer5::transcode::VideoGeometry geometry;
        if (!dvbstreamer5::transcode::videoGeometry(context->config.transcodeResolution, geometry)) {
            std::cerr << "Transcoder: unsupported output video geometry" << std::endl;
            gst_caps_unref(caps);
            drainPad(context->bin, pad);
            return;
        }
        const int width = geometry.width;
        const int height = geometry.height;
        const guint bitrateKbps = static_cast<guint>(
            std::max<uint64_t>(500000, context->config.transcodeVideoBitrate) / 1000);

        const std::string videoEncoderFactory = selectedVideoEncoderFactory(context->config);
        const bool hevc = toLower(context->config.transcodeVideoCodec) == "hevc";
        const bool hardwareVideo = isNvidiaVideoEncoder(videoEncoderFactory) ||
            isIntelVideoEncoder(videoEncoderFactory);
        GstElement* queue = gst_element_factory_make("queue", nullptr);
        GstElement* convert = gst_element_factory_make("videoconvert", nullptr);
        GstElement* deinterlace = gst_element_factory_make("deinterlace", nullptr);
        GstElement* scale = gst_element_factory_make("videoscale", nullptr);
        GstElement* postScaleConvert = hardwareVideo
            ? gst_element_factory_make("videoconvert", nullptr)
            : nullptr;
        GstElement* filter = gst_element_factory_make("capsfilter", nullptr);
        GstElement* encoder = videoEncoderFactory.empty()
            ? nullptr
            : gst_element_factory_make(videoEncoderFactory.c_str(), nullptr);
        GstElement* parser = gst_element_factory_make(hevc ? "h265parse" : "h264parse", nullptr);
        GstElement* outQueue = gst_element_factory_make("queue", nullptr);
        if (!queue || !convert || !deinterlace || !scale ||
            (hardwareVideo && !postScaleConvert) ||
            !filter || !encoder || !parser || !outQueue) {
            std::cerr << "Transcoder: missing video elements" << std::endl;
            gst_caps_unref(caps);
            drainPad(context->bin, pad);
            return;
        }

        const char* rawFormat = hardwareVideo ? "NV12" : "I420";
        GstCaps* rawCaps = gst_caps_new_simple("video/x-raw",
            "format", G_TYPE_STRING, rawFormat,
            "width", G_TYPE_INT, width,
            "height", G_TYPE_INT, height,
            "pixel-aspect-ratio", GST_TYPE_FRACTION,
            geometry.pixelAspectNum, geometry.pixelAspectDen,
            "interlace-mode", G_TYPE_STRING, "progressive",
            nullptr);
        g_object_set(filter, "caps", rawCaps, nullptr);
        gst_caps_unref(rawCaps);
        // 202.73: preserve temporal resolution. 576i25/1080i25 carries 50 fields/s;
        // YADIF all-fields converts that to 50 progressive frames/s. Progressive
        // sources pass through auto-strict without an artificial videorate stage.
        gst_util_set_object_arg(G_OBJECT(deinterlace), "method", "yadif");
        gst_util_set_object_arg(G_OBJECT(deinterlace), "mode", "auto-strict");
        gst_util_set_object_arg(G_OBJECT(deinterlace), "fields", "all");
        gst_util_set_object_arg(G_OBJECT(deinterlace), "locking", "passive");
        configureVideoEncoder(encoder, videoEncoderFactory, bitrateKbps);
        // 202.74: repeat parameter sets with every IDR so late SRT/UDP subscribers
        // acquire decoder configuration immediately at the next keyframe.
        g_object_set(parser, "config-interval", -1, nullptr);

        const bool elementsAdded =
            add(context->bin, queue) && add(context->bin, convert) && add(context->bin, deinterlace) &&
            add(context->bin, scale) && (!postScaleConvert || add(context->bin, postScaleConvert)) &&
            add(context->bin, filter) && add(context->bin, encoder) && add(context->bin, parser) &&
            add(context->bin, outQueue);
        const bool videoLinked = elementsAdded &&
            (postScaleConvert
                ? gst_element_link_many(queue, convert, deinterlace, scale, postScaleConvert,
                                        filter, encoder, parser, outQueue, nullptr)
                : gst_element_link_many(queue, convert, deinterlace, scale,
                                        filter, encoder, parser, outQueue, nullptr));
        const auto nativeVideoCodec = hevc
            ? dvbstreamer5::media::mpegts::ElementaryCodec::H265
            : dvbstreamer5::media::mpegts::ElementaryCodec::H264;
        const char* nativeVideoCaps = hevc
            ? "video/x-h265,stream-format=byte-stream,alignment=au"
            : "video/x-h264,stream-format=byte-stream,alignment=au";
        if (!elementsAdded || !videoLinked ||
            !linkElementToNativeMux(outQueue, context,
                dvbstreamer5::media::mpegts::ElementaryKind::Video,
                nativeVideoCodec, nativeVideoCaps)) {
            std::cerr << "Transcoder: failed to build video branch" << std::endl;
            gst_caps_unref(caps);
            drainPad(context->bin, pad);
            return;
        }

        GstPad* sinkPad = gst_element_get_static_pad(queue, "sink");
        if (sinkPad && gst_pad_link(pad, sinkPad) == GST_PAD_LINK_OK) {
            context->videoLinked = true;
            std::cerr << "Transcoder: video linked using " << videoEncoderFactory << " "
                      << width << "x" << height << " @ "
                      << context->config.transcodeVideoBitrate
                      << " bit/s cadence=preserve-progressive/double-interlaced-fields"
                      << " headers=every-idr" << std::endl;
        }
        if (sinkPad) gst_object_unref(sinkPad);
        for (GstElement* e : {queue, convert, deinterlace, scale, postScaleConvert, filter, encoder, parser, outQueue}) sync(e);
    } else if (media.rfind("audio/x-raw", 0) == 0 && !context->audioLinked) {
        const std::string codec = context->config.transcodeAudioCodec == "mp3" ? "mp3" :
            (context->config.transcodeAudioCodec == "mp2" ? "mp2" : "aac");
        GstElement* queue = gst_element_factory_make("queue", nullptr);
        GstElement* convert = gst_element_factory_make("audioconvert", nullptr);
        GstElement* resample = gst_element_factory_make("audioresample", nullptr);
        GstElement* rate = gst_element_factory_make("audiorate", nullptr);
        GstElement* filter = gst_element_factory_make("capsfilter", nullptr);
        const auto encoderSelection = makeAudioEncoder(codec);
        GstElement* encoder = encoderSelection.element;
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(resample), "quality")) {
            g_object_set(resample, "quality", 6, nullptr);
        }
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(rate), "skip-to-first")) {
            g_object_set(rate, "skip-to-first", TRUE, nullptr);
        }
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(rate), "tolerance")) {
            g_object_set(rate, "tolerance", static_cast<guint64>(20 * GST_MSECOND), nullptr);
        }
        GstElement* parser = gst_element_factory_make(
            (codec == "mp3" || codec == "mp2") ? "mpegaudioparse" : "aacparse", nullptr);
        // MPEG audio gets fixed layer/rate caps for MPEG-TS stream-type selection.
        // AAC must negotiate directly from aacparse so codec_data is preserved.
        GstElement* encodedFilter = (codec == "mp3" || codec == "mp2")
            ? gst_element_factory_make("capsfilter", nullptr) : nullptr;
        GstElement* outQueue = gst_element_factory_make("queue", nullptr);
        if (!queue || !convert || !resample || !rate || !filter || !encoder || !parser ||
            ((codec == "mp3" || codec == "mp2") && !encodedFilter) || !outQueue) {
            std::cerr << "Transcoder: missing " << codec << " audio elements" << std::endl;
            gst_caps_unref(caps);
            drainPad(context->bin, pad);
            return;
        }

        // Use the exact PCM format accepted by the selected encoder.
        const char* rawAudioFormat = "S16LE";
        if (encoderSelection.factory == "avenc_aac") {
            rawAudioFormat = "F32LE";
        } else if (encoderSelection.factory == "avenc_mp3") {
            rawAudioFormat = "S16P";
        }
        const char* rawAudioLayout = encoderSelection.factory == "avenc_mp3"
            ? "non-interleaved"
            : "interleaved";
        GstCaps* audioCaps = gst_caps_new_simple("audio/x-raw",
            "format", G_TYPE_STRING, rawAudioFormat,
            "rate", G_TYPE_INT, 48000,
            "channels", G_TYPE_INT, 2,
            "layout", G_TYPE_STRING, rawAudioLayout,
            nullptr);
        g_object_set(filter, "caps", audioCaps, nullptr);
        gst_caps_unref(audioCaps);
        configureAudioBitrate(encoder, encoderSelection.factory, context->config.transcodeAudioBitrate);
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(parser), "disable-passthrough")) {
            g_object_set(parser, "disable-passthrough", TRUE, nullptr);
        }

        if (codec == "mp3" || codec == "mp2") {
            const int layer = codec == "mp2" ? 2 : 3;
            GstCaps* encodedCaps = gst_caps_new_simple(
                "audio/mpeg",
                "mpegversion", G_TYPE_INT, 1,
                "layer", G_TYPE_INT, layer,
                "parsed", G_TYPE_BOOLEAN, TRUE,
                "rate", G_TYPE_INT, 48000,
                "channels", G_TYPE_INT, 2,
                nullptr);
            g_object_set(encodedFilter, "caps", encodedCaps, nullptr);
            gst_caps_unref(encodedCaps);
        }

        // Normalize timestamps on the last encoded element before the mux.
        const GstClockTime audioFrameDuration = (codec == "mp3" || codec == "mp2")
            ? gst_util_uint64_scale_int(GST_SECOND, 1152, 48000)
            : gst_util_uint64_scale_int(GST_SECOND, 1024, 48000);
        attachTimestampNormalizer(encodedFilter ? encodedFilter : parser, audioFrameDuration);

        bool branchBuilt = add(context->bin, queue) && add(context->bin, convert) &&
            add(context->bin, resample) && add(context->bin, rate) && add(context->bin, filter) &&
            add(context->bin, encoder) && add(context->bin, parser);
        if (branchBuilt && encodedFilter) branchBuilt = add(context->bin, encodedFilter);
        branchBuilt = branchBuilt && add(context->bin, outQueue);

        bool branchLinked = false;
        if (branchBuilt) {
            if (encodedFilter) {
                branchLinked = gst_element_link_many(queue, convert, resample, rate, filter,
                    encoder, parser, encodedFilter, outQueue, nullptr);
            } else {
                branchLinked = gst_element_link_many(queue, convert, resample, rate, filter,
                    encoder, parser, outQueue, nullptr);
            }
        }

        const auto nativeAudioCodec = codec == "aac"
            ? dvbstreamer5::media::mpegts::ElementaryCodec::AacAdts
            : dvbstreamer5::media::mpegts::ElementaryCodec::MpegAudio;
        const char* nativeAudioCaps = codec == "aac"
            ? "audio/mpeg,mpegversion=4,stream-format=adts"
            : "audio/mpeg,mpegversion=1,parsed=true";
        if (!branchBuilt || !branchLinked ||
            !linkElementToNativeMux(outQueue, context,
                dvbstreamer5::media::mpegts::ElementaryKind::Audio,
                nativeAudioCodec, nativeAudioCaps)) {
            std::cerr << "Transcoder: failed to build " << codec << " audio branch with "
                      << encoderSelection.factory << std::endl;
            gst_caps_unref(caps);
            drainPad(context->bin, pad);
            return;
        }

        GstPad* sinkPad = gst_element_get_static_pad(queue, "sink");
        if (sinkPad && gst_pad_link(pad, sinkPad) == GST_PAD_LINK_OK) {
            context->audioLinked = true;
            std::cerr << "Transcoder: audio linked using " << encoderSelection.factory
                      << " input=" << rawAudioFormat << "/" << rawAudioLayout << "/48000/stereo"
                      << " output=" << (codec == "aac" ? "AAC negotiated by aacparse" :
                          (codec == "mp2" ? "MPEG-1 Layer II" : "MP3"))
                      << " at " << context->config.transcodeAudioBitrate << " bit/s" << std::endl;
        }
        if (sinkPad) gst_object_unref(sinkPad);
        for (GstElement* e : {queue, convert, resample, rate, filter, encoder, parser, encodedFilter, outQueue}) sync(e);
    } else {
        // Multiple programs, subtitles, data PIDs, and duplicate audio/video tracks must
        // be consumed. Leaving a tsdemux pad unlinked can propagate GST_FLOW_NOT_LINKED
        // and stop the complete stream.
        drainPad(context->bin, pad);
    }
    gst_caps_unref(caps);
}


bool buildVideoPassthroughBranch(TranscodeContext* context, GstPad* pad, GstCaps* caps) {
    if (!context || !context->bin || !context->outputAppSrc || !pad || !caps || context->videoLinked) return false;

    const GstStructure* structure = gst_caps_get_structure(caps, 0);
    if (!structure) return false;
    const char* mediaType = gst_structure_get_name(structure);
    std::string parserFactory;

    if (g_strcmp0(mediaType, "video/x-h264") == 0) {
        parserFactory = "h264parse";
    } else if (g_strcmp0(mediaType, "video/x-h265") == 0) {
        parserFactory = "h265parse";
    } else if (g_strcmp0(mediaType, "video/mpeg") == 0) {
        parserFactory = "mpegvideoparse";
    }

    if (parserFactory.empty()) {
        gchar* capsText = gst_caps_to_string(caps);
        std::cerr << "Transcoder: video passthrough does not support caps="
                  << (capsText ? capsText : "unknown") << std::endl;
        g_free(capsText);
        return false;
    }

    const auto nativeCodec = parserFactory == "h264parse"
        ? dvbstreamer5::media::mpegts::ElementaryCodec::H264
        : (parserFactory == "h265parse"
            ? dvbstreamer5::media::mpegts::ElementaryCodec::H265
            : dvbstreamer5::media::mpegts::ElementaryCodec::Mpeg2Video);
    const char* nativeCaps = parserFactory == "h264parse"
        ? "video/x-h264,stream-format=byte-stream,alignment=au"
        : (parserFactory == "h265parse"
            ? "video/x-h265,stream-format=byte-stream,alignment=au"
            : "video/mpeg,parsed=true");

    GstElement* queue = gst_element_factory_make("queue", nullptr);
    GstElement* parser = gst_element_factory_make(parserFactory.c_str(), nullptr);
    GstElement* outQueue = gst_element_factory_make("queue", nullptr);
    if (!queue || !parser || !outQueue || !add(context->bin, queue) ||
        !add(context->bin, parser) || !add(context->bin, outQueue) ||
        !gst_element_link_many(queue, parser, outQueue, nullptr) ||
        !linkElementToNativeMux(outQueue, context,
            dvbstreamer5::media::mpegts::ElementaryKind::Video,
            nativeCodec, nativeCaps)) {
        std::cerr << "Transcoder: failed to build video passthrough branch using "
                  << parserFactory << std::endl;
        return false;
    }

    if (g_object_class_find_property(G_OBJECT_GET_CLASS(parser), "disable-passthrough")) {
        g_object_set(parser, "disable-passthrough", FALSE, nullptr);
    }
    // Make late SRT/UDP subscribers recover quickly from a mid-GOP join.
    if ((parserFactory == "h264parse" || parserFactory == "h265parse") &&
        g_object_class_find_property(G_OBJECT_GET_CLASS(parser), "config-interval")) {
        g_object_set(parser, "config-interval", -1, nullptr);
    }

    GstPad* sinkPad = gst_element_get_static_pad(queue, "sink");
    const bool linked = sinkPad && gst_pad_link(pad, sinkPad) == GST_PAD_LINK_OK;
    if (sinkPad) gst_object_unref(sinkPad);
    if (!linked) return false;

    context->videoLinked = true;
    gchar* capsText = gst_caps_to_string(caps);
    std::cerr << "Transcoder: original video passthrough linked using " << parserFactory
              << " caps=" << (capsText ? capsText : "unknown") << std::endl;
    g_free(capsText);
    sync(queue);
    sync(parser);
    sync(outQueue);
    return true;
}

bool buildAudioPassthroughBranch(TranscodeContext* context, GstPad* pad, GstCaps* caps) {
    if (!context || !context->bin || !context->outputAppSrc || !pad || !caps || context->audioLinked) return false;

    const GstStructure* structure = gst_caps_get_structure(caps, 0);
    if (!structure) return false;
    const char* mediaType = gst_structure_get_name(structure);
    std::string parserFactory;

    if (g_strcmp0(mediaType, "audio/mpeg") == 0) {
        // parsebin may expose non-fixed caps such as:
        // audio/mpeg, mpegversion=(int){ 2, 4 }, stream-format=(string){ raw, adts, adif, loas }
        // gst_structure_get_int() fails on lists/ranges, so inspect the GValue directly.
        const bool canBeMpegAudio = structureFieldCanContainInt(structure, "mpegversion", 1) ||
            structureFieldCanContainInt(structure, "layer", 1) ||
            structureFieldCanContainInt(structure, "layer", 2) ||
            structureFieldCanContainInt(structure, "layer", 3);
        const bool canBeAac = structureFieldCanContainInt(structure, "mpegversion", 4) ||
            structureFieldCanContainInt(structure, "mpegversion", 2) ||
            structureFieldStringCanContain(structure, "stream-format", "adts") ||
            structureFieldStringCanContain(structure, "stream-format", "raw") ||
            structureFieldStringCanContain(structure, "stream-format", "loas") ||
            structureFieldStringCanContain(structure, "stream-format", "adif");

        if (canBeMpegAudio && !canBeAac) parserFactory = "mpegaudioparse";
        else if (canBeAac) parserFactory = "aacparse";
        else parserFactory = "aacparse";
    } else if (g_strcmp0(mediaType, "audio/x-ac3") == 0 ||
               g_strcmp0(mediaType, "audio/x-eac3") == 0) {
        parserFactory = "ac3parse";
    }

    if (parserFactory.empty()) {
        gchar* capsText = gst_caps_to_string(caps);
        std::cerr << "Transcoder: audio passthrough does not support caps="
                  << (capsText ? capsText : "unknown") << std::endl;
        g_free(capsText);
        return false;
    }

    const bool eac3 = g_strcmp0(mediaType, "audio/x-eac3") == 0;
    const auto nativeCodec = parserFactory == "aacparse"
        ? dvbstreamer5::media::mpegts::ElementaryCodec::AacAdts
        : (eac3
            ? dvbstreamer5::media::mpegts::ElementaryCodec::Eac3
            : (parserFactory == "ac3parse"
                ? dvbstreamer5::media::mpegts::ElementaryCodec::Ac3
                : dvbstreamer5::media::mpegts::ElementaryCodec::MpegAudio));
    const char* nativeCaps = parserFactory == "aacparse"
        ? "audio/mpeg,mpegversion=4,stream-format=adts"
        : (eac3 ? "audio/x-eac3,framed=true" :
           (parserFactory == "ac3parse" ? "audio/x-ac3,framed=true" :
            "audio/mpeg,mpegversion=1,parsed=true"));

    GstElement* queue = gst_element_factory_make("queue", nullptr);
    GstElement* parser = gst_element_factory_make(parserFactory.c_str(), nullptr);
    GstElement* outQueue = gst_element_factory_make("queue", nullptr);
    if (!queue || !parser || !outQueue || !add(context->bin, queue) ||
        !add(context->bin, parser) || !add(context->bin, outQueue) ||
        !gst_element_link_many(queue, parser, outQueue, nullptr) ||
        !linkElementToNativeMux(outQueue, context,
            dvbstreamer5::media::mpegts::ElementaryKind::Audio,
            nativeCodec, nativeCaps)) {
        std::cerr << "Transcoder: failed to build audio passthrough branch using "
                  << parserFactory << std::endl;
        return false;
    }

    if (g_object_class_find_property(G_OBJECT_GET_CLASS(parser), "disable-passthrough")) {
        g_object_set(parser, "disable-passthrough", FALSE, nullptr);
    }

    GstPad* sinkPad = gst_element_get_static_pad(queue, "sink");
    const bool linked = sinkPad && gst_pad_link(pad, sinkPad) == GST_PAD_LINK_OK;
    if (sinkPad) gst_object_unref(sinkPad);
    if (!linked) return false;

    context->audioLinked = true;
    gchar* capsText = gst_caps_to_string(caps);
    std::cerr << "Transcoder: original audio passthrough linked using " << parserFactory
              << " caps=" << (capsText ? capsText : "unknown") << std::endl;
    g_free(capsText);
    sync(queue);
    sync(parser);
    sync(outQueue);
    return true;
}

void onDemuxPadAdded(GstElement*, GstPad* pad, gpointer userData) {
    auto* context = static_cast<TranscodeContext*>(userData);
    if (!context || !context->bin) return;

    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    std::string capsText;
    if (caps) {
        gchar* serializedCaps = gst_caps_to_string(caps);
        if (serializedCaps) {
            capsText = serializedCaps;
            g_free(serializedCaps);
        }
        gst_caps_unref(caps);
    }
    const bool mediaPad = capsText.find("video/") != std::string::npos ||
                          capsText.find("audio/") != std::string::npos;
    if (!mediaPad) {
        drainPad(context->bin, pad);
        return;
    }

    if (context->config.transcodeVideoCodec == "copy" &&
        capsText.find("video/") != std::string::npos) {
        GstCaps* videoCaps = gst_pad_get_current_caps(pad);
        if (!videoCaps) videoCaps = gst_pad_query_caps(pad, nullptr);
        const bool linked = videoCaps && buildVideoPassthroughBranch(context, pad, videoCaps);
        if (videoCaps) gst_caps_unref(videoCaps);
        if (!linked) drainPad(context->bin, pad);
        return;
    }

    if (context->config.transcodeAudioCodec == "copy" &&
        capsText.find("audio/") != std::string::npos) {
        GstCaps* audioCaps = gst_pad_get_current_caps(pad);
        if (!audioCaps) audioCaps = gst_pad_query_caps(pad, nullptr);
        const bool linked = audioCaps && buildAudioPassthroughBranch(context, pad, audioCaps);
        if (audioCaps) gst_caps_unref(audioCaps);
        if (!linked) drainPad(context->bin, pad);
        return;
    }

    GstElement* queue = gst_element_factory_make("queue", nullptr);
    GstElement* decode = gst_element_factory_make("decodebin", nullptr);
    if (!queue || !decode || !add(context->bin, queue) || !add(context->bin, decode) ||
        !gst_element_link(queue, decode)) {
        std::cerr << "Transcoder: failed to create decoder branch" << std::endl;
        drainPad(context->bin, pad);
        return;
    }
    GstPad* sinkPad = gst_element_get_static_pad(queue, "sink");
    const bool linked = sinkPad && gst_pad_link(pad, sinkPad) == GST_PAD_LINK_OK;
    if (sinkPad) gst_object_unref(sinkPad);
    if (!linked) {
        drainPad(context->bin, pad);
        return;
    }
    g_signal_connect(decode, "pad-added", G_CALLBACK(onDecodedPadAdded), context);
    sync(queue);
    sync(decode);
}

} // namespace

int TranscoderModule::runEncoderProbeWorker(const std::string& factory) {
    GError* initError = nullptr;
    if (!gst_init_check(nullptr, nullptr, &initError)) {
        if (initError) g_error_free(initError);
        return 2;
    }
    if (!factoryAvailable(factory.c_str())) return 3;

    GstElement* pipeline = gst_pipeline_new("dvbstreamer5_encoder_probe");
    GstElement* source = gst_element_factory_make("videotestsrc", "probe_source");
    GstElement* filter = gst_element_factory_make("capsfilter", "probe_caps");
    GstElement* encoder = gst_element_factory_make(factory.c_str(), "probe_encoder");
    GstElement* sink = gst_element_factory_make("fakesink", "probe_sink");
    if (!pipeline || !source || !filter || !encoder || !sink) {
        if (source && !GST_OBJECT_PARENT(source)) gst_object_unref(source);
        if (filter && !GST_OBJECT_PARENT(filter)) gst_object_unref(filter);
        if (encoder && !GST_OBJECT_PARENT(encoder)) gst_object_unref(encoder);
        if (sink && !GST_OBJECT_PARENT(sink)) gst_object_unref(sink);
        if (pipeline) gst_object_unref(pipeline);
        return 4;
    }

    g_object_set(source, "num-buffers", 24, nullptr);
    GstCaps* caps = gst_caps_new_simple(
        "video/x-raw",
        "format", G_TYPE_STRING, "NV12",
        "width", G_TYPE_INT, 320,
        "height", G_TYPE_INT, 240,
        "framerate", GST_TYPE_FRACTION, 25, 1,
        nullptr);
    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(encoder), "bitrate")) {
        g_object_set(encoder, "bitrate", 1000u, nullptr);
    }
    g_object_set(sink, "sync", FALSE, "async", FALSE, nullptr);

    gst_bin_add_many(GST_BIN(pipeline), source, filter, encoder, sink, nullptr);
    if (!gst_element_link_many(source, filter, encoder, sink, nullptr)) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return 5;
    }

    GstBus* bus = gst_element_get_bus(pipeline);
    const GstStateChangeReturn stateResult = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (stateResult == GST_STATE_CHANGE_FAILURE) {
        if (bus) gst_object_unref(bus);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return 6;
    }

    GstMessage* message = bus
        ? gst_bus_timed_pop_filtered(
              bus,
              5 * GST_SECOND,
              static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))
        : nullptr;
    const bool ok = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
    if (message) gst_message_unref(message);
    if (bus) gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return ok ? 0 : 7;
}

std::string TranscoderModule::workingIntelVideoEncoderFactory() {
    static std::once_flag probeOnce;
    static std::string selected;

    std::call_once(probeOnce, []() {
        // Preserve the preferred order on modern Intel systems, but only keep
        // an encoder after it has encoded real NV12 frames in a child process.
        for (const char* name : {"qsvh264enc", "vah264enc", "vaapih264enc"}) {
            if (!factoryAvailable(name)) continue;
            const EncoderProbeResult probe = probeVideoEncoderFactory(name);
            std::cerr << "Intel encoder probe 203.08: factory=" << name
                      << " result=" << (probe.ok ? "ok" : "failed");
            if (probe.timedOut) std::cerr << " reason=timeout";
            else if (probe.signal != 0) std::cerr << " signal=" << probe.signal;
            else if (probe.exitCode >= 0) std::cerr << " exit=" << probe.exitCode;
            std::cerr << std::endl;
            if (probe.ok) {
                selected = name;
                break;
            }
        }

        std::cerr << "Intel encoder selection 203.08: selected="
                  << (selected.empty() ? "none" : selected)
                  << " policy=runtime-probe qsv->va->legacy-vaapi" << std::endl;
    });

    return selected;
}

std::string TranscoderModule::workingIntelHevcEncoderFactory() {
    static std::once_flag probeOnce;
    static std::string selected;

    std::call_once(probeOnce, []() {
        for (const char* name : {"qsvh265enc", "vah265enc", "vaapih265enc"}) {
            if (!factoryAvailable(name)) continue;
            const EncoderProbeResult probe = probeVideoEncoderFactory(name);
            std::cerr << "Intel HEVC encoder probe 203.75: factory=" << name
                      << " result=" << (probe.ok ? "ok" : "failed");
            if (probe.timedOut) std::cerr << " reason=timeout";
            else if (probe.signal != 0) std::cerr << " signal=" << probe.signal;
            else if (probe.exitCode >= 0) std::cerr << " exit=" << probe.exitCode;
            std::cerr << std::endl;
            if (probe.ok) {
                selected = name;
                break;
            }
        }
        std::cerr << "Intel HEVC encoder selection 203.75: selected="
                  << (selected.empty() ? "none" : selected)
                  << " policy=runtime-probe qsv->va->legacy-vaapi" << std::endl;
    });
    return selected;
}

TranscoderCapabilities TranscoderModule::inspectCapabilities() {
    TranscoderCapabilities result;
    result.mp2EncoderAvailable = dvbstreamer5_gst_mp2_encoder_register() &&
        factoryAvailable("mpegaudioparse");
    const char* required[] = {
        "parsebin", "decodebin", "queue",
        "videoconvert", "deinterlace", "videoscale", "capsfilter",
        "audioconvert", "audioresample", "audiorate",
        "aacparse", "appsrc", "appsink", nullptr
    };

    for (const char** name = required; *name; ++name) {
        GstElementFactory* factory = gst_element_factory_find(*name);
        if (!factory) result.missingElements.emplace_back(*name);
        else gst_object_unref(factory);
    }

    const bool h264Parser = factoryAvailable("h264parse");
    result.x264Available = factoryAvailable("x264enc");
    result.nvencAvailable = factoryAvailable("nvh264enc");
    result.intelEncoder = intelVideoEncoderFactory();
    result.intelAvailable = !result.intelEncoder.empty();
    if (h264Parser) {
        if (result.nvencAvailable) result.videoEncoder = "nvh264enc";
        else if (result.intelAvailable) result.videoEncoder = result.intelEncoder;
        else if (result.x264Available) result.videoEncoder = "x264enc";
    }

    const bool hevcParser = factoryAvailable("h265parse");
    result.x265Available = factoryAvailable("x265enc");
    result.nvencHevcAvailable = factoryAvailable("nvh265enc");
    result.intelHevcEncoder = intelHevcVideoEncoderFactory();
    result.intelHevcAvailable = !result.intelHevcEncoder.empty();
    if (hevcParser) {
        if (result.nvencHevcAvailable) result.hevcVideoEncoder = "nvh265enc";
        else if (result.intelHevcAvailable) result.hevcVideoEncoder = result.intelHevcEncoder;
        else if (result.x265Available) result.hevcVideoEncoder = "x265enc";
    }
    if (result.videoEncoder.empty() && result.hevcVideoEncoder.empty()) {
        result.missingElements.emplace_back(
            "video encoder: H.264 nvh264/qsv/va/x264 or HEVC nvh265/qsv/va/x265");
    }
    GstElementFactory* aacParser = gst_element_factory_find("aacparse");
    if (aacParser) {
        gst_object_unref(aacParser);
        for (const char* name : {"voaacenc", "fdkaacenc", "avenc_aac"}) {
            GstElementFactory* factory = gst_element_factory_find(name);
            if (factory) {
                result.aacEncoder = name;
                gst_object_unref(factory);
                break;
            }
        }
    }
    GstElementFactory* mp3Parser = gst_element_factory_find("mpegaudioparse");
    if (mp3Parser) {
        gst_object_unref(mp3Parser);
        for (const char* name : {"lamemp3enc", "avenc_mp3"}) {
            GstElementFactory* factory = gst_element_factory_find(name);
            if (factory) {
                result.mp3Encoder = name;
                gst_object_unref(factory);
                break;
            }
        }
    }
    result.audioEncoder = !result.aacEncoder.empty() ? result.aacEncoder :
        (!result.mp3Encoder.empty() ? result.mp3Encoder :
         (result.mp2EncoderAvailable ? "dvbstreamer5mp2enc" : std::string()));
    if (GstElementFactory* factory = gst_element_factory_find("deinterlace")) {
        result.deinterlaceAvailable = true;
        gst_object_unref(factory);
    }
    result.available = result.missingElements.empty();
    result.message = result.available
        ? "In-process transcoding is available: H.264=" +
              (result.videoEncoder.empty() ? std::string("unavailable") : result.videoEncoder) +
              ", HEVC=" +
              (result.hevcVideoEncoder.empty() ? std::string("unavailable") : result.hevcVideoEncoder) +
              ", native MPEG-TS mux=enabled, external gst-launch=disabled"
        : "Transcoding is unavailable because required in-process media elements are missing";
    return result;
}

bool TranscoderModule::resolutionSize(const std::string& value, int& width, int& height) {
    dvbstreamer5::transcode::VideoGeometry geometry;
    if (!dvbstreamer5::transcode::videoGeometry(value, geometry)) return false;
    width = geometry.width;
    height = geometry.height;
    return true;
}

uint64_t TranscoderModule::recommendedVideoBitrate(const std::string& value) {
    if (value == "3840x2160") return 25000000;
    if (value == "3200x1800") return 18000000;
    if (value == "2560x1440") return 12000000;
    if (value == "1920x1080") return 6000000;
    if (value == "1280x720") return 3500000;
    if (value == "1024x576") return 2500000;
    if (value == "720x576_16_9") return 2000000;
    if (value == "720x576") return 2000000;
    return 6000000;
}

GstElement* TranscoderModule::createBin(const StreamConfig& config, std::string& error) {
    const std::string videoCodec =
        config.transcodeVideoCodec == "copy" ? "copy" :
        (toLower(config.transcodeVideoCodec) == "hevc" ? "hevc" : "h264");
    int width = 0, height = 0;
    if (videoCodec != "copy" && !resolutionSize(config.transcodeResolution, width, height)) {
        error = "unsupported transcode resolution";
        return nullptr;
    }
    const auto capabilities = inspectCapabilities();
    if (!capabilities.available) {
        error = capabilities.message;
        if (!capabilities.missingElements.empty()) {
            error += ": ";
            for (size_t i = 0; i < capabilities.missingElements.size(); ++i) {
                if (i) error += ", ";
                error += capabilities.missingElements[i];
            }
        }
        return nullptr;
    }
    if (videoCodec != "copy") {
        if (videoCodec == "hevc" && !factoryAvailable("h265parse")) {
            error = "HEVC was requested but GStreamer h265parse is not available";
            return nullptr;
        }
        if (videoCodec == "h264" && !factoryAvailable("h264parse")) {
            error = "H.264 was requested but GStreamer h264parse is not available";
            return nullptr;
        }
        const std::string selected = selectedVideoEncoderFactory(config);
        if (selected.empty()) {
            error = videoCodec == "hevc"
                ? "HEVC encoder is unavailable (need nvh265enc, Intel qsv/va H.265, or x265enc)"
                : "H.264 encoder is unavailable (need nvh264enc, Intel qsv/va H.264, or x264enc)";
            return nullptr;
        }
    }

    const std::string audioCodec = config.transcodeAudioCodec == "copy" ? "copy" :
        (config.transcodeAudioCodec == "mp3" ? "mp3" :
         (config.transcodeAudioCodec == "mp2" ? "mp2" : "aac"));
    if ((audioCodec == "aac" && capabilities.aacEncoder.empty()) ||
        (audioCodec == "mp3" && capabilities.mp3Encoder.empty()) ||
        (audioCodec == "mp2" && !capabilities.mp2EncoderAvailable)) {
        error = audioCodec + " encoder is not available";
        return nullptr;
    }
    if (audioCodec == "mp2") {
        const uint64_t bitrate = config.transcodeAudioBitrate;
        if (bitrate != 96000 && bitrate != 128000 && bitrate != 160000 &&
            bitrate != 192000 && bitrate != 256000 && bitrate != 320000) {
            error = "MP2 audio bitrate must be one of 96, 128, 160, 192, 256, or 320 kbit/s";
            return nullptr;
        }
    }

    GstElement* bin = gst_bin_new("transcoder_bin");
    GstElement* inputQueue = gst_element_factory_make("queue", "transcode_input_queue");
    GstElement* parsebin = gst_element_factory_make("parsebin", "transcode_parsebin");
    GstElement* outputAppSrc = gst_element_factory_make("appsrc", "transcode_native_ts_source");
    if (!bin || !inputQueue || !parsebin || !outputAppSrc ||
        !add(bin, inputQueue) || !add(bin, parsebin) || !add(bin, outputAppSrc)) {
        error = "failed to create transcoder bin elements";
        if (bin) gst_object_unref(bin);
        return nullptr;
    }

    const guint64 elementaryMuxBitrate = static_cast<guint64>(
        (videoCodec == "copy" ? std::max<uint64_t>(config.transcodeVideoBitrate, 500000)
                              : config.transcodeVideoBitrate) +
        (audioCodec == "copy" ? 384000 : config.transcodeAudioBitrate) + 350000);
    const guint64 muxBitrate = static_cast<guint64>(std::max<uint64_t>(
        elementaryMuxBitrate, config.cbr ? config.targetBitrate : 0));
    GstCaps* tsCaps = gst_caps_new_simple(
        "video/mpegts",
        "systemstream", G_TYPE_BOOLEAN, TRUE,
        "packetsize", G_TYPE_INT, 188,
        nullptr);
    g_object_set(outputAppSrc,
        "caps", tsCaps,
        "is-live", TRUE,
        "format", GST_FORMAT_TIME,
        "block", TRUE,
        "max-bytes", static_cast<guint64>(8 * 1024 * 1024),
        "do-timestamp", FALSE,
        nullptr);
    gst_caps_unref(tsCaps);
    gst_app_src_set_stream_type(GST_APP_SRC(outputAppSrc), GST_APP_STREAM_TYPE_STREAM);

    std::cerr << "Transcoder Stage 2: video=" << videoCodec
              << " video_encoder=" << (videoCodec == "copy" ? "copy" : selectedVideoEncoderFactory(config))
              << " audio=" << audioCodec
              << " cadence=preserve-progressive/double-interlaced-fields"
              << " native_mux_bitrate=" << muxBitrate
              << " mpegtsmux=off tsparse=off" << std::endl;
    if (!gst_element_link(inputQueue, parsebin)) {
        error = "failed to link transcoder parser input";
        gst_object_unref(bin);
        return nullptr;
    }

    GstPad* parseSink = gst_element_get_static_pad(inputQueue, "sink");
    GstPad* outputSrc = gst_element_get_static_pad(outputAppSrc, "src");
    GstPad* ghostSink = parseSink ? gst_ghost_pad_new("sink", parseSink) : nullptr;
    GstPad* ghostSrc = outputSrc ? gst_ghost_pad_new("src", outputSrc) : nullptr;
    if (parseSink) gst_object_unref(parseSink);
    if (outputSrc) gst_object_unref(outputSrc);
    if (!ghostSink || !ghostSrc || !gst_element_add_pad(bin, ghostSink) ||
        !gst_element_add_pad(bin, ghostSrc)) {
        if (ghostSink && !GST_OBJECT_PARENT(ghostSink)) gst_object_unref(ghostSink);
        if (ghostSrc && !GST_OBJECT_PARENT(ghostSrc)) gst_object_unref(ghostSrc);
        error = "failed to create transcoder ghost pads";
        gst_object_unref(bin);
        return nullptr;
    }

    auto* context = new TranscodeContext();
    context->bin = bin;
    context->outputAppSrc = outputAppSrc;
    context->config = config;
    dvbstreamer5::media::mpegts::NativeMuxConfig muxConfig;
    muxConfig.serviceId = static_cast<std::uint16_t>(config.serviceId == 0 ? 1 : config.serviceId);
    muxConfig.videoPid = static_cast<std::uint16_t>(config.videoPid);
    muxConfig.audioPid = static_cast<std::uint16_t>(config.audioPid);
    muxConfig.targetBitrate = muxBitrate;
    muxConfig.serviceName = config.serviceName.empty() ? config.name : config.serviceName;
    muxConfig.serviceProvider = config.serviceProvider.empty() ? "DVBStreamer5" : config.serviceProvider;
    if (!context->mux.initialize(muxConfig, error)) {
        delete context;
        gst_object_unref(bin);
        return nullptr;
    }
    g_object_set_data_full(G_OBJECT(bin), "dvbstreamer5-transcode-context", context,
        [](gpointer p) { delete static_cast<TranscodeContext*>(p); });
    g_signal_connect(parsebin, "pad-added", G_CALLBACK(onDemuxPadAdded), context);
    return bin;
}
