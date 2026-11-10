#pragma once

#include <glib.h>

typedef struct VdCache VdCache;
typedef struct VdCacheEntry VdCacheEntry;

typedef struct {
  guint max_entries;
  guint evict_ratio_percent;
  guint debounce_ms;
  guint negative_ttl_sec;
  guint timeout_ttl_sec;
  guint drop_ttl_ms;
} VdCacheConfig;

typedef enum {
  VD_CACHE_HIT,
  VD_CACHE_NEGATIVE,
  VD_CACHE_BUSY,
  VD_CACHE_RESERVED,
} VdCacheDecision;

VdCache *vd_cache_new(const VdCacheConfig *config);
void vd_cache_free(VdCache *cache);

/* Scheduler serializes cache operations with queue transitions under one lock.
 * A reservation pins the entry and its immutable path until finish or cancel.
 */
VdCacheDecision vd_cache_reserve(VdCache *cache, const gchar *path,
                                 guint64 mtime, guint64 size, gint64 now_us,
                                 guint32 *seconds, VdCacheEntry **reservation);
const gchar *vd_cache_entry_path(const VdCacheEntry *reservation);
void vd_cache_finish(VdCache *cache, VdCacheEntry *reservation, guint32 seconds,
                     gboolean timed_out, gint64 now_us);
void vd_cache_cancel(VdCache *cache, VdCacheEntry *reservation);
void vd_cache_drop(VdCache *cache, VdCacheEntry *reservation, gint64 now_us);
