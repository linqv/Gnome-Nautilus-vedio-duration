#include "vd-cache.h"

static VdCache *new_cache(guint capacity) {
  VdCacheConfig config = {.max_entries = capacity,
                          .evict_ratio_percent = 95,
                          .debounce_ms = 250,
                          .negative_ttl_sec = 10,
                          .timeout_ttl_sec = 1,
                          .drop_ttl_ms = 20};
  return vd_cache_new(&config);
}

static VdCacheEntry *reserve(VdCache *c, const gchar *path, gint64 now) {
  VdCacheEntry *entry;
  guint32 seconds;
  g_assert_cmpint(vd_cache_reserve(c, path, 1, 2, now, &seconds, &entry), ==,
                  VD_CACHE_RESERVED);
  g_assert_cmpstr(vd_cache_entry_path(entry), ==, path);
  return entry;
}

static void expect(VdCache *c, const gchar *path, gint64 now,
                   VdCacheDecision wanted, guint32 value) {
  VdCacheEntry *entry;
  guint32 seconds;
  g_assert_cmpint(vd_cache_reserve(c, path, 1, 2, now, &seconds, &entry), ==,
                  wanted);
  g_assert_cmpuint(seconds, ==, value);
  g_assert_null(entry);
}

static void expiry(void) {
  VdCache *c = new_cache(8);
  VdCacheEntry *entry = reserve(c, "a", 0);
  expect(c, "a", 0, VD_CACHE_BUSY, 0);
  vd_cache_finish(c, entry, 0, FALSE, 0);
  expect(c, "a", 9999999, VD_CACHE_NEGATIVE, 0);
  entry = reserve(c, "a", 10000000);
  vd_cache_finish(c, entry, 0, TRUE, 10000000);
  expect(c, "a", 10999999, VD_CACHE_NEGATIVE, 0);
  entry = reserve(c, "a", 11000000);
  vd_cache_drop(c, entry, 11000000);
  expect(c, "a", 11019999, VD_CACHE_NEGATIVE, 0);
  vd_cache_cancel(c, reserve(c, "a", 11020000));
  vd_cache_finish(c, reserve(c, "a", 11020000), 7, FALSE, 11020000);
  expect(c, "a", 11020001, VD_CACHE_HIT, 7);
  vd_cache_free(c);
}

static void identity(void) {
  VdCache *c = new_cache(8);
  vd_cache_finish(c, reserve(c, "Aa", 0), 10, FALSE, 0);
  g_assert_cmpuint(g_str_hash("Aa"), ==, g_str_hash("B@"));
  vd_cache_finish(c, reserve(c, "B@", 0), 20, FALSE, 0);
  expect(c, "Aa", 1, VD_CACHE_HIT, 10);
  expect(c, "B@", 1, VD_CACHE_HIT, 20);

  VdCacheEntry *entry;
  guint32 seconds;
  g_assert_cmpint(vd_cache_reserve(c, "Aa", 2, 2, 1, &seconds, &entry), ==,
                  VD_CACHE_RESERVED);
  vd_cache_cancel(c, entry);
  g_assert_cmpint(vd_cache_reserve(c, "Aa", 1, 3, 1, &seconds, &entry), ==,
                  VD_CACHE_RESERVED);
  vd_cache_cancel(c, entry);
  vd_cache_free(c);
}

static void eviction(void) {
  VdCache *c = new_cache(3);
  vd_cache_finish(c, reserve(c, "a", 0), 1, FALSE, 0);
  vd_cache_finish(c, reserve(c, "b", 0), 2, FALSE, 0);
  vd_cache_finish(c, reserve(c, "c", 0), 3, FALSE, 0);
  expect(c, "a", 1, VD_CACHE_HIT, 1);
  VdCacheEntry *d = reserve(c, "d", 2);
  expect(c, "a", 3, VD_CACHE_HIT, 1);
  VdCacheEntry *b = reserve(c, "b", 3);
  VdCacheEntry *e = reserve(c, "e", 4);
  VdCacheEntry *f = reserve(c, "f", 5);
  expect(c, "d", 5, VD_CACHE_BUSY, 0);
  vd_cache_cancel(c, b);
  vd_cache_cancel(c, e);
  vd_cache_cancel(c, f);
  vd_cache_finish(c, d, 4, FALSE, 6);
  expect(c, "d", 7, VD_CACHE_HIT, 4);
  vd_cache_free(c);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/cache/expiry", expiry);
  g_test_add_func("/cache/file-identity", identity);
  g_test_add_func("/cache/eviction", eviction);
  return g_test_run();
}
