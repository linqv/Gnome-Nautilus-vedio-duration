#include "vd-media.h"

#include <string.h>

static gboolean extension_matches(const gchar *ext, const gchar *const *exts)
{
  for (int i = 0; exts[i]; i++)
    if (g_ascii_strcasecmp(ext, exts[i]) == 0)
      return TRUE;
  return FALSE;
}

MediaKind vd_media_kind_from_name(const gchar *name)
{
  if (!name)
    return MEDIA_KIND_NONE;
  const gchar *ext = strrchr(name, '.');
  if (!ext)
    return MEDIA_KIND_NONE;
  ext++;

  static const gchar *const audio_exts[] = {"mp3", "flac", "wav",  "aac",
                                            "m4a", "mka",  "ogg",  "opus",
                                            "wma", "alac", "aiff", NULL};
  static const gchar *const video_exts[] = {
      "mp4",  "mkv", "avi",  "mov", "webm", "flv", "wmv", "m4v",
      "mpeg", "mpg", "m2ts", "ts",  "3gp",  "ogv", NULL};

  if (extension_matches(ext, audio_exts))
    return MEDIA_KIND_AUDIO;
  if (extension_matches(ext, video_exts))
    return MEDIA_KIND_VIDEO;
  return MEDIA_KIND_NONE;
}

const gchar *vd_media_format_seconds(guint32 sec, gboolean is_audio,
                                     gchar buf[DURATION_TEXT_MAX])
{
  if (sec == 0)
    return "-";
  if (is_audio && sec < 3600)
  {
    guint32 m = sec / 60, s = sec % 60;
    g_snprintf(buf, DURATION_TEXT_MAX, "%02u:%02u", m, s);
    return buf;
  }
  guint32 h = sec / 3600;
  guint32 m = (sec % 3600) / 60;
  guint32 s = sec % 60;
  g_snprintf(buf, DURATION_TEXT_MAX, "%u:%02u:%02u", h, m, s);
  return buf;
}
