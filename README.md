GStreamer OBS Studio plugin
===

1. An OBS Studio source plugin to feed GStreamer launch pipelines into [OBS
Studio].

This plugin has interesting use cases but may be difficult to understand and is
clunky use if you are _not_ familiar with GStreamer.

2. An OBS Studio encoder plugin to use GStreamer encoder elements into [OBS Studio].

This may be interesting for people trying to run OBS Studio to different platforms like the RaspberryPi or NVIDIA Tegra.

Prebuilt
---

Experimental prebuilt 64-bit Windows plugin is available. You still require the
official [GStreamer run-time] to be installed.

Experimental prebuilt macOS plugin available. You still require the GStreamer
run-time installed via [Homebrew] or [Macports].

Experimental prebuilt Linux plugin is available. You still require the GStreamer
run-time installed via your Linux ditribution's package manager.

[OBS Studio]: https://obsproject.com/
[GStreamer run-time]: https://gstreamer.freedesktop.org/data/pkg/windows/
[Homebrew]: https://brew.sh/
[Macports]: https://www.macports.org/

Usage
---

The source plugin makes use of the GStreamer launch pipeline descriptions. Please
refer to the GStreamer documentation to understand what this means:

https://gstreamer.freedesktop.org/documentation/tools/gst-launch.html

This plugins provides two media sinks named `video` and `audio`. These are the
media sinks that hand over data to OBS Studio. So your pipeline should connect
to these sinks.

An example pipeline:

    videotestsrc is-live=true ! video/x-raw, framerate=30/1, width=960, height=540 ! video. audiotestsrc wave=ticks is-live=true ! audio/x-raw, channels=2, rate=44100 ! audio.

RTMP example:

    uridecodebin uri=rtmp://184.72.239.149/vod/mp4:bigbuckbunny_1500.mp4 name=bin ! queue ! video. bin. ! queue ! audio.

RTSP example:

    uridecodebin uri=rtsp://184.72.239.149/vod/mp4:BigBuckBunny_115k.mov name=bin ! queue ! video. bin. ! queue ! audio.

HLS example:

    uridecodebin uri=http://184.72.239.149:1935/vod/mp4:sample.mp4/playlist.m3u8 name=bin ! queue ! video. bin. ! queue ! audio.

Linux webcam example:

    v4l2src ! decodebin ! video.

Linux webcam example with watchdog (automatically restarts the pipeline if the webcam stream crashes for some reason):

    v4l2src ! watchdog ! decodebin ! video.

If you don't understand what is happening in these lines please check the
GStreamer documentation as mentioned above!


Build
---

```shell
$ meson --buildtype=release build
$ ninja -C build

# optional for installing the plugin
$ sudo ninja -C build install
```

It will install into the obs-plugins directory inside whatever libdir in meson is set to.

E.g.
```shell
meson --buildtype=release --libdir=lib
```
will install at /usr/local/lib/obs-plugins.


If you want it to install outside of /usr/local you will have to set a prefix as well.

E.g.
```shell
meson --buildtype=release --libdir=lib --prefix=/usr
```
You can also make it install in your user home directory (wherever that directory was exactly..)
