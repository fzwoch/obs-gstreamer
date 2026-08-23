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
#include <string.h>

#include "plugin-i18n.h"

extern const char *obs_gstreamer_version;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-gstreamer", "en-US")

/*
 * Embedded English fallbacks, used when the locale file cannot be consulted
 * (e.g. when the plugin binary is loaded without its data directory during
 * development). Keys must mirror data/locale/en-US.ini.
 */
static const struct {
	const char *key;
	const char *text;
} english_fallbacks[] = {
	{"status.stopped", "Pipeline stopped"},
	{"status.opening", "Opening pipeline..."},
	{"status.buffering", "Buffering..."},
	{"status.running", "Pipeline running"},
	{"status.paused", "Pipeline paused"},
	{"status.ended", "Pipeline ended"},
	{"filter.ready", "Filter ready"},
	{"err.cannot_start", "Cannot start pipeline:"},
	{"err.invalid_pipeline", "Invalid pipeline:"},
	{"suggest.didyoumean", "Did you mean"},
	{"suggest.or", "or"},
	{"group.pipeline", "Pipeline"},
	{"group.video", "Video"},
	{"group.audio", "Audio"},
	{"group.behavior", "Behavior"},
	{"group.network", "Network clock (advanced)"},
	{"template.label", "Template"},
	{"template.none", "(choose a template...)"},
	{"template.desc",
	 "Picking an entry fills the pipeline text below; adjust addresses and options as needed."},
	{"preset.test", "Test pattern (video + audio)"},
	{"preset.rtsp", "RTSP camera"},
	{"preset.srt", "SRT listener"},
	{"preset.webcam", "Webcam via V4L2"},
	{"preset.x11", "Screen capture (X11)"},
	{"pipeline.label", "Pipeline"},
	{"pipeline.desc",
	 "Use \"video\" and \"audio\" as names for the media sinks, e.g.: v4l2src device=/dev/video0 ! videoconvert ! video. / pulsesrc ! audioconvert ! audio."},
	{"timestamps.video", "Use pipeline time stamps (video)"},
	{"timestamps.audio", "Use pipeline time stamps (audio)"},
	{"sync.video", "Sync appsink to clock (video)"},
	{"sync.audio", "Sync appsink to clock (audio)"},
	{"noasync.video", "Disable asynchronous state change in appsink (video)"},
	{"noasync.audio", "Disable asynchronous state change in appsink (audio)"},
	{"block.video", "Limit video sink buffer to 1 frame"},
	{"block.video.desc",
	 "Restricts the appsink queue to a single buffer. Reduces latency at the cost of dropping frames when processing is slower than the source."},
	{"drop.video", "Drop late video frames"},
	{"drop.audio", "Drop late audio buffers"},
	{"drop.generic.desc", "Drop older buffers when the sink is not fast enough."},
	{"block.audio", "Limit audio sink buffer to 1 frame"},
	{"block.audio.desc", "See video option; applies to audio."},
	{"restart.eos", "Try to restart when end of stream is reached"},
	{"restart.error", "Try to restart after pipeline encountered an error"},
	{"restart.timeout", "Error timeout (ms)"},
	{"stop.on_hide", "Stop pipeline when hidden"},
	{"resume.position", "Resume playback position when re-shown"},
	{"resume.position.desc",
	 "When the pipeline is stopped on hide, remember the playback position and seek back there when the source is shown again. Only applies to seekable streams."},
	{"clear.on_end", "Clear image data after end-of-stream or error"},
	{"no_buffer", "Disable buffering in OBS"},
	{"latency.label", "Fixed latency (ms)"},
	{"latency.desc",
	 "Sets a fixed latency for the pipeline for syncing different inputs. Check the error log for clock errors if the set latency is too low. Setting 0 auto-detects the lowest possible latency for the given pipeline."},
	{"verbose.log", "Verbose pipeline logging"},
	{"verbose.log.desc",
	 "Log QoS events, element state changes and custom bus messages to the OBS log file while this source is active. Useful for debugging pipelines."},
	{"ntp.server", "NTP server"},
	{"ntp.server.desc",
	 "Sets an NTP server for syncing the GStreamer clock to. Use e.g. with rtspsrc rfc7273-sync or ntp-sync options. Leave empty to not use an NTP server; if the server cannot be reached the pipeline continues without it."},
	{"ntp.port", "NTP server port"},
	{"apply", "Apply"},
	{"filter.pipeline.desc",
	 "Use \"identity\" for passthru. Note: changing resolution or sample rate inside the filter is not supported."},
	{"output.pipeline.desc", "Use \"video\" and \"audio\" as names for the media sources."},
};

const char *T(const char *key)
{
	const char *text = NULL;

	if (obs_module_get_string(key, &text))
		return text;

	for (size_t i = 0; i < sizeof(english_fallbacks) / sizeof(english_fallbacks[0]); i++) {
		if (strcmp(english_fallbacks[i].key, key) == 0)
			return english_fallbacks[i].text;
	}

	return key;
}

// gstreamer-source.c
extern const char *gstreamer_source_get_name(void *type_data);
extern void *gstreamer_source_create(obs_data_t *settings, obs_source_t *source);
extern void gstreamer_source_destroy(void *data);
extern void gstreamer_source_get_defaults(obs_data_t *settings);
extern obs_properties_t *gstreamer_source_get_properties(void *data);
extern void gstreamer_source_update(void *data, obs_data_t *settings);
extern void gstreamer_source_show(void *data);
extern void gstreamer_source_hide(void *data);
extern enum obs_media_state gstreamer_source_get_state(void *data);
extern int64_t gstreamer_source_get_time(void *data);
extern int64_t gstreamer_source_get_duration(void *data);
extern void gstreamer_source_play_pause(void *data, bool pause);
extern void gstreamer_source_stop(void *data);
extern void gstreamer_source_restart(void *data);
extern void gstreamer_source_set_time(void *data, int64_t ms);

// gstreamer-encoder.c
extern const char *gstreamer_encoder_get_name_h264(void *type_data);
extern const char *gstreamer_encoder_get_name_h265(void *type_data);
extern void *gstreamer_encoder_create_h264(obs_data_t *settings, obs_encoder_t *encoder);
extern void *gstreamer_encoder_create_h265(obs_data_t *settings, obs_encoder_t *encoder);
extern void gstreamer_encoder_destroy(void *data);
extern bool gstreamer_encoder_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet,
				     bool *received_packet);
extern void gstreamer_encoder_get_defaults_h264(obs_data_t *settings);
extern void gstreamer_encoder_get_defaults_h265(obs_data_t *settings);
extern obs_properties_t *gstreamer_encoder_get_properties_h264(void *data);
extern obs_properties_t *gstreamer_encoder_get_properties_h265(void *data);
extern bool gstreamer_encoder_get_extra_data(void *data, uint8_t **extra_data, size_t *size);

// gstreamer-filter.c
extern const char *gstreamer_filter_get_name_video(void *type_data);
extern const char *gstreamer_filter_get_name_audio(void *type_data);
extern void *gstreamer_filter_create(obs_data_t *settings, obs_source_t *source);
extern void gstreamer_filter_destroy(void *data);
extern void gstreamer_filter_get_defaults_video(obs_data_t *settings);
extern void gstreamer_filter_get_defaults_audio(obs_data_t *settings);
extern obs_properties_t *gstreamer_filter_get_properties(void *data);
extern void gstreamer_filter_update(void *data, obs_data_t *settings);
extern struct obs_source_frame *gstreamer_filter_filter_video(void *data, struct obs_source_frame *frame);
struct obs_audio_data *gstreamer_filter_filter_audio(void *p, struct obs_audio_data *audio_data);

// gstreamer-output.c
extern const char *gstreamer_output_get_name(void *type_data);
extern void *gstreamer_output_create(obs_data_t *settings, obs_output_t *output);
extern void gstreamer_output_destroy(void *data);
extern bool gstreamer_output_start(void *data);
extern void gstreamer_output_stop(void *data, uint64_t ts);
extern void gstreamer_output_encoded_packet(void *data, struct encoder_packet *packet);
extern void gstreamer_output_get_defaults(obs_data_t *settings);
extern obs_properties_t *gstreamer_output_get_properties(void *data);

bool obs_module_load(void)
{
	guint major, minor, micro, nano;

	gst_version(&major, &minor, &micro, &nano);

	blog(LOG_INFO, "[obs-gstreamer] build: %s, gst-runtime: %u.%u.%u", obs_gstreamer_version, major, minor, micro);

	struct obs_source_info source_info = {
		.id = "gstreamer-source",
		.type = OBS_SOURCE_TYPE_INPUT,
		.icon_type = OBS_ICON_TYPE_MEDIA,
		.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE |
				OBS_SOURCE_CONTROLLABLE_MEDIA,
		.get_name = gstreamer_source_get_name,
		.create = gstreamer_source_create,
		.destroy = gstreamer_source_destroy,

		.get_defaults = gstreamer_source_get_defaults,
		.get_properties = gstreamer_source_get_properties,
		.update = gstreamer_source_update,
		.show = gstreamer_source_show,
		.hide = gstreamer_source_hide,

		.media_get_state = gstreamer_source_get_state,
		.media_get_time = gstreamer_source_get_time,
		.media_get_duration = gstreamer_source_get_duration,

		.media_play_pause = gstreamer_source_play_pause,
		.media_stop = gstreamer_source_stop,
		.media_restart = gstreamer_source_restart,
		.media_set_time = gstreamer_source_set_time,
	};

	obs_register_source(&source_info);

	struct obs_encoder_info encoder_info_h264 = {
		.id = "gstreamer-encoder-h264",
		.type = OBS_ENCODER_VIDEO,
		.codec = "h264",

		.caps = OBS_ENCODER_CAP_DEPRECATED,

		.get_name = gstreamer_encoder_get_name_h264,
		.create = gstreamer_encoder_create_h264,
		.destroy = gstreamer_encoder_destroy,

		.encode = gstreamer_encoder_encode,

		.get_defaults = gstreamer_encoder_get_defaults_h264,
		.get_properties = gstreamer_encoder_get_properties_h264,

		.get_extra_data = gstreamer_encoder_get_extra_data,
	};

	obs_register_encoder(&encoder_info_h264);

	struct obs_encoder_info encoder_info_h265 = {
		.id = "gstreamer-encoder-h265",
		.type = OBS_ENCODER_VIDEO,
		.codec = "hevc",

		.caps = OBS_ENCODER_CAP_DEPRECATED,

		.get_name = gstreamer_encoder_get_name_h265,
		.create = gstreamer_encoder_create_h265,
		.destroy = gstreamer_encoder_destroy,

		.encode = gstreamer_encoder_encode,

		.get_defaults = gstreamer_encoder_get_defaults_h265,
		.get_properties = gstreamer_encoder_get_properties_h265,

		.get_extra_data = gstreamer_encoder_get_extra_data,
	};

	obs_register_encoder(&encoder_info_h265);

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

	struct obs_output_info output_info = {
		.id = "gstreamer-output",
		.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED,

		.get_name = gstreamer_output_get_name,
		.create = gstreamer_output_create,
		.destroy = gstreamer_output_destroy,
		.start = gstreamer_output_start,
		.stop = gstreamer_output_stop,

		.encoded_packet = gstreamer_output_encoded_packet,

		.get_defaults = gstreamer_output_get_defaults,
		.get_properties = gstreamer_output_get_properties,
	};

	obs_register_output(&output_info);

	gst_init(NULL, NULL);

	return true;
}
