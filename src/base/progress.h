#ifndef NY_PROGRESS_H
#define NY_PROGRESS_H

#include <stdbool.h>

typedef struct ny_progress_node_t {
  int id;
} ny_progress_node_t;

bool ny_progress_enabled_from_env(void);
void ny_progress_force(void);
void ny_progress_start(const char *name, long total);
ny_progress_node_t ny_progress_task_begin(const char *name, long total);
void ny_progress_task_update(ny_progress_node_t node, long completed);
void ny_progress_task_end(ny_progress_node_t node);
void ny_progress_finish(void);
void ny_progress_stderr_lock(void);
void ny_progress_stderr_unlock(void);

#endif
