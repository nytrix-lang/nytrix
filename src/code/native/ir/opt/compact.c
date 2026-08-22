/*
 * IR compaction: renumbers SSA values and basic blocks into a dense
 * contiguous range after optimization removes dead entries.
 */
#include "code/native/ir/opt/util.h"
#include "code/native/ir/internal.h"
#include "base/compat.h"
#include "base/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool nyir_compact(nyir_func_t *f) {
  if (!f || f->len == 0)
    return true;

  size_t out = 0;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op == NYIR_NOP)
      continue;
    if (out != i)
      f->data[out] = f->data[i];
    out++;
  }
  f->len = out;

  if (f->next_value <= 0)
    return true;

  int *map = (int *)malloc((size_t)f->next_value * sizeof(*map));
  if (!map)
    return false;
  for (int i = 0; i < f->next_value; ++i)
    map[i] = -1;

  int next = 0;
  for (size_t i = 0; i < f->len; ++i) {
    int dst = f->data[i].dst;
    if (dst >= 0) {
      if (dst >= f->next_value) {
        free(map);
        return false;
      }
      map[dst] = next++;
    }
  }

  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (!nir_remap_value(map, f->next_value, in->dst, &in->dst) ||
        !nir_remap_value(map, f->next_value, in->a, &in->a) ||
        !nir_remap_value(map, f->next_value, in->b, &in->b) ||
        !nir_remap_value(map, f->next_value, in->c, &in->c) ||
        !nir_remap_value(map, f->next_value, in->d, &in->d) ||
        !nir_remap_value(map, f->next_value, in->e, &in->e) ||
        !nir_remap_value(map, f->next_value, in->f, &in->f)) {
      free(map);
      return false;
    }
    for (size_t k = 0; k < in->extra_args_len; ++k) {
      if (!nir_remap_value(map, f->next_value, in->extra_args[k],
                           &in->extra_args[k])) {
        free(map);
        return false;
      }
    }
    for (size_t k = 0; k < in->phi_incoming_len; ++k) {
      if (!nir_remap_value(map, f->next_value, in->phi_incoming[k].value,
                           &in->phi_incoming[k].value)) {
        free(map);
        return false;
      }
    }
  }
  f->next_value = next;
  free(map);
  return true;
}
