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
	gsize buffer_size;
	GstClockTime offset;
	guint8 *codec_data;
	size_t codec_data_size;
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

	const char *format = "";

	data->encoder = encoder;
	data->settings = settings;

	obs_get_video_info(&data->ovi);

	switch (data->ovi.output_format) {
	case VIDEO_FORMAT_I420:
		format = "I420";
		data->buffer_size = data->ovi.output_width *
				    data->ovi.output_height * 3 / 2;
		break;
	case VIDEO_FORMAT_NV12:
		format = "NV12";
		data->buffer_size = data->ovi.output_width *
				    data->ovi.output_height * 3 / 2;
		break;
	case VIDEO_FORMAT_YVYU:
		format = "YVYU";
		data->buffer_size =
			data->ovi.output_width * data->ovi.output_height * 2;
		break;
	case VIDEO_FORMAT_YUY2:
		format = "YUY2";
		data->buffer_size =
			data->ovi.output_width * data->ovi.output_height * 2;
		break;
	case VIDEO_FORMAT_UYVY:
		format = "UYVY";
		data->buffer_size =
			data->ovi.output_width * data->ovi.output_height * 2;
		break;
		/*
	case VIDEO_FORMAT_I422:
		format = "I422";
		data->buffer_size =
			data->ovi.output_width * data->ovi.output_height * 2;
		break;
		*/
	case VIDEO_FORMAT_RGBA:
		format = "RGBA";
		data->buffer_size =
			data->ovi.output_width * data->ovi.output_height * 3;
	case VIDEO_FORMAT_BGRA:
		format = "BGRA";
		data->buffer_size =
			data->ovi.output_width * data->ovi.output_height * 3;
	case VIDEO_FORMAT_BGRX:
		format = "BGRX";
		data->buffer_size =
			data->ovi.output_width * data->ovi.output_height * 3;
	case VIDEO_FORMAT_I444:
		format = "I444";
		data->buffer_size =
			data->ovi.output_width * data->ovi.output_height * 3;
		break;
	default:
		blog(LOG_ERROR, "unhandled output format: %d\n",
		     data->ovi.output_format);
		break;
	}

	const gchar *encoder_type =
		obs_data_get_string(data->settings, "encoder_type");

	gchar *encoder_string = "";
	if (g_strcmp0(encoder_type, "x264") == 0) {
		encoder_string = g_strdup_printf(
			"x264enc bframes=0 tune=zerolatency bitrate=%lld key-int-max=%lld",
			obs_data_get_int(data->settings, "bitrate"),
			obs_data_get_int(data->settings, "keyint_sec") *
				data->ovi.fps_num / data->ovi.fps_den);
	} else if (g_strcmp0(encoder_type, "nvh264enc") == 0) {
		encoder_string = g_strdup_printf(
			"nvh264enc bitrate=%lld gop-size=%lld",
			obs_data_get_int(data->settings, "bitrate"),
			obs_data_get_int(data->settings, "keyint_sec") *
				data->ovi.fps_num / data->ovi.fps_den);
	}

	gchar *pipe_string = g_strdup_printf(
		"appsrc name=appsrc ! video/x-raw, format=%s, width=%d, height=%d, framerate=%d/%d, interlace-mode=progressive ! videoconvert ! %s ! h264parse ! video/x-h264, stream-format=byte-stream, alignment=au ! appsink sync=false name=appsink",
		format, data->ovi.output_width, data->ovi.output_height,
		data->ovi.fps_num, data->ovi.fps_den, encoder_string);

	GError *err = NULL;

	data->pipe = gst_parse_launch(pipe_string, &err);

	g_free(encoder_string);
	g_free(pipe_string);

	if (err != NULL) {
		blog(LOG_ERROR, "%s", err->message);
		return NULL;
	}

	data->appsrc = gst_bin_get_by_name(GST_BIN(data->pipe), "appsrc");
	data->appsink = gst_bin_get_by_name(GST_BIN(data->pipe), "appsink");

	data->offset = GST_CLOCK_TIME_NONE;

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

	g_free(data->codec_data);
	g_free(data);
}

bool gstreamer_encoder_encode(void *p, struct encoder_frame *frame,
			      struct encoder_packet *packet,
			      bool *received_packet)
{
	data_t *data = (data_t *)p;

	GstBuffer *buffer =
		gst_buffer_new_allocate(NULL, data->buffer_size, NULL);

	gint32 offset = 0;
	for (int j = 0; frame->linesize[j] != 0; j++) {
		for (int i = 0; i < data->ovi.output_height; i++) {
			gst_buffer_fill(buffer, offset,
					frame->data[j] + i * frame->linesize[j],
					frame->linesize[j]);
			offset += frame->linesize[j];
		}
	}

	GST_BUFFER_PTS(buffer) = frame->pts * (GST_SECOND / (data->ovi.fps_num / data->ovi.fps_den));

	gst_app_src_push_buffer(GST_APP_SRC(data->appsrc), buffer);

	GstSample *sample =
		gst_app_sink_try_pull_sample(GST_APP_SINK(data->appsink), 0);
	if (sample == NULL)
		return true;

	*received_packet = true;

	buffer = gst_sample_get_buffer(sample);

	if (data->offset == GST_CLOCK_TIME_NONE)
		data->offset = GST_BUFFER_PTS(buffer);

	GstMapInfo info;

	gst_buffer_map(buffer, &info, GST_MAP_READ);

	if (!data->codec_data) {
		size_t size;

		// this is pretty lazy..
		for (size = 0; size < info.size; size++) {
			if (info.data[size + 0] == 0 &&
			    info.data[size + 1] == 0 &&
			    info.data[size + 2] == 0 &&
			    info.data[size + 3] == 1 &&
			    (info.data[size + 4] & 0x1f) == 5) {
				break;
			}
		}

		data->codec_data = g_memdup(info.data, size);
		data->codec_data_size = size;
	}

	packet->data = g_memdup(info.data, info.size);
	packet->size = info.size;

	packet->pts = (int64_t)GST_BUFFER_PTS(buffer) - (int64_t)data->offset;
	packet->dts = (int64_t)GST_BUFFER_DTS(buffer) - (int64_t)data->offset;

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
	obs_data_set_default_int(settings, "bitrate", 2500);
	obs_data_set_default_string(settings, "rate_control", "CBR");
	obs_data_set_default_int(settings, "keyint_sec", 2);
}

static bool check_feature(char *name)
{
	GstRegistry *registry = gst_registry_get();
	GstPluginFeature *feature = gst_registry_lookup_feature(registry, name);

	if (feature) {
		g_object_unref(feature);
		return true;
	}

	return false;
}

obs_properties_t *gstreamer_encoder_get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_property_t *prop = obs_properties_add_list(props, "encoder_type",
						       "Encoder type",
						       OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);

	if (check_feature("x264enc"))
		obs_property_list_add_string(prop, "x264", "x264");
	if (check_feature("nvh264enc"))
		obs_property_list_add_string(prop, "NVIDIA (NVENC)",
					     "nvh264enc");
	if (check_feature("omxh264enc"))
		obs_property_list_add_string(
			prop, "OpenMAX (Raspberry Pi / Tegra)", "omx");
	if (check_feature("vtenc_h264"))
		obs_property_list_add_string(prop, "Apple (VideoToolBox)",
					     "vtenc_h264");

	prop = obs_properties_add_int(props, "bitrate", "Bitrate", 50, 10000000,
				      50);
	//	obs_property_int_set_suffix(prop, " Kbps");

	prop = obs_properties_add_list(props, "rate_control", "Rate control",
				       OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_STRING);

	obs_property_list_add_string(prop, "Constant bitrate", "CBR");
	obs_property_list_add_string(prop, "Variable bitrate", "VBR");

	prop = obs_properties_add_int(props, "keyint_sec", "Keyframe interval",
				      0, 20, 1);
	//	obs_property_int_set_suffix(prop, " seconds");

	return props;
}

bool gstreamer_encoder_get_extra_data(void *p, uint8_t **extra_data,
				      size_t *size)
{
	data_t *data = (data_t *)p;

	if (!data->codec_data) {
		return false;
	}

	*extra_data = data->codec_data;
	*size = data->codec_data_size;

	return true;
}

bool gstreamer_encoder_update(void *data, obs_data_t *settings)
{
	return true;
}
