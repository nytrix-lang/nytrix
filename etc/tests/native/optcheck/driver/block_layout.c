/*
 * Regression driver for profile-heat block layout (nyir_block_layout).
 */
#include "util.h"

#include <unistd.h>
extern bool nyir_block_layout(nyir_func_t *f);
extern bool ny_native_profile_load_path(const char *path, char *err,
                                        size_t err_len);
extern void ny_native_profile_select(const char *name);
extern void ny_native_profile_clear(void);

/*
 *   entry:
 *     t = cond()
 *     if t goto then_label else join_label   (via br_if to then, fallthrough join)
 *   join (falls through):
 *     ret 0
 *   then:
 *     ret 1
 *
 * Profile marks the `then` block as hot, so the greedy layout should move it
 * so it sits on the hot path; regardless of the exact ordering, the CFG and
 * evaluation must be preserved and the verifier must accept the reordering.
 */
static void build_fn(nyir_func_t *f) {
  ny_begin(f);
  int c = ny_const(f, 0);
  int cond = f->next_value;
  nyir_emit(f, (nyir_inst_t){.op = NYIR_CMP_I64, .dst = cond, .a = c, .b = c,
                             .c = -1, .d = -1, .e = -1, .f = -1,
                             .cmp = NYIR_CMP_EQ});
  f->next_value = cond + 1;

  ny_br_if(f, cond, 20);

  ny_label(f, 30);
  ny_ret(f, ny_const(f, 0));

  ny_label(f, 20);
  ny_ret(f, ny_const(f, 1));
}

static int write_profile(char *path, size_t cap, const char *name) {
  int n = snprintf(path, cap, "%s/block_layout_profile_%ld.nyp",
                   getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (long)getpid());
  if (n < 0 || (size_t)n >= cap)
    return -1;
  FILE *f2 = fopen(path, "w");
  if (!f2)
    return -1;
  fprintf(f2, "NYP2 function=%s steps=100000\n", name);
  fprintf(f2, "pc 6 count=99999\n"); /* hot `then` block (label 20) */
  fprintf(f2, "edge 0 2 count=99999\n");
  fclose(f2);
  return 0;
}

static bool run_case(void) {
  nyir_func_t f;
  build_fn(&f);
  if (!ny_verify(&f, "block-layout-before"))
    return false;
  long long before = ny_eval(&f, "block-layout-before");
  size_t len_before = f.len;

  char path[512];
  if (write_profile(path, sizeof(path), "test") != 0) {
    nyir_func_free(&f);
    return false;
  }
  char err[256] = {0};
  bool loaded =
      ny_native_profile_load_path(path, err, sizeof(err));
  ny_native_profile_select("test");
  remove(path);
  if (!loaded) {
    fprintf(stderr, "[block-layout] profile load failed: %s\n", err);
    nyir_func_free(&f);
    return false;
  }

  bool ok = nyir_block_layout(&f) && ny_verify(&f, "block-layout-after");
  if (ny_want_dump()) {
    fprintf(stderr, "[block-layout] steps=%llu blockhot6=%llu\n",
            (unsigned long long)ny_native_profile_steps(),
            (unsigned long long)ny_native_profile_block_hot(6));
    nyir_dump(stderr, &f, "block-layout-after");
  }
  long long after = ny_eval(&f, "block-layout-after");
  size_t len_after = f.len;

  /*
   * Assert the reorder actually placed the hot `then` block (which returns 1)
   * before the cold `else` block (which returns 0) in the linear stream.
   */
  size_t first_ret0 = SIZE_MAX, first_ret1 = SIZE_MAX;
  for (size_t i = 0; i < f.len; ++i) {
    const nyir_inst_t *in = &f.data[i];
    if (in->op != NYIR_RET || in->a < 0 || in->a >= f.next_value)
      continue;
    /*
     * Find the return's source definition value.
     */
    int src = in->a;
    for (size_t j = i; j-- > 0;) {
      const nyir_inst_t *d = &f.data[j];
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
  nyir_func_free(&f);
  if (!ok)
    return false;
  if (before != after) {
    fprintf(stderr,
            "[block-layout] evaluation changed after reorder: %lld vs %lld\n",
            before, after);
    return false;
  }
  /*
   * Reordering a conditional-branch CFG may legitimately grow the stream by
   * one `br <fallthrough>` per moved br.if (the fallthrough edge cannot ride
   * the linear layout after the move).  The invariant is never-shrink and
   * identical evaluation, not byte-identical instruction count.
   */
  if (len_after < len_before) {
    fprintf(stderr,
            "[block-layout] instruction count shrank: %zu vs %zu\n",
            len_after, len_before);
    return false;
  }
  if (!reordered_ok) {
    fprintf(stderr,
            "[block-layout] hot block was not placed before the cold block\n");
    return false;
  }

  printf("[block-layout] ok result=%lld\n", after);
  return true;
}

int main(void) { return run_case() ? 0 : 1; }
