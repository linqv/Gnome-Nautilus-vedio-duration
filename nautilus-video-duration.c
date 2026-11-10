#include "vd-helper-pool.h"
#include "vd-media.h"

#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>
#include <nautilus-extension.h>
/* for WIFEXITED/WIFSIGNALED */
#include <signal.h>
#include <string.h>   /* for memcpy */
#include <sys/stat.h> /* for stat */
#include <sys/wait.h>

#define VD_BUILD_TAG                                                           \
  "VD_BUILD_TAG: helper-only-mka-2026-05-03"

#define COLUMN_ID "video-duration::duration"
#define ATTR_KEY "video-duration::duration"
#define NAUTILUS_LIST_VIEW_SCHEMA "org.gnome.nautilus.list-view"
#define NAUTILUS_VISIBLE_COLUMNS_KEY "default-visible-columns"

/* Defaults */
#define DEFAULT_CACHE_MAX_ENTRIES 512
#define DEFAULT_QUEUE_MAX 256
#define DEFAULT_DEBOUNCE_MS 250
#define DEFAULT_NEGATIVE_TTL_SEC 600 /* 10 min */
#define DEFAULT_EVICT_RATIO_PERCENT 70
#define DEFAULT_DROP_NEGATIVE_TTL_MS 2000 /* drop 后短暂负缓存 */

#define DEFAULT_HELPER_MAX 2
#define DEFAULT_HELPER_REQS_BEFORE_RESTART 400
#define DEFAULT_HELPER_TIMEOUT_MS 2000
#define DEFAULT_HELPER_RETRY_TIMEOUT_MS 12000
#define DEFAULT_TIMEOUT_NEG_TTL_SEC 3
#define DEFAULT_HELPER_IDLE_SEC 30 /* helper 空闲回收（秒），0 禁用 */

#ifndef VD_HELPER_PATH
#define VD_HELPER_PATH "/usr/lib/nautilus/extensions-4/vd-ffmpeg-helper"
#endif

/* Flags */
#define FLAG_INFLIGHT (1u << 0)
#define FLAG_HAS_VALUE (1u << 1)
#define FLAG_NEGATIVE (1u << 2)
#define FLAG_QUEUED (1u << 3)


#define VIDEO_DURATION_TYPE (video_duration_get_type())
G_DECLARE_FINAL_TYPE(VideoDuration, video_duration, VIDEO, DURATION, GObject)

struct _VideoDuration {
  GObject parent_instance;
};

static void column_provider_iface_init(NautilusColumnProviderInterface *iface);
static void info_provider_iface_init(NautilusInfoProviderInterface *iface);

G_DEFINE_DYNAMIC_TYPE_EXTENDED(
    VideoDuration, video_duration, G_TYPE_OBJECT, 0,
    G_IMPLEMENT_INTERFACE_DYNAMIC(NAUTILUS_TYPE_COLUMN_PROVIDER,
                                  column_provider_iface_init)
        G_IMPLEMENT_INTERFACE_DYNAMIC(NAUTILUS_TYPE_INFO_PROVIDER,
                                      info_provider_iface_init))

/* ------------------- Cache Entry ------------------- */
typedef struct _CacheEntry CacheEntry;

struct _CacheEntry {
  guint64 key;
  guint64 mtime;
  guint64 size;

  guint32 seconds;
  guint32 flags;

  gint64 negative_until_us;
  gint64 last_request_us;

  CacheEntry *lru_prev;
  CacheEntry *lru_next;
};

/* ------------------- Job ------------------- */
typedef struct _Job Job;

struct _Job {
  NautilusFileInfo *file;
  gchar *path;
  guint64 key;
  gboolean audio; /* worker 不再 basename 分配 */
  Job *pending_next;
};

/* ------------------- Globals ------------------- */
static GThread **workers = NULL;
static gint worker_count = 0;
static Job *pending_head = NULL;
static Job *pending_tail = NULL;
static guint pending_length = 0;
static GCond pending_cond;
static guint queue_max = DEFAULT_QUEUE_MAX;

/* Phase 2.2: Separate locks for better concurrency */
static GMutex pending_lock;        /* protects pending queue only */
static GRWLock cache_rwlock;       /* protects entries/lru - allows concurrent reads */

static GHashTable *entries = NULL; /* key(&CacheEntry.key) -> CacheEntry* */
static CacheEntry *lru_head = NULL;
static CacheEntry *lru_tail = NULL;
static guint lru_length = 0;

static volatile gint shutting_down = 0;
static guint cache_max_entries = DEFAULT_CACHE_MAX_ENTRIES;
static guint debounce_ms = DEFAULT_DEBOUNCE_MS;
static guint negative_ttl_sec = DEFAULT_NEGATIVE_TTL_SEC;
static guint evict_ratio_percent = DEFAULT_EVICT_RATIO_PERCENT;
static guint drop_negative_ttl_ms = DEFAULT_DROP_NEGATIVE_TTL_MS;

/* Helper config */
static guint helper_max = DEFAULT_HELPER_MAX;
static guint helper_timeout_ms = DEFAULT_HELPER_TIMEOUT_MS;
static guint helper_retry_timeout_ms = DEFAULT_HELPER_RETRY_TIMEOUT_MS;
static guint timeout_negative_ttl_sec = DEFAULT_TIMEOUT_NEG_TTL_SEC;
static guint helper_reqs_before_restart = DEFAULT_HELPER_REQS_BEFORE_RESTART;
static guint helper_idle_sec = DEFAULT_HELPER_IDLE_SEC;
static VdHelperPool *helper_pool;
static GSettings *nautilus_list_view_settings = NULL;
static volatile gint duration_column_enabled = 1;

static void cancel_pending_duration_jobs(void);

/* ------------------- Helpers ------------------- */
static gboolean visible_columns_contain_duration(GSettings *settings) {
  if (!settings)
    return TRUE;

  gboolean enabled = FALSE;
  gchar **columns =
      g_settings_get_strv(settings, NAUTILUS_VISIBLE_COLUMNS_KEY);

  if (columns) {
    for (guint i = 0; columns[i]; i++) {
      if (g_strcmp0(columns[i], COLUMN_ID) == 0) {
        enabled = TRUE;
        break;
      }
    }
  }

  g_strfreev(columns);
  return enabled;
}

static void refresh_duration_column_enabled(void) {
  gboolean enabled = visible_columns_contain_duration(nautilus_list_view_settings);
  gint was_enabled = g_atomic_int_get(&duration_column_enabled);

  g_atomic_int_set(&duration_column_enabled, enabled ? 1 : 0);

  if (was_enabled && !enabled)
    cancel_pending_duration_jobs();
}

static void on_visible_columns_changed(GSettings *settings, gchar *key,
                                       gpointer user_data) {
  (void)settings;
  (void)key;
  (void)user_data;
  refresh_duration_column_enabled();
}

/* ------------------- key(uint64) ------------------- */
static inline guint64 mix64(guint64 x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return x;
}
static guint64 make_key_u64(const gchar *path, guint64 mtime, guint64 size) {
  guint32 h1 = g_str_hash(path);
  guint64 x =
      ((guint64)h1 << 32) ^ (mtime * 1315423911ULL) ^ (size * 2654435761ULL);
  return mix64(x);
}
static guint key_u64_hash(gconstpointer p) {
  const guint64 v = *(const guint64 *)p;
  return (guint)(v ^ (v >> 32));
}
static gboolean key_u64_equal(gconstpointer a, gconstpointer b) {
  return (*(const guint64 *)a) == (*(const guint64 *)b);
}

/* ------------------- LRU ------------------- */
static gboolean lru_is_linked_unlocked(const CacheEntry *e) {
  return e && (e == lru_head || e->lru_prev || e->lru_next);
}

static void lru_unlink_unlocked(CacheEntry *e) {
  if (!lru_is_linked_unlocked(e))
    return;

  if (e->lru_prev)
    e->lru_prev->lru_next = e->lru_next;
  else
    lru_head = e->lru_next;

  if (e->lru_next)
    e->lru_next->lru_prev = e->lru_prev;
  else
    lru_tail = e->lru_prev;

  e->lru_prev = NULL;
  e->lru_next = NULL;

  if (lru_length > 0)
    lru_length--;
}

static void lru_append_tail_unlocked(CacheEntry *e) {
  if (!e)
    return;

  e->lru_prev = lru_tail;
  e->lru_next = NULL;

  if (lru_tail)
    lru_tail->lru_next = e;
  else
    lru_head = e;

  lru_tail = e;
  lru_length++;
}

static void lru_touch_unlocked(CacheEntry *e) {
  if (!e)
    return;

  if (e == lru_tail && lru_is_linked_unlocked(e))
    return;

  lru_unlink_unlocked(e);
  lru_append_tail_unlocked(e);
}

static CacheEntry *lru_pop_head_unlocked(void) {
  CacheEntry *e = lru_head;
  if (!e)
    return NULL;

  lru_unlink_unlocked(e);
  return e;
}

static void lru_reset_unlocked(void) {
  lru_head = NULL;
  lru_tail = NULL;
  lru_length = 0;
}

static void entry_destroy(gpointer data) {
  CacheEntry *e = data;
  if (!e)
    return;
  e->lru_prev = NULL;
  e->lru_next = NULL;
  g_free(e);
}

static void cache_entry_remove_unlocked(CacheEntry *e) {
  if (!e)
    return;
  lru_unlink_unlocked(e);
  g_hash_table_remove(entries, &e->key);
}

static void evict_if_needed_unlocked(void) {
  guint sz = g_hash_table_size(entries);
  if (sz <= cache_max_entries)
    return;

  guint target = (cache_max_entries * evict_ratio_percent) / 100;
  if (target < 1)
    target = 1;

  guint tries = 0;
  guint max_tries = lru_length;
  if (max_tries < 1)
    max_tries = 1;

  while (sz > target && tries < max_tries) {
    CacheEntry *old = lru_pop_head_unlocked();
    if (!old)
      break;

    if (old->flags & FLAG_INFLIGHT) {
      lru_append_tail_unlocked(old);
      tries++;
      continue;
    }

    g_hash_table_remove(entries, &old->key);
    sz--;
    tries++;
  }
}

/* ------------------- Pending Queue ------------------- */
static gboolean pending_queue_is_empty_unlocked(void) {
  return pending_head == NULL;
}

static void pending_queue_push_tail_unlocked(Job *job) {
  if (!job)
    return;

  job->pending_next = NULL;
  if (pending_tail)
    pending_tail->pending_next = job;
  else
    pending_head = job;

  pending_tail = job;
  pending_length++;
}

static Job *pending_queue_pop_head_unlocked(void) {
  Job *job = pending_head;
  if (!job)
    return NULL;

  pending_head = job->pending_next;
  if (!pending_head)
    pending_tail = NULL;

  job->pending_next = NULL;
  if (pending_length > 0)
    pending_length--;

  return job;
}

/* ------------------- File stat ------------------- */
static gboolean get_local_path_and_stat(const gchar *uri, gchar **out_path,
                                        guint64 *out_mtime, guint64 *out_size) {
  *out_path = NULL;
  *out_mtime = 0;
  *out_size = 0;
  if (!uri || !g_str_has_prefix(uri, "file://"))
    return FALSE;

  gchar *path = g_filename_from_uri(uri, NULL, NULL);
  if (!path)
    return FALSE;

  struct stat st;
  if (stat(path, &st) != 0) {
    g_free(path);
    return FALSE;
  }

  *out_mtime = st.st_mtime > 0 ? (guint64)st.st_mtime : 0;
  *out_size = st.st_size > 0 ? (guint64)st.st_size : 0;

  *out_path = path;
  return TRUE;
}

/* ------------------- Env ------------------- */
static guint read_env_uint(const gchar *name, guint fallback, guint min,
                           guint max) {
  const gchar *env = g_getenv(name);
  if (!env || !*env)
    return fallback;
  gchar *end = NULL;
  guint64 v = g_ascii_strtoull(env, &end, 10);
  if (end == env)
    return fallback;
  if (v < min)
    v = min;
  if (v > max)
    v = max;
  return (guint)v;
}

static void init_limits_from_env(void) {
  cache_max_entries =
      read_env_uint("NAUTILUS_VD_CACHE", DEFAULT_CACHE_MAX_ENTRIES, 1, 200000);
  queue_max =
      read_env_uint("NAUTILUS_VD_QUEUE_MAX", DEFAULT_QUEUE_MAX, 1, 200000);
  debounce_ms =
      read_env_uint("NAUTILUS_VD_DEBOUNCE_MS", DEFAULT_DEBOUNCE_MS, 0, 5000);
  negative_ttl_sec =
      read_env_uint("NAUTILUS_VD_NEG_TTL", DEFAULT_NEGATIVE_TTL_SEC, 1, 86400);
  evict_ratio_percent = read_env_uint("NAUTILUS_VD_EVICT_RATIO",
                                      DEFAULT_EVICT_RATIO_PERCENT, 10, 95);
  drop_negative_ttl_ms = read_env_uint("NAUTILUS_VD_DROP_NEG_TTL_MS",
                                       DEFAULT_DROP_NEGATIVE_TTL_MS, 0, 60000);

  helper_max =
      read_env_uint("NAUTILUS_VD_HELPER_MAX", DEFAULT_HELPER_MAX, 1, 16);
  helper_timeout_ms = read_env_uint("NAUTILUS_VD_FF_TIMEOUT_MS",
                                    DEFAULT_HELPER_TIMEOUT_MS, 100, 60000);
  helper_retry_timeout_ms =
      read_env_uint("NAUTILUS_VD_FF_RETRY_TIMEOUT_MS",
                    DEFAULT_HELPER_RETRY_TIMEOUT_MS, 200, 180000);
  timeout_negative_ttl_sec = read_env_uint(
      "NAUTILUS_VD_TIMEOUT_NEG_TTL_SEC", DEFAULT_TIMEOUT_NEG_TTL_SEC, 0, 3600);
  helper_reqs_before_restart =
      read_env_uint("NAUTILUS_VD_HELPER_REQS_BEFORE_RESTART",
                    DEFAULT_HELPER_REQS_BEFORE_RESTART, 50, 100000);

  helper_idle_sec = read_env_uint("NAUTILUS_VD_HELPER_IDLE_SEC",
                                  DEFAULT_HELPER_IDLE_SEC, 0, 3600);
}

/* ------------------- Main thread update ------------------- */
typedef struct {
  NautilusFileInfo *file;
  guint32 seconds;
  gboolean negative;
  gboolean audio;
} MainUpdate;

static gboolean apply_update_main(gpointer data) {
  MainUpdate *u = data;
  if (!u)
    return G_SOURCE_REMOVE;

  if (!g_atomic_int_get(&shutting_down) &&
      g_atomic_int_get(&duration_column_enabled)) {
    gchar dur[DURATION_TEXT_MAX];
    const gchar *text =
        (!u->negative && u->seconds > 0)
            ? vd_media_format_seconds(u->seconds, u->audio, dur)
            : "-";
    nautilus_file_info_add_string_attribute(u->file, ATTR_KEY, text);
  }

  g_object_unref(u->file);
  g_free(u);
  return G_SOURCE_REMOVE;
}

static void post_update_to_main(NautilusFileInfo *file, guint32 seconds,
                                gboolean negative, gboolean audio) {
  if (g_atomic_int_get(&shutting_down))
    return;

  MainUpdate *u = g_new0(MainUpdate, 1);
  u->file = g_object_ref(file);
  u->seconds = seconds;
  u->negative = negative;
  u->audio = audio;

  g_main_context_invoke(NULL, apply_update_main, u);
}

/* ------------------- Job / Worker ------------------- */
static void job_free(Job *job) {
  if (!job)
    return;
  if (job->file)
    g_object_unref(job->file);
  g_free(job->path);
  g_free(job);
}

static void clear_job_flags_unlocked(guint64 key) {
  gpointer fk = NULL, fv = NULL;
  if (!entries || !g_hash_table_lookup_extended(entries, &key, &fk, &fv))
    return;

  CacheEntry *e = fv;
  e->flags &= ~(FLAG_INFLIGHT | FLAG_QUEUED);
}

static void cancel_pending_duration_jobs(void) {
  Job *jobs = NULL;

  g_rw_lock_writer_lock(&cache_rwlock);
  g_mutex_lock(&pending_lock);

  for (;;) {
    Job *job = pending_queue_pop_head_unlocked();
    if (!job)
      break;

    clear_job_flags_unlocked(job->key);
    job->pending_next = jobs;
    jobs = job;
  }

  g_mutex_unlock(&pending_lock);
  g_rw_lock_writer_unlock(&cache_rwlock);

  while (jobs) {
    Job *next = jobs->pending_next;
    jobs->pending_next = NULL;
    job_free(jobs);
    jobs = next;
  }
}

static void worker_process(Job *job) {
  if (!job)
    return;
  if (g_atomic_int_get(&shutting_down) ||
      !g_atomic_int_get(&duration_column_enabled)) {
    g_rw_lock_writer_lock(&cache_rwlock);
    clear_job_flags_unlocked(job->key);
    g_rw_lock_writer_unlock(&cache_rwlock);
    job_free(job);
    return;
  }

  DurationResult dr = vd_helper_pool_probe(helper_pool, job->path);
  guint32 seconds = dr.seconds;
  gboolean negative = (seconds == 0);
  gboolean timeout_fail = (seconds == 0 && dr.timed_out);

  gboolean audio = job->audio;

  gint64 now_us = g_get_monotonic_time();

  /* Phase 2.2: Use write lock for cache modification */
  g_rw_lock_writer_lock(&cache_rwlock);

  if (!g_atomic_int_get(&duration_column_enabled)) {
    clear_job_flags_unlocked(job->key);
    g_rw_lock_writer_unlock(&cache_rwlock);
    post_update_to_main(job->file, 0, TRUE, audio);
    job_free(job);
    return;
  }

  gpointer fk = NULL, fv = NULL;
  if (g_hash_table_lookup_extended(entries, &job->key, &fk, &fv)) {
    CacheEntry *e = fv;

    e->flags &= ~(FLAG_INFLIGHT | FLAG_QUEUED);

    if (!negative) {
      e->flags &= ~FLAG_NEGATIVE;
      e->flags |= FLAG_HAS_VALUE;
      e->seconds = seconds;
      e->negative_until_us = 0;
    } else {
      e->flags &= ~FLAG_HAS_VALUE;
      e->flags |= FLAG_NEGATIVE;
      e->seconds = 0;

      guint ttl = negative_ttl_sec;
      if (timeout_fail)
        ttl = timeout_negative_ttl_sec;

      if (ttl == 0)
        e->negative_until_us = now_us;
      else
        e->negative_until_us = now_us + (gint64)ttl * G_USEC_PER_SEC;
    }

    lru_touch_unlocked(e);
    evict_if_needed_unlocked();
  }

  g_rw_lock_writer_unlock(&cache_rwlock);

  post_update_to_main(job->file, seconds, negative, audio);
  job_free(job);
}

static gpointer worker_thread(gpointer data) {
  (void)data;
  for (;;) {
    g_mutex_lock(&pending_lock);
    while (!g_atomic_int_get(&shutting_down) &&
           pending_queue_is_empty_unlocked())
      g_cond_wait(&pending_cond, &pending_lock);

    if (g_atomic_int_get(&shutting_down)) {
      g_mutex_unlock(&pending_lock);
      break;
    }

    Job *job = pending_queue_pop_head_unlocked();
    g_mutex_unlock(&pending_lock);

    if (job)
      worker_process(job);
  }
  return NULL;
}

/* ------------------- Schedule ------------------- */
static void schedule_duration_job(NautilusFileInfo *file) {
  if (g_atomic_int_get(&shutting_down))
    return;

  if (!g_atomic_int_get(&duration_column_enabled))
    return;

  if (nautilus_file_info_get_file_type(file) == G_FILE_TYPE_DIRECTORY)
    return;

  gchar *name = nautilus_file_info_get_name(file);
  MediaKind media_kind = vd_media_kind_from_name(name);
  if (media_kind == MEDIA_KIND_NONE) {
    g_free(name);
    return;
  }
  g_free(name);

  gchar *uri = nautilus_file_info_get_uri(file);
  if (!uri)
    return;

  gchar *path = NULL;
  guint64 mtime = 0, size = 0;
  if (!get_local_path_and_stat(uri, &path, &mtime, &size)) {
    g_free(uri);
    return;
  }
  g_free(uri);

  guint64 key = make_key_u64(path, mtime, size);
  gint64 now_us = g_get_monotonic_time();
  gboolean audio = (media_kind == MEDIA_KIND_AUDIO);

  g_rw_lock_writer_lock(&cache_rwlock);

  if (!g_atomic_int_get(&duration_column_enabled)) {
    g_rw_lock_writer_unlock(&cache_rwlock);
    g_free(path);
    return;
  }

  CacheEntry *e = NULL;
  gpointer fk = NULL, fv = NULL;
  if (g_hash_table_lookup_extended(entries, &key, &fk, &fv)) {
    e = fv;
    if (e->mtime != mtime || e->size != size) {
      cache_entry_remove_unlocked(e);
      e = NULL;
    }
  }

  gboolean is_new = FALSE;
  if (!e) {
    e = g_new0(CacheEntry, 1);
    e->key = key;
    e->mtime = mtime;
    e->size = size;
    e->seconds = 0;
    e->flags = 0;
    e->negative_until_us = 0;
    e->last_request_us = 0;
    e->lru_prev = NULL;
    e->lru_next = NULL;

    g_hash_table_insert(entries, &e->key, e);
    lru_touch_unlocked(e);
    evict_if_needed_unlocked();
    is_new = TRUE;
  }

  /* debounce */
  if (!is_new && debounce_ms > 0 && e->last_request_us) {
    gint64 dt = now_us - e->last_request_us;
    if (dt < (gint64)debounce_ms * 1000) {
      if (e->flags & FLAG_HAS_VALUE) {
        gchar dur[DURATION_TEXT_MAX];
        const gchar *text = vd_media_format_seconds(e->seconds, audio, dur);
        nautilus_file_info_add_string_attribute(file, ATTR_KEY, text);
        lru_touch_unlocked(e);
        g_rw_lock_writer_unlock(&cache_rwlock);
        g_free(path);
        return;
      }
      if ((e->flags & FLAG_NEGATIVE) && now_us < e->negative_until_us) {
        nautilus_file_info_add_string_attribute(file, ATTR_KEY, "-");
        lru_touch_unlocked(e);
        g_rw_lock_writer_unlock(&cache_rwlock);
        g_free(path);
        return;
      }
      if (e->flags & FLAG_INFLIGHT) {
        g_rw_lock_writer_unlock(&cache_rwlock);
        g_free(path);
        return;
      }
    }
  }
  e->last_request_us = now_us;

  /* cache hit */
  if (e->flags & FLAG_HAS_VALUE) {
    gchar dur[DURATION_TEXT_MAX];
    const gchar *text = vd_media_format_seconds(e->seconds, audio, dur);
    nautilus_file_info_add_string_attribute(file, ATTR_KEY, text);
    lru_touch_unlocked(e);
    g_rw_lock_writer_unlock(&cache_rwlock);
    g_free(path);
    return;
  }

  /* negative cache */
  if ((e->flags & FLAG_NEGATIVE) && now_us < e->negative_until_us) {
    nautilus_file_info_add_string_attribute(file, ATTR_KEY, "-");
    lru_touch_unlocked(e);
    g_rw_lock_writer_unlock(&cache_rwlock);
    g_free(path);
    return;
  }

  /* inflight */
  if (e->flags & FLAG_INFLIGHT) {
    g_rw_lock_writer_unlock(&cache_rwlock);
    g_free(path);
    return;
  }

  e->flags |= FLAG_INFLIGHT;

  if (!(e->flags & FLAG_QUEUED)) {
    e->flags |= FLAG_QUEUED;

    Job *job = g_new0(Job, 1);
    job->file = g_object_ref(file);
    job->key = key;

    /* job 接管 path 所有权 */
    job->path = path;
    job->audio = audio;
    path = NULL;

    Job *dropped = NULL;
    g_mutex_lock(&pending_lock);
    if (pending_length >= queue_max)
      dropped = pending_queue_pop_head_unlocked();
    pending_queue_push_tail_unlocked(job);
    g_cond_signal(&pending_cond);
    g_mutex_unlock(&pending_lock);

    if (dropped) {
      gpointer dfk = NULL, dfv = NULL;
      if (g_hash_table_lookup_extended(entries, &dropped->key, &dfk, &dfv)) {
        CacheEntry *de = dfv;
        de->flags &= ~(FLAG_INFLIGHT | FLAG_QUEUED);

        if (drop_negative_ttl_ms > 0) {
          de->flags &= ~FLAG_HAS_VALUE;
          de->flags |= FLAG_NEGATIVE;
          de->seconds = 0;
          de->negative_until_us =
              now_us + (gint64)drop_negative_ttl_ms * 1000;
          lru_touch_unlocked(de);
        }
      }
      post_update_to_main(dropped->file, 0, TRUE, dropped->audio);
    }

    if (dropped)
      job_free(dropped);
  }

  g_rw_lock_writer_unlock(&cache_rwlock);
  g_free(path);
}

static NautilusOperationResult video_duration_update_file_info(
    NautilusInfoProvider *provider, NautilusFileInfo *file,
    GClosure *update_complete, NautilusOperationHandle **handle) {
  (void)provider;
  (void)update_complete;
  (void)handle;

  schedule_duration_job(file);
  return NAUTILUS_OPERATION_COMPLETE;
}

static void info_provider_iface_init(NautilusInfoProviderInterface *iface) {
  iface->update_file_info = video_duration_update_file_info;
}

/* ------------------- ColumnProvider ------------------- */
static GList *video_duration_get_columns(NautilusColumnProvider *provider) {
  (void)provider;
  NautilusColumn *col =
      nautilus_column_new(COLUMN_ID, ATTR_KEY, "时长", "媒体时长（视频/音频）");
  return g_list_append(NULL, col);
}
static void column_provider_iface_init(NautilusColumnProviderInterface *iface) {
  iface->get_columns = video_duration_get_columns;
}

/* ------------------- Class init ------------------- */
static void video_duration_class_init(VideoDurationClass *klass) {
  (void)klass;
}
static void video_duration_class_finalize(VideoDurationClass *klass) {
  (void)klass;
}
static void video_duration_init(VideoDuration *self) { (void)self; }

/* ------------------- Module entry ------------------- */
void nautilus_module_initialize(GTypeModule *module) {
  video_duration_register_type(module);

  g_message("%s", VD_BUILD_TAG);

  g_atomic_int_set(&shutting_down, 0);
  init_limits_from_env();

  /* Phase 2.2: Initialize separate locks */
  g_rw_lock_init(&cache_rwlock);
  g_mutex_init(&pending_lock);
  g_cond_init(&pending_cond);

  nautilus_list_view_settings = g_settings_new(NAUTILUS_LIST_VIEW_SCHEMA);
  refresh_duration_column_enabled();
  g_signal_connect(nautilus_list_view_settings, "changed::" NAUTILUS_VISIBLE_COLUMNS_KEY,
                   G_CALLBACK(on_visible_columns_changed), NULL);

  entries =
      g_hash_table_new_full(key_u64_hash, key_u64_equal, NULL, entry_destroy);
  pending_head = NULL;
  pending_tail = NULL;
  pending_length = 0;

  const gchar *override_path = g_getenv("NAUTILUS_VD_HELPER");
  VdHelperConfig helper_config = {
      .path = override_path && *override_path ? override_path : VD_HELPER_PATH,
      .capacity = helper_max,
      .timeout_ms = helper_timeout_ms,
      .retry_timeout_ms = helper_retry_timeout_ms,
      .requests_before_restart = helper_reqs_before_restart,
      .idle_sec = helper_idle_sec,
  };
  helper_pool = vd_helper_pool_new(&helper_config);

  /* helper idle GC starts lazily when a helper becomes idle. */

  /* threads default: <= 8 */
  gint n_threads = (gint)g_get_num_processors();
  if (n_threads < 1)
    n_threads = 1;
  gint capped = n_threads > 8 ? 8 : n_threads;

  /* 默认线程数对齐 helper_max（受 capped 限制） */
  guint def_workers = helper_max;
  if (def_workers < 1)
    def_workers = 1;
  if ((gint)def_workers > capped)
    def_workers = (guint)capped;

  worker_count =
      (gint)read_env_uint("NAUTILUS_VD_THREADS", def_workers, 1, 128);

  g_message("VD: helperpool threads=%d helper_max=%u helper_idle_sec=%u "
            "cache_max=%u queue_max=%u debounce_ms=%u neg_ttl=%u "
            "evict_ratio=%u drop_neg_ttl_ms=%u ff_timeout=%u ff_retry=%u "
            "timeout_neg_ttl=%u helper_reqs_restart=%u helper_path=%s "
            "column_enabled=%d",
            worker_count, helper_max, helper_idle_sec, cache_max_entries,
            queue_max, debounce_ms, negative_ttl_sec, evict_ratio_percent,
            drop_negative_ttl_ms, helper_timeout_ms, helper_retry_timeout_ms,
            timeout_negative_ttl_sec, helper_reqs_before_restart,
            helper_config.path, g_atomic_int_get(&duration_column_enabled));

  workers = g_new0(GThread *, worker_count);
  for (gint i = 0; i < worker_count; i++)
    workers[i] = g_thread_new("vd-worker", worker_thread, NULL);
}

void nautilus_module_shutdown(void) {
  g_atomic_int_set(&shutting_down, 1);

  /* wake pending workers */
  g_mutex_lock(&pending_lock);
  g_cond_broadcast(&pending_cond);
  g_mutex_unlock(&pending_lock);

  /* wake helper pool waiters */
  vd_helper_pool_stop(helper_pool);

  if (workers) {
    for (gint i = 0; i < worker_count; i++)
      if (workers[i])
        g_thread_join(workers[i]);
    g_free(workers);
    workers = NULL;
    worker_count = 0;
  }

  g_rw_lock_writer_lock(&cache_rwlock);

  g_mutex_lock(&pending_lock);
  for (;;) {
    Job *job = pending_queue_pop_head_unlocked();
    if (!job)
      break;
    job_free(job);
  }
  g_mutex_unlock(&pending_lock);

  lru_reset_unlocked();

  if (entries) {
    g_hash_table_destroy(entries);
    entries = NULL;
  }

  g_rw_lock_writer_unlock(&cache_rwlock);

  vd_helper_pool_free(helper_pool);
  helper_pool = NULL;

  if (nautilus_list_view_settings) {
    g_signal_handlers_disconnect_by_func(nautilus_list_view_settings,
                                         G_CALLBACK(on_visible_columns_changed),
                                         NULL);
    g_clear_object(&nautilus_list_view_settings);
  }

  g_cond_clear(&pending_cond);
  g_mutex_clear(&pending_lock);
  g_rw_lock_clear(&cache_rwlock);
}

void nautilus_module_list_types(const GType **types, int *num_types) {
  static GType type_list[1];
  type_list[0] = VIDEO_DURATION_TYPE;
  *types = type_list;
  *num_types = 1;
}
