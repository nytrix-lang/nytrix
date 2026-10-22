/*
 * Regression driver for profile-heat block layout (nyr_block_layout).
 */
#include "support.h"

#include <unistd.h>
extern bool nyr_block_layout(nyr_func_t *f);
extern bool ny_native_profile_load_path(const char *path, char *err,
                                        size_t err_len);
extern void ny_native_profile_select(const char *name);
extern void ny_native_profile_clear(void);
extern void ny_native_profile_set_runtime(uint64_t edges, uint64_t steps);

/*
 *   entry:
 *     t = cond()
 *     if t goto then_label else join_label   (via br_if to then, fallthrough
 * join) join (falls through): ret 0 then: ret 1
 *
 * Profile marks the `then` block as hot, so the greedy layout should move it
 * so it sits on the hot path; regardless of the exact ordering, the CFG and
 * evaluation must be preserved and the verifier must accept the reordering.
 */
static void build_fn(nyr_func_t *f, int64_t lhs, int64_t rhs) {
  nyr_opt_begin(f);
  int a = nyr_opt_const(f, lhs);
  int b = nyr_opt_const(f, rhs);
  int cond = f->next_value;
  nyr_ir_emit(f, (nyr_inst_t){.op = NYIR_CMP_I64,
                             .dst = cond,
                             .a = a,
                             .b = b,
                             .c = -1,
                             .d = -1,
                             .e = -1,
                             .f = -1,
                             .cmp = NYIR_CMP_EQ});
  f->next_value = cond + 1;

  nyr_opt_br_if(f, cond, 20);

  nyr_opt_label(f, 30);
  nyr_opt_ret(f, nyr_opt_const(f, 0));

  nyr_opt_label(f, 20);
  nyr_opt_ret(f, nyr_opt_const(f, 1));
}

static int write_profile(char *path, size_t cap, const char *name,
                         size_t hot_pc) {
  int n =
      snprintf(path, cap, "%s/block_layout_profile_%ld.nyp",
               getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (long)getpid());
  if (n < 0 || (size_t)n >= cap)
    return -1;
  FILE *f2 = fopen(path, "w");
  if (!f2)
    return -1;
  fprintf(f2, "NYP2 function=%s steps=100000\n", name);
  fprintf(f2, "pc %zu count=99999\n", hot_pc);
  fprintf(f2, "edge 0 2 count=99999\n");
  fclose(f2);
  return 0;
}

static bool run_case(int64_t lhs, int64_t rhs, int64_t expected,
                     bool check_reorder) {
  nyr_func_t f;
  build_fn(&f, lhs, rhs);
  if (!nyr_opt_verify(&f, "block-layout-before"))
    return false;
  long long before = nyr_opt_eval(&f, "block-layout-before");
  size_t len_before = f.len;

  size_t hot_pc = SIZE_MAX;
  for (size_t i = 0; i < f.len; ++i)
    if (f.data[i].op == NYIR_LABEL && f.data[i].imm == 20) {
      hot_pc = i;
      break;
    }
  if (hot_pc == SIZE_MAX) {
    nyr_ir_func_free(&f);
    return false;
  }

  char path[512];
  if (write_profile(path, sizeof(path), "test", hot_pc) != 0) {
    nyr_ir_func_free(&f);
    return false;
  }
  char err[256] = {0};
  bool loaded = ny_native_profile_load_path(path, err, sizeof(err));
  ny_native_profile_select("test");
  remove(path);
  if (!loaded) {
    fprintf(stderr, "[block-layout] profile load failed: %s\n", err);
    nyr_ir_func_free(&f);
    return false;
  }

  bool ok = nyr_block_layout(&f) && nyr_opt_verify(&f, "block-layout-after");
  if (nyr_opt_want_dump()) {
    fprintf(stderr, "[block-layout] steps=%llu blockhot=%llu\n",
            (unsigned long long)ny_native_profile_steps(),
            (unsigned long long)ny_native_profile_block_hot(hot_pc));
    nyr_dump(stderr, &f, "block-layout-after");
  }
  long long after = nyr_opt_eval(&f, "block-layout-after");
  size_t len_after = f.len;

  /*
   * Assert the reorder actually placed the hot `then` block (which returns 1)
   * before the cold `else` block (which returns 0) in the linear stream.
   */
  size_t first_ret0 = SIZE_MAX, first_ret1 = SIZE_MAX;
  for (size_t i = 0; i < f.len; ++i) {
    const nyr_inst_t *in = &f.data[i];
    if (in->op != NYIR_RET || in->a < 0 || in->a >= f.next_value)
      continue;
    /*
     * Find the return's source definition value.
     */
    int src = in->a;
    for (size_t j = i; j-- > 0;) {
      const nyr_inst_t *d = &f.data[j];
      if (d->dst == src && d->op == NYIR_CONST_I64) {
        if (d->imm == 0 && first_ret0 == SIZE_MAX)
          first_ret0 = i;
        else if (d->imm == 1 && first_ret1 == SIZE_MAX)
          first_ret1 = i;
        break;
      }
    }
  }
  bool reordered_ok = first_ret1 != SIZE_MAX && first_ret0 != SIZE_MAX &&
                      first_ret1 < first_ret0;

  ny_native_profile_clear();
  nyr_ir_func_free(&f);
  if (!ok)
    return false;
  if (before != expected || after != expected) {
    fprintf(stderr,
            "[block-layout] expected %lld; evaluation was %lld before and %lld "
            "after reorder\n",
            (long long)expected, before, after);
    return false;
  }
  /*
   * Reordering a conditional-branch CFG may legitimately grow the stream by
   * one `br <fallthrough>` per moved br.if (the fallthrough edge cannot ride
   * the linear layout after the move).  The invariant is never-shrink and
   * identical evaluation, not byte-identical instruction count.
   */
  if (len_after < len_before) {
    fprintf(stderr, "[block-layout] instruction count shrank: %zu vs %zu\n",
            len_after, len_before);
    return false;
  }
  if (check_reorder && !reordered_ok) {
    fprintf(stderr,
            "[block-layout] hot block was not placed before the cold block\n");
    return false;
  }

  printf("[block-layout] ok input=%lld==%lld result=%lld reordered=%s\n",
         (long long)lhs, (long long)rhs, after, reordered_ok ? "yes" : "no");
  return true;
}

/*
 * Synthetic profile hints must not make an all-zero block profile reorder CFG.
 */
static bool run_no_heat_case(void) {
  ny_native_profile_clear();
  ny_native_profile_set_runtime(200, 2000);
  nyr_func_t f;
  build_fn(&f, 0, 0);
  if (f.len > 32 || !nyr_opt_verify(&f, "block-layout-no-heat-before")) {
    nyr_ir_func_free(&f);
    ny_native_profile_clear();
    return false;
  }
  nyr_inst_t before[32];
  size_t len = f.len;
  memcpy(before, f.data, len * sizeof(*before));
  long long expected = nyr_opt_eval(&f, "block-layout-no-heat-before");
  bool ok = nyr_block_layout(&f) &&
            nyr_opt_verify(&f, "block-layout-no-heat-after") && f.len == len &&
            memcmp(before, f.data, len * sizeof(*before)) == 0 &&
            nyr_opt_eval(&f, "block-layout-no-heat-after") == expected;
  nyr_ir_func_free(&f);
  ny_native_profile_clear();
  if (!ok)
    fprintf(stderr,
            "[block-layout] synthetic profile reordered a zero-heat CFG\n");
  else
    puts("[block-layout] ok synthetic profile without block heat is a no-op");
  return ok;
}

int main(void) {
  return run_case(0, 0, 1, true) && run_case(1, 0, 0, false) &&
                 run_no_heat_case()
             ? 0
             : 1;
}
