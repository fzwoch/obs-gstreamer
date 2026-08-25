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
#include <gst/net/gstnet.h>
#include <string.h>

#include "plugin-i18n.h"

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#define MIN3(a, b, c) MIN(MIN(a, b), c)

typedef struct {
	GstElement *pipe;
	GstClock *clock;
	obs_source_t *source;
	obs_data_t *settings;
	gint64 frame_count;
	gint64 audio_count;
	enum obs_media_state obs_media_state;
	gint64 seek_pos_pending;
	bool buffering;
	char last_error[256];
	gint64 resume_pos_pending;
	bool resume_requested;
	GSource *timeout;
	GThread *thread;
	GMainLoop *loop;
	GMutex mutex;
	GCond cond;
} data_t;

static void create_pipeline(data_t *data);

// Case-insensitive edit distance, capped to keep lookups cheap.
static size_t edit_distance(const char *a, const char *b)
{
	char ra[64] = {0}, rb[64] = {0};
	size_t la = 0, lb = 0;

	for (; a[la] != '\0' && la < 63; la++)
		ra[la] = (char)g_ascii_tolower(a[la]);
	for (; b[lb] != '\0' && lb < 63; lb++)
		rb[lb] = (char)g_ascii_tolower(b[lb]);

	size_t row[64 + 1] = {0};
	for (size_t j = 0; j <= lb; j++)
		row[j] = j;

	for (size_t i = 1; i <= la; i++) {
		size_t prev = row[0];
		row[0] = i;
		for (size_t j = 1; j <= lb; j++) {
			size_t cur = row[j];
			size_t subst = prev + (ra[i - 1] == rb[j - 1] ? 0 : 1);
			row[j] = MIN3(row[j] + 1, row[j - 1] + 1, subst);
			prev = cur;
		}
	}

	return lb > 0 ? row[lb] : la;
}

// Fills `out` with a "Did you mean ...?" hint listing up to three installed
// elements close in spelling to `name`. Empty string when nothing is close.
static void suggest_element_names(const char *name, char *out, size_t size)
{
	out[0] = '\0';
	if (name == NULL || name[0] == '\0')
		return;

	GList *features = gst_registry_get_feature_list(gst_registry_get(), GST_TYPE_ELEMENT_FACTORY);

	const char *best[3] = {NULL, NULL, NULL};
	size_t best_dist[3] = {(size_t)-1, (size_t)-1, (size_t)-1};

	for (GList *l = features; l != NULL; l = l->next) {
		const char *candidate = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(l->data));
		size_t d = edit_distance(name, candidate);

		for (int i = 0; i < 3; i++) {
			if (d < best_dist[i]) {
				for (int j = 2; j > i; j--) {
					best[j] = best[j - 1];
					best_dist[j] = best_dist[j - 1];
				}
				best[i] = candidate;
				best_dist[i] = d;
				break;
			}
		}
	}

	gst_plugin_feature_list_free(features);

	if (best[0] == NULL || best_dist[0] > 3)
		return;

	g_strlcat(out, " ", size);
	g_strlcat(out, T("suggest.didyoumean"), size);
	for (int i = 0; i < 3; i++) {
		if (best[i] == NULL || best_dist[i] > 3)
			break;
		if (i > 0) {
			g_strlcat(out, " ", size);
			g_strlcat(out, T("suggest.or"), size);
		}
		g_strlcat(out, "'", size);
		g_strlcat(out, best[i], size);
		g_strlcat(out, "'", size);
	}
	g_strlcat(out, "?", size);
}

// Extracts the offending element name from a gst_parse_launch error message
// like: no element "videotsrc"
static void find_missing_element(const char *message, char *out, size_t size)
{
	out[0] = '\0';
	if (message == NULL)
		return;

	const char *q1 = strchr(message, '"');
	if (q1 == NULL)
		return;

	const char *q2 = strchr(q1 + 1, '"');
	if (q2 == NULL || (size_t)(q2 - q1 - 1) >= size)
		return;

	memcpy(out, q1 + 1, q2 - q1 - 1);
	out[q2 - q1 - 1] = '\0';
}

// Records a parse/start failure in the source status line, including an
// element-name suggestion when applicable.
static void format_parse_error(data_t *data, const char *context, const GError *err)
{
	char missing[64] = {0};
	char hint[192] = {0};
	char msg[256];

	find_missing_element(err != NULL ? err->message : NULL, missing, sizeof(missing));
	suggest_element_names(missing, hint, sizeof(hint));

	snprintf(msg, sizeof(msg), "%s %s%s", context, err != NULL ? err->message : "unknown parse error", hint);
	blog(LOG_ERROR, "[obs-gstreamer] %s: %s", obs_source_get_name(data->source), msg);

	// Stored for the properties dialog; guarded because the UI thread reads it.
	g_mutex_lock(&data->mutex);
	snprintf(data->last_error, sizeof(data->last_error), "%s", msg);
	g_mutex_unlock(&data->mutex);
}

static void timeout_destroy(gpointer user_data)
{
	data_t *data = user_data;

	g_source_destroy(data->timeout);
	g_source_unref(data->timeout);
	data->timeout = NULL;
}

static gboolean pipeline_destroy(gpointer user_data)
{
	data_t *data = user_data;

	g_mutex_lock(&data->mutex);

	if (!data->pipe) {
		g_mutex_unlock(&data->mutex);
		return G_SOURCE_REMOVE;
	}

	// reset OBS media flags
	data->obs_media_state = OBS_MEDIA_STATE_STOPPED;
	data->seek_pos_pending = -1;
	data->buffering = false;

	// stop the bus_callback
	GstBus *bus = gst_element_get_bus(data->pipe);
	gst_bus_remove_watch(bus);
	gst_object_unref(bus);

	// set state to GST_STATE_NULL here and _only_ here, just before
	// unreferencing data->pipe
	gst_element_set_state(data->pipe, GST_STATE_NULL);

	gst_object_unref(data->pipe);
	if (data->clock != NULL)
		gst_object_unref(data->clock);
	data->pipe = NULL;
	data->clock = NULL;

	g_mutex_unlock(&data->mutex);

	return G_SOURCE_REMOVE;
}

static gboolean pipeline_restart(gpointer user_data)
{
	data_t *data = user_data;

	if (data->pipe)
		pipeline_destroy(data);

	create_pipeline(data);

	if (data->pipe)
		gst_element_set_state(data->pipe, GST_STATE_PLAYING);

	return G_SOURCE_REMOVE;
}

static void update_obs_media_state(GstMessage *message, data_t *data)
{
	switch (GST_MESSAGE_TYPE(message)) {
	case GST_MESSAGE_BUFFERING: {
		gint percent;
		gst_message_parse_buffering(message, &percent);
		data->buffering = (percent < 100);
	} break;
	case GST_MESSAGE_STATE_CHANGED: {
		// Child elements post their own transitions; only trust the pipeline
		// itself, otherwise the reported state flaps during preroll.
		if (GST_MESSAGE_SRC(message) != GST_OBJECT(data->pipe))
			break;
		GstState newstate;
		gst_message_parse_state_changed(message, NULL, &newstate, NULL);
		switch (newstate) {
		default:
		case GST_STATE_NULL:
			blog(LOG_WARNING, "[obs-gstreamer] state is GST_STATE_NULL, unexpected.");
			data->obs_media_state = OBS_MEDIA_STATE_NONE;
			break;
		case GST_STATE_READY:
			data->obs_media_state = OBS_MEDIA_STATE_STOPPED;
			break;
		case GST_STATE_PAUSED:
			data->obs_media_state = OBS_MEDIA_STATE_PAUSED;
			break;
		case GST_STATE_PLAYING:
			data->obs_media_state = OBS_MEDIA_STATE_PLAYING;
			break;
		}
	} break;
	case GST_MESSAGE_ERROR: {
		data->obs_media_state = OBS_MEDIA_STATE_ERROR;
	} break;
	case GST_MESSAGE_EOS: {
		data->obs_media_state = OBS_MEDIA_STATE_ENDED;
	} break;
	default:
		break;
	}
}

static gboolean bus_callback(GstBus *bus, GstMessage *message, gpointer user_data)
{
	data_t *data = user_data;

	// Optional deep logging to help pipeline authors debug their graphs.
	if (obs_data_get_bool(data->settings, "verbose_bus_log")) {
		const char *source_name = obs_source_get_name(data->source);
		switch (GST_MESSAGE_TYPE(message)) {
		case GST_MESSAGE_ELEMENT: {
			const GstStructure *s = gst_message_get_structure(message);
			blog(LOG_INFO, "[obs-gstreamer] %s: message: %s", source_name,
			     s != NULL ? gst_structure_get_name(s) : "(unknown)");
		} break;
		case GST_MESSAGE_QOS:
			blog(LOG_INFO, "[obs-gstreamer] %s: QoS event (element is running behind)", source_name);
			break;
		case GST_MESSAGE_LATENCY:
			blog(LOG_INFO, "[obs-gstreamer] %s: latency renegotiation", source_name);
			break;
		case GST_MESSAGE_STATE_CHANGED: {
			if (GST_MESSAGE_SRC(message) != GST_OBJECT(data->pipe)) {
				GstState old_state, new_state;
				gst_message_parse_state_changed(message, &old_state, &new_state, NULL);
				blog(LOG_DEBUG, "[obs-gstreamer] %s: element %s state %s -> %s", source_name,
				     GST_OBJECT_NAME(GST_MESSAGE_SRC(message)),
				     gst_element_state_get_name(old_state),
				     gst_element_state_get_name(new_state));
			}
		} break;
		default:
			break;
		}
	}

	update_obs_media_state(message, data);

	switch (GST_MESSAGE_TYPE(message)) {
	case GST_MESSAGE_ERROR: {
		GError *err;
		gst_message_parse_error(message, &err, NULL);
		const char *source_name = obs_source_get_name(data->source);
		blog(LOG_ERROR, "[obs-gstreamer] %s: %s", source_name, err->message);
		g_error_free(err);
	} // fallthrough
	case GST_MESSAGE_EOS:
		if (obs_data_get_bool(data->settings, "clear_on_end"))
			obs_source_output_video(data->source, NULL);
		if (obs_data_get_bool(data->settings, GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR
							      ? "restart_on_error"
							      : "restart_on_eos") &&
		    data->timeout == NULL) {
			data->timeout = g_timeout_source_new(obs_data_get_int(data->settings, "restart_timeout"));
			g_source_set_callback(data->timeout, pipeline_restart, data, timeout_destroy);
			g_source_attach(data->timeout, g_main_context_get_thread_default());
		}
		break;
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

	frame.timestamp = obs_data_get_bool(data->settings, "use_timestamps_video") ? GST_BUFFER_PTS(buffer)
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

	video_format_get_parameters(cs, range, frame.color_matrix, frame.color_range_min, frame.color_range_max);

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
	case GST_VIDEO_FORMAT_I420_10LE:
		frame.format = VIDEO_FORMAT_I010;
		break;
	case GST_VIDEO_FORMAT_P010_10LE:
		frame.format = VIDEO_FORMAT_P010;
		break;
	case GST_VIDEO_FORMAT_I422_10LE:
		frame.format = VIDEO_FORMAT_I210;
		break;
	case GST_VIDEO_FORMAT_Y444_12LE:
		frame.format = VIDEO_FORMAT_I412;
		break;
	default:
		frame.format = VIDEO_FORMAT_NONE;
		const char *source_name = obs_source_get_name(data->source);
		blog(LOG_ERROR, "[obs-gstreamer] %s: Unknown video format: %s", source_name, video_info.finfo->name);
		break;
	}

	// Do not hand frames with an unmapped pixel format to libobs.
	if (frame.format != VIDEO_FORMAT_NONE)
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

	audio.timestamp = obs_data_get_bool(data->settings, "use_timestamps_audio")
				  ? GST_BUFFER_PTS(buffer)
				  : data->audio_count++ * GST_SECOND * (audio.frames / (double)audio_info.rate);

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
		const char *source_name = obs_source_get_name(data->source);
		blog(LOG_ERROR, "[obs-gstreamer] %s: Unsupported audio channel count: %d", source_name,
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
		const char *source_name = obs_source_get_name(data->source);
		blog(LOG_ERROR, "[obs-gstreamer] %s: Unknown audio format: %s", source_name, audio_info.finfo->name);
		break;
	}

	// Skip output for channel layouts / sample formats libobs cannot represent.
	if (audio.format != AUDIO_FORMAT_UNKNOWN && audio.speakers != SPEAKERS_UNKNOWN)
		obs_source_output_audio(data->source, &audio);

	gst_buffer_unmap(buffer, &info);
	gst_sample_unref(sample);

	return GST_FLOW_OK;
}

const char *gstreamer_source_get_name(void *type_data)
{
	return "GStreamer Source";
}

enum obs_media_state gstreamer_source_get_state(void *user_data)
{
	data_t *data = user_data;

	if (data->buffering && data->obs_media_state != OBS_MEDIA_STATE_ERROR)
		return OBS_MEDIA_STATE_BUFFERING;

	return data->obs_media_state;
}

int64_t gstreamer_source_get_time(void *user_data)
{
	data_t *data = user_data;
	int64_t position;

	if (!data->pipe)
		return 0;

	// data->pipe may be torn down on the pipeline thread while we are being
	// polled from an OBS thread.
	g_mutex_lock(&data->mutex);
	gboolean valid = data->pipe != NULL &&
			 gst_element_query_position(data->pipe, GST_FORMAT_TIME, &position) &&
			 GST_CLOCK_TIME_IS_VALID(position);
	g_mutex_unlock(&data->mutex);

	if (valid)
		return GST_TIME_AS_MSECONDS(position);

	return 0;
}

int64_t gstreamer_source_get_duration(void *user_data)
{
	data_t *data = user_data;
	int64_t duration;

	if (!data->pipe)
		return 0;

	// data->pipe may be torn down on the pipeline thread while we are being
	// polled from an OBS thread.
	g_mutex_lock(&data->mutex);
	gboolean valid = data->pipe != NULL &&
			 gst_element_query_duration(data->pipe, GST_FORMAT_TIME, &duration) &&
			 GST_CLOCK_TIME_IS_VALID(duration);
	g_mutex_unlock(&data->mutex);

	if (valid)
		return GST_TIME_AS_MSECONDS(duration);

	return 0;
}

static gboolean pipeline_pause(gpointer user_data)
{
	data_t *data = user_data;

	if (data->pipe)
		gst_element_set_state(data->pipe, GST_STATE_PAUSED);

	return G_SOURCE_REMOVE;
}

static gboolean pipeline_play(gpointer user_data)
{
	data_t *data = user_data;

	if (data->pipe)
		gst_element_set_state(data->pipe, GST_STATE_PLAYING);

	return G_SOURCE_REMOVE;
}

void gstreamer_source_play_pause(void *user_data, bool pause)
{
	data_t *data = user_data;

	if (!data->loop)
		return;

	g_main_context_invoke(g_main_loop_get_context(data->loop), pause ? pipeline_pause : pipeline_play, data);
}

void gstreamer_source_stop(void *user_data)
{
	data_t *data = user_data;

	if (!data->loop)
		return;

	g_main_context_invoke(g_main_loop_get_context(data->loop), pipeline_destroy, data);
}

void gstreamer_source_restart(void *user_data)
{
	data_t *data = user_data;

	if (!data->loop)
		return;

	g_main_context_invoke(g_main_loop_get_context(data->loop), pipeline_restart, data);
}

static gboolean pipeline_seek_to_pending(gpointer user_data)
{
	data_t *data = user_data;
	gint64 seek_pos_pending;
	gboolean seek_enabled;

	g_mutex_lock(&data->mutex);
	seek_pos_pending = data->seek_pos_pending;
	data->seek_pos_pending = -1;
	g_mutex_unlock(&data->mutex);

	if (!data->pipe)
		return G_SOURCE_REMOVE;

	if (seek_pos_pending < 0) {
		const char *source_name = obs_source_get_name(data->source);
		blog(LOG_WARNING, "[obs-gstreamer] %s: No seek_pos_pending", source_name);
		return G_SOURCE_REMOVE;
	}

	// determine whether seeking is possible on this pipeline
	GstQuery *query;
	gint64 start, end;
	query = gst_query_new_seeking(GST_FORMAT_TIME);
	if (!gst_element_query(data->pipe, query)) {
		const char *source_name = obs_source_get_name(data->source);
		blog(LOG_ERROR, "[obs-gstreamer] %s: Seeking query failed", source_name);
		gst_query_unref(query);
		return G_SOURCE_REMOVE;
	}
	gst_query_parse_seeking(query, NULL, &seek_enabled, &start, &end);
	gst_query_unref(query);

	if (!seek_enabled) {
		const char *source_name = obs_source_get_name(data->source);
		blog(LOG_WARNING, "[obs-gstreamer] %s: Seeking is disabled", source_name);
		return G_SOURCE_REMOVE;
	}

	// do the seek
	gst_element_seek_simple(data->pipe, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
				seek_pos_pending);

	return G_SOURCE_REMOVE;
}

void gstreamer_source_set_time(void *user_data, int64_t ms)
{
	data_t *data = user_data;

	if (!data->loop)
		return;

	g_mutex_lock(&data->mutex);
	data->seek_pos_pending = ms * GST_MSECOND;
	g_mutex_unlock(&data->mutex);

	g_main_context_invoke(g_main_loop_get_context(data->loop), pipeline_seek_to_pending, data);
}

static gboolean loop_startup(gpointer user_data)
{
	data_t *data = user_data;

	// Wake start() as soon as the loop is running. Pipeline creation below
	// may block (e.g. waiting for an NTP clock to sync) and must not freeze
	// the OBS thread that called start().
	g_mutex_lock(&data->mutex);
	g_cond_signal(&data->cond);
	g_mutex_unlock(&data->mutex);

	create_pipeline(data);

	if (data->pipe) {
		gst_element_set_state(data->pipe, GST_STATE_PLAYING);

		// Resume the previous position after a hide/show cycle, if one was
		// requested. Reuses the regular seek machinery once the pipeline
		// had a chance to settle.
		if (data->resume_requested) {
			data->resume_requested = false;
			data->seek_pos_pending = data->resume_pos_pending * GST_MSECOND;
			g_main_context_invoke(g_main_loop_get_context(data->loop),
					      pipeline_seek_to_pending, data);
		}
	}

	return G_SOURCE_REMOVE;
}

// Builds the full launch string: appsink plumbing plus the user pipeline.
static gchar *build_pipeline_string(data_t *data)
{
	// Note: GST_VIDEO_FORMAT_I420_10LE is an enum value, not a macro, so the
	// former #ifdef guard here was always false. Offer exactly the formats
	// that are mapped to libobs formats below.
	return g_strdup_printf(
		"videoconvert name=video ! video/x-raw, format={I420,NV12,BGRA,BGRx,RGBx,RGBA,YUY2,YVYU,UYVY,I420_10LE,P010_10LE,I422_10LE,Y444_12LE} ! appsink name=video_appsink "
		"audioconvert name=audio ! audioresample ! audio/x-raw, format={U8,S16LE,S32LE,F32LE}, channels={1,2,3,4,5,6,8}, layout=interleaved ! appsink name=audio_appsink "
		"%s",
		obs_data_get_string(data->settings, "pipeline"));
}

static void create_pipeline(data_t *data)
{
	GError *err = NULL;

	data->frame_count = 0;
	data->audio_count = 0;
	data->obs_media_state = OBS_MEDIA_STATE_OPENING;
	data->seek_pos_pending = -1;
	data->buffering = false;

	gchar *pipeline = build_pipeline_string(data);

	data->pipe = gst_parse_launch(pipeline, &err);
	g_free(pipeline);
	if (err != NULL) {
		format_parse_error(data, T("err.cannot_start"), err);
		g_error_free(err);

		// gst_parse_launch() can return a partially built pipeline even on
		// error; do not leak it.
		if (data->pipe != NULL) {
			gst_object_unref(data->pipe);
			data->pipe = NULL;
		}

		data->obs_media_state = OBS_MEDIA_STATE_ERROR;

		obs_source_output_video(data->source, NULL);

		return;
	}

	// Pipeline launched fine; clear any previously shown error.
	g_mutex_lock(&data->mutex);
	data->last_error[0] = '\0';
	g_mutex_unlock(&data->mutex);

	GstAppSinkCallbacks video_cbs = {NULL, NULL, video_new_sample};

	GstElement *appsink = gst_bin_get_by_name(GST_BIN(data->pipe), "video_appsink");
	gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &video_cbs, data, NULL);

	if (!obs_data_get_bool(data->settings, "sync_appsink_video"))
		g_object_set(appsink, "sync", FALSE, NULL);

	if (obs_data_get_bool(data->settings, "disable_async_appsink_video"))
		g_object_set(appsink, "async", FALSE, NULL);

	if (obs_data_get_bool(data->settings, "block_video"))
		g_object_set(appsink, "max-buffers", 1, NULL);

	if (obs_data_get_bool(data->settings, "drop_video"))
		gst_app_sink_set_drop(GST_APP_SINK(appsink), TRUE);

	// check if connected and remove if not
	GstElement *sink = gst_bin_get_by_name(GST_BIN(data->pipe), "video");
	if (sink != NULL) {
		GstPad *pad = gst_element_get_static_pad(sink, "sink");
		if (pad != NULL) {
			if (!gst_pad_is_linked(pad))
				gst_bin_remove(GST_BIN(data->pipe), appsink);
			gst_object_unref(pad);
		} else {
			blog(LOG_WARNING, "[obs-gstreamer] element 'video' has no static sink pad");
		}
		gst_object_unref(sink);
	} else {
		blog(LOG_WARNING, "[obs-gstreamer] pipeline has no element named 'video'");
	}

	gst_object_unref(appsink);

	GstAppSinkCallbacks audio_cbs = {NULL, NULL, audio_new_sample};

	appsink = gst_bin_get_by_name(GST_BIN(data->pipe), "audio_appsink");
	gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &audio_cbs, data, NULL);

	if (!obs_data_get_bool(data->settings, "sync_appsink_audio"))
		g_object_set(appsink, "sync", FALSE, NULL);

	if (obs_data_get_bool(data->settings, "disable_async_appsink_audio"))
		g_object_set(appsink, "async", FALSE, NULL);

	if (obs_data_get_bool(data->settings, "block_audio"))
		g_object_set(appsink, "max-buffers", 1, NULL);

	if (obs_data_get_bool(data->settings, "drop_audio"))
		gst_app_sink_set_drop(GST_APP_SINK(appsink), TRUE);

	// check if connected and remove if not
	sink = gst_bin_get_by_name(GST_BIN(data->pipe), "audio");
	if (sink != NULL) {
		GstPad *pad = gst_element_get_static_pad(sink, "sink");
		if (pad != NULL) {
			if (!gst_pad_is_linked(pad))
				gst_bin_remove(GST_BIN(data->pipe), appsink);
			gst_object_unref(pad);
		} else {
			blog(LOG_WARNING, "[obs-gstreamer] element 'audio' has no static sink pad");
		}
		gst_object_unref(sink);
	} else {
		blog(LOG_WARNING, "[obs-gstreamer] pipeline has no element named 'audio'");
	}

	gst_object_unref(appsink);

	GstBus *bus = gst_element_get_bus(data->pipe);
	gst_bus_add_watch(bus, bus_callback, data);
	gst_object_unref(bus);

	// set clock
	const char *server = obs_data_get_string(data->settings, "ntp_server");
	if (strlen(server) > 0) {
		gint clock_port = obs_data_get_int(data->settings, "ntp_port");
		GstClock *clock = gst_ntp_clock_new("net_clock", server, clock_port, 0);
		blog(LOG_INFO, "Connect to NTP server %s", server);
		if (clock == NULL) {
			blog(LOG_ERROR, "Failed to connect to net clock %s, continuing without it", server);
		} else if (!gst_clock_wait_for_sync(clock, 5 * GST_SECOND)) {
			blog(LOG_ERROR, "Failed to sync to net clock %s, timeout, continuing without it", server);
			gst_object_unref(clock);
		} else {
			data->clock = clock;
			gst_pipeline_use_clock(GST_PIPELINE(data->pipe), GST_CLOCK(data->clock));
		}
	}
	gint latency = obs_data_get_int(data->settings, "latency");
	// set latency
	if (latency) {
		gst_pipeline_set_latency(GST_PIPELINE(data->pipe), latency * GST_MSECOND);
		gint cur_latency = gst_pipeline_get_latency(GST_PIPELINE(data->pipe)) / GST_MSECOND;
		blog(LOG_INFO, "Set latency for pipeline to %dms", cur_latency);
	}
}

static gpointer _start(gpointer user_data)
{
	data_t *data = user_data;

	GMainContext *context = g_main_context_new();

	g_main_context_push_thread_default(context);

	data->loop = g_main_loop_new(context, FALSE);

	GSource *source = g_idle_source_new();
	g_source_set_callback(source, loop_startup, data, NULL);
	g_source_attach(source, context);

	g_main_loop_run(data->loop);

	if (data->pipe)
		pipeline_destroy(data);

	g_main_loop_unref(data->loop);
	data->loop = NULL;

	g_main_context_unref(context);

	return NULL;
}

static void start(data_t *data)
{
	g_mutex_lock(&data->mutex);

	// Guard against a second start while the worker thread is still around
	// (e.g. show() racing a restart that briefly has no pipeline).
	if (data->thread != NULL) {
		g_mutex_unlock(&data->mutex);
		return;
	}

	data->thread = g_thread_new("GStreamer Source", _start, data);

	g_cond_wait(&data->cond, &data->mutex);
	g_mutex_unlock(&data->mutex);
}

void *gstreamer_source_create(obs_data_t *settings, obs_source_t *source)
{
	bool nobuf = obs_data_get_bool(settings, "no_buffer");
	obs_source_set_async_unbuffered(source, nobuf);

	data_t *data = g_new0(data_t, 1);

	data->source = source;
	data->settings = settings;

	g_mutex_init(&data->mutex);
	g_cond_init(&data->cond);

	if (obs_data_get_bool(settings, "stop_on_hide") == false)
		start(data);

	return data;
}

static void stop(data_t *data)
{
	if (data->thread == NULL)
		return;

	g_main_loop_quit(data->loop);

	g_thread_join(data->thread);
	data->thread = NULL;

	obs_source_output_video(data->source, NULL);
}

void gstreamer_source_destroy(void *user_data)
{
	data_t *data = user_data;

	stop(data);

	g_mutex_clear(&data->mutex);
	g_cond_clear(&data->cond);

	g_free(data);
}

void gstreamer_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(
		settings, "pipeline",
		"videotestsrc is-live=true ! video/x-raw, framerate=30/1, width=960, height=540 ! video. "
		"audiotestsrc wave=ticks is-live=true ! audio/x-raw, channels=2, rate=44100 ! audio.");
	obs_data_set_default_bool(settings, "use_timestamps_video", true);
	obs_data_set_default_bool(settings, "use_timestamps_audio", true);
	obs_data_set_default_bool(settings, "sync_appsink_video", true);
	obs_data_set_default_bool(settings, "sync_appsink_audio", true);
	obs_data_set_default_bool(settings, "disable_async_appsink_video", false);
	obs_data_set_default_bool(settings, "disable_async_appsink_audio", false);
	obs_data_set_default_bool(settings, "restart_on_eos", true);
	obs_data_set_default_bool(settings, "restart_on_error", false);
	obs_data_set_default_int(settings, "restart_timeout", 2000);
	obs_data_set_default_bool(settings, "no_buffer", false);
	obs_data_set_default_int(settings, "latency", 0);
	obs_data_set_default_string(settings, "ntp_server", "");
	obs_data_set_default_int(settings, "ntp_port", 123);
	obs_data_set_default_bool(settings, "stop_on_hide", true);
	obs_data_set_default_bool(settings, "block_video", false);
	obs_data_set_default_bool(settings, "block_audio", false);
	obs_data_set_default_bool(settings, "drop_video", false);
	obs_data_set_default_bool(settings, "drop_audio", false);
	obs_data_set_default_bool(settings, "clear_on_end", true);
	obs_data_set_default_bool(settings, "resume_position", false);
	obs_data_set_default_bool(settings, "verbose_bus_log", false);
}

void gstreamer_source_update(void *data, obs_data_t *settings);

static bool on_apply_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	gstreamer_source_update(data, ((data_t *)data)->settings);

	return false;
}

static const struct {
	const char *label_key;
	const char *pipeline;
} source_presets[] = {
	{"preset.test",
	 "videotestsrc is-live=true ! video/x-raw, framerate=30/1, width=960, height=540 ! video. "
	 "audiotestsrc wave=ticks is-live=true ! audio/x-raw, channels=2, rate=44100 ! audio."},
	{"preset.rtsp",
	 "rtspsrc location=rtsp://192.168.1.100:554/stream latency=200 ! decodebin ! queue ! videoconvert ! video."},
	{"preset.srt",
	 "srtsrc uri=srt://:7001 mode=listener latency=200 ! decodebin name=dec dec. ! queue ! videoconvert ! video. "
	 "dec. ! queue ! audioconvert ! audio."},
	{"preset.webcam",
	 "v4l2src device=/dev/video0 ! image/jpeg ! jpegdec ! videoconvert ! video."},
	{"preset.x11",
	 "ximagesrc use-damage=0 show-pointer=true ! video/x-raw, framerate=30/1 ! videoconvert ! video."},
};

static bool preset_selected(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	const char *preset = obs_data_get_string(settings, "preset");
	if (preset[0] == '\0')
		return false;

	// Insert the template into the pipeline field and reset the combo so
	// selecting the same entry again re-applies it.
	obs_data_set_string(settings, "pipeline", preset);
	obs_data_set_string(settings, "preset", "");

	// Rebuild the properties so the new pipeline text shows up.
	return true;
}

obs_properties_t *gstreamer_source_get_properties(void *data)
{
	data_t *d = (data_t *)data;

	obs_properties_t *props = obs_properties_create();

	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	// Runtime status line. libobs has no per-source error API (only outputs
	// and encoders have obs_*_set_last_error()), so errors are surfaced through
	// an info text property. The status is passed as the property description;
	// nothing is written to the saved settings.
	char last_error[sizeof(d->last_error)];
	g_mutex_lock(&d->mutex);
	memcpy(last_error, d->last_error, sizeof(last_error));
	g_mutex_unlock(&d->mutex);

	const char *status = T("status.stopped");
	enum obs_text_info_type status_type = OBS_TEXT_INFO_NORMAL;
	if (last_error[0] != '\0') {
		status = last_error;
		status_type = OBS_TEXT_INFO_ERROR;
	} else if (d->thread != NULL) {
		switch (d->obs_media_state) {
		case OBS_MEDIA_STATE_OPENING:
			status = T("status.opening");
			status_type = OBS_TEXT_INFO_WARNING;
			break;
		case OBS_MEDIA_STATE_BUFFERING:
			status = T("status.buffering");
			status_type = OBS_TEXT_INFO_WARNING;
			break;
		case OBS_MEDIA_STATE_PLAYING:
			status = T("status.running");
			break;
		case OBS_MEDIA_STATE_PAUSED:
			status = T("status.paused");
			break;
		case OBS_MEDIA_STATE_ENDED:
			status = T("status.ended");
			break;
		default:
			break;
		}
	}
	// Purge the key an earlier version persisted into saved settings.
	obs_data_erase(d->settings, "_last_status");

	obs_property_t *prop = obs_properties_add_text(props, "_status_line", status, OBS_TEXT_INFO);
	obs_property_text_set_info_type(prop, status_type);

	// --- Pipeline ---
	obs_properties_t *pgroup = obs_properties_create();

	prop = obs_properties_add_list(pgroup, "preset", T("template.label"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(prop, T("template.none"), "");
	for (size_t i = 0; i < sizeof(source_presets) / sizeof(source_presets[0]); i++)
		obs_property_list_add_string(prop, T(source_presets[i].label_key), source_presets[i].pipeline);
	obs_property_set_modified_callback(prop, preset_selected);
	obs_property_set_long_description(prop, T("template.desc"));

	prop = obs_properties_add_text(pgroup, "pipeline", T("pipeline.label"), OBS_TEXT_MULTILINE);
	obs_property_set_long_description(prop, T("pipeline.desc"));

	obs_properties_add_group(props, "pipeline_group", T("group.pipeline"), OBS_GROUP_NORMAL, pgroup);

	// --- Video ---
	obs_properties_t *vgroup = obs_properties_create();

	obs_properties_add_bool(vgroup, "use_timestamps_video", T("timestamps.video"));
	obs_properties_add_bool(vgroup, "sync_appsink_video", T("sync.video"));
	obs_properties_add_bool(vgroup, "disable_async_appsink_video", T("noasync.video"));
	prop = obs_properties_add_bool(vgroup, "block_video", T("block.video"));
	obs_property_set_long_description(prop, T("block.video.desc"));
	prop = obs_properties_add_bool(vgroup, "drop_video", T("drop.video"));
	obs_property_set_long_description(prop, T("drop.generic.desc"));

	obs_properties_add_group(props, "video_group", T("group.video"), OBS_GROUP_NORMAL, vgroup);

	// --- Audio ---
	obs_properties_t *agroup = obs_properties_create();

	obs_properties_add_bool(agroup, "use_timestamps_audio", T("timestamps.audio"));
	obs_properties_add_bool(agroup, "sync_appsink_audio", T("sync.audio"));
	obs_properties_add_bool(agroup, "disable_async_appsink_audio", T("noasync.audio"));
	prop = obs_properties_add_bool(agroup, "block_audio", T("block.audio"));
	obs_property_set_long_description(prop, T("block.audio.desc"));
	prop = obs_properties_add_bool(agroup, "drop_audio", T("drop.audio"));
	obs_property_set_long_description(prop, T("drop.generic.desc"));

	obs_properties_add_group(props, "audio_group", T("group.audio"), OBS_GROUP_NORMAL, agroup);

	// --- Behavior ---
	obs_properties_t *bgroup = obs_properties_create();

	obs_properties_add_bool(bgroup, "restart_on_eos", T("restart.eos"));
	obs_properties_add_bool(bgroup, "restart_on_error", T("restart.error"));
	obs_properties_add_int(bgroup, "restart_timeout", T("restart.timeout"), 0, 10000, 100);
	obs_properties_add_bool(bgroup, "stop_on_hide", T("stop.on_hide"));
	prop = obs_properties_add_bool(bgroup, "resume_position", T("resume.position"));
	obs_property_set_long_description(prop, T("resume.position.desc"));
	obs_properties_add_bool(bgroup, "clear_on_end", T("clear.on_end"));
	obs_properties_add_bool(bgroup, "no_buffer", T("no_buffer"));
	prop = obs_properties_add_int(bgroup, "latency", T("latency.label"), 0, 10000, 10);
	obs_property_set_long_description(prop, T("latency.desc"));

	obs_properties_add_group(props, "behavior_group", T("group.behavior"), OBS_GROUP_NORMAL, bgroup);

	// --- Network clock ---
	obs_properties_t *ngroup = obs_properties_create();

	prop = obs_properties_add_text(ngroup, "ntp_server", T("ntp.server"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(prop, T("ntp.server.desc"));
	obs_properties_add_int(ngroup, "ntp_port", T("ntp.port"), 1, 65536, 1);

	obs_properties_add_group(props, "network_group", T("group.network"), OBS_GROUP_NORMAL, ngroup);

	prop = obs_properties_add_bool(props, "verbose_bus_log", T("verbose.log"));
	obs_property_set_long_description(prop, T("verbose.log.desc"));

	obs_properties_add_button2(props, "apply", T("apply"), on_apply_clicked, data);

	return props;
}

void gstreamer_source_update(void *data, obs_data_t *settings)
{
	// Validate the pipeline before tearing down a possibly working one. A
	// typo then shows up as an error in the properties dialog instead of
	// killing the running source.
	GError *err = NULL;
	gchar *pipe_string = build_pipeline_string((data_t *)data);
	GstElement *test_pipe = gst_parse_launch(pipe_string, &err);
	g_free(pipe_string);

	if (err != NULL || test_pipe == NULL) {
		format_parse_error((data_t *)data, T("err.invalid_pipeline"), err);
		if (err != NULL)
			g_error_free(err);
		if (test_pipe != NULL)
			gst_object_unref(test_pipe);
		return;
	}
	gst_object_unref(test_pipe);

	// An explicit apply is an intentional fresh start; drop any pending
	// resume from earlier hide/show cycles.
	((data_t *)data)->resume_requested = false;

	stop(data);

	bool nobuf = obs_data_get_bool(settings, "no_buffer");
	obs_source_set_async_unbuffered(((data_t *)data)->source, nobuf);

	// Don't start the pipeline if source is hidden and 'stop_on_hide' is set.
	// From GUI this is probably irrelevant but works around some quirks when
	// controlled from script.
	if (obs_data_get_bool(settings, "stop_on_hide") && !obs_source_showing(((data_t *)data)->source))
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
	data_t *d = ((data_t *)data);

	if (!obs_data_get_bool(d->settings, "stop_on_hide"))
		return;

	// Remember where playback stood so show() can resume, if requested and
	// the stream is seekable (has a finite duration).
	if (obs_data_get_bool(d->settings, "resume_position") && d->pipe != NULL) {
		int64_t pos = gstreamer_source_get_time(d);
		int64_t dur = gstreamer_source_get_duration(d);
		if (pos > 0 && dur > 0 && pos < dur) {
			d->resume_pos_pending = pos;
			d->resume_requested = true;
		}
	}

	stop(d);
}
