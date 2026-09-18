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
  meson_version : '>=0.63.0',
  version : '0.4.2',
  default_options : [
    'prefix=~/.config/obs-studio/plugins/obs-gstreamer/bin/64bit',
    'libdir=',
  ],
)

obs_dep = dependency('libobs', required : false)
if not obs_dep.found()
  obs_dep = meson.get_compiler('c').find_library('obs', has_headers : 'obs/obs.h', required : get_option('require-libobs'))
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
  gstreamer_core_options = [
    'tests=disabled',
    'examples=disabled',
    'benchmarks=disabled',
    'tools=disabled',
    'introspection=disabled',
    'nls=disabled',
    'doc=disabled',
    'extra-checks=disabled',
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
  subproject('libffi')
  subproject('glib', default_options : [
    'tests=disabled',
  ])
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
  link_args: get_option('link-args').split(),
  install : true,
)
