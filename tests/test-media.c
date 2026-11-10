#include "vd-media.h"

static void classify(void)
{
  g_assert_cmpint(vd_media_kind_from_name("track.MKA"), ==, MEDIA_KIND_AUDIO);
  g_assert_cmpint(vd_media_kind_from_name("movie.MkV"), ==, MEDIA_KIND_VIDEO);
  g_assert_cmpint(vd_media_kind_from_name("note.xyz"), ==, MEDIA_KIND_NONE);
  g_assert_cmpint(vd_media_kind_from_name("README"), ==, MEDIA_KIND_NONE);
}

static void format(void)
{
  gchar buf[DURATION_TEXT_MAX];
  g_assert_cmpstr(vd_media_format_seconds(0, FALSE, buf), ==, "-");
  g_assert_cmpstr(vd_media_format_seconds(3599, TRUE, buf), ==, "59:59");
  g_assert_cmpstr(vd_media_format_seconds(3600, TRUE, buf), ==, "1:00:00");
  g_assert_cmpstr(vd_media_format_seconds(59, FALSE, buf), ==, "0:00:59");
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/media/classify", classify);
  g_test_add_func("/media/format", format);
  return g_test_run();
}
