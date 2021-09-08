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

typedef struct {
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

bool gstreamer_output_start(void *data)
{
	g_print("start\n");

	return true;
}

void gstreamer_output_stop(void *data, uint64_t ts)
{
	g_print("stop\n");
}

void gstreamer_output_raw_video(void *data, struct video_data *frame)
{
	g_print("raw_video\n");
}

void gstreamer_output_raw_audio(void *data, struct audio_data *frames)
{
	g_print("raw_audio\n");
}

void gstreamer_output_encoded_packet(void *data, struct encoder_packet *packet)
{
	g_print("encoded_packet\n");
}
