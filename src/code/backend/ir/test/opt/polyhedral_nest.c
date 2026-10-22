/*
 * Driver for nyr_affine_nest: adjacent counted-loop fusion.
 *
 * Uses the shared two-loop fixture (util.h).  Golden 209: both
 * accumulators start at 100, loop1 adds its index three times
 * (100+0+1+2 = 103), loop2 adds twice its index (100+0+2+4 = 106), and
 * the exit sums the two header phis.
 */
#include "code/backend/ir/opt/loop.h"
#include "support.h"

static int root_copy(const nyr_func_t *f, const int *defs, int value) {
  for (int depth = 0; f && defs && value >= 0 && value < f->next_value &&
                      defs[value] >= 0 && depth < 16;
       ++depth) {
    const nyr_inst_t *in = &f->data[(size_t)defs[value]];
    if (in->op != NYIR_COPY)
      break;
    value = in->a;
  }
  return value;
}

static int *build_defs(const nyr_func_t *f) {
  if (!f || f->next_value < 0)
    return NULL;
  size_t count = f->next_value > 0 ? (size_t)f->next_value : 1;
  int *defs = malloc(count * sizeof(*defs));
  if (!defs)
    return NULL;
  for (size_t i = 0; i < count; ++i)
    defs[i] = -1;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].dst >= 0 && f->data[i].dst < f->next_value)
      defs[f->data[i].dst] = (int)i;
  return defs;
}

static bool affine_address_regression(int iv_scale) {
  nyr_func_t f;
  nyr_opt_begin(&f);
  int base = nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_ALLOCA,
                                         .dst = -1,
                                         .a = -1,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1,
                                         .imm = 128,
                                         .scalar_class = NYIR_SCALAR_POINTER});
  int zero = nyr_opt_const(&f, 0);
  int one = nyr_opt_const(&f, 1);
  int row = nyr_opt_const(&f, 2);
  int scale = nyr_opt_const(&f, 3);
  int limit = nyr_opt_const(&f, 4);
  nyr_opt_label(&f, 9);
  nyr_opt_br(&f, 10);
  nyr_opt_label(&f, 10);
  size_t phi_at = f.len;
  int iv = f.next_value;
  nyr_opt_phi(&f, 9, zero, 11, zero);
  int cond = nyr_opt_cmp(&f, iv, limit, NYIR_CMP_LT);
  nyr_opt_br_if(&f, cond, 11);
  nyr_opt_br(&f, 13);
  nyr_opt_label(&f, 11);
  int index_iv = iv;
  if (iv_scale != 1) {
    int factor = nyr_opt_const(&f, iv_scale);
    index_iv = nyr_opt_binop(&f, NYIR_MUL_I64, iv, factor);
  }
  int index = nyr_opt_binop(&f, NYIR_ADD_I64, row, index_iv);
  int byte_offset = nyr_opt_binop(&f, NYIR_SHL_I64, index, scale);
  int address = nyr_opt_binop(&f, NYIR_ADD_I64, base, byte_offset);
  nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_STORE_I64,
                              .dst = -1,
                              .a = address,
                              .b = -1,
                              .c = iv,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .effects = NYIR_EFFECT_WRITE_MEMORY |
                                         NYIR_EFFECT_MAY_TRAP,
                              .scalar_class = NYIR_SCALAR_I64_RAW});
  int next = nyr_opt_binop(&f, NYIR_ADD_I64, iv, one);
  nyr_opt_br(&f, 10);
  nyr_opt_label(&f, 13);
  int result_offset = nyr_opt_const(&f, 8 * (2 + 2 * iv_scale));
  int result_address = nyr_opt_binop(&f, NYIR_ADD_I64, base, result_offset);
  int result =
      nyr_ir_emit(&f, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                  .dst = -1,
                                  .a = result_address,
                                  .b = -1,
                                  .c = -1,
                                  .d = -1,
                                  .e = -1,
                                  .f = -1,
                                  .effects = NYIR_EFFECT_READ_MEMORY |
                                             NYIR_EFFECT_MAY_TRAP,
                                  .scalar_class = NYIR_SCALAR_I64_RAW});
  nyr_opt_ret(&f, result);
  f.data[phi_at].phi_incoming[1].value = next;
  bool ok = nyr_opt_verify(&f, "affine-address/before");
  long long before = ok ? nyr_opt_eval(&f, "affine-address/before") : 0;
  ok = ok && before == 2 && nyr_affine_nest(&f) && nyr_licm(&f) &&
       nyr_opt_verify(&f, "affine-address/after") &&
       nyr_opt_eval(&f, "affine-address/after") == before;

  int *defs = ok ? build_defs(&f) : NULL;
  nyr_cfg_t cfg = {0};
  nyr_scev_info_t info = {0};
  bool shape_ok = defs && nyr_cfg_build(&f, &cfg) &&
                  nyr_scev_analyze(&f, &info) && info.count == 1;
  bool *members = shape_ok ? calloc(cfg.block_count, sizeof(*members)) : NULL;
  shape_ok = shape_ok && members &&
             nyr_cfg_natural_loop_blocks(&cfg, info.loops[0].latch_block,
                                          info.loops[0].header_block, members,
                                          cfg.block_count);
  size_t store_at = SIZE_MAX;
  for (size_t i = 0; shape_ok && i < f.len; ++i)
    if (f.data[i].op == NYIR_STORE_I64) {
      store_at = i;
      break;
    }
  shape_ok = shape_ok && store_at != SIZE_MAX;
  if (shape_ok && iv_scale == 1) {
    int new_address = f.data[store_at].a;
    int address_def =
        new_address >= 0 && new_address < f.next_value ? defs[new_address] : -1;
    const nyr_inst_t *split = address_def >= 0 ? &f.data[address_def] : NULL;
    int iv_bytes = split ? root_copy(&f, defs, split->b) : -1;
    int iv_scale_def =
        iv_bytes >= 0 && iv_bytes < f.next_value ? defs[iv_bytes] : -1;
    const nyr_inst_t *iv_scale =
        iv_scale_def >= 0 ? &f.data[iv_scale_def] : NULL;
    int iv_root = iv_scale ? root_copy(&f, defs, iv_scale->a) : -1;
    int row_base = split ? root_copy(&f, defs, split->a) : -1;
    int row_base_def =
        row_base >= 0 && row_base < f.next_value ? defs[row_base] : -1;
    shape_ok = split && split->op == NYIR_ADD_I64 && iv_scale &&
               iv_scale->op == NYIR_SHL_I64 && iv_root == info.loops[0].iv &&
               row_base_def >= 0 &&
               !members[cfg.inst_block[(size_t)row_base_def]];
  } else if (shape_ok) {
    bool unexpected_split = false;
    for (size_t i = 0; i < f.len; ++i) {
      const nyr_inst_t *in = &f.data[i];
      if (in->op != NYIR_SHL_I64 || in->a < 0 || in->a >= f.next_value)
        continue;
      unexpected_split |= root_copy(&f, defs, in->a) == info.loops[0].iv;
    }
    shape_ok = !unexpected_split;
  }
  free(members);
  free(defs);
  nyr_scev_free(&info);
  nyr_cfg_free(&cfg);
  nyr_ir_func_free(&f);
  if (!shape_ok)
    fputs("[affine-address] invariant row base was not hoisted away from "
          "the column loop\n",
          stderr);
  else if (iv_scale == 1)
    puts("[affine-address] row base hoisted; scaled IV remains in the "
         "memory address");
  else
    puts("[affine-address] non-unit IV coefficient stays unchanged");
  return shape_ok;
}

int main(void) {
  if (!affine_address_regression(1) || !affine_address_regression(2))
    return 1;
  /*
   * Positive: equal trip counts fuse and keep the program intact.
   */
  nyr_func_t f;
  nyr_opt_build_two_loops(&f, 3, 3);
  if (!nyr_opt_verify(&f, "fuse/verify-before"))
    return 1;
  long long before = nyr_opt_eval(&f, "fuse/before");
  if (nyr_opt_want_dump())
    nyr_dump(stdout, &f, "before-fuse");
  if (!nyr_affine_nest(&f)) {
    fprintf(stderr, "[fuse] pass reported failure\n");
    return 1;
  }
  if (!nyr_opt_verify(&f, "fuse/verify-after"))
    return 1;
  long long after = nyr_opt_eval(&f, "fuse/after");
  if (nyr_opt_want_dump())
    nyr_dump(stdout, &f, "after-fuse");
  nyr_ir_func_free(&f);
  nyr_opt_check("fuse", before, after, 209);

  /*
   * Negative: mismatched trip counts must not fuse (and must not
   * corrupt anything on the way out).
   */
  nyr_func_t g;
  nyr_opt_build_two_loops(&g, 3, 5);
  if (!nyr_opt_verify(&g, "nofuse/verify-before"))
    return 1;
  before = nyr_opt_eval(&g, "nofuse/before");
  if (!nyr_affine_nest(&g)) {
    fprintf(stderr, "[nofuse] pass reported failure\n");
    return 1;
  }
  if (!nyr_opt_verify(&g, "nofuse/verify-after"))
    return 1;
  after = nyr_opt_eval(&g, "nofuse/after");
  nyr_ir_func_free(&g);
  /*
   * m=5: s1 stays 103, s2 becomes 100+0+2+4+6+8 = 120
   */
  nyr_opt_check("nofuse", before, after, 223);
  return 0;
}
