# What is this ?

This is a modified and somewhat stripped-down version of `gst-play-1.0` tool in [GStreamer](https://gitlab.freedesktop.org/gstreamer).

Added functionality:
- playback rate command-line option
- always opens in full-screen mode
- dbus interface for commands

Removed functionality:
- keyboard and mouse input
- option to use playbin3
- i18n

# But why ?

We needed a simple player where we could adjust the playback rate from the command-line and that would always open in full-screen mode.

# How do I compile it ?

Make sure you have [GStreamer](https://gitlab.freedesktop.org/gstreamer), [xcb](https://xcb.freedesktop.org/), [dbus](https://github.com/freedesktop/dbus) and [gio](https://gitlab.gnome.org/GNOME/glib/) installed. Then run this in your command-line:

```
gcc gst-play-mod.c -o gst-play-mod-1.0 `pkg-config --cflags --libs gstreamer-1.0 gstreamer-audio-1.0 gstreamer-video-1.0 xcb dbus-1 dbus-glib-1 gio-2.0` -lm
```

or simply

```
make
```

# What is the license ?

This is licensed under the same license as the [GStreamer](https://gitlab.freedesktop.org/gstreamer) version it was based on - [LGPLv2](https://www.gnu.org/licenses/old-licenses/lgpl-2.0.html).

Also see the `COPYING` file in this repository.
