#ifndef BIT_PROJECT_GSTRESTARTSRC_H
#define BIT_PROJECT_GSTRESTARTSRC_H

#include <gst/gst.h>

#define PACKAGE "gstrestartsrc"
#define VERSION "1.0"
#define LICENSE "LGPL"
#define DESCRIPTION "restartsrc plugin"
#define BINARY_PACKAGE "restartsrc plugin"
#define URL "http://nvidia.com/"

G_BEGIN_DECLS

#define GST_TYPE_RESTART_SRC (gst_restart_src_get_type ())
G_DECLARE_FINAL_TYPE (GstRestartSrc, gst_restart_src, GST, RESTART_SRC, GstBin)

G_END_DECLS

#endif //BIT_PROJECT_GSTRESTARTSRC_H
