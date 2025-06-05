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

GstElement *
nd_video_file_stream_create_source (const gchar *video_file_path)
{
  GstElement *playbin, *videosink, *audiosink, *res;
  GstBin *bin;
  gchar *uri;

  if (!video_file_path) {
    g_warning ("nd_video_file_stream_create_source: No video file path provided!");
    return NULL;
  }

  bin = GST_BIN (gst_bin_new ("playbin-wrapper-bin"));

  playbin = gst_element_factory_make ("playbin", "video-player");

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
  videosink = gst_element_factory_make ("intervideosink", "inter video sink");
  g_object_set (videosink,
                "channel", "nd-inter-video",
                "max-lateness", (gint64) 33333333,  /* Drop frames older than ~33ms (2 frames) */
                "sync", TRUE,
                "async", FALSE,
                "qos", TRUE,
                NULL);

  /* Set up audio sink for wireless display */
  audiosink = gst_element_factory_make ("interaudiosink", "inter audio sink");
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
  g_object_set (res,
                "do-timestamp", FALSE,     /* Let playbin handle timing */
                "timeout", (guint64) G_MAXUINT64, /* Back to original timeout */
                "channel", "nd-inter-video",
                NULL);

  gst_bin_add (bin, res);

  gst_element_add_pad (GST_ELEMENT (bin),
                       gst_ghost_pad_new ("src",
                                          gst_element_get_static_pad (res, "src")));

  g_object_ref_sink (bin);
  return GST_ELEMENT (bin);
}

GstElement *
nd_video_file_stream_create_audio_source (void)
{
  GstElement *res;

  /* Try to get audio from the video file via interaudiosrc */
  res = gst_element_factory_make ("interaudiosrc", "inter audio src");
  if (res) {
    g_object_set (res,
                  "channel", "nd-inter-audio",
                  NULL);
    g_debug ("nd_video_file_stream_create_audio_source: Using inter audio source for video file audio");
    return g_object_ref_sink (res);
  }

  g_debug ("nd_video_file_stream_create_audio_source: Failed to create inter audio source");
  return NULL;
}
