/* Based on the GStreamer command line playback testing utility
 *
 * Copyright (C) 2013-2014 Tim-Philipp Müller <tim centricular net>
 * Copyright (C) 2013 Collabora Ltd.
 * Copyright (C) 2015 Centricular Ltd
 * Copyright (C) 2021 No Isolation AS
 *
 * This application is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This application is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <gst/audio/audio.h>
#include <gst/video/video.h>
#include <gst/math-compat.h>
#include <xcb/xcb.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib/gprintf.h>

#include <dbus/dbus.h>
#include <dbus/dbus-glib-lowlevel.h>

#define PROGRAM_NAME "gst-play-mod"
#define VERSION_STRING "1.0"

#define VOLUME_STEPS 20

GST_DEBUG_CATEGORY (play_debug);
#define GST_CAT_DEFAULT play_debug

#define INTERFACE_NAME "com.noisolation.MediaPlayer"
#define OBJECT_PATH "/com/noisolation/MediaPlayer"

typedef enum
{
  GST_PLAY_TRICK_MODE_NONE = 0,
  GST_PLAY_TRICK_MODE_DEFAULT,
  GST_PLAY_TRICK_MODE_DEFAULT_NO_AUDIO,
  GST_PLAY_TRICK_MODE_KEY_UNITS,
  GST_PLAY_TRICK_MODE_KEY_UNITS_NO_AUDIO,
  GST_PLAY_TRICK_MODE_LAST
} GstPlayTrickMode;

typedef enum
{
  GST_PLAY_TRACK_TYPE_INVALID = 0,
  GST_PLAY_TRACK_TYPE_AUDIO,
  GST_PLAY_TRACK_TYPE_VIDEO,
  GST_PLAY_TRACK_TYPE_SUBTITLE
} GstPlayTrackType;

typedef struct
{
  gchar **uris;
  guint num_uris;
  gint cur_idx;

  GstElement *playbin;

  GstStreamCollection *collection;
  gchar *cur_audio_sid;
  gchar *cur_video_sid;
  gchar *cur_text_sid;
  GMutex selection_lock;

  GMainLoop *loop;
  guint bus_watch;
  guint timeout;

  gboolean buffering;
  gboolean is_live;

  GstState desired_state;

  gulong deep_notify_id;

  /* configuration */
  gboolean gapless;

  GstPlayTrickMode trick_mode;
  gdouble rate;
} GstPlay;

static gboolean quiet = FALSE;

static gboolean play_bus_msg (GstBus * bus, GstMessage * msg, gpointer data);
static gboolean play_next (GstPlay * play);
static gboolean play_prev (GstPlay * play);
static gboolean play_timeout (gpointer user_data);
static void play_about_to_finish (GstElement * playbin, gpointer user_data);
static void play_reset (GstPlay * play);
static void play_set_relative_volume (GstPlay * play, gdouble volume_step);
static void play_set_playback_rate (GstPlay * play, gdouble rate);
static void play_set_relative_playback_rate (GstPlay * play, gdouble rate_step, gboolean reverse_direction);
static void play_switch_trick_mode (GstPlay * play);
static void play_cycle_track_selection (GstPlay * play, GstPlayTrackType track_type);
static void play_toggle_audio_mute (GstPlay * play);
static gboolean play_do_seek (GstPlay * play, gint64 pos, gdouble rate, GstPlayTrickMode mode);
static void toggle_paused (GstPlay * play);
static void relative_seek (GstPlay * play, gdouble percent);

/* *INDENT-OFF* */
static void gst_play_printf (const gchar * format, ...) G_GNUC_PRINTF (1, 2);
/* *INDENT-ON* */

static DBusHandlerResult
server_message_handler (DBusConnection *conn, DBusMessage *message, void *user_data)
{
  DBusError err;
  GstPlay *play = (GstPlay *) user_data;
  gboolean quit = FALSE;

  gst_printerr ("Got D-Bus request: %s.%s on %s\n",
    dbus_message_get_interface (message),
    dbus_message_get_member (message),
    dbus_message_get_path (message));

  dbus_error_init (&err);

  if (dbus_message_is_signal (message, INTERFACE_NAME, "Quit")) {
    quit = TRUE;
  } else if (dbus_message_is_signal (message, INTERFACE_NAME, "TogglePaused")) {
    toggle_paused (play);
  } else if (dbus_message_is_signal (message, INTERFACE_NAME, "PlayNext")) {
    if (!play_next (play)) {
      gst_print ("\n%s\n", "Reached end of play list.");
      quit = TRUE;
    }
  } else if (dbus_message_is_signal (message, INTERFACE_NAME, "PlayPrevious")) {
    play_prev (play);
  } else if (dbus_message_is_signal (message, INTERFACE_NAME, "IncreasePlaybackRate")) {
    if (play->rate > -0.2 && play->rate < 0.0)
      play_set_relative_playback_rate (play, 0.0, TRUE);
    else if (ABS (play->rate) < 2.0)
      play_set_relative_playback_rate (play, 0.1, FALSE);
    else if (ABS (play->rate) < 4.0)
      play_set_relative_playback_rate (play, 0.5, FALSE);
    else
      play_set_relative_playback_rate (play, 1.0, FALSE);
  } else if (dbus_message_is_signal (message, INTERFACE_NAME, "DecreasePlaybackRate")) {
    if (play->rate > 0.0 && play->rate < 0.20)
      play_set_relative_playback_rate (play, 0.0, TRUE);
    else if (ABS (play->rate) <= 2.0)
      play_set_relative_playback_rate (play, -0.1, FALSE);
    else if (ABS (play->rate) <= 4.0)
      play_set_relative_playback_rate (play, -0.5, FALSE);
    else
      play_set_relative_playback_rate (play, -1.0, FALSE);
  } else if (dbus_message_is_signal (message, INTERFACE_NAME, "ChangePlaybackDirection")) {
    play_set_relative_playback_rate (play, 0.0, TRUE);
  } else if (dbus_message_is_signal (message, INTERFACE_NAME, "ToggleTrickMode")) {
    play_switch_trick_mode (play);
  } else if (dbus_message_is_signal (message, INTERFACE_NAME, "ChangeAudioTrack")) {
    play_cycle_track_selection (play, GST_PLAY_TRACK_TYPE_AUDIO);
  } else if (dbus_message_is_signal (message, INTERFACE_NAME, "ChangeVideoTrack")) {
    play_cycle_track_selection (play, GST_PLAY_TRACK_TYPE_VIDEO);
  } else if (dbus_message_is_signal (message, INTERFACE_NAME, "ChangeSubtitleTrack")) {
    play_cycle_track_selection (play, GST_PLAY_TRACK_TYPE_SUBTITLE);
  } else if (dbus_message_is_signal (message, INTERFACE_NAME, "SeekToBeginning")) {
    play_do_seek (play, 0, play->rate, play->trick_mode);
  } else if (dbus_message_is_signal (message, INTERFACE_NAME, "ToggleAudioMute")) {
    play_toggle_audio_mute (play);
  } else if (dbus_message_is_signal (message, INTERFACE_NAME, "IncreaseAudioVolume")) {
    play_set_relative_volume (play, +1.0 / VOLUME_STEPS);
  } else if (dbus_message_is_signal (message, INTERFACE_NAME, "DecreaseAudioVolume")) {
    play_set_relative_volume (play, -1.0 / VOLUME_STEPS);
  } else if (dbus_message_is_signal (message, INTERFACE_NAME, "SeekRight")) {
    relative_seek (play, +0.08);
  } else if (dbus_message_is_signal (message, INTERFACE_NAME, "SeekLeft")) {
    relative_seek (play, +0.08);
	} else {
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }

  if (dbus_error_is_set (&err)) {
    dbus_error_free (&err);
  }

  if (quit) {
      gst_printerr ("Server exiting...\n");
      g_main_loop_quit (play->loop);
  }

  return DBUS_HANDLER_RESULT_HANDLED;
}

static gboolean
server_setup(GstPlay *play)
{
  DBusConnection *connection;
  DBusError error;
  int rv;

  dbus_error_init (&error);

  connection = dbus_bus_get(DBUS_BUS_SESSION, &error);
  if (!connection) {
    gst_printerr ("Failed to get a session DBus connection: %s\n", error.message);
    dbus_error_free (&error);
    return FALSE;
  }

  rv = dbus_bus_request_name(connection, INTERFACE_NAME, DBUS_NAME_FLAG_REPLACE_EXISTING , &error);
  if (rv != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
    gst_printerr ("Failed to request name on bus: %s\n", error.message);
    dbus_error_free (&error);
    return FALSE;
  }

  dbus_bus_add_match (connection, "type='signal',interface='com.noisolation.MediaPlayer'", &error);
  if (dbus_error_is_set (&error)) {
    gst_printerr ("Failed to add match: %s\n", error.message);
    dbus_error_free (&error);
    return FALSE;
  }

  if (!dbus_connection_add_filter (connection, server_message_handler, play, NULL)) {
    gst_printerr ("Failed to add filter: %s\n", error.message);
    dbus_error_free (&error);
    return FALSE;
  }

  dbus_connection_setup_with_g_main (connection, NULL);

  return TRUE;
}

static void
client_send_signal(const char * signal)
{
  DBusConnection *connection;
  DBusMessage *message;
  DBusError error;

  dbus_error_init (&error);

  connection = dbus_bus_get (DBUS_BUS_SESSION, &error);

  if (!connection) {
    gst_printerr (" Failed to connect to the D-BUS daemon: %s", error.message);
    dbus_error_free (&error);
    return;
  }

  message = dbus_message_new_signal (OBJECT_PATH, INTERFACE_NAME, signal);
  if (!message) {
    gst_printerr("Error creating DBus message\n");
    dbus_connection_unref(connection);
    return;
  }

  if (!dbus_connection_send (connection, message, NULL)) {
    gst_printerr (" Failed to send signal");
    dbus_error_free (&error);
    dbus_message_unref (message);
    dbus_connection_unref (connection);
    return;
  }

  dbus_connection_flush (connection);
  dbus_message_unref (message);
  dbus_connection_unref (connection);

  gst_print (" Signal sendt: %s\n", signal);
}

static void
create_window (GstBus * bus, GstMessage * message, GstPipeline * pipeline)
{
  xcb_connection_t *connection;
  xcb_screen_t     *screen;
  xcb_window_t     window;
  uint32_t         values[2];

  connection = xcb_connect (NULL, NULL);
  screen = xcb_setup_roots_iterator (xcb_get_setup (connection)).data;
  window = xcb_generate_id(connection);

  values[0] = screen->black_pixel;
  values[1] = XCB_EVENT_MASK_EXPOSURE;

  xcb_create_window (connection, XCB_COPY_FROM_PARENT, window, screen->root,
    0, 0, screen->width_in_pixels, screen->height_in_pixels, 0,
    XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
    XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);

  xcb_map_window (connection, window);
  xcb_flush (connection);

  gst_video_overlay_set_window_handle (
    GST_VIDEO_OVERLAY (GST_MESSAGE_SRC (message)), window);
}

static void
gst_play_printf (const gchar * format, ...)
{
  gchar *str = NULL;
  va_list args;
  int len;

  if (quiet)
    return;

  va_start (args, format);

  len = g_vasprintf (&str, format, args);

  va_end (args);

  if (len > 0 && str != NULL)
    gst_print ("%s", str);

  g_free (str);
}

#define gst_print gst_play_printf

static GstPlay *
play_new (gchar ** uris, const gchar * audio_sink, const gchar * video_sink,
    gboolean gapless, gdouble initial_volume, gdouble initial_rate, gboolean verbose,
    const gchar * flags_string)
{
  GstElement *sink, *playbin;
  GstPlay *play;

  playbin = gst_element_factory_make ("playbin", "playbin");

  if (playbin == NULL)
    return NULL;

  play = g_new0 (GstPlay, 1);

  play->uris = uris;
  play->num_uris = g_strv_length (uris);
  play->cur_idx = -1;

  play->playbin = playbin;

  g_mutex_init (&play->selection_lock);

  if (audio_sink != NULL) {
    if (strchr (audio_sink, ' ') != NULL)
      sink = gst_parse_bin_from_description (audio_sink, TRUE, NULL);
    else
      sink = gst_element_factory_make (audio_sink, NULL);

    if (sink != NULL)
      g_object_set (play->playbin, "audio-sink", sink, NULL);
    else
      g_warning ("Couldn't create specified audio sink '%s'", audio_sink);
  }
  if (video_sink != NULL) {
    if (strchr (video_sink, ' ') != NULL)
      sink = gst_parse_bin_from_description (video_sink, TRUE, NULL);
    else
      sink = gst_element_factory_make (video_sink, NULL);

    if (sink != NULL)
      g_object_set (play->playbin, "video-sink", sink, NULL);
    else
      g_warning ("Couldn't create specified video sink '%s'", video_sink);
  }

  if (flags_string != NULL) {
    GParamSpec *pspec;
    GValue val = { 0, };

    pspec =
        g_object_class_find_property (G_OBJECT_GET_CLASS (playbin), "flags");
    g_value_init (&val, pspec->value_type);
    if (gst_value_deserialize (&val, flags_string))
      g_object_set_property (G_OBJECT (play->playbin), "flags", &val);
    else
      gst_printerr ("Couldn't convert '%s' to playbin flags!\n", flags_string);
    g_value_unset (&val);
  }

  if (verbose) {
    play->deep_notify_id =
        gst_element_add_property_deep_notify_watch (play->playbin, NULL, TRUE);
  }

  play->loop = g_main_loop_new (NULL, FALSE);

  play->bus_watch = gst_bus_add_watch (GST_ELEMENT_BUS (play->playbin),
      play_bus_msg, play);

  /* FIXME: make configurable incl. 0 for disable */
  play->timeout = g_timeout_add (100, play_timeout, play);

  play->buffering = FALSE;
  play->is_live = FALSE;

  play->desired_state = GST_STATE_PLAYING;

  play->gapless = gapless;
  if (gapless) {
    g_signal_connect (play->playbin, "about-to-finish",
        G_CALLBACK (play_about_to_finish), play);
  }

  if (initial_volume != -1)
    play_set_relative_volume (play, initial_volume - 1.0);

  play->rate = 1.0;
  play->trick_mode = GST_PLAY_TRICK_MODE_NONE;

  if (initial_rate != -1)
    play->rate = initial_rate;

  return play;
}

static void
play_free (GstPlay * play)
{
  /* No need to see all those pad caps going to NULL etc., it's just noise */
  if (play->deep_notify_id != 0)
    g_signal_handler_disconnect (play->playbin, play->deep_notify_id);

  play_reset (play);

  gst_element_set_state (play->playbin, GST_STATE_NULL);
  gst_object_unref (play->playbin);

  g_source_remove (play->bus_watch);
  g_source_remove (play->timeout);
  g_main_loop_unref (play->loop);

  g_strfreev (play->uris);

  if (play->collection)
    gst_object_unref (play->collection);
  g_free (play->cur_audio_sid);
  g_free (play->cur_video_sid);
  g_free (play->cur_text_sid);

  g_mutex_clear (&play->selection_lock);

  g_free (play);
}

/* reset for new file/stream */
static void
play_reset (GstPlay * play)
{
  play->buffering = FALSE;
  play->is_live = FALSE;
}

static void
play_set_relative_volume (GstPlay * play, gdouble volume_step)
{
  gdouble volume;

  volume = gst_stream_volume_get_volume (GST_STREAM_VOLUME (play->playbin),
      GST_STREAM_VOLUME_FORMAT_CUBIC);

  volume = round ((volume + volume_step) * VOLUME_STEPS) / VOLUME_STEPS;
  volume = CLAMP (volume, 0.0, 10.0);

  gst_stream_volume_set_volume (GST_STREAM_VOLUME (play->playbin),
      GST_STREAM_VOLUME_FORMAT_CUBIC, volume);

  gst_print ("Volume: %.0f%%", volume * 100);
  gst_print ("                  \n");
}

static void
play_toggle_audio_mute (GstPlay * play)
{
  gboolean mute;

  mute = gst_stream_volume_get_mute (GST_STREAM_VOLUME (play->playbin));

  mute = !mute;
  gst_stream_volume_set_mute (GST_STREAM_VOLUME (play->playbin), mute);

  if (mute)
    gst_print ("Mute: on");
  else
    gst_print ("Mute: off");
  gst_print ("                  \n");
}

static gboolean
play_bus_msg (GstBus * bus, GstMessage * msg, gpointer user_data)
{
  GstPlay *play = user_data;

  if (gst_is_video_overlay_prepare_window_handle_message (msg))
    create_window (bus, msg, GST_PIPELINE (play->playbin));

  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_ASYNC_DONE:

      /* dump graph on preroll */
      GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (play->playbin),
          GST_DEBUG_GRAPH_SHOW_ALL, "gst-play.async-done");

      gst_print ("Prerolled.\r");
      break;
    case GST_MESSAGE_BUFFERING:{
      gint percent;

      if (!play->buffering)
        gst_print ("\n");

      gst_message_parse_buffering (msg, &percent);
      gst_print ("%s %d%%  \r", "Buffering...", percent);

      if (percent == 100) {
        /* a 100% message means buffering is done */
        if (play->buffering) {
          play->buffering = FALSE;
          /* no state management needed for live pipelines */
          if (!play->is_live)
            gst_element_set_state (play->playbin, GST_STATE_PLAYING);
        }
      } else {
        /* buffering... */
        if (!play->buffering) {
          if (!play->is_live)
            gst_element_set_state (play->playbin, GST_STATE_PAUSED);
          play->buffering = TRUE;
        }
      }
      break;
    }
    case GST_MESSAGE_CLOCK_LOST:{
      gst_print ("Clock lost, selecting a new one\n");
      gst_element_set_state (play->playbin, GST_STATE_PAUSED);
      gst_element_set_state (play->playbin, GST_STATE_PLAYING);
      break;
    }
    case GST_MESSAGE_LATENCY:
      gst_print ("Redistribute latency...\n");
      gst_bin_recalculate_latency (GST_BIN (play->playbin));
      break;
    case GST_MESSAGE_REQUEST_STATE:{
      GstState state;
      gchar *name;

      name = gst_object_get_path_string (GST_MESSAGE_SRC (msg));

      gst_message_parse_request_state (msg, &state);

      gst_print ("Setting state to %s as requested by %s...\n",
          gst_element_state_get_name (state), name);

      gst_element_set_state (play->playbin, state);
      g_free (name);
      break;
    }
    case GST_MESSAGE_EOS:
      /* print final position at end */
      play_timeout (play);
      gst_print ("\n");
      /* and switch to next item in list */
      if (!play_next (play)) {
        gst_print ("%s\n", "Reached end of play list.");
        g_main_loop_quit (play->loop);
      }
      break;
    case GST_MESSAGE_WARNING:{
      GError *err;
      gchar *dbg = NULL;

      /* dump graph on warning */
      GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (play->playbin),
          GST_DEBUG_GRAPH_SHOW_ALL, "gst-play.warning");

      gst_message_parse_warning (msg, &err, &dbg);
      gst_printerr ("WARNING %s\n", err->message);
      if (dbg != NULL)
        gst_printerr ("WARNING debug information: %s\n", dbg);
      g_clear_error (&err);
      g_free (dbg);
      break;
    }
    case GST_MESSAGE_ERROR:{
      GError *err;
      gchar *dbg;

      /* dump graph on error */
      GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (play->playbin),
          GST_DEBUG_GRAPH_SHOW_ALL, "gst-play.error");

      gst_message_parse_error (msg, &err, &dbg);
      gst_printerr ("ERROR %s for %s\n", err->message,
          play->uris[play->cur_idx]);
      if (dbg != NULL)
        gst_printerr ("ERROR debug information: %s\n", dbg);
      g_clear_error (&err);
      g_free (dbg);

      /* flush any other error messages from the bus and clean up */
      gst_element_set_state (play->playbin, GST_STATE_NULL);

      /* try next item in list then */
      if (!play_next (play)) {
        gst_print ("%s\n", "Reached end of play list.");
        g_main_loop_quit (play->loop);
      }
      break;
    }

    case GST_MESSAGE_PROPERTY_NOTIFY:{
      const GValue *val;
      const gchar *name;
      GstObject *obj;
      gchar *val_str = NULL;
      gchar *obj_name;

      gst_message_parse_property_notify (msg, &obj, &name, &val);

      obj_name = gst_object_get_path_string (GST_OBJECT (obj));
      if (val != NULL) {
        if (G_VALUE_HOLDS_STRING (val))
          val_str = g_value_dup_string (val);
        else if (G_VALUE_TYPE (val) == GST_TYPE_CAPS)
          val_str = gst_caps_to_string (g_value_get_boxed (val));
        else if (G_VALUE_TYPE (val) == GST_TYPE_TAG_LIST)
          val_str = gst_tag_list_to_string (g_value_get_boxed (val));
        else
          val_str = gst_value_serialize (val);
      } else {
        val_str = g_strdup ("(no value)");
      }

      gst_play_printf ("%s: %s = %s\n", obj_name, name, val_str);
      g_free (obj_name);
      g_free (val_str);
      break;
    }
    case GST_MESSAGE_STREAM_COLLECTION:
    {
      GstStreamCollection *collection = NULL;
      gst_message_parse_stream_collection (msg, &collection);

      if (collection) {
        g_mutex_lock (&play->selection_lock);
        gst_object_replace ((GstObject **) & play->collection,
            (GstObject *) collection);
        g_mutex_unlock (&play->selection_lock);
      }
      break;
    }
    case GST_MESSAGE_STREAMS_SELECTED:
    {
      GstStreamCollection *collection = NULL;
      guint i, len;

      gst_message_parse_streams_selected (msg, &collection);
      if (collection) {
        g_mutex_lock (&play->selection_lock);
        gst_object_replace ((GstObject **) & play->collection,
            (GstObject *) collection);

        /* Free all last stream-ids */
        g_free (play->cur_audio_sid);
        g_free (play->cur_video_sid);
        g_free (play->cur_text_sid);
        play->cur_audio_sid = NULL;
        play->cur_video_sid = NULL;
        play->cur_text_sid = NULL;

        len = gst_message_streams_selected_get_size (msg);
        for (i = 0; i < len; i++) {
          GstStream *stream = gst_message_streams_selected_get_stream (msg, i);
          if (stream) {
            GstStreamType type = gst_stream_get_stream_type (stream);
            const gchar *stream_id = gst_stream_get_stream_id (stream);

            if (type & GST_STREAM_TYPE_AUDIO) {
              play->cur_audio_sid = g_strdup (stream_id);
            } else if (type & GST_STREAM_TYPE_VIDEO) {
              play->cur_video_sid = g_strdup (stream_id);
            } else if (type & GST_STREAM_TYPE_TEXT) {
              play->cur_text_sid = g_strdup (stream_id);
            } else {
              gst_print ("Unknown stream type with stream-id %s", stream_id);
            }
            gst_object_unref (stream);
          }
        }

        gst_object_unref (collection);
        g_mutex_unlock (&play->selection_lock);
      }
      break;
    }
    default:
      break;
  }

  return TRUE;
}

static gboolean
play_timeout (gpointer user_data)
{
  GstPlay *play = user_data;
  gint64 pos = -1, dur = -1;
  const gchar *paused = "Paused";
  gchar *status;

  if (play->buffering)
    return TRUE;

  gst_element_query_position (play->playbin, GST_FORMAT_TIME, &pos);
  gst_element_query_duration (play->playbin, GST_FORMAT_TIME, &dur);

  gint len = g_utf8_strlen (paused, -1);
  status = g_newa (gchar, len + 1);
  memset (status, ' ', len);
  status[len] = '\0';

  if (pos >= 0 && dur > 0) {
    gchar dstr[32], pstr[32];

    /* FIXME: pretty print in nicer format */
    g_snprintf (pstr, 32, "%" GST_TIME_FORMAT, GST_TIME_ARGS (pos));
    pstr[9] = '\0';
    g_snprintf (dstr, 32, "%" GST_TIME_FORMAT, GST_TIME_ARGS (dur));
    dstr[9] = '\0';
    gst_print ("%s / %s %s\r", pstr, dstr, status);
  }

  return TRUE;
}

static gchar *
play_uri_get_display_name (GstPlay * play, const gchar * uri)
{
  gchar *loc;

  if (gst_uri_has_protocol (uri, "file")) {
    loc = g_filename_from_uri (uri, NULL, NULL);
  } else if (gst_uri_has_protocol (uri, "pushfile")) {
    loc = g_filename_from_uri (uri + 4, NULL, NULL);
  } else {
    loc = g_strdup (uri);
  }

  /* Maybe additionally use glib's filename to display name function */
  return loc;
}

static void
play_uri (GstPlay * play, const gchar * next_uri)
{
  gchar *loc;

  gst_element_set_state (play->playbin, GST_STATE_READY);
  play_reset (play);

  loc = play_uri_get_display_name (play, next_uri);
  gst_print ("Now playing %s\n", loc);
  g_free (loc);

  g_object_set (play->playbin, "uri", next_uri, NULL);

  switch (gst_element_set_state (play->playbin, GST_STATE_PAUSED)) {
    case GST_STATE_CHANGE_FAILURE:
      /* ignore, we should get an error message posted on the bus */
      break;
    case GST_STATE_CHANGE_NO_PREROLL:
      gst_print ("Pipeline is live.\n");
      play->is_live = TRUE;
      break;
    case GST_STATE_CHANGE_ASYNC:
      gst_print ("Prerolling...\r");
      break;
    default:
      break;
  }

  gst_element_set_state (play->playbin, GST_STATE_PLAYING);

  gst_print ("checking state...\r");
  GstState cur_state;
  do {
    gst_element_get_state (play->playbin, &cur_state, NULL, 0);
  } while (cur_state <= GST_STATE_READY);
  gst_print ("setting playback rate to %f...\r", play->rate);
  play_set_playback_rate (play, play->rate);
}

/* returns FALSE if we have reached the end of the playlist */
static gboolean
play_next (GstPlay * play)
{
  if ((play->cur_idx + 1) >= play->num_uris)
    return FALSE;

  play_uri (play, play->uris[++play->cur_idx]);
  return TRUE;
}

/* returns FALSE if we have reached the beginning of the playlist */
static gboolean
play_prev (GstPlay * play)
{
  if (play->cur_idx == 0 || play->num_uris <= 1)
    return FALSE;

  play_uri (play, play->uris[--play->cur_idx]);
  return TRUE;
}

static void
play_about_to_finish (GstElement * playbin, gpointer user_data)
{
  GstPlay *play = user_data;
  const gchar *next_uri;
  gchar *loc;
  guint next_idx;

  if (!play->gapless)
    return;

  next_idx = play->cur_idx + 1;
  if (next_idx >= play->num_uris)
    return;

  next_uri = play->uris[next_idx];
  loc = play_uri_get_display_name (play, next_uri);
  gst_print ("About to finish, preparing next title: %s", loc);
  gst_print ("\n");
  g_free (loc);

  g_object_set (play->playbin, "uri", next_uri, NULL);
  play->cur_idx = next_idx;
}

static void
do_play (GstPlay * play)
{
  gint i;

  /* dump playlist */
  for (i = 0; i < play->num_uris; ++i)
    GST_INFO ("%4u : %s", i, play->uris[i]);

  if (!play_next (play))
    return;

  g_main_loop_run (play->loop);
}

static gint
compare (gconstpointer a, gconstpointer b)
{
  gchar *a1, *b1;
  gint ret;

  a1 = g_utf8_collate_key_for_filename ((gchar *) a, -1);
  b1 = g_utf8_collate_key_for_filename ((gchar *) b, -1);
  ret = strcmp (a1, b1);
  g_free (a1);
  g_free (b1);

  return ret;
}

static void
add_to_playlist (GPtrArray * playlist, const gchar * filename)
{
  GDir *dir;
  gchar *uri;

  if (gst_uri_is_valid (filename)) {
    g_ptr_array_add (playlist, g_strdup (filename));
    return;
  }

  if ((dir = g_dir_open (filename, 0, NULL))) {
    const gchar *entry;
    GList *l, *files = NULL;

    while ((entry = g_dir_read_name (dir))) {
      gchar *path;

      path = g_build_filename (filename, entry, NULL);
      files = g_list_insert_sorted (files, path, compare);
    }

    g_dir_close (dir);

    for (l = files; l != NULL; l = l->next) {
      gchar *path = (gchar *) l->data;

      add_to_playlist (playlist, path);
      g_free (path);
    }
    g_list_free (files);
    return;
  }

  uri = gst_filename_to_uri (filename, NULL);
  if (uri != NULL)
    g_ptr_array_add (playlist, uri);
  else
    g_warning ("Could not make URI out of filename '%s'", filename);
}

static void
shuffle_uris (gchar ** uris, guint num)
{
  gchar *tmp;
  guint i, j;

  if (num < 2)
    return;

  for (i = num - 1; i >= 1; i--) {
    /* +1 because number returned will be in range [a;b[ so excl. stop */
    j = g_random_int_range (0, i + 1);
    tmp = uris[j];
    uris[j] = uris[i];
    uris[i] = tmp;
  }
}

static void
toggle_paused (GstPlay * play)
{
  if (play->desired_state == GST_STATE_PLAYING)
    play->desired_state = GST_STATE_PAUSED;
  else
    play->desired_state = GST_STATE_PLAYING;

  if (!play->buffering) {
    gst_element_set_state (play->playbin, play->desired_state);
  } else if (play->desired_state == GST_STATE_PLAYING) {
    gst_print ("\nWill play as soon as buffering finishes)\n");
  }
}

static void
relative_seek (GstPlay * play, gdouble percent)
{
  GstQuery *query;
  gboolean seekable = FALSE;
  gint64 dur = -1, pos = -1, step;

  g_return_if_fail (percent >= -1.0 && percent <= 1.0);

  if (!gst_element_query_position (play->playbin, GST_FORMAT_TIME, &pos))
    goto seek_failed;

  query = gst_query_new_seeking (GST_FORMAT_TIME);
  if (!gst_element_query (play->playbin, query)) {
    gst_query_unref (query);
    goto seek_failed;
  }

  gst_query_parse_seeking (query, NULL, &seekable, NULL, &dur);
  gst_query_unref (query);

  if (!seekable || dur <= 0)
    goto seek_failed;

  step = dur * percent;
  if (ABS (step) < GST_SECOND)
    step = (percent < 0) ? -GST_SECOND : GST_SECOND;

  pos = pos + step;
  if (pos > dur) {
    if (!play_next (play)) {
      gst_print ("\n%s\n", "Reached end of play list.");
      g_main_loop_quit (play->loop);
    }
  } else {
    if (pos < 0)
      pos = 0;

    play_do_seek (play, pos, play->rate, play->trick_mode);
  }

  return;

seek_failed:
  {
    gst_print ("\nCould not seek.\n");
  }
}

static gboolean
play_set_rate_and_trick_mode (GstPlay * play, gdouble rate,
    GstPlayTrickMode mode)
{
  gint64 pos = -1;

  g_return_val_if_fail (rate != 0, FALSE);

  if (!gst_element_query_position (play->playbin, GST_FORMAT_TIME, &pos))
    return FALSE;

  return play_do_seek (play, pos, rate, mode);
}

static gboolean
play_do_seek (GstPlay * play, gint64 pos, gdouble rate, GstPlayTrickMode mode)
{
  GstSeekFlags seek_flags;
  GstQuery *query;
  GstEvent *seek;
  gboolean seekable = FALSE;

  query = gst_query_new_seeking (GST_FORMAT_TIME);
  if (!gst_element_query (play->playbin, query)) {
    gst_query_unref (query);
    return FALSE;
  }

  gst_query_parse_seeking (query, NULL, &seekable, NULL, NULL);
  gst_query_unref (query);

  if (!seekable)
    return FALSE;

  seek_flags = GST_SEEK_FLAG_FLUSH;

  switch (mode) {
    case GST_PLAY_TRICK_MODE_DEFAULT:
      seek_flags |= GST_SEEK_FLAG_TRICKMODE;
      break;
    case GST_PLAY_TRICK_MODE_DEFAULT_NO_AUDIO:
      seek_flags |= GST_SEEK_FLAG_TRICKMODE | GST_SEEK_FLAG_TRICKMODE_NO_AUDIO;
      break;
    case GST_PLAY_TRICK_MODE_KEY_UNITS:
      seek_flags |= GST_SEEK_FLAG_TRICKMODE_KEY_UNITS;
      break;
    case GST_PLAY_TRICK_MODE_KEY_UNITS_NO_AUDIO:
      seek_flags |=
          GST_SEEK_FLAG_TRICKMODE_KEY_UNITS | GST_SEEK_FLAG_TRICKMODE_NO_AUDIO;
      break;
    case GST_PLAY_TRICK_MODE_NONE:
    default:
      break;
  }

  if (rate >= 0)
    seek = gst_event_new_seek (rate, GST_FORMAT_TIME,
        seek_flags | GST_SEEK_FLAG_ACCURATE,
        /* start */ GST_SEEK_TYPE_SET, pos,
        /* stop */ GST_SEEK_TYPE_SET, GST_CLOCK_TIME_NONE);
  else
    seek = gst_event_new_seek (rate, GST_FORMAT_TIME,
        seek_flags | GST_SEEK_FLAG_ACCURATE,
        /* start */ GST_SEEK_TYPE_SET, 0,
        /* stop */ GST_SEEK_TYPE_SET, pos);

  if (!gst_element_send_event (play->playbin, seek))
    return FALSE;

  play->rate = rate;
  play->trick_mode = mode;
  return TRUE;
}

static void
play_set_playback_rate (GstPlay * play, gdouble rate)
{
  if (play_set_rate_and_trick_mode (play, rate, play->trick_mode)) {
    gst_print ("Playback rate: %.2f", rate);
    gst_print ("                               \n");
  } else {
    gst_print ("\n");
    gst_print ("Could not change playback rate to %.2f", rate);
    gst_print (".\n");
  }
}

static void
play_set_relative_playback_rate (GstPlay * play, gdouble rate_step,
    gboolean reverse_direction)
{
  gdouble new_rate = play->rate + rate_step;

  if (reverse_direction)
    new_rate *= -1.0;

  play_set_playback_rate (play, new_rate);
}

static const gchar *
trick_mode_get_description (GstPlayTrickMode mode)
{
  switch (mode) {
    case GST_PLAY_TRICK_MODE_NONE:
      return "normal playback, trick modes disabled";
    case GST_PLAY_TRICK_MODE_DEFAULT:
      return "trick mode: default";
    case GST_PLAY_TRICK_MODE_DEFAULT_NO_AUDIO:
      return "trick mode: default, no audio";
    case GST_PLAY_TRICK_MODE_KEY_UNITS:
      return "trick mode: key frames only";
    case GST_PLAY_TRICK_MODE_KEY_UNITS_NO_AUDIO:
      return "trick mode: key frames only, no audio";
    default:
      break;
  }
  return "unknown trick mode";
}

static GstPlayTrickMode
play_next_trick_mode (GstPlayTrickMode trick_mode)
{
  switch (trick_mode) {
  case GST_PLAY_TRICK_MODE_NONE:
    return GST_PLAY_TRICK_MODE_DEFAULT;
  case GST_PLAY_TRICK_MODE_DEFAULT:
    return GST_PLAY_TRICK_MODE_DEFAULT_NO_AUDIO;
  case GST_PLAY_TRICK_MODE_DEFAULT_NO_AUDIO:
    return GST_PLAY_TRICK_MODE_KEY_UNITS;
  case GST_PLAY_TRICK_MODE_KEY_UNITS:
    return GST_PLAY_TRICK_MODE_KEY_UNITS_NO_AUDIO;
  case GST_PLAY_TRICK_MODE_KEY_UNITS_NO_AUDIO:
    return GST_PLAY_TRICK_MODE_LAST;
  case GST_PLAY_TRICK_MODE_LAST:
    break;
  }
  return GST_PLAY_TRICK_MODE_NONE;
}

static void
play_switch_trick_mode (GstPlay * play)
{
  GstPlayTrickMode new_mode;
  const gchar *mode_desc;

  play->trick_mode = play_next_trick_mode( play->trick_mode );
  new_mode = play->trick_mode;

  if (new_mode == GST_PLAY_TRICK_MODE_LAST)
    new_mode = GST_PLAY_TRICK_MODE_NONE;

  mode_desc = trick_mode_get_description (new_mode);

  if (play_set_rate_and_trick_mode (play, play->rate, new_mode)) {
    gst_print ("Rate: %.2f (%s)                      \n", play->rate,
        mode_desc);
  } else {
    gst_print ("\nCould not change trick mode to %s.\n", mode_desc);
  }
}

static void
play_cycle_track_selection (GstPlay * play, GstPlayTrackType track_type)
{
  const gchar *prop_cur, *prop_n, *prop_get, *name;
  gint cur = -1, n = -1;
  guint flag, cur_flags;

  /* playbin3 variables */
  GList *selected_streams = NULL;

  switch (track_type) {
    case GST_PLAY_TRACK_TYPE_AUDIO:
      prop_get = "get-audio-tags";
      prop_cur = "current-audio";
      prop_n = "n-audio";
      name = "audio";
      flag = 0x2;
      break;
    case GST_PLAY_TRACK_TYPE_VIDEO:
      prop_get = "get-video-tags";
      prop_cur = "current-video";
      prop_n = "n-video";
      name = "video";
      flag = 0x1;
      break;
    case GST_PLAY_TRACK_TYPE_SUBTITLE:
      prop_get = "get-text-tags";
      prop_cur = "current-text";
      prop_n = "n-text";
      name = "subtitle";
      flag = 0x4;
      break;
    default:
      return;
  }

  g_object_get (play->playbin, prop_cur, &cur, prop_n, &n, "flags",
      &cur_flags, NULL);

  if (!(cur_flags & flag))
    cur = 0;
  else
    cur = (cur + 1) % (n + 1);

  if (n < 1) {
    gst_print ("No %s tracks.\n", name);
    g_mutex_unlock (&play->selection_lock);
  } else {
    gchar *lcode = NULL, *lname = NULL;
    const gchar *lang = NULL;
    GstTagList *tags = NULL;

    if (cur >= n && track_type != GST_PLAY_TRACK_TYPE_VIDEO) {
      cur = -1;
      gst_print ("Disabling %s.           \n", name);
      if (cur_flags & flag) {
        cur_flags &= ~flag;
        g_object_set (play->playbin, "flags", cur_flags, NULL);
      }
    } else {
      /* For video we only want to switch between streams, not disable it altogether */
      if (cur >= n)
        cur = 0;

      if (!(cur_flags & flag) && track_type != GST_PLAY_TRACK_TYPE_VIDEO) {
        cur_flags |= flag;
        g_object_set (play->playbin, "flags", cur_flags, NULL);
      }
      g_signal_emit_by_name (play->playbin, prop_get, cur, &tags);

      if (tags != NULL) {
        if (gst_tag_list_get_string (tags, GST_TAG_LANGUAGE_NAME, &lname))
          lang = lname;
        gst_tag_list_unref (tags);
      }
      if (lang != NULL)
        gst_print ("Switching to %s track %d of %d (%s).\n", name, cur + 1, n,
            lang);
      else
        gst_print ("Switching to %s track %d of %d.\n", name, cur + 1, n);
    }
    g_free (lcode);
    g_free (lname);
    g_mutex_unlock (&play->selection_lock);

    g_object_set (play->playbin, prop_cur, cur, NULL);
  }

  if (selected_streams)
    g_list_free (selected_streams);
}

int
main (int argc, char **argv)
{
  GstPlay *play;
  GPtrArray *playlist;
  gboolean verbose = FALSE;
  gboolean print_version = FALSE;
  gboolean gapless = FALSE;
  gboolean shuffle = FALSE;
  gdouble volume = -1;
  gdouble rate = -1;
  gchar **filenames = NULL;
  gchar *audio_sink = NULL;
  gchar *video_sink = NULL;
  gchar **uris;
  gchar *flags = NULL;
  guint num, i;
  GError *err = NULL;
  GOptionContext *ctx;
  gchar *playlist_file = NULL;
  gchar *emit = NULL;
  GOptionEntry options[] = {
    {"verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose,
        "Output status information and property notifications", NULL},
    {"flags", 0, 0, G_OPTION_ARG_STRING, &flags,
          "Control playback behaviour setting playbin 'flags' property",
        NULL},
    {"version", 0, 0, G_OPTION_ARG_NONE, &print_version,
        "Print version information and exit", NULL},
    {"videosink", 0, 0, G_OPTION_ARG_STRING, &video_sink,
        "Video sink to use (default is autovideosink)", NULL},
    {"audiosink", 0, 0, G_OPTION_ARG_STRING, &audio_sink,
        "Audio sink to use (default is autoaudiosink)", NULL},
    {"gapless", 0, 0, G_OPTION_ARG_NONE, &gapless,
        "Enable gapless playback", NULL},
    {"shuffle", 0, 0, G_OPTION_ARG_NONE, &shuffle,
        "Shuffle playlist", NULL},
    {"volume", 0, 0, G_OPTION_ARG_DOUBLE, &volume,
        "Volume", NULL},
    {"rate", 0, 0, G_OPTION_ARG_DOUBLE, &rate,
        "Playback rate", NULL},
    {"playlist", 0, 0, G_OPTION_ARG_FILENAME, &playlist_file,
        "Playlist file containing input media files", NULL},
    {"quiet", 'q', 0, G_OPTION_ARG_NONE, &quiet,
        "Do not print any output (apart from errors)", NULL},
    {"emit", 0, 0, G_OPTION_ARG_STRING, &emit,
        "Emit a dbus signal (requires running player)", NULL},
    {G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &filenames, NULL},
    {NULL}
  };

  g_set_prgname (PROGRAM_NAME "-" VERSION_STRING);
  /* Ensure XInitThreads() is called if/when needed */
  g_setenv ("GST_GL_XINITTHREADS", "1", TRUE);

  ctx = g_option_context_new ("FILE1|URI1 [FILE2|URI2] [FILE3|URI3] ...");
  g_option_context_add_main_entries (ctx, options, NULL);
  g_option_context_add_group (ctx, gst_init_get_option_group ());
  if (!g_option_context_parse (ctx, &argc, &argv, &err)) {
    gst_print ("Error initializing: %s\n", GST_STR_NULL (err->message));
    g_option_context_free (ctx);
    g_clear_error (&err);
    return 1;
  }
  g_option_context_free (ctx);

  GST_DEBUG_CATEGORY_INIT (play_debug, "play", 0, g_get_prgname ());

  if (print_version) {
    gchar *version_str;

    version_str = gst_version_string ();
    gst_print ("%s version %s\n", g_get_prgname (), VERSION_STRING);
    gst_print ("%s\n", version_str);
    gst_print ("%s\n", "NoIS");
    g_free (version_str);

    g_free (audio_sink);
    g_free (video_sink);
    g_free (playlist_file);

    return 0;
  }

  /* DBus client */
  if (emit != NULL) {
    client_send_signal(emit);
    return 0;
  }

  playlist = g_ptr_array_new ();

  if (playlist_file != NULL) {
    gchar *playlist_contents = NULL;
    gchar **lines = NULL;

    if (g_file_get_contents (playlist_file, &playlist_contents, NULL, &err)) {
      lines = g_strsplit (playlist_contents, "\n", 0);
      num = g_strv_length (lines);

      for (i = 0; i < num; i++) {
        if (lines[i][0] != '\0') {
          GST_LOG ("Playlist[%d]: %s", i + 1, lines[i]);
          add_to_playlist (playlist, lines[i]);
        }
      }
      g_strfreev (lines);
      g_free (playlist_contents);
    } else {
      gst_printerr ("Could not read playlist: %s\n", err->message);
      g_clear_error (&err);
    }
    g_free (playlist_file);
    playlist_file = NULL;
  }

  if (playlist->len == 0 && (filenames == NULL || *filenames == NULL)) {
    gst_printerr ("Usage: %s FILE1|URI1 [FILE2|URI2] [FILE3|URI3] ...",
    g_get_prgname ());
    gst_printerr ("\n\n"),
    gst_printerr ("%s\n\n",
        "You must provide at least one filename or URI to play.");
    /* No input provided. Free array */
    g_ptr_array_free (playlist, TRUE);

    g_free (audio_sink);
    g_free (video_sink);

    return 1;
  }

  /* fill playlist */
  if (filenames != NULL && *filenames != NULL) {
    num = g_strv_length (filenames);
    for (i = 0; i < num; ++i) {
      GST_LOG ("command line argument: %s", filenames[i]);
      add_to_playlist (playlist, filenames[i]);
    }
    g_strfreev (filenames);
  }

  num = playlist->len;
  g_ptr_array_add (playlist, NULL);

  uris = (gchar **) g_ptr_array_free (playlist, FALSE);

  if (shuffle)
    shuffle_uris (uris, num);

  /* prepare */
  play = play_new (uris, audio_sink, video_sink, gapless, volume, rate, verbose, flags);

  if (play == NULL) {
    gst_printerr ("Failed to create 'playbin' element. Check your GStreamer installation.\n");
    return EXIT_FAILURE;
  }

  /* Set up the DBus server */
  if (!server_setup (play)) {
	  gst_printerr ("Failed to set up the DBus server.\n");
	  return EXIT_FAILURE;
  }

  /* play */
  do_play (play);

  /* clean up */
  play_free (play);

  g_free (audio_sink);
  g_free (video_sink);

  gst_print ("\n");
  gst_deinit ();
  return 0;
}
