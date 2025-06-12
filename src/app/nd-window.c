/*
 * Copyright 2018 Benjamin Berg <bberg@redhat.com>
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

#include <avahi-gobject/ga-client.h>
#include <avahi-gobject/ga-service-browser.h>
#include <glib/gi18n.h>
#include <gst/base/base.h>
#include <gst/gst.h>
#include "gnome-network-displays-config.h"
#include "nd-cc-provider.h"
#include "nd-codec-install.h"
#include "nd-dummy-provider.h"
#include "nd-meta-provider.h"
#include "nd-nm-device-registry.h"
#include "nd-pulseaudio.h"
#include "nd-sink-list-model.h"
#include "nd-sink-row.h"
#include "nd-wfd-mice-provider.h"
#include "nd-window.h"
#include "nd-video-file-stream.h"
#include "nd-application-window-stream.h"
#include "nd-mtk-wifi-manager.h"

struct _NdWindow
{
  AdwApplicationWindow   parent_instance;

  GaClient              *avahi_client;
  NdMetaProvider        *meta_provider;
  NdNMDeviceRegistry    *nm_device_registry;

  NdScreenCastSourceType screencast_type;
  gboolean               use_x11;

  NdPulseaudio          *pulse;

  GCancellable          *cancellable;

  NdSink                *stream_sink;
  NdSink                *pending_sink;
  gchar                 *selected_video_file;

  /* X11 session support */
  NdX11Session          *x11_session;

  /* MediaTek WiFi Manager */
  NdMtkWifiManager      *mtk_wifi_manager;

  /* Media controls support */
  gboolean               is_video_file_streaming;
  guint                  media_update_timeout_id;

  GPtrArray             *sink_property_bindings;

  /* Template widgets */
  GtkStack        *has_providers_stack;
  GtkStack        *step_stack;

  GtkListBox      *find_sink_list;
  NdSinkListModel *find_sink_list_model;

  /* Content selection page widgets */
  GtkButton       *select_video_file_button;
  GtkButton       *select_content_back_button;
  GtkButton       *select_x11_session_button;

  GtkListBox      *connect_sink_list;
  GListStore      *connect_sink_list_model;

  GtkLabel        *connect_state_label;
  GtkButton       *connect_cancel;

  GtkListBox      *stream_sink_list;
  GListStore      *stream_sink_list_model;

  GtkLabel        *stream_state_label;
  GtkButton       *stream_cancel;

  NdCodecInstall  *codec_install_video;
  NdCodecInstall  *codec_install_audio;

  GtkListBox      *error_sink_list;
  GListStore      *error_sink_list_model;
  GtkBox          *error_firewall_zone;
  GtkButton       *error_return;

  /* Media control widgets */
  GtkBox          *media_controls_box;
  GtkLabel        *current_time_label;
  GtkLabel        *total_time_label;
  GtkScale        *progress_scale;
  GtkButton       *play_pause_button;
  GtkButton       *previous_button;
  GtkButton       *next_button;
  GtkScale        *volume_scale;

  /* Video file stream context */
  NdVideoFileStream *video_file_stream;
};

G_DEFINE_FINAL_TYPE (NdWindow, nd_window, ADW_TYPE_APPLICATION_WINDOW)

static gchar *
format_time (gint64 nanoseconds)
{
  gint64 seconds = nanoseconds / GST_SECOND;
  gint minutes = seconds / 60;
  gint hours = minutes / 60;

  seconds %= 60;
  minutes %= 60;

  if (hours > 0)
    return g_strdup_printf ("%d:%02d:%02d", hours, minutes, (gint)seconds);
  else
    return g_strdup_printf ("%d:%02d", minutes, (gint)seconds);
}

static void
on_progress_scale_value_changed (GtkRange *range, NdWindow *self)
{
  gdouble value = gtk_range_get_value (range);
  if (self->video_file_stream)
    nd_video_file_stream_global_seek (self->video_file_stream, value / 100.0);
  g_debug ("Progress scale: Seeked to position %f", value / 100.0);
}

static void
on_volume_scale_value_changed (GtkRange *range, NdWindow *self)
{
  gdouble value = gtk_range_get_value (range);
  if (self->video_file_stream)
    nd_video_file_stream_global_set_volume (self->video_file_stream, value / 100.0);
  g_debug ("Volume scale: Set volume to %f", value / 100.0);
}

static gboolean
update_media_controls (gpointer user_data)
{
  NdWindow *self = ND_WINDOW (user_data);

  if (!self->is_video_file_streaming)
    return G_SOURCE_REMOVE;

  /* Only update if actually playing (not paused/blocked) */
  gboolean is_playing = self->video_file_stream &&
                        nd_video_file_stream_global_is_playing (self->video_file_stream);
  if (is_playing) {
    gdouble position = nd_video_file_stream_global_get_position (self->video_file_stream);
    gint64 duration = nd_video_file_stream_global_get_duration (self->video_file_stream);

    /* Block the signal to prevent recursive calls */
    g_signal_handlers_block_by_func (self->progress_scale,
                                     on_progress_scale_value_changed,
                                     self);
    gtk_range_set_value (GTK_RANGE (self->progress_scale), position * 100.0);
    g_signal_handlers_unblock_by_func (self->progress_scale,
                                       on_progress_scale_value_changed,
                                       self);

    /* Update time labels */
    if (duration > 0) {
      gint64 current_time = (gint64) (position * duration);
      g_autofree gchar *current_str = format_time (current_time);
      g_autofree gchar *total_str = format_time (duration);

      gtk_label_set_text (self->current_time_label, current_str);
      gtk_label_set_text (self->total_time_label, total_str);
    }
  }

  /* Always update play/pause button state */
  const gchar *icon_name = is_playing ? "media-playback-pause-symbolic" : "media-playback-start-symbolic";
  gtk_button_set_icon_name (self->play_pause_button, icon_name);

  return G_SOURCE_CONTINUE;
}

static void
on_play_pause_button_clicked (GtkButton *button, NdWindow *self)
{
  if (!self->video_file_stream)
    return;

  if (nd_video_file_stream_global_is_playing (self->video_file_stream)) {
    nd_video_file_stream_global_pause (self->video_file_stream);
    g_debug ("Play/Pause: Paused playback");
  } else {
    nd_video_file_stream_global_play (self->video_file_stream);
    g_debug ("Play/Pause: Started playback");
  }
}

static void
on_previous_button_clicked (GtkButton *button, NdWindow *self)
{
  if (!self->video_file_stream)
    return;

  /* Seek backward 10 seconds */
  gdouble current_pos = nd_video_file_stream_global_get_position (self->video_file_stream);
  gint64 duration = nd_video_file_stream_global_get_duration (self->video_file_stream);

  if (duration > 0) {
    gint64 seek_amount = 10 * GST_SECOND; /* 10 seconds */
    gdouble new_pos = current_pos - ((gdouble) seek_amount / duration);
    new_pos = CLAMP (new_pos, 0.0, 1.0);
    nd_video_file_stream_global_seek (self->video_file_stream, new_pos);
    g_debug ("Previous: Seeked to position %f", new_pos);
  }
}

static void
on_next_button_clicked (GtkButton *button, NdWindow *self)
{
  if (!self->video_file_stream)
    return;

  /* Seek forward 10 seconds */
  gdouble current_pos = nd_video_file_stream_global_get_position (self->video_file_stream);
  gint64 duration = nd_video_file_stream_global_get_duration (self->video_file_stream);

  if (duration > 0) {
    gint64 seek_amount = 10 * GST_SECOND; /* 10 seconds */
    gdouble new_pos = current_pos + ((gdouble) seek_amount / duration);
    new_pos = CLAMP (new_pos, 0.0, 1.0);
    nd_video_file_stream_global_seek (self->video_file_stream, new_pos);
    g_debug ("Next: Seeked to position %f", new_pos);
  }
}

static GstElement *
sink_create_source_cb (NdWindow *self, NdSink *sink)
{
  GstElement *source = NULL;

  /* Check if we're streaming X11 session or video file */
  if (nd_x11_session_is_active (self->x11_session)) {
    g_debug ("Creating X11 session source");
    source = nd_application_window_stream_create_source (self->x11_session);
  } else if (self->selected_video_file) {
    g_debug ("Creating video file source for: %s", self->selected_video_file);
    /* Free any previous stream before starting a new one */
    if (self->video_file_stream)
      nd_video_file_stream_free (self->video_file_stream);
    self->video_file_stream = nd_video_file_stream_new (self->selected_video_file);
    source = nd_video_file_stream_get_source (self->video_file_stream);
    if (source)
      nd_video_file_stream_initialize_global_references (self->video_file_stream, source);
  } else {
    g_warning ("NdWindow: No content selected for streaming!");
  }

  return source;
}

static GstElement *
sink_create_audio_source_cb (NdWindow * self, NdSink * sink)
{
  GstElement *res;

  /* First, try to get audio from the video file via interaudiosrc */
  if (self->video_file_stream) {
    res = nd_video_file_stream_create_audio_source (self->video_file_stream);
    if (res) {
      g_debug ("NdWindow: Using inter audio source for video file audio");
      return res;
    }
  }

  /* Fallback to PulseAudio if inter audio source is not available */
  if (!self->pulse) {
    g_debug ("NdWindow: No PulseAudio available for audio source");
    return NULL;
  }

  res = nd_pulseaudio_get_source (self->pulse);

  /* Configure PulseAudio source for low latency if it's a pulsesrc element */
  if (res && GST_IS_ELEMENT (res)) {
    const gchar *factory_name = GST_OBJECT_NAME (gst_element_get_factory (res));
    if (g_strcmp0 (factory_name, "pulsesrc") == 0)
      g_object_set (res,
                    "latency-time", 10000,    /* 10ms latency */
                    "buffer-time", 20000,     /* 20ms buffer */
                    "provide-clock", FALSE,   /* Don't provide clock */
                    NULL);
  }

  g_debug ("NdWindow: Using PulseAudio source as fallback");

  return g_object_ref_sink (res);
}

static void
sink_notify_state_cb (NdWindow *self, GParamSpec *pspec, NdSink *sink)
{
  NdSinkState state;

  g_object_get (sink, "state", &state, NULL);
  g_debug ("Got state change notification from streaming sink to state %s",
           g_enum_to_string (ND_TYPE_SINK_STATE, state));

  if (state == ND_SINK_STATE_STREAMING) {
    GObjectClass *sink_class = G_OBJECT_GET_CLASS (sink);

    if (g_object_class_find_property (sink_class, "max-lateness"))
      g_object_set (sink, "max-lateness", (gint64) 16666667, NULL);  /* ~16ms */
    if (g_object_class_find_property (sink_class, "qos"))
      g_object_set (sink, "qos", TRUE, NULL);
    if (g_object_class_find_property (sink_class, "processing-deadline"))
      g_object_set (sink, "processing-deadline", (gint64) 20000000, NULL);  /* 20ms */
  }

  switch (state)
    {
    case ND_SINK_STATE_ENSURE_FIREWALL:
      gtk_label_set_text (self->connect_state_label,
                          _("Checking and installing required firewall zones."));

      gtk_stack_set_visible_child_name (self->step_stack, "connect");
      break;

    case ND_SINK_STATE_WAIT_P2P:
      gtk_label_set_text (self->connect_state_label,
                          _("Making P2P connection"));

      gtk_stack_set_visible_child_name (self->step_stack, "connect");
      break;

    case ND_SINK_STATE_WAIT_SOCKET:
      gtk_label_set_text (self->connect_state_label,
                          _("Establishing connection to sink"));

      gtk_stack_set_visible_child_name (self->step_stack, "connect");
      break;

    case ND_SINK_STATE_WAIT_STREAMING:
      gtk_label_set_text (self->connect_state_label,
                          _("Starting to stream"));

      gtk_stack_set_visible_child_name (self->step_stack, "connect");
      break;

    case ND_SINK_STATE_STREAMING:
      g_list_store_remove_all (self->connect_sink_list_model);
      g_list_store_remove_all (self->stream_sink_list_model);
      g_list_store_remove_all (self->error_sink_list_model);

      g_list_store_append (self->stream_sink_list_model, self->stream_sink);

      /* Show/hide media controls based on content type */
      if (self->selected_video_file) {
        self->is_video_file_streaming = TRUE;
        gtk_widget_set_visible (GTK_WIDGET (self->media_controls_box), TRUE);

        /* Start media controls update timer */
        if (self->media_update_timeout_id == 0)
          self->media_update_timeout_id = g_timeout_add (1000, update_media_controls, self);

        /* Initialize volume scale and ensure playback starts */
        gdouble volume = nd_video_file_stream_global_get_volume (self->video_file_stream);
        if (self->volume_scale)
          gtk_range_set_value (GTK_RANGE (self->volume_scale), volume * 100.0);

        /* Force playback to start */
        nd_video_file_stream_global_play (self->video_file_stream);
      } else {
        self->is_video_file_streaming = FALSE;
        gtk_widget_set_visible (GTK_WIDGET (self->media_controls_box), FALSE);
      }

      gtk_stack_set_visible_child_name (self->step_stack, "stream");
      break;

    case ND_SINK_STATE_ERROR:
      g_list_store_remove_all (self->connect_sink_list_model);
      g_list_store_remove_all (self->stream_sink_list_model);
      g_list_store_remove_all (self->error_sink_list_model);

      g_list_store_append (self->error_sink_list_model, self->stream_sink);

      gtk_stack_set_visible_child_name (self->step_stack, "error");
      break;

    case ND_SINK_STATE_DISCONNECTED:
      g_list_store_remove_all (self->connect_sink_list_model);
      g_list_store_remove_all (self->stream_sink_list_model);
      g_list_store_remove_all (self->error_sink_list_model);

      /* Clean up X11 session if it's running */
      if (nd_x11_session_is_active (self->x11_session))
        nd_x11_session_stop (self->x11_session);

      /* Clean up media controls */
      self->is_video_file_streaming = FALSE;
      gtk_widget_set_visible (GTK_WIDGET (self->media_controls_box), FALSE);
      if (self->media_update_timeout_id != 0) {
        g_source_remove (self->media_update_timeout_id);
        self->media_update_timeout_id = 0;
      }

      /* Clean up video file stream */
      if (self->video_file_stream) {
        nd_video_file_stream_free (self->video_file_stream);
        self->video_file_stream = NULL;
      }

      gtk_stack_set_visible_child_name (self->step_stack, "find");
      g_object_set (self->meta_provider, "discover", TRUE, NULL);

      g_ptr_array_set_size (self->sink_property_bindings, 0);

      g_signal_handlers_disconnect_by_data (self->stream_sink, self);
      g_clear_object (&self->stream_sink);
      break;
    }
}

gboolean
transform_str_is_set_to_bool (GBinding     *binding,
                              const GValue *from_value,
                              GValue       *to_value,
                              gpointer      user_data)
{
  g_value_set_boolean (to_value, g_value_get_string (from_value) != NULL);

  return TRUE;
}

static void
setup_streaming_with_pending_sink (NdWindow *self)
{
  if (!self->pending_sink) {
    g_warning ("NdWindow: No pending sink available for streaming!");
    return;
  }

  self->stream_sink = nd_sink_start_stream (self->pending_sink);

  if (!self->stream_sink) {
    g_warning ("NdWindow: Could not start streaming!");
    g_clear_object (&self->pending_sink);
    return;
  }

  g_signal_connect_object (self->stream_sink,
                           "create-source",
                           (GCallback) sink_create_source_cb,
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (self->stream_sink,
                           "create-audio-source",
                           (GCallback) sink_create_audio_source_cb,
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (self->stream_sink,
                           "notify::state",
                           (GCallback) sink_notify_state_cb,
                           self,
                           G_CONNECT_SWAPPED);

  /* We might have moved into the error state in the meantime. */
  sink_notify_state_cb (self, NULL, self->stream_sink);

  g_ptr_array_add (self->sink_property_bindings,
                   g_object_ref (g_object_bind_property (self->stream_sink,
                                                         "missing-video-codec",
                                                         self->codec_install_video,
                                                         "codecs",
                                                         G_BINDING_SYNC_CREATE)));

  g_ptr_array_add (self->sink_property_bindings,
                   g_object_ref (g_object_bind_property (self->stream_sink,
                                                         "missing-audio-codec",
                                                         self->codec_install_audio,
                                                         "codecs",
                                                         G_BINDING_SYNC_CREATE)));

  g_ptr_array_add (self->sink_property_bindings,
                   g_object_ref (g_object_bind_property_full (self->stream_sink,
                                                              "missing-firewall-zone",
                                                              self->error_firewall_zone,
                                                              "reveal-child",
                                                              G_BINDING_SYNC_CREATE,
                                                              transform_str_is_set_to_bool,
                                                              NULL,
                                                              NULL,
                                                              NULL)));

  g_object_set (self->meta_provider, "discover", FALSE, NULL);
  g_list_store_append (self->connect_sink_list_model, self->stream_sink);

  g_clear_object (&self->pending_sink);
}

static void
on_file_dialog_response (GObject *source_object,
                         GAsyncResult *res,
                         gpointer user_data)
{
  GtkFileDialog *dialog = GTK_FILE_DIALOG (source_object);
  NdWindow *self = ND_WINDOW (user_data);
  g_autoptr(GError) error = NULL;
  GFile *file;

  file = gtk_file_dialog_open_finish (dialog, res, &error);

  if (file) {
    g_free (self->selected_video_file);
    self->selected_video_file = g_file_get_path (file);
    g_object_unref (file);

    g_debug ("NdWindow: Selected video file: %s", self->selected_video_file);

    /* Now proceed with streaming using the pending sink */
    setup_streaming_with_pending_sink (self);
  } else {
    /* User cancelled or error occurred, clean up pending sink */
    if (error && !g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED))
      g_warning ("NdWindow: Error opening file dialog: %s", error->message);
    g_clear_object (&self->pending_sink);
  }
}

static void
show_file_chooser_dialog (NdWindow *self)
{
  GtkFileDialog *dialog;
  GtkFileFilter *filter;
  GListStore *filters;

  dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, _("Select Video File"));

  /* Create file filters */
  filters = g_list_store_new (GTK_TYPE_FILE_FILTER);

  /* Add video file filter */
  filter = gtk_file_filter_new ();
  gtk_file_filter_set_name (filter, _("Video files"));
  gtk_file_filter_add_mime_type (filter, "video/*");

  /* Add common video formats */
  gtk_file_filter_add_pattern (filter, "*.mp4");
  gtk_file_filter_add_pattern (filter, "*.avi");
  gtk_file_filter_add_pattern (filter, "*.mkv");
  gtk_file_filter_add_pattern (filter, "*.mov");
  gtk_file_filter_add_pattern (filter, "*.wmv");
  gtk_file_filter_add_pattern (filter, "*.flv");
  gtk_file_filter_add_pattern (filter, "*.webm");
  gtk_file_filter_add_pattern (filter, "*.ogv");
  g_list_store_append (filters, filter);
  g_object_unref (filter);

  /* Add "All files" filter */
  filter = gtk_file_filter_new ();
  gtk_file_filter_set_name (filter, _("All files"));
  gtk_file_filter_add_pattern (filter, "*");
  g_list_store_append (filters, filter);
  g_object_unref (filter);

  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  g_object_unref (filters);

  gtk_file_dialog_open (dialog,
                        GTK_WINDOW (self),
                        NULL, /* cancellable */
                        on_file_dialog_response,
                        self);

  g_object_unref (dialog);
}

static void
select_x11_session_button_clicked_cb (NdWindow *self)
{
  if (!nd_x11_session_start (self->x11_session)) {
    GtkAlertDialog *dialog = gtk_alert_dialog_new (_("Failed to start X11 session"));
    gtk_alert_dialog_set_detail (dialog, _("Could not start Xvfb. Please ensure Xvfb and openbox are installed."));
    gtk_alert_dialog_show (dialog, GTK_WINDOW (self));
    g_object_unref (dialog);
    return;
  }

  /* Proceed with streaming the X11 session */
  setup_streaming_with_pending_sink (self);
}

static void
select_video_file_button_clicked_cb (NdWindow *self)
{
  show_file_chooser_dialog (self);
}

static void
select_content_back_button_clicked_cb (NdWindow *self)
{
  /* Go back to device selection and clean up pending sink */
  g_clear_object (&self->pending_sink);
  gtk_stack_set_visible_child_name (self->step_stack, "find");
}

static void
find_sink_list_row_activated_cb (NdWindow *self, NdSinkRow *row, GtkListBox *sink_list)
{
  NdSink *sink;

  g_assert (ND_IS_SINK_ROW (row));

  sink = nd_sink_row_get_sink (row);

  /* Store the sink for later use and show content selection page */
  self->pending_sink = g_object_ref (sink);
  gtk_stack_set_visible_child_name (self->step_stack, "select-content");
}

static void
on_mtk_wifi_manager_refresh_p2p_cb (GObject      *source_object,
                                    GAsyncResult *res,
                                    gpointer      user_data)
{
  NdMtkWifiManager *wifi_manager = ND_MTK_WIFI_MANAGER (source_object);
  g_autoptr(GError) error = NULL;
  gboolean success;

  success = nd_mtk_wifi_manager_refresh_p2p_finish (wifi_manager, res, &error);

  if (success)
    g_debug ("NdWindow: P2P refresh completed successfully");
  else
    g_debug ("NdWindow: P2P refresh failed: %s", error ? error->message : "Unknown error");
}

static gboolean
p2p_refresh_timeout (gpointer user_data)
{
  NdMtkWifiManager *wifi_manager = ND_MTK_WIFI_MANAGER (user_data);

  g_debug ("NdWindow: Starting P2P refresh");

  nd_mtk_wifi_manager_refresh_p2p_async (wifi_manager,
                                         on_mtk_wifi_manager_refresh_p2p_cb,
                                         NULL);

  return G_SOURCE_REMOVE;
}

static void
on_mtk_wifi_manager_init_cb (GObject      *source_object,
                             GAsyncResult *res,
                             gpointer      user_data)
{
  NdMtkWifiManager *wifi_manager = ND_MTK_WIFI_MANAGER (source_object);
  NdWindow *self = ND_WINDOW (user_data);
  g_autoptr(GError) error = NULL;

  if (!nd_mtk_wifi_manager_init_finish (wifi_manager, res, &error)) {
    if (error)
      g_debug ("NdWindow: Failed to initialize MediaTek WiFi manager: %s", error->message);
    return;
  }

  if (nd_mtk_wifi_manager_is_available (wifi_manager)) {
    g_debug ("NdWindow: MediaTek WiFi manager initialized successfully");

    /* Update the device registry with the initialized MTK WiFi manager */
    nd_nm_device_registry_set_mtk_wifi_manager (self->nm_device_registry, wifi_manager);

    /* Ensure P2P mode is active */
    nd_mtk_wifi_manager_ensure_p2p_mode (wifi_manager);

    /* Schedule P2P refresh after 3 seconds to allow mode switch to complete */
    g_timeout_add_seconds (3, p2p_refresh_timeout, wifi_manager);
  } else {
    g_debug ("NdWindow: MediaTek WiFi manager not available on this system");
  }
}

static void
nd_window_constructed (GObject *obj)
{
  G_OBJECT_CLASS (nd_window_parent_class)->constructed (obj);

  g_autoptr(GError) error = NULL;
  g_autoptr(NdWFDMiceProvider) mice_provider = NULL;
  g_autoptr(NdCCProvider) cc_provider = NULL;
  NdWindow *self = ND_WINDOW (obj);

  self->cancellable = g_cancellable_new ();
  self->avahi_client = ga_client_new (GA_CLIENT_FLAG_NO_FLAGS);

  self->mtk_wifi_manager = nd_mtk_wifi_manager_new ();
  nd_mtk_wifi_manager_init_async (self->mtk_wifi_manager,
                                  on_mtk_wifi_manager_init_cb,
                                  self);

  if (!ga_client_start (self->avahi_client, &error))
    {
      g_warning ("NdWindow: Failed to start Avahi Client");
      if (error != NULL)
        g_warning ("NdWindow: Error: %s", error->message);
      return;
    }

  g_debug ("NdWindow: Got avahi client");

  mice_provider = nd_wfd_mice_provider_new (self->avahi_client);
  cc_provider = nd_cc_provider_new (self->avahi_client);

  if (!nd_wfd_mice_provider_browse (mice_provider, error) || !nd_cc_provider_browse (cc_provider, error))
    {
      g_warning ("NdWindow: Avahi client failed to browse: %s", error->message);
      return;
    }

  g_debug ("NdWindow: Got avahi browser");
  nd_meta_provider_add_provider (self->meta_provider, ND_PROVIDER (mice_provider));
  nd_meta_provider_add_provider (self->meta_provider, ND_PROVIDER (cc_provider));
  if (g_strcmp0 (g_getenv ("NETWORK_DISPLAYS_DUMMY"), "1") == 0)
    {
      g_autoptr(NdDummyProvider) dummy_provider = NULL;

      g_debug ("Adding dummy provider");
      dummy_provider = nd_dummy_provider_new ();
      nd_meta_provider_add_provider (self->meta_provider, ND_PROVIDER (dummy_provider));
    }
}

static void
nd_window_finalize (GObject *obj)
{
  NdWindow *self = ND_WINDOW (obj);

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);

  /* Clean up PulseAudio properly to remove Network-Displays sink */
  if (self->pulse) {
    nd_pulseaudio_unload (self->pulse);
    g_clear_object (&self->pulse);
  }

  /* Clean up MediaTek WiFi Manager (this will restore AP mode) */
  if (self->mtk_wifi_manager) {
    nd_mtk_wifi_manager_restore_ap_mode (self->mtk_wifi_manager);
    g_clear_object (&self->mtk_wifi_manager);
  }

  /* Clean up media controls */
  if (self->media_update_timeout_id != 0) {
    g_source_remove (self->media_update_timeout_id);
    self->media_update_timeout_id = 0;
  }

  /* Clean up video file stream */
  if (self->video_file_stream)
    nd_video_file_stream_free (self->video_file_stream);

  /* Clean up X11 session */
  g_clear_pointer (&self->x11_session, nd_x11_session_free);

  g_clear_object (&self->stream_sink);
  g_clear_object (&self->pending_sink);
  g_free (self->selected_video_file);

  g_clear_object (&self->meta_provider);
  g_clear_object (&self->nm_device_registry);
  g_clear_object (&self->avahi_client);

  g_clear_pointer (&self->sink_property_bindings, g_ptr_array_unref);

  G_OBJECT_CLASS (nd_window_parent_class)->finalize (obj);
}

static void
nd_window_dispose (GObject *obj)
{
  NdWindow *self = ND_WINDOW (obj);

  g_object_run_dispose (G_OBJECT (self->avahi_client));

  G_OBJECT_CLASS (nd_window_parent_class)->dispose (obj);
}

static void
nd_window_class_init (NdWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->constructed = nd_window_constructed;
  object_class->finalize = nd_window_finalize;
  object_class->dispose = nd_window_dispose;

  ND_TYPE_CODEC_INSTALL;

  gtk_widget_class_set_template_from_resource (widget_class, "/io/furios/WiDiStream/nd-window.ui");
  gtk_widget_class_bind_template_child (widget_class, NdWindow, has_providers_stack);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, step_stack);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, find_sink_list);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, select_video_file_button);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, select_content_back_button);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, select_x11_session_button);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, connect_sink_list);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, connect_state_label);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, connect_cancel);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, stream_sink_list);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, stream_state_label);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, codec_install_audio);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, codec_install_video);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, stream_cancel);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, error_sink_list);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, error_firewall_zone);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, error_return);

  /* Media control widgets */
  gtk_widget_class_bind_template_child (widget_class, NdWindow, media_controls_box);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, current_time_label);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, total_time_label);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, progress_scale);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, play_pause_button);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, previous_button);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, next_button);
  gtk_widget_class_bind_template_child (widget_class, NdWindow, volume_scale);
}

static void
nd_pulseaudio_init_async_cb (GObject      *source_object,
                             GAsyncResult *res,
                             gpointer      user_data)
{
  NdWindow *window;

  g_autoptr(GError) error = NULL;

  if (!g_async_initable_init_finish (G_ASYNC_INITABLE (source_object), res, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Error initializing pulse audio sink: %s", error->message);

      g_object_unref (source_object);
      return;
    }

  window = ND_WINDOW (user_data);
  window->pulse = ND_PULSEAUDIO (source_object);
}

static void
stream_stop_clicked_cb (NdWindow *self)
{
  if (!self->stream_sink)
    return;

  nd_sink_stop_stream (self->stream_sink);
}

static void
on_meta_provider_has_provider_changed_cb (NdWindow *self, GParamSpec *pspec, NdMetaProvider *provider)
{
  gboolean has_providers;

  g_object_get (self->meta_provider,
                "has-providers", &has_providers,
                NULL);
  gtk_stack_set_visible_child_name (self->has_providers_stack,
                                    has_providers ? "has-providers" : "no-providers");
}

static void
nd_window_init (NdWindow *self)
{
  g_autoptr(GError) error = NULL;
  NdPulseaudio *pulse;

  g_debug ("WiDiStream v%s started", PACKAGE_VERSION);

  gtk_widget_init_template (GTK_WIDGET (self));

  /* Initialize new fields */
  self->pending_sink = NULL;
  self->selected_video_file = NULL;
  self->x11_session = nd_x11_session_new ();
  self->is_video_file_streaming = FALSE;
  self->media_update_timeout_id = 0;
  self->video_file_stream = NULL;

  self->meta_provider = nd_meta_provider_new ();
  g_signal_connect_object (self->meta_provider,
                           "notify::has-providers",
                           (GCallback) on_meta_provider_has_provider_changed_cb,
                           self,
                           G_CONNECT_SWAPPED);

  self->nm_device_registry = nd_nm_device_registry_new (self->meta_provider, self->mtk_wifi_manager);

  self->connect_sink_list_model = g_list_store_new (ND_TYPE_SINK);
  self->stream_sink_list_model = g_list_store_new (ND_TYPE_SINK);
  self->error_sink_list_model = g_list_store_new (ND_TYPE_SINK);
  self->find_sink_list_model = nd_sink_list_model_new (ND_PROVIDER (self->meta_provider));

  gtk_list_box_bind_model (self->connect_sink_list,
                           G_LIST_MODEL (self->connect_sink_list_model),
                           (GtkListBoxCreateWidgetFunc) nd_sink_row_new,
                           NULL,
                           NULL);
  gtk_list_box_bind_model (self->stream_sink_list,
                           G_LIST_MODEL (self->stream_sink_list_model),
                           (GtkListBoxCreateWidgetFunc) nd_sink_row_new,
                           NULL,
                           NULL);
  gtk_list_box_bind_model (self->error_sink_list,
                           G_LIST_MODEL (self->error_sink_list_model),
                           (GtkListBoxCreateWidgetFunc) nd_sink_row_new,
                           NULL,
                           NULL);
  gtk_list_box_bind_model (self->find_sink_list,
                           G_LIST_MODEL (self->find_sink_list_model),
                           (GtkListBoxCreateWidgetFunc) nd_sink_row_new,
                           NULL,
                           NULL);

  g_signal_connect_object (self->find_sink_list,
                           "row-activated",
                           (GCallback) find_sink_list_row_activated_cb,
                           self,
                           G_CONNECT_SWAPPED);

  /* Connect content selection page signals */
  g_signal_connect_object (self->select_video_file_button,
                           "clicked",
                           (GCallback) select_video_file_button_clicked_cb,
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (self->select_content_back_button,
                           "clicked",
                           (GCallback) select_content_back_button_clicked_cb,
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (self->select_x11_session_button,
                           "clicked",
                           (GCallback) select_x11_session_button_clicked_cb,
                           self,
                           G_CONNECT_SWAPPED);

  self->cancellable = g_cancellable_new ();

  /* All of these buttons just stop the stream, which will return us
   * to the DISCONNECTED state and the selection page. */
  g_signal_connect_object (self->connect_cancel,
                           "clicked",
                           (GCallback) stream_stop_clicked_cb,
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (self->stream_cancel,
                           "clicked",
                           (GCallback) stream_stop_clicked_cb,
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (self->error_return,
                           "clicked",
                           (GCallback) stream_stop_clicked_cb,
                           self,
                           G_CONNECT_SWAPPED);

  if (self->play_pause_button)
    g_signal_connect (self->play_pause_button,
                      "clicked",
                      G_CALLBACK (on_play_pause_button_clicked),
                      self);

  if (self->previous_button)
    g_signal_connect (self->previous_button,
                      "clicked",
                      G_CALLBACK (on_previous_button_clicked),
                      self);

  if (self->next_button)
    g_signal_connect (self->next_button,
                      "clicked",
                      G_CALLBACK (on_next_button_clicked),
                      self);

  if (self->progress_scale)
    g_signal_connect (self->progress_scale,
                      "value-changed",
                      G_CALLBACK (on_progress_scale_value_changed),
                      self);

  if (self->volume_scale)
    g_signal_connect (self->volume_scale,
                      "value-changed",
                      G_CALLBACK (on_volume_scale_value_changed),
                      self);

  pulse = nd_pulseaudio_new ();
  g_async_initable_init_async (G_ASYNC_INITABLE (pulse),
                               G_PRIORITY_LOW,
                               self->cancellable,
                               nd_pulseaudio_init_async_cb,
                               self);

  self->sink_property_bindings = g_ptr_array_new_full (0, (GDestroyNotify) g_binding_unbind);
}

NdWindow *
nd_window_new (void)
{
  return g_object_new (ND_TYPE_WINDOW, NULL);
}
