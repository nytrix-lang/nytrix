/*
 * IR compaction: renumbers SSA values and basic blocks into a dense
 * contiguous range after optimization removes dead entries.
 */
#include "code/ir/opt/util.h"
#include "code/ir/internal.h"
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

  /*
   * Validate every value reference BEFORE renumbering anything. A use
   * whose definition was already erased has map[v] == -1; failing that
   * halfway through the mutation left instructions [0,k) renumbered to
   * dense ids while the rest kept old ids (two colliding id spaces, and
   * f->next_value still at the old bound). With validation up front the
   * mutation below cannot fail midway.
   */
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    const int refs[] = {in->dst, in->a, in->b, in->c, in->d, in->e, in->f};
    for (size_t k = 0; k < sizeof(refs) / sizeof(refs[0]); ++k) {
      int v = refs[k];
      if (v >= 0 && (v >= f->next_value || map[v] < 0)) {
        free(map);
        return false;
      }
    }
    for (size_t k = 0; k < in->extra_args_len; ++k) {
      int v = in->extra_args[k];
      if (v >= 0 && (v >= f->next_value || map[v] < 0)) {
        free(map);
        return false;
      }
    }
    for (size_t k = 0; k < in->phi_incoming_len; ++k) {
      int v = in->phi_incoming[k].value;
      if (v >= 0 && (v >= f->next_value || map[v] < 0)) {
        free(map);
        return false;
      }
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
