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

#include <stdio.h>
#include <obs/obs.h>
#include <obs/obs-nix-platform.h>
#include <wayland-client.h>
#include <assert.h>

int main()
{
    struct wl_display *display = wl_display_connect(NULL);
    assert(display != NULL);

    blog(LOG_INFO, "OBS Version: %s", obs_get_version_string());

    obs_set_nix_platform(OBS_NIX_PLATFORM_WAYLAND);
    obs_set_nix_platform_display(display);

    obs_startup("en-US", NULL, NULL);

    obs_module_t *module;

    int res = obs_open_module(&module, "/usr/local/lib/obs-plugins/obs-gstreamer.so", NULL);
    assert(res == MODULE_SUCCESS);
    obs_init_module(module);

    res = obs_open_module(&module, "/usr/local/lib/obs-plugins/obs-ffmpeg.so", NULL);
    assert(res == MODULE_SUCCESS);
    obs_init_module(module);

    obs_post_load_modules();

    struct obs_video_info video_info = {
        .graphics_module = "libobs-opengl",
        .fps_num = 30,
        .fps_den = 1,
        .base_width = 960,
        .base_height = 540,
        .output_width = 960,
        .output_height = 540,
        .output_format = VIDEO_FORMAT_NV12,
        .adapter = 0,
        .gpu_conversion = true,
        .colorspace = VIDEO_CS_709,
        .range = VIDEO_RANGE_PARTIAL,
        .scale_type = OBS_SCALE_BILINEAR,
    };

    struct obs_audio_info audio_info = {
        .samples_per_sec = 48000,
        .speakers = SPEAKERS_STEREO,
    };

    obs_reset_video(&video_info);
    obs_reset_audio(&audio_info);

    const char *tmp;

    blog(LOG_INFO, "Input Types:");
    for (int i = 0; obs_enum_input_types(i, &tmp); i++)
    {
        blog(LOG_INFO, "  %s", tmp);
    }

    blog(LOG_INFO, "Filter Types:");
    for (int i = 0; obs_enum_filter_types(i, &tmp); i++)
    {
        blog(LOG_INFO, "  %s", tmp);
    }

    blog(LOG_INFO, "Output Types:");
    for (int i = 0; obs_enum_output_types(i, &tmp); i++)
    {
        blog(LOG_INFO, "  %s", tmp);
    }

    blog(LOG_INFO, "Encoder Types:");
    for (int i = 0; obs_enum_encoder_types(i, &tmp); i++)
    {
        blog(LOG_INFO, "  %s", tmp);
    }

    obs_source_t *source = obs_source_create("gstreamer-source", "source", NULL, NULL);
    obs_source_t *filter_video = obs_source_create("gstreamer-filter-video", "video filter", NULL, NULL);
    obs_source_t *filter_audio = obs_source_create("gstreamer-filter-audio", "audio filter", NULL, NULL);
    obs_encoder_t *encoder_video = obs_video_encoder_create("gstreamer-encoder", "encoder_video", NULL, NULL);
    obs_encoder_t *encoder_audio = obs_audio_encoder_create("ffmpeg_aac", "encoder_audio", NULL, 0, NULL);
    obs_output_t *output = obs_output_create("gstreamer-output", "output", NULL, NULL);

    obs_source_filter_add(source, filter_video);
    obs_source_filter_add(source, filter_audio);
    obs_set_output_source(0, source);
    obs_encoder_set_video(encoder_video, obs_get_video());
    obs_encoder_set_audio(encoder_audio, obs_get_audio());
    obs_output_set_video_encoder(output, encoder_video);
    obs_output_set_audio_encoder(output, encoder_audio, 0);
    obs_output_start(output);

    blog(LOG_INFO, "---------------------------------");
    blog(LOG_INFO, "Running. Press ENTER to stop.");
    getchar();

    obs_output_stop(output);

    obs_output_release(output);
    obs_encoder_release(encoder_video);
    obs_encoder_release(encoder_audio);
    obs_source_release(filter_video);
    obs_source_release(filter_audio);
    obs_source_release(source);

    obs_shutdown();

    wl_display_disconnect(display);

    return 0;
}
