/*
 * Native emit: converts NYIR to the target-specific machine-form
 * representation, dispatching to per-backend encoder pipelines.
 */
#include "base/common.h"
#include "base/debug.h"
#include "base/time.h"
#include "base/trace.h"
#include "base/util.h"
#include "code/backend/internal.h"
#include "code/backend/ir/module.h"
#include "code/backend/object/internal.h"
#include "code/lang/pipeline/build.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Native emission orchestration: target dispatch, assembly output, internal
 * object selection, and the explicit external-assembler fallback.
 */
size_t nyr_local_count(const nyr_func_t *f) {
  int64_t max_slot = -1;
  for (size_t i = 0; f && i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL ||
         in->op == NYIR_ADDR_LOCAL) &&
        in->imm > max_slot)
      max_slot = in->imm;
  }
  size_t inferred = max_slot >= 0 ? (size_t)max_slot + 1 : 0;
  return f && f->param_count > inferred ? f->param_count : inferred;
}

bool nyr_ensure_parent_dir_for_path(const char *path) {
  if (!path || !*path)
    return true;
  char tmp[4096];
  snprintf(tmp, sizeof(tmp), "%s", path);
  char *slash = strrchr(tmp, '/');
#ifdef _WIN32
  char *bslash = strrchr(tmp, '\\');
  if (!slash || (bslash && bslash > slash))
    slash = bslash;
#endif
  if (!slash || slash == tmp)
    return true;
  *slash = '\0';
  ny_ensure_dir_recursive(tmp);
  return true;
}

#include "code/backend/ir/interface.h"
static const nyr_backend_emitter_t g_backend_emitters[] = {
    {"x86-64", NY_NATIVE_TARGET_X86_64, nyr_x86_64_emit_ir,
     nyr_x86_64_emit_mach_scalar},
    {"aarch64", NY_NATIVE_TARGET_AARCH64, nyr_aarch64_emit_ir, NULL},
    {"i386", NY_NATIVE_TARGET_X86, nyr_i386_emit_ir, NULL},
    {"arm", NY_NATIVE_TARGET_ARM, nyr_arm_emit_ir, NULL},
    {"riscv", NY_NATIVE_TARGET_RISCV, nyr_riscv_emit_ir, NULL},
    {"bpf", NY_NATIVE_TARGET_BPF, nyr_bpf_emit_ir, NULL},
    {"mips", NY_NATIVE_TARGET_MIPS, nyr_mips_emit_ir, NULL},
    {"powerpc", NY_NATIVE_TARGET_POWERPC, nyr_powerpc_emit_ir, NULL},
    {"avr", NY_NATIVE_TARGET_AVR, nyr_avr_emit_ir, NULL},
    {"wasm", NY_NATIVE_TARGET_WASM, nyr_wasm_emit_ir, NULL},
};
#define NUM_BACKEND_EMITTERS                                                   \
  (sizeof(g_backend_emitters) / sizeof(g_backend_emitters[0]))

const nyr_backend_emitter_t *
nyr_backend_find_emitter(nyr_backend_target_t target) {
  for (size_t i = 0; i < NUM_BACKEND_EMITTERS; i++) {
    if (g_backend_emitters[i].target == target)
      return &g_backend_emitters[i];
  }
  return NULL;
}

static bool nyr_nir_has_vec4_f64(const nyr_func_t *nyir) {
  if (!nyir)
    return false;
  for (size_t i = 0; i < nyir->len; ++i) {
    switch (nyir->data[i].op) {
    case NYIR_VEC4_LOAD_F64:
    case NYIR_VEC4_STORE_F64:
    case NYIR_VEC4_ADD_F64:
    case NYIR_VEC4_SUB_F64:
    case NYIR_VEC4_MUL_F64:
    case NYIR_VEC4_DIV_F64:
    case NYIR_VEC4_FMA_F64:
    case NYIR_VEC4_SET1_F64:
    case NYIR_VEC4_SHUFFLE_F64:
    case NYIR_VEC4_REDUCE_ADD_F64:
      return true;
    default:
      break;
    }
  }
  return false;
}
bool nyr_emit_func(nyr_writer_t *w, const ny_native_target_info_t *target,
                   const nyr_func_t *nyir, const char *label, bool tag_return,
                   char *err, size_t err_len) {
  if (!target)
    return false;

  const nyr_backend_emitter_t *emitter =
      nyr_backend_find_emitter(target->target);

  /*
   * The scalar machine form path is intentionally tried first when supported.
   * It writes into a temporary buffer, so an unsupported shape or allocation
   * failure cannot leave a partial function before the established NYIR
   * fallback is selected.
   * The current x86 machine form carries only two f64 lanes per value.
   */
  bool use_machine = !(target->target == NY_NATIVE_TARGET_X86_64 &&
                       nyr_nir_has_vec4_f64(nyir));
  if (getenv("NY_FORCE_NIR"))
    use_machine = false;
  if (emitter && emitter->emit_mach_scalar && use_machine) {
    nyr_mach_func_t machine = {0};
    char machine_err[256] = {0};
    bool machine_lowered = nyr_mach_lower_nir(nyir, &machine, target->caps,
                                              machine_err, sizeof(machine_err));
    if (machine_lowered && getenv("NY_DUMP_MACH")) {
      fprintf(stderr, "native machine form before scalar emission for %s:\n",
              label ? label : "<entry>");
      nyr_mach_dump(&machine, stderr);
    }
    if (machine_lowered) {
      nyr_writer_t machine_out = {0};
      if (emitter->emit_mach_scalar(&machine_out, target, &machine, label,
                                    tag_return, machine_err,
                                    sizeof(machine_err))) {
        bool appended = nyr_put(w, machine_out.data ? machine_out.data : "");
        free(machine_out.data);
        nyr_mach_func_free(&machine);
        if (!appended)
          nyr_set_error(err, err_len,
                        "native machine form emitter: out of memory");
        return appended;
      }
      free(machine_out.data);
    }
    if (getenv("NY_DUMP_MACH"))
      fprintf(stderr, "native machine form %s for %s: %s\n",
              machine_lowered ? "emission failed" : "lowering failed",
              label ? label : "<entry>",
              machine_err[0] ? machine_err : "<no diagnostic>");
    nyr_mach_func_free(&machine);
  }

  if (emitter && emitter->emit_nir) {
    nyr_phi_elim((nyr_func_t *)nyir);
    bool ok =
        emitter->emit_nir(w, target, nyir, label, tag_return, err, err_len);
    if (!ok && getenv("NY_DUMP_MACH"))
      fprintf(stderr, "native NIR emitter failed for %s: %s\n",
              label ? label : "<entry>", err && err[0] ? err : "<no detail>");
    return ok;
  }

  nyr_set_error(err, err_len, "native backend target '%s' has no NYIR emitter",
                target->target_name ? target->target_name : "unknown");
  return false;
}
