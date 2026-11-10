#include "vd-scheduler.h"

typedef struct {
  VdCacheEntry *reservation;
  GObject *target;
  MediaKind kind;
  guint32 seconds;
  gboolean cancelled;
} Job;

struct VdScheduler {
  VdCache *cache;
  VdHelperPool *pool;
  VdDurationUpdate update;
  GThread **workers;
  guint n_workers;
  guint queue_max;
  GMutex lock;
  GCond available;
  GQueue pending;
  GQueue completed;
  guint update_source;
  gboolean enabled;
  gboolean stopping;
};

static void job_free(gpointer data) {
  Job *job = data;
  g_object_unref(job->target);
  g_free(job);
}

static gboolean dispatch_result(gpointer data) {
  VdScheduler *s = data;
  g_mutex_lock(&s->lock);
  Job *job = g_queue_pop_head(&s->completed);
  gboolean more = !g_queue_is_empty(&s->completed);
  if (!more)
    s->update_source = 0;
  g_mutex_unlock(&s->lock);

  if (!job->cancelled)
    s->update(job->target, job->kind, job->seconds);
  job_free(job);
  return more;
}

static gpointer worker_thread(gpointer data) {
  VdScheduler *s = data;
  g_mutex_lock(&s->lock);
  while (!s->stopping) {
    Job *job = g_queue_pop_head(&s->pending);
    if (!job) {
      g_cond_wait(&s->available, &s->lock);
      continue;
    }
    g_mutex_unlock(&s->lock);
    DurationResult result =
        vd_helper_pool_probe(s->pool, vd_cache_entry_path(job->reservation));
    g_mutex_lock(&s->lock);
    if (s->stopping || !s->enabled) {
      vd_cache_cancel(s->cache, job->reservation);
      job->cancelled = TRUE;
    } else {
      vd_cache_finish(s->cache, job->reservation, result.seconds,
                      result.timed_out, g_get_monotonic_time());
      job->seconds = result.seconds;
    }
    job->reservation = NULL;
    g_queue_push_tail(&s->completed, job);
    if (!s->stopping && !s->update_source)
      s->update_source =
          g_idle_add_full(G_PRIORITY_DEFAULT, dispatch_result, s, NULL);
  }
  g_mutex_unlock(&s->lock);
  return NULL;
}

VdScheduler *vd_scheduler_new(const VdSchedulerConfig *config,
                              VdDurationUpdate update) {
  VdScheduler *s = g_new0(VdScheduler, 1);
  s->cache = vd_cache_new(&config->cache);
  s->pool = vd_helper_pool_new(&config->helper);
  s->update = update;
  s->enabled = TRUE;
  s->queue_max = config->queue_max;
  s->n_workers = config->workers;
  g_mutex_init(&s->lock);
  g_cond_init(&s->available);
  s->workers = g_new(GThread *, s->n_workers);
  for (guint i = 0; i < s->n_workers; i++)
    s->workers[i] = g_thread_new("vd-worker", worker_thread, s);
  return s;
}

void vd_scheduler_submit(VdScheduler *s, const gchar *path, guint64 mtime,
                         guint64 size, MediaKind kind, GObject *target) {
  if (!s->enabled)
    return;
  guint32 seconds;
  VdCacheEntry *reservation;
  gint64 now = g_get_monotonic_time();
  g_mutex_lock(&s->lock);
  VdCacheDecision decision = vd_cache_reserve(s->cache, path, mtime, size, now,
                                              &seconds, &reservation);
  if (decision != VD_CACHE_RESERVED) {
    g_mutex_unlock(&s->lock);
    if (decision != VD_CACHE_BUSY)
      s->update(target, kind, seconds);
    return;
  }

  Job *job = g_new0(Job, 1);
  job->reservation = reservation;
  job->target = g_object_ref(target);
  job->kind = kind;
  Job *dropped = NULL;
  if (s->pending.length >= s->queue_max) {
    dropped = g_queue_pop_head(&s->pending);
    vd_cache_drop(s->cache, dropped->reservation, now);
  }
  g_queue_push_tail(&s->pending, job);
  g_cond_signal(&s->available);
  g_mutex_unlock(&s->lock);
  if (dropped) {
    s->update(dropped->target, dropped->kind, 0);
    job_free(dropped);
  }
}

void vd_scheduler_set_enabled(VdScheduler *s, gboolean enabled) {
  GQueue discarded = G_QUEUE_INIT;
  g_mutex_lock(&s->lock);
  s->enabled = enabled;
  if (!enabled) {
    if (s->update_source) {
      g_source_remove(s->update_source);
      s->update_source = 0;
    }
    discarded = s->completed;
    g_queue_init(&s->completed);
    Job *job;
    while ((job = g_queue_pop_head(&s->pending))) {
      vd_cache_cancel(s->cache, job->reservation);
      g_queue_push_tail(&discarded, job);
    }
  }
  g_mutex_unlock(&s->lock);
  g_queue_clear_full(&discarded, job_free);
}

void vd_scheduler_free(VdScheduler *s) {
  g_mutex_lock(&s->lock);
  s->stopping = TRUE;
  g_cond_broadcast(&s->available);
  g_mutex_unlock(&s->lock);
  vd_helper_pool_stop(s->pool);
  for (guint i = 0; i < s->n_workers; i++)
    g_thread_join(s->workers[i]);
  vd_scheduler_set_enabled(s, FALSE);
  vd_helper_pool_free(s->pool);
  vd_cache_free(s->cache);
  g_free(s->workers);
  g_cond_clear(&s->available);
  g_mutex_clear(&s->lock);
  g_free(s);
}
