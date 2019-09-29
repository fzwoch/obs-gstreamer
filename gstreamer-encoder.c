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
	GstElement *pipe;
	GstElement *appsrc;
	GstElement *appsink;
	obs_encoder_t *encoder;
	obs_data_t *settings;
	struct obs_video_info ovi;
} data_t;

const char *gstreamer_encoder_get_name(void *type_data)
{
	return "GStreamer Encoder";
}

void *gstreamer_encoder_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	data_t *data = g_new0(data_t, 1);

	data->encoder = encoder;
	data->settings = settings;

	obs_get_video_info(&data->ovi);

	const char *format_mapping[18] = {
		"",                             // invalid
		"I420", "NV12",                 // 4:2:0
		"YVYU", "YUY2", "UYVY",         // packed 4:2:2
		"RGBA", "BGRA", "BGRX", "Y800", // packed
		"I444",                         // planar 4:4:4
		"BGR3",                         // uh..
		"I422",                         // planar 4:2:2
	};

	gchar *pipe_string = g_strdup_printf(
		"appsrc name=appsrc ! video/x-raw, format=%s, width=%d, height=%d, framerate=%d/%d ! videoconvert ! x264enc ! h264parse ! video/x-h264, stream-format=byte-stream, alignment=au ! appsink sync=false name=appsink",
		format_mapping[data->ovi.output_format], data->ovi.output_width,
		data->ovi.output_height, data->ovi.fps_num, data->ovi.fps_den);

	GError *err = NULL;

	data->pipe = gst_parse_launch(pipe_string, &err);
	g_free(pipe_string);

	if (err != NULL) {
		blog(LOG_ERROR, "%s", err->message);
		return NULL;
	}

	data->appsrc = gst_bin_get_by_name(GST_BIN(data->pipe), "appsrc");
	data->appsink = gst_bin_get_by_name(GST_BIN(data->pipe), "appsink");

	gst_element_set_state(data->pipe, GST_STATE_PLAYING);

	return data;
}

void gstreamer_encoder_destroy(void *p)
{
	data_t *data = (data_t *)p;

	gst_element_set_state(data->pipe, GST_STATE_NULL);

	gst_object_unref(data->appsink);
	gst_object_unref(data->appsrc);
	gst_object_unref(data->pipe);

	data->appsink = NULL;
	data->appsrc = NULL;
	data->pipe = NULL;

	g_free(data);
}

bool gstreamer_encoder_encode(void *p, struct encoder_frame *frame,
			      struct encoder_packet *packet,
			      bool *received_packet)
{
	data_t *data = (data_t *)p;

	GstBuffer *buffer = gst_buffer_new_allocate(
		NULL, data->ovi.output_width * data->ovi.output_height * 3 / 2,
		NULL);

	//	gst_buffer_fill(buffer, 0, NULL, 0);

	GST_BUFFER_PTS(buffer) = frame->pts;

	gst_app_src_push_buffer(GST_APP_SRC(data->appsrc), buffer);

	GstSample *sample =
		gst_app_sink_try_pull_sample(GST_APP_SINK(data->appsink), 0);
	if (sample == NULL) {
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

	packet->keyframe =
		!GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);

	gst_buffer_unmap(buffer, &info);
	gst_sample_unref(sample);

	return true;
}

void gstreamer_encoder_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "encoder_type", "x264");
	obs_data_set_default_int(settings, "bitrate", 2000);
	obs_data_set_default_string(settings, "rate_control", "CBR");
	obs_data_set_default_int(settings, "keyint_sec", 2);
}

obs_properties_t *gstreamer_encoder_get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_property_t *prop = obs_properties_add_list(props, "encoder_type",
						       "Encoder type",
						       OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);

	obs_property_list_add_string(prop, "x264", "x264");
	obs_property_list_add_string(prop, "NVIDIA (NVENC)", "nvenc");
	obs_property_list_add_string(prop, "OpenMAX (Raspberry Pi / Tegra)",
				     "omx");
	obs_property_list_add_string(prop, "Apple (VideoToolBox)", "vtenc");

	obs_properties_add_int(props, "bitrate", "Bitrate", 100, 200000, 100);

	prop = obs_properties_add_list(props, "rate_control", "Rate control",
				       OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_STRING);

	obs_property_list_add_string(prop, "Constant bitrate", "CBR");
	obs_property_list_add_string(prop, "Variable bitrate", "VBR");

	obs_properties_add_int(props, "keyint_sec", "Keyframe interval", 1, 200,
			       1);

	return props;
}

bool gstreamer_encoder_get_extra_data(void *data, uint8_t **extra_data,
				      size_t *size)
{
	blog(LOG_INFO, "!! get_extra_data");

	return false;
}

bool gstreamer_encoder_get_sei_data(void *data, uint8_t **sei_data,
				    size_t *size)
{
	blog(LOG_INFO, "!! get_sei_data");

	return false;
}

void gstreamer_encoder_video_info(void *p, struct video_scale_info *info)
{
	data_t *data = (data_t *)p;

	blog(LOG_INFO, "!! video_info");

	info->format = VIDEO_FORMAT_I420;
	info->width = data->ovi.output_width;
	info->height = data->ovi.output_height;
	info->range = VIDEO_RANGE_DEFAULT;
	info->colorspace = VIDEO_CS_DEFAULT;
}

bool gstreamer_encoder_update(void *data, obs_data_t *settings)
{
	return true;
}
