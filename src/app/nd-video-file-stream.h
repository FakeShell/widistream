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

#pragma once

#include <glib-object.h>
#include <gst/gst.h>

G_BEGIN_DECLS

/**
 * nd_video_file_stream_create_source:
 * @video_file_path: Path to the video file to stream
 *
 * Creates a GStreamer source element for streaming a video file.
 * The source uses playbin internally with intervideo/interaudio elements
 * for wireless display streaming.
 *
 * Returns: (transfer full): A new GStreamer element for video file streaming,
 *          or NULL on error
 */
GstElement *nd_video_file_stream_create_source (const gchar *video_file_path);

/**
 * nd_video_file_stream_create_audio_source:
 *
 * Creates a GStreamer audio source element for video file audio.
 * Uses interaudiosrc to get audio from the video file playback.
 *
 * Returns: (transfer full): A new GStreamer element for video file audio,
 *          or NULL on error
 */
GstElement *nd_video_file_stream_create_audio_source (void);

G_END_DECLS
