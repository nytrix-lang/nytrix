#ifndef NYTRIX_BASE_PARALLEL_H
#define NYTRIX_BASE_PARALLEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef bool (*ny_parallel_task_fn)(size_t index, void *ctx);

typedef struct {
  uint64_t regions;
  uint64_t tasks;
  uint64_t serial_regions;
  uint64_t openmp_regions;
  uint64_t c11_regions;
} ny_parallel_stats_t;

int ny_parallel_threads(void);
size_t ny_parallel_min_work(void);
bool ny_parallel_enabled(size_t work_items);
bool ny_parallel_for(size_t count, size_t work_items, ny_parallel_task_fn fn,
                     void *ctx);
void ny_parallel_stats(ny_parallel_stats_t *out);
void ny_parallel_stats_reset(void);

#endif
