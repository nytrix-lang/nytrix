/*
 * Native target selection: resolves the target triple, ABI, and
 * CPU features for the selected native backend and optimization tier.
 */
#include "code/backend/target.h"
#include "base/debug.h"

#include <stdlib.h>
#include <string.h>

static bool nyr_triple_is_windows(const char *triple) {
  return triple && (strstr(triple, "windows") || strstr(triple, "mingw") ||
                    strstr(triple, "msvc") || strstr(triple, "win32"));
}

static bool nyr_triple_is_macho(const char *triple) {
  return triple && (strstr(triple, "apple") || strstr(triple, "darwin") ||
                    strstr(triple, "macos"));
}

static const char *nyr_arm_float_abi_name(void) {
  const char *abi = getenv("NYTRIX_ARM_FLOAT_ABI");
  if (abi && *abi) {
    if (strcmp(abi, "hard") == 0 || strcmp(abi, "softfp") == 0 ||
        strcmp(abi, "soft") == 0)
      return abi;
  }
  return "softfp";
}

static const char *nyr_abi_label(nyr_abi_t abi) {
  switch (abi) {
  case NY_NATIVE_ABI_SYSV:
    return "sysv";
  case NY_NATIVE_ABI_WIN64:
    return "win64";
  case NY_NATIVE_ABI_AAPCS:
    return "aapcs";
  default:
    return "auto";
  }
}

/*
 * Machine-form fused multiply-add support follows the executing CPU, not the
 * triple: the machine encoders emit FMA3/NEON fused forms for the host that
 * runs them. Targets without a fused path leave the bit clear so vector f64
 * FMA fails closed instead of silently changing rounding.
 */
static unsigned nyr_target_mach_fma_cap(nyr_backend_target_t target) {
#if defined(__x86_64__) || defined(__i386__)
  (void)target;
#if defined(__GNUC__) || defined(__clang__)
  __builtin_cpu_init();
  if (__builtin_cpu_supports("avx") && __builtin_cpu_supports("fma"))
    return NY_NATIVE_CAP_MACH_FMA;
#endif
  return 0;
#elif defined(__aarch64__)
  (void)target;
  /*
   * NEON fmla is always fused.
   */
  return NY_NATIVE_CAP_MACH_FMA;
#else
  (void)target;
  return 0;
#endif
}

static unsigned nyr_target_mach_avx_cap(nyr_backend_target_t target) {
#if defined(__x86_64__) || defined(__i386__)
  (void)target;
#if defined(__GNUC__) || defined(__clang__)
  __builtin_cpu_init();
  if (__builtin_cpu_supports("avx"))
    return NY_NATIVE_CAP_MACH_AVX;
#endif
  return 0;
#else
  (void)target;
  return 0;
#endif
}

static void nyr_target_trace(const nyr_target_request_t *request,
                             const ny_native_target_info_t *info,
                             const char *decision) {
  if (!ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    return;
  ny_dbg_heuristic_event(
      "NATIVE_TARGET",
      "%s backend=%d target=%s triple=%s requested-abi=%s abi=%s object=%s "
      "ptr=%zu gp-args=%zu "
      "fp-args=%zu stack-align=%zu shadow=%zu red-zone=%s caps=0x%08x",
      decision, request ? (int)request->backend : -1,
      info && info->target_name ? info->target_name : "unknown",
      request && request->target_triple ? request->target_triple : "(unset)",
      request ? nyr_abi_label(request->abi) : "auto",
      info && info->abi_name ? info->abi_name : "unresolved",
      info && info->object_format ? info->object_format : "unresolved",
      info ? info->pointer_bits : 0, info ? info->gp_arg_reg_count : 0,
      info ? info->fp_arg_reg_count : 0, info ? info->stack_align : 0,
      info ? info->shadow_space_bytes : 0,
      info && info->red_zone ? "yes" : "no", info ? info->caps : 0);
}

static bool nyr_backend_target(nyr_backend_t backend,
                               nyr_backend_target_t *target,
                               const char **name) {
  if (!target || !name)
    return false;
  switch (backend) {
  case NY_NATIVE_BACKEND_X86_64:
    *target = NY_NATIVE_TARGET_X86_64;
    *name = "x86_64";
    return true;
  case NY_NATIVE_BACKEND_X86:
    *target = NY_NATIVE_TARGET_X86;
    *name = "x86";
    return true;
  case NY_NATIVE_BACKEND_AARCH64:
    *target = NY_NATIVE_TARGET_AARCH64;
    *name = "aarch64";
    return true;
  case NY_NATIVE_BACKEND_AMDGPU:
    *target = NY_NATIVE_TARGET_AMDGPU;
    *name = "amdgpu";
    return true;
  case NY_NATIVE_BACKEND_ARM:
    *target = NY_NATIVE_TARGET_ARM;
    *name = "arm";
    return true;
  case NY_NATIVE_BACKEND_AVR:
    *target = NY_NATIVE_TARGET_AVR;
    *name = "avr";
    return true;
  case NY_NATIVE_BACKEND_BPF:
    *target = NY_NATIVE_TARGET_BPF;
    *name = "bpf";
    return true;
  case NY_NATIVE_BACKEND_MIPS:
    *target = NY_NATIVE_TARGET_MIPS;
    *name = "mips";
    return true;
  case NY_NATIVE_BACKEND_POWERPC:
    *target = NY_NATIVE_TARGET_POWERPC;
    *name = "powerpc";
    return true;
  case NY_NATIVE_BACKEND_RISCV:
    *target = NY_NATIVE_TARGET_RISCV;
    *name = "riscv";
    return true;
  case NY_NATIVE_BACKEND_WASM:
    *target = NY_NATIVE_TARGET_WASM;
    *name = "wasm";
    return true;
  default:
    *target = NY_NATIVE_TARGET_UNKNOWN;
    *name = "unknown";
    return false;
  }
}

bool nyr_target_is_host_executable(const nyr_target_request_t *request) {
  if (!request)
    return false;
  nyr_backend_target_t target = NY_NATIVE_TARGET_UNKNOWN;
  const char *name = "unknown";
  if (!nyr_backend_target(request->backend, &target, &name))
    return false;
#if defined(__x86_64__) || defined(_M_X64)
  return target == NY_NATIVE_TARGET_X86_64 || target == NY_NATIVE_TARGET_X86;
#elif defined(__aarch64__) || defined(_M_ARM64)
  return target == NY_NATIVE_TARGET_AARCH64;
#else
  return false;
#endif
}

bool nyr_target_info_init(ny_native_target_info_t *info,
                          const nyr_target_request_t *request) {
  if (!info || !request)
    return false;
  memset(info, 0, sizeof(*info));
  info->outline_machine_functions = request->size_optimized;
  const char *triple = request->target_triple;
  nyr_backend_t backend = request->backend;
  if (backend == NY_NATIVE_BACKEND_LLVM) {
    nyr_target_trace(request, NULL,
                     "LLVM selected; custom native target model skipped");
    return false;
  }
  const char *target_name = "unknown";
  if (!nyr_backend_target(backend, &info->target, &target_name)) {
    info->target = NY_NATIVE_TARGET_UNKNOWN;
    target_name = "unknown";
  }
  info->target_name = target_name;

  info->abi = request->abi;
  if (info->abi == NY_NATIVE_ABI_AUTO) {
    if (info->target == NY_NATIVE_TARGET_ARM ||
        info->target == NY_NATIVE_TARGET_AARCH64)
      info->abi = NY_NATIVE_ABI_AAPCS;
    else
      info->abi = nyr_triple_is_windows(triple) ? NY_NATIVE_ABI_WIN64
                                                : NY_NATIVE_ABI_SYSV;
  } else if (info->abi == NY_NATIVE_ABI_AAPCS &&
             info->target != NY_NATIVE_TARGET_ARM &&
             info->target != NY_NATIVE_TARGET_AARCH64) {
    info->abi = nyr_triple_is_windows(triple) ? NY_NATIVE_ABI_WIN64
                                              : NY_NATIVE_ABI_SYSV;
  }
  info->abi_name = nyr_abi_label(info->abi);
  info->object_format = nyr_triple_is_macho(triple)     ? "macho"
                        : nyr_triple_is_windows(triple) ? "coff"
                                                        : "elf";
  info->symbol_prefix = strcmp(info->object_format, "macho") == 0 ? "_" : "";
  info->stack_align = 16;
  info->pointer_bits = 64;
  info->float_abi_name = "";
  if (info->target == NY_NATIVE_TARGET_X86_64 &&
      info->abi == NY_NATIVE_ABI_WIN64) {
    static const char *win64_regs[] = {"%rcx", "%rdx", "%r8", "%r9"};
    static const char *win64_fp_regs[] = {"%xmm0", "%xmm1", "%xmm2", "%xmm3"};
    for (size_t i = 0; i < 4; i++)
      info->gp_arg_regs[i] = win64_regs[i];
    for (size_t i = 0; i < 4; i++)
      info->fp_arg_regs[i] = win64_fp_regs[i];
    info->gp_arg_reg_count = 4;
    info->fp_arg_reg_count = 4;
    info->shadow_space_bytes = 32;
    info->red_zone = false;
    info->caps = NY_NATIVE_CAP_NIR_ASM | NY_NATIVE_CAP_AST_FALLBACK |
                 NY_NATIVE_CAP_ASM_OBJECT | NY_NATIVE_CAP_NIR_VM |
                 NY_NATIVE_CAP_LIVE_JIT | NY_NATIVE_CAP_MACH_BYTE |
                 NY_NATIVE_CAP_MACH_TRAP | NY_NATIVE_CAP_MACH_SDIV_POW2 |
                 NY_NATIVE_CAP_MACH_SCALED_LEA |
                 nyr_target_mach_avx_cap(info->target) |
                 nyr_target_mach_fma_cap(info->target);
    if (strcmp(info->object_format, "elf") == 0)
      info->caps |= NY_NATIVE_CAP_ELF_OBJECT;
    else if (strcmp(info->object_format, "coff") == 0)
      info->caps |= NY_NATIVE_CAP_COFF_OBJECT;
    else if (strcmp(info->object_format, "macho") == 0)
      info->caps |= NY_NATIVE_CAP_MACHO_OBJECT;
  } else if (info->target == NY_NATIVE_TARGET_X86_64) {
    static const char *sysv_regs[] = {"%rdi", "%rsi", "%rdx",
                                      "%rcx", "%r8",  "%r9"};
    static const char *sysv_fp_regs[] = {"%xmm0", "%xmm1", "%xmm2", "%xmm3",
                                         "%xmm4", "%xmm5", "%xmm6", "%xmm7"};
    for (size_t i = 0; i < 6; i++)
      info->gp_arg_regs[i] = sysv_regs[i];
    for (size_t i = 0; i < 8; i++)
      info->fp_arg_regs[i] = sysv_fp_regs[i];
    info->gp_arg_reg_count = 6;
    info->fp_arg_reg_count = 8;
    info->shadow_space_bytes = 0;
    info->red_zone = true;
    info->caps = NY_NATIVE_CAP_NIR_ASM | NY_NATIVE_CAP_AST_FALLBACK |
                 NY_NATIVE_CAP_ASM_OBJECT | NY_NATIVE_CAP_NIR_VM |
                 NY_NATIVE_CAP_LIVE_JIT | NY_NATIVE_CAP_MACH_BYTE |
                 NY_NATIVE_CAP_MACH_TRAP | NY_NATIVE_CAP_MACH_SDIV_POW2 |
                 NY_NATIVE_CAP_MACH_SCALED_LEA |
                 nyr_target_mach_avx_cap(info->target) |
                 nyr_target_mach_fma_cap(info->target);
    if (strcmp(info->object_format, "elf") == 0)
      info->caps |= NY_NATIVE_CAP_ELF_OBJECT;
    else if (strcmp(info->object_format, "coff") == 0)
      info->caps |= NY_NATIVE_CAP_COFF_OBJECT;
    else if (strcmp(info->object_format, "macho") == 0)
      info->caps |= NY_NATIVE_CAP_MACHO_OBJECT;
  } else if (info->target == NY_NATIVE_TARGET_AARCH64) {
    static const char *aarch64_regs[] = {"x0", "x1", "x2", "x3",
                                         "x4", "x5", "x6", "x7"};
    static const char *aarch64_fp_regs[] = {"v0", "v1", "v2", "v3",
                                            "v4", "v5", "v6", "v7"};
    for (size_t i = 0; i < 8; i++)
      info->gp_arg_regs[i] = aarch64_regs[i];
    for (size_t i = 0; i < 8; i++)
      info->fp_arg_regs[i] = aarch64_fp_regs[i];
    info->gp_arg_reg_count = 8;
    info->fp_arg_reg_count = 8;
    info->shadow_space_bytes = 0;
    info->red_zone = false;
    info->pointer_bits = 64;
    info->caps = NY_NATIVE_CAP_NIR_ASM | NY_NATIVE_CAP_NIR_VM |
                 NY_NATIVE_CAP_LIVE_JIT | NY_NATIVE_CAP_MACH_TRAP;
    if (strcmp(info->object_format, "elf") == 0)
      info->caps |= NY_NATIVE_CAP_ASM_OBJECT | NY_NATIVE_CAP_ELF_OBJECT;
  } else if (info->target == NY_NATIVE_TARGET_X86) {
    info->gp_arg_reg_count = 0;
    info->shadow_space_bytes = 0;
    info->red_zone = false;
    info->pointer_bits = 32;
    info->caps =
        NY_NATIVE_CAP_NIR_ASM | NY_NATIVE_CAP_NIR_VM | NY_NATIVE_CAP_ASM_OBJECT;
    if (strcmp(info->object_format, "elf") == 0)
      info->caps |= NY_NATIVE_CAP_ELF_OBJECT;
  } else if (info->target == NY_NATIVE_TARGET_ARM) {
    static const char *aapcs_regs[] = {"r0", "r1", "r2", "r3"};
    info->abi = NY_NATIVE_ABI_AAPCS;
    info->abi_name = nyr_abi_label(info->abi);
    for (size_t i = 0; i < 4; i++)
      info->gp_arg_regs[i] = aapcs_regs[i];
    info->gp_arg_reg_count = 4;
    info->shadow_space_bytes = 0;
    info->stack_align = 8;
    info->red_zone = false;
    info->pointer_bits = 32;
    info->float_abi_name = nyr_arm_float_abi_name();
    info->caps = NY_NATIVE_CAP_NIR_ASM | NY_NATIVE_CAP_NIR_VM;
  } else if (info->target == NY_NATIVE_TARGET_BPF) {
    static const char *bpf_regs[] = {"r1", "r2", "r3", "r4", "r5"};
    for (size_t i = 0; i < 5; i++)
      info->gp_arg_regs[i] = bpf_regs[i];
    info->gp_arg_reg_count = 5;
    info->shadow_space_bytes = 0;
    info->red_zone = false;
    info->pointer_bits = 64;
    info->stack_align = 8;
    info->caps = NY_NATIVE_CAP_NIR_ASM | NY_NATIVE_CAP_NIR_VM;
  } else if (info->target == NY_NATIVE_TARGET_MIPS) {
    static const char *mips_regs[] = {"$a0", "$a1", "$a2", "$a3",
                                      "$a4", "$a5", "$a6", "$a7"};
    for (size_t i = 0; i < 8; i++)
      info->gp_arg_regs[i] = mips_regs[i];
    info->gp_arg_reg_count = 8;
    info->shadow_space_bytes = 0;
    info->red_zone = false;
    info->pointer_bits = 64;
    info->caps = NY_NATIVE_CAP_NIR_ASM | NY_NATIVE_CAP_NIR_VM;
  } else if (info->target == NY_NATIVE_TARGET_POWERPC) {
    static const char *ppc_regs[] = {"r3", "r4", "r5", "r6",
                                     "r7", "r8", "r9", "r10"};
    for (size_t i = 0; i < 8; i++)
      info->gp_arg_regs[i] = ppc_regs[i];
    info->gp_arg_reg_count = 8;
    info->shadow_space_bytes = 0;
    info->red_zone = false;
    info->pointer_bits = 64;
    info->caps = NY_NATIVE_CAP_NIR_ASM | NY_NATIVE_CAP_NIR_VM;
  } else if (info->target == NY_NATIVE_TARGET_AVR) {
    static const char *avr_regs[] = {"r18:r25", "r10:r17"};
    for (size_t i = 0; i < 2; i++)
      info->gp_arg_regs[i] = avr_regs[i];
    info->gp_arg_reg_count = 2;
    info->shadow_space_bytes = 0;
    info->red_zone = false;
    info->pointer_bits = 16;
    info->stack_align = 1;
    info->caps = NY_NATIVE_CAP_NIR_ASM | NY_NATIVE_CAP_NIR_VM;
  } else if (info->target == NY_NATIVE_TARGET_WASM) {
    static const char *wasm_regs[] = {"$a0", "$a1", "$a2", "$a3"};
    for (size_t i = 0; i < 4; i++)
      info->gp_arg_regs[i] = wasm_regs[i];
    info->gp_arg_reg_count = 4;
    info->shadow_space_bytes = 0;
    info->red_zone = false;
    info->pointer_bits = 32;
    info->caps = NY_NATIVE_CAP_NIR_ASM | NY_NATIVE_CAP_NIR_VM;
  } else if (info->target == NY_NATIVE_TARGET_RISCV) {
    static const char *riscv_regs[] = {"a0", "a1", "a2", "a3",
                                       "a4", "a5", "a6", "a7"};
    for (size_t i = 0; i < 8; i++)
      info->gp_arg_regs[i] = riscv_regs[i];
    info->gp_arg_reg_count = 8;
    info->shadow_space_bytes = 0;
    info->red_zone = false;
    info->pointer_bits = 64;
    info->caps = NY_NATIVE_CAP_NIR_ASM | NY_NATIVE_CAP_NIR_VM;
  }
  bool supported = info->target != NY_NATIVE_TARGET_UNKNOWN;
  nyr_target_trace(request, info,
                   supported ? "target model selected"
                             : "no custom target model for backend value");
  return supported;
}
