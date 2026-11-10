#include <glib.h>
#include <stdio.h>
#include <string.h>

/* A directory request signals "ready" and blocks until "release" exists. */
int main(void)
{
  gchar *line = NULL;
  size_t capacity = 0;
  guint requests = 0;
  while (getline(&line, &capacity, stdin) >= 0)
  {
    g_strchomp(line);
    if (g_str_equal(line, "QUIT"))
      break;
    gchar **parts = g_strsplit(line, " ", 4);
    if (g_strv_length(parts) != 4 || !g_str_equal(parts[0], "REQ"))
      return 1;
    gsize len;
    guchar *bytes = g_base64_decode(parts[3], &len);
    gchar *path = g_strndup((gchar *)bytes, len);
    g_free(bytes);
    g_strfreev(parts);
    if (g_str_equal(path, "crash"))
      return 1;
    if (g_file_test(path, G_FILE_TEST_IS_DIR))
    {
      gchar *ready = g_build_filename(path, "ready", NULL);
      gchar *release = g_build_filename(path, "release", NULL);
      if (!g_file_set_contents(ready, "", 0, NULL))
        return 1;
      while (!g_file_test(release, G_FILE_TEST_EXISTS))
        g_usleep(1000);
      g_free(ready);
      g_free(release);
    }
    requests++;
    if (g_str_equal(path, "timeout"))
      puts("OK 0 T");
    else if (g_str_equal(path, "sequence"))
      printf("OK %u \n", requests);
    else
      puts("OK 42 ");
    fflush(stdout);
    g_free(path);
  }
  free(line);
  return 0;
}
