/* Hierarchical, thread-safe compiler progress reporting. */
#ifndef NY_PROGRESS_H
#define NY_PROGRESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ny_progress_node ny_progress_node_t;

/* Create or return the global root progress scope for a compilation/build.
 * `estimated_total` is the expected number of direct work items when known,
 * otherwise 0 for an indeterminate scope. */
ny_progress_node_t *ny_progress_start(const char *name, size_t estimated_total);

/* Start a child scope under `parent` (or the root when parent is NULL).
 * `estimated_total` is 0 when unknown/indeterminate. */
ny_progress_node_t *ny_progress_begin(ny_progress_node_t *parent,
                                      const char *name,
                                      size_t estimated_total);

/* Update progress metadata. Safe to call from any thread. */
void ny_progress_set_name(ny_progress_node_t *node, const char *name);
void ny_progress_set_total(ny_progress_node_t *node, size_t estimated_total);
void ny_progress_set_completed(ny_progress_node_t *node, size_t completed);
void ny_progress_add(ny_progress_node_t *node, size_t amount);

/* Mark a scope complete or failed and detach it from active rendering. */
void ny_progress_end(ny_progress_node_t *node);
void ny_progress_fail(ny_progress_node_t *node, const char *status);

/* Finish global progress, stop rendering, restore terminal state, and emit a
 * final summary when appropriate. Safe to call multiple times. */
void ny_progress_finish(void);

/* Environment/request helpers. */
bool ny_progress_enabled_from_env(void);
void ny_progress_force(void);

/* Coordinate stderr output with the renderer. While locked, progress display is
 * cleared/paused so diagnostics and other stderr output do not interleave. */
void ny_progress_stderr_lock(void);
void ny_progress_stderr_unlock(void);

/* Compatibility wrappers for legacy call sites. */
typedef ny_progress_node_t *ny_progress_task_t;
static inline ny_progress_task_t ny_progress_task_begin(const char *name,
                                                        long total) {
  return ny_progress_begin(NULL, name, total > 0 ? (size_t)total : 0);
}
static inline void ny_progress_task_update(ny_progress_task_t node,
                                           long completed) {
  if (node && completed >= 0)
    ny_progress_set_completed(node, (size_t)completed);
}
static inline void ny_progress_task_end(ny_progress_task_t node) {
  ny_progress_end(node);
}

#endif
