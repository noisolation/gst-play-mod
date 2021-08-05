
CFLAGS_GSTREAMER = $(shell pkg-config --cflags --libs gstreamer-1.0 gstreamer-audio-1.0 gstreamer-video-1.0)
CFLAGS_XCB = $(shell pkg-config --cflags --libs xcb)
CFLAGS_DBUS = $(shell pkg-config --cflags --libs dbus-1)
CFLAGS_DBUS_GLIB = $(shell pkg-config --cflags --libs dbus-glib-1)

CFLAGS = -g -Wall -Werror -lm

VERSION = 1.0

all: gst-play-mod

gst-play-mod: gst-play-mod.c
	gcc $< -o $@-$(VERSION) $(CFLAGS) $(CFLAGS_GSTREAMER) $(CFLAGS_XCB) $(CFLAGS_DBUS) $(CFLAGS_DBUS_GLIB)

clean:
	rm -f gst-play-mod-$(VERSION)


.PHONY: all clean
