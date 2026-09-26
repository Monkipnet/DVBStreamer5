#include "media/GstMp2Encoder.h"

#include "media/Mp2Encoder.h"

#include <gst/audio/audio.h>
#include <gst/audio/gstaudioencoder.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

typedef struct _TvsGstMp2Encoder {
    GstAudioEncoder parent;
    guint bitrate;
    guint sample_rate;
    guint channels;
    tvs::media::Mp2Encoder* encoder;
} TvsGstMp2Encoder;

typedef struct _TvsGstMp2EncoderClass {
    GstAudioEncoderClass parent_class;
} TvsGstMp2EncoderClass;

static void tvs_gst_mp2_encoder_class_init(TvsGstMp2EncoderClass* klass);
static void tvs_gst_mp2_encoder_init(TvsGstMp2Encoder* self);

G_DEFINE_TYPE(TvsGstMp2Encoder, tvs_gst_mp2_encoder, GST_TYPE_AUDIO_ENCODER)

namespace {

enum {
    kPropertyNone,
    kPropertyBitrate
};

constexpr guint kSamplesPerFrame = 1152;

GstStaticPadTemplate kSinkTemplate = GST_STATIC_PAD_TEMPLATE(
    "sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "audio/x-raw, "
        "format=(string)S16LE, "
        "layout=(string)interleaved, "
        "rate=(int){32000,44100,48000}, "
        "channels=(int){1,2}"));

GstStaticPadTemplate kSrcTemplate = GST_STATIC_PAD_TEMPLATE(
    "src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "audio/mpeg, "
        "mpegversion=(int)1, "
        "layer=(int)2, "
        "rate=(int){32000,44100,48000}, "
        "channels=(int){1,2}"));

void setProperty(GObject* object, guint propertyId, const GValue* value, GParamSpec* spec) {
    auto* self = reinterpret_cast<TvsGstMp2Encoder*>(object);
    if (propertyId == kPropertyBitrate) self->bitrate = g_value_get_uint(value);
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, propertyId, spec);
}

void getProperty(GObject* object, guint propertyId, GValue* value, GParamSpec* spec) {
    auto* self = reinterpret_cast<TvsGstMp2Encoder*>(object);
    if (propertyId == kPropertyBitrate) g_value_set_uint(value, self->bitrate);
    else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, propertyId, spec);
}

gboolean setFormat(GstAudioEncoder* encoder, GstAudioInfo* info) {
    auto* self = reinterpret_cast<TvsGstMp2Encoder*>(encoder);
    if (GST_AUDIO_INFO_FORMAT(info) != GST_AUDIO_FORMAT_S16LE) {
        GST_ELEMENT_ERROR(self, STREAM, FORMAT, ("MP2 encoder requires S16LE PCM"), (nullptr));
        return FALSE;
    }

    const guint sampleRate = static_cast<guint>(GST_AUDIO_INFO_RATE(info));
    const guint channels = static_cast<guint>(GST_AUDIO_INFO_CHANNELS(info));
    auto replacement = std::make_unique<tvs::media::Mp2Encoder>();
    std::string error;
    if (!replacement->initialize({sampleRate, channels, self->bitrate}, error)) {
        GST_ELEMENT_ERROR(self, STREAM, FORMAT, ("Unsupported MP2 input profile"), ("%s", error.c_str()));
        return FALSE;
    }

    GstCaps* outputCaps = gst_caps_new_simple(
        "audio/mpeg",
        "mpegversion", G_TYPE_INT, 1,
        "layer", G_TYPE_INT, 2,
        "rate", G_TYPE_INT, static_cast<gint>(sampleRate),
        "channels", G_TYPE_INT, static_cast<gint>(channels),
        nullptr);
    const gboolean negotiated = gst_audio_encoder_set_output_format(encoder, outputCaps);
    gst_caps_unref(outputCaps);
    if (!negotiated) {
        GST_ELEMENT_ERROR(self, CORE, NEGOTIATION, ("Could not negotiate MPEG-1 Layer II output"), (nullptr));
        return FALSE;
    }

    delete self->encoder;
    self->encoder = replacement.release();
    self->sample_rate = sampleRate;
    self->channels = channels;
    gst_audio_encoder_set_frame_samples_min(encoder, kSamplesPerFrame);
    gst_audio_encoder_set_frame_samples_max(encoder, kSamplesPerFrame);
    return TRUE;
}

GstFlowReturn emitEncoded(TvsGstMp2Encoder* self,
                          const std::vector<std::uint8_t>& bytes,
                          gint samples) {
    if (bytes.empty()) return gst_audio_encoder_finish_frame(
        GST_AUDIO_ENCODER(self), nullptr, samples);

    GstBuffer* output = gst_audio_encoder_allocate_output_buffer(
        GST_AUDIO_ENCODER(self), bytes.size());
    if (!output) return GST_FLOW_ERROR;
    GstMapInfo map;
    if (!gst_buffer_map(output, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(output);
        return GST_FLOW_ERROR;
    }
    std::memcpy(map.data, bytes.data(), bytes.size());
    gst_buffer_unmap(output, &map);
    return gst_audio_encoder_finish_frame(GST_AUDIO_ENCODER(self), output, samples);
}

GstFlowReturn handleFrame(GstAudioEncoder* encoder, GstBuffer* input) {
    auto* self = reinterpret_cast<TvsGstMp2Encoder*>(encoder);
    if (!self->encoder) {
        GST_ELEMENT_ERROR(self, STREAM, ENCODE, ("MP2 encoder is not configured"), (nullptr));
        return GST_FLOW_NOT_NEGOTIATED;
    }

    std::vector<std::uint8_t> encoded;
    std::string error;
    if (!input) {
        if (!self->encoder->finish(encoded, error)) {
            GST_ELEMENT_ERROR(self, STREAM, ENCODE, ("MP2 finalization failed"), ("%s", error.c_str()));
            return GST_FLOW_ERROR;
        }
        return emitEncoded(self, encoded, 0);
    }

    GstMapInfo map;
    if (!gst_buffer_map(input, &map, GST_MAP_READ)) return GST_FLOW_ERROR;
    const std::size_t sampleBytes = sizeof(std::int16_t) * self->channels;
    if (sampleBytes == 0 || map.size % sampleBytes != 0) {
        gst_buffer_unmap(input, &map);
        GST_ELEMENT_ERROR(self, STREAM, FORMAT, ("PCM buffer is not channel aligned"), (nullptr));
        return GST_FLOW_ERROR;
    }
    const std::size_t samples = map.size / sampleBytes;
    if (samples > static_cast<std::size_t>(G_MAXINT)) {
        gst_buffer_unmap(input, &map);
        GST_ELEMENT_ERROR(self, STREAM, FORMAT, ("PCM buffer is too large"), (nullptr));
        return GST_FLOW_ERROR;
    }
    const bool ok = self->encoder->encodeInterleaved(
        reinterpret_cast<const std::int16_t*>(map.data), samples, encoded, error);
    gst_buffer_unmap(input, &map);
    if (!ok) {
        GST_ELEMENT_ERROR(self, STREAM, ENCODE, ("MP2 encoding failed"), ("%s", error.c_str()));
        return GST_FLOW_ERROR;
    }
    return emitEncoded(self, encoded, static_cast<gint>(samples));
}

void flush(GstAudioEncoder* encoder) {
    auto* self = reinterpret_cast<TvsGstMp2Encoder*>(encoder);
    if (!self->sample_rate || !self->channels) return;

    delete self->encoder;
    self->encoder = new tvs::media::Mp2Encoder();
    std::string error;
    if (!self->encoder->initialize(
            {self->sample_rate, self->channels, self->bitrate}, error)) {
        delete self->encoder;
        self->encoder = nullptr;
        GST_ELEMENT_ERROR(self, STREAM, FORMAT, ("Could not reset MP2 encoder"), ("%s", error.c_str()));
    }
}

void finalize(GObject* object) {
    auto* self = reinterpret_cast<TvsGstMp2Encoder*>(object);
    delete self->encoder;
    self->encoder = nullptr;
    G_OBJECT_CLASS(tvs_gst_mp2_encoder_parent_class)->finalize(object);
}

} // namespace

static void tvs_gst_mp2_encoder_class_init(TvsGstMp2EncoderClass* klass) {
    auto* objectClass = G_OBJECT_CLASS(klass);
    auto* elementClass = GST_ELEMENT_CLASS(klass);
    auto* audioEncoderClass = GST_AUDIO_ENCODER_CLASS(klass);

    objectClass->set_property = setProperty;
    objectClass->get_property = getProperty;
    objectClass->finalize = finalize;
    g_object_class_install_property(
        objectClass,
        kPropertyBitrate,
        g_param_spec_uint(
            "bitrate",
            "Bitrate",
            "MPEG-1 Layer II bitrate in bits per second",
            32000,
            384000,
            192000,
            static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    gst_element_class_set_static_metadata(
        elementClass,
        "DVBStreamer5 MP2 audio encoder",
        "Codec/Encoder/Audio",
        "Encodes signed 16-bit PCM to MPEG-1 Layer II using vendored TwoLAME",
        "DVBStreamer5 contributors");
    gst_element_class_add_static_pad_template(elementClass, &kSinkTemplate);
    gst_element_class_add_static_pad_template(elementClass, &kSrcTemplate);

    audioEncoderClass->set_format = setFormat;
    audioEncoderClass->handle_frame = handleFrame;
    audioEncoderClass->flush = flush;
}

static void tvs_gst_mp2_encoder_init(TvsGstMp2Encoder* self) {
    self->bitrate = 192000;
    self->sample_rate = 0;
    self->channels = 0;
    self->encoder = nullptr;
    gst_audio_encoder_set_frame_samples_min(GST_AUDIO_ENCODER(self), kSamplesPerFrame);
    gst_audio_encoder_set_frame_samples_max(GST_AUDIO_ENCODER(self), kSamplesPerFrame);
}

gboolean tvs_gst_mp2_encoder_register(void) {
    GstElementFactory* factory = gst_element_factory_find("dvbstreamer5mp2enc");
    if (factory) {
        gst_object_unref(factory);
        return TRUE;
    }
    return gst_element_register(
        nullptr, "dvbstreamer5mp2enc", GST_RANK_NONE, tvs_gst_mp2_encoder_get_type());
}
