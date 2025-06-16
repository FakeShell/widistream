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
#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _NdMutterScreencast NdMutterScreencast;

/**
 * NdMutterScreencastState:
 * @ND_MUTTER_SCREENCAST_STATE_IDLE: Not currently capturing
 * @ND_MUTTER_SCREENCAST_STATE_INITIALIZING: Setting up D-Bus connections
 * @ND_MUTTER_SCREENCAST_STATE_CREATING_SESSION: Creating ScreenCast session
 * @ND_MUTTER_SCREENCAST_STATE_STARTING: Starting screen capture
 * @ND_MUTTER_SCREENCAST_STATE_ACTIVE: Actively capturing screen
 * @ND_MUTTER_SCREENCAST_STATE_ERROR: Error occurred during capture
 *
 * State of the Mutter ScreenCast capture session.
 */
typedef enum {
  ND_MUTTER_SCREENCAST_STATE_IDLE,
  ND_MUTTER_SCREENCAST_STATE_INITIALIZING,
  ND_MUTTER_SCREENCAST_STATE_CREATING_SESSION,
  ND_MUTTER_SCREENCAST_STATE_STARTING,
  ND_MUTTER_SCREENCAST_STATE_ACTIVE,
  ND_MUTTER_SCREENCAST_STATE_ERROR
} NdMutterScreencastState;

/**
 * nd_mutter_screencast_new:
 *
 * Creates a new Mutter ScreenCast manager for wireless display streaming.
 * Initializes D-Bus connections to GNOME Shell's ScreenCast interface.
 *
 * Returns: (transfer full): A new NdMutterScreencast, or NULL on error
 */
NdMutterScreencast *nd_mutter_screencast_new (void);

/**
 * nd_mutter_screencast_free:
 * @screencast: An NdMutterScreencast
 *
 * Frees a Mutter ScreenCast manager and stops any active capture session.
 */
void nd_mutter_screencast_free (NdMutterScreencast *screencast);

/**
 * nd_mutter_screencast_get_state:
 * @screencast: An NdMutterScreencast
 *
 * Gets the current state of the ScreenCast session.
 *
 * Returns: The current NdMutterScreencastState
 */
NdMutterScreencastState nd_mutter_screencast_get_state (NdMutterScreencast *screencast);

/**
 * nd_mutter_screencast_is_available:
 * @screencast: An NdMutterScreencast
 *
 * Checks if GNOME Shell ScreenCast is available on the system.
 *
 * Returns: TRUE if ScreenCast is available, FALSE otherwise
 */
gboolean nd_mutter_screencast_is_available (NdMutterScreencast *screencast);

/**
 * nd_mutter_screencast_start_capture:
 * @screencast: An NdMutterScreencast
 * @x: X coordinate of capture area (0 for full screen)
 * @y: Y coordinate of capture area (0 for full screen)
 * @width: Width of capture area (0 for full screen)
 * @height: Height of capture area (0 for full screen)
 * @error: Return location for error
 *
 * Starts screen capture using GNOME Shell ScreenCast.
 * If width and height are 0, captures the full screen.
 *
 * Returns: TRUE on success, FALSE on error
 */
gboolean nd_mutter_screencast_start_capture (NdMutterScreencast *screencast,
                                             gint x,
                                             gint y,
                                             gint width,
                                             gint height,
                                             GError **error);

/**
 * nd_mutter_screencast_stop_capture:
 * @screencast: An NdMutterScreencast
 *
 * Stops the current screen capture session and cleans up resources.
 */
void nd_mutter_screencast_stop_capture (NdMutterScreencast *screencast);

/**
 * nd_mutter_screencast_get_source:
 * @screencast: An NdMutterScreencast
 *
 * Gets the GStreamer source element for the screen capture.
 * This element can be used in wireless display streaming pipelines.
 *
 * Returns: (transfer full): A GStreamer element, or NULL if not capturing
 */
GstElement *nd_mutter_screencast_get_source (NdMutterScreencast *screencast);

/**
 * nd_mutter_screencast_create_source:
 *
 * Creates a GStreamer source element for GNOME Shell screen capture.
 * This is a convenience function that creates and configures a complete
 * screen capture pipeline suitable for wireless display streaming.
 *
 * Returns: (transfer full): A new GStreamer element for screen capture,
 *          or NULL on error
 */
GstElement *nd_mutter_screencast_create_source (void);

/**
 * nd_mutter_screencast_get_node_id:
 * @screencast: An NdMutterScreencast
 *
 * Gets the PipeWire node ID for the current capture session.
 * This is useful for debugging and advanced pipeline configuration.
 *
 * Returns: PipeWire node ID, or 0 if not capturing
 */
guint nd_mutter_screencast_get_node_id (NdMutterScreencast *screencast);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (NdMutterScreencast, nd_mutter_screencast_free)

G_END_DECLS
