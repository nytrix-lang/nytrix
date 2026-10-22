#ifndef NYR_TARGET_H
#define NYR_TARGET_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
  NY_NATIVE_BACKEND_LLVM = 0,
  NY_NATIVE_BACKEND_X86_64 = 1,
  NY_NATIVE_BACKEND_X86 = 2,
  NY_NATIVE_BACKEND_AARCH64 = 3,
  NY_NATIVE_BACKEND_AMDGPU = 4,
  NY_NATIVE_BACKEND_ARM = 5,
  NY_NATIVE_BACKEND_AVR = 6,
  NY_NATIVE_BACKEND_BPF = 7,
  NY_NATIVE_BACKEND_MIPS = 8,
  NY_NATIVE_BACKEND_POWERPC = 9,
  NY_NATIVE_BACKEND_RISCV = 10,
  NY_NATIVE_BACKEND_WASM = 11,
} nyr_backend_t;

typedef enum {
  NY_NATIVE_ABI_AUTO = 0,
  NY_NATIVE_ABI_SYSV = 1,
  NY_NATIVE_ABI_WIN64 = 2,
  NY_NATIVE_ABI_AAPCS = 3,
} nyr_abi_t;

typedef struct {
  nyr_backend_t backend;
  nyr_abi_t abi;
  const char *target_triple;
  bool size_optimized;
} nyr_target_request_t;

static inline nyr_target_request_t
nyr_target_request_make(nyr_backend_t backend, nyr_abi_t abi,
                        const char *target_triple, bool size_optimized) {
  return (nyr_target_request_t){backend, abi, target_triple, size_optimized};
}

typedef enum {
  NY_NATIVE_TARGET_UNKNOWN = 0,
  NY_NATIVE_TARGET_X86_64,
  NY_NATIVE_TARGET_X86,
  NY_NATIVE_TARGET_AARCH64,
  NY_NATIVE_TARGET_AMDGPU,
  NY_NATIVE_TARGET_ARM,
  NY_NATIVE_TARGET_AVR,
  NY_NATIVE_TARGET_BPF,
  NY_NATIVE_TARGET_MIPS,
  NY_NATIVE_TARGET_POWERPC,
  NY_NATIVE_TARGET_RISCV,
  NY_NATIVE_TARGET_WASM,
} nyr_backend_target_t;

typedef enum {
  NY_NATIVE_CAP_NIR_ASM = 1u << 0,
  NY_NATIVE_CAP_AST_FALLBACK = 1u << 1,
  NY_NATIVE_CAP_ASM_OBJECT = 1u << 2,
  NY_NATIVE_CAP_NIR_VM = 1u << 3,
  NY_NATIVE_CAP_ELF_OBJECT = 1u << 4,
  NY_NATIVE_CAP_COFF_OBJECT = 1u << 5,
  NY_NATIVE_CAP_MACHO_OBJECT = 1u << 6,
  NY_NATIVE_CAP_LIVE_JIT = 1u << 7,
  NY_NATIVE_CAP_MACH_TRAP = 1u << 8,
  NY_NATIVE_CAP_MACH_BYTE = 1u << 9,
  NY_NATIVE_CAP_MACH_SDIV_POW2 = 1u << 10,
  NY_NATIVE_CAP_MACH_SCALED_LEA = 1u << 11,
  NY_NATIVE_CAP_MACH_FMA = 1u << 12,
  NY_NATIVE_CAP_MACH_AVX = 1u << 13
} nyr_target_cap_t;

typedef struct nyr_target_info_impl_t {
  nyr_backend_target_t target;
  nyr_abi_t abi;
  const char *target_name;
  const char *abi_name;
  const char *object_format;
  const char *symbol_prefix;
  const char *float_abi_name;
  size_t pointer_bits;
  const char *gp_arg_regs[8];
  size_t gp_arg_reg_count;
  const char *fp_arg_regs[8];
  size_t fp_arg_reg_count;
  size_t shadow_space_bytes;
  size_t stack_align;
  unsigned caps;
  bool red_zone;
  bool outline_machine_functions;
} ny_native_target_info_t;

/* Resolve a target request to the backend's ABI/layout/capability record.
 * `nyr_target_is_host_executable` checks whether the requested target can run
 * on the current host. Both helpers are implemented by the Nyr target layer. */
bool nyr_target_info_init(ny_native_target_info_t *info,
                          const nyr_target_request_t *request);
bool nyr_target_is_host_executable(const nyr_target_request_t *request);

#endif
