/*
 * Private home dead-store analysis helpers, included by object.c.
 */
#include <stdatomic.h>
/*
 * Every %rbp-relative encoding the x86-64 emitter can produce.
 */
static void mach_home_count_refs(const unsigned char *code, size_t len,
                                 uint32_t *refs, size_t limit) {
  for (size_t i = 0; i + 1 < len; ++i) {
    unsigned char modrm = code[i];
    int64_t disp = 0;
    if ((modrm & 0xC7) == 0x45) {
      disp = (int8_t)code[i + 1];
    } else if ((modrm & 0xC7) == 0x85 && i + 4 < len) {
      int32_t d;
      memcpy(&d, code + i + 1, 4);
      disp = d;
    } else if ((modrm & 0xC7) == 0x44 && i + 2 < len &&
               (code[i + 1] & 7) == 5) {
      disp = (int8_t)code[i + 2];
    } else if ((modrm & 0xC7) == 0x84 && i + 5 < len &&
               (code[i + 1] & 7) == 5) {
      int32_t d;
      memcpy(&d, code + i + 2, 4);
      disp = d;
    } else {
      continue;
    }
    if (disp < 0 && (uint64_t)(-disp) < limit)
      refs[-disp]++;
  }
}

/*
 * Encode once, then find vreg homes whose only references in the machine code
 * are the stores the encoder counted. Returns the table for the real encoding,
 * or NULL when nothing can be dropped. A failure here is ignored: the real
 * encoding reports its own errors.
 */
static unsigned char *
mach_home_dse_analyze(const nyr_mach_func_t *mach,
                      const ny_native_target_info_t *target, const char *symbol,
                      bool tag_return, bool bare_syscalls, size_t *limit_out,
                      nyr_x64_mach_enc_t *reuse, bool *reused) {
  static atomic_int enabled = ATOMIC_VAR_INIT(-1);
  int enabled_value = atomic_load_explicit(&enabled, memory_order_acquire);
  if (enabled_value < 0) {
    const char *v = getenv("NYTRIX_NO_HOME_DSE");
    int initial = !(v && v[0] && v[0] != '0');
    int unknown = -1;
    atomic_compare_exchange_strong_explicit(&enabled, &unknown, initial,
                                            memory_order_release,
                                            memory_order_relaxed);
    enabled_value = atomic_load_explicit(&enabled, memory_order_acquire);
  }
  if (!enabled_value || !mach->vreg_len)
    return NULL;
  size_t limit = (mach->vreg_len + mach->frame_slot_len + 64) * 40;
  if (limit > (size_t)1 << 20)
    limit = (size_t)1 << 20;
  bool has_scalar_home = false;
  for (size_t i = 0; i < mach->vreg_len; ++i) {
    if (mach_vreg_home_size(mach, i) == 8) {
      has_scalar_home = true;
      break;
    }
  }
  char err[32];
  nyr_x64_mach_enc_t probe = {0};
  probe.mach = mach;
  probe.target = target;
  probe.bare_syscalls = bare_syscalls;
  probe.suppress_div_stats = true;
  probe.suppress_stats = true;
  probe.err = err;
  probe.err_len = sizeof(err);
  probe.home_limit = limit;
  probe.home_store_counts = calloc(limit, sizeof(uint32_t));
  uint32_t *refs = NULL;
  unsigned char *dead = NULL;
  bool any = false;
  bool probe_ok = false;
  if (probe.home_store_counts &&
      mach_encode_function(&probe, symbol, tag_return) &&
      probe.home_offs_snapshot) {
    probe_ok = true;
    if (has_scalar_home && probe.home_store_seen) {
      refs = calloc(limit, sizeof(uint32_t));
      dead = calloc(limit, 1);
      if (!refs || !dead) {
        probe_ok = false;
      } else {
        mach_home_count_refs(probe.code.data, probe.code.len, refs, limit);
        for (size_t i = 0; i < mach->vreg_len; ++i) {
          if (mach_vreg_home_size(mach, i) != 8)
            continue;
          int off = probe.home_offs_snapshot[i];
          if (off >= 0 || (size_t)(-(int64_t)off) >= limit)
            continue;
          size_t index = (size_t)(-(int64_t)off);
          if (probe.home_store_counts[index] > 0 &&
              probe.home_store_counts[index] == refs[index]) {
            dead[index] = 1;
            any = true;
          }
        }
      }
    }
  }
  free(probe.home_store_counts);
  free(probe.home_offs_snapshot);
  probe.home_store_counts = NULL;
  probe.home_offs_snapshot = NULL;
  free(refs);
  /*
   * With no dead home the real encoding would reproduce this one exactly.
   */
  if (probe_ok && !any && reuse && reused) {
    for (unsigned long long i = 0; i < probe.suppressed_div_magic; ++i)
      nyr_native_mach_div_record(true);
    for (unsigned long long i = 0; i < probe.suppressed_div_idiv; ++i)
      nyr_native_mach_div_record(false);
    probe.suppress_div_stats = false;
    probe.suppress_stats = false;
    probe.home_limit = 0;
    *reuse = probe;
    *reused = true;
    free(dead);
    return NULL;
  }
  mach_enc_dispose(&probe);
  if (!any) {
    free(dead);
    return NULL;
  }
  *limit_out = limit;
  return dead;
}
