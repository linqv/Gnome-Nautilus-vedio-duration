#pragma once

#include <glib.h>

typedef struct VdHelperPool VdHelperPool;

typedef struct
{
  const gchar *path;
  guint capacity;
  guint timeout_ms;
  guint retry_timeout_ms;
  guint requests_before_restart;
  guint idle_sec;
} VdHelperConfig;

typedef struct
{
  guint32 seconds;
  gboolean timed_out;
} DurationResult;

VdHelperPool *vd_helper_pool_new(const VdHelperConfig *config);
DurationResult vd_helper_pool_probe(VdHelperPool *pool, const gchar *path);
/* Call on the main thread before joining workers. Free after the join. */
void vd_helper_pool_stop(VdHelperPool *pool);
void vd_helper_pool_free(VdHelperPool *pool);
