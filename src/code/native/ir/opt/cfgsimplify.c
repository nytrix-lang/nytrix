#include "code/native/ir/opt/util.h"
#include "code/native/ir/internal.h"
#include "base/compat.h"
#include "base/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool nyir_cfg_simplify(nyir_func_t *f) {
  if (!f)
    return true;

  bool *known = NULL;
  int64_t *value = NULL;
  if (f->next_value > 0) {
    known = (bool *)calloc((size_t)f->next_value, sizeof(bool));
    value = (int64_t *)calloc((size_t)f->next_value, sizeof(int64_t));
    if (!known || !value) {
      free(known);
      free(value);
      return false;
    }
    if (!nir_collect_consts(f, known, value)) {
      free(known);
      free(value);
      return false;
    }
  }

  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_BR_IF && in->a >= 0 && known && known[in->a]) {
      int64_t target = in->imm;
      if (value[in->a] != 0) {
        *in = (nyir_inst_t){.op = NYIR_BR,
                              .dst = -1,
                              .a = -1,
                              .b = -1,
                              .imm = target};
        /* Fallthrough is dead (e.g. `br L_else` after taken br_if). */
        for (size_t j = i + 1; j < f->len && f->data[j].op != NYIR_LABEL;
             ++j) {
          if (f->data[j].op != NYIR_NOP)
            (void)nyir_erase_instruction(f, j);
        }
      } else {
        *in = (nyir_inst_t){.op = NYIR_NOP, .dst = -1, .a = -1, .b = -1};
      }
    }
    if (in->op == NYIR_BR || in->op == NYIR_RET) {
      /* Anything after an unconditional terminator until the next label is
       * unreachable in linear NYIR layout. */
      for (size_t j = i + 1; j < f->len && f->data[j].op != NYIR_LABEL; ++j) {
        if (f->data[j].op != NYIR_NOP)
          (void)nyir_erase_instruction(f, j);
      }
    }
    if (in->op != NYIR_BR)
      continue;
    size_t next = nir_next_non_nop(f, i + 1);
    if (next < f->len && f->data[next].op == NYIR_LABEL &&
        f->data[next].imm == in->imm) {
      *in = (nyir_inst_t){.op = NYIR_NOP, .dst = -1, .a = -1, .b = -1};
    }
  }
  free(known);
  free(value);
  return true;
}
