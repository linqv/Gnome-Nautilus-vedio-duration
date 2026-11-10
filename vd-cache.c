#include "vd-cache.h"
#include <string.h>

typedef struct {
  const gchar *path;
  guint64 mtime;
  guint64 size;
} FileKey;

struct VdCacheEntry {
  FileKey key;
  GList link;
  guint32 seconds;
  gint64 negative_until_us;
  gboolean reserved;
  gchar path_storage[];
};

struct VdCache {
  VdCacheConfig config;
  GHashTable *entries;
  GQueue lru; /* Only completed entries are eligible for eviction. */
  VdCacheEntry *last_lookup;
  gint64 last_lookup_us;
};

static guint file_hash(gconstpointer data) {
  const FileKey *k = data;
  return g_str_hash(k->path) ^ g_int64_hash(&k->mtime) ^ g_int64_hash(&k->size);
}

static gboolean same_file(gconstpointer a, gconstpointer b) {
  const FileKey *x = a, *y = b;
  return x->mtime == y->mtime && x->size == y->size &&
         g_str_equal(x->path, y->path);
}

VdCache *vd_cache_new(const VdCacheConfig *config) {
  VdCache *cache = g_new0(VdCache, 1);
  cache->config = *config;
  cache->entries = g_hash_table_new_full(file_hash, same_file, NULL, g_free);
  return cache;
}

void vd_cache_free(VdCache *cache) {
  g_hash_table_destroy(cache->entries);
  g_free(cache);
}

static void touch(VdCache *cache, VdCacheEntry *entry) {
  if (cache->lru.tail != &entry->link) {
    g_queue_unlink(&cache->lru, &entry->link);
    g_queue_push_tail_link(&cache->lru, &entry->link);
  }
}

static void remove_entry(VdCache *cache, VdCacheEntry *entry) {
  if (cache->last_lookup == entry)
    cache->last_lookup = NULL;
  if (!entry->reserved)
    g_queue_unlink(&cache->lru, &entry->link);
  g_hash_table_remove(cache->entries, &entry->key);
}

static void evict(VdCache *cache) {
  if (g_hash_table_size(cache->entries) <= cache->config.max_entries)
    return;
  guint target = MAX(1u, cache->config.max_entries *
                             cache->config.evict_ratio_percent / 100);
  while (g_hash_table_size(cache->entries) > target &&
         !g_queue_is_empty(&cache->lru))
    remove_entry(cache, g_queue_peek_head(&cache->lru));
}

VdCacheDecision vd_cache_reserve(VdCache *cache, const gchar *path,
                                 guint64 mtime, guint64 size, gint64 now_us,
                                 guint32 *seconds, VdCacheEntry **reservation) {
  FileKey key = {path, mtime, size};
  VdCacheEntry *entry = cache->last_lookup;
  *reservation = NULL;
  *seconds = 0;

  /* Repeated provider requests can reuse the lookup within the debounce
   * window. Negative expiry is still checked on every request. */
  if (!entry ||
      now_us - cache->last_lookup_us >=
          (gint64)cache->config.debounce_ms * 1000 ||
      !same_file(&entry->key, &key)) {
    entry = g_hash_table_lookup(cache->entries, &key);
    if (!entry) {
      gsize path_size = strlen(path) + 1;
      entry = g_malloc0(sizeof *entry + path_size);
      entry->key = key;
      memcpy(entry->path_storage, path, path_size);
      entry->key.path = entry->path_storage;
      entry->link.data = entry;
      g_queue_push_tail_link(&cache->lru, &entry->link);
      g_hash_table_insert(cache->entries, &entry->key, entry);
    }
    cache->last_lookup = entry;
    cache->last_lookup_us = now_us;
  }

  if (entry->reserved)
    return VD_CACHE_BUSY;
  if (entry->seconds) {
    touch(cache, entry);
    *seconds = entry->seconds;
    return VD_CACHE_HIT;
  }
  if (now_us < entry->negative_until_us) {
    touch(cache, entry);
    return VD_CACHE_NEGATIVE;
  }
  g_queue_unlink(&cache->lru, &entry->link);

  entry->reserved = TRUE;
  *reservation = entry;
  evict(cache);
  return VD_CACHE_RESERVED;
}

const gchar *vd_cache_entry_path(const VdCacheEntry *entry) {
  return entry->key.path;
}

void vd_cache_finish(VdCache *cache, VdCacheEntry *entry, guint32 seconds,
                     gboolean timed_out, gint64 now_us) {
  guint ttl = timed_out ? cache->config.timeout_ttl_sec
                        : cache->config.negative_ttl_sec;
  entry->reserved = FALSE;
  entry->seconds = seconds;
  entry->negative_until_us =
      seconds ? 0 : now_us + (gint64)ttl * G_USEC_PER_SEC;
  g_queue_push_tail_link(&cache->lru, &entry->link);
  evict(cache);
}

void vd_cache_cancel(VdCache *cache, VdCacheEntry *entry) {
  remove_entry(cache, entry);
}

void vd_cache_drop(VdCache *cache, VdCacheEntry *entry, gint64 now_us) {
  if (!cache->config.drop_ttl_ms) {
    vd_cache_cancel(cache, entry);
    return;
  }
  entry->reserved = FALSE;
  entry->negative_until_us = now_us + (gint64)cache->config.drop_ttl_ms * 1000;
  g_queue_push_tail_link(&cache->lru, &entry->link);
  evict(cache);
}
