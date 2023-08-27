#include "gstrestartsrc.h"


#define RESTART_TIMEOUT_DEFAULT   3 * GST_SECOND

GST_DEBUG_CATEGORY_STATIC (gst_restart_src_debug);
#define GST_CAT_DEFAULT gst_restart_src_debug

enum {
  PROP_0,
  PROP_SRC,
  PROP_RESTART_TIMEOUT,
};

struct _GstRestartSrc {
  GstBin element;

  GstElement * src;
  GstPad * ghost_srcpad;
  GstClockID timeout_shot_id;

  gint pending_restart;
  GstClockTime restart_timeout;
};

G_DEFINE_TYPE(GstRestartSrc, gst_restart_src, GST_TYPE_BIN)

static GstStaticPadTemplate srcpad_template =
    GST_STATIC_PAD_TEMPLATE(
        "src",
        GST_PAD_SRC,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS_ANY);

static void gst_restart_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_restart_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static void gst_restart_src_handle_message (GstBin * bin, GstMessage * msg);
static GstStateChangeReturn gst_restart_src_change_state (GstElement * element,
    GstStateChange transition);

static void gst_restart_src_handle_error (GstRestartSrc * restart_src);
static gboolean gst_restart_src_restart_timeout (GstClock * clock, GstClockTime time,
    GstClockID id, gpointer user_data);
static GstPadProbeReturn gst_restart_src_probe_drop_eos (GstPad * pad,
    GstPadProbeInfo * info, gpointer user_data);

static void
gst_restart_src_init (GstRestartSrc * restart_src) {
  GstBin * bin = GST_BIN(restart_src);
  restart_src->src = NULL;
  restart_src->pending_restart = FALSE;
  restart_src->restart_timeout = RESTART_TIMEOUT_DEFAULT;

  restart_src->ghost_srcpad = gst_ghost_pad_new_no_target("src", GST_PAD_SRC);
  gst_pad_add_probe (restart_src->ghost_srcpad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      gst_restart_src_probe_drop_eos, NULL, NULL);
  g_assert (gst_element_add_pad (GST_ELEMENT(restart_src), restart_src->ghost_srcpad));
}

static void
gst_restart_src_class_init (GstRestartSrcClass * restart_src_cls) {
  GST_DEBUG_CATEGORY_INIT(gst_restart_src_debug, "restartsrc", 0, "Autorestart source wrapper");

  GObjectClass * object_cls = G_OBJECT_CLASS (restart_src_cls);
  object_cls->get_property = gst_restart_src_get_property;
  object_cls->set_property = gst_restart_src_set_property;

  g_object_class_install_property (object_cls, PROP_SRC,
      g_param_spec_pointer ("src", "Source element",
          "Pointer to source element",
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  g_object_class_install_property (object_cls, PROP_RESTART_TIMEOUT,
      g_param_spec_uint64 ("restart-timeout", "Restart timeout",
          "Source element will be restarted after timeout in case of failure",
          0, G_MAXUINT64, RESTART_TIMEOUT_DEFAULT,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  GstElementClass * element_cls = GST_ELEMENT_CLASS(restart_src_cls);
  element_cls->change_state = gst_restart_src_change_state;

  gst_element_class_add_pad_template (element_cls,
      gst_static_pad_template_get (&srcpad_template));

  GstBinClass * bin_cls = GST_BIN_CLASS(restart_src_cls);
  bin_cls->handle_message = gst_restart_src_handle_message;

  gst_element_class_set_static_metadata (element_cls,
                                         "Restartsrc plugin",
                                         "classification",
                                         "description",
                                         "author");

}

static void
gst_restart_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec) {
  GstRestartSrc * restart_src = GST_RESTART_SRC(object);

  switch (prop_id) {
    case PROP_SRC:
      if (restart_src->src != NULL) {
        gst_ghost_pad_set_target (restart_src->ghost_srcpad, NULL);

        gst_element_set_locked_state(restart_src->src, FALSE);
        gst_bin_remove (GST_BIN (object), restart_src->src);
      }

      restart_src->src = g_value_get_pointer (value);
      break;

    case PROP_RESTART_TIMEOUT:
      restart_src->restart_timeout = g_value_get_uint64 (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_restart_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec) {
  GstRestartSrc * restart_src = GST_RESTART_SRC(object);

  switch (prop_id) {
    case PROP_SRC:
      g_value_set_pointer (value, restart_src->src);
      break;

    case PROP_RESTART_TIMEOUT:
      g_value_set_uint64 (value, restart_src->restart_timeout);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_restart_src_handle_message (GstBin * bin, GstMessage * msg) {
  GstRestartSrc * restart_src = GST_RESTART_SRC (bin);

  if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR &&
      GST_ELEMENT(msg->src) == restart_src->src) {

    gst_restart_src_handle_error (restart_src);
    gst_message_unref (msg);
    return;
  }

  GST_BIN_CLASS (gst_restart_src_parent_class)->handle_message (bin, msg);
}

static void
gst_restart_src_handle_error_async (GstElement * element, gpointer user_data) {
  GstRestartSrc * restart_src = GST_RESTART_SRC (element);

  gst_element_set_state (restart_src->src, GST_STATE_NULL);

  GstClock * clock = gst_system_clock_obtain ();
  GstClockTime timeout = gst_clock_get_time (clock) + restart_src->restart_timeout;
  restart_src->timeout_shot_id = gst_clock_new_single_shot_id (clock, timeout);
  g_assert (gst_clock_id_wait_async (restart_src->timeout_shot_id,
      gst_restart_src_restart_timeout, restart_src, NULL) == GST_CLOCK_OK);
  gst_clock_id_unref (restart_src->timeout_shot_id);
  gst_object_unref (clock);
}

static void
gst_restart_src_handle_error (GstRestartSrc * restart_src) {
  GST_DEBUG_OBJECT (restart_src, "Failed to sync source state, waiting for restart.");

  if (g_atomic_int_get (&restart_src->pending_restart) == TRUE) {
    GST_DEBUG_OBJECT (restart_src, "Already pending restart, cancelling.");
    return;
  }

  g_atomic_int_set (&restart_src->pending_restart, TRUE);

  gst_element_call_async (GST_ELEMENT(restart_src), gst_restart_src_handle_error_async,
      NULL, NULL);
}

static GstPadProbeReturn
gst_restart_src_probe_drop_eos (GstPad * pad, GstPadProbeInfo * info, gpointer user_data) {
  if (GST_EVENT_TYPE (info->data) == GST_EVENT_EOS) {
    return GST_PAD_PROBE_DROP;
  } else {
    return GST_PAD_PROBE_OK;
  }
}

static gboolean
gst_restart_src_restart_timeout (GstClock * clock, GstClockTime time, GstClockID id,
    gpointer user_data) {
  GstRestartSrc * restart_src = GST_RESTART_SRC(user_data);

  g_assert (time != GST_CLOCK_TIME_NONE);

  GST_DEBUG_OBJECT (restart_src, "Trying to sync source state.");
  g_atomic_int_set (&restart_src->pending_restart, FALSE);

  if (!gst_element_sync_state_with_parent (restart_src->src)) {
    gst_restart_src_handle_error (restart_src);
    return TRUE;
  }

  return TRUE;
}

static void
gst_restart_src_change_src_state (GstRestartSrc * restart_src, GstStateChange transition) {
  if (g_atomic_int_get (&restart_src->pending_restart) == TRUE) {
    if (GST_STATE_TRANSITION_NEXT (transition) <= GST_STATE_READY) {
      g_atomic_int_set (& restart_src->pending_restart, FALSE);
      gst_clock_id_unschedule (restart_src->timeout_shot_id);
    }
    return;
  }

  GstStateChangeReturn res = gst_element_set_state (
      restart_src->src, GST_STATE_TRANSITION_NEXT (transition));

  if (res == GST_STATE_CHANGE_FAILURE) {
    if (transition == GST_STATE_CHANGE_READY_TO_NULL)
      return;

    gst_restart_src_handle_error (restart_src);
  }
}

static GstStateChangeReturn
gst_restart_src_change_state (GstElement * element, GstStateChange transition) {
  GstRestartSrc * restart_src = GST_RESTART_SRC(element);

  if (GST_STATE_TRANSITION_NEXT (transition) >= GST_STATE_READY &&
      restart_src->src == NULL) {

    GST_ELEMENT_ERROR (restart_src, RESOURCE, FAILED,
        ("Source pointer should be set."), (NULL));
    return GST_STATE_CHANGE_FAILURE;
  }

  if (transition == GST_STATE_CHANGE_NULL_TO_READY) {
    gst_element_set_locked_state (restart_src->src, TRUE);
    gst_bin_add (GST_BIN(element), restart_src->src);

    GstPad * srcpad = gst_element_get_static_pad (restart_src->src, "src");
    gst_ghost_pad_set_target (restart_src->ghost_srcpad, srcpad);
    gst_object_unref (srcpad);
  }

  GST_ELEMENT_CLASS (gst_restart_src_parent_class)->change_state (element, transition);

  if (restart_src->src != NULL) {
    gst_restart_src_change_src_state (restart_src, transition);
  }

  if (transition == GST_STATE_CHANGE_READY_TO_PAUSED ||
      transition == GST_STATE_CHANGE_PLAYING_TO_PAUSED) {
    return GST_STATE_CHANGE_NO_PREROLL;
  }

  return GST_STATE_CHANGE_SUCCESS;
}

static gboolean
gstrestartsrc_plugin_init (GstPlugin * plugin) {
    GST_DEBUG_CATEGORY_INIT (gst_restart_src_debug, "restartsrc", 0, DESCRIPTION);

    return gst_element_register(plugin, "restartsrc", GST_RANK_PRIMARY, GST_TYPE_RESTART_SRC);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, restartsrc,
                   DESCRIPTION, gstrestartsrc_plugin_init, VERSION, LICENSE, BINARY_PACKAGE, URL)

