/*
 * obs-gstreamer. OBS Studio plugin.
 * Copyright (C) 2018-2019 Florian Zwoch <fzwoch@gmail.com>
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
	GstElement* pipe;
	GstElement* appsrc;
	GstElement* appsink;
	obs_encoder_t* encoder;
	obs_data_t* settings;
} data_t;

const char* gstreamer_encoder_get_name(void* type_data)
{
	return "GStreamer Encoder";
}

void* gstreamer_encoder_create(obs_data_t* settings, obs_encoder_t* encoder)
{
	data_t* data = g_new0(data_t, 1);

	data->encoder = encoder;
	data->settings = settings;

	data->pipe = gst_parse_launch("appsrc name=appsrc ! video/x-raw, format=I420 ! x264enc ! h264parse ! video/x-h264 stream-format=byte-stream alignment=au ! appsink name=appsink", NULL);

	data->appsrc = gst_bin_get_by_name(GST_BIN(data->pipe), "appsrc");
	data->appsink = gst_bin_get_by_name(GST_BIN(data->pipe), "appsink");

	gst_element_set_state(data->pipe, GST_STATE_PLAYING);

	return data;
}

void gstreamer_encoder_destroy(void* p)
{
   	data_t* data = (data_t*)p;

	gst_element_set_state(data->pipe, GST_STATE_NULL);

	gst_object_unref(data->appsink);
	gst_object_unref(data->appsrc);
	gst_object_unref(data->pipe);

	data->appsink = NULL;
	data->appsrc = NULL;
	data->pipe = NULL;

	g_free(data);
}

bool gstreamer_encoder_encode(void* p, struct encoder_frame* frame, struct encoder_packet* packet, bool* received_packet)
{
	data_t* data = (data_t*)p;

	GstBuffer* buffer = gst_buffer_new_allocate(NULL, 0, NULL);

	gst_buffer_fill(buffer, 0, NULL, 0);

	GST_BUFFER_PTS(buffer) = frame->pts;

	gst_app_src_push_buffer(GST_APP_SRC(data->appsrc), NULL);

	GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(data->appsink), 0);
	if (sample == NULL)
	{
		*received_packet = false;

		return true;
	}

	*received_packet = true;

	buffer = gst_sample_get_buffer(sample);
	GstMapInfo info;

	gst_buffer_map(buffer, &info, GST_MAP_READ);

	packet->data = g_memdup(info.data, info.size);
	packet->size = info.size;

	packet->pts = GST_BUFFER_PTS(buffer);
	packet->dts = GST_BUFFER_DTS(buffer);

	packet->timebase_num = GST_SECOND;
	packet->timebase_den = 1;

	packet->type = OBS_ENCODER_VIDEO;

	packet->keyframe = !GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);

	gst_buffer_unmap(buffer, &info);
	gst_sample_unref(sample);

	return true;
}

void gstreamer_encoder_get_defaults(obs_data_t* settings)
{
}

obs_properties_t* gstreamer_encoder_get_properties(void* data)
{
	obs_properties_t* props = obs_properties_create();

	return props;
}

bool gstreamer_encoder_update(void* data, obs_data_t* settings)
{
	return true;
}
