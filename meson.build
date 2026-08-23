# Simplified meson.build for custom Windows builds
project('obs-gstreamer', 'c')

# Force Meson to pass empty, dummy dependency wrappers
obs_dep = declare_dependency()
gst_dep = declare_dependency()
gst_video_dep = declare_dependency()
gst_audio_dep = declare_dependency()
gst_app_dep = declare_dependency()
gst_net_dep = declare_dependency()

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
    fallback : 'custom-build'
  ),
  name_prefix : '',
  name_suffix : name_suffix,
  dependencies : [
    obs_dep,
    gst_dep,
    gst_video_dep,
    gst_audio_dep,
    gst_app_dep,
    gst_net_dep,
  ],
  install : true,
)
