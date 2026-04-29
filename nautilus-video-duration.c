#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>
#include <nautilus-extension.h>
/* for WIFEXITED/WIFSIGNALED */
#include <sys/wait.h>
#include <signal.h>
#include <string.h>  /* for memcpy */
#include <setjmp.h>  /* for sigjmp_buf for crash protection */

/* Embedded FFmpeg support (direct API calls, no IPC overhead) */
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>

#define VD_BUILD_TAG                                                           \
  "VD_BUILD_TAG: EMBEDDED-ffmpeg-rwlock-2026-01-31"

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
typedef struct {
  guint64 *keyp;
  guint64 mtime;
  guint64 size;

  guint32 seconds;
  guint32 flags;

  gint64 negative_until_us;
  gint64 last_request_us;

  GList *lru_link; /* O(1) */
} CacheEntry;

/* ------------------- Job ------------------- */
typedef struct {
  NautilusFileInfo *file;
  gchar *path;
  guint64 key;
  gboolean audio; /* worker 不再 basename 分配 */
} Job;

/* ------------------- Globals ------------------- */
static GThread **workers = NULL;
static gint worker_count = 0;
static GQueue *pending = NULL;
static GCond pending_cond;
static guint queue_max = DEFAULT_QUEUE_MAX;

/* Phase 2.2: Separate locks for better concurrency */
static GMutex pending_lock;        /* protects pending queue only */
static GRWLock cache_rwlock;       /* protects entries/lru - allows concurrent reads */

static GHashTable *entries = NULL; /* key(uint64*) -> CacheEntry* */
static GQueue *lru = NULL;

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
static guint helper_idle_source_id = 0;
static GSettings *nautilus_list_view_settings = NULL;
static volatile gint duration_column_enabled = 1;

/* ------------------- Helpers ------------------- */
static gboolean is_media_file(const gchar *name) {
  if (!name)
    return FALSE;
  const gchar *ext = strrchr(name, '.');
  if (!ext)
    return FALSE;
  ext++;
  const gchar *media_exts[] = {
      "mp4", "mkv",  "avi",  "mov", "webm", "flv",  "wmv",  "m4v", "mpeg",
      "mpg", "m2ts", "ts",   "3gp", "ogv",  "mp3",  "flac", "wav", "aac",
      "m4a", "ogg",  "opus", "wma", "alac", "aiff", NULL};
  for (int i = 0; media_exts[i]; i++)
    if (g_ascii_strcasecmp(ext, media_exts[i]) == 0)
      return TRUE;
  return FALSE;
}

static gboolean is_audio_ext(const gchar *name) {
  if (!name)
    return FALSE;
  const gchar *ext = strrchr(name, '.');
  if (!ext)
    return FALSE;
  ext++;
  const gchar *audio_exts[] = {"mp3",  "flac", "wav",  "aac",  "m4a", "ogg",
                               "opus", "wma",  "alac", "aiff","mka",NULL};
  for (int i = 0; audio_exts[i]; i++)
    if (g_ascii_strcasecmp(ext, audio_exts[i]) == 0)
      return TRUE;
  return FALSE;
}

static gchar *format_seconds(guint32 sec, gboolean is_audio) {
  if (sec == 0)
    return g_strdup("-");
  if (is_audio && sec < 3600) {
    guint32 m = sec / 60, s = sec % 60;
    return g_strdup_printf("%02u:%02u", m, s);
  }
  guint32 h = sec / 3600;
  guint32 m = (sec % 3600) / 60;
  guint32 s = sec % 60;
  return g_strdup_printf("%u:%02u:%02u", h, m, s);
}

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
  g_atomic_int_set(&duration_column_enabled, enabled ? 1 : 0);
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
static void lru_touch_unlocked(CacheEntry *e) {
  if (!e)
    return;
  if (e->lru_link) {
    g_queue_unlink(lru, e->lru_link);
    g_queue_push_tail_link(lru, e->lru_link);
  } else {
    g_queue_push_tail(lru, e);
    e->lru_link = g_queue_peek_tail_link(lru);
  }
}

static void entry_destroy(gpointer data) {
  CacheEntry *e = data;
  if (!e)
    return;
  e->lru_link = NULL;
  g_free(e);
}

static void cache_entry_remove_unlocked(CacheEntry *e) {
  if (!e)
    return;
  if (e->lru_link) {
    g_queue_unlink(lru, e->lru_link);
    e->lru_link = NULL;
  }
  g_hash_table_remove(entries, e->keyp);
}

static void evict_if_needed_unlocked(void) {
  guint sz = g_hash_table_size(entries);
  if (sz <= cache_max_entries)
    return;

  guint target = (cache_max_entries * evict_ratio_percent) / 100;
  if (target < 1)
    target = 1;

  guint tries = 0;
  guint max_tries = (guint)g_queue_get_length(lru);
  if (max_tries < 1)
    max_tries = 1;

  while (sz > target && tries < max_tries) {
    CacheEntry *old = g_queue_pop_head(lru);
    if (!old)
      break;

    old->lru_link = NULL;

    if (old->flags & FLAG_INFLIGHT) {
      g_queue_push_tail(lru, old);
      old->lru_link = g_queue_peek_tail_link(lru);
      tries++;
      continue;
    }

    g_hash_table_remove(entries, old->keyp);
    sz--;
    tries++;
  }
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

  GFile *gf = g_file_new_for_path(path);
  GFileInfo *fi = g_file_query_info(
      gf, G_FILE_ATTRIBUTE_TIME_MODIFIED "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
      G_FILE_QUERY_INFO_NONE, NULL, NULL);

  if (fi) {
    *out_mtime =
        g_file_info_get_attribute_uint64(fi, G_FILE_ATTRIBUTE_TIME_MODIFIED);
    *out_size =
        g_file_info_get_attribute_uint64(fi, G_FILE_ATTRIBUTE_STANDARD_SIZE);
    g_object_unref(fi);
  }
  g_object_unref(gf);

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

  if (!g_atomic_int_get(&shutting_down)) {
    gchar *dur = NULL;
    if (!u->negative && u->seconds > 0)
      dur = format_seconds(u->seconds, u->audio);
    else
      dur = g_strdup("-");

    nautilus_file_info_add_string_attribute(u->file, ATTR_KEY, dur);
    g_free(dur);
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

/* ------------------- Helper Pool ------------------- */
typedef struct {
  GSubprocess *proc;
  GDataInputStream *din;
  GOutputStream *out;
  guint req_count;
} HelperInstance;

typedef struct {
  GMutex m;
  GCond c;
  GQueue *free_list; /* HelperInstance* */
  gint total_created;
  gint capacity;
  gboolean inited;
  gchar *helper_path;

  /* 用于空闲回收判断：只在一次“请求结束后”更新 */
  gint64 last_used_us;
} HelperPool;

static HelperPool helper_pool;

static gchar *resolve_helper_path(void) {
  const gchar *env = g_getenv("NAUTILUS_VD_HELPER");
  if (env && *env)
    return g_strdup(env);
  return g_strdup(VD_HELPER_PATH);
}

static void helper_instance_destroy(HelperInstance *h) {
  if (!h)
    return;

  if (h->proc) {
    /* 先温柔退出：发送 QUIT，让 helper 自己 break 主循环 */
    if (h->out) {
      const gchar *q = "QUIT\n";
      GError *err = NULL;
      g_output_stream_write_all(h->out, q, strlen(q), NULL, NULL, &err);
      g_clear_error(&err);
      g_output_stream_flush(h->out, NULL, NULL);
    }

    /* 请求退出（SIGTERM） */
    g_subprocess_send_signal(h->proc, SIGTERM);

    /* 等最多 200ms：轮询 status（结束后才会变为 != -1） */
    for (int i = 0; i < 20; i++) {
      int st = g_subprocess_get_status(h->proc);
      if (st != -1 && (WIFEXITED(st) || WIFSIGNALED(st)))
        break;
      g_usleep(10 * 1000);
    }

    /* 仍未退出：强制结束（比 SIGKILL 更靠谱，glib 自己处理） */
    {
      int st2 = g_subprocess_get_status(h->proc);
      if (!(st2 != -1 && (WIFEXITED(st2) || WIFSIGNALED(st2)))) {
        g_subprocess_force_exit(h->proc);
      }
    }

    /* wait 一下，避免僵尸 */
    g_subprocess_wait(h->proc, NULL, NULL);
  }

  if (h->din)
    g_object_unref(h->din);
  if (h->out)
    g_object_unref(h->out);
  if (h->proc)
    g_object_unref(h->proc);

  g_free(h);
}

/* 说明：不要在这里用 g_subprocess_get_status 做太多假设；
 * 我们只用它做轻量判断，且确保不会对已释放对象调用（UAF 已修复）。
 */
static gboolean helper_instance_is_alive(HelperInstance *h) {
  if (!h || !h->proc)
    return FALSE;
  int st = g_subprocess_get_status(h->proc);
  if (st == -1)
    return TRUE;
  return !(WIFEXITED(st) || WIFSIGNALED(st));
}

static HelperInstance *helper_instance_spawn(const gchar *helper_path) {
  GError *err = NULL;
  const gchar *argv[] = {helper_path, NULL};

  GSubprocess *p = g_subprocess_newv(argv,
                                     G_SUBPROCESS_FLAGS_STDIN_PIPE |
                                         G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                         G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                     &err);
  if (!p) {
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

static void helper_pool_init(HelperPool *p, gint capacity) {
  g_mutex_init(&p->m);
  g_cond_init(&p->c);
  p->free_list = g_queue_new();
  p->total_created = 0;
  p->capacity = capacity;
  p->inited = TRUE;
  p->helper_path = resolve_helper_path();

  /* last_used_us 初始化为 now，避免刚启动就被 GC 误判为空闲 */
  p->last_used_us = g_get_monotonic_time();
}

static void helper_pool_wake_all(HelperPool *p) {
  if (!p || !p->inited)
    return;
  g_mutex_lock(&p->m);
  g_cond_broadcast(&p->c);
  g_mutex_unlock(&p->m);
}

static gboolean helper_idle_timer_start_main(gpointer data);

static void helper_pool_clear(HelperPool *p) {
  if (!p || !p->inited)
    return;

  g_mutex_lock(&p->m);
  while (!g_queue_is_empty(p->free_list)) {
    HelperInstance *h = g_queue_pop_head(p->free_list);
    helper_instance_destroy(h);
  }
  g_queue_free(p->free_list);
  p->free_list = NULL;
  p->total_created = 0;
  p->capacity = 0;
  p->inited = FALSE;
  p->last_used_us = 0;
  g_mutex_unlock(&p->m);

  g_free(p->helper_path);

  g_cond_clear(&p->c);
  g_mutex_clear(&p->m);
}

/* acquire/release 不再更新 last_used_us！
 * last_used_us 只在“一次请求真正结束后”更新，避免被 Nautilus 周期性触发刷掉。
 */
static HelperInstance *helper_pool_acquire(HelperPool *p) {
  if (!p || !p->inited)
    return NULL;

  g_mutex_lock(&p->m);
  while (!g_atomic_int_get(&shutting_down)) {
    if (!g_queue_is_empty(p->free_list)) {
      HelperInstance *h = g_queue_pop_head(p->free_list);
      g_mutex_unlock(&p->m);
      return h;
    }

    if (p->total_created < p->capacity) {
      p->total_created++;
      gchar *hp = g_strdup(p->helper_path);
      g_mutex_unlock(&p->m);

      HelperInstance *h = helper_instance_spawn(hp);
      g_free(hp);

      if (!h) {
        g_mutex_lock(&p->m);
        p->total_created--;
        g_cond_signal(&p->c);
        g_mutex_unlock(&p->m);
        return NULL;
      }
      return h;
    }

    g_cond_wait(&p->c, &p->m);
  }
  g_mutex_unlock(&p->m);
  return NULL;
}

static void helper_pool_release(HelperPool *p, HelperInstance *h) {
  if (!p || !p->inited || !h)
    return;

  g_mutex_lock(&p->m);
  g_queue_push_tail(p->free_list, h);
  g_cond_signal(&p->c);
  g_mutex_unlock(&p->m);

  if (helper_idle_sec > 0 && !g_atomic_int_get(&shutting_down))
    g_main_context_invoke(NULL, helper_idle_timer_start_main, NULL);
}

static void helper_pool_mark_used_now(HelperPool *p) {
  if (!p || !p->inited)
    return;
  g_mutex_lock(&p->m);
  p->last_used_us = g_get_monotonic_time();
  g_mutex_unlock(&p->m);
}

typedef struct {
  guint32 seconds;
  gboolean timed_out;
} DurationResult;

static DurationResult get_duration_seconds_via_helper(const gchar *path);

/* One request to helper. Protocol:
 *   REQ <timeout_ms> <retry_timeout_ms> <b64(path)>\n
 *   -> OK <seconds> <flags>\n
 * flags: include 'T' if timed out.
 */
static DurationResult helper_request_duration(HelperInstance *h,
                                              const gchar *path) {
  DurationResult r = {0, FALSE};
  if (!h || !path)
    return r;

  gchar *b64 = g_base64_encode((const guchar *)path, (gsize)strlen(path));
  gchar *line = g_strdup_printf("REQ %u %u %s\n", helper_timeout_ms,
                                helper_retry_timeout_ms, b64);
  g_free(b64);

  GError *err = NULL;
  gsize written = 0;
  gboolean ok = g_output_stream_write_all(h->out, line, strlen(line), &written,
                                          NULL, &err);
  g_free(line);

  if (!ok) {
    g_clear_error(&err);
    return r;
  }
  ok = g_output_stream_flush(h->out, NULL, &err);
  if (!ok) {
    g_clear_error(&err);
    return r;
  }

  gsize len = 0;
  gchar *resp = g_data_input_stream_read_line(h->din, &len, NULL, &err);
  if (!resp) {
    g_clear_error(&err);
    return r;
  }

  if (g_str_has_prefix(resp, "OK")) {
    gchar **parts = g_strsplit(resp, " ", 0);
    if (parts && parts[1]) {
      guint64 v = g_ascii_strtoull(parts[1], NULL, 10);
      if (v > G_MAXUINT32)
        v = G_MAXUINT32;
      r.seconds = (guint32)v;
    }
    if (parts && parts[2]) {
      if (strchr(parts[2], 'T'))
        r.timed_out = TRUE;
    }
    g_strfreev(parts);
  }

  g_free(resp);
  return r;
}

/* 修复 UAF：传入 HelperInstance**，销毁时置 NULL，返回是否仍可用 */
static gboolean helper_instance_maybe_restart(HelperPool *p,
                                              HelperInstance **ph) {
  HelperInstance *h = (ph ? *ph : NULL);
  if (!h)
    return FALSE;

  if (!helper_instance_is_alive(h)) {
    helper_instance_destroy(h);
    g_mutex_lock(&p->m);
    p->total_created--;
    g_cond_signal(&p->c);
    g_mutex_unlock(&p->m);
    *ph = NULL;
    return FALSE;
  }

  h->req_count++;
  if (h->req_count >= helper_reqs_before_restart) {
    GError *err = NULL;
    const gchar *q = "QUIT\n";
    g_output_stream_write_all(h->out, q, strlen(q), NULL, NULL, &err);
    g_clear_error(&err);
    g_output_stream_flush(h->out, NULL, NULL);

    helper_instance_destroy(h);
    g_mutex_lock(&p->m);
    p->total_created--;
    g_cond_signal(&p->c);
    g_mutex_unlock(&p->m);
    *ph = NULL;
    return FALSE;
  }

  return TRUE;
}

/* ------------------- Embedded FFmpeg Probing (No IPC) ------------------- */

/* Interrupt callback context for timeout control */
typedef struct {
  int64_t deadline_us;
} VdInterruptContext;

static int vd_interrupt_cb(void *opaque) {
  VdInterruptContext *ctx = (VdInterruptContext *)opaque;
  if (!ctx)
    return 0;
  return (av_gettime_relative() > ctx->deadline_us) ? 1 : 0;
}

/* Thread-local signal handling for crash protection */
static __thread sigjmp_buf probe_jmpbuf;
static __thread volatile sig_atomic_t in_probe = 0;

/* Saved original signal handlers */
static struct sigaction orig_sigsegv;
static struct sigaction orig_sigfpe;
static struct sigaction orig_sigbus;
static volatile sig_atomic_t probe_signal_handlers_installed = 0;

static const struct sigaction *probe_original_action_for_signal(int sig) {
  switch (sig) {
  case SIGSEGV:
    return &orig_sigsegv;
  case SIGFPE:
    return &orig_sigfpe;
  case SIGBUS:
    return &orig_sigbus;
  default:
    return NULL;
  }
}

static void probe_forward_signal(int sig, siginfo_t *info, void *ucontext) {
  const struct sigaction *orig = probe_original_action_for_signal(sig);
  if (!orig) {
    signal(sig, SIG_DFL);
    raise(sig);
    return;
  }

  if (orig->sa_flags & SA_SIGINFO) {
    if (orig->sa_sigaction)
      orig->sa_sigaction(sig, info, ucontext);
    return;
  }

  if (orig->sa_handler == SIG_IGN)
    return;

  if (orig->sa_handler == SIG_DFL) {
    sigaction(sig, orig, NULL);
    raise(sig);
    return;
  }

  orig->sa_handler(sig);
}

static void probe_signal_handler(int sig, siginfo_t *info, void *ucontext) {
  if (in_probe) {
    siglongjmp(probe_jmpbuf, 1);
  }

  probe_forward_signal(sig, info, ucontext);
}

static gboolean probe_signal_handlers_install(void) {
  if (probe_signal_handlers_installed)
    return TRUE;

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = probe_signal_handler;
  sigemptyset(&sa.sa_mask);
  sigaddset(&sa.sa_mask, SIGSEGV);
  sigaddset(&sa.sa_mask, SIGFPE);
  sigaddset(&sa.sa_mask, SIGBUS);
  sa.sa_flags = SA_SIGINFO;

  if (sigaction(SIGSEGV, &sa, &orig_sigsegv) != 0)
    return FALSE;
  if (sigaction(SIGFPE, &sa, &orig_sigfpe) != 0) {
    sigaction(SIGSEGV, &orig_sigsegv, NULL);
    return FALSE;
  }
  if (sigaction(SIGBUS, &sa, &orig_sigbus) != 0) {
    sigaction(SIGFPE, &orig_sigfpe, NULL);
    sigaction(SIGSEGV, &orig_sigsegv, NULL);
    return FALSE;
  }

  probe_signal_handlers_installed = 1;
  return TRUE;
}

static void probe_signal_handler_restore_one(int sig,
                                             const struct sigaction *orig) {
  struct sigaction cur;
  memset(&cur, 0, sizeof(cur));
  if (sigaction(sig, NULL, &cur) != 0)
    return;

  if ((cur.sa_flags & SA_SIGINFO) &&
      cur.sa_sigaction == probe_signal_handler) {
    sigaction(sig, orig, NULL);
  }
}

static void probe_signal_handlers_restore(void) {
  if (!probe_signal_handlers_installed)
    return;

  probe_signal_handler_restore_one(SIGSEGV, &orig_sigsegv);
  probe_signal_handler_restore_one(SIGFPE, &orig_sigfpe);
  probe_signal_handler_restore_one(SIGBUS, &orig_sigbus);
  probe_signal_handlers_installed = 0;
}

/* Direct probe without IPC - eliminates ~90% overhead */
static guint32 probe_duration_direct(const gchar *path, guint timeout_ms) {
  if (!path || timeout_ms == 0)
    return 0;

  AVFormatContext *fmt = avformat_alloc_context();
  if (!fmt)
    return 0;

  VdInterruptContext ictx;
  ictx.deadline_us = av_gettime_relative() + (int64_t)timeout_ms * 1000;

  fmt->interrupt_callback.callback = vd_interrupt_cb;
  fmt->interrupt_callback.opaque = &ictx;

  AVDictionary *opts = NULL;
  av_dict_set(&opts, "probesize", "131072", 0);      /* 128K fast probe */
  av_dict_set(&opts, "analyzeduration", "0", 0);
  av_dict_set(&opts, "max_delay", "0", 0);

  int ret = avformat_open_input(&fmt, path, NULL, &opts);
  av_dict_free(&opts);

  if (ret < 0) {
    return 0;
  }

  guint32 sec = 0;

  /* Try container duration first */
  if (fmt->duration > 0 && fmt->duration != AV_NOPTS_VALUE) {
    double dsec = (double)fmt->duration / (double)AV_TIME_BASE;
    sec = (guint32)(dsec + 0.5);
  }

  /* Fallback: try stream durations */
  if (sec == 0) {
    double best = 0.0;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
      AVStream *st = fmt->streams[i];
      if (st && st->duration > 0 && st->duration != AV_NOPTS_VALUE) {
        double s = (double)st->duration * av_q2d(st->time_base);
        if (s > best)
          best = s;
      }
    }
    if (best > 0)
      sec = (guint32)(best + 0.5);
  }

  /* If still no duration, try finding stream info (pass 1) */
  if (sec == 0 && !vd_interrupt_cb(&ictx)) {
    AVDictionary *si_opts = NULL;
    av_dict_set(&si_opts, "probesize", "2097152", 0);      /* 2MB */
    av_dict_set(&si_opts, "analyzeduration", "1000000", 0); /* 1s */

    ret = avformat_find_stream_info(fmt, &si_opts);
    av_dict_free(&si_opts);

    if (ret >= 0) {
      if (fmt->duration > 0 && fmt->duration != AV_NOPTS_VALUE) {
        double dsec = (double)fmt->duration / (double)AV_TIME_BASE;
        sec = (guint32)(dsec + 0.5);
      }
      if (sec == 0) {
        double best = 0.0;
        for (unsigned i = 0; i < fmt->nb_streams; i++) {
          AVStream *st = fmt->streams[i];
          if (st && st->duration > 0 && st->duration != AV_NOPTS_VALUE) {
            double s = (double)st->duration * av_q2d(st->time_base);
            if (s > best)
              best = s;
          }
        }
        if (best > 0)
          sec = (guint32)(best + 0.5);
      }
    }
  }

  avformat_close_input(&fmt);
  return sec;
}

/* Safe wrapper with signal handling to prevent FFmpeg crashes from taking down Nautilus */
static DurationResult get_duration_embedded(const gchar *path) {
  DurationResult r = {0, FALSE};
  if (!path)
    return r;

  if (!probe_signal_handlers_installed)
    return get_duration_seconds_via_helper(path);

  if (sigsetjmp(probe_jmpbuf, 1) == 0) {
    /* Normal execution path */
    in_probe = 1;
    r.seconds = probe_duration_direct(path, helper_timeout_ms);

    /* If first attempt failed, retry with longer timeout */
    if (r.seconds == 0) {
      r.seconds = probe_duration_direct(path, helper_retry_timeout_ms);
      if (r.seconds == 0) {
        r.timed_out = TRUE;
      }
    }
  } else {
    /* Signal was caught - FFmpeg crashed on this file */
    r.seconds = 0;
    r.timed_out = FALSE;
  }

  in_probe = 0;

  return r;
}

/* Legacy helper-based implementation (kept for fallback) */
static DurationResult get_duration_seconds_via_helper(const gchar *path) {
  DurationResult r = {0, FALSE};
  if (!path)
    return r;

  HelperInstance *h = helper_pool_acquire(&helper_pool);
  if (!h)
    return r;

  r = helper_request_duration(h, path);

  /* 关键：一次请求结束才刷新 last_used_us，用于 idle GC 判定 */
  helper_pool_mark_used_now(&helper_pool);

  if (helper_instance_maybe_restart(&helper_pool, &h)) {
    helper_pool_release(&helper_pool, h);
  }
  return r;
}

/* helper 空闲回收：只清 free_list 中空闲实例（busy 的在 worker 手里） */
static gboolean helper_pool_idle_gc_cb(gpointer data) {
  HelperPool *p = (HelperPool *)data;
  if (!p || !p->inited) {
    helper_idle_source_id = 0;
    return G_SOURCE_REMOVE;
  }

  if (g_atomic_int_get(&shutting_down)) {
    helper_idle_source_id = 0;
    return G_SOURCE_REMOVE;
  }

  if (helper_idle_sec == 0) {
    helper_idle_source_id = 0;
    return G_SOURCE_REMOVE;
  }

  gint64 now = g_get_monotonic_time();
  gboolean remove_timer = FALSE;

  g_mutex_lock(&p->m);
  gint64 last = p->last_used_us;
  guint free_n = p->free_list ? (guint)g_queue_get_length(p->free_list) : 0;

  if (free_n == 0) {
    remove_timer = TRUE;
  } else if (last != 0 &&
      (now - last) > (gint64)helper_idle_sec * (gint64)G_USEC_PER_SEC) {

    guint reaped = 0;
    while (!g_queue_is_empty(p->free_list)) {
      HelperInstance *h = g_queue_pop_head(p->free_list);
      helper_instance_destroy(h);
      p->total_created--;
      reaped++;
    }

    g_cond_broadcast(&p->c);
    p->last_used_us = now;
    remove_timer = TRUE;

    if (reaped > 0) {
      g_message("VD: helper_idle_gc reap=%u now_total=%d", reaped,
                p->total_created);
    }
  }
  g_mutex_unlock(&p->m);

  if (remove_timer) {
    helper_idle_source_id = 0;
    return G_SOURCE_REMOVE;
  }

  return G_SOURCE_CONTINUE;
}

/* 确保 idle timer 在主线程默认 MainContext 启动 */
static gboolean helper_idle_timer_start_main(gpointer data) {
  (void)data;
  if (helper_idle_source_id == 0 && helper_idle_sec > 0 &&
      !g_atomic_int_get(&shutting_down)) {
    helper_idle_source_id =
        g_timeout_add_seconds(5, helper_pool_idle_gc_cb, &helper_pool);
  }
  return G_SOURCE_REMOVE;
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

static void worker_process(Job *job) {
  if (!job)
    return;
  if (g_atomic_int_get(&shutting_down)) {
    job_free(job);
    return;
  }

  /* Phase 2.1: Use embedded FFmpeg probe (direct API call, no IPC overhead) */
  DurationResult dr = get_duration_embedded(job->path);
  guint32 seconds = dr.seconds;
  gboolean negative = (seconds == 0);
  gboolean timeout_fail = (seconds == 0 && dr.timed_out);

  gboolean audio = job->audio;

  gint64 now_us = g_get_monotonic_time();

  /* Phase 2.2: Use write lock for cache modification */
  g_rw_lock_writer_lock(&cache_rwlock);

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
    while (!g_atomic_int_get(&shutting_down) && g_queue_is_empty(pending))
      g_cond_wait(&pending_cond, &pending_lock);

    if (g_atomic_int_get(&shutting_down)) {
      g_mutex_unlock(&pending_lock);
      break;
    }

    Job *job = g_queue_pop_head(pending);
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

  gchar *name = nautilus_file_info_get_name(file);
  if (!is_media_file(name)) {
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
  gboolean audio = is_audio_ext(path);

  g_rw_lock_writer_lock(&cache_rwlock);

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
    guint64 *keyp = g_new(guint64, 1);
    *keyp = key;

    e = g_new0(CacheEntry, 1);
    e->keyp = keyp;
    e->mtime = mtime;
    e->size = size;
    e->seconds = 0;
    e->flags = 0;
    e->negative_until_us = 0;
    e->last_request_us = 0;
    e->lru_link = NULL;

    g_hash_table_insert(entries, keyp, e);
    lru_touch_unlocked(e);
    evict_if_needed_unlocked();
    is_new = TRUE;
  }

  /* debounce */
  if (!is_new && debounce_ms > 0 && e->last_request_us) {
    gint64 dt = now_us - e->last_request_us;
    if (dt < (gint64)debounce_ms * 1000) {
      if (e->flags & FLAG_HAS_VALUE) {
        gchar *dur = format_seconds(e->seconds, audio);
        nautilus_file_info_add_string_attribute(file, ATTR_KEY, dur);
        g_free(dur);
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
    gchar *dur = format_seconds(e->seconds, audio);
    nautilus_file_info_add_string_attribute(file, ATTR_KEY, dur);
    g_free(dur);
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
    if (pending && g_queue_get_length(pending) >= queue_max) {
      dropped = g_queue_pop_head(pending);
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
      }
    }

    g_queue_push_tail(pending, job);
    g_cond_signal(&pending_cond);

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

  /* Phase 2.1: Initialize FFmpeg for embedded probing */
  av_log_set_level(AV_LOG_QUIET);
  avformat_network_init();
  if (!probe_signal_handlers_install()) {
    g_warning("VD: failed to install embedded probe signal handlers; "
              "falling back to helper process");
  }

  g_atomic_int_set(&shutting_down, 0);
  init_limits_from_env();

  nautilus_list_view_settings = g_settings_new(NAUTILUS_LIST_VIEW_SCHEMA);
  refresh_duration_column_enabled();
  g_signal_connect(nautilus_list_view_settings, "changed::" NAUTILUS_VISIBLE_COLUMNS_KEY,
                   G_CALLBACK(on_visible_columns_changed), NULL);

  /* Phase 2.2: Initialize separate locks */
  g_rw_lock_init(&cache_rwlock);
  g_mutex_init(&pending_lock);
  g_cond_init(&pending_cond);

  entries =
      g_hash_table_new_full(key_u64_hash, key_u64_equal, g_free, entry_destroy);
  lru = g_queue_new();
  pending = g_queue_new();

  helper_pool_init(&helper_pool, (gint)helper_max);

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
            helper_pool.helper_path, g_atomic_int_get(&duration_column_enabled));

  workers = g_new0(GThread *, worker_count);
  for (gint i = 0; i < worker_count; i++)
    workers[i] = g_thread_new("vd-worker", worker_thread, NULL);
}

void nautilus_module_shutdown(void) {
  g_atomic_int_set(&shutting_down, 1);

  /* stop idle timer */
  if (helper_idle_source_id) {
    g_source_remove(helper_idle_source_id);
    helper_idle_source_id = 0;
  }

  /* wake pending workers */
  g_mutex_lock(&pending_lock);
  g_cond_broadcast(&pending_cond);
  g_mutex_unlock(&pending_lock);

  /* wake helper pool waiters */
  helper_pool_wake_all(&helper_pool);

  if (workers) {
    for (gint i = 0; i < worker_count; i++)
      if (workers[i])
        g_thread_join(workers[i]);
    g_free(workers);
    workers = NULL;
    worker_count = 0;
  }

  g_rw_lock_writer_lock(&cache_rwlock);

  if (pending) {
    while (!g_queue_is_empty(pending)) {
      Job *job = g_queue_pop_head(pending);
      if (job)
        job_free(job);
    }
    g_queue_free(pending);
    pending = NULL;
  }

  if (lru) {
    g_queue_free(lru);
    lru = NULL;
  }
  if (entries) {
    g_hash_table_destroy(entries);
    entries = NULL;
  }

  g_rw_lock_writer_unlock(&cache_rwlock);

  helper_pool_clear(&helper_pool);
  probe_signal_handlers_restore();

  if (nautilus_list_view_settings) {
    g_signal_handlers_disconnect_by_func(nautilus_list_view_settings,
                                         G_CALLBACK(on_visible_columns_changed),
                                         NULL);
    g_clear_object(&nautilus_list_view_settings);
  }

  /* Phase 2.1: Cleanup FFmpeg */
  avformat_network_deinit();

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
