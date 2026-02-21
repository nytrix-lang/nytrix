#include "llvm.h"
#include "base/common.h"
#include "base/util.h"
#include <llvm-c/Analysis.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <stdio.h>
#include <stdlib.h>

typedef enum {
  NY_ARM_FLOAT_ABI_DEFAULT = 0,
  NY_ARM_FLOAT_ABI_SOFT = 1,
  NY_ARM_FLOAT_ABI_SOFTFP = 2,
  NY_ARM_FLOAT_ABI_HARD = 3,
} ny_arm_float_abi_t;

static ny_arm_float_abi_t parse_arm_float_abi(const char *abi) {
  if (!abi || !*abi)
    return NY_ARM_FLOAT_ABI_DEFAULT;
  if (strcmp(abi, "hard") == 0)
    return NY_ARM_FLOAT_ABI_HARD;
  if (strcmp(abi, "softfp") == 0)
    return NY_ARM_FLOAT_ABI_SOFTFP;
  if (strcmp(abi, "soft") == 0)
    return NY_ARM_FLOAT_ABI_SOFT;
  return NY_ARM_FLOAT_ABI_DEFAULT;
}

static ny_arm_float_abi_t host_arm_float_abi(void) {
  const char *abi = getenv("NYTRIX_ARM_FLOAT_ABI");
  ny_arm_float_abi_t parsed = parse_arm_float_abi(abi);
  if (parsed != NY_ARM_FLOAT_ABI_DEFAULT)
    return parsed;
  const char *env = getenv("NYTRIX_HOST_CFLAGS");
  if (env && *env) {
    if (strstr(env, "-mfloat-abi=hard"))
      return NY_ARM_FLOAT_ABI_HARD;
    if (strstr(env, "-mfloat-abi=softfp"))
      return NY_ARM_FLOAT_ABI_SOFTFP;
    if (strstr(env, "-mfloat-abi=soft"))
      return NY_ARM_FLOAT_ABI_SOFT;
  }
#if defined(__ARM_PCS_VFP)
  return NY_ARM_FLOAT_ABI_HARD;
#endif
#if defined(__arm__) && !defined(__aarch64__)
  if (access("/lib/arm-linux-gnueabihf", F_OK) == 0)
    return NY_ARM_FLOAT_ABI_HARD;
  if (access("/usr/lib/arm-linux-gnueabihf", F_OK) == 0)
    return NY_ARM_FLOAT_ABI_HARD;
  if (access("/lib/ld-linux-armhf.so.3", F_OK) == 0)
    return NY_ARM_FLOAT_ABI_HARD;
  if (access("/usr/lib/ld-linux-armhf.so.3", F_OK) == 0)
    return NY_ARM_FLOAT_ABI_HARD;
  return NY_ARM_FLOAT_ABI_HARD;
#endif
  return NY_ARM_FLOAT_ABI_DEFAULT;
}

static char *normalize_triple(char *triple, bool *needs_free) {
  if (!triple)
    return NULL;
  if (needs_free)
    *needs_free = false;
  if (!strstr(triple, "arm"))
    return triple;
  if (strstr(triple, "aarch64") || strstr(triple, "arm64"))
    return triple;
  ny_arm_float_abi_t abi = host_arm_float_abi();
  if (abi != NY_ARM_FLOAT_ABI_HARD) {
    const char *hf = "gnueabihf";
    const char *hpos = strstr(triple, hf);
    if (!hpos)
      return triple;
    size_t pre = (size_t)(hpos - triple);
    size_t post = strlen(hpos + strlen(hf));
    size_t out_len = pre + strlen("gnueabi") + post + 1;
    char *out = (char *)malloc(out_len);
    if (!out)
      return triple;
    memcpy(out, triple, pre);
    memcpy(out + pre, "gnueabi", strlen("gnueabi"));
    memcpy(out + pre + strlen("gnueabi"), hpos + strlen(hf), post);
    out[out_len - 1] = '\0';
    if (needs_free)
      *needs_free = true;
    return out;
  }
  if (strstr(triple, "gnueabihf"))
    return triple;
  const char *needle = "gnueabi";
  const char *pos = strstr(triple, needle);
  if (!pos)
    goto try_gnu;
  size_t pre = (size_t)(pos - triple);
  size_t post = strlen(pos + strlen(needle));
  size_t out_len = pre + strlen("gnueabihf") + post + 1;
  char *out = (char *)malloc(out_len);
  if (!out)
    return triple;
  memcpy(out, triple, pre);
  memcpy(out + pre, "gnueabihf", strlen("gnueabihf"));
  memcpy(out + pre + strlen("gnueabihf"), pos + strlen(needle), post);
  out[out_len - 1] = '\0';
  if (needs_free)
    *needs_free = true;
  return out;
try_gnu: {
  const char *gnu = "linux-gnu";
  const char *gpos = strstr(triple, gnu);
  if (gpos) {
    size_t pre2 = (size_t)(gpos - triple);
    size_t post2 = strlen(gpos + strlen(gnu));
    size_t out_len2 = pre2 + strlen("linux-gnueabihf") + post2 + 1;
    char *out2 = (char *)malloc(out_len2);
    if (!out2)
      return triple;
    memcpy(out2, triple, pre2);
    memcpy(out2 + pre2, "linux-gnueabihf", strlen("linux-gnueabihf"));
    memcpy(out2 + pre2 + strlen("linux-gnueabihf"), gpos + strlen(gnu), post2);
    out2[out_len2 - 1] = '\0';
    if (needs_free)
      *needs_free = true;
    return out2;
  }
  {
    const char *fallback = "armv7-unknown-linux-gnueabihf";
    size_t out_len3 = strlen(fallback) + 1;
    char *out3 = (char *)malloc(out_len3);
    if (!out3)
      return triple;
    memcpy(out3, fallback, out_len3);
    if (needs_free)
      *needs_free = true;
    return out3;
  }
}
}

static void append_feature(char *buf, size_t *len, size_t cap,
                           const char *feature) {
  size_t fl = strlen(feature);
  if (*len + fl + 1 >= cap)
    return;
  if (*len)
    buf[(*len)++] = ',';
  memcpy(buf + *len, feature, fl);
  *len += fl;
  buf[*len] = '\0';
}

static void derive_host_target(const char *triple, char *cpu, size_t cpu_cap,
                               char *features, size_t feat_cap) {
  bool arm32 = triple && strstr(triple, "arm") && !strstr(triple, "aarch64") &&
               !strstr(triple, "arm64");
  const char *env = getenv("NYTRIX_HOST_CFLAGS");
  bool cpu_set = cpu && *cpu != '\0';
  size_t feat_len = 0;
  int float_abi_hard = 0;
  int float_abi_soft = 0;
  int saw_fpu = 0;
  ny_arm_float_abi_t arm_float_abi = host_arm_float_abi();
  if (!env)
    env = "";
  char *copy = ny_strdup(env);
  if (copy) {
    char *tok = strtok(copy, " \t");
    while (tok) {
      if (strstr(tok, "-mcpu=") == tok) {
        if (cpu && !cpu_set) {
          strncpy(cpu, tok + 6, cpu_cap - 1);
          cpu[cpu_cap - 1] = '\0';
          cpu_set = true;
        }
      } else if (strstr(tok, "-mfpu=") == tok) {
        const char *val = tok + 6;
        saw_fpu = 1;
        if (strstr(val, "vfpv4")) {
          append_feature(features, &feat_len, feat_cap, "+vfp4");
        } else if (strstr(val, "vfpv3")) {
          append_feature(features, &feat_len, feat_cap, "+vfp3");
        } else if (strstr(val, "vfp")) {
          append_feature(features, &feat_len, feat_cap, "+vfp2");
        }
        if (strstr(val, "neon") || strstr(val, "asimd"))
          append_feature(features, &feat_len, feat_cap, "+neon");
      }
      tok = strtok(NULL, " \t");
    }
    free(copy);
  }
  if (arm32) {
    if (arm_float_abi == NY_ARM_FLOAT_ABI_HARD) {
      float_abi_hard = 1;
    } else if (arm_float_abi == NY_ARM_FLOAT_ABI_SOFT ||
               arm_float_abi == NY_ARM_FLOAT_ABI_SOFTFP) {
      float_abi_soft = 1;
    } else if (triple && strstr(triple, "gnueabihf")) {
      float_abi_hard = 1;
    }
  }
  if (arm32) {
    if (float_abi_hard) {
      append_feature(features, &feat_len, feat_cap, "-soft-float");
      if (!saw_fpu)
        append_feature(features, &feat_len, feat_cap, "+vfp2");
    } else if (float_abi_soft) {
      append_feature(features, &feat_len, feat_cap, "+soft-float");
    }
  }
  if (cpu && !cpu_set && !arm32) {
    char *host_cpu = LLVMGetHostCPUName();
    if (host_cpu) {
      strncpy(cpu, host_cpu, cpu_cap - 1);
      cpu[cpu_cap - 1] = '\0';
      LLVMDisposeMessage(host_cpu);
    }
  }
}

static void apply_target_attrs(LLVMModuleRef module, const char *cpu,
                               const char *features) {
  if (!module)
    return;
  if ((!cpu || !*cpu) && (!features || !*features))
    return;
  for (LLVMValueRef fn = LLVMGetFirstFunction(module); fn;
       fn = LLVMGetNextFunction(fn)) {
    if (features && *features)
      LLVMAddTargetDependentFunctionAttr(fn, "target-features", features);
    if (cpu && *cpu)
      LLVMAddTargetDependentFunctionAttr(fn, "target-cpu", cpu);
  }
}

static LLVMCodeModel host_code_model(void) {
  const char *cm = getenv("NYTRIX_LLVM_CODE_MODEL");
  if (cm && *cm) {
    if (strcmp(cm, "small") == 0)
      return LLVMCodeModelSmall;
    if (strcmp(cm, "medium") == 0)
      return LLVMCodeModelMedium;
    if (strcmp(cm, "large") == 0)
      return LLVMCodeModelLarge;
  }
  return LLVMCodeModelDefault;
}

bool ny_llvm_init_native(void) {
  static bool initialized = false;
  if (initialized)
    return true;
#if defined(__arm__) && !defined(__aarch64__)
  if (!getenv("NYTRIX_ARM_FLOAT_ABI")) {
    setenv("NYTRIX_ARM_FLOAT_ABI", "hard", 1);
  }
#endif
  LLVMInitializeNativeTarget();
  LLVMInitializeNativeAsmPrinter();
  LLVMInitializeNativeAsmParser();
  initialized = true;
  return true;
}

void ny_llvm_prepare_module(LLVMModuleRef module) {
  if (!ny_llvm_init_native())
    return;
  char *raw_triple = NULL;
  const char *env_triple = getenv("NYTRIX_HOST_TRIPLE");
  if (env_triple && *env_triple)
    raw_triple = ny_strdup(env_triple);
  if (!raw_triple)
    raw_triple = LLVMGetDefaultTargetTriple();
  bool triple_needs_free = false;
  char *triple = normalize_triple(raw_triple, &triple_needs_free);
  LLVMSetTarget(module, triple);

  // Set data layout from native target machine
  LLVMTargetRef target;
  char *err = NULL;
  if (LLVMGetTargetFromTriple(triple, &target, &err) == 0) {
    char cpu_buf[128] = {0};
    char feat_buf[256] = {0};
    derive_host_target(triple, cpu_buf, sizeof(cpu_buf), feat_buf,
                       sizeof(feat_buf));
    char *host_features = NULL;
    const char *cpu = cpu_buf[0] ? cpu_buf : "";
    const char *features = feat_buf;
    bool arm32 = triple && strstr(triple, "arm") &&
                 !strstr(triple, "aarch64") && !strstr(triple, "arm64");
    if (!feat_buf[0] && !arm32) {
      host_features = LLVMGetHostCPUFeatures();
      if (host_features)
        features = host_features;
    } else if (!feat_buf[0] && arm32) {
      features = "";
    }
    LLVMTargetMachineRef tm = LLVMCreateTargetMachine(
        target, triple, cpu, features ? features : "", LLVMCodeGenLevelDefault,
        LLVMRelocPIC, host_code_model());
    if (tm) {
      LLVMTargetDataRef td = LLVMCreateTargetDataLayout(tm);
      char *layout = LLVMCopyStringRepOfTargetData(td);
      if (layout) {
        LLVMSetDataLayout(module, layout);
        LLVMDisposeMessage(layout);
      }
      LLVMDisposeTargetData(td);
      LLVMDisposeTargetMachine(tm);
    }
    if (host_features)
      LLVMDisposeMessage(host_features);
  }
  if (env_triple && env_triple[0]) {
    if (triple_needs_free)
      free(triple);
    free(raw_triple);
  } else {
    if (triple_needs_free)
      free(triple);
    LLVMDisposeMessage(raw_triple);
  }
}

void ny_llvm_apply_host_attrs(LLVMModuleRef module) {
  if (!module)
    return;
  if (!ny_llvm_init_native())
    return;
  char *raw_triple = NULL;
  const char *env_triple = getenv("NYTRIX_HOST_TRIPLE");
  if (env_triple && *env_triple)
    raw_triple = ny_strdup(env_triple);
  if (!raw_triple)
    raw_triple = LLVMGetDefaultTargetTriple();
  if (!raw_triple)
    return;
  bool triple_needs_free = false;
  char *triple = normalize_triple(raw_triple, &triple_needs_free);
  char cpu_buf[128] = {0};
  char feat_buf[256] = {0};
  derive_host_target(triple, cpu_buf, sizeof(cpu_buf), feat_buf,
                     sizeof(feat_buf));
  char *host_features = NULL;
  const char *cpu = cpu_buf[0] ? cpu_buf : "";
  const char *features = feat_buf;
  bool arm32 = triple && strstr(triple, "arm") && !strstr(triple, "aarch64") &&
               !strstr(triple, "arm64");
  if (!feat_buf[0] && !arm32) {
    host_features = LLVMGetHostCPUFeatures();
    if (host_features)
      features = host_features;
  } else if (!feat_buf[0] && arm32) {
    features = "";
  }
  apply_target_attrs(module, cpu, features);
  if (host_features)
    LLVMDisposeMessage(host_features);
  if (env_triple && env_triple[0]) {
    if (triple_needs_free)
      free(triple);
    free(raw_triple);
  } else {
    if (triple_needs_free)
      free(triple);
    LLVMDisposeMessage(raw_triple);
  }
}

bool ny_llvm_emit_object(LLVMModuleRef module, const char *path) {
  if (!module || !path)
    return false;
  if (!ny_llvm_init_native())
    return false;
  char *raw_triple = NULL;
  const char *env_triple = getenv("NYTRIX_HOST_TRIPLE");
  if (env_triple && *env_triple)
    raw_triple = ny_strdup(env_triple);
  if (!raw_triple)
    raw_triple = LLVMGetDefaultTargetTriple();
  if (!raw_triple)
    return false;
  bool triple_needs_free = false;
  char *triple = normalize_triple(raw_triple, &triple_needs_free);
  LLVMTargetRef target;
  char *err = NULL;
  if (LLVMGetTargetFromTriple(triple, &target, &err)) {
    NY_LOG_ERR("Invalid target triple: %s\n", err);
    LLVMDisposeMessage(err);
    if (triple_needs_free)
      free(triple);
    else
      LLVMDisposeMessage(raw_triple);
    return false;
  }
  char cpu_buf[128] = {0};
  char feat_buf[256] = {0};
  derive_host_target(triple, cpu_buf, sizeof(cpu_buf), feat_buf,
                     sizeof(feat_buf));
  char *host_features = NULL;
  const char *cpu = cpu_buf[0] ? cpu_buf : "";
  const char *features = feat_buf;
  bool arm32 = triple && strstr(triple, "arm") && !strstr(triple, "aarch64") &&
               !strstr(triple, "arm64");
  if (!feat_buf[0] && !arm32) {
    host_features = LLVMGetHostCPUFeatures();
    if (host_features)
      features = host_features;
  } else if (!feat_buf[0] && arm32) {
    features = "";
  }
  apply_target_attrs(module, cpu, features);
  LLVMTargetMachineRef tm = LLVMCreateTargetMachine(
      target, triple, cpu, features ? features : "", LLVMCodeGenLevelDefault,
      LLVMRelocPIC, host_code_model());
  if (!tm) {
    NY_LOG_ERR("Failed to create target machine\n");
    if (triple_needs_free)
      free(triple);
    else
      LLVMDisposeMessage(raw_triple);
    if (host_features)
      LLVMDisposeMessage(host_features);
    return false;
  }
  LLVMTargetDataRef td = LLVMCreateTargetDataLayout(tm);
  char *layout = LLVMCopyStringRepOfTargetData(td);
  if (layout) {
    LLVMSetDataLayout(module, layout);
    LLVMDisposeMessage(layout);
  }
  LLVMDisposeTargetData(td);
  LLVMSetTarget(module, triple);
  char *emit_err = NULL;
  int res = LLVMTargetMachineEmitToFile(tm, module, (char *)path,
                                        LLVMObjectFile, &emit_err);
  if (emit_err) {
    NY_LOG_ERR("Object emission failed: %s\n", emit_err);
    LLVMDisposeMessage(emit_err);
  }
  if (env_triple && env_triple[0]) {
    if (triple_needs_free)
      free(triple);
    free(raw_triple);
  } else {
    if (triple_needs_free)
      free(triple);
    LLVMDisposeMessage(raw_triple);
  }
  if (host_features)
    LLVMDisposeMessage(host_features);
  LLVMDisposeTargetMachine(tm);
  return res == 0;
}

bool ny_llvm_emit_file(LLVMModuleRef module, const char *path,
                       LLVMCodeGenFileType kind) {
  if (!module || !path)
    return false;
  if (!ny_llvm_init_native())
    return false;
  char *raw_triple = NULL;
  const char *env_triple = getenv("NYTRIX_HOST_TRIPLE");
  if (env_triple && *env_triple)
    raw_triple = ny_strdup(env_triple);
  if (!raw_triple)
    raw_triple = LLVMGetDefaultTargetTriple();
  if (!raw_triple)
    return false;
  bool triple_needs_free = false;
  char *triple = normalize_triple(raw_triple, &triple_needs_free);
  LLVMTargetRef target;
  char *err = NULL;
  if (LLVMGetTargetFromTriple(triple, &target, &err)) {
    NY_LOG_ERR("Invalid target triple: %s\n", err);
    LLVMDisposeMessage(err);
    if (triple_needs_free)
      free(triple);
    else
      LLVMDisposeMessage(raw_triple);
    return false;
  }
  char cpu_buf[128] = {0};
  char feat_buf[256] = {0};
  derive_host_target(triple, cpu_buf, sizeof(cpu_buf), feat_buf,
                     sizeof(feat_buf));
  char *host_features = NULL;
  const char *cpu = cpu_buf[0] ? cpu_buf : "";
  const char *features = feat_buf;
  bool arm32 = triple && strstr(triple, "arm") && !strstr(triple, "aarch64") &&
               !strstr(triple, "arm64");
  if (!feat_buf[0] && !arm32) {
    host_features = LLVMGetHostCPUFeatures();
    if (host_features)
      features = host_features;
  } else if (!feat_buf[0] && arm32) {
    features = "";
  }
  apply_target_attrs(module, cpu, features);
  LLVMTargetMachineRef tm = LLVMCreateTargetMachine(
      target, triple, cpu, features ? features : "", LLVMCodeGenLevelDefault,
      LLVMRelocPIC, host_code_model());
  if (!tm) {
    NY_LOG_ERR("Failed to create target machine\n");
    if (triple_needs_free)
      free(triple);
    else
      LLVMDisposeMessage(raw_triple);
    if (host_features)
      LLVMDisposeMessage(host_features);
    return false;
  }
  char *emit_err = NULL;
  int res =
      LLVMTargetMachineEmitToFile(tm, module, (char *)path, kind, &emit_err);
  if (emit_err) {
    NY_LOG_ERR("Emission failed: %s\n", emit_err);
    LLVMDisposeMessage(emit_err);
  }
  if (env_triple && env_triple[0]) {
    if (triple_needs_free)
      free(triple);
    free(raw_triple);
  } else {
    if (triple_needs_free)
      free(triple);
    LLVMDisposeMessage(raw_triple);
  }
  if (host_features)
    LLVMDisposeMessage(host_features);
  LLVMDisposeTargetMachine(tm);
  return res == 0;
}

LLVMTypeRef ny_llvm_ptr_type(LLVMContextRef ctx) {
  return LLVMPointerTypeInContext(ctx, 0);
}

LLVMValueRef ny_llvm_const_gep2(LLVMTypeRef elem_ty, LLVMValueRef base,
                                LLVMValueRef *indices, unsigned count) {
  return LLVMConstGEP2(elem_ty, base, indices, count);
}
