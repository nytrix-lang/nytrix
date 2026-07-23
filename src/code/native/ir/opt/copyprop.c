#include "code/native/ir/opt/util.h"
#include "code/native/ir/internal.h"
#include "base/compat.h"
#include "base/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int nir_alias_find(const int *alias, int v) {
  if (!alias || v < 0)
    return v;
  int cur = v;
  for (int depth = 0; depth < 64 && alias[cur] >= 0 && alias[cur] != cur; ++depth)
    cur = alias[cur];
  return cur;
}

bool nyir_copy_prop(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  int *alias = (int *)malloc((size_t)f->next_value * sizeof(int));
  if (!alias)
    return false;
  for (int i = 0; i < f->next_value; ++i)
    alias[i] = i;
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->a >= 0)
      in->a = nir_alias_find(alias, in->a);
    if (in->b >= 0)
      in->b = nir_alias_find(alias, in->b);
    if (in->c >= 0)
      in->c = nir_alias_find(alias, in->c);
    if (in->d >= 0)
      in->d = nir_alias_find(alias, in->d);
    if (in->e >= 0)
      in->e = nir_alias_find(alias, in->e);
    if (in->f >= 0)
      in->f = nir_alias_find(alias, in->f);
    for (size_t k = 0; k < in->extra_args_len; ++k) {
      if (in->extra_args[k] >= 0)
        in->extra_args[k] = nir_alias_find(alias, in->extra_args[k]);
    }
    for (size_t k = 0; k < in->phi_incoming_len; ++k)
      if (in->phi_incoming[k].value >= 0)
        in->phi_incoming[k].value =
            nir_alias_find(alias, in->phi_incoming[k].value);
    if (in->dst >= 0) {
      if (in->op == NYIR_COPY && in->a >= 0)
        alias[in->dst] = nir_alias_find(alias, in->a);
      else
        alias[in->dst] = in->dst;
    }
  }
  free(alias);
  return true;
}
