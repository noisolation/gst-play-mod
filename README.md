# What is this ?

This is a modified and somewhat stripped-down version of `gst-play-1.0` tool in [GStreamer](https://gitlab.freedesktop.org/gstreamer).

Added functionality:
- playback rate command-line option
- always opens in full-screen mode

Removed functionality:
- keyboard and mouse input
- option to use playbin3
- i18n

# But why ?

We needed a simple player where we could adjust the playback rate from the command-line and that would always open in full-screen mode.

# How do I compile it ?

Make sure you have [GStreamer](https://gitlab.freedesktop.org/gstreamer) and [xcb](https://xcb.freedesktop.org/) installed. Then run this in your command-line:

```
gcc gst-play-mod.c -o gst-play-mod-1.0 `pkg-config --cflags --libs gstreamer-1.0 gstreamer-pbutils-1.0 xcb` -lm
```

# What is the license ?

This is licensed under the same license as the [GStreamer](https://gitlab.freedesktop.org/gstreamer) version it was based on - [LGPLv2](https://www.gnu.org/licenses/old-licenses/lgpl-2.0.html).

Also see the `COPYING` file in this repository.
