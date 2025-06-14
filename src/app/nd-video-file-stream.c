/*
 * Copyright 2025 Bardia Moshiri <bardia@furilabs.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "nd-video-file-stream.h"
#include <gst/base/base.h>

typedef struct _NdVideoFileStreamGlobal NdVideoFileStreamGlobal;

struct _NdVideoFileStreamGlobal {
  GstElement *current_playbin;
  GstElement *current_intervideosrc;
  GstElement *current_interaudiosrc;
  gulong video_probe_id;
  gulong audio_probe_id;
  gboolean pipeline_blocked;
  gdouble pause_position;
  guint seek_timeout_id;
};

struct _NdVideoFileStream {
  gchar *video_file_path;
  GstElement *playbin;
  GstElement *volume_element;
  GstElement *source_bin;
  gboolean is_playing;
  NdVideoFileStreamGlobal *global;
};

/* This is a very ugly hack. I can't figure out how to pause both playbin and inter*src
 * so this will poll and reset the location every second when paused
 */
static gboolean
seek_to_pause_position (gpointer user_data)
{
  NdVideoFileStream *stream = (NdVideoFileStream *)user_data;
  NdVideoFileStreamGlobal *global = stream->global;
  if (global->current_playbin && global->pause_position >= 0.0) {
    gint64 duration;
    if (gst_element_query_duration (global->current_playbin, GST_FORMAT_TIME, &duration) && duration > 0) {
      gint64 seek_pos = (gint64) (global->pause_position * duration);
      g_debug ("Periodic seek to pause position: %f", global->pause_position);
      gst_element_seek_simple (global->current_playbin, GST_FORMAT_TIME,
                               GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE,
                               seek_pos);
    }
  }
  return G_SOURCE_CONTINUE;
}

static GstPadProbeReturn
video_blocking_probe_cb (GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
  NdVideoFileStream *stream = (NdVideoFileStream *)user_data;
  if (stream->global->pipeline_blocked) {
    g_debug ("Video probe: Blocking buffer flow");
    return GST_PAD_PROBE_DROP;
  }
  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
audio_blocking_probe_cb (GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
  NdVideoFileStream *stream = (NdVideoFileStream *)user_data;
  if (stream->global->pipeline_blocked) {
    g_debug ("Audio probe: Blocking buffer flow");
    return GST_PAD_PROBE_DROP;
  }
  return GST_PAD_PROBE_OK;
}

NdVideoFileStream *
nd_video_file_stream_new (const gchar *video_file_path)
{
  NdVideoFileStream *stream;
  if (!video_file_path) {
    g_warning ("nd_video_file_stream_new: No video file path provided!");
    return NULL;
  }

  stream = g_new0 (NdVideoFileStream, 1);
  stream->video_file_path = g_strdup (video_file_path);
  stream->playbin = NULL;
  stream->volume_element = NULL;
  stream->source_bin = NULL;
  stream->is_playing = FALSE;

  stream->global = g_new0 (NdVideoFileStreamGlobal, 1);
  stream->global->pause_position = -1.0;
  return stream;
}

void
nd_video_file_stream_free (NdVideoFileStream *stream)
{
  if (!stream)
    return;

  if (stream->source_bin) {
    gst_element_set_state (stream->source_bin, GST_STATE_NULL);
    gst_object_unref (stream->source_bin);
  }

  /* Clean up seek timer */
  if (stream->global && stream->global->seek_timeout_id != 0) {
    g_source_remove (stream->global->seek_timeout_id);
    stream->global->seek_timeout_id = 0;
  }

  g_free (stream->video_file_path);
  g_free (stream->global);
  g_free (stream);
}

GstElement *
nd_video_file_stream_get_source (NdVideoFileStream *stream)
{
  g_return_val_if_fail (stream != NULL, NULL);

  if (!stream->source_bin) {
    stream->source_bin = nd_video_file_stream_create_source (stream->video_file_path);

    /* Store references to playbin for media controls */
    if (stream->source_bin) {
      GstIterator *iter = gst_bin_iterate_elements (GST_BIN (stream->source_bin));
      GValue item = G_VALUE_INIT;

      while (gst_iterator_next (iter, &item) == GST_ITERATOR_OK) {
        GstElement *element = g_value_get_object (&item);
        const gchar *element_name = GST_ELEMENT_NAME (element);

        g_debug ("Found element: %s", element_name);

        if (g_str_has_prefix (element_name, "video-player")) {
          stream->playbin = element;
          stream->global->current_playbin = element;
          g_debug ("Set global playbin reference: %s", GST_ELEMENT_NAME (element));
        } else if (g_str_has_prefix (element_name, "screencastsrc")) {
          stream->global->current_intervideosrc = element;

          /* Add blocking probe to intervideosrc */
          GstPad *srcpad = gst_element_get_static_pad (element, "src");
          if (srcpad) {
            stream->global->video_probe_id = gst_pad_add_probe (srcpad,
                                                                GST_PAD_PROBE_TYPE_BUFFER,
                                                                video_blocking_probe_cb,
                                                                stream, NULL);
            gst_object_unref (srcpad);
            g_debug ("Added video blocking probe");
          }
        }

        g_value_reset (&item);
      }

      g_value_unset (&item);
      gst_iterator_free (iter);

      /* Start playback immediately */
      if (stream->playbin) {
        gst_element_set_state (stream->playbin, GST_STATE_PLAYING);
        stream->is_playing = TRUE;
        stream->global->pipeline_blocked = FALSE;
      }
    }
  }

  return stream->source_bin ? gst_object_ref (stream->source_bin) : NULL;
}

void
nd_video_file_stream_play (NdVideoFileStream *stream)
{
  g_return_if_fail (stream != NULL);

  if (stream->playbin) {
    gst_element_set_state (stream->playbin, GST_STATE_PLAYING);
    stream->is_playing = TRUE;
    stream->global->pipeline_blocked = FALSE;
    g_debug ("Stream play: Unblocked pipeline and started playback");
  }
}

void
nd_video_file_stream_pause (NdVideoFileStream *stream)
{
  g_return_if_fail (stream != NULL);

  if (stream->playbin) {
    gst_element_set_state (stream->playbin, GST_STATE_PAUSED);
    stream->is_playing = FALSE;
    stream->global->pipeline_blocked = TRUE;
    g_debug ("Stream pause: Blocked pipeline and paused playback");
  }
}

gboolean
nd_video_file_stream_is_playing (NdVideoFileStream *stream)
{
  g_return_val_if_fail (stream != NULL, FALSE);

  if (stream->playbin) {
    GstState state;
    gst_element_get_state (stream->playbin, &state, NULL, 0);
    return state == GST_STATE_PLAYING && !stream->global->pipeline_blocked;
  }

  return stream->is_playing && !stream->global->pipeline_blocked;
}

void
nd_video_file_stream_seek (NdVideoFileStream *stream, gdouble position)
{
  g_return_if_fail (stream != NULL);
  g_return_if_fail (position >= 0.0 && position <= 1.0);

  if (stream->playbin) {
    gint64 duration = nd_video_file_stream_get_duration (stream);
    if (duration > 0) {
      gint64 seek_pos = (gint64) (position * duration);
      gst_element_seek_simple (stream->playbin, GST_FORMAT_TIME,
                               GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
                               seek_pos);
    }
  }
}

gdouble
nd_video_file_stream_get_position (NdVideoFileStream *stream)
{
  g_return_val_if_fail (stream != NULL, 0.0);

  if (stream->playbin) {
    gint64 position, duration;

    if (gst_element_query_position (stream->playbin, GST_FORMAT_TIME, &position) &&
        gst_element_query_duration (stream->playbin, GST_FORMAT_TIME, &duration) &&
        duration > 0)
      return (gdouble) position / duration;
  }

  return 0.0;
}

gint64
nd_video_file_stream_get_duration (NdVideoFileStream *stream)
{
  g_return_val_if_fail (stream != NULL, -1);

  if (stream->playbin) {
    gint64 duration;
    if (gst_element_query_duration (stream->playbin, GST_FORMAT_TIME, &duration))
      return duration;
  }

  return -1;
}

void
nd_video_file_stream_set_volume (NdVideoFileStream *stream, gdouble volume)
{
  g_return_if_fail (stream != NULL);
  g_return_if_fail (volume >= 0.0 && volume <= 1.0);

  if (stream->volume_element)
    g_object_set (stream->volume_element, "volume", volume, NULL);
  else if (stream->playbin)
    g_object_set (stream->playbin, "volume", volume, NULL);
}

gdouble
nd_video_file_stream_get_volume (NdVideoFileStream *stream)
{
  g_return_val_if_fail (stream != NULL, 1.0);

  gdouble volume = 1.0;

  if (stream->volume_element)
    g_object_get (stream->volume_element, "volume", &volume, NULL);
  else if (stream->playbin)
    g_object_get (stream->playbin, "volume", &volume, NULL);

  return volume;
}

void
nd_video_file_stream_global_play (NdVideoFileStream *stream)
{
  NdVideoFileStreamGlobal *global = stream->global;
  if (global->current_playbin && GST_IS_ELEMENT (global->current_playbin)) {
    /* Stop periodic seeking */
    if (global->seek_timeout_id != 0) {
      g_source_remove (global->seek_timeout_id);
      global->seek_timeout_id = 0;
      g_debug ("Stopped periodic seek timer");
    }

    /* Final seek to pause position if we have one */
    if (global->pause_position >= 0.0) {
      gint64 duration;
      if (gst_element_query_duration (global->current_playbin, GST_FORMAT_TIME, &duration) && duration > 0) {
        gint64 seek_pos = (gint64) (global->pause_position * duration);
        g_debug ("Final seek to pause position: %f", global->pause_position);
        gst_element_seek_simple (global->current_playbin, GST_FORMAT_TIME,
                                 GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE,
                                 seek_pos);
      }
      global->pause_position = -1.0; /* Reset */
    }

    /* Unblock pipeline */
    global->pipeline_blocked = FALSE;

    g_debug ("Global play: unblocked pipeline");
  } else {
    g_debug ("Global play: no valid playbin available");
  }
}

void
nd_video_file_stream_global_pause (NdVideoFileStream *stream)
{
  NdVideoFileStreamGlobal *global = stream->global;
  if (global->current_playbin && GST_IS_ELEMENT (global->current_playbin)) {
    /* Store current position */
    gint64 position, duration;
    if (gst_element_query_position (global->current_playbin, GST_FORMAT_TIME, &position) &&
        gst_element_query_duration (global->current_playbin, GST_FORMAT_TIME, &duration) &&
        duration > 0) {
      global->pause_position = (gdouble) position / duration;
      g_debug ("Stored pause position: %f", global->pause_position);
    }

    /* Block the pipeline to stop output */
    global->pipeline_blocked = TRUE;

    /* Start periodic seeking to maintain position */
    if (global->seek_timeout_id == 0) {
      global->seek_timeout_id = g_timeout_add (1000, seek_to_pause_position, stream); /* Every 1 second */
      g_debug ("Started periodic seek timer");
    }

    g_debug ("Global pause: blocked pipeline");
  } else {
    g_debug ("Global pause: no valid playbin available");
  }
}

gboolean
nd_video_file_stream_global_is_playing (NdVideoFileStream *stream)
{
  NdVideoFileStreamGlobal *global = stream->global;
  if (global->current_playbin && GST_IS_ELEMENT (global->current_playbin)) {
    GstState state;
    GstStateChangeReturn ret = gst_element_get_state (global->current_playbin, &state, NULL, GST_CLOCK_TIME_NONE);
    if (ret == GST_STATE_CHANGE_SUCCESS || ret == GST_STATE_CHANGE_ASYNC)
      return state == GST_STATE_PLAYING && !global->pipeline_blocked;
  }
  return FALSE;
}

void
nd_video_file_stream_global_seek (NdVideoFileStream *stream, gdouble position)
{
  NdVideoFileStreamGlobal *global = stream->global;
  if (global->current_playbin && GST_IS_ELEMENT (global->current_playbin)) {
    gint64 duration;
    if (gst_element_query_duration (global->current_playbin, GST_FORMAT_TIME, &duration) && duration > 0) {
      gint64 seek_pos = (gint64) (position * duration);
      gboolean success = gst_element_seek_simple (global->current_playbin, GST_FORMAT_TIME,
                                                  GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
                                                  seek_pos);
      if (success)
        g_debug ("Global seek: seeked to position %f", position);
      else
        g_warning ("Global seek: seek operation failed");
    } else {
      g_debug ("Global seek: unable to get duration");
    }
  } else {
    g_debug ("Global seek: no valid playbin available");
  }
}

gdouble
nd_video_file_stream_global_get_position (NdVideoFileStream *stream)
{
  NdVideoFileStreamGlobal *global = stream->global;

  /* If paused, return the stored pause position */
  if (global->pipeline_blocked && global->pause_position >= 0.0)
    return global->pause_position;

  if (global->current_playbin) {
    gint64 position, duration;
    if (gst_element_query_position (global->current_playbin, GST_FORMAT_TIME, &position) &&
        gst_element_query_duration (global->current_playbin, GST_FORMAT_TIME, &duration) &&
        duration > 0)
      return (gdouble) position / duration;
  }
  return 0.0;
}

gint64
nd_video_file_stream_global_get_duration (NdVideoFileStream *stream)
{
  NdVideoFileStreamGlobal *global = stream->global;
  if (global->current_playbin) {
    gint64 duration;
    if (gst_element_query_duration (global->current_playbin, GST_FORMAT_TIME, &duration))
      return duration;
  }
  return -1;
}

gdouble
nd_video_file_stream_global_get_volume (NdVideoFileStream *stream)
{
  NdVideoFileStreamGlobal *global = stream->global;
  if (global->current_playbin) {
    gdouble volume;
    g_object_get (global->current_playbin, "volume", &volume, NULL);
    return volume;
  }
  return 1.0;
}

void
nd_video_file_stream_global_set_volume (NdVideoFileStream *stream, gdouble volume)
{
  NdVideoFileStreamGlobal *global = stream->global;
  if (global->current_playbin && GST_IS_ELEMENT (global->current_playbin)) {
    g_object_set (global->current_playbin, "volume", volume, NULL);
    g_debug ("Global volume: set to %f", volume);
  } else {
    g_debug ("Global volume: no valid playbin available");
  }
}

GstElement *
nd_video_file_stream_get_global_playbin (NdVideoFileStream *stream)
{
  return stream->global->current_playbin;
}

void
nd_video_file_stream_set_global_playbin (NdVideoFileStream *stream, GstElement *playbin)
{
  stream->global->current_playbin = playbin;
  g_debug ("Set global playbin: %s", playbin ? GST_ELEMENT_NAME (playbin) : "NULL");
}

void
nd_video_file_stream_initialize_global_references (NdVideoFileStream *stream, GstElement *source_bin)
{
  if (!source_bin) {
    g_warning ("initialize_global_references: No source bin provided");
    return;
  }

  GstIterator *iter = gst_bin_iterate_elements (GST_BIN (source_bin));
  GValue item = G_VALUE_INIT;

  while (gst_iterator_next (iter, &item) == GST_ITERATOR_OK) {
    GstElement *element = g_value_get_object (&item);
    const gchar *element_name = GST_ELEMENT_NAME (element);

    g_debug ("Checking element: %s", element_name);

    if (g_str_has_prefix (element_name, "video-player")) {
      stream->global->current_playbin = element;
      g_debug ("Set global playbin reference from initialize: %s", GST_ELEMENT_NAME (element));
    } else if (g_str_has_prefix (element_name, "screencastsrc")) {
      stream->global->current_intervideosrc = element;

      /* Add blocking probe to intervideosrc */
      GstPad *srcpad = gst_element_get_static_pad (element, "src");
      if (srcpad) {
        stream->global->video_probe_id = gst_pad_add_probe (srcpad,
                                                           GST_PAD_PROBE_TYPE_BUFFER,
                                                           video_blocking_probe_cb,
                                                           stream, NULL);
        gst_object_unref (srcpad);
        g_debug ("Added video blocking probe from initialize");
      }
    }

    g_value_reset (&item);
  }

  g_value_unset (&item);
  gst_iterator_free (iter);
}

GstElement *
nd_video_file_stream_create_source (const gchar *video_file_path)
{
  GstElement *playbin, *videosink, *audiosink, *res, *videoscale, *capsfilter;
  GstBin *bin;
  gchar *uri;
  GstCaps *caps;

  if (!video_file_path) {
    g_warning ("nd_video_file_stream_create_source: No video file path provided!");
    return NULL;
  }

  bin = GST_BIN (gst_bin_new ("playbin-wrapper-bin"));

  playbin = gst_element_factory_make ("playbin", "video-player");
  if (!playbin) {
    g_warning ("Failed to create playbin element");
    gst_object_unref (bin);
    return NULL;
  }

  /* Convert file path to URI */
  uri = g_filename_to_uri (video_file_path, NULL, NULL);
  if (!uri) {
    g_warning ("nd_video_file_stream_create_source: Failed to convert file path to URI: %s", video_file_path);
    gst_object_unref (bin);
    return NULL;
  }

  /* Configure playbin for reduced buffering but keep timing */
  g_object_set (playbin,
                "uri", uri,
                "buffer-size", 2048,       /* Moderate buffer size */
                "buffer-duration", 1000000000, /* 1 second buffer */
                NULL);
  g_free (uri);

  /* Set up video sink */
  videosink = gst_element_factory_make ("intervideosink", "inter-video-sink");
  if (!videosink) {
    g_warning ("Failed to create intervideosink element");
    gst_object_unref (bin);
    return NULL;
  }

  g_object_set (videosink,
                "channel", "nd-inter-video",
                "max-lateness", (gint64) 33333333,  /* Drop frames older than ~33ms (2 frames) */
                "sync", TRUE,
                "async", FALSE,
                "qos", TRUE,
                NULL);

  /* Set up audio sink for wireless display */
  audiosink = gst_element_factory_make ("interaudiosink", "inter-audio-sink");
  if (!audiosink) {
    g_warning ("Failed to create interaudiosink element");
    gst_object_unref (bin);
    return NULL;
  }

  g_object_set (audiosink,
                "channel", "nd-inter-audio",
                "sync", TRUE,
                "async", FALSE,
                NULL);

  /* Create a volume element to ensure proper audio levels */
  GstElement *volume = gst_element_factory_make ("volume", "audio-volume");
  GstElement *audiobin = gst_bin_new ("audio-bin");

  if (volume && audiobin) {
    /* Set volume to 100% (1.0) to ensure good audio levels */
    g_object_set (volume, "volume", 1.0, NULL);

    gst_bin_add_many (GST_BIN (audiobin), volume, audiosink, NULL);
    gst_element_link (volume, audiosink);

    /* Create ghost pad for the audio bin */
    GstPad *sink_pad = gst_element_get_static_pad (volume, "sink");
    gst_element_add_pad (audiobin, gst_ghost_pad_new ("sink", sink_pad));
    gst_object_unref (sink_pad);

    g_object_set (playbin,
                  "video-sink", videosink,
                  "audio-sink", audiobin,
                  NULL);
  } else {
    /* Fallback to direct audio sink if volume element fails */
    g_object_set (playbin,
                  "video-sink", videosink,
                  "audio-sink", audiosink,
                  NULL);
  }

  gst_bin_add (bin, playbin);

  /* Add probe to intervideosink sink pad */
  GstPad *sinkpad = gst_element_get_static_pad (videosink, "sink");
  if (sinkpad) {
    gst_pad_add_probe (sinkpad,
                       GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                       NULL,
                       playbin, NULL);
    gst_object_unref (sinkpad);
  }

  /* intervideosrc for output */
  res = gst_element_factory_make ("intervideosrc", "screencastsrc");
  videoscale = gst_element_factory_make ("videoscale", "downscale");
  capsfilter = gst_element_factory_make ("capsfilter", "cap720p");

  if (!res || !videoscale || !capsfilter) {
    g_warning ("Failed to create intervideosrc or scaling elements");
    gst_object_unref (bin);
    return NULL;
  }

  g_object_set (res,
                "do-timestamp", FALSE,     /* Let playbin handle timing */
                "timeout", (guint64) G_MAXUINT64, /* Back to original timeout */
                "channel", "nd-inter-video",
                NULL);

  caps = gst_caps_new_simple ("video/x-raw",
                              "width", G_TYPE_INT, 1280,
                              "height", G_TYPE_INT, 720,
                              NULL);
  g_object_set (capsfilter, "caps", caps, NULL);
  gst_caps_unref (caps);

  gst_bin_add_many (bin, res, videoscale, capsfilter, NULL);
  if (!gst_element_link_many (res, videoscale, capsfilter, NULL)) {
    g_warning ("Failed to link intervideosrc -> videoscale -> capsfilter");
    gst_object_unref (bin);
    return NULL;
  }

  gst_element_add_pad (GST_ELEMENT (bin),
                       gst_ghost_pad_new ("src",
                                          gst_element_get_static_pad (capsfilter, "src")));

  g_object_ref_sink (bin);
  return GST_ELEMENT (bin);
}

GstElement *
nd_video_file_stream_create_audio_source (NdVideoFileStream *stream)
{
  GstElement *res;

  /* Try to get audio from the video file via interaudiosrc */
  res = gst_element_factory_make ("interaudiosrc", "inter-audio-src");
  if (res) {
    g_object_set (res,
                  "channel", "nd-inter-audio",
                  NULL);

    /* Store reference and add blocking probe */
    stream->global->current_interaudiosrc = res;

    /* Add blocking probe to interaudiosrc */
    GstPad *srcpad = gst_element_get_static_pad (res, "src");
    if (srcpad) {
      stream->global->audio_probe_id = gst_pad_add_probe (srcpad,
                                                          GST_PAD_PROBE_TYPE_BUFFER,
                                                          audio_blocking_probe_cb,
                                                          stream, NULL);
      gst_object_unref (srcpad);
      g_debug ("Added audio blocking probe");
    }

    g_debug ("nd_video_file_stream_create_audio_source: Using inter audio source for video file audio");
    return g_object_ref_sink (res);
  }

  g_debug ("nd_video_file_stream_create_audio_source: Failed to create inter audio source");
  return NULL;
}
