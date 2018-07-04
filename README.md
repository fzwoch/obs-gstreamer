GStreamer OBS Studio plugin
===

An OBS Studio source plugin to feed GStreamer launch pipelines into [OBS
Studio][1].

This plugin has interesting use cases but may be difficult to understand and is
clunky use if you are _not_ familiar with GStreamer.

No binaries provided due to too many platforms potentially being supported plus
the plugin needs to be compiled against the major version of the GStreamer
installation on the target system anyway.

[1]: https://obsproject.com

Usage
---

This plugin makes use of the GStreamer launch pipeline descriptions. Please
refer to the GStreamer documentation to understand what this means:

https://gstreamer.freedesktop.org/documentation/tools/gst-launch.html

This plugins provides two media sinks named `video` and `audio`. These are the
media sinks that hand over data to OBS Studio. So your pipeline should connect
to these sinks.

An example pipeline:

    videotestsrc is-live=true ! video/x-raw, framerate=30/1, width=960, height=540 ! video. audiotestsrc wave=ticks is-live=true ! audio.

Another one:

    uridecodebin uri=rtmp://184.72.239.149/vod/mp4:bigbuckbunny_1500.mp4 name=bin ! queue ! video. bin. ! queue ! audio.

If you don't understand what is happening in this line please check the
GStreamer documentation as mentioned above!


Build
---

```shell
$ meson --buildtype=release build
$ ninja -C build

# optional for installing the plugin
$ sudo ninja -C build install
```
