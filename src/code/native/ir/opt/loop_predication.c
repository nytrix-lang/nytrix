/*
 * Conservative loop predication for empty integer diamonds.
 *
 * Convert a loop-local control-only diamond selecting two already-computed
 * i64 values into wrapping arithmetic:
 *
 *   result = false_value + condition * (true_value - false_value)
 *
 * The transform is deliberately general (it is not a vector-tail guard), but
 * only fires when both arms contain no executable instruction other than their
 * branch to the same merge, both selected values dominate the condition, and
 * the condition is verifier-proven boolean.  Nothing is speculated and NyIR's
 * wrapping integer semantics make the select identity exact.
 */
#include "code/native/ir/opt/loop_analysis.h"
#include "code/native/ir/opt/util.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
  int64_t target;
  int64_t local;
  int value;
} predicated_arm_t;

/*
 * Frontend conditionals commonly materialize their result through one local
 * even after mem2reg.  Accept exactly COPY*; STORE_LOCAL; BR and resolve the
 * copied value back to a definition that dominates the diamond.
 */
static bool parse_store_arm(const nyir_func_t *f, const nyir_cfg_t *cfg,
                            const int *defs, size_t block,
                            predicated_arm_t *arm) {
  if (!f || !cfg || !defs || !arm || block >= cfg->block_count)
    return false;
  *arm = (predicated_arm_t){.target = -1, .local = -1, .value = -1};
  bool saw_store = false, saw_branch = false;
  for (size_t i = cfg->block_start[block]; i < cfg->block_end[block]; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL || in->op == NYIR_NOP || in->op == NYIR_COPY)
      continue;
    if (in->op == NYIR_STORE_LOCAL && !saw_store && !saw_branch) {
      saw_store = true;
      arm->local = in->imm;
      arm->value = in->a;
      continue;
    }
    if (in->op == NYIR_BR && saw_store && !saw_branch) {
      saw_branch = true;
      arm->target = in->imm;
      continue;
    }
    return false;
  }
  while (arm->value >= 0 && arm->value < f->next_value &&
         defs[arm->value] >= 0 && f->data[defs[arm->value]].op == NYIR_COPY)
    arm->value = f->data[defs[arm->value]].a;
  if (saw_store && !saw_branch && block + 1 < cfg->block_count) {
    arm->target = cfg->block_label[block + 1];
    saw_branch = true;
  }
  return saw_store && saw_branch;
}

static size_t skip_branch_trampoline(const nyir_func_t *f,
                                     const nyir_cfg_t *cfg, size_t block) {
  if (!f || !cfg || block >= cfg->block_count)
    return block;
  int64_t target = -1;
  for (size_t i = cfg->block_start[block]; i < cfg->block_end[block]; ++i) {
    if (f->data[i].op == NYIR_LABEL || f->data[i].op == NYIR_NOP)
      continue;
    if (f->data[i].op == NYIR_BR && target < 0) {
      target = f->data[i].imm;
      continue;
    }
    return block;
  }
  if (target < 0)
    return block;
  for (size_t b = 0; b < cfg->block_count; ++b)
    if (cfg->block_label[b] == target)
      return b;
  return block;
}

static bool bool_condition(const nyir_func_t *f, const int *defs, int value) {
  if (!f || !defs || value < 0 || value >= f->next_value || defs[value] < 0)
    return false;
  const nyir_inst_t *def = &f->data[defs[value]];
  return def->op == NYIR_CMP_I64 ||
         (def->range.has_min && def->range.has_max && def->range.min >= 0 &&
          def->range.max <= 1);
}

static bool value_dominates_block(const nyir_cfg_t *cfg, const int *defs,
                                  int value, size_t block) {
  return value >= 0 && defs[value] >= 0 &&
         nyir_cfg_dominates(cfg, cfg->inst_block[(size_t)defs[value]], block);
}

static bool replace_with_five(nyir_func_t *f, size_t at,
                              const nyir_inst_t replacement[5]) {
  if (!nir_ensure_inst_space(f, 4))
    return false;
  memmove(&f->data[at + 5], &f->data[at + 1],
          (f->len - at - 1) * sizeof(*f->data));
  memcpy(&f->data[at], replacement, 5 * sizeof(*replacement));
  f->len += 4;
  return true;
}

bool nyir_loop_predication(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  nyir_cfg_t cfg = {0};
  int *defs = nyir_build_defs(f);
  if (!defs || !nyir_cfg_build(f, &cfg)) {
    free(defs);
    nyir_cfg_free(&cfg);
    return false;
  }

  bool *loop_member = calloc(cfg.block_count, sizeof(*loop_member));
  bool *natural = calloc(cfg.block_count, sizeof(*natural));
  if (!loop_member || !natural) {
    free(loop_member);
    free(natural);
    nyir_cfg_free(&cfg);
    free(defs);
    return false;
  }
  for (size_t latch = 0; latch < cfg.block_count; ++latch) {
    for (size_t edge = cfg.succ_offsets[latch];
         edge < cfg.succ_offsets[latch + 1]; ++edge) {
      size_t header = cfg.succ_blocks[edge];
      if (!nyir_cfg_is_backedge(&cfg, latch, header))
        continue;
      if (!nyir_cfg_natural_loop_blocks(&cfg, latch, header, natural,
                                         cfg.block_count)) {
        free(loop_member);
        free(natural);
        nyir_cfg_free(&cfg);
        free(defs);
        return false;
      }
      for (size_t block = 0; block < cfg.block_count; ++block)
        loop_member[block] = loop_member[block] || natural[block];
    }
  }

  for (size_t head = 0; head < cfg.block_count; ++head) {
    if (!loop_member[head])
      continue;
    size_t end = cfg.block_end[head];
    while (end > cfg.block_start[head] && f->data[end - 1].op == NYIR_NOP)
      --end;
    if (end == cfg.block_start[head] || f->data[end - 1].op != NYIR_BR_IF)
      continue;
    size_t br_i = end - 1;
    nyir_inst_t *br = &f->data[br_i];
    if (!bool_condition(f, defs, br->a))
      continue;

    size_t true_block = SIZE_MAX;
    for (size_t b = 0; b < cfg.block_count; ++b)
      if (cfg.block_label[b] == br->imm)
        true_block = b;
    size_t false_block = head + 1;
    if (true_block == SIZE_MAX || false_block >= cfg.block_count)
      continue;
    false_block = skip_branch_trampoline(f, &cfg, false_block);
    if (true_block == false_block)
      continue;
    predicated_arm_t true_arm, false_arm;
    if (!parse_store_arm(f, &cfg, defs, true_block, &true_arm) ||
        !parse_store_arm(f, &cfg, defs, false_block, &false_arm) ||
        true_arm.target != false_arm.target ||
        true_arm.local != false_arm.local || true_arm.local < 0 ||
        !value_dominates_block(&cfg, defs, true_arm.value, head) ||
        !value_dominates_block(&cfg, defs, false_arm.value, head))
      continue;

    int sub = f->next_value++;
    int mul = f->next_value++;
    int result = f->next_value++;
    nyir_debug_loc_t debug = br->debug;
    nyir_inst_t replacement[5] = {
        {.op=NYIR_SUB_I64,.dst=sub,.a=true_arm.value,.b=false_arm.value,
         .c=-1,.d=-1,.e=-1,.f=-1,.debug=debug},
        {.op=NYIR_MUL_I64,.dst=mul,.a=br->a,.b=sub,
         .c=-1,.d=-1,.e=-1,.f=-1,.debug=debug},
        {.op=NYIR_ADD_I64,.dst=result,.a=false_arm.value,.b=mul,
         .c=-1,.d=-1,.e=-1,.f=-1,.debug=debug},
        {.op=NYIR_STORE_LOCAL,.dst=-1,.a=result,.b=-1,.c=-1,.d=-1,
         .e=-1,.f=-1,.imm=true_arm.local,.debug=debug},
        {.op=NYIR_BR,.dst=-1,.a=-1,.b=-1,.c=-1,.d=-1,.e=-1,.f=-1,
         .imm=true_arm.target,.debug=debug}};
    if (!replace_with_five(f, br_i, replacement)) {
      free(loop_member);
      free(natural);
      nyir_cfg_free(&cfg);
      free(defs);
      return false;
    }
    nyir_refresh_metadata(f);
    free(loop_member);
    free(natural);
    nyir_cfg_free(&cfg);
    free(defs);
    return true;
  }
  free(loop_member);
  free(natural);
  nyir_cfg_free(&cfg);
  free(defs);
  return true;
}
