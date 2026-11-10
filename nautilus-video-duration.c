#include "vd-scheduler.h"

#include <gio/gio.h>
#include <nautilus-extension.h>
#include <sys/stat.h>

#define VD_BUILD_TAG "VD_BUILD_TAG: helper-only-mka-2026-05-03"

#define COLUMN_ID "video-duration::duration"
#define ATTR_KEY "video-duration::duration"
#define NAUTILUS_LIST_VIEW_SCHEMA "org.gnome.nautilus.list-view"
#define NAUTILUS_VISIBLE_COLUMNS_KEY "default-visible-columns"

#ifndef VD_HELPER_PATH
#define VD_HELPER_PATH "/usr/lib/nautilus/extensions-4/vd-ffmpeg-helper"
#endif

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

static VdScheduler *scheduler;
static GSettings *nautilus_list_view_settings;
static gboolean duration_column_enabled;

static gboolean visible_columns_contain_duration(GSettings *settings) {
  if (!settings)
    return TRUE;

  gboolean enabled = FALSE;
  gchar **columns = g_settings_get_strv(settings, NAUTILUS_VISIBLE_COLUMNS_KEY);

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
  duration_column_enabled =
      visible_columns_contain_duration(nautilus_list_view_settings);
  vd_scheduler_set_enabled(scheduler, duration_column_enabled);
}

static void on_visible_columns_changed(GSettings *settings, gchar *key,
                                       gpointer user_data) {
  (void)settings;
  (void)key;
  (void)user_data;
  refresh_duration_column_enabled();
}

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

static VdSchedulerConfig config_from_env(void) {
  const gchar *path = g_getenv("NAUTILUS_VD_HELPER");
  VdSchedulerConfig config = {
      .queue_max = read_env_uint("NAUTILUS_VD_QUEUE_MAX", 256, 1, 200000),
      .cache =
          {
              .max_entries = read_env_uint("NAUTILUS_VD_CACHE", 512, 1, 200000),
              .evict_ratio_percent =
                  read_env_uint("NAUTILUS_VD_EVICT_RATIO", 70, 10, 95),
              .debounce_ms =
                  read_env_uint("NAUTILUS_VD_DEBOUNCE_MS", 250, 0, 5000),
              .negative_ttl_sec =
                  read_env_uint("NAUTILUS_VD_NEG_TTL", 600, 1, 86400),
              .timeout_ttl_sec =
                  read_env_uint("NAUTILUS_VD_TIMEOUT_NEG_TTL_SEC", 3, 0, 3600),
              .drop_ttl_ms =
                  read_env_uint("NAUTILUS_VD_DROP_NEG_TTL_MS", 2000, 0, 60000),
          },
      .helper =
          {
              .path = path && *path ? path : VD_HELPER_PATH,
              .capacity = read_env_uint("NAUTILUS_VD_HELPER_MAX", 2, 1, 16),
              .timeout_ms =
                  read_env_uint("NAUTILUS_VD_FF_TIMEOUT_MS", 2000, 100, 60000),
              .retry_timeout_ms = read_env_uint(
                  "NAUTILUS_VD_FF_RETRY_TIMEOUT_MS", 12000, 200, 180000),
              .requests_before_restart = read_env_uint(
                  "NAUTILUS_VD_HELPER_REQS_BEFORE_RESTART", 400, 50, 100000),
              .idle_sec =
                  read_env_uint("NAUTILUS_VD_HELPER_IDLE_SEC", 30, 0, 3600),
          },
  };
  guint cpu_limit = CLAMP(g_get_num_processors(), 1u, 8u);
  config.workers = read_env_uint(
      "NAUTILUS_VD_THREADS", MIN(config.helper.capacity, cpu_limit), 1, 128);
  return config;
}

static void apply_duration(GObject *target, MediaKind kind, guint32 seconds) {
  gchar buf[DURATION_TEXT_MAX];
  const gchar *text =
      vd_media_format_seconds(seconds, kind == MEDIA_KIND_AUDIO, buf);
  nautilus_file_info_add_string_attribute(NAUTILUS_FILE_INFO(target), ATTR_KEY,
                                          text);
}

static void schedule_duration_job(NautilusFileInfo *file) {
  if (!duration_column_enabled)
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

  vd_scheduler_submit(scheduler, path, mtime, size, media_kind, G_OBJECT(file));
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

  VdSchedulerConfig config = config_from_env();
  scheduler = vd_scheduler_new(&config, apply_duration);
  nautilus_list_view_settings = g_settings_new(NAUTILUS_LIST_VIEW_SCHEMA);
  refresh_duration_column_enabled();
  g_signal_connect(nautilus_list_view_settings,
                   "changed::" NAUTILUS_VISIBLE_COLUMNS_KEY,
                   G_CALLBACK(on_visible_columns_changed), NULL);

  g_message("VD: helperpool threads=%u helper_max=%u helper_idle_sec=%u "
            "cache_max=%u queue_max=%u debounce_ms=%u neg_ttl=%u "
            "evict_ratio=%u drop_neg_ttl_ms=%u ff_timeout=%u ff_retry=%u "
            "timeout_neg_ttl=%u helper_reqs_restart=%u helper_path=%s "
            "column_enabled=%d",
            config.workers, config.helper.capacity, config.helper.idle_sec,
            config.cache.max_entries, config.queue_max,
            config.cache.debounce_ms, config.cache.negative_ttl_sec,
            config.cache.evict_ratio_percent, config.cache.drop_ttl_ms,
            config.helper.timeout_ms, config.helper.retry_timeout_ms,
            config.cache.timeout_ttl_sec, config.helper.requests_before_restart,
            config.helper.path, duration_column_enabled);
}

void nautilus_module_shutdown(void) {
  duration_column_enabled = FALSE;
  g_signal_handlers_disconnect_by_func(nautilus_list_view_settings,
                                       G_CALLBACK(on_visible_columns_changed),
                                       NULL);
  g_clear_object(&nautilus_list_view_settings);
  g_clear_pointer(&scheduler, vd_scheduler_free);
}

void nautilus_module_list_types(const GType **types, int *num_types) {
  static GType type_list[1];
  type_list[0] = VIDEO_DURATION_TYPE;
  *types = type_list;
  *num_types = 1;
}
