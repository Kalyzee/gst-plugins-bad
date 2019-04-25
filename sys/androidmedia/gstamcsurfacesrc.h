/**
* CONFIDENTIAL and PROPRIETARY software of KALYZEE SAS.
* Copyright (c) 2019 Kalyzee
* All rights reserved
* This copyright notice MUST be reproduced on all authorized copies.
*
* Authors : - Julien Bardagi   <julien.bardagi@kalyzee.com>
*/

#ifndef GST_AMC_SURFACE_SRC_H
#define GST_AMC_SURFACE_SRC_H

#include <gst/gst.h>
#include <gst/base/gstbasesrc.h>
#include "gstamc.h"

G_BEGIN_DECLS

#define GST_TYPE_AMC_SURFACE_SRC \
  (gst_amc_surface_src_get_type())
#define GST_AMC_SURFACE_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_AMC_SURFACE_SRC,GstAmcSurfaceSrc))
#define GST_AMC_SURFACE_SRC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_AMC_SURFACE_SRC,GstAmcSurfaceSrcClass))
#define GST_AMC_SURFACE_SRC_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS((obj),GST_TYPE_AMC_SURFACE_SRC,GstAmcSurfaceSrcClass))
#define GST_IS_AMC_SURFACE_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_AMC_SURFACE_SRC))
#define GST_IS_AMC_SURFACE_SRC_CLASS(obj) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_AMC_SURFACE_SRC))
typedef struct _GstAmcSurfaceSrc GstAmcSurfaceSrc;
typedef struct _GstAmcSurfaceSrcClass GstAmcSurfaceSrcClass;

struct _GstAmcSurfaceSrc{
  GstBaseSrc parent_instance;

  GstAmcCodec *codec;
  GstAmcFormat *amc_format;
  jobject surface_jobject;

  GstVideoCodecState *input_state;

  /* Input format of the codec */
  GstVideoFormat format;
  GstAmcColorFormatInfo color_format_info;

  guint bitrate;
  guint i_frame_int;

  /* TRUE if the component is configured and saw
   * the first buffer */
  gboolean started;
  gboolean flushing;

  GstClockTime last_upstream_ts;

  /* Draining state */
  GMutex drain_lock;
  GCond drain_cond;
  /* TRUE if EOS buffers shouldn't be forwarded */
  gboolean draining;
  /* TRUE if the component is drained */
  gboolean drained;

  GstFlowReturn downstream_flow_ret;
  GRecMutex stream_lock;

  gint width;
  gint height;

  GstBuffer *headers;
};

struct _GstAmcSurfaceSrcClass {
  GstBaseSrcClass parent_class;

  const GstAmcCodecInfo *codec_info;
};

GType gst_amc_surface_src_get_type (void);

gboolean gst_amc_surface_src_plugin_init (GstPlugin *plugin);

G_END_DECLS

#endif
