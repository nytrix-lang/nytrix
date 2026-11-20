/*
 * Execute latch recoloring under pressure, fixed scratch and PHI snapshots.
 */
#include "base/compat.h"
#include "code/backend/mach/object.h"
#include "opt.h"

enum recolor_mode {
  PLAIN,
  EXTRA_USE,
  OVERLAP,
  CALL,
  SHIFT,
  DIVIDE,
  POW2_DIV,
  POW2_MOD,
  STORE,
  CYCLE
};
static const char *const mode_names[] = {
    "plain",  "extra-use", "overlap",  "call",  "shift",
    "divide", "pow2-div",  "pow2-mod", "store", "cycle"};
static struct {
  int64_t count, shift, divisor;
  uint64_t seed[12], stored;
} input;

static int load_at(nyr_func_t *ir, const void *p) {
  int address = nyr_opt_const(ir, (int64_t)(uintptr_t)p);
  return nyr_opt_emit_abc(ir, NYIR_LOAD_I64, address, -1, -1);
}

static void build(nyr_func_t *ir, enum recolor_mode mode, int homes) {
  nyr_opt_begin(ir);
  nyr_opt_label(ir, 1);
  int limit = load_at(ir, &input.count);
  int seeds[12], state[12], read[12], next[12];
  size_t phis[12];
  for (int j = 0; j < homes; ++j)
    seeds[j] = load_at(ir, &input.seed[j]);
  int zero = nyr_opt_const(ir, 0), one = nyr_opt_const(ir, 1);
  nyr_opt_br(ir, 2);
  nyr_opt_label(ir, 2);
  size_t iv_phi = ir->len;
  int iv = ir->next_value;
  nyr_opt_phi(ir, 1, zero, 3, zero);
  for (int j = 0; j < homes; ++j) {
    phis[j] = ir->len;
    state[j] = ir->next_value;
    nyr_opt_phi(ir, 1, seeds[j], 3, seeds[j]);
  }
  /*
   * Read the old destinations in the header. Their latch ranges begin at
   * the edge copies, whereas producers live across the body scratch below.
   */
  int result = zero;
  for (int j = 0; j < homes; ++j) {
    int salt = nyr_opt_const(ir, j + 1);
    read[j] = nyr_opt_binop(ir, NYIR_ADD_I64, state[j], salt);
    result = nyr_opt_binop(ir, NYIR_XOR_I64, result, read[j]);
  }
  nyr_opt_br_if(ir, nyr_opt_cmp(ir, iv, limit, NYIR_CMP_LT), 3);
  nyr_opt_br(ir, 4);
  nyr_opt_label(ir, 3);
  for (int j = 0; j < homes; ++j)
    next[j] = nyr_opt_binop(ir, NYIR_ADD_I64, read[j], one);
  int noise = zero;
  if (mode == EXTRA_USE)
    noise = nyr_opt_binop(ir, NYIR_XOR_I64, next[0], iv);
  if (mode == OVERLAP)
    /*
     * Old destination still needed after its would-be producer.
     */
    noise = nyr_opt_binop(ir, NYIR_ADD_I64, state[0], iv);
  if (mode == SHIFT) {
    int shift = load_at(ir, &input.shift);
    noise = nyr_opt_binop(ir, NYIR_SHL_I64, iv, shift);
  }
  if (mode == DIVIDE || mode == POW2_DIV || mode == POW2_MOD) {
    int divisor =
        mode == DIVIDE ? load_at(ir, &input.divisor) : nyr_opt_const(ir, 8);
    noise = nyr_opt_binop(ir, mode == POW2_MOD ? NYIR_MOD_I64 : NYIR_DIV_I64,
                          read[homes - 1], divisor);
  }
  if (mode == CALL) {
    noise = nyr_ir_emit(
        ir, (nyr_inst_t){
                .op = NYIR_CALL,
                .dst = -1,
                .a = -1,
                .b = -1,
                .c = -1,
                .d = -1,
                .e = -1,
                .f = -1,
                .imm = 0,
                .symbol = nyr_func_own_symbol_copy(ir, "recolor_clobber"),
                .flags = NYIR_INST_F_EFFECTS_KNOWN,
                .effects = NYIR_EFFECT_CALL | NYIR_EFFECT_UNKNOWN_SIDE_EFFECT});
    ir->has_symbol_calls = true;
  }
  if (mode == STORE) {
    int address = nyr_opt_const(ir, (int64_t)(uintptr_t)&input.stored);
    nyr_ir_emit(ir, (nyr_inst_t){.op = NYIR_STORE_I64,
                                 .dst = -1,
                                 .a = address,
                                 .b = -1,
                                 .c = result,
                                 .d = -1,
                                 .e = -1,
                                 .f = -1});
  }
  if (mode == CYCLE) {
    next[0] = state[1];
    next[1] = nyr_opt_binop(ir, NYIR_ADD_I64, state[0], state[1]);
  } else if (mode != PLAIN && mode != STORE) {
    next[homes - 1] = nyr_opt_binop(ir, NYIR_XOR_I64, next[homes - 1], noise);
  }
  int next_iv = nyr_opt_binop(ir, NYIR_ADD_I64, iv, one);
  nyr_opt_br(ir, 2);
  nyr_opt_label(ir, 4);
  nyr_opt_ret(ir, result);
  ir->data[iv_phi].phi_incoming[1].value = next_iv;
  for (int j = 0; j < homes; ++j)
    ir->data[phis[j]].phi_incoming[1].value = next[j];
}

static uint64_t reference(enum recolor_mode mode, int homes) {
  uint64_t state[12], read[12], next[12], result = 0;
  memcpy(state, input.seed, sizeof(state));
  for (int64_t i = 0;; ++i) {
    result = 0;
    for (int j = 0; j < homes; ++j) {
      read[j] = state[j] + (unsigned)j + 1;
      result ^= read[j];
      next[j] = read[j] + 1;
    }
    if (i >= input.count)
      return result;
    uint64_t noise = 0;
    switch (mode) {
    case EXTRA_USE:
      noise = next[0] ^ (uint64_t)i;
      break;
    case OVERLAP:
      noise = state[0] + (uint64_t)i;
      break;
    case CALL:
      noise = 17;
      break;
    case SHIFT:
      noise = (uint64_t)i << input.shift;
      break;
    case DIVIDE:
      noise = (uint64_t)((int64_t)read[homes - 1] / input.divisor);
      break;
    case POW2_DIV:
      noise = (uint64_t)((int64_t)read[homes - 1] / 8);
      break;
    case POW2_MOD:
      noise = (uint64_t)((int64_t)read[homes - 1] % 8);
      break;
    case STORE:
      input.stored = result;
      break;
    case CYCLE:
      next[0] = state[1];
      next[1] = state[0] + state[1];
      break;
    default:
      break;
    }
    if (mode != PLAIN && mode != STORE && mode != CYCLE)
      next[homes - 1] ^= noise;
    memcpy(state, next, (size_t)homes * sizeof(*state));
  }
}

static int check(enum recolor_mode mode, int homes) {
#if !defined(__x86_64__)
  return 0;
#else
  nyr_func_t ir;
  build(&ir, mode, homes);
  ny_options opt = {0};
  opt.native_backend = NY_NATIVE_BACKEND_X86_64;
  opt.native_abi = NY_NATIVE_ABI_SYSV;
  opt.host_triple = "x86_64-linux-gnu";
  nyr_target_request_t request = nyr_target_request_make(
      opt.native_backend, opt.native_abi, opt.host_triple, opt.opt_size);
  ny_native_target_info_t target = {0};
  nyr_mach_func_t mach = {0};
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[2] = {0};
  nyr_x64_obj_reloc_t relocs[8] = {0};
  size_t def_count = 0, reloc_count = 0;
  char err[256] = {0};
  bool ok = nyr_opt_verify(&ir, "loop-recolor") &&
            nyr_target_info_init(&target, &request) &&
            nyr_mach_lower_nir(&ir, &mach, target.caps, err, sizeof(err)) &&
            nyr_x64_mach_append_function(
                &code, defs, &def_count, relocs, &reloc_count, &mach, &target,
                "loop_recolor", false, err, sizeof(err));
  /*
   * A real call to a local stub that destroys every SysV caller-saved GPR.
   * Keep it in the mapping so its PC-relative relocation is always in range.
   */
  static const unsigned char clobber[] = {
      0x31, 0xc9, 0x31, 0xd2, 0x31, 0xf6, 0x31, 0xff, 0x45,
      0x31, 0xc0, 0x45, 0x31, 0xc9, 0x45, 0x31, 0xd2, 0x45,
      0x31, 0xdb, 0xb8, 17,   0,    0,    0,    0xc3};
  size_t stub = code.len;
  if (ok && mode == CALL)
    ok = nyr_obj_emit(&code, clobber, sizeof(clobber));
  for (size_t i = 0; ok && i < reloc_count; ++i) {
    ok = mode == CALL &&
         strcmp(relocs[i].symbol, "ny_fn_recolor_clobber") == 0 &&
         relocs[i].type == NY_RELOC_PLT32 && relocs[i].disp_off + 4 <= stub;
    if (ok) {
      int32_t delta = (int32_t)(stub - (relocs[i].disp_off + 4));
      memcpy(code.data + relocs[i].disp_off, &delta, sizeof(delta));
    }
  }
  void *mapping = ok ? nyr_opt_execmem_alloc(code.len) : MAP_FAILED;
  ok = ok && mapping != MAP_FAILED && def_count == 1 &&
       reloc_count == (mode == CALL ? 1u : 0u);
  if (ok) {
    memcpy(mapping, code.data, code.len);
    ok = nyr_opt_execmem_protect(mapping, code.len) == 0;
  }
  if (ok && getenv("NYTRIX_TEST_DUMP_CODE")) {
    char dump_path[512];
    snprintf(dump_path, sizeof(dump_path), "%s%s",
             getenv("NYTRIX_TEST_DUMP_CODE"),
             getenv("NYTRIX_DISABLE_LOOP_GPR") ? ".noloop" : "");
    FILE *dump = fopen(dump_path, "wb");
    if (dump) {
      fwrite(code.data, 1, code.len, dump);
      fclose(dump);
    }
  }
  if (ok) {
    typedef uint64_t(__attribute__((sysv_abi)) * kernel_t)(void);
    kernel_t fn = (kernel_t)((unsigned char *)mapping + defs[0].off);
    uint64_t random = UINT64_C(0x123456789abcdef0);
    for (int trial = 0; ok && trial < 32; ++trial) {
      for (int j = 0; j < 12; ++j) {
        random = random * UINT64_C(6364136223846793005) + 1;
        input.seed[j] = trial == 0 ? UINT64_C(0x8000000000000000) + j : random;
      }
      input.shift = trial % 64;
      input.divisor = trial % 2 ? -3 : 3;
      for (input.count = -2; ok && input.count <= 64; ++input.count) {
        input.stored = UINT64_MAX;
        uint64_t expected = reference(mode, homes), stored = input.stored;
        input.stored = UINT64_MAX;
        uint64_t actual = fn();
        ok = actual == expected && input.stored == stored;
        if (!ok) {
          fprintf(stderr,
                  "recolor mode=%s homes=%d trial=%d count=%lld "
                  "shift=%lld divisor=%lld expected=%llu actual=%llu "
                  "store=%llu/%llu seeds=",
                  mode_names[mode], homes, trial, (long long)input.count,
                  (long long)input.shift, (long long)input.divisor,
                  (unsigned long long)expected, (unsigned long long)actual,
                  (unsigned long long)stored, (unsigned long long)input.stored);
          for (int j = 0; j < homes; ++j)
            fprintf(stderr, " %llu", (unsigned long long)input.seed[j]);
          fputc('\n', stderr);
        }
      }
    }
  }
  if (!ok)
    fprintf(stderr, "x64 loop recolor mode=%s homes=%d: %s\n", mode_names[mode],
            homes, err);
  nyr_opt_execmem_free(mapping, code.len);
  nyr_mach_func_free(&mach);
  nyr_obj_free(&code);
  nyr_ir_func_free(&ir);
  return ok ? 0 : 1;
#endif
}

int main(void) {
  int failed = 0;
  /*
   * Run each case with dedicated loop homes and again with them disabled:
   * the ordinary allocator must honor the same fixed clobbers (RCX for a
   * variable shift) when it recolors loop-carried edge copies.
   */
  const char *saved = getenv("NYTRIX_DISABLE_LOOP_GPR");
  char *saved_copy = saved ? strdup(saved) : NULL;
  for (int disable = 0; disable < 2; ++disable) {
    if (disable)
      ny_setenv("NYTRIX_DISABLE_LOOP_GPR", "1", 1);
    else if (saved_copy)
      ny_setenv("NYTRIX_DISABLE_LOOP_GPR", saved_copy, 1);
    else
      ny_unsetenv("NYTRIX_DISABLE_LOOP_GPR");
    for (int mode = PLAIN; mode <= CYCLE; ++mode) {
      if (getenv("NYTRIX_TEST_CASE") &&
          mode != atoi(getenv("NYTRIX_TEST_CASE")))
        continue;
      /*
       * Low pressure exercises already colored sources; high pressure reaches
       * R8/R9/RCX/RDX and homes overlapping the extended producer intervals.
       */
      for (int homes = 2; homes <= 12; ++homes) {
        if (getenv("NYTRIX_TEST_HOMES") &&
            homes != atoi(getenv("NYTRIX_TEST_HOMES")))
          continue;
        failed |= check((enum recolor_mode)mode, homes);
      }
    }
  }
  if (saved_copy) {
    ny_setenv("NYTRIX_DISABLE_LOOP_GPR", saved_copy, 1);
    free(saved_copy);
  } else {
    ny_unsetenv("NYTRIX_DISABLE_LOOP_GPR");
  }
  return failed;
}
