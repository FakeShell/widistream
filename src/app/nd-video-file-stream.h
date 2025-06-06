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
 * NdVideoFileStream:
 *
 * Structure to manage video file streaming with media controls and pipeline blocking.
 */
typedef struct _NdVideoFileStream NdVideoFileStream;

/**
 * nd_video_file_stream_new:
 * @video_file_path: Path to the video file to stream
 *
 * Creates a new video file stream manager.
 *
 * Returns: (transfer full): A new NdVideoFileStream, or NULL on error
 */
NdVideoFileStream *nd_video_file_stream_new (const gchar *video_file_path);

/**
 * nd_video_file_stream_free:
 * @stream: An NdVideoFileStream
 *
 * Frees a video file stream manager and cleans up global references.
 */
void nd_video_file_stream_free (NdVideoFileStream *stream);

/**
 * nd_video_file_stream_get_source:
 * @stream: An NdVideoFileStream
 *
 * Gets the GStreamer source element for the video file stream.
 * Sets up global references for media controls and blocking probes.
 *
 * Returns: (transfer full): A GStreamer element, or NULL on error
 */
GstElement *nd_video_file_stream_get_source (NdVideoFileStream *stream);

/**
 * nd_video_file_stream_play:
 * @stream: An NdVideoFileStream
 *
 * Starts or resumes playback and unblocks the pipeline.
 */
void nd_video_file_stream_play (NdVideoFileStream *stream);

/**
 * nd_video_file_stream_pause:
 * @stream: An NdVideoFileStream
 *
 * Pauses playback and blocks the pipeline output.
 */
void nd_video_file_stream_pause (NdVideoFileStream *stream);

/**
 * nd_video_file_stream_is_playing:
 * @stream: An NdVideoFileStream
 *
 * Checks if the stream is currently playing (not paused or blocked).
 *
 * Returns: TRUE if playing, FALSE otherwise
 */
gboolean nd_video_file_stream_is_playing (NdVideoFileStream *stream);

/**
 * nd_video_file_stream_seek:
 * @stream: An NdVideoFileStream
 * @position: Position to seek to (0.0 to 1.0)
 *
 * Seeks to a specific position in the video.
 */
void nd_video_file_stream_seek (NdVideoFileStream *stream, gdouble position);

/**
 * nd_video_file_stream_get_position:
 * @stream: An NdVideoFileStream
 *
 * Gets the current playback position.
 *
 * Returns: Current position (0.0 to 1.0)
 */
gdouble nd_video_file_stream_get_position (NdVideoFileStream *stream);

/**
 * nd_video_file_stream_get_duration:
 * @stream: An NdVideoFileStream
 *
 * Gets the total duration of the video.
 *
 * Returns: Duration in nanoseconds, or -1 if unknown
 */
gint64 nd_video_file_stream_get_duration (NdVideoFileStream *stream);

/**
 * nd_video_file_stream_set_volume:
 * @stream: An NdVideoFileStream
 * @volume: Volume level (0.0 to 1.0)
 *
 * Sets the playback volume.
 */
void nd_video_file_stream_set_volume (NdVideoFileStream *stream, gdouble volume);

/**
 * nd_video_file_stream_get_volume:
 * @stream: An NdVideoFileStream
 *
 * Gets the current playback volume.
 *
 * Returns: Volume level (0.0 to 1.0)
 */
gdouble nd_video_file_stream_get_volume (NdVideoFileStream *stream);

/**
 * nd_video_file_stream_global_play:
 * @stream: An NdVideoFileStream
 *
 * Plays the current global video stream. Stops periodic seeking timer,
 * performs final seek to stored pause position, and unblocks the pipeline.
 */
void nd_video_file_stream_global_play (NdVideoFileStream *stream);

/**
 * nd_video_file_stream_global_pause:
 * @stream: An NdVideoFileStream
 *
 * Pauses the current global video stream. Stores current position,
 * blocks the pipeline, and starts periodic seeking to maintain position.
 */
void nd_video_file_stream_global_pause (NdVideoFileStream *stream);

/**
 * nd_video_file_stream_global_is_playing:
 * @stream: An NdVideoFileStream
 *
 * Checks if the current global video stream is playing and not blocked.
 *
 * Returns: TRUE if playing and not blocked, FALSE otherwise
 */
gboolean nd_video_file_stream_global_is_playing (NdVideoFileStream *stream);

/**
 * nd_video_file_stream_global_seek:
 * @stream: An NdVideoFileStream
 * @position: Position to seek to (0.0 to 1.0)
 *
 * Seeks the current global video stream to a specific position.
 */
void nd_video_file_stream_global_seek (NdVideoFileStream *stream, gdouble position);

/**
 * nd_video_file_stream_global_get_position:
 * @stream: An NdVideoFileStream
 *
 * Gets the current playback position of the global video stream.
 * Returns stored pause position when paused, actual position when playing.
 *
 * Returns: Current position (0.0 to 1.0)
 */
gdouble nd_video_file_stream_global_get_position (NdVideoFileStream *stream);

/**
 * nd_video_file_stream_global_get_duration:
 * @stream: An NdVideoFileStream
 *
 * Gets the total duration of the current global video stream.
 *
 * Returns: Duration in nanoseconds, or -1 if unknown
 */
gint64 nd_video_file_stream_global_get_duration (NdVideoFileStream *stream);

/**
 * nd_video_file_stream_global_set_volume:
 * @stream: An NdVideoFileStream
 * @volume: Volume level (0.0 to 1.0)
 *
 * Sets the playback volume of the current global video stream.
 */
void nd_video_file_stream_global_set_volume (NdVideoFileStream *stream, gdouble volume);

/**
 * nd_video_file_stream_global_get_volume:
 * @stream: An NdVideoFileStream
 *
 * Gets the current playback volume of the global video stream.
 *
 * Returns: Volume level (0.0 to 1.0)
 */
gdouble nd_video_file_stream_global_get_volume (NdVideoFileStream *stream);

/**
 * nd_video_file_stream_get_global_playbin:
 * @stream: An NdVideoFileStream
 *
 * Gets the current global playbin element for media controls.
 *
 * Returns: (transfer none): The current playbin element, or NULL
 */
GstElement *nd_video_file_stream_get_global_playbin (NdVideoFileStream *stream);

/**
 * nd_video_file_stream_set_global_playbin:
 * @stream: An NdVideoFileStream
 * @playbin: The playbin element to set as global
 *
 * Sets the global playbin element for media controls.
 */
void nd_video_file_stream_set_global_playbin (NdVideoFileStream *stream, GstElement *playbin);

/**
 * nd_video_file_stream_initialize_global_references:
 * @stream: An NdVideoFileStream
 * @source_bin: The source bin containing the playbin and intervideo elements
 *
 * Initializes global references for media controls from an existing source bin.
 * Finds and sets up global references to playbin and intervideosrc elements,
 * and installs blocking probes for pipeline control.
 */
void nd_video_file_stream_initialize_global_references (NdVideoFileStream *stream, GstElement *source_bin);

/**
 * nd_video_file_stream_create_source:
 * @video_file_path: Path to the video file to stream
 *
 * Creates a GStreamer source element for streaming a video file.
 * The source uses playbin internally with intervideo/interaudio elements
 * for wireless display streaming. Includes proper buffering configuration
 * and volume control setup.
 *
 * Returns: (transfer full): A new GStreamer element for video file streaming,
 *          or NULL on error
 */
GstElement *nd_video_file_stream_create_source (const gchar *video_file_path);

/**
 * nd_video_file_stream_create_audio_source:
 * @stream: An NdVideoFileStream
 *
 * Creates a GStreamer audio source element for video file audio.
 * Uses interaudiosrc to get audio from the video file playback and
 * installs blocking probes for pause/resume functionality.
 *
 * Returns: (transfer full): A new GStreamer element for video file audio,
 *          or NULL on error
 */
GstElement *nd_video_file_stream_create_audio_source (NdVideoFileStream *stream);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (NdVideoFileStream, nd_video_file_stream_free)

G_END_DECLS
