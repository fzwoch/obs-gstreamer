/*
 * obs-gstreamer. OBS Studio plugin.
 * Copyright (C) 2018-2021 Florian Zwoch <fzwoch@gmail.com>
 *
 * This file is part of obs-gstreamer.
 *
 * obs-gstreamer is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * obs-gstreamer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with obs-gstreamer. If not, see <http://www.gnu.org/licenses/>.
 */

#include <obs/obs-module.h>
#include <gst/gst.h>
#include <gst/app/app.h>

#include "plugin-i18n.h"

// How long to wait for EOS when stopping the output before forcing teardown.
#define OUTPUT_EOS_TIMEOUT (3 * GST_SECOND)

typedef struct {
	GstElement *pipe;
	GstElement *video;
	GstElement *audio;
	obs_output_t *output;
	obs_data_t *settings;
} data_t;

const char *gstreamer_output_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "GStreamer Output";
}

void *gstreamer_output_create(obs_data_t *settings, obs_output_t *output)
{
	data_t *data = g_new0(data_t, 1);

	data->output = output;
	data->settings = settings;

	return data;
}

void gstreamer_output_destroy(void *data)
{
	// The pipeline is owned and cleaned up by stop(); data itself belongs
	// to libobs and must not be freed here beyond our allocation.
	g_free(data);
}

static int speaker_to_channels(enum speaker_layout speakers)
{
	switch (speakers) {
	case SPEAKERS_MONO:
		return 1;
	case SPEAKERS_STEREO:
		return 2;
	case SPEAKERS_2POINT1:
		return 3;
	case SPEAKERS_4POINT0:
		return 4;
	case SPEAKERS_4POINT1:
		return 5;
	case SPEAKERS_5POINT1:
		return 6;
	case SPEAKERS_7POINT1:
		return 8;
	default:
		return 0;
	}
}

static int get_aac_frequency_index(int samples_per_sec)
{
	static const int frequencies[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
					  22050, 16000, 12000, 11025, 8000,  7350};

	for (size_t i = 0; i < sizeof(frequencies) / sizeof(frequencies[0]); i++) {
		if (frequencies[i] == samples_per_sec)
			return (int)i;
	}

	return -1;
}

// Build an AudioSpecificConfig caps fragment for AAC-LC matching the actual
// output audio configuration. The previously hardcoded value (1190) only
// described 48 kHz stereo.
static gchar *make_aac_codec_data_fragment(int samples_per_sec, int channels)
{
	int frequency_index = get_aac_frequency_index(samples_per_sec);
	if (frequency_index < 0)
		return NULL;

	// 5 bits audio object type (2 == AAC-LC), 4 bits sampling frequency
	// index, 4 bits channel configuration.
	unsigned int asc = (2 << 11) | ((unsigned int)frequency_index << 7) | ((unsigned int)channels << 3);

	return g_strdup_printf("codec_data=(buffer)%04x", asc);
}

static void clear_pipeline(data_t *data)
{
	if (data->video != NULL) {
		gst_object_unref(data->video);
		data->video = NULL;
	}
	if (data->audio != NULL) {
		gst_object_unref(data->audio);
		data->audio = NULL;
	}
	if (data->pipe != NULL) {
		gst_element_set_state(data->pipe, GST_STATE_NULL);
		gst_object_unref(data->pipe);
		data->pipe = NULL;
	}
}

bool gstreamer_output_start(void *p)
{
	data_t *data = (data_t *)p;

	struct obs_video_info ovi;
	struct obs_audio_info oai;

	if (!obs_get_video_info(&ovi) || !obs_get_audio_info(&oai)) {
		blog(LOG_ERROR, "[obs-gstreamer] output: cannot obtain video/audio info");
		obs_output_set_last_error(data->output, "Cannot obtain video/audio info");
		return false;
	}

	int channels = speaker_to_channels(oai.speakers);
	if (channels == 0) {
		blog(LOG_ERROR, "[obs-gstreamer] output: unsupported speaker layout: %d", oai.speakers);
		obs_output_set_last_error(data->output, "Unsupported speaker layout");
		return false;
	}

	gchar *codec_data_fragment = make_aac_codec_data_fragment(oai.samples_per_sec, channels);
	if (codec_data_fragment == NULL) {
		blog(LOG_ERROR, "[obs-gstreamer] output: unsupported sample rate for AAC: %d", oai.samples_per_sec);
		obs_output_set_last_error(data->output,
					  "Unsupported sample rate for AAC (use one of the standard MPEG-4 rates)");
		return false;
	}

	GError *err = NULL;

	gchar *pipe = g_strdup_printf(
		"appsrc name=appsrc_video ! video/x-h264, width=%d, height=%d, stream-format=byte-stream ! h264parse name=video "
		"appsrc name=appsrc_audio ! audio/mpeg, mpegversion=4, stream-format=raw, rate=%d, channels=%d, %s ! aacparse name=audio "
		"%s",
		ovi.output_width, ovi.output_height, oai.samples_per_sec, channels, codec_data_fragment,
		obs_data_get_string(data->settings, "pipeline"));

	g_free(codec_data_fragment);

	// Note: gst_parse_launch() can return a partially built pipeline even
	// when it reports an error, so always clean up via clear_pipeline().
	data->pipe = gst_parse_launch(pipe, &err);
	g_free(pipe);
	if (err != NULL) {
		blog(LOG_ERROR, "[obs-gstreamer] output: %s", err->message);
		g_error_free(err);
		clear_pipeline(data);
		obs_output_set_last_error(data->output, err->message);
		return false;
	}

	data->video = gst_bin_get_by_name(GST_BIN(data->pipe), "appsrc_video");
	data->audio = gst_bin_get_by_name(GST_BIN(data->pipe), "appsrc_audio");

	if (data->video == NULL || data->audio == NULL) {
		blog(LOG_ERROR, "[obs-gstreamer] output: pipeline misses appsrc_video/appsrc_audio");
		clear_pipeline(data);
		obs_output_set_last_error(data->output, "Pipeline misses appsrc_video/appsrc_audio elements");
		return false;
	}

	g_object_set(data->video, "format", GST_FORMAT_TIME, NULL);
	g_object_set(data->audio, "format", GST_FORMAT_TIME, NULL);

	if (!obs_output_can_begin_data_capture(data->output, 0)) {
		clear_pipeline(data);
		obs_output_set_last_error(data->output, "Cannot begin data capture");
		return false;
	}
	if (!obs_output_initialize_encoders(data->output, 0)) {
		clear_pipeline(data);
		obs_output_set_last_error(data->output, "Failed to initialize encoders");
		return false;
	}

	gst_element_set_state(data->pipe, GST_STATE_PLAYING);

	obs_output_begin_data_capture(data->output, 0);

	// Started fine; clear any previously shown error.
	obs_output_set_last_error(data->output, NULL);

	return true;
}

void gstreamer_output_stop(void *p, uint64_t ts)
{
	UNUSED_PARAMETER(ts);

	data_t *data = (data_t *)p;

	obs_output_end_data_capture(data->output);

	if (data->pipe == NULL)
		return;

	if (data->video != NULL)
		gst_app_src_end_of_stream(GST_APP_SRC(data->video));
	if (data->audio != NULL)
		gst_app_src_end_of_stream(GST_APP_SRC(data->audio));

	GstBus *bus = gst_element_get_bus(data->pipe);
	GstMessage *msg = gst_bus_timed_pop_filtered(bus, OUTPUT_EOS_TIMEOUT, GST_MESSAGE_EOS);
	if (msg != NULL) {
		gst_message_unref(msg);
	} else {
		// Pipeline never reached EOS (mislinked user pipeline, stalled
		// element). Tear down instead of blocking forever.
		blog(LOG_WARNING, "[obs-gstreamer] output: timed out waiting for EOS");
	}
	gst_object_unref(bus);

	clear_pipeline(data);
}

void gstreamer_output_encoded_packet(void *p, struct encoder_packet *packet)
{
	data_t *data = (data_t *)p;

	if (data->video == NULL || data->audio == NULL)
		return;

	GstBuffer *buffer = gst_buffer_new_allocate(NULL, packet->size, NULL);
	gst_buffer_fill(buffer, 0, packet->data, packet->size);

	// Convert encoder timebase ticks to nanoseconds without truncation for
	// fractional time bases.
	GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(packet->pts, (guint64)GST_SECOND * packet->timebase_num,
						       packet->timebase_den);
	GST_BUFFER_DTS(buffer) = gst_util_uint64_scale(packet->dts, (guint64)GST_SECOND * packet->timebase_num,
						       packet->timebase_den);

	gst_buffer_set_flags(buffer, packet->keyframe ? 0 : GST_BUFFER_FLAG_DELTA_UNIT);

	GstElement *appsrc = packet->type == OBS_ENCODER_VIDEO ? data->video : data->audio;

	gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer);
}

void gstreamer_output_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "pipeline", "video. ! matroskamux name=mux ! fakesink audio. ! mux.");
}

obs_properties_t *gstreamer_output_get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_property_t *prop = obs_properties_add_text(props, "pipeline", T("pipeline.label"), OBS_TEXT_MULTILINE);

	obs_property_set_long_description(prop, T("output.pipeline.desc"));

	UNUSED_PARAMETER(data);
	return props;
}
