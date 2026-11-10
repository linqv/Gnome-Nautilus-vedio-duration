#include "vd-helper-pool.h"
#include <glib/gstdio.h>

static const gchar *helper_path;

static VdHelperPool *new_pool(void)
{
  VdHelperConfig config = {.path = helper_path,
                           .capacity = 1,
                           .timeout_ms = 300,
                           .retry_timeout_ms = 600,
                           .requests_before_restart = 3,
                           .idle_sec = 30};
  return vd_helper_pool_new(&config);
}

static void reuse_and_recover(void)
{
  VdHelperPool *pool = new_pool();
  g_assert_cmpuint(vd_helper_pool_probe(pool, "a path\nwith spaces").seconds,
                   ==, 42);
  g_assert_true(vd_helper_pool_probe(pool, "timeout").timed_out);
  g_assert_cmpuint(vd_helper_pool_probe(pool, "sequence").seconds, ==, 3);
  g_assert_cmpuint(vd_helper_pool_probe(pool, "sequence").seconds, ==, 1);
  g_assert_cmpuint(vd_helper_pool_probe(pool, "crash").seconds, ==, 0);
  g_assert_cmpuint(vd_helper_pool_probe(pool, "sequence").seconds, ==, 1);
  vd_helper_pool_free(pool);
}

typedef struct
{
  VdHelperPool *pool;
  const gchar *path;
} Request;

static gpointer probe(gpointer data)
{
  Request *req = data;
  g_assert_cmpuint(vd_helper_pool_probe(req->pool, req->path).seconds, ==, 0);
  return NULL;
}

static void stop_while_busy(void)
{
  VdHelperPool *pool = new_pool();
  gchar *dir = g_dir_make_tmp("vd-pool-XXXXXX", NULL);
  g_assert_nonnull(dir);
  Request busy = {pool, dir}, waiting = {pool, "sequence"};
  gchar *ready = g_build_filename(dir, "ready", NULL);
  GThread *first = g_thread_new("busy", probe, &busy);
  while (!g_file_test(ready, G_FILE_TEST_EXISTS))
    g_usleep(1000);
  GThread *second = g_thread_new("waiting", probe, &waiting);
  vd_helper_pool_stop(pool);
  g_thread_join(first);
  g_thread_join(second);
  vd_helper_pool_free(pool);
  g_remove(ready);
  g_rmdir(dir);
  g_free(ready);
  g_free(dir);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_assert_cmpint(argc, ==, 2);
  helper_path = argv[1];
  g_test_add_func("/pool/reuse-and-recover", reuse_and_recover);
  g_test_add_func("/pool/stop-while-busy", stop_while_busy);
  return g_test_run();
}
