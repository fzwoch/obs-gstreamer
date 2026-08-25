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

#define _GNU_SOURCE

#ifdef __linux__
#include <dirent.h>
#endif
#include <obs/obs-module.h>
#include <obs/util/dstr.h>
#include <gst/gst.h>
#include <gst/app/app.h>
#include <gst/video/video.h>

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

typedef struct {
	GstElement *pipe;
	GstElement *appsrc;
	GstElement *appsink;
	GstVideoInfo vinfo;
	bool is_h265;
	guint8 *codec_data;
	size_t codec_data_size;
	GstSample *sample;
	GstMapInfo info;
	obs_encoder_t *encoder;
	obs_data_t *settings;
	struct obs_video_info ovi;
} data_t;

const char *gstreamer_encoder_get_name_h264(void *type_data)
{
	return "GStreamer Encoder H.264";
}

const char *gstreamer_encoder_get_name_h265(void *type_data)
{
	return "GStreamer Encoder H.265";
}

static const char *get_gst_format(enum video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_I420:
		return "I420";
	case VIDEO_FORMAT_NV12:
		return "NV12";
	case VIDEO_FORMAT_YVYU:
		return "YVYU";
	case VIDEO_FORMAT_YUY2:
		return "YUY2";
	case VIDEO_FORMAT_UYVY:
		return "UYVY";
	case VIDEO_FORMAT_I422:
		return "Y42B";
	case VIDEO_FORMAT_RGBA:
		return "RGBA";
	case VIDEO_FORMAT_BGRA:
		return "BGRA";
	case VIDEO_FORMAT_BGRX:
		return "BGRx";
	case VIDEO_FORMAT_I444:
		return "Y444";
	default:
		return NULL;
	}
}

// Number of rows a plane has for the given OBS pixel format. Used to copy
// plane-by-plane without overrunning chroma planes.
static int plane_rows(enum video_format format, int height, int plane)
{
	switch (format) {
	case VIDEO_FORMAT_I420:
	case VIDEO_FORMAT_NV12:
		if (plane == 0)
			return height;
		return plane == 1 ? (height + 1) / 2 : 0;
	case VIDEO_FORMAT_I422:
	case VIDEO_FORMAT_I444:
		return height;
	default:
		return plane == 0 ? height : 0;
	}
}

static void clear_pipeline(data_t *data)
{
	if (data->appsink != NULL) {
		gst_object_unref(data->appsink);
		data->appsink = NULL;
	}
	if (data->appsrc != NULL) {
		gst_object_unref(data->appsrc);
		data->appsrc = NULL;
	}
	if (data->pipe != NULL) {
		gst_element_set_state(data->pipe, GST_STATE_NULL);
		gst_object_unref(data->pipe);
		data->pipe = NULL;
	}
}

// Extract codec data (SPS/PPS/AUD and friends) from the first Annex B byte
// stream AU up to the first intra (keyframe) NAL unit. Returns true when the
// codec data was captured. The scan is bounded so it can never read past the
// mapped buffer.
static bool extract_codec_data(data_t *data)
{
	GstMapInfo info = data->info;

	for (size_t size = 0; size + 5 <= info.size; size++) {
		if (info.data[size + 0] != 0 || info.data[size + 1] != 0 || info.data[size + 2] != 0 ||
		    info.data[size + 3] != 1)
			continue;

		bool is_keyframe_nal;
		if (data->is_h265) {
			// HEVC: NAL type occupies bits 6..1 of the first header byte.
			// Types 16..23 are BLA/IDR/CRA (intra) NAL units.
			int nal_type = (info.data[size + 4] >> 1) & 0x3f;
			is_keyframe_nal = nal_type >= 16 && nal_type <= 23;
		} else {
			// H.264: NAL type occupies bits 4..0 of the header byte.
			is_keyframe_nal = (info.data[size + 4] & 0x1f) == 5;
		}

		if (!is_keyframe_nal)
			continue;

		if (size == 0)
			return false;

		data->codec_data = g_malloc(size);
		memcpy(data->codec_data, info.data, size);
		data->codec_data_size = size;
		return true;
	}

	return false;
}

static void *gstreamer_encoder_create(obs_data_t *settings, obs_encoder_t *encoder, bool h265)
{
	data_t *data = g_new0(data_t, 1);

	data->encoder = encoder;
	data->settings = settings;
	data->is_h265 = h265;

	obs_get_video_info(&data->ovi);

	data->ovi.output_width = obs_encoder_get_width(encoder);
	data->ovi.output_height = obs_encoder_get_height(encoder);

	const char *format = get_gst_format(data->ovi.output_format);
	if (format == NULL) {
		blog(LOG_ERROR, "[obs-gstreamer] unhandled output format: %d", data->ovi.output_format);
		g_free(data);
		return NULL;
	}

	const gchar *encoder_type = obs_data_get_string(data->settings, "encoder_type");

	const gboolean is_cbr = g_strcmp0(obs_data_get_string(data->settings, "rate_control"), "CBR") == 0 ? true
													   : false;

	const int gop_frames =
		MAX(1, (int)obs_data_get_int(data->settings, "keyint_sec") * (int)data->ovi.fps_num /
			       (int)data->ovi.fps_den);

	gchar *encoder_string = NULL;
	if (!h265 && g_strcmp0(encoder_type, "x264") == 0) {
		encoder_string = g_strdup_printf("x264enc tune=zerolatency bitrate=%d pass=%s key-int-max=%d",
						 (int)obs_data_get_int(data->settings, "bitrate"),
						 is_cbr ? "cbr" : "pass1", gop_frames);
	} else if (!h265 && g_strcmp0(encoder_type, "nvh264enc") == 0) {
		encoder_string = g_strdup_printf(
			"nvh264enc bitrate=%d rc-mode=%s gop-size=%d",
			(int)obs_data_get_int(data->settings, "bitrate"), is_cbr ? "cbr" : "vbr", gop_frames);
	} else if (!h265 && g_strcmp0(encoder_type, "vaapih264enc") == 0) {
		g_setenv("GST_VAAPI_DRM_DEVICE", obs_data_get_string(data->settings, "device"), TRUE);
		encoder_string = g_strdup_printf("vaapih264enc bitrate=%d rate-control=%s keyframe-period=%d",
						 (int)obs_data_get_int(data->settings, "bitrate"),
						 is_cbr ? "cbr" : "vbr", gop_frames);
	} else if (!h265 && g_strcmp0(encoder_type, "omxh264enc") == 0) {
		encoder_string = g_strdup_printf(
			"omxh264enc target-bitrate=%d control-rate=%s periodicity-idr=%d",
			(int)obs_data_get_int(data->settings, "bitrate") * 1000, is_cbr ? "constant" : "variable",
			gop_frames);
	} else if (!h265 && g_strcmp0(encoder_type, "omxh264enc_old") == 0) {
		encoder_string = g_strdup_printf("omxh264enc bitrate=%d control-rate=%s iframeinterval=%d",
						 (int)obs_data_get_int(data->settings, "bitrate") * 1000,
						 is_cbr ? "constant" : "variable", gop_frames);
	} else if (!h265 && g_strcmp0(encoder_type, "vtenc_h264") == 0) {
		encoder_string = g_strdup_printf("vtenc_h264 bitrate=%d max-keyframe-interval=%d",
						 (int)obs_data_get_int(data->settings, "bitrate"), gop_frames);
	} else if (!h265 && g_strcmp0(encoder_type, "msdkh264enc") == 0) {
		encoder_string = g_strdup_printf("msdkh264enc bitrate=%d rate-control=%s gop-size=%d",
						 (int)obs_data_get_int(data->settings, "bitrate"),
						 is_cbr ? "cbr" : "vbr", gop_frames);
	} else if (!h265 && g_strcmp0(encoder_type, "mpph264enc") == 0) {
		encoder_string = g_strdup_printf("mpph264enc bps=%d rc-mode=%s gop=%d",
						 (int)obs_data_get_int(data->settings, "bitrate") * 1000,
						 is_cbr ? "cbr" : "vbr", gop_frames);
	} else if (h265 && g_strcmp0(encoder_type, "vaapih265enc") == 0) {
		g_setenv("GST_VAAPI_DRM_DEVICE", obs_data_get_string(data->settings, "device"), TRUE);
		encoder_string = g_strdup_printf("vaapih265enc bitrate=%d rate-control=%s keyframe-period=%d",
						 (int)obs_data_get_int(data->settings, "bitrate"),
						 is_cbr ? "cbr" : "vbr", gop_frames);
	} else if (h265 && g_strcmp0(encoder_type, "nvh265enc") == 0) {
		encoder_string = g_strdup_printf(
			"nvh265enc bitrate=%d rc-mode=%s gop-size=%d",
			(int)obs_data_get_int(data->settings, "bitrate"), is_cbr ? "cbr" : "vbr", gop_frames);
	} else if (h265 && g_strcmp0(encoder_type, "msdkh265enc") == 0) {
		encoder_string = g_strdup_printf("msdkh265enc bitrate=%d rate-control=%s gop-size=%d",
						 (int)obs_data_get_int(data->settings, "bitrate"),
						 is_cbr ? "cbr" : "vbr", gop_frames);
	} else if (h265 && g_strcmp0(encoder_type, "mpph265enc") == 0) {
		encoder_string = g_strdup_printf("mpph265enc bps=%d rc-mode=%s gop=%d",
						 (int)obs_data_get_int(data->settings, "bitrate") * 1000,
						 is_cbr ? "cbr" : "vbr", gop_frames);
	} else {
		blog(LOG_ERROR, "[obs-gstreamer] invalid encoder selected");
		g_free(data);
		return NULL;
	}

	gchar *caps_string = g_strdup_printf(
		"video/x-raw, format=%s, width=%d, height=%d, framerate=%u/%u, interlace-mode=progressive", format,
		data->ovi.output_width, data->ovi.output_height, data->ovi.fps_num, data->ovi.fps_den);

	// Derive strides/offsets/total size for the caps we advertise so input
	// frames can be copied safely even when libobs pads its linesizes.
	GstCaps *caps = gst_caps_from_string(caps_string);
	bool have_info = caps != NULL && gst_video_info_from_caps(&data->vinfo, caps);
	if (caps != NULL)
		gst_caps_unref(caps);
	if (!have_info) {
		blog(LOG_ERROR, "[obs-gstreamer] cannot derive video info for format %s", format);
		g_free(caps_string);
		g_free(encoder_string);
		g_free(data);
		return NULL;
	}

	const char *parser = h265 ? "h265parse" : "h264parse";
	const char *media_type = h265 ? "h265" : "h264";

	gchar *pipe_string = g_strdup_printf(
		"appsrc name=appsrc ! %s ! videoconvert ! %s name=video_encoder %s ! %s ! video/x-%s, stream-format=byte-stream, alignment=au ! appsink sync=false name=appsink",
		caps_string, encoder_string, obs_data_get_string(data->settings, "extra_options"), parser,
		media_type);

	g_free(caps_string);
	g_free(encoder_string);

	GError *err = NULL;

	// Note: gst_parse_launch() can return a partially built pipeline even
	// when it reports an error, so always clean up data->pipe here.
	data->pipe = gst_parse_launch(pipe_string, &err);

	g_free(pipe_string);

	if (err != NULL) {
		blog(LOG_ERROR, "[obs-gstreamer] %s", err->message);
		g_error_free(err);
		clear_pipeline(data);
		g_free(data);
		return NULL;
	}

	data->appsrc = gst_bin_get_by_name(GST_BIN(data->pipe), "appsrc");
	data->appsink = gst_bin_get_by_name(GST_BIN(data->pipe), "appsink");

	if (data->appsrc == NULL || data->appsink == NULL) {
		blog(LOG_ERROR, "[obs-gstreamer] pipeline misses appsrc/appsink");
		clear_pipeline(data);
		g_free(data);
		return NULL;
	}

	gst_element_set_state(data->pipe, GST_STATE_PLAYING);

	return data;
}

void *gstreamer_encoder_create_h264(obs_data_t *settings, obs_encoder_t *encoder)
{
	return gstreamer_encoder_create(settings, encoder, false);
}

void *gstreamer_encoder_create_h265(obs_data_t *settings, obs_encoder_t *encoder)
{
	return gstreamer_encoder_create(settings, encoder, true);
}

void gstreamer_encoder_destroy(void *p)
{
	data_t *data = (data_t *)p;

	clear_pipeline(data);

	if (data->sample != NULL) {
		GstBuffer *buffer = gst_sample_get_buffer(data->sample);
		gst_buffer_unmap(buffer, &data->info);
		gst_sample_unref(data->sample);
	}

	g_free(data->codec_data);
	g_free(data);
}

bool gstreamer_encoder_encode(void *p, struct encoder_frame *frame, struct encoder_packet *packet,
			      bool *received_packet)
{
	data_t *data = (data_t *)p;

	// delayed release of previous sample
	if (data->sample != NULL) {
		GstBuffer *buffer = gst_sample_get_buffer(data->sample);
		gst_buffer_unmap(buffer, &data->info);
		gst_sample_unref(data->sample);
		data->sample = NULL;
	}

	GstBuffer *buffer;

	// Fast path: wrap the frame directly when libobs planes are packed the
	// same way GStreamer expects them. Otherwise (or with force_copy) make
	// a plane-wise copy that honors linesize padding and chroma heights.
	bool packed = frame->linesize[0] == (uint32_t)data->vinfo.stride[0] &&
		      frame->linesize[1] == (uint32_t)data->vinfo.stride[1] &&
		      frame->linesize[2] == (uint32_t)data->vinfo.stride[2];

	if (obs_data_get_bool(data->settings, "force_copy") == true || !packed) {
		buffer = gst_buffer_new_allocate(NULL, data->vinfo.size, NULL);

		GstMapInfo map;
		gst_buffer_map(buffer, &map, GST_MAP_WRITE);

		for (int j = 0; j <= 2 && frame->linesize[j] != 0; j++) {
			size_t src_stride = frame->linesize[j];
			size_t dst_stride = data->vinfo.stride[j];
			size_t bytes = MIN(src_stride, dst_stride);
			int rows = plane_rows(data->ovi.output_format, data->ovi.output_height, j);

			for (int i = 0; i < rows; i++)
				memcpy(map.data + data->vinfo.offset[j] + i * dst_stride,
				       frame->data[j] + i * src_stride, bytes);
		}

		gst_buffer_unmap(buffer, &map);
	} else {
		buffer = gst_buffer_new_wrapped_full(0, frame->data[0], data->vinfo.size, 0, data->vinfo.size, NULL,
						     NULL);
	}

	// frame->pts is in units of the encoder timebase (num/den). Convert to
	// nanoseconds without truncating fractional frame rates (e.g. 30000/1001).
	GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(frame->pts, (guint64)GST_SECOND * packet->timebase_num,
						       packet->timebase_den);

	gst_app_src_push_buffer(GST_APP_SRC(data->appsrc), buffer);

	data->sample = gst_app_sink_try_pull_sample(GST_APP_SINK(data->appsink), 0);
	if (data->sample == NULL)
		return true;

	*received_packet = true;

	buffer = gst_sample_get_buffer(data->sample);

	gst_buffer_map(buffer, &data->info, GST_MAP_READ);

	if (data->codec_data == NULL)
		extract_codec_data(data);

	packet->data = data->info.data;
	packet->size = data->info.size;

	// Convert nanoseconds back to encoder timebase ticks.
	packet->pts = gst_util_uint64_scale(GST_BUFFER_PTS(buffer), packet->timebase_den,
					   (guint64)packet->timebase_num * GST_SECOND);

	if (GST_CLOCK_TIME_IS_VALID(GST_BUFFER_DTS(buffer)))
		packet->dts = gst_util_uint64_scale(GST_BUFFER_DTS(buffer), packet->timebase_den,
						    (guint64)packet->timebase_num * GST_SECOND);
	else
		packet->dts = packet->pts;

	packet->type = OBS_ENCODER_VIDEO;

	packet->keyframe = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);

	return true;
}

void gstreamer_encoder_get_defaults_h264(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "device", "/dev/dri/renderD128");
	obs_data_set_default_string(settings, "encoder_type", "x264");
	obs_data_set_default_int(settings, "bitrate", 2500);
	obs_data_set_default_string(settings, "rate_control", "CBR");
	obs_data_set_default_int(settings, "keyint_sec", 2);
	obs_data_set_default_bool(settings, "force_copy", false);
}

void gstreamer_encoder_get_defaults_h265(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "device", "/dev/dri/renderD128");
	obs_data_set_default_string(settings, "encoder_type", "vaapih265enc");
	obs_data_set_default_int(settings, "bitrate", 2500);
	obs_data_set_default_string(settings, "rate_control", "CBR");
	obs_data_set_default_int(settings, "keyint_sec", 2);
	obs_data_set_default_bool(settings, "force_copy", false);
}

static bool check_feature(char *name)
{
	GstRegistry *registry = gst_registry_get();
	GstPluginFeature *feature = gst_registry_lookup_feature(registry, name);

	if (feature) {
		gst_object_unref(feature);
		return true;
	}

	return false;
}

#ifdef __linux__
static int scanfilter(const struct dirent *entry)
{
	return !astrcmp_n(entry->d_name, "renderD", 7);
}

static void populate_vaapi_devices(obs_property_t *prop)
{
	struct dirent **list;
	int n = scandir("/dev/dri", &list, scanfilter, versionsort);

	for (int i = 0; i < n; i++) {
		char device[16 + NAME_MAX] = {0};
		int w = snprintf(device, sizeof(device), "/dev/dri/%s", list[i]->d_name);
		(void)w;
		obs_property_list_add_string(prop, device, device);
	}

	while (n--)
		free(list[n]);
	free(list);
}
#endif

static bool encoder_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	obs_property_t *device = obs_properties_get(props, "device");

	if (g_strcmp0(obs_data_get_string(settings, "encoder_type"), "vaapih264enc") == 0 ||
	    g_strcmp0(obs_data_get_string(settings, "encoder_type"), "vaapih265enc") == 0)
		obs_property_set_visible(device, true);
	else
		obs_property_set_visible(device, false);

	return true;
}

static obs_properties_t *encoder_get_properties(bool h265)
{
	obs_properties_t *props = obs_properties_create();

	obs_property_t *prop = obs_properties_add_list(props, "encoder_type", "Encoder type", OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);

	obs_property_set_modified_callback(prop, encoder_modified);

	if (!h265) {
		if (check_feature("x264enc"))
			obs_property_list_add_string(prop, "x264", "x264");
		if (check_feature("nvh264enc"))
			obs_property_list_add_string(prop, "NVIDIA (NVENC)", "nvh264enc");
		if (check_feature("vaapih264enc"))
			obs_property_list_add_string(prop, "VA-API", "vaapih264enc");
		if (check_feature("omxh264enc"))
			obs_property_list_add_string(prop, "OpenMAX (Raspberry Pi)", "omxh264enc");
		if (check_feature("omxh264enc"))
			obs_property_list_add_string(prop, "OpenMAX (Tegra)", "omxh264enc_old");
		if (check_feature("vtenc_h264"))
			obs_property_list_add_string(prop, "Apple (VideoToolBox)", "vtenc_h264");
		if (check_feature("msdkh264enc"))
			obs_property_list_add_string(prop, "Intel MSDK H264 encoder", "msdkh264enc");
		if (check_feature("mpph264enc"))
			obs_property_list_add_string(prop, "Rockchip MPP H264 encoder", "mpph264enc");
	} else {
		if (check_feature("vaapih265enc"))
			obs_property_list_add_string(prop, "VA-API", "vaapih265enc");
		if (check_feature("nvh265enc"))
			obs_property_list_add_string(prop, "NVIDIA (NVENC)", "nvh265enc");
		if (check_feature("msdkh265enc"))
			obs_property_list_add_string(prop, "Intel MSDK H265 encoder", "msdkh265enc");
		if (check_feature("mpph265enc"))
			obs_property_list_add_string(prop, "Rockchip MPP H265 encoder", "mpph265enc");
	}

	prop = obs_properties_add_list(props, "device", "Device", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	obs_property_set_long_description(prop, "For VAAPI only");

#ifdef __linux__
	populate_vaapi_devices(prop);
#endif

	prop = obs_properties_add_int(props, "bitrate", "Bitrate", 50, 10000000, 50);
	//	obs_property_int_set_suffix(prop, " Kbps");

	prop = obs_properties_add_list(props, "rate_control", "Rate control", OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_STRING);

	obs_property_list_add_string(prop, "Constant bitrate", "CBR");
	obs_property_list_add_string(prop, "Variable bitrate", "VBR");
	obs_property_list_add_string(prop, "Constant QP", "CQP");
	obs_property_list_add_string(prop, "Constant QP - Intelligent", "ICQ");
	obs_property_list_add_string(prop, "Variable bitrate - Quality defined", "QVBR");

	prop = obs_properties_add_int(props, "keyint_sec", "Keyframe interval", 1, 20, 1);
	//	obs_property_int_set_suffix(prop, " seconds");

	prop = obs_properties_add_text(props, "extra_options", "Extra encoder options", OBS_TEXT_MULTILINE);
	obs_property_set_long_description(prop,
					  "Extra encoder options. Use the form of key=value separated by spaces.");

	obs_properties_add_bool(props, "force_copy", "Force memory copy");

	return props;
}

obs_properties_t *gstreamer_encoder_get_properties_h264(void *data)
{
	UNUSED_PARAMETER(data);
	return encoder_get_properties(false);
}

obs_properties_t *gstreamer_encoder_get_properties_h265(void *data)
{
	UNUSED_PARAMETER(data);
	return encoder_get_properties(true);
}

bool gstreamer_encoder_get_extra_data(void *p, uint8_t **extra_data, size_t *size)
{
	data_t *data = (data_t *)p;

	if (data->codec_data == NULL)
		return false;

	*extra_data = data->codec_data;
	*size = data->codec_data_size;

	return true;
}
