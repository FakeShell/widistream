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

#include "nd-mutter-screencast.h"
#include <gst/base/base.h>

struct _NdMutterScreencast {
  NdMutterScreencastState state;

  /* D-Bus proxies for GNOME Shell ScreenCast */
  GDBusProxy *screencast_proxy;
  GDBusProxy *session_proxy;
  GDBusProxy *stream_proxy;

  /* Session and stream paths */
  gchar *session_path;
  gchar *stream_path;

  /* PipeWire node ID from ScreenCast */
  guint node_id;

  /* GStreamer pipeline for screen capture */
  GstElement *pipeline;
  GstElement *source_element;

  /* Capture area configuration */
  gint capture_x;
  gint capture_y;
  gint capture_width;
  gint capture_height;
};

static void
cleanup_dbus_resources (NdMutterScreencast *screencast)
{
  g_return_if_fail (screencast != NULL);

  if (screencast->stream_proxy) {
    g_object_unref (screencast->stream_proxy);
    screencast->stream_proxy = NULL;
  }

  if (screencast->session_proxy) {
    g_object_unref (screencast->session_proxy);
    screencast->session_proxy = NULL;
  }

  g_free (screencast->session_path);
  screencast->session_path = NULL;

  g_free (screencast->stream_path);
  screencast->stream_path = NULL;

  screencast->node_id = 0;
}

static void
cleanup_pipeline (NdMutterScreencast *screencast)
{
  g_return_if_fail (screencast != NULL);

  if (screencast->pipeline) {
    gst_element_set_state (screencast->pipeline, GST_STATE_NULL);
    gst_object_unref (screencast->pipeline);
    screencast->pipeline = NULL;
  }

  if (screencast->source_element) {
    gst_object_unref (screencast->source_element);
    screencast->source_element = NULL;
  }
}

static void
on_pipewire_stream_added (GDBusProxy *proxy,
                          const gchar *sender_name,
                          const gchar *signal_name,
                          GVariant *parameters,
                          gpointer user_data)
{
  NdMutterScreencast *screencast = (NdMutterScreencast *)user_data;
  GError *error = NULL;

  g_variant_get (parameters, "(u)", &screencast->node_id);
  g_debug ("NdMutterScreencast: PipeWire stream added with node ID: %u", screencast->node_id);

  gchar *pipeline_str = g_strdup_printf (
    "pipewiresrc path=%u do-timestamp=true ! "
    "videoconvert ! "
    "videoscale method=lanczos add-borders=false ! "
    "intervideosink channel=nd-mutter-screencast sync=true async=false "
    "max-lateness=33333333 qos=true",
    screencast->node_id);

  screencast->pipeline = gst_parse_launch (pipeline_str, &error);
  g_free (pipeline_str);

  if (error) {
    g_warning ("NdMutterScreencast: Failed to create pipeline: %s", error->message);
    g_error_free (error);
    screencast->state = ND_MUTTER_SCREENCAST_STATE_ERROR;
    return;
  }

  gst_element_set_state (screencast->pipeline, GST_STATE_PLAYING);
  screencast->state = ND_MUTTER_SCREENCAST_STATE_ACTIVE;
  g_debug ("NdMutterScreencast: Pipeline started successfully");
}

static gboolean
create_screencast_session (NdMutterScreencast *screencast, GError **error)
{
  GVariant *result;

  g_return_val_if_fail (screencast != NULL, FALSE);
  g_return_val_if_fail (screencast->screencast_proxy != NULL, FALSE);

  screencast->state = ND_MUTTER_SCREENCAST_STATE_CREATING_SESSION;

  result = g_dbus_proxy_call_sync (screencast->screencast_proxy,
                                   "CreateSession",
                                   g_variant_new ("(a{sv})", NULL),
                                   G_DBUS_CALL_FLAGS_NONE,
                                   -1,
                                   NULL,
                                   error);

  if (!result) {
    screencast->state = ND_MUTTER_SCREENCAST_STATE_ERROR;
    return FALSE;
  }

  g_variant_get (result, "(o)", &screencast->session_path);
  g_variant_unref (result);
  g_debug ("NdMutterScreencast: Created session: %s", screencast->session_path);

  screencast->session_proxy = g_dbus_proxy_new_for_bus_sync (
    G_BUS_TYPE_SESSION,
    G_DBUS_PROXY_FLAGS_NONE,
    NULL,
    "org.gnome.Mutter.ScreenCast",
    screencast->session_path,
    "org.gnome.Mutter.ScreenCast.Session",
    NULL,
    error);

  if (!screencast->session_proxy) {
    screencast->state = ND_MUTTER_SCREENCAST_STATE_ERROR;
    return FALSE;
  }

  return TRUE;
}

static gboolean
start_screen_recording (NdMutterScreencast *screencast, GError **error)
{
  GVariant *result;
  GVariantBuilder builder;

  g_return_val_if_fail (screencast != NULL, FALSE);
  g_return_val_if_fail (screencast->session_proxy != NULL, FALSE);

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&builder, "{sv}", "cursor-mode", g_variant_new_uint32 (1)); /* embedded cursor */

  /* Record the primary monitor: empty string means primary monitor */
  result = g_dbus_proxy_call_sync (screencast->session_proxy,
                                   "RecordMonitor",
                                   g_variant_new ("(sa{sv})", "", &builder),
                                   G_DBUS_CALL_FLAGS_NONE,
                                   -1,
                                   NULL,
                                   error);

  if (!result) {
    screencast->state = ND_MUTTER_SCREENCAST_STATE_ERROR;
    return FALSE;
  }

  g_variant_get (result, "(o)", &screencast->stream_path);
  g_variant_unref (result);
  g_debug ("NdMutterScreencast: Created monitor stream: %s", screencast->stream_path);

  screencast->stream_proxy = g_dbus_proxy_new_for_bus_sync (
    G_BUS_TYPE_SESSION,
    G_DBUS_PROXY_FLAGS_NONE,
    NULL,
    "org.gnome.Mutter.ScreenCast",
    screencast->stream_path,
    "org.gnome.Mutter.ScreenCast.Stream",
    NULL,
    error);

  if (!screencast->stream_proxy) {
    screencast->state = ND_MUTTER_SCREENCAST_STATE_ERROR;
    return FALSE;
  }

  /* Connect to PipeWire stream signal */
  g_signal_connect (screencast->stream_proxy, "g-signal",
                    G_CALLBACK (on_pipewire_stream_added), screencast);

  return TRUE;
}

static gboolean
start_session (NdMutterScreencast *screencast, GError **error)
{
  g_return_val_if_fail (screencast != NULL, FALSE);
  g_return_val_if_fail (screencast->session_proxy != NULL, FALSE);

  screencast->state = ND_MUTTER_SCREENCAST_STATE_STARTING;

  g_dbus_proxy_call_sync (screencast->session_proxy,
                          "Start",
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          NULL,
                          error);

  if (*error) {
    screencast->state = ND_MUTTER_SCREENCAST_STATE_ERROR;
    return FALSE;
  }

  g_debug ("NdMutterScreencast: Session started successfully");
  return TRUE;
}

NdMutterScreencast *
nd_mutter_screencast_new (void)
{
  NdMutterScreencast *screencast;
  GError *error = NULL;

  screencast = g_new0 (NdMutterScreencast, 1);
  screencast->state = ND_MUTTER_SCREENCAST_STATE_INITIALIZING;

  screencast->screencast_proxy = g_dbus_proxy_new_for_bus_sync (
    G_BUS_TYPE_SESSION,
    G_DBUS_PROXY_FLAGS_NONE,
    NULL,
    "org.gnome.Mutter.ScreenCast",
    "/org/gnome/Mutter/ScreenCast",
    "org.gnome.Mutter.ScreenCast",
    NULL,
    &error);

  if (!screencast->screencast_proxy) {
    g_warning ("NdMutterScreencast: Failed to create ScreenCast proxy: %s",
               error ? error->message : "Unknown error");
    if (error)
      g_error_free (error);
    screencast->state = ND_MUTTER_SCREENCAST_STATE_ERROR;
    return screencast;
  }

  screencast->state = ND_MUTTER_SCREENCAST_STATE_IDLE;
  g_debug ("NdMutterScreencast: D-Bus connection established");

  return screencast;
}

void
nd_mutter_screencast_free (NdMutterScreencast *screencast)
{
  if (!screencast)
    return;

  /* Stop capture if active */
  nd_mutter_screencast_stop_capture (screencast);

  if (screencast->screencast_proxy)
    g_object_unref (screencast->screencast_proxy);

  cleanup_dbus_resources (screencast);
  cleanup_pipeline (screencast);

  g_free (screencast);
}

NdMutterScreencastState
nd_mutter_screencast_get_state (NdMutterScreencast *screencast)
{
  g_return_val_if_fail (screencast != NULL, ND_MUTTER_SCREENCAST_STATE_ERROR);
  return screencast->state;
}

gboolean
nd_mutter_screencast_is_available (NdMutterScreencast *screencast)
{
  g_return_val_if_fail (screencast != NULL, FALSE);

  return screencast->screencast_proxy != NULL &&
         screencast->state != ND_MUTTER_SCREENCAST_STATE_ERROR;
}

gboolean
nd_mutter_screencast_start_capture (NdMutterScreencast *screencast,
                                    gint x,
                                    gint y,
                                    gint width,
                                    gint height,
                                    GError **error)
{
  g_return_val_if_fail (screencast != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (screencast->state == ND_MUTTER_SCREENCAST_STATE_ACTIVE) {
    g_debug ("NdMutterScreencast: Already capturing");
    return TRUE;
  }

  if (!nd_mutter_screencast_is_available (screencast)) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "GNOME Shell ScreenCast is not available");
    return FALSE;
  }

  /* Store capture area */
  screencast->capture_x = x;
  screencast->capture_y = y;
  screencast->capture_width = width;
  screencast->capture_height = height;

  /* Create session */
  if (!create_screencast_session (screencast, error)) {
    g_prefix_error (error, "Failed to create ScreenCast session: ");
    return FALSE;
  }

  /* Start recording */
  if (!start_screen_recording (screencast, error)) {
    g_prefix_error (error, "Failed to start screen recording: ");
    cleanup_dbus_resources (screencast);
    return FALSE;
  }

  /* Start session */
  if (!start_session (screencast, error)) {
    g_prefix_error (error, "Failed to start ScreenCast session: ");
    cleanup_dbus_resources (screencast);
    return FALSE;
  }

  g_debug ("NdMutterScreencast: Screen capture started successfully");
  return TRUE;
}

void
nd_mutter_screencast_stop_capture (NdMutterScreencast *screencast)
{
  g_return_if_fail (screencast != NULL);

  if (screencast->state == ND_MUTTER_SCREENCAST_STATE_IDLE) {
    g_debug ("NdMutterScreencast: Not currently capturing");
    return;
  }

  if (screencast->session_proxy) {
    GError *error = NULL;
    g_dbus_proxy_call_sync (screencast->session_proxy,
                            "Stop",
                            NULL,
                            G_DBUS_CALL_FLAGS_NONE,
                            -1,
                            NULL,
                            &error);

    if (error) {
      g_warning ("NdMutterScreencast: Failed to stop session: %s", error->message);
      g_error_free (error);
    }
  }

  cleanup_pipeline (screencast);
  cleanup_dbus_resources (screencast);

  screencast->state = ND_MUTTER_SCREENCAST_STATE_IDLE;
  g_debug ("NdMutterScreencast: Screen capture stopped");
}

GstElement *
nd_mutter_screencast_get_source (NdMutterScreencast *screencast)
{
  GstElement *source;

  g_return_val_if_fail (screencast != NULL, NULL);

  if (screencast->state != ND_MUTTER_SCREENCAST_STATE_ACTIVE) {
    g_debug ("NdMutterScreencast: Not currently capturing, cannot provide source");
    return NULL;
  }

  if (screencast->source_element) {
    return gst_object_ref (screencast->source_element);
  }

  /* Create intervideosrc to receive from the pipeline */
  source = gst_element_factory_make ("intervideosrc", "mutter-screencast-src");
  if (!source) {
    g_warning ("NdMutterScreencast: Failed to create intervideosrc element");
    return NULL;
  }

  g_object_set (source,
                "channel", "nd-mutter-screencast",
                "do-timestamp", TRUE,
                "timeout", (guint64) G_MAXUINT64,
                NULL);

  screencast->source_element = gst_object_ref (source);
  g_debug ("NdMutterScreencast: Created source element");

  return source;
}

GstElement *
nd_mutter_screencast_create_source (void)
{
  GstElement *source, *videoconvert, *videoscale;
  GstBin *bin;

  bin = GST_BIN (gst_bin_new ("mutter-screencast-source"));

  source = gst_element_factory_make ("intervideosrc", "mutter-screencast-src");
  videoconvert = gst_element_factory_make ("videoconvert", "convert");
  videoscale = gst_element_factory_make ("videoscale", "scale");

  if (!source || !videoconvert || !videoscale) {
    g_warning ("NdMutterScreencast: Failed to create pipeline elements");
    gst_object_unref (bin);
    return NULL;
  }

  g_object_set (source,
                "channel", "nd-mutter-screencast",
                "do-timestamp", TRUE,
                "timeout", (guint64) G_MAXUINT64,
                NULL);

  g_object_set (videoscale,
                "method", 2,  /* Lanczos scaling */
                "add-borders", FALSE,  /* Don't add black borders; stretch to fill */
                NULL);

  gst_bin_add_many (bin, source, videoconvert, videoscale, NULL);

  /* Link elements without caps filter; let downstream negotiate resolution */
  if (!gst_element_link_many (source, videoconvert, videoscale, NULL)) {
    g_warning ("NdMutterScreencast: Failed to link pipeline elements");
    gst_object_unref (bin);
    return NULL;
  }

  gst_element_add_pad (GST_ELEMENT (bin),
                       gst_ghost_pad_new ("src",
                                          gst_element_get_static_pad (videoscale, "src")));

  g_object_ref_sink (bin);
  g_debug ("NdMutterScreencast: Created dynamic scaling source pipeline");

  return GST_ELEMENT (bin);
}

guint
nd_mutter_screencast_get_node_id (NdMutterScreencast *screencast)
{
  g_return_val_if_fail (screencast != NULL, 0);
  return screencast->node_id;
}
