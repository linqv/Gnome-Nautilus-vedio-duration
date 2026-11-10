#pragma once

#include <glib.h>

#define DURATION_TEXT_MAX 32

typedef enum
{
  MEDIA_KIND_NONE = 0,
  MEDIA_KIND_VIDEO,
  MEDIA_KIND_AUDIO,
} MediaKind;

MediaKind vd_media_kind_from_name(const gchar *name);
const gchar *vd_media_format_seconds(guint32 seconds, gboolean is_audio,
                                     gchar buf[DURATION_TEXT_MAX]);
