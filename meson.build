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
    'libdir='
  ],
)

obs_dep = dependency('libobs', required : false)
if not obs_dep.found()
  obs_dep = meson.get_compiler('c').find_library('obs', has_headers : 'obs/obs.h')
endif

name_suffix = []
if host_machine.system() == 'darwin'
  name_suffix = 'so'
endif

gst_deps = [
  dependency('gstreamer-1.0', version : '>=1.16.0'),
  dependency('gstreamer-video-1.0'),
  dependency('gstreamer-audio-1.0'),
  dependency('gstreamer-app-1.0'),
  dependency('gstreamer-net-1.0'),
]

link_deps = [obs_dep] + gst_deps
extra_link_args = []

if host_machine.system() == 'windows'
  # obs-gstreamer.dll must be loadable by OBS even when the GStreamer/GLib
  # runtime it links against isn't resolvable yet (OBS 32.2 dropped %PATH%
  # from DLL search, see gstreamer.c's configure_gstreamer_windows_paths()).
  # Take compile args from pkg-config as usual, but link against pre-generated
  # delay-import archives (docker/gen-delayimp-libs.sh) instead of the normal
  # eagerly-loaded import libraries.
  gst_compile_deps = []
  foreach d : gst_deps
    gst_compile_deps += d.partial_dependency(compile_args : true, includes : true)
  endforeach
  link_deps = [obs_dep] + gst_compile_deps

  delayimp_dir = get_option('gst_delayimp_dir')
  if delayimp_dir == ''
    error('gst_delayimp_dir must be set when building for Windows (see docker/gen-delayimp-libs.sh)')
  endif

  foreach name : ['libgstreamer-1.0-0', 'libgobject-2.0-0', 'libglib-2.0-0', 'libintl-8',
                   'libgstvideo-1.0-0', 'libgstbase-1.0-0', 'libgstaudio-1.0-0', 'libgsttag-1.0-0',
                   'libgstapp-1.0-0', 'libgstnet-1.0-0', 'libgio-2.0-0']
    extra_link_args += [delayimp_dir / 'lib' + name + '_delay.a']
  endforeach

  extra_link_args += ['-ldelayimp']
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
  dependencies : link_deps,
  link_args : extra_link_args,
  install : true,
)
