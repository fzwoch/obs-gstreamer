/*
 * obs-gstreamer. OBS Studio plugin.
 * Copyright (C) 2018-2020 Florian Zwoch <fzwoch@gmail.com>
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

typedef struct {
	GstElement *pipe;
	obs_source_t *source;
	obs_data_t *settings;
	gint64 frame_count;
	gint64 audio_count;
	guint timeout_id;
} data_t;

static void start(data_t *data);
static void stop(data_t *data);

static gboolean start_pipe(gpointer user_data)
{
	data_t *data = user_data;

	data->timeout_id = 0;

	stop(data);
	start(data);

	return FALSE;
}

static gboolean bus_callback(GstBus *bus, GstMessage *message,
			     gpointer user_data)
{
	data_t *data = user_data;

	switch (GST_MESSAGE_TYPE(message)) {
	case GST_MESSAGE_EOS:
		if (obs_data_get_bool(data->settings, "restart_on_eos"))
			gst_element_seek_simple(data->pipe, GST_FORMAT_TIME,
						GST_SEEK_FLAG_FLUSH, 0);
		else if (obs_data_get_bool(data->settings, "clear_on_end"))
			obs_source_output_video(data->source, NULL);
		break;
	case GST_MESSAGE_ERROR: {
		GError *err;
		gst_message_parse_error(message, &err, NULL);
		blog(LOG_ERROR, "%s", err->message);
		g_error_free(err);
	}
		gst_element_set_state(data->pipe, GST_STATE_NULL);
		if (obs_data_get_bool(data->settings, "clear_on_end"))
			obs_source_output_video(data->source, NULL);
		if (obs_data_get_bool(data->settings, "restart_on_error") &&
		    data->timeout_id == 0)
			data->timeout_id = g_timeout_add(
				obs_data_get_int(data->settings,
						 "restart_timeout"),
				start_pipe, data);
		break;
	case GST_MESSAGE_WARNING: {
		GError *err;
		gst_message_parse_warning(message, &err, NULL);
		blog(LOG_WARNING, "%s", err->message);
		g_error_free(err);
	} break;
	default:
		break;
	}

	return TRUE;
}

static GstFlowReturn video_new_sample(GstAppSink *appsink, gpointer user_data)
{
	data_t *data = user_data;
	GstSample *sample = gst_app_sink_pull_sample(appsink);
	GstBuffer *buffer = gst_sample_get_buffer(sample);
	GstCaps *caps = gst_sample_get_caps(sample);
	GstMapInfo info;
	GstVideoInfo video_info;

	gst_video_info_from_caps(&video_info, caps);
	gst_buffer_map(buffer, &info, GST_MAP_READ);

	struct obs_source_frame frame = {};

	frame.timestamp =
		obs_data_get_bool(data->settings, "use_timestamps_video")
			? GST_BUFFER_PTS(buffer)
			: data->frame_count++;

	frame.width = video_info.width;
	frame.height = video_info.height;
	frame.linesize[0] = video_info.stride[0];
	frame.linesize[1] = video_info.stride[1];
	frame.linesize[2] = video_info.stride[2];
	frame.data[0] = info.data + video_info.offset[0];
	frame.data[1] = info.data + video_info.offset[1];
	frame.data[2] = info.data + video_info.offset[2];

	enum video_range_type range = VIDEO_RANGE_DEFAULT;
	switch (video_info.colorimetry.range) {
	case GST_VIDEO_COLOR_RANGE_0_255:
		range = VIDEO_RANGE_FULL;
		frame.full_range = 1;
		break;
	case GST_VIDEO_COLOR_RANGE_16_235:
		range = VIDEO_RANGE_PARTIAL;
		break;
	default:
		break;
	}

	enum video_colorspace cs = VIDEO_CS_DEFAULT;
	switch (video_info.colorimetry.matrix) {
	case GST_VIDEO_COLOR_MATRIX_BT709:
		cs = VIDEO_CS_709;
		break;
	case GST_VIDEO_COLOR_MATRIX_BT601:
		cs = VIDEO_CS_601;
		break;
	default:
		break;
	}

	video_format_get_parameters(cs, range, frame.color_matrix,
				    frame.color_range_min,
				    frame.color_range_max);

	switch (video_info.finfo->format) {
	case GST_VIDEO_FORMAT_I420:
		frame.format = VIDEO_FORMAT_I420;
		break;
	case GST_VIDEO_FORMAT_NV12:
		frame.format = VIDEO_FORMAT_NV12;
		break;
	case GST_VIDEO_FORMAT_BGRA:
		frame.format = VIDEO_FORMAT_BGRA;
		break;
	case GST_VIDEO_FORMAT_BGRx:
		frame.format = VIDEO_FORMAT_BGRX;
		break;
	case GST_VIDEO_FORMAT_RGBx:
	case GST_VIDEO_FORMAT_RGBA:
		frame.format = VIDEO_FORMAT_RGBA;
		break;
	case GST_VIDEO_FORMAT_UYVY:
		frame.format = VIDEO_FORMAT_UYVY;
		break;
	case GST_VIDEO_FORMAT_YUY2:
		frame.format = VIDEO_FORMAT_YUY2;
		break;
	case GST_VIDEO_FORMAT_YVYU:
		frame.format = VIDEO_FORMAT_YVYU;
		break;
	default:
		frame.format = VIDEO_FORMAT_NONE;
		blog(LOG_ERROR, "Unknown video format: %s",
		     video_info.finfo->name);
		break;
	}

	obs_source_output_video(data->source, &frame);

	gst_buffer_unmap(buffer, &info);
	gst_sample_unref(sample);

	return GST_FLOW_OK;
}

static GstFlowReturn audio_new_sample(GstAppSink *appsink, gpointer user_data)
{
	data_t *data = user_data;
	GstSample *sample = gst_app_sink_pull_sample(appsink);
	GstBuffer *buffer = gst_sample_get_buffer(sample);
	GstCaps *caps = gst_sample_get_caps(sample);
	GstMapInfo info;
	GstAudioInfo audio_info;

	gst_audio_info_from_caps(&audio_info, caps);
	gst_buffer_map(buffer, &info, GST_MAP_READ);

	struct obs_source_audio audio = {};

	audio.frames = info.size / audio_info.bpf;
	audio.samples_per_sec = audio_info.rate;
	audio.data[0] = info.data;

	audio.timestamp =
		obs_data_get_bool(data->settings, "use_timestamps_audio")
			? GST_BUFFER_PTS(buffer)
			: data->audio_count++ * GST_SECOND *
				  (audio.frames / (double)audio_info.rate);

	switch (audio_info.channels) {
	case 1:
		audio.speakers = SPEAKERS_MONO;
		break;
	case 2:
		audio.speakers = SPEAKERS_STEREO;
		break;
	case 3:
		audio.speakers = SPEAKERS_2POINT1;
		break;
	case 4:
		audio.speakers = SPEAKERS_4POINT0;
		break;
	case 5:
		audio.speakers = SPEAKERS_4POINT1;
		break;
	case 6:
		audio.speakers = SPEAKERS_5POINT1;
		break;
	case 8:
		audio.speakers = SPEAKERS_7POINT1;
		break;
	default:
		audio.speakers = SPEAKERS_UNKNOWN;
		blog(LOG_ERROR, "Unsupported channel count: %d",
		     audio_info.channels);
		break;
	}

	switch (audio_info.finfo->format) {
	case GST_AUDIO_FORMAT_U8:
		audio.format = AUDIO_FORMAT_U8BIT;
		break;
	case GST_AUDIO_FORMAT_S16LE:
		audio.format = AUDIO_FORMAT_16BIT;
		break;
	case GST_AUDIO_FORMAT_S32LE:
		audio.format = AUDIO_FORMAT_32BIT;
		break;
	case GST_AUDIO_FORMAT_F32LE:
		audio.format = AUDIO_FORMAT_FLOAT;
		break;
	default:
		audio.format = AUDIO_FORMAT_UNKNOWN;
		blog(LOG_ERROR, "Unknown audio format: %s",
		     audio_info.finfo->name);
		break;
	}

	obs_source_output_audio(data->source, &audio);

	gst_buffer_unmap(buffer, &info);
	gst_sample_unref(sample);

	return GST_FLOW_OK;
}

const char *gstreamer_source_get_name(void *type_data)
{
	return "GStreamer Source";
}

static void start(data_t *data)
{
	GError *err = NULL;

	gchar *pipeline = g_strdup_printf(
		"videoconvert name=video ! video/x-raw, format={I420,NV12,BGRA,BGRx,RGBx,RGBA,YUY2,YVYU,UYVY} ! appsink name=video_appsink "
		"audioconvert name=audio ! audioresample ! audio/x-raw, format={U8,S16LE,S32LE,F32LE}, channels={1,2,3,4,5,6,8} ! appsink name=audio_appsink "
		"%s",
		obs_data_get_string(data->settings, "pipeline"));

	data->pipe = gst_parse_launch(pipeline, &err);
	if (err != NULL) {
		blog(LOG_ERROR, "Cannot start GStreamer: %s", err->message);
		g_error_free(err);

		obs_source_output_video(data->source, NULL);

		g_free(pipeline);

		return;
	}

	g_free(pipeline);

	GstAppSinkCallbacks video_cbs = {NULL, NULL, video_new_sample};

	GstElement *appsink =
		gst_bin_get_by_name(GST_BIN(data->pipe), "video_appsink");
	gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &video_cbs, data,
				   NULL);

	if (!obs_data_get_bool(data->settings, "sync_appsink_video"))
		g_object_set(appsink, "sync", FALSE, NULL);

	// check if connected and remove if not
	GstElement *sink = gst_bin_get_by_name(GST_BIN(data->pipe), "video");
	GstPad *pad = gst_element_get_static_pad(sink, "sink");
	if (!gst_pad_is_linked(pad))
		gst_bin_remove(GST_BIN(data->pipe), appsink);
	gst_object_unref(pad);
	gst_object_unref(sink);

	gst_object_unref(appsink);

	GstAppSinkCallbacks audio_cbs = {NULL, NULL, audio_new_sample};

	appsink = gst_bin_get_by_name(GST_BIN(data->pipe), "audio_appsink");
	gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &audio_cbs, data,
				   NULL);

	if (!obs_data_get_bool(data->settings, "sync_appsink_audio"))
		g_object_set(appsink, "sync", FALSE, NULL);

	// check if connected and remove if not
	sink = gst_bin_get_by_name(GST_BIN(data->pipe), "audio");
	pad = gst_element_get_static_pad(sink, "sink");
	if (!gst_pad_is_linked(pad))
		gst_bin_remove(GST_BIN(data->pipe), appsink);
	gst_object_unref(pad);
	gst_object_unref(sink);

	gst_object_unref(appsink);

	data->frame_count = 0;
	data->audio_count = 0;

	GstBus *bus = gst_element_get_bus(data->pipe);
	gst_bus_add_watch(bus, bus_callback, data);
	gst_object_unref(bus);

	gst_element_set_state(data->pipe, GST_STATE_PLAYING);
}

void *gstreamer_source_create(obs_data_t *settings, obs_source_t *source)
{
	data_t *data = g_new0(data_t, 1);

	data->source = source;
	data->settings = settings;

	if (obs_data_get_bool(settings, "stop_on_hide") == false)
		start(data);

	return data;
}

static void stop(data_t *data)
{
	if (data->timeout_id != 0) {
		g_source_remove(data->timeout_id);
		data->timeout_id = 0;
	}

	if (data->pipe == NULL) {
		return;
	}

	gst_element_set_state(data->pipe, GST_STATE_NULL);
	gst_object_unref(data->pipe);
	data->pipe = NULL;

	obs_source_output_video(data->source, NULL);
}

void gstreamer_source_destroy(void *data)
{
	stop(data);
	g_free(data);
}

void gstreamer_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(
		settings, "pipeline",
		"videotestsrc is-live=true ! video/x-raw, framerate=30/1, width=960, height=540 ! video. "
		"audiotestsrc wave=ticks is-live=true ! audio/x-raw, channels=2, rate=44100 ! audio.");
	obs_data_set_default_bool(settings, "use_timestamps_video", false);
	obs_data_set_default_bool(settings, "use_timestamps_audio", false);
	obs_data_set_default_bool(settings, "sync_appsink_video", true);
	obs_data_set_default_bool(settings, "sync_appsink_audio", true);
	obs_data_set_default_bool(settings, "restart_on_eos", true);
	obs_data_set_default_bool(settings, "restart_on_error", false);
	obs_data_set_default_int(settings, "restart_timeout", 2000);
	obs_data_set_default_bool(settings, "stop_on_hide", true);
	obs_data_set_default_bool(settings, "clear_on_end", true);
}

void gstreamer_source_update(void *data, obs_data_t *settings);

static bool on_apply_clicked(obs_properties_t *props, obs_property_t *property,
			     void *data)
{
	gstreamer_source_update(data, ((data_t *)data)->settings);

	return false;
}

obs_properties_t *gstreamer_source_get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	obs_property_t *prop = obs_properties_add_text(
		props, "pipeline", "Pipeline", OBS_TEXT_MULTILINE);
	obs_property_set_long_description(
		prop,
		"Use \"video\" and \"audio\" as names for the media sinks.");
	obs_properties_add_bool(props, "use_timestamps_video",
				"Use pipeline time stamps (video)");
	obs_properties_add_bool(props, "use_timestamps_audio",
				"Use pipeline time stamps (audio)");
	obs_properties_add_bool(props, "sync_appsink_video",
				"Sync appsink to clock (video)");
	obs_properties_add_bool(props, "sync_appsink_audio",
				"Sync appsink to clock (audio)");
	obs_properties_add_bool(props, "restart_on_eos",
				"Try to restart when end of stream is reached");
	obs_properties_add_bool(
		props, "restart_on_error",
		"Try to restart after pipeline encountered an error");
	obs_properties_add_int(props, "restart_timeout", "Error timeout (ms)",
			       0, 10000, 100);
	obs_properties_add_bool(props, "stop_on_hide",
				"Stop pipeline when hidden");
	obs_properties_add_bool(
		props, "clear_on_end",
		"Clear image data after end-of-stream or error");
	obs_properties_add_button2(props, "apply", "Apply", on_apply_clicked,
				   data);

	return props;
}

void gstreamer_source_update(void *data, obs_data_t *settings)
{
	stop(data);

	// Don't start the pipeline if source is hidden and 'stop_on_hide' is set.
	// From GUI this is probably irrelevant but works around some quirks when
	// controlled from script.
	if (obs_data_get_bool(settings, "stop_on_hide") &&
	    !obs_source_showing(((data_t *)data)->source))
		return;

	start(data);
}

void gstreamer_source_show(void *data)
{
	if (((data_t *)data)->pipe == NULL)
		start(data);
}

void gstreamer_source_hide(void *data)
{
	if (obs_data_get_bool(((data_t *)data)->settings, "stop_on_hide"))
		stop(data);
}
