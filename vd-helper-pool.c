#include "vd-helper-pool.h"

#include <gio/gio.h>
#include <string.h>

typedef struct
{
  GSubprocess *proc;
  GDataInputStream *din;
  GOutputStream *out;
  guint req_count;
  gboolean broken;
} HelperInstance;

struct VdHelperPool
{
  VdHelperConfig config;
  gchar *path;
  GMutex lock;
  GCond available;
  GQueue idle;
  guint count;
  guint timer_id;
  gint64 last_used_us;
  GCancellable *cancel;
};

static void helper_instance_destroy(gpointer data)
{
  HelperInstance *h = data;
  if (!h->broken)
    g_output_stream_write_all(h->out, "QUIT\n", 5, NULL, NULL, NULL);
  g_subprocess_force_exit(h->proc);
  g_subprocess_wait(h->proc, NULL, NULL);
  g_object_unref(h->out);
  g_object_unref(h->din);
  g_object_unref(h->proc);
  g_free(h);
}

static HelperInstance *helper_instance_spawn(const gchar *helper_path)
{
  GError *err = NULL;
  const gchar *argv[] = {helper_path, NULL};

  GSubprocess *p = g_subprocess_newv(argv,
                                     G_SUBPROCESS_FLAGS_STDIN_PIPE |
                                         G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                         G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                     &err);
  if (!p)
  {
    g_message("VD: spawn helper failed: %s", err ? err->message : "(unknown)");
    g_clear_error(&err);
    return NULL;
  }

  GInputStream *stdout_is = g_subprocess_get_stdout_pipe(p);
  GOutputStream *stdin_os = g_subprocess_get_stdin_pipe(p);

  HelperInstance *h = g_new0(HelperInstance, 1);
  h->proc = p;
  h->din = g_data_input_stream_new(stdout_is);
  h->out = g_object_ref(stdin_os);
  h->req_count = 0;

  g_data_input_stream_set_newline_type(h->din, G_DATA_STREAM_NEWLINE_TYPE_LF);
  return h;
}

/* One request to helper. Protocol:
 *   REQ <timeout_ms> <retry_timeout_ms> <b64(path)>\n
 *   -> OK <seconds> <flags>\n
 * flags: include 'T' if timed out.
 */
static DurationResult helper_request_duration(VdHelperPool *pool,
                                              HelperInstance *h,
                                              const gchar *path)
{
  DurationResult r = {0, FALSE};
  if (!h || !path)
    return r;

  gchar *b64 = g_base64_encode((const guchar *)path, (gsize)strlen(path));
  gchar *line = g_strdup_printf("REQ %u %u %s\n", pool->config.timeout_ms,
                                pool->config.retry_timeout_ms, b64);
  g_free(b64);

  GError *err = NULL;
  gsize written = 0;
  gboolean ok = g_output_stream_write_all(h->out, line, strlen(line), &written,
                                          pool->cancel, &err);
  g_free(line);

  if (!ok)
  {
    h->broken = TRUE;
    g_clear_error(&err);
    return r;
  }
  ok = g_output_stream_flush(h->out, pool->cancel, &err);
  if (!ok)
  {
    h->broken = TRUE;
    g_clear_error(&err);
    return r;
  }

  gsize len = 0;
  gchar *resp = g_data_input_stream_read_line(h->din, &len, pool->cancel, &err);
  if (!resp)
  {
    h->broken = TRUE;
    g_clear_error(&err);
    return r;
  }

  if (g_str_has_prefix(resp, "OK"))
  {
    gchar **parts = g_strsplit(resp, " ", 0);
    if (parts && parts[1])
    {
      guint64 v = g_ascii_strtoull(parts[1], NULL, 10);
      if (v > G_MAXUINT32)
        v = G_MAXUINT32;
      r.seconds = (guint32)v;
    }
    if (parts && parts[1] && parts[2])
    {
      if (strchr(parts[2], 'T'))
        r.timed_out = TRUE;
    }
    g_strfreev(parts);
  }

  g_free(resp);
  return r;
}

VdHelperPool *vd_helper_pool_new(const VdHelperConfig *config)
{
  VdHelperPool *pool = g_new0(VdHelperPool, 1);
  pool->config = *config;
  pool->path = g_strdup(config->path);
  pool->config.path = pool->path;
  pool->cancel = g_cancellable_new();
  g_mutex_init(&pool->lock);
  g_cond_init(&pool->available);
  return pool;
}

static HelperInstance *acquire(VdHelperPool *pool)
{
  g_mutex_lock(&pool->lock);
  while (!g_cancellable_is_cancelled(pool->cancel))
  {
    HelperInstance *h = g_queue_pop_head(&pool->idle);
    if (h)
    {
      g_mutex_unlock(&pool->lock);
      return h;
    }

    if (pool->count < pool->config.capacity)
    {
      pool->count++;
      g_mutex_unlock(&pool->lock);
      h = helper_instance_spawn(pool->path);
      if (!h)
      {
        g_mutex_lock(&pool->lock);
        pool->count--;
        g_cond_signal(&pool->available);
        g_mutex_unlock(&pool->lock);
      }
      return h;
    }
    g_cond_wait(&pool->available, &pool->lock);
  }
  g_mutex_unlock(&pool->lock);
  return NULL;
}

static gboolean reap_idle(gpointer data)
{
  VdHelperPool *pool = data;
  g_mutex_lock(&pool->lock);
  if (!g_queue_is_empty(&pool->idle) &&
      g_get_monotonic_time() - pool->last_used_us <
          (gint64)pool->config.idle_sec * G_USEC_PER_SEC)
  {
    g_mutex_unlock(&pool->lock);
    return G_SOURCE_CONTINUE;
  }
  HelperInstance *h;
  while ((h = g_queue_pop_head(&pool->idle)))
  {
    helper_instance_destroy(h);
    pool->count--;
  }
  pool->timer_id = 0;
  g_cond_broadcast(&pool->available);
  g_mutex_unlock(&pool->lock);
  return G_SOURCE_REMOVE;
}

DurationResult vd_helper_pool_probe(VdHelperPool *pool, const gchar *path)
{
  HelperInstance *h = acquire(pool);
  if (!h)
    return (DurationResult){0};

  DurationResult result = helper_request_duration(pool, h, path);
  h->req_count++;
  gboolean retire =
      h->broken || h->req_count >= pool->config.requests_before_restart;
  if (retire)
    helper_instance_destroy(h);

  g_mutex_lock(&pool->lock);
  pool->last_used_us = g_get_monotonic_time();
  if (retire)
  {
    pool->count--;
  }
  else
  {
    g_queue_push_tail(&pool->idle, h);
    if (!pool->timer_id && pool->config.idle_sec &&
        !g_cancellable_is_cancelled(pool->cancel))
      pool->timer_id = g_timeout_add_seconds(5, reap_idle, pool);
  }
  g_cond_signal(&pool->available);
  g_mutex_unlock(&pool->lock);
  return result;
}

void vd_helper_pool_stop(VdHelperPool *pool)
{
  g_cancellable_cancel(pool->cancel);
  g_mutex_lock(&pool->lock);
  if (pool->timer_id)
  {
    g_source_remove(pool->timer_id);
    pool->timer_id = 0;
  }
  g_cond_broadcast(&pool->available);
  g_mutex_unlock(&pool->lock);
}

void vd_helper_pool_free(VdHelperPool *pool)
{
  vd_helper_pool_stop(pool);
  g_queue_clear_full(&pool->idle, helper_instance_destroy);
  g_object_unref(pool->cancel);
  g_free(pool->path);
  g_cond_clear(&pool->available);
  g_mutex_clear(&pool->lock);
  g_free(pool);
}
