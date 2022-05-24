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

project('obs-gstreamer', 'c')

obs_dep = dependency('libobs', required : false)
if not obs_dep.found()
  obs_dep = meson.get_compiler('c').find_library('obs', has_headers : 'obs/obs.h')
endif

name_suffix = []
if host_machine.system() == 'darwin'
  name_suffix = 'so'
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
  name_suffix : name_suffix,
  dependencies : [
    obs_dep,
    dependency('gstreamer-1.0', version : '>=1.16.0'),
    dependency('gstreamer-video-1.0'),
    dependency('gstreamer-audio-1.0'),
    dependency('gstreamer-app-1.0'),
  ],
  install : true,
  install_dir : join_paths(get_option('libdir'), 'obs-plugins'),
)
