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

extern const char *obs_gstreamer_version;

OBS_DECLARE_MODULE()

// gstreamer-source.c
extern const char *gstreamer_source_get_name(void *type_data);
extern void *gstreamer_source_create(obs_data_t *settings,
				     obs_source_t *source);
extern void gstreamer_source_destroy(void *data);
extern void gstreamer_source_get_defaults(obs_data_t *settings);
extern obs_properties_t *gstreamer_source_get_properties(void *data);
extern void gstreamer_source_update(void *data, obs_data_t *settings);
extern void gstreamer_source_show(void *data);
extern void gstreamer_source_hide(void *data);

// gstreamer-encoder.c
extern const char *gstreamer_encoder_get_name(void *type_data);
extern void *gstreamer_encoder_create(obs_data_t *settings,
				      obs_encoder_t *encoder);
extern void gstreamer_encoder_destroy(void *data);
extern bool gstreamer_encoder_encode(void *data, struct encoder_frame *frame,
				     struct encoder_packet *packet,
				     bool *received_packet);
extern void gstreamer_encoder_get_defaults(obs_data_t *settings);
extern obs_properties_t *gstreamer_encoder_get_properties(void *data);
extern bool gstreamer_encoder_update(void *data, obs_data_t *settings);
extern bool gstreamer_encoder_get_extra_data(void *data, uint8_t **extra_data,
					     size_t *size);

// gstreamer-filter.c
extern const char *gstreamer_filter_get_name_video(void *type_data);
extern const char *gstreamer_filter_get_name_audio(void *type_data);
extern void *gstreamer_filter_create(obs_data_t *settings,
				     obs_source_t *source);
extern void gstreamer_filter_destroy(void *data);
extern void gstreamer_filter_get_defaults_video(obs_data_t *settings);
extern void gstreamer_filter_get_defaults_audio(obs_data_t *settings);
extern obs_properties_t *gstreamer_filter_get_properties(void *data);
extern void gstreamer_filter_update(void *data, obs_data_t *settings);
extern struct obs_source_frame *
gstreamer_filter_filter_video(void *data, struct obs_source_frame *frame);
struct obs_audio_data *
gstreamer_filter_filter_audio(void *p, struct obs_audio_data *audio_data);

bool obs_module_load(void)
{
	blog(LOG_INFO, "obs-gstreamer build: %s", obs_gstreamer_version);

	struct obs_source_info source_info = {
		.id = "gstreamer-source",
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO |
				OBS_SOURCE_DO_NOT_DUPLICATE,

		.get_name = gstreamer_source_get_name,
		.create = gstreamer_source_create,
		.destroy = gstreamer_source_destroy,

		.get_defaults = gstreamer_source_get_defaults,
		.get_properties = gstreamer_source_get_properties,
		.update = gstreamer_source_update,
		.show = gstreamer_source_show,
		.hide = gstreamer_source_hide,
	};

	obs_register_source(&source_info);

	struct obs_encoder_info encoder_info = {
		.id = "gstreamer-encoder",
		.type = OBS_ENCODER_VIDEO,
		.codec = "h264",

		.get_name = gstreamer_encoder_get_name,
		.create = gstreamer_encoder_create,
		.destroy = gstreamer_encoder_destroy,

		.encode = gstreamer_encoder_encode,

		.get_defaults = gstreamer_encoder_get_defaults,
		.get_properties = gstreamer_encoder_get_properties,
		.update = gstreamer_encoder_update,

		.get_extra_data = gstreamer_encoder_get_extra_data,
	};

	obs_register_encoder(&encoder_info);

	struct obs_source_info filter_info_video = {
		.id = "gstreamer-filter-video",
		.type = OBS_SOURCE_TYPE_FILTER,
		.output_flags = OBS_SOURCE_ASYNC_VIDEO,

		.get_name = gstreamer_filter_get_name_video,
		.create = gstreamer_filter_create,
		.destroy = gstreamer_filter_destroy,

		.get_defaults = gstreamer_filter_get_defaults_video,
		.get_properties = gstreamer_filter_get_properties,
		.update = gstreamer_filter_update,

		.filter_video = gstreamer_filter_filter_video,
	};

	obs_register_source(&filter_info_video);

	struct obs_source_info filter_info_audio = {
		.id = "gstreamer-filter-audio",
		.type = OBS_SOURCE_TYPE_FILTER,
		.output_flags = OBS_SOURCE_AUDIO,

		.get_name = gstreamer_filter_get_name_audio,
		.create = gstreamer_filter_create,
		.destroy = gstreamer_filter_destroy,

		.get_defaults = gstreamer_filter_get_defaults_audio,
		.get_properties = gstreamer_filter_get_properties,
		.update = gstreamer_filter_update,

		.filter_audio = gstreamer_filter_filter_audio,
	};

	obs_register_source(&filter_info_audio);

	gst_init(NULL, NULL);

	return true;
}
