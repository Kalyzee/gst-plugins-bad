
/**
* CONFIDENTIAL and PROPRIETARY software of KALYZEE SAS.
* Copyright (c) 2019 Kalyzee
* All rights reserved
* This copyright notice MUST be reproduced on all authorized copies.
*
* Authors : - Julien Bardagi   <julien.bardagi@kalyzee.com>
*/

#ifdef HAVE_ORC
#include <orc/orc.h>
#else
#define orc_memcpy memcpy
#endif

#include "gstamcsurfacesrc.h"
#include "gstamc-constants.h"

#define BIT_RATE_DEFAULT (2 * 1024 * 1024)
#define I_FRAME_INTERVAL_DEFAULT 0

#define FPS_N_DEFAULT 25
#define FPS_D_DEFAULT 1

enum
{
  PROP_0 = 0,
  PROP_BIT_RATE,
  PROP_I_FRAME_INTERVAL
};

enum
{
  SIGNAL_SURFACE_READY = 0,
  LAST_SIGNAL
};

static guint surface_src_signals[LAST_SIGNAL] = { 0 };

GST_DEBUG_CATEGORY_STATIC (gst_amc_surface_src_debug_category);
#define GST_CAT_DEFAULT gst_amc_surface_src_debug_category

static void gst_amc_surface_src_class_init (GstAmcSurfaceSrcClass * klass);
static void gst_amc_surface_src_init (GstAmcSurfaceSrc * self);
static void gst_amc_surface_src_base_init (gpointer g_class);

static GstBaseSrcClass *parent_class = NULL;

static void
gst_amc_surface_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);

static void
gst_amc_surface_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

GType
gst_amc_surface_src_get_type (void)
{
  static volatile gsize type = 0;

  if (g_once_init_enter (&type)) {
    GType _type;
    static const GTypeInfo info = {
      sizeof (GstAmcSurfaceSrcClass),
      gst_amc_surface_src_base_init,
      NULL,
      (GClassInitFunc) gst_amc_surface_src_class_init,
      NULL,
      NULL,
      sizeof (GstAmcSurfaceSrc),
      0,
      (GInstanceInitFunc) gst_amc_surface_src_init,
      NULL
    };

    _type = g_type_register_static (GST_TYPE_BASE_SRC, "GstAmcSurfaceSrc",
        &info, 0);

    GST_DEBUG_CATEGORY_INIT (gst_amc_surface_src_debug_category, "amcsursrc", 0,
        "Android MediaCodec video encoder (surface src)");

    g_once_init_leave (&type, _type);
  }

  return type;
}

static void
gst_amc_surface_src_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);
  GstAmcSurfaceSrcClass *surfacesrc_class = GST_AMC_SURFACE_SRC_CLASS (g_class);
  const GstAmcCodecInfo *codec_info;
  GstPadTemplate *templ;
  GstCaps *sink_caps, *src_caps;
  gchar *longname;

  codec_info =
      g_type_get_qdata (G_TYPE_FROM_CLASS (g_class), gst_amc_codec_info_quark);
  /* This happens for the base class and abstract subclasses */
  if (!codec_info)
    return;

  surfacesrc_class->codec_info = codec_info;

  gst_amc_codec_info_to_caps (codec_info, &sink_caps, &src_caps);
  /* Add pad template */

  templ = gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS, src_caps);
  gst_element_class_add_pad_template (element_class, templ);
  gst_caps_unref (src_caps);

  longname = g_strdup_printf ("Android MediaCodec %s", codec_info->name);
  gst_element_class_set_metadata (element_class,
      codec_info->name,
      "Codec/Encoder/Video",
      longname, "Julien Bardagi <julien.bardagi@kalyzee.com>");
  g_free (longname);
}

static void
gst_amc_surface_src_stream_lock (GstAmcSurfaceSrc * self)
{
  g_rec_mutex_lock (&self->stream_lock);
}

static void
gst_amc_surface_src_stream_unlock (GstAmcSurfaceSrc * self)
{
  g_rec_mutex_unlock (&self->stream_lock);
}

#define GST_AMC_SURFACE_SRC_STREAM_LOCK(encoder) gst_amc_surface_src_stream_lock (encoder)
#define GST_AMC_SURFACE_SRC_STREAM_UNLOCK(encoder) gst_amc_surface_src_stream_unlock (encoder)

typedef struct _BufferIdentification BufferIdentification;
struct _BufferIdentification
{
  guint64 timestamp;
};

static GstStateChangeReturn
gst_amc_surface_src_change_state (GstElement * element,
    GstStateChange transition);

static GstFlowReturn
gst_amc_surface_src_create (GstBaseSrc * src, guint64 offset, guint size,
    GstBuffer ** buf);

static GstCaps *caps_from_amc_format (GstAmcFormat * amc_format, gint fps_n,
    gint fps_d);

static gboolean gst_amc_surface_src_open (GstAmcSurfaceSrc * encoder);
static gboolean gst_amc_surface_src_close (GstAmcSurfaceSrc * encoder);
static gboolean gst_amc_surface_src_start (GstAmcSurfaceSrc * encoder);
static gboolean gst_amc_surface_src_stop (GstAmcSurfaceSrc * encoder);
static gboolean gst_amc_surface_src_flush (GstAmcSurfaceSrc * encoder);

#define MAX_FRAME_DIST_TIME  (5 * GST_SECOND)
#define MAX_FRAME_DIST_FRAMES (100)

static GstAmcFormat *
create_amc_format (GstAmcSurfaceSrc * self, GstStructure * s)
{
  GstAmcSurfaceSrcClass *klass;
  const gchar *mime = NULL;
  struct
  {
    const gchar *key;
    gint id;
  } amc_profile = {
  NULL, -1};
  struct
  {
    const gchar *key;
    gint id;
  } amc_level = {
  NULL, -1};
  gint color_format;
  gint stride, slice_height;
  GstAmcFormat *format = NULL;
  GError *err = NULL;

  gint width = -1;
  gint height = -1;
  gint fps_n = -1;
  gint fps_d = -1;
  GstVideoFormat videoformat;

  const gchar *name = gst_structure_get_name (s);
  const gchar *profile_string = gst_structure_get_string (s, "profile");
  const gchar *level_string = gst_structure_get_string (s, "level");

  klass = GST_AMC_SURFACE_SRC_GET_CLASS (self);

  if (!gst_structure_get_int (s, "width", &width))
    return FALSE;
  if (!gst_structure_get_int (s, "height", &height))
    return FALSE;
  if (!gst_structure_get_fraction (s, "framerate", &fps_n, &fps_d))
    return FALSE;

  videoformat = gst_video_format_from_string ("I420");
  if (videoformat == GST_VIDEO_FORMAT_UNKNOWN) {
    return FALSE;
  }

  if (strcmp (name, "video/mpeg") == 0) {
    gint mpegversion;

    if (!gst_structure_get_int (s, "mpegversion", &mpegversion))
      return NULL;

    if (mpegversion == 4) {
      mime = "video/mp4v-es";

      if (profile_string) {
        amc_profile.key = "profile";    /* named profile ? */
        amc_profile.id = gst_amc_mpeg4_profile_from_string (profile_string);
      }

      if (level_string) {
        amc_level.key = "level";        /* named level ? */
        amc_level.id = gst_amc_mpeg4_level_from_string (level_string);
      }
    } else if ( /* mpegversion == 1 || */ mpegversion == 2)
      mime = "video/mpeg2";
  } else if (strcmp (name, "video/x-h263") == 0) {
    mime = "video/3gpp";
  } else if (strcmp (name, "video/x-h264") == 0) {
    mime = "video/avc";

    if (profile_string) {
      amc_profile.key = "profile";      /* named profile ? */
      amc_profile.id = gst_amc_avc_profile_from_string (profile_string);
    }

    if (level_string) {
      amc_level.key = "level";  /* named level ? */
      amc_level.id = gst_amc_avc_level_from_string (level_string);
    }
  } else if (strcmp (name, "video/x-vp8") == 0) {
    mime = "video/x-vnd.on2.vp8";
  } else if (strcmp (name, "video/x-vp9") == 0) {
    mime = "video/x-vnd.on2.vp9";
  } else {
    GST_ERROR_OBJECT (self, "Failed to convert caps(%s/...) to any mime", name);
    return NULL;
  }

  format = gst_amc_format_new_video (mime, width, height, &err);
  if (!format) {
    GST_ERROR_OBJECT (self, "Failed to create a \"%s,%dx%d\" MediaFormat",
        mime, width, height);
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    return NULL;
  }

  videoformat = GST_VIDEO_FORMAT_I420;
  color_format = COLOR_FormatAndroidOpaque;     //Required by createInputSurface

  if (color_format == -1)
    goto video_format_failed_to_convert;

  gst_amc_format_set_int (format, "bitrate", self->bitrate, &err);
  if (err)
    GST_ELEMENT_WARNING_FROM_ERROR (self, err);
  gst_amc_format_set_int (format, "color-format", color_format, &err);
  if (err)
    GST_ELEMENT_WARNING_FROM_ERROR (self, err);
  stride = GST_ROUND_UP_4 (width);      /* safe (?) */
  gst_amc_format_set_int (format, "stride", stride, &err);
  if (err)
    GST_ELEMENT_WARNING_FROM_ERROR (self, err);
  slice_height = height;
  gst_amc_format_set_int (format, "slice-height", slice_height, &err);
  if (err)
    GST_ELEMENT_WARNING_FROM_ERROR (self, err);

  if (profile_string) {
    if (amc_profile.id == -1)
      goto unsupported_profile;

    /* FIXME: Set to any value in AVCProfile* leads to
     * codec configuration fail */
    /* gst_amc_format_set_int (format, amc_profile.key, 0x40); */
  }

  if (level_string) {
    if (amc_level.id == -1)
      goto unsupported_level;

    /* gst_amc_format_set_int (format, amc_level.key, amc_level.id); */
  }

  gst_amc_format_set_int (format, "i-frame-interval", self->i_frame_int, &err);
  if (err)
    GST_ELEMENT_WARNING_FROM_ERROR (self, err);

  if (fps_d)
    gst_amc_format_set_float (format, "frame-rate",
        ((gfloat) fps_n) / fps_d, &err);
  if (err)
    GST_ELEMENT_WARNING_FROM_ERROR (self, err);

  return format;

video_format_failed_to_convert:
  GST_ERROR_OBJECT (self, "Failed to convert video format");
  gst_amc_format_free (format);
  return NULL;

unsupported_profile:
  GST_ERROR_OBJECT (self, "Unsupport profile '%s'", profile_string);
  gst_amc_format_free (format);
  return NULL;

unsupported_level:
  GST_ERROR_OBJECT (self, "Unsupport level '%s'", level_string);
  gst_amc_format_free (format);
  return NULL;
}

static gboolean
gst_amc_surface_src_set_caps (GstBaseSrc * src, GstCaps * caps)
{
  GstAmcSurfaceSrc *self = GST_AMC_SURFACE_SRC (src);
  GstStructure *s = NULL;
  gboolean ret = FALSE;
  GError *err = NULL;
  GstAmcFormat *format = NULL;
  gchar *format_string = NULL;
  GstCaps *allowed_caps = NULL;
  gboolean free_format = TRUE;

  gint width = -1;
  gint height = -1;

  gboolean is_format_change = FALSE;
  gboolean needs_disable = FALSE;

  gst_caps_ref (caps);

  allowed_caps = gst_caps_truncate (caps);

  s = gst_caps_get_structure (allowed_caps, 0);
  if (!s)
    goto quit;

  if (!gst_structure_get_int (s, "width", &width))
    goto quit;
  if (!gst_structure_get_int (s, "height", &height))
    goto quit;

  /* Check if the caps change is a real format change or if only irrelevant
   * parts of the caps have changed or nothing at all.
   */
  is_format_change |= width != self->width;
  is_format_change |= height != self->height;
  needs_disable = self->started;

  /* If the component is not started and a real format change happens
   * we have to restart the component. If no real format change
   * happened we can just exit here.
   */
  if (needs_disable && !is_format_change) {
    /* Framerate or something minor changed */
    GST_DEBUG_OBJECT (self,
        "Already running and caps did not change the format");
    ret = TRUE;
    goto quit;
  }

  format = create_amc_format (self, s);

  if (!format) {
    goto quit;
  }

  format_string = gst_amc_format_to_string (format, &err);
  if (err)
    GST_ELEMENT_WARNING_FROM_ERROR (self, err);
  GST_DEBUG_OBJECT (self, "Configuring codec with format: %s",
      GST_STR_NULL (format_string));
  g_free (format_string);

  if (!gst_amc_codec_configure (self->codec, format, NULL, 1, &err)) {
    GST_ERROR_OBJECT (self, "Failed to configure codec");
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    goto quit;
  }

  if (!(self->surface_jobject =
          gst_amc_codec_create_input_surface (self->codec, &err))) {
    GST_ERROR_OBJECT (self, "Failed to create input surface");
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    goto quit;
  }

  g_signal_emit (self, surface_src_signals[SIGNAL_SURFACE_READY], 0,
      self->surface_jobject);

  if (!gst_amc_codec_start (self->codec, &err)) {
    GST_ERROR_OBJECT (self, "Failed to start codec");
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    goto quit;
  }

  ret = TRUE;
  self->width = width;
  self->height = height;

  free_format = FALSE;

  if (self->amc_format)
    gst_amc_format_free (self->amc_format);

  self->amc_format = format;

quit:
  if (allowed_caps)
    gst_caps_unref (allowed_caps);

  if (format && free_format)
    gst_amc_format_free (format);

  return ret;
}

static gboolean
gst_amc_surface_src_set_src_caps (GstAmcSurfaceSrc * self,
    GstAmcFormat * format, gint fps_n, gint fps_d)
{
  GstCaps *caps;

  caps = caps_from_amc_format (format, fps_n, fps_d);
  if (!caps) {
    GST_ERROR_OBJECT (self, "Failed to create output caps");
    return FALSE;
  }

  return gst_base_src_set_caps (GST_BASE_SRC (self), caps);
}

static GstFlowReturn
gst_amc_surface_src_handle_output_frame (GstAmcSurfaceSrc * self,
    GstAmcBuffer * buf, const GstAmcBufferInfo * buffer_info,
    GstBuffer ** out_buf)
{
  GstFlowReturn flow_ret = GST_FLOW_OK;
  GstPad *srcpad;

  srcpad = GST_BASE_SRC_PAD (self);

  /* The BUFFER_FLAG_CODEC_CONFIG logic is borrowed from
   * gst-omx. see *_handle_output_frame in
   * gstomxvideoenc.c and gstomxh264enc.c */
  if ((buffer_info->flags & BUFFER_FLAG_CODEC_CONFIG)
      && buffer_info->size > 0) {
    GstStructure *s;

    GstCaps *src_caps = gst_pad_get_current_caps (srcpad);
    s = gst_caps_get_structure (src_caps, 0);
    if (!strcmp (gst_structure_get_name (s), "video/x-h264")) {

      if (buffer_info->size > 4 &&
          GST_READ_UINT32_BE (buf->data + buffer_info->offset) == 0x00000001) {

        GST_DEBUG_OBJECT (self, "got codecconfig in byte-stream format");

        *out_buf = gst_buffer_new_and_alloc (buffer_info->size);
        gst_buffer_fill (*out_buf, 0, buf->data + buffer_info->offset,
            buffer_info->size);
        GST_BUFFER_PTS (*out_buf) =
            gst_util_uint64_scale (buffer_info->presentation_time_us,
            GST_USECOND, 1);

        if (self->headers)
          gst_buffer_unref (self->headers);

        self->headers = *out_buf;

        gst_buffer_ref (self->headers);
      }
    } else {
      gst_caps_unref (src_caps);
      return GST_FLOW_ERROR;
    }

    gst_caps_unref (src_caps);
  }

  if (buffer_info->size > 0) {
    parent_class->alloc (GST_BASE_SRC (self), buffer_info->offset,
        buffer_info->size, out_buf);
    gst_buffer_fill (*out_buf, 0, buf->data + buffer_info->offset,
        buffer_info->size);

    GST_BUFFER_PTS (*out_buf) =
        gst_util_uint64_scale (buffer_info->presentation_time_us, GST_USECOND,
        1);
  }

  return flow_ret;
}

static void
gst_amc_surface_src_finalize (GObject * object)
{
  GstAmcSurfaceSrc *self = GST_AMC_SURFACE_SRC (object);
  g_rec_mutex_clear (&self->stream_lock);
  g_mutex_clear (&self->drain_lock);
  g_cond_clear (&self->drain_cond);

  G_OBJECT_CLASS (parent_class)->finalize (G_OBJECT (self));
}

static void
gst_amc_surface_src_init (GstAmcSurfaceSrc * self)
{
  g_rec_mutex_init (&self->stream_lock);
  g_mutex_init (&self->drain_lock);
  g_cond_init (&self->drain_cond);

  self->bitrate = BIT_RATE_DEFAULT;
  self->i_frame_int = I_FRAME_INTERVAL_DEFAULT;
  self->surface_jobject = NULL;
  self->headers = NULL;
  self->amc_format = NULL;
}

static void
gst_amc_surface_src_class_init (GstAmcSurfaceSrcClass * klass)
{
  GstBaseSrcClass *base_src_class = GST_BASE_SRC_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  parent_class = g_type_class_peek_parent (klass);
  base_src_class->set_caps = gst_amc_surface_src_set_caps;
  base_src_class->create = gst_amc_surface_src_create;
  object_class->set_property = gst_amc_surface_src_set_property;
  object_class->get_property = gst_amc_surface_src_get_property;
  object_class->finalize = gst_amc_surface_src_finalize;

  element_class->change_state = gst_amc_surface_src_change_state;

  surface_src_signals[SIGNAL_SURFACE_READY] =
      g_signal_newv ("surface-ready",
      G_TYPE_FROM_CLASS (object_class),
      G_SIGNAL_RUN_LAST | G_SIGNAL_NO_RECURSE | G_SIGNAL_NO_HOOKS,
      NULL /* closure */ ,
      NULL /* accumulator */ ,
      NULL /* accumulator data */ ,
      NULL /* C marshaller */ ,
      G_TYPE_NONE /* return_type */ ,
      1 /* n_params */ ,
      (GType[1]) {
      G_TYPE_POINTER} /* param_types */ );
}

gboolean
gst_amc_surface_src_plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "amcsurfacesrc",
      GST_RANK_NONE, GST_TYPE_AMC_SURFACE_SRC);
}

static GstStateChangeReturn
gst_amc_surface_src_change_state (GstElement * element,
    GstStateChange transition)
{
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
  GstAmcSurfaceSrc *self = GST_AMC_SURFACE_SRC (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:{
      if (!gst_amc_surface_src_open (self)) {
        GST_ERROR_OBJECT (self, "Failed to open encoder");
        return GST_STATE_CHANGE_FAILURE;
      }
      break;
    }
    case GST_STATE_CHANGE_READY_TO_PAUSED:{
      self->started = gst_amc_surface_src_start (self);
      GST_AMC_SURFACE_SRC_STREAM_LOCK (self);
      gst_amc_surface_src_flush (self);
      GST_AMC_SURFACE_SRC_STREAM_UNLOCK (self);
    }
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:{
      self->started = !gst_amc_surface_src_stop (self);
    }
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:{
      if (!gst_amc_surface_src_close (self)) {
        GST_ERROR_OBJECT (self, "Failed to close encoder");
        return GST_STATE_CHANGE_FAILURE;
      }
      break;
    }
    default:
      break;
  }

  return ret;
}

static GstFlowReturn
gst_amc_surface_src_create (GstBaseSrc * src, guint64 offset, guint size,
    GstBuffer ** gstbuf)
{

  GstAmcSurfaceSrc *self = GST_AMC_SURFACE_SRC (src);
  GstFlowReturn flow_ret = GST_FLOW_OK;
  gboolean is_eos;
  GstAmcBufferInfo buffer_info;
  GstAmcBuffer *buf;
  gint idx;
  GError *err = NULL;
  gint fps_n = FPS_N_DEFAULT;
  gint fps_d = FPS_D_DEFAULT;
  GstStructure *src_caps_struct = NULL;
  GstCaps *src_caps = NULL;

  GST_AMC_SURFACE_SRC_STREAM_LOCK (self);

retry:
  GST_DEBUG_OBJECT (self, "Waiting for available output buffer");
  GST_AMC_SURFACE_SRC_STREAM_UNLOCK (self);
  /* Wait at most 100ms here, some codecs don't fail dequeueing if
   * the codec is flushing, causing deadlocks during shutdown */
  idx =
      gst_amc_codec_dequeue_output_buffer (self->codec, &buffer_info, 100000,
      &err);
  GST_AMC_SURFACE_SRC_STREAM_LOCK (self);
  /*} */

  if (idx < 0 || self->amc_format) {
    if (self->flushing) {
      g_clear_error (&err);
      goto flushing;
    }

    /* The comments from https://android.googlesource.com/platform/cts/+/android-4.3_r3.1/tests/tests/media/src/android/media/cts/EncodeDecodeTest.java
     * line 539 says INFO_OUTPUT_FORMAT_CHANGED is not expected for an encoder
     */
    if (self->amc_format || idx == INFO_OUTPUT_FORMAT_CHANGED) {
      GstAmcFormat *format;
      gchar *format_string;

      GST_DEBUG_OBJECT (self, "Output format has changed");

      format = (idx == INFO_OUTPUT_FORMAT_CHANGED) ?
          gst_amc_codec_get_output_format (self->codec,
          &err) : self->amc_format;
      if (err) {
        format = self->amc_format;
        GST_ELEMENT_WARNING_FROM_ERROR (self, err);
      }

      if (self->amc_format) {
        if (format != self->amc_format) {
          gst_amc_format_free (self->amc_format);
        }
        self->amc_format = NULL;
      }

      if (!format) {
        goto format_error;
      }

      format_string = gst_amc_format_to_string (format, &err);
      if (err) {
        gst_amc_format_free (format);
        goto format_error;
      }

      GST_DEBUG_OBJECT (self, "Got new output format: %s", format_string);
      g_free (format_string);

      src_caps = gst_pad_get_current_caps (GST_BASE_SRC_PAD (self));

      if ((src_caps_struct = gst_caps_get_structure (src_caps, 0)))
        gst_structure_get_fraction (src_caps_struct, "framerate", &fps_n,
            &fps_d);

      gst_caps_unref (src_caps);

      if (!gst_amc_surface_src_set_src_caps (self, format, fps_n, fps_d)) {
        gst_amc_format_free (format);
        goto format_error;
      }

      gst_amc_format_free (format);

      if (idx >= 0)
        goto process_buffer;

      goto retry;
    }

    switch (idx) {
      case INFO_OUTPUT_BUFFERS_CHANGED:
        /* Handled internally */
        g_assert_not_reached ();
        break;
      case INFO_TRY_AGAIN_LATER:
        GST_DEBUG_OBJECT (self, "Dequeueing output buffer timed out");
        goto retry;
        break;
      case G_MININT:
        GST_ERROR_OBJECT (self, "Failure dequeueing input buffer");
        goto dequeue_error;
        break;
      default:
        g_assert_not_reached ();
        break;
    }

    goto retry;
  }

process_buffer:
  GST_DEBUG_OBJECT (self,
      "Got output buffer at index %d: size %d time %" G_GINT64_FORMAT
      " flags 0x%08x", idx, buffer_info.size, buffer_info.presentation_time_us,
      buffer_info.flags);

  buf = gst_amc_codec_get_output_buffer (self->codec, idx, &err);
  if (err) {
    if (self->flushing) {
      g_clear_error (&err);
      goto flushing;
    }
    goto failed_to_get_output_buffer;
  } else if (!buf) {
    goto got_null_output_buffer;
  }

  is_eos = ! !(buffer_info.flags & BUFFER_FLAG_END_OF_STREAM);

  flow_ret =
      gst_amc_surface_src_handle_output_frame (self, buf, &buffer_info, gstbuf);

  gst_amc_buffer_free (buf);
  buf = NULL;

  if (!gst_amc_codec_release_output_buffer (self->codec, idx, FALSE, &err)) {
    if (self->flushing) {
      g_clear_error (&err);
      goto flushing;
    }
    goto failed_release;
  }

  if (is_eos || flow_ret == GST_FLOW_EOS) {
    GST_AMC_SURFACE_SRC_STREAM_UNLOCK (self);
    g_mutex_lock (&self->drain_lock);
    if (self->draining) {
      GST_DEBUG_OBJECT (self, "Drained");
      self->draining = FALSE;
      g_cond_broadcast (&self->drain_cond);
    } else if (flow_ret == GST_FLOW_OK) {
      GST_DEBUG_OBJECT (self, "Component signalled EOS");
      flow_ret = GST_FLOW_EOS;
    }
    g_mutex_unlock (&self->drain_lock);
    GST_AMC_SURFACE_SRC_STREAM_LOCK (self);
  } else {
    GST_DEBUG_OBJECT (self, "Finished frame: %s", gst_flow_get_name (flow_ret));
  }

  self->downstream_flow_ret = flow_ret;

  if (flow_ret != GST_FLOW_OK)
    goto flow_error;

  GST_AMC_SURFACE_SRC_STREAM_UNLOCK (self);

  return flow_ret;

dequeue_error:
  {
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    gst_pad_push_event (GST_BASE_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_BASE_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_ERROR;
    GST_AMC_SURFACE_SRC_STREAM_UNLOCK (self);
    g_mutex_lock (&self->drain_lock);
    self->draining = FALSE;
    g_cond_broadcast (&self->drain_cond);
    g_mutex_unlock (&self->drain_lock);
    return GST_FLOW_ERROR;
  }

format_error:
  {
    if (err)
      GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    else
      GST_ELEMENT_ERROR (self, LIBRARY, FAILED, (NULL),
          ("Failed to handle format"));
    gst_pad_push_event (GST_BASE_SRC_PAD (self), gst_event_new_eos ());
    self->downstream_flow_ret = GST_FLOW_ERROR;
    GST_AMC_SURFACE_SRC_STREAM_UNLOCK (self);
    g_mutex_lock (&self->drain_lock);
    self->draining = FALSE;
    g_cond_broadcast (&self->drain_cond);
    g_mutex_unlock (&self->drain_lock);
    return GST_FLOW_NOT_NEGOTIATED;
  }
failed_release:
  {
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    gst_pad_push_event (GST_BASE_SRC_PAD (self), gst_event_new_eos ());

    self->downstream_flow_ret = GST_FLOW_ERROR;
    GST_AMC_SURFACE_SRC_STREAM_UNLOCK (self);
    g_mutex_lock (&self->drain_lock);
    self->draining = FALSE;
    g_cond_broadcast (&self->drain_cond);
    g_mutex_unlock (&self->drain_lock);
    return GST_FLOW_ERROR;
  }
flushing:
  {
    GST_DEBUG_OBJECT (self, "Flushing");
    self->downstream_flow_ret = GST_FLOW_FLUSHING;
    GST_AMC_SURFACE_SRC_STREAM_UNLOCK (self);
    return GST_FLOW_FLUSHING;
  }

flow_error:
  {
    if (flow_ret == GST_FLOW_EOS) {
      GST_DEBUG_OBJECT (self, "EOS");
      gst_pad_push_event (GST_BASE_SRC_PAD (self), gst_event_new_eos ());
    } else if (flow_ret == GST_FLOW_NOT_LINKED || flow_ret < GST_FLOW_EOS) {
      GST_ELEMENT_FLOW_ERROR (self, flow_ret);
      gst_pad_push_event (GST_BASE_SRC_PAD (self), gst_event_new_eos ());
    }
    GST_AMC_SURFACE_SRC_STREAM_UNLOCK (self);
    g_mutex_lock (&self->drain_lock);
    self->draining = FALSE;
    g_cond_broadcast (&self->drain_cond);
    g_mutex_unlock (&self->drain_lock);
    return flow_ret;
  }

failed_to_get_output_buffer:
  {
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    gst_pad_push_event (GST_BASE_SRC_PAD (self), gst_event_new_eos ());
    self->downstream_flow_ret = GST_FLOW_ERROR;
    GST_AMC_SURFACE_SRC_STREAM_UNLOCK (self);
    g_mutex_lock (&self->drain_lock);
    self->draining = FALSE;
    g_cond_broadcast (&self->drain_cond);
    g_mutex_unlock (&self->drain_lock);
    return GST_FLOW_ERROR;
  }

got_null_output_buffer:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Got no output buffer"));
    gst_pad_push_event (GST_BASE_SRC_PAD (self), gst_event_new_eos ());
    self->downstream_flow_ret = GST_FLOW_ERROR;
    GST_AMC_SURFACE_SRC_STREAM_UNLOCK (self);
    g_mutex_lock (&self->drain_lock);
    self->draining = FALSE;
    g_cond_broadcast (&self->drain_cond);
    g_mutex_unlock (&self->drain_lock);
    return GST_FLOW_EOS;
  }

  return GST_FLOW_ERROR;
}

static gboolean
gst_amc_surface_src_open (GstAmcSurfaceSrc * self)
{
  GstAmcSurfaceSrcClass *klass =
      GST_AMC_SURFACE_SRC_CLASS (GST_BASE_SRC_GET_CLASS (self));
  GError *err = NULL;

  GST_DEBUG_OBJECT (self, "Opening encoder");

  self->codec = gst_amc_codec_new (klass->codec_info->name, &err);
  if (!self->codec) {
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    return FALSE;
  }
  self->started = FALSE;
  self->flushing = TRUE;

  GST_DEBUG_OBJECT (self, "Opened encoder");

  return TRUE;
}

static gboolean
gst_amc_surface_src_close (GstAmcSurfaceSrc * self)
{
  GST_DEBUG_OBJECT (self, "Closing encoder");

  if (self->codec) {
    GError *err = NULL;

    gst_amc_codec_release (self->codec, &err);
    if (err)
      GST_ELEMENT_WARNING_FROM_ERROR (self, err);

    gst_amc_codec_free (self->codec);
  }
  self->codec = NULL;

  self->started = FALSE;
  self->flushing = TRUE;

  GST_DEBUG_OBJECT (self, "Closed encoder");

  return TRUE;
}

static gboolean
gst_amc_surface_src_start (GstAmcSurfaceSrc * self)
{
  self->last_upstream_ts = 0;
  self->drained = TRUE;
  self->downstream_flow_ret = GST_FLOW_OK;
  self->started = FALSE;
  self->flushing = TRUE;

  return TRUE;
}

static gboolean
gst_amc_surface_src_stop (GstAmcSurfaceSrc * self)
{
  GError *err = NULL;

  GST_DEBUG_OBJECT (self, "Stopping encoder");
  self->flushing = TRUE;
  if (self->started) {
    gst_amc_codec_flush (self->codec, &err);
    if (err)
      GST_ELEMENT_WARNING_FROM_ERROR (self, err);
    gst_amc_codec_stop (self->codec, &err);
    if (err)
      GST_ELEMENT_WARNING_FROM_ERROR (self, err);
    self->started = FALSE;
  }

  self->downstream_flow_ret = GST_FLOW_FLUSHING;
  self->drained = TRUE;
  g_mutex_lock (&self->drain_lock);
  self->draining = FALSE;
  g_cond_broadcast (&self->drain_cond);
  g_mutex_unlock (&self->drain_lock);
  if (self->input_state)
    gst_video_codec_state_unref (self->input_state);
  self->input_state = NULL;

  if (self->amc_format) {
    gst_amc_format_free (self->amc_format);
    self->amc_format = NULL;
  }

  GST_DEBUG_OBJECT (self, "Stopped encoder");
  return TRUE;
}

static gboolean
gst_amc_surface_src_flush (GstAmcSurfaceSrc * self)
{
  GError *err = NULL;

  GST_DEBUG_OBJECT (self, "Flushing encoder");

  if (!self->started) {
    GST_DEBUG_OBJECT (self, "Codec not started yet");
    return TRUE;
  }

  self->flushing = TRUE;
  gst_amc_codec_flush (self->codec, &err);
  if (err)
    GST_ELEMENT_WARNING_FROM_ERROR (self, err);

  /* Wait until the srcpad loop is finished,
   * unlock GST_AMC_SURFACE_SRC_STREAM_LOCK to prevent deadlocks
   * caused by using this lock from inside the loop function */
  GST_AMC_SURFACE_SRC_STREAM_UNLOCK (self);
  GST_PAD_STREAM_LOCK (GST_BASE_SRC_PAD (self));
  GST_PAD_STREAM_UNLOCK (GST_BASE_SRC_PAD (self));
  GST_AMC_SURFACE_SRC_STREAM_LOCK (self);
  self->flushing = FALSE;

  /* Start the srcpad loop again */
  self->last_upstream_ts = 0;
  self->drained = TRUE;
  self->downstream_flow_ret = GST_FLOW_OK;

  GST_DEBUG_OBJECT (self, "Flush encoder");

  return TRUE;
}

static GstCaps *
caps_from_amc_format (GstAmcFormat * amc_format, gint fps_n, gint fps_d)
{
  GstCaps *caps = NULL;
  gchar *mime = NULL;
  gint width, height;
  gint amc_profile, amc_level;
  gfloat frame_rate = 0.0;
  gint fraction_n, fraction_d;
  GError *err = NULL;

  if (!gst_amc_format_get_string (amc_format, "mime", &mime, &err)) {
    GST_ERROR ("Failed to get 'mime': %s", err->message);
    g_clear_error (&err);
    return NULL;
  }

  if (!gst_amc_format_get_int (amc_format, "width", &width, &err) ||
      !gst_amc_format_get_int (amc_format, "height", &height, &err)) {
    GST_ERROR ("Failed to get size: %s", err->message);
    g_clear_error (&err);

    g_free (mime);
    return NULL;
  }

  gst_amc_format_get_float (amc_format, "frame-rate", &frame_rate, NULL);
  gst_util_double_to_fraction (frame_rate, &fraction_n, &fraction_d);
  fraction_n = fraction_n < 0 ? fps_n : fraction_n;
  fraction_d = fraction_d < 0 ? fps_d : fraction_d;

  if (strcmp (mime, "video/mp4v-es") == 0) {
    const gchar *profile_string, *level_string;

    caps =
        gst_caps_new_simple ("video/mpeg", "mpegversion", G_TYPE_INT, 4,
        "systemstream", G_TYPE_BOOLEAN, FALSE, NULL);

    if (gst_amc_format_get_int (amc_format, "profile", &amc_profile, NULL)) {
      profile_string = gst_amc_mpeg4_profile_to_string (amc_profile);
      if (!profile_string)
        goto unsupported_profile;

      gst_caps_set_simple (caps, "profile", G_TYPE_STRING, profile_string,
          NULL);
    }

    if (gst_amc_format_get_int (amc_format, "level", &amc_level, NULL)) {
      level_string = gst_amc_mpeg4_level_to_string (amc_profile);
      if (!level_string)
        goto unsupported_level;

      gst_caps_set_simple (caps, "level", G_TYPE_STRING, level_string, NULL);
    }

  } else if (strcmp (mime, "video/mpeg2") == 0) {
    caps = gst_caps_new_simple ("video/mpeg", "mpegversion", 2, NULL);
  } else if (strcmp (mime, "video/3gpp") == 0) {
    caps = gst_caps_new_empty_simple ("video/x-h263");
  } else if (strcmp (mime, "video/avc") == 0) {
    const gchar *profile_string, *level_string;

    caps =
        gst_caps_new_simple ("video/x-h264",
        "stream-format", G_TYPE_STRING, "byte-stream", NULL);

    if (gst_amc_format_get_int (amc_format, "profile", &amc_profile, NULL)) {
      profile_string = gst_amc_avc_profile_to_string (amc_profile, NULL);
      if (!profile_string)
        goto unsupported_profile;

      gst_caps_set_simple (caps, "profile", G_TYPE_STRING, profile_string,
          NULL);
    }

    if (gst_amc_format_get_int (amc_format, "level", &amc_level, NULL)) {
      level_string = gst_amc_avc_level_to_string (amc_profile);
      if (!level_string)
        goto unsupported_level;

      gst_caps_set_simple (caps, "level", G_TYPE_STRING, level_string, NULL);
    }
  } else if (strcmp (mime, "video/x-vnd.on2.vp8") == 0) {
    caps = gst_caps_new_empty_simple ("video/x-vp8");
  } else if (strcmp (mime, "video/x-vnd.on2.vp9") == 0) {
    caps = gst_caps_new_empty_simple ("video/x-vp9");
  }

  gst_caps_set_simple (caps, "width", G_TYPE_INT, width,
      "height", G_TYPE_INT, height,
      "framerate", GST_TYPE_FRACTION, fraction_n, fraction_d, NULL);

  g_free (mime);
  return caps;

unsupported_profile:
  GST_ERROR ("Unsupport amc profile id %d", amc_profile);
  g_free (mime);
  gst_caps_unref (caps);

  return NULL;

unsupported_level:
  GST_ERROR ("Unsupport amc level id %d", amc_level);
  g_free (mime);
  gst_caps_unref (caps);

  return NULL;
}

static void
gst_amc_surface_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstAmcSurfaceSrc *self;
  GstState state;

  self = GST_AMC_SURFACE_SRC (object);

  GST_OBJECT_LOCK (self);

  state = GST_STATE (self);
  if (state != GST_STATE_READY && state != GST_STATE_NULL)
    goto wrong_state;

  switch (prop_id) {
    case PROP_BIT_RATE:
      self->bitrate = g_value_get_uint (value);
      break;
    case PROP_I_FRAME_INTERVAL:
      self->i_frame_int = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
  return;

  /* ERROR */
wrong_state:
  {
    GST_WARNING_OBJECT (self, "setting property in wrong state");
    GST_OBJECT_UNLOCK (self);
  }
}

static void
gst_amc_surface_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstAmcSurfaceSrc *self;

  self = GST_AMC_SURFACE_SRC (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_BIT_RATE:
      g_value_set_uint (value, self->bitrate);
      break;
    case PROP_I_FRAME_INTERVAL:
      g_value_set_uint (value, self->i_frame_int);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}
