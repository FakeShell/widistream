/* gnome-nd-window.c
 *
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
};

G_DEFINE_FINAL_TYPE (NdWindow, nd_window, ADW_TYPE_APPLICATION_WINDOW)

static GstElement *
sink_create_source_cb (NdWindow *self, NdSink *sink)
{
  /* Check if we're streaming X11 session or video file */
  if (nd_x11_session_is_active (self->x11_session)) {
    g_debug ("Creating X11 session source");
    return nd_application_window_stream_create_source (self->x11_session);
  } else if (self->selected_video_file) {
    g_debug ("Creating video file source");
    return nd_video_file_stream_create_source (self->selected_video_file);
  } else {
    g_warning ("NdWindow: No content selected for streaming!");
    return NULL;
  }
}

static GstElement *
sink_create_audio_source_cb (NdWindow * self, NdSink * sink)
{
  GstElement *res;

  /* First, try to get audio from the video file via interaudiosrc */
  res = nd_video_file_stream_create_audio_source ();
  if (res) {
    g_debug ("NdWindow: Using inter audio source for video file audio");
    return res;
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
nd_window_constructed (GObject *obj)
{
  G_OBJECT_CLASS (nd_window_parent_class)->constructed (obj);

  g_autoptr(GError) error = NULL;
  g_autoptr(NdWFDMiceProvider) mice_provider = NULL;
  g_autoptr(NdCCProvider) cc_provider = NULL;
  NdWindow *self = ND_WINDOW (obj);

  self->cancellable = g_cancellable_new ();
  self->avahi_client = ga_client_new (GA_CLIENT_FLAG_NO_FLAGS);

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

  self->meta_provider = nd_meta_provider_new ();
  g_signal_connect_object (self->meta_provider,
                           "notify::has-providers",
                           (GCallback) on_meta_provider_has_provider_changed_cb,
                           self,
                           G_CONNECT_SWAPPED);

  self->nm_device_registry = nd_nm_device_registry_new (self->meta_provider);

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
