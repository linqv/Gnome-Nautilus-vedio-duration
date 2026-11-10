#include "vd-scheduler.h"
#include <glib/gstdio.h>

static GThread *main_thread;
static const gchar *helper_path;

typedef struct {
  guint calls;
  guint32 seconds;
} Updates;

static void record(GObject *target, MediaKind kind, guint32 seconds) {
  Updates *u = g_object_get_data(target, "updates");
  g_assert_true(g_thread_self() == main_thread);
  g_assert_cmpint(kind, ==, MEDIA_KIND_AUDIO);
  u->calls++;
  u->seconds = seconds;
}

static GObject *target(Updates *updates) {
  GObject *object = g_object_new(G_TYPE_OBJECT, NULL);
  g_object_set_data(object, "updates", updates);
  return object;
}

static VdScheduler *new_scheduler(void) {
  VdSchedulerConfig config = {
      .workers = 1,
      .queue_max = 1,
      .cache = {.max_entries = 8,
                .evict_ratio_percent = 70,
                .drop_ttl_ms = 2000},
      .helper = {.path = helper_path,
                 .capacity = 1,
                 .timeout_ms = 2000,
                 .retry_timeout_ms = 12000,
                 .requests_before_restart = 100,
                 .idle_sec = 30},
  };
  return vd_scheduler_new(&config, record);
}

static void submit(VdScheduler *s, const gchar *path, GObject *target) {
  vd_scheduler_submit(s, path, 1, 2, MEDIA_KIND_AUDIO, target);
}

static void queue_and_visibility(void) {
  g_autofree gchar *dir = g_dir_make_tmp("vd-queue-XXXXXX", NULL);
  g_assert_nonnull(dir);
  g_autofree gchar *ready = g_build_filename(dir, "ready", NULL);
  g_autofree gchar *release = g_build_filename(dir, "release", NULL);
  Updates busy = {0}, dropped = {0}, queued = {0};
  g_autoptr(GObject) a = target(&busy), b = target(&dropped),
                     c = target(&queued);
  VdScheduler *s = new_scheduler();
  submit(s, dir, a);
  while (!g_file_test(ready, G_FILE_TEST_EXISTS))
    g_usleep(1000);
  submit(s, dir, a);
  submit(s, "oldest", b);
  submit(s, "sequence", c);
  g_assert_cmpuint(dropped.calls, ==, 1);
  g_assert_cmpuint(dropped.seconds, ==, 0);
  submit(s, "oldest", b);
  g_assert_cmpuint(dropped.calls, ==, 2);

  vd_scheduler_set_enabled(s, FALSE);
  submit(s, "sequence", c);
  vd_scheduler_set_enabled(s, TRUE);
  submit(s, "sequence", c);
  g_assert_true(g_file_set_contents(release, "", 0, NULL));
  while (!queued.calls)
    g_main_context_iteration(NULL, TRUE);
  g_assert_cmpuint(busy.calls, ==, 1);
  g_assert_cmpuint(busy.seconds, ==, 42);
  g_assert_cmpuint(queued.calls, ==, 1);
  g_assert_cmpuint(queued.seconds, ==, 2);
  submit(s, "sequence", c);
  g_assert_cmpuint(queued.calls, ==, 2);
  g_assert_cmpuint(queued.seconds, ==, 2);

  vd_scheduler_free(s);
  g_remove(ready);
  g_remove(release);
  g_rmdir(dir);
}

static void unload_while_busy(void) {
  g_autofree gchar *dir = g_dir_make_tmp("vd-unload-XXXXXX", NULL);
  g_assert_nonnull(dir);
  g_autofree gchar *ready = g_build_filename(dir, "ready", NULL);
  Updates updates = {0};
  GObject *obj = target(&updates);
  GObject *weak = obj;
  g_object_add_weak_pointer(obj, (gpointer *)&weak);
  VdScheduler *s = new_scheduler();
  submit(s, dir, obj);
  while (!g_file_test(ready, G_FILE_TEST_EXISTS))
    g_usleep(1000);
  submit(s, "pending", obj);
  g_object_unref(obj);
  vd_scheduler_free(s);
  g_assert_null(weak);
  g_assert_cmpuint(updates.calls, ==, 0);
  g_remove(ready);
  g_rmdir(dir);
}

static void unload_pending_update(void) {
  Updates updates = {0};
  GObject *obj = target(&updates);
  GObject *weak = obj;
  g_object_add_weak_pointer(obj, (gpointer *)&weak);
  VdScheduler *s = new_scheduler();
  submit(s, "sequence", obj);
  g_object_unref(obj);
  while (!g_main_context_pending(NULL))
    g_usleep(1000);
  vd_scheduler_free(s);
  while (g_main_context_iteration(NULL, FALSE)) {
  }
  g_assert_cmpuint(updates.calls, ==, 0);
  g_assert_null(weak);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_assert_cmpint(argc, ==, 2);
  helper_path = argv[1];
  main_thread = g_thread_self();
  g_test_add_func("/scheduler/queue-and-visibility", queue_and_visibility);
  g_test_add_func("/scheduler/unload-while-busy", unload_while_busy);
  g_test_add_func("/scheduler/unload-pending-update", unload_pending_update);
  return g_test_run();
}
