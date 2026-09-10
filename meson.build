#
# obs-gstreamer. OBS Studio plugin.
# Copyright (C) 2018-2021 Florian Zwoch <fzwoch@gmail.com>
#
# This file is part of obs-gstreamer.
#
# obs-gstreamer is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 of the License, or
# (at your option) any later version.
#
# obs-gstreamer is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with obs-gstreamer. If not, see <http://www.gnu.org/licenses/>.
#

project('obs-gstreamer', 'c',
  default_options : [
    'prefix=~/.config/obs-studio/plugins/obs-gstreamer/bin/64bit',
    'libdir=',
  ],
)

obs_dep = dependency('libobs', required : false)
if not obs_dep.found()
#  obs_dep = meson.get_compiler('c').find_library('obs', has_headers : 'obs/obs.h')
endif

gstreamer_dependencies = []
if get_option('system-gstreamer')
  gstreamer_dependencies += [
    dependency('gstreamer-1.0', version : '>=1.16.0'),
    dependency('gstreamer-video-1.0'),
    dependency('gstreamer-audio-1.0'),
    dependency('gstreamer-app-1.0'),
    dependency('gstreamer-net-1.0'),
  ]
else
  # Build GStreamer from source tarballs as meson subprojects
  # (subprojects/*.wrap) instead of using the system installation. Only the
  # minimum needed to build this plugin is enabled: gstreamer core and
  # gst-plugins-base (which provide gstreamer-1.0, gstreamer-net-1.0,
  # gstreamer-video-1.0, gstreamer-audio-1.0 and gstreamer-app-1.0).
  gstreamer_core_options = [
    'tests=disabled',
    'examples=disabled',
    'benchmarks=disabled',
    'tools=disabled',
    'introspection=disabled',
    'nls=disabled',
    'doc=disabled',
    'extra-checks=disabled',
    # Not used by this plugin, and would require a Rust toolchain
    # matching the target when cross compiling.
    'ptp-helper=disabled',
    'bash-completion=disabled',
    'libunwind=disabled',
    'libdw=disabled',
  ]
  gstreamer_base_options = [
    'tests=disabled',
    'examples=disabled',
    'tools=disabled',
    'introspection=disabled',
    'nls=disabled',
    'doc=disabled',
    'qt5=disabled',
    # Disable everything that requires external libraries. The
    # dependency-free plugins (appsrc/appsink, converters, scalers, mixers,
    # ...) remain enabled.
    'alsa=disabled',
    'cdparanoia=disabled',
    'drm=disabled',
    'gl=disabled',
    'iso-codes=disabled',
    'libvisual=disabled',
    'ogg=disabled',
    'opus=disabled',
    'pango=disabled',
    'theora=disabled',
    'tremor=disabled',
    'vorbis=disabled',
    'x11=disabled',
    'xshm=disabled',
    'xvideo=disabled',
    'xi=disabled',
  ]
  # Vendor glib first: it is built from source (subprojects/glib.wrap) and
  # registers meson.override_dependency() for glib-2.0, gobject-2.0, gio-2.0,
  # ... so that GStreamer (and this plugin) never link against the system
  # glib. Required for cross compilation where no system libraries are
  # available.
  #
  # Note that allow_fallback ensures none of the dependencies below resolve
  # against the system installation.
  subproject('glib')
  subproject('gstreamer', default_options : gstreamer_core_options)
  subproject('gst-plugins-base', default_options : gstreamer_base_options)
  gstreamer_dependencies += [
    dependency('gstreamer-1.0', version : '>=1.16.0', allow_fallback : true),
    dependency('gstreamer-video-1.0', allow_fallback : true),
    dependency('gstreamer-audio-1.0', allow_fallback : true),
    dependency('gstreamer-app-1.0', allow_fallback : true),
    dependency('gstreamer-net-1.0', allow_fallback : true),
  ]
endif

shared_library('obs-gstreamer',
  'gstreamer.c',
  'gstreamer-source.c',
  'gstreamer-encoder.c',
  'gstreamer-filter.c',
  'gstreamer-output.c',
  vcs_tag(
    command : ['git', 'rev-parse', '--short', 'HEAD'],
    input : 'version.c.in',
    output : 'version.c',
  ),
  name_prefix : '',
  dependencies : [obs_dep] + gstreamer_dependencies,
  link_args: get_option('link-args'),
  install : true,
)
