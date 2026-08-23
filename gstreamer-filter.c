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
#include <gst/video/video.h>
#include <gst/audio/audio.h>
#include <gst/app/app.h>

// Upper bound for how long the video filter waits for a converted sample.
// Keeps a stalled user pipeline from blocking OBS' graphics thread forever.
#define FILTER_VIDEO_PULL_TIMEOUT (15 * GST_MSECOND)

typedef struct {
	GstElement *pipe;
	GstElement *appsrc;
	GstElement *appsink;
	GMutex mutex;
	gint frame_size;
	uint32_t last_width;
	uint32_t last_height;
	enum video_format last_format;
	GstAudioInfo audio_info;
	char last_error[256];
	obs_source_t *source;
	obs_data_t *settings;
} data_t;

static gboolean bus_callback(GstBus *bus, GstMessage *message, gpointer user_data)
{
	data_t *data = user_data;

	switch (GST_MESSAGE_TYPE(message)) {
	case GST_MESSAGE_ERROR: {
		GError *err;
		gst_message_parse_error(message, &err, NULL);
		const char *source_name = obs_source_get_name(data->source);
		blog(LOG_ERROR, "[obs-gstreamer] %s: %s", source_name, err->message);
		g_error_free(err);
		break;
	}
	case GST_MESSAGE_WARNING: {
		GError *err;
		gst_message_parse_warning(message, &err, NULL);
		const char *source_name = obs_source_get_name(data->source);
		blog(LOG_WARNING, "[obs-gstreamer] %s: %s", source_name, err->message);
		g_error_free(err);
	} break;
	default:
		break;
	}

	return TRUE;
}

const char *gstreamer_filter_get_name_video(void *type_data)
{
	return "GStreamer Filter (Video)";
}

const char *gstreamer_filter_get_name_audio(void *type_data)
{
	return "GStreamer Filter (Audio)";
}

void *gstreamer_filter_create(obs_data_t *settings, obs_source_t *source)
{
	data_t *data = g_new0(data_t, 1);

	data->source = source;
	data->settings = settings;

	g_mutex_init(&data->mutex);

	return data;
}

// Tears down the pipeline. Callers must hold data->mutex.
static void filter_close_pipeline_locked(data_t *data)
{
	if (data->pipe == NULL)
		return;

	// Detach the bus watch first so no pending callback can run against a
	// pipeline that is about to be freed.
	GstBus *bus = gst_element_get_bus(data->pipe);
	if (bus != NULL) {
		gst_bus_remove_watch(bus);
		gst_object_unref(bus);
	}

	gst_element_set_state(data->pipe, GST_STATE_NULL);

	if (data->appsink != NULL) {
		gst_object_unref(data->appsink);
		data->appsink = NULL;
	}
	if (data->appsrc != NULL) {
		gst_object_unref(data->appsrc);
		data->appsrc = NULL;
	}

	gst_object_unref(data->pipe);
	data->pipe = NULL;
}

static void filter_close_pipeline(data_t *data)
{
	g_mutex_lock(&data->mutex);
	filter_close_pipeline_locked(data);
	g_mutex_unlock(&data->mutex);
}

void gstreamer_filter_destroy(void *p)
{
	data_t *data = (data_t *)p;

	filter_close_pipeline(data);

	g_mutex_clear(&data->mutex);

	g_free(data);
}

void gstreamer_filter_get_defaults_video(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "pipeline", "videoflip video-direction=horiz");
}

void gstreamer_filter_get_defaults_audio(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "pipeline", "audioecho delay=200000000 intensity=0.3");
}

void gstreamer_filter_update(void *data, obs_data_t *settings);

static bool on_apply_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	gstreamer_filter_update(data, ((data_t *)data)->settings);

	return false;
}

obs_properties_t *gstreamer_filter_get_properties(void *data)
{
	data_t *d = (data_t *)data;

	obs_properties_t *props = obs_properties_create();

	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	// Runtime status line; see gstreamer_source_get_properties().
	const char *status = d->last_error[0] != '\0' ? d->last_error : "Filter ready";
	enum obs_text_info_type status_type =
		d->last_error[0] != '\0' ? OBS_TEXT_INFO_ERROR : OBS_TEXT_INFO_NORMAL;
	obs_data_set_string(d->settings, "_last_status", status);
	obs_property_t *prop = obs_properties_add_text(props, "_last_status", NULL, OBS_TEXT_INFO);
	obs_property_text_set_info_type(prop, status_type);

	prop = obs_properties_add_text(props, "pipeline", "Pipeline", OBS_TEXT_MULTILINE);
	obs_property_set_long_description(
		prop,
		"Use \"identity\" for passthru. Note: changing resolution or sample rate inside the filter is not supported.");
	obs_properties_add_button2(props, "apply", "Apply", on_apply_clicked, data);

	UNUSED_PARAMETER(data);
	return props;
}

void gstreamer_filter_update(void *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);

	data_t *data = (data_t *)p;

	filter_close_pipeline(data);
}

struct obs_source_frame *gstreamer_filter_filter_video(void *p, struct obs_source_frame *frame)
{
	GstMapInfo info;
	data_t *data = (data_t *)p;

	g_mutex_lock(&data->mutex);

	// Rebuild when the pixel format or dimensions changed since the
	// pipeline was created; otherwise the cached frame_size would no
	// longer match the incoming frames.
	if (data->pipe != NULL && (frame->width != data->last_width || frame->height != data->last_height ||
				   frame->format != data->last_format))
		filter_close_pipeline_locked(data);

	if (data->pipe == NULL) {
		GError *err = NULL;
		gchar *format = "";

		switch (frame->format) {
		case VIDEO_FORMAT_I420:
			data->frame_size = frame->width * frame->height * 3 / 2;
			format = "I420";
			break;
		case VIDEO_FORMAT_NV12:
			data->frame_size = frame->width * frame->height * 3 / 2;
			format = "NV12";
			break;
		case VIDEO_FORMAT_I422:
			data->frame_size = frame->width * frame->height * 2;
			format = "Y42B";
			break;

		case VIDEO_FORMAT_YVYU:
			data->frame_size = frame->width * frame->height * 2;
			format = "YVYU";
			break;
		case VIDEO_FORMAT_YUY2:
			data->frame_size = frame->width * frame->height * 2;
			format = "YUY2";
			break;
		case VIDEO_FORMAT_UYVY:
			data->frame_size = frame->width * frame->height * 2;
			format = "UYVY";
			break;

		case VIDEO_FORMAT_RGBA:
			data->frame_size = frame->width * frame->height * 4;
			format = "RGBA";
			break;
		case VIDEO_FORMAT_BGRA:
			data->frame_size = frame->width * frame->height * 4;
			format = "BGRA";
			break;
		case VIDEO_FORMAT_BGRX:
			data->frame_size = frame->width * frame->height * 4;
			format = "BGRx";
			break;
		default: {
			const char *source_name = obs_source_get_name(data->source);
			blog(LOG_ERROR, "[obs-gstreamer] %s: invalid video format: %d", source_name, frame->format);
			g_mutex_unlock(&data->mutex);
			return frame;
		}
		}

		gchar *str = g_strdup_printf(
			"appsrc name=appsrc format=time ! video/x-raw, width=%d, height=%d, format=%s, framerate=0/1 ! videoconvert ! "
			"%s ! videoconvert ! video/x-raw, width=%d, height=%d, format=%s, framerate=0/1 ! appsink name=appsink sync=false",
			frame->width, frame->height, format, obs_data_get_string(data->settings, "pipeline"),
			frame->width, frame->height, format);
		data->pipe = gst_parse_launch(str, &err);
		g_free(str);
		if (err != NULL || data->pipe == NULL) {
			const char *source_name = obs_source_get_name(data->source);
			blog(LOG_ERROR, "[obs-gstreamer] %s: %s", source_name,
			     err != NULL ? err->message : "cannot create pipeline");
			snprintf(data->last_error, sizeof(data->last_error), "Cannot create video pipeline: %s",
				 err != NULL ? err->message : "unknown parse error");
			if (err != NULL)
				g_error_free(err);

			if (data->pipe != NULL) {
				gst_object_unref(data->pipe);
				data->pipe = NULL;
			}

			g_mutex_unlock(&data->mutex);
			return frame;
		}

		data->appsrc = gst_bin_get_by_name(GST_BIN(data->pipe), "appsrc");
		data->appsink = gst_bin_get_by_name(GST_BIN(data->pipe), "appsink");

		if (data->appsrc == NULL || data->appsink == NULL) {
			const char *source_name = obs_source_get_name(data->source);
			blog(LOG_ERROR, "[obs-gstreamer] %s: pipeline misses appsrc/appsink", source_name);
			filter_close_pipeline_locked(data);

			g_mutex_unlock(&data->mutex);
			return frame;
		}

		GstBus *bus = gst_element_get_bus(data->pipe);
		gst_bus_add_watch(bus, bus_callback, data);
		gst_object_unref(bus);

		gst_element_set_state(data->pipe, GST_STATE_PLAYING);

		data->last_width = frame->width;
		data->last_height = frame->height;
		data->last_format = frame->format;

		// Pipeline is up; clear any previously shown error.
		data->last_error[0] = '\0';
	}

	GstBuffer *buffer =
		gst_buffer_new_wrapped_full(0, frame->data[0], data->frame_size, 0, data->frame_size, NULL, NULL);

	GST_BUFFER_PTS(buffer) = frame->timestamp;

	gst_app_src_push_buffer(GST_APP_SRC(data->appsrc), buffer);

	// Bounded wait: never block OBS' graphics thread indefinitely if the
	// user pipeline stalls.
	GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(data->appsink), FILTER_VIDEO_PULL_TIMEOUT);
	if (sample == NULL) {
		g_mutex_unlock(&data->mutex);
		return frame;
	}
	buffer = gst_sample_get_buffer(sample);

	gst_buffer_map(buffer, &info, GST_MAP_READ);

	if (info.size == (gsize)data->frame_size)
		memcpy(frame->data[0], info.data, data->frame_size);

	gst_buffer_unmap(buffer, &info);
	gst_sample_unref(sample);

	g_mutex_unlock(&data->mutex);

	return frame;
}

struct obs_audio_data *gstreamer_filter_filter_audio(void *p, struct obs_audio_data *audio_data)
{
	GstMapInfo info;
	data_t *data = (data_t *)p;

	g_mutex_lock(&data->mutex);

	if (data->pipe == NULL) {
		GError *err = NULL;
		struct obs_audio_info audio_info;

		if (!obs_get_audio_info(&audio_info)) {
			g_mutex_unlock(&data->mutex);
			return audio_data;
		}

		gst_audio_info_init(&data->audio_info);
		gst_audio_info_set_format(&data->audio_info, GST_AUDIO_FORMAT_F32LE, audio_info.samples_per_sec,
					  audio_info.speakers, NULL);
		data->audio_info.layout = GST_AUDIO_LAYOUT_NON_INTERLEAVED;

		gchar *str = g_strdup_printf(
			"appsrc name=appsrc format=time ! audio/x-raw, rate=%d, channels=%d, format=F32LE, layout=non-interleaved ! audioconvert ! "
			"%s ! audioconvert ! audio/x-raw, rate=%d, channels=%d, format=F32LE, layout=non-interleaved ! appsink name=appsink sync=false",
			data->audio_info.rate, data->audio_info.channels,
			obs_data_get_string(data->settings, "pipeline"), data->audio_info.rate,
			data->audio_info.channels);
		data->pipe = gst_parse_launch(str, &err);
		g_free(str);
		if (err != NULL || data->pipe == NULL) {
			const char *source_name = obs_source_get_name(data->source);
			blog(LOG_ERROR, "[obs-gstreamer] %s: %s", source_name,
			     err != NULL ? err->message : "cannot create pipeline");
			snprintf(data->last_error, sizeof(data->last_error), "Cannot create audio pipeline: %s",
				 err != NULL ? err->message : "unknown parse error");
			if (err != NULL)
				g_error_free(err);

			if (data->pipe != NULL) {
				gst_object_unref(data->pipe);
				data->pipe = NULL;
			}

			g_mutex_unlock(&data->mutex);
			return audio_data;
		}

		data->appsrc = gst_bin_get_by_name(GST_BIN(data->pipe), "appsrc");
		data->appsink = gst_bin_get_by_name(GST_BIN(data->pipe), "appsink");

		if (data->appsrc == NULL || data->appsink == NULL) {
			const char *source_name = obs_source_get_name(data->source);
			blog(LOG_ERROR, "[obs-gstreamer] %s: pipeline misses appsrc/appsink", source_name);
			filter_close_pipeline_locked(data);

			g_mutex_unlock(&data->mutex);
			return audio_data;
		}

		GstBus *bus = gst_element_get_bus(data->pipe);
		gst_bus_add_watch(bus, bus_callback, data);
		gst_object_unref(bus);

		gst_element_set_state(data->pipe, GST_STATE_PLAYING);

		// Pipeline is up; clear any previously shown error.
		data->last_error[0] = '\0';
	}

	gint channel_size = data->audio_info.bpf * audio_data->frames / data->audio_info.channels;

	GstBuffer *buffer = gst_buffer_new_allocate(NULL, channel_size * data->audio_info.channels, NULL);

	gst_buffer_map(buffer, &info, GST_MAP_WRITE);

	for (int i = 0; i < data->audio_info.channels; i++)
		memcpy(info.data + i * channel_size, audio_data->data[i], channel_size);

	gst_buffer_unmap(buffer, &info);

	gst_buffer_add_audio_meta(buffer, &data->audio_info, audio_data->frames, NULL);
	GST_BUFFER_PTS(buffer) = audio_data->timestamp;

	gst_app_src_push_buffer(GST_APP_SRC(data->appsrc), buffer);

	// Non-blocking: this runs on OBS' audio thread which must not stall.
	GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(data->appsink), 0);
	if (sample == NULL) {
		g_mutex_unlock(&data->mutex);
		return audio_data;
	}

	buffer = gst_sample_get_buffer(sample);

	gst_buffer_map(buffer, &info, GST_MAP_READ);

	if (info.size == (gsize)(channel_size * data->audio_info.channels))
		for (int i = 0; i < data->audio_info.channels; i++)
			memcpy(audio_data->data[i], info.data + i * channel_size, channel_size);

	gst_buffer_unmap(buffer, &info);
	gst_sample_unref(sample);

	g_mutex_unlock(&data->mutex);

	return audio_data;
}
