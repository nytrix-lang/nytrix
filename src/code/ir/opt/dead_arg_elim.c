/*
 * Remove dead results from calls whose callee is audited as pure.
 *
 * This pass never changes a call's argument list or ABI.  Unknown and foreign
 * calls stay intact; only a call with a known local effect summary, or an
 * audited runtime helper with a CALL-only summary, can be removed when its
 * result has no uses.
 */
#include "code/ir/opt/util.h"
#include <stdlib.h>

static bool dead_arg_removable_call(const nyir_inst_t *in) {
  if (!in || in->op != NYIR_CALL || in->dst < 0 ||
      (in->flags & NYIR_INST_F_SRET) != 0 || !in->symbol || !in->symbol[0])
    return false;

  unsigned effects;
  if ((in->flags & NYIR_INST_F_EXTERN) != 0) {
    /*
     * Foreign calls are accepted only when the runtime table proves purity.
     */
    effects = nyir_call_effect_summary(in);
    if (effects != NYIR_EFFECT_CALL)
      return false;
  } else {
    /*
     * A local callee needs a builder-attached interprocedural summary.
     */
    if ((in->flags & NYIR_INST_F_EFFECTS_KNOWN) == 0 &&
        in->effects != NYIR_EFFECT_CALL)
      return false;
    effects = nyir_effective_effects(in);
    if (effects != NYIR_EFFECT_CALL)
      return false;
  }

  return (effects & ~(NYIR_EFFECT_CALL)) == 0;
}

bool nyir_dead_arg_elim(nyir_func_t *f) {
  if (!f || f->len == 0 || f->next_value <= 0)
    return true;

  /*
   * Removing a dead result can expose another dead call in a result chain.
   */
  bool changed;
  do {
    changed = false;
    nyir_use_def_t uses = {0};
    if (!nyir_build_use_def(f, &uses))
      return false;
    for (size_t i = f->len; i > 0; --i) {
      nyir_inst_t *in = &f->data[i - 1];
      if (!dead_arg_removable_call(in) ||
          (size_t)in->dst >= uses.value_count ||
          uses.offsets[in->dst] != uses.offsets[in->dst + 1])
        continue;
      /*
       * CAPTURE_RET implicitly reads the secondary return register of the
       * lexically preceding call. Erasing that call strands the capture
       * (stale rdx/xmm1 or a verifier failure), so a call followed by
       * CAPTURE_RET is never removable here even when its primary dst is
       * unused.
       */
      if (i < f->len && f->data[i].op == NYIR_CAPTURE_RET)
        continue;
      if (!nyir_erase_instruction(f, i - 1)) {
        nyir_use_def_free(&uses);
        return false;
      }
      changed = true;
    }
    nyir_use_def_free(&uses);
  } while (changed);
  return true;
}
