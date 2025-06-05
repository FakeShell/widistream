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
#include <sys/types.h>

G_BEGIN_DECLS

/**
 * NdX11Session:
 *
 * Structure to manage X11 session state for application window streaming.
 */
typedef struct {
  GPid xvfb_pid;
  gchar *x11_display;
  gboolean x11_session_active;
} NdX11Session;

/**
 * nd_x11_session_new:
 *
 * Creates a new X11 session structure.
 *
 * Returns: (transfer full): A new NdX11Session
 */
NdX11Session *nd_x11_session_new (void);

/**
 * nd_x11_session_free:
 * @session: An NdX11Session
 *
 * Frees an X11 session structure and stops any running session.
 */
void nd_x11_session_free (NdX11Session *session);

/**
 * nd_x11_session_start:
 * @session: An NdX11Session
 *
 * Starts an X11 session using Xvfb and openbox window manager.
 * The session will run on display :99 with 1920x1080 resolution.
 *
 * Returns: TRUE if the session started successfully, FALSE otherwise
 */
gboolean nd_x11_session_start (NdX11Session *session);

/**
 * nd_x11_session_stop:
 * @session: An NdX11Session
 *
 * Stops the running X11 session and cleans up processes.
 */
void nd_x11_session_stop (NdX11Session *session);

/**
 * nd_x11_session_is_active:
 * @session: An NdX11Session
 *
 * Checks if the X11 session is currently active.
 *
 * Returns: TRUE if the session is active, FALSE otherwise
 */
gboolean nd_x11_session_is_active (NdX11Session *session);

/**
 * nd_x11_session_get_display:
 * @session: An NdX11Session
 *
 * Gets the display name for the X11 session.
 *
 * Returns: (nullable): The display name, or NULL if session is not active
 */
const gchar *nd_x11_session_get_display (NdX11Session *session);

/**
 * nd_application_window_stream_create_source:
 * @session: An active NdX11Session
 *
 * Creates a GStreamer source element for capturing the X11 session.
 * Uses ximagesrc to capture the virtual display created by Xvfb.
 *
 * Returns: (transfer full): A new GStreamer element for X11 capture,
 *          or NULL on error
 */
GstElement *nd_application_window_stream_create_source (NdX11Session *session);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (NdX11Session, nd_x11_session_free)

G_END_DECLS
