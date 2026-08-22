#ifndef NYIR_LOOP_ANALYSIS_H
#define NYIR_LOOP_ANALYSIS_H

#include "code/native/ir.h"

typedef struct {
  size_t header_block;
  size_t latch_block;
  size_t preheader_block;
  size_t header_index;
  size_t latch_index;
  int64_t header_label;
  int iv;
  int init_value;
  int next_value;
  int limit_value;
  int64_t init;
  int64_t step;
  int64_t limit;
  nyir_cmp_t predicate;
  bool limit_is_const;
  bool trip_count_known;
  uint64_t trip_count;
} nyir_scev_loop_t;

typedef struct {
  nyir_scev_loop_t *loops;
  size_t count;
} nyir_scev_info_t;

bool nyir_scev_analyze(const nyir_func_t *f, nyir_scev_info_t *out);
void nyir_scev_free(nyir_scev_info_t *info);
bool nyir_scev_lite(nyir_func_t *f);
bool nyir_irce(nyir_func_t *f);
bool nyir_loop_idiom(nyir_func_t *f);
bool nyir_loop_rotate(nyir_func_t *f);
bool nyir_loop_interchange(nyir_func_t *f);
bool nyir_loop_versioning(nyir_func_t *f);
bool nyir_loop_predication(nyir_func_t *f);

#endif
