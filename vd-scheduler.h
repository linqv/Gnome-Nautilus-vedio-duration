#pragma once

#include "vd-cache.h"
#include "vd-helper-pool.h"
#include "vd-media.h"
#include <glib-object.h>

typedef struct VdScheduler VdScheduler;
typedef void (*VdDurationUpdate)(GObject *target, MediaKind kind,
                                 guint32 seconds);

typedef struct {
  VdCacheConfig cache;
  VdHelperConfig helper;
  guint queue_max;
  guint workers;
} VdSchedulerConfig;

/* Call from the default main context. Updates (including synchronous cache
 * hits) run there. A queued request holds a reference to its target. */
VdScheduler *vd_scheduler_new(const VdSchedulerConfig *config,
                              VdDurationUpdate update);
void vd_scheduler_set_enabled(VdScheduler *s, gboolean enabled);
void vd_scheduler_submit(VdScheduler *s, const gchar *path, guint64 mtime,
                         guint64 size, MediaKind kind, GObject *target);
void vd_scheduler_free(VdScheduler *s);
