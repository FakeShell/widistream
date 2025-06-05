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

#include "nd-application-window-stream.h"
#include <signal.h>
#include <unistd.h>

NdX11Session *
nd_x11_session_new (void)
{
  NdX11Session *session = g_new0 (NdX11Session, 1);
  session->xvfb_pid = 0;
  session->x11_display = NULL;
  session->x11_session_active = FALSE;
  return session;
}

void
nd_x11_session_free (NdX11Session *session)
{
  if (!session)
    return;

  if (session->x11_session_active)
    nd_x11_session_stop (session);

  g_free (session->x11_display);
  g_free (session);
}

gboolean
nd_x11_session_start (NdX11Session *session)
{
  GError *error = NULL;
  gchar *xvfb_command[] = {
    "Xvfb",
    ":99",           /* Display number */
    "-screen", "0",
    "1920x1080x24",  /* Resolution and color depth */
    "-ac",           /* Disable access control */
    "-nolisten", "tcp",
    NULL
  };

  g_return_val_if_fail (session != NULL, FALSE);

  if (session->x11_session_active) {
    g_warning ("X11 session is already active");
    return TRUE;
  }

  /* Try to start Xvfb */
  if (!g_spawn_async (NULL, xvfb_command, NULL,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                      NULL, NULL, &session->xvfb_pid, &error)) {
    g_warning ("Failed to start Xvfb: %s", error->message);
    g_error_free (error);
    return FALSE;
  }

  session->x11_display = g_strdup (":99");
  session->x11_session_active = TRUE;

  g_debug ("Started Xvfb with PID %d on display %s", session->xvfb_pid, session->x11_display);

  /* Give Xvfb a moment to start up */
  g_usleep (500000); /* 0.5 seconds */

  /* Start a simple window manager and desktop environment */
  gchar *wm_command[] = {
    "sh", "-c",
    "DISPLAY=:99 openbox &",
    NULL
  };

  GPid wm_pid;
  if (g_spawn_async (NULL, wm_command, NULL,
                     G_SPAWN_SEARCH_PATH,
                     NULL, NULL, &wm_pid, NULL))
    g_debug ("Started window manager with PID %d", wm_pid);

  return TRUE;
}

void
nd_x11_session_stop (NdX11Session *session)
{
  g_return_if_fail (session != NULL);

  if (!session->x11_session_active)
    return;

  g_debug ("Stopping X11 session on display %s", session->x11_display);

  /* Kill Xvfb process */
  if (session->xvfb_pid > 0) {
    kill (session->xvfb_pid, SIGTERM);
    g_spawn_close_pid (session->xvfb_pid);
    session->xvfb_pid = 0;
  }

  /* Kill any remaining processes on the display */
  gchar *kill_command[] = {
    "sh", "-c",
    "pkill -f 'DISPLAY=:99'",
    NULL
  };

  g_spawn_async (NULL, kill_command, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);

  g_free (session->x11_display);
  session->x11_display = NULL;
  session->x11_session_active = FALSE;
}

gboolean
nd_x11_session_is_active (NdX11Session *session)
{
  g_return_val_if_fail (session != NULL, FALSE);
  return session->x11_session_active;
}

const gchar *
nd_x11_session_get_display (NdX11Session *session)
{
  g_return_val_if_fail (session != NULL, NULL);
  return session->x11_session_active ? session->x11_display : NULL;
}

GstElement *
nd_application_window_stream_create_source (NdX11Session *session)
{
  GstElement *source, *videoscale, *videoconvert, *queue;
  GstBin *bin;
  GstCaps *caps;

  g_return_val_if_fail (session != NULL, NULL);

  if (!session->x11_session_active) {
    g_warning ("X11 session is not active");
    return NULL;
  }

  bin = GST_BIN (gst_bin_new ("x11-source-bin"));

  /* Create X11 screen capture source */
  source = gst_element_factory_make ("ximagesrc", "x11-source");
  if (!source) {
    g_warning ("Failed to create ximagesrc element");
    gst_object_unref (bin);
    return NULL;
  }

  /* Configure ximagesrc for our X11 display */
  g_object_set (source,
                "display-name", session->x11_display,
                "use-damage", TRUE,
                "show-pointer", TRUE,
                "do-timestamp", TRUE,
                NULL);

  /* Add a small queue to prevent blocking but allow some buffering */
  queue = gst_element_factory_make ("queue", "x11-queue");
  g_object_set (queue,
                "max-size-buffers", 5,     /* Allow a few more buffers */
                "max-size-bytes", 0,
                "max-size-time", 0,
                "leaky", 2,                /* Drop old buffers */
                NULL);

  /* Add video conversion and scaling */
  videoconvert = gst_element_factory_make ("videoconvert", "x11-convert");
  videoscale = gst_element_factory_make ("videoscale", "x11-scale");

  if (!videoconvert || !videoscale || !queue) {
    g_warning ("Failed to create video processing elements");
    gst_object_unref (bin);
    return NULL;
  }

  g_object_set (videoconvert,
                "n-threads", 0,            /* Use all available threads */
                NULL);
  g_object_set (videoscale,
                "method", 0,               /* Nearest neighbor (fastest) */
                NULL);

  /* Add elements to bin */
  gst_bin_add_many (bin, source, queue, videoconvert, videoscale, NULL);

  /* Create caps for consistent output */
  caps = gst_caps_from_string ("video/x-raw,width=1920,height=1080,framerate=30/1");

  /* Link elements */
  if (!gst_element_link (source, queue) ||
      !gst_element_link (queue, videoconvert) ||
      !gst_element_link_filtered (videoconvert, videoscale, caps)) {
    g_warning ("Failed to link X11 source elements");
    gst_caps_unref (caps);
    gst_object_unref (bin);
    return NULL;
  }

  gst_caps_unref (caps);

  /* Create ghost pad */
  GstPad *src_pad = gst_element_get_static_pad (videoscale, "src");
  gst_element_add_pad (GST_ELEMENT (bin), gst_ghost_pad_new ("src", src_pad));
  gst_object_unref (src_pad);

  g_object_ref_sink (bin);
  return GST_ELEMENT (bin);
}
