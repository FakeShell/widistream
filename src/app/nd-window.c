/* gnome-nd-window.c
 *
 * Copyright 2018 Benjamin Berg <bberg@redhat.com>
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
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
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
  GPid                   xvfb_pid;
  gchar                 *x11_display;
  gboolean               x11_session_active;

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

G_DEFINE_TYPE (NdWindow, gnome_nd_window, ADW_TYPE_APPLICATION_WINDOW)

static gboolean
start_x11_session (NdWindow *self)
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

  /* Try to start Xvfb */
  if (!g_spawn_async (NULL, xvfb_command, NULL,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                      NULL, NULL, &self->xvfb_pid, &error)) {
    g_warning ("Failed to start Xvfb: %s", error->message);
    g_error_free (error);
    return FALSE;
  }

  self->x11_display = g_strdup (":99");
  self->x11_session_active = TRUE;

  g_debug ("Started Xvfb with PID %d on display %s", self->xvfb_pid, self->x11_display);

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

static void
stop_x11_session (NdWindow *self)
{
  if (!self->x11_session_active)
    return;

  g_debug ("Stopping X11 session on display %s", self->x11_display);

  /* Kill Xvfb process */
  if (self->xvfb_pid > 0) {
    kill (self->xvfb_pid, SIGTERM);
    g_spawn_close_pid (self->xvfb_pid);
    self->xvfb_pid = 0;
  }

  /* Kill any remaining processes on the display */
  gchar *kill_command[] = {
    "sh", "-c",
    "pkill -f 'DISPLAY=:99'",
    NULL
  };

  g_spawn_async (NULL, kill_command, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);

  g_free (self->x11_display);
  self->x11_display = NULL;
  self->x11_session_active = FALSE;
}

static GstElement *
create_x11_source (NdWindow *self)
{
  GstElement *source, *videoscale, *videoconvert;
  GstBin *bin;
  GstCaps *caps;

  if (!self->x11_session_active) {
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
                "display-name", self->x11_display,
                "use-damage", FALSE,  /* Capture full frames */
                "show-pointer", TRUE,
                NULL);

  /* Add video conversion and scaling */
  videoconvert = gst_element_factory_make ("videoconvert", "x11-convert");
  videoscale = gst_element_factory_make ("videoscale", "x11-scale");

  if (!videoconvert || !videoscale) {
    g_warning ("Failed to create video processing elements");
    gst_object_unref (bin);
    return NULL;
  }

  /* Add elements to bin */
  gst_bin_add_many (bin, source, videoconvert, videoscale, NULL);

  /* Create caps for consistent output */
  caps = gst_caps_from_string ("video/x-raw,width=1920,height=1080,framerate=30/1");

  /* Link elements */
  if (!gst_element_link (source, videoconvert) ||
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

static GstElement *
create_video_file_source (NdWindow *self)
{
  GstElement *playbin, *videosink, *audiosink, *res;
  GstBin *bin;
  gchar *uri;

  if (!self->selected_video_file) {
    g_warning ("NdWindow: No video file selected!");
    return NULL;
  }

  bin = GST_BIN (gst_bin_new ("playbin-wrapper-bin"));

  playbin = gst_element_factory_make ("playbin", "video-player");

  /* Convert file path to URI */
  uri = g_filename_to_uri (self->selected_video_file, NULL, NULL);
  if (!uri) {
    g_warning ("NdWindow: Failed to convert file path to URI: %s", self->selected_video_file);
    gst_object_unref (bin);
    return NULL;
  }

  g_object_set (playbin, "uri", uri, NULL);
  g_free (uri);

  /* Set up video sink */
  videosink = gst_element_factory_make ("intervideosink", "inter video sink");
  g_object_set (videosink,
                "channel", "nd-inter-video",
                "max-lateness", (gint64) -1,
                "sync", TRUE,
                NULL);

  /* Set up audio sink for wireless display */
  audiosink = gst_element_factory_make ("interaudiosink", "inter audio sink");
  g_object_set (audiosink,
                "channel", "nd-inter-audio",
                "sync", TRUE,
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
                "do-timestamp", FALSE,
                "timeout", (guint64) G_MAXUINT64,
                "channel", "nd-inter-video",
                NULL);

  gst_bin_add (bin, res);

  gst_element_add_pad (GST_ELEMENT (bin),
                       gst_ghost_pad_new ("src",
                                          gst_element_get_static_pad (res, "src")));

  g_object_ref_sink (bin);
  return GST_ELEMENT (bin);
}

static GstElement *
sink_create_source_cb (NdWindow *self, NdSink *sink)
{
  /* Check if we're streaming X11 session or video file */
  if (self->x11_session_active) {
    g_debug ("Creating X11 session source");
    return create_x11_source (self);
  } else if (self->selected_video_file) {
    g_debug ("Creating video file source");
    return create_video_file_source (self);
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
  res = gst_element_factory_make ("interaudiosrc", "inter audio src");
  if (res) {
    g_object_set (res,
                  "channel", "nd-inter-audio",
                  NULL);
    g_debug ("NdWindow: Using inter audio source for video file audio");
    return g_object_ref_sink (res);
  }

  /* Fallback to PulseAudio if inter audio source is not available */
  if (!self->pulse) {
    g_debug ("NdWindow: No PulseAudio available for audio source");
    return NULL;
  }

  res = nd_pulseaudio_get_source (self->pulse);
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
      if (self->x11_session_active)
        stop_x11_session (self);

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
    if (self->pending_sink) {
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
  if (!start_x11_session (self)) {
    GtkAlertDialog *dialog = gtk_alert_dialog_new (_("Failed to start X11 session"));
    gtk_alert_dialog_set_detail (dialog, _("Could not start Xvfb. Please ensure Xvfb and openbox are installed."));
    gtk_alert_dialog_show (dialog, GTK_WINDOW (self));
    g_object_unref (dialog);
    return;
  }

  /* Proceed with streaming the X11 session */
  if (self->pending_sink) {
    self->stream_sink = nd_sink_start_stream (self->pending_sink);

    if (!self->stream_sink) {
      g_warning ("NdWindow: Could not start streaming!");
      stop_x11_session (self);
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
gnome_nd_window_constructed (GObject *obj)
{
  G_OBJECT_CLASS (gnome_nd_window_parent_class)->constructed (obj);

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
gnome_nd_window_finalize (GObject *obj)
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
  if (self->x11_session_active)
    stop_x11_session (self);

  g_clear_object (&self->stream_sink);
  g_clear_object (&self->pending_sink);
  g_free (self->selected_video_file);

  g_clear_object (&self->meta_provider);
  g_clear_object (&self->nm_device_registry);
  g_clear_object (&self->avahi_client);

  g_clear_pointer (&self->sink_property_bindings, g_ptr_array_unref);

  G_OBJECT_CLASS (gnome_nd_window_parent_class)->finalize (obj);
}

static void
gnome_nd_window_dispose (GObject *obj)
{
  NdWindow *self = ND_WINDOW (obj);

  g_object_run_dispose (G_OBJECT (self->avahi_client));

  G_OBJECT_CLASS (gnome_nd_window_parent_class)->dispose (obj);
}

static void
gnome_nd_window_class_init (NdWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->constructed = gnome_nd_window_constructed;
  object_class->finalize = gnome_nd_window_finalize;
  object_class->dispose = gnome_nd_window_dispose;

  ND_TYPE_CODEC_INSTALL;

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/NetworkDisplays/nd-window.ui");
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
gnome_nd_window_init (NdWindow *self)
{
  g_autoptr(GError) error = NULL;
  NdPulseaudio *pulse;

  g_debug ("WiDiStream v%s started", PACKAGE_VERSION);

  gtk_widget_init_template (GTK_WIDGET (self));

  /* Initialize new fields */
  self->pending_sink = NULL;
  self->selected_video_file = NULL;
  self->xvfb_pid = 0;
  self->x11_display = NULL;
  self->x11_session_active = FALSE;

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
