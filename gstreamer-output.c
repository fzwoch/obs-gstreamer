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

typedef struct {
	GstElement *pipe;
	GstElement *video;
	GstElement *audio;
	obs_output_t *output;
	obs_data_t *settings;
} data_t;

const char *gstreamer_output_get_name(void *type_data)
{
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
	g_free(data);
}

bool gstreamer_output_start(void *p)
{
	data_t *data = (data_t *)p;

	g_print("start\n");

	struct obs_video_info ovi;
	obs_get_video_info(&ovi);

	struct obs_audio_info oai;
	obs_get_audio_info(&oai);

	GError *err = NULL;

	gchar *pipe = g_strdup_printf(
		"appsrc name=video ! video/x-h264, width=%d, height=%d, stream-format=byte-stream ! h264parse ! queue ! matroskamux name=mux ! filesink location=/tmp/out.mkv "
		"appsrc name=audio ! audio/mpeg, mpegversion=4, stream-format=raw, rate=%d, channels=%d, codec_data=(buffer)1190 ! aacparse ! queue ! mux.",
		ovi.output_width, ovi.output_height, oai.samples_per_sec,
		oai.speakers);

	data->pipe = gst_parse_launch(pipe, &err);
	g_free(pipe);
	if (err) {
		g_error_free(err);
		g_free(data);

		return NULL;
	}

	data->video = gst_bin_get_by_name(GST_BIN(data->pipe), "video");
	data->audio = gst_bin_get_by_name(GST_BIN(data->pipe), "audio");

	g_object_set(data->video, "format", GST_FORMAT_TIME, NULL);
	g_object_set(data->audio, "format", GST_FORMAT_TIME, NULL);

	gst_element_set_state(data->pipe, GST_STATE_PLAYING);

	if (!obs_output_can_begin_data_capture(data->output, 0))
		return false;
	if (!obs_output_initialize_encoders(data->output, 0))
		return false;

	obs_output_begin_data_capture(data->output, 0);

	return true;
}

void gstreamer_output_stop(void *p, uint64_t ts)
{
	data_t *data = (data_t *)p;

	g_print("stop\n");

	obs_output_end_data_capture(data->output);

	if (data->pipe) {
		gst_app_src_end_of_stream(GST_APP_SRC(data->video));
		gst_app_src_end_of_stream(GST_APP_SRC(data->audio));

		GstBus *bus = gst_element_get_bus(data->pipe);
		GstMessage *msg = gst_bus_timed_pop_filtered(
			bus, GST_CLOCK_TIME_NONE, GST_MESSAGE_EOS);
		gst_message_unref(msg);
		gst_object_unref(bus);

		gst_object_unref(data->video);
		gst_object_unref(data->audio);

		gst_element_set_state(data->pipe, GST_STATE_NULL);
		gst_object_unref(data->pipe);
		data->pipe = NULL;
	}
}

void gstreamer_output_encoded_packet(void *p, struct encoder_packet *packet)
{
	data_t *data = (data_t *)p;

	g_print("encoded_packet\n");

	GstBuffer *buffer = gst_buffer_new_allocate(NULL, packet->size, NULL);
	gst_buffer_fill(buffer, 0, packet->data, packet->size);

	GST_BUFFER_PTS(buffer) = packet->pts * GST_SECOND /
				 (packet->timebase_den / packet->timebase_num);
	GST_BUFFER_DTS(buffer) = packet->dts * GST_SECOND /
				 (packet->timebase_den / packet->timebase_num);

	gst_buffer_set_flags(buffer,
			     packet->keyframe ? 0 : GST_BUFFER_FLAG_DELTA_UNIT);

	GstElement *appsrc = packet->type == OBS_ENCODER_VIDEO ? data->video
							       : data->audio;

	gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer);
}
