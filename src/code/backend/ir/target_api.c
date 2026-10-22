#include "code/backend/ir/api_internal.h"
#include <nyr/nyr.h>
#include <stdio.h>
#include <string.h>

void nyr_api_error(char *err, size_t err_len, const char *message) {
  if (err && err_len)
    snprintf(err, err_len, "%s", message);
}

void nyr_api_clear_error(char *err, size_t err_len) {
  if (err && err_len)
    err[0] = '\0';
}

static unsigned nyr_api_capabilities(const ny_native_target_info_t *info) {
  unsigned capabilities = NYR_CAP_VM;
  if (info->pointer_bits != 64 || !info->object_format)
    return capabilities;
  if (strcmp(info->object_format, "elf") == 0 &&
      (info->target == NY_NATIVE_TARGET_X86_64 ||
       info->target == NY_NATIVE_TARGET_AARCH64))
    capabilities |= NYR_CAP_ELF_OBJECT;
  else if (info->target == NY_NATIVE_TARGET_X86_64 &&
           strcmp(info->object_format, "coff") == 0)
    capabilities |= NYR_CAP_COFF_OBJECT;
  else if (info->target == NY_NATIVE_TARGET_X86_64 &&
           strcmp(info->object_format, "macho") == 0)
    capabilities |= NYR_CAP_MACHO_OBJECT;
  return capabilities;
}

static bool nyr_target_arch_is(const char *triple, const char *arch) {
  size_t len = strlen(arch);
  return strncmp(triple, arch, len) == 0 &&
         (triple[len] == '\0' || triple[len] == '-');
}

static size_t nyr_target_arch_len(const char *triple) {
  return triple ? strcspn(triple, "-") : 0;
}

static bool nyr_target_triple_is_well_formed(const char *triple) {
  if (!triple || !*triple)
    return false;
  size_t component_len = 0;
  for (const unsigned char *p = (const unsigned char *)triple; *p; ++p) {
    if (*p == '-') {
      if (!component_len)
        return false;
      component_len = 0;
    } else if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '_' || *p == '.' ||
               *p == '+') {
      ++component_len;
    } else {
      return false;
    }
  }
  return component_len != 0;
}

static bool nyr_target_arch_is_any(const char *triple,
                                   const char *const *architectures,
                                   size_t architecture_count) {
  for (size_t i = 0; i < architecture_count; ++i)
    if (nyr_target_arch_is(triple, architectures[i]))
      return true;
  return false;
}

static bool nyr_target_arch_is_arm_variant(const char *triple) {
  size_t len = nyr_target_arch_len(triple);
  if (len == 3 && memcmp(triple, "arm", 3) == 0)
    return true;
  if (len < 5 || memcmp(triple, "armv", 4) != 0 || triple[4] < '0' ||
      triple[4] > '9')
    return false;
  for (size_t i = 4; i < len; ++i) {
    char c = triple[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '.' ||
          c == '_'))
      return false;
  }
  return true;
}

static bool nyr_target_arch_is_riscv_variant(const char *triple) {
  size_t len = nyr_target_arch_len(triple);
  size_t base_len = 0;
  if (len >= 7 && memcmp(triple, "riscv32", 7) == 0)
    base_len = 7;
  else if (len >= 7 && memcmp(triple, "riscv64", 7) == 0)
    base_len = 7;
  else
    return false;
  for (size_t i = base_len; i < len; ++i) {
    char c = triple[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '_'))
      return false;
  }
  return true;
}

static bool nyr_options_for_host(ny_options *opt) {
  if (!opt)
    return false;
  memset(opt, 0, sizeof(*opt));
#if defined(__x86_64__) || defined(_M_X64)
  opt->native_backend = NY_NATIVE_BACKEND_X86_64;
  const char *arch = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  opt->native_backend = NY_NATIVE_BACKEND_AARCH64;
  const char *arch = "aarch64";
#else
  return false;
#endif
  opt->native_abi = NY_NATIVE_ABI_AUTO;
#if defined(_WIN32)
  opt->host_triple =
      arch[0] == 'x' ? "x86_64-windows-msvc" : "aarch64-windows-msvc";
#elif defined(__APPLE__)
  opt->host_triple =
      arch[0] == 'x' ? "x86_64-apple-darwin" : "aarch64-apple-darwin";
#else
  opt->host_triple =
      arch[0] == 'x' ? "x86_64-unknown-linux-gnu" : "aarch64-unknown-linux-gnu";
#endif
  return true;
}

bool nyr_target_info_for_triple(const char *triple,
                                ny_native_target_info_t *info, char *err,
                                size_t err_len) {
  if (!info) {
    nyr_api_error(err, err_len, "target output must not be null");
    return false;
  }
  if (!triple || !*triple) {
    nyr_api_error(err, err_len, "target triple is required");
    return false;
  }
  if (!nyr_target_triple_is_well_formed(triple)) {
    nyr_api_error(err, err_len, "target triple is malformed");
    return false;
  }
  nyr_backend_t backend;
  if (nyr_target_arch_is(triple, "aarch64") ||
      nyr_target_arch_is(triple, "arm64") ||
      nyr_target_arch_is(triple, "arm64e"))
    backend = NY_NATIVE_BACKEND_AARCH64;
  else if (nyr_target_arch_is(triple, "x86_64") ||
           nyr_target_arch_is(triple, "x86_64h") ||
           nyr_target_arch_is(triple, "amd64"))
    backend = NY_NATIVE_BACKEND_X86_64;
  else if (nyr_target_arch_is(triple, "i386") ||
           nyr_target_arch_is(triple, "i486") ||
           nyr_target_arch_is(triple, "i586") ||
           nyr_target_arch_is(triple, "i686") ||
           nyr_target_arch_is(triple, "x86"))
    backend = NY_NATIVE_BACKEND_X86;
  else if (nyr_target_arch_is_arm_variant(triple))
    backend = NY_NATIVE_BACKEND_ARM;
  else if (nyr_target_arch_is_riscv_variant(triple))
    backend = NY_NATIVE_BACKEND_RISCV;
  else if (nyr_target_arch_is(triple, "wasm32") ||
           nyr_target_arch_is(triple, "wasm64"))
    backend = NY_NATIVE_BACKEND_WASM;
  else if (nyr_target_arch_is(triple, "avr") ||
           nyr_target_arch_is(triple, "avr2") ||
           nyr_target_arch_is(triple, "avr25") ||
           nyr_target_arch_is(triple, "avr3") ||
           nyr_target_arch_is(triple, "avr31") ||
           nyr_target_arch_is(triple, "avr35") ||
           nyr_target_arch_is(triple, "avr4") ||
           nyr_target_arch_is(triple, "avr5") ||
           nyr_target_arch_is(triple, "avr51") ||
           nyr_target_arch_is(triple, "avr6") ||
           nyr_target_arch_is(triple, "avrtiny") ||
           nyr_target_arch_is(triple, "avrxmega1") ||
           nyr_target_arch_is(triple, "avrxmega2") ||
           nyr_target_arch_is(triple, "avrxmega3") ||
           nyr_target_arch_is(triple, "avrxmega4") ||
           nyr_target_arch_is(triple, "avrxmega5") ||
           nyr_target_arch_is(triple, "avrxmega6") ||
           nyr_target_arch_is(triple, "avrxmega7"))
    backend = NY_NATIVE_BACKEND_AVR;
  else if (nyr_target_arch_is(triple, "bpf") ||
           nyr_target_arch_is(triple, "bpfel") ||
           nyr_target_arch_is(triple, "bpfeb"))
    backend = NY_NATIVE_BACKEND_BPF;
  else if (nyr_target_arch_is_any(
               triple,
               (const char *const[]){
                   "mips", "mipsel", "mips32", "mips32el", "mips64", "mips64el",
                   "mipsisa32r2", "mipsisa32r2el", "mipsisa32r6",
                   "mipsisa32r6el", "mipsisa64r2", "mipsisa64r2el",
                   "mipsisa64r6", "mipsisa64r6el"},
               14))
    backend = NY_NATIVE_BACKEND_MIPS;
  else if (nyr_target_arch_is_any(
               triple,
               (const char *const[]){"powerpc", "powerpcle", "powerpc64",
                                     "powerpc64le", "ppc", "ppcle", "ppc64",
                                     "ppc64le"},
               8))
    backend = NY_NATIVE_BACKEND_POWERPC;
  else if (nyr_target_arch_is(triple, "amdgcn"))
    backend = NY_NATIVE_BACKEND_AMDGPU;
  else {
    nyr_api_error(err, err_len, "target architecture is unsupported");
    return false;
  }
  const nyr_target_request_t request =
      nyr_target_request_make(backend, NY_NATIVE_ABI_AUTO, triple, false);
  if (!nyr_target_info_init(info, &request)) {
    nyr_api_error(err, err_len, "target triple is unsupported");
    return false;
  }
  if (backend == NY_NATIVE_BACKEND_RISCV) {
    if (nyr_target_arch_len(triple) >= 7 && memcmp(triple, "riscv32", 7) == 0)
      info->pointer_bits = 32;
    else if (nyr_target_arch_len(triple) >= 7 &&
             memcmp(triple, "riscv64", 7) == 0)
      info->pointer_bits = 64;
  } else if (backend == NY_NATIVE_BACKEND_WASM) {
    if (nyr_target_arch_is(triple, "wasm32"))
      info->pointer_bits = 32;
    else if (nyr_target_arch_is(triple, "wasm64"))
      info->pointer_bits = 64;
  } else if (backend == NY_NATIVE_BACKEND_MIPS) {
    info->pointer_bits =
        nyr_target_arch_is_any(
            triple,
            (const char *const[]){"mips64", "mips64el", "mipsisa64r2",
                                  "mipsisa64r2el", "mipsisa64r6",
                                  "mipsisa64r6el"},
            6)
            ? 64
            : 32;
  } else if (backend == NY_NATIVE_BACKEND_POWERPC) {
    info->pointer_bits =
        nyr_target_arch_is_any(triple,
                               (const char *const[]){"powerpc64", "powerpc64le",
                                                     "ppc64", "ppc64le"},
                               4)
            ? 64
            : 32;
  }
  return true;
}

static void nyr_api_target_fill(nyr_target_t *out,
                                const ny_native_target_info_t *info) {
  *out = (nyr_target_t){.target = info->target_name,
                        .object_format = info->object_format,
                        .pointer_bits = (unsigned)info->pointer_bits,
                        .capabilities = nyr_api_capabilities(info)};
}

nyr_version_t nyr_version(void) {
  return (nyr_version_t){NYR_API_MAJOR, NYR_API_MINOR};
}

bool nyr_host_target(nyr_target_t *target, char *err, size_t err_len) {
  if (!target) {
    nyr_api_error(err, err_len, "target output must not be null");
    return false;
  }
  ny_options opt;
  if (!nyr_options_for_host(&opt)) {
    nyr_api_error(err, err_len, "host target discovery is unsupported");
    return false;
  }
  ny_native_target_info_t info;
  const nyr_target_request_t request = nyr_target_request_make(
      opt.native_backend, opt.native_abi, opt.host_triple, opt.opt_size);
  if (!nyr_target_info_init(&info, &request)) {
    nyr_api_error(err, err_len, "host target discovery failed");
    return false;
  }
  nyr_api_target_fill(target, &info);
  nyr_api_clear_error(err, err_len);
  return true;
}

bool nyr_target_query(const char *target_triple, nyr_target_t *target,
                      char *err, size_t err_len) {
  if (!target) {
    nyr_api_error(err, err_len, "target output must not be null");
    return false;
  }
  ny_native_target_info_t info;
  if (!nyr_target_info_for_triple(target_triple, &info, err, err_len))
    return false;
  nyr_api_target_fill(target, &info);
  nyr_api_clear_error(err, err_len);
  return true;
}

bool nyr_api_target_has(const ny_native_target_info_t *target,
                        unsigned capability) {
  return target && (target->caps & capability) != 0;
}
