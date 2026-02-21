#include "base/common.h"
#include "base/util.h"
#include "code/priv.h"
#include <ctype.h>
#include <limits.h>
#include <llvm-c/Core.h>
#include <llvm-c/DebugInfo.h>
#include <llvm/Config/llvm-config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <direct.h>
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

typedef void *(*ny_di_insert_at_end_fn)(LLVMDIBuilderRef, LLVMValueRef,
                                        LLVMMetadataRef, LLVMMetadataRef,
                                        LLVMMetadataRef, LLVMBasicBlockRef);
typedef LLVMBool (*ny_llvm_is_new_dbg_info_format_fn)(LLVMModuleRef);
typedef void (*ny_llvm_set_new_dbg_info_format_fn)(LLVMModuleRef, LLVMBool);

#ifdef _WIN32
static void *ny_resolve_symbol_win(const char *name) {
  if (!name || !*name)
    return NULL;
  static const char *const dll_names[] = {"LLVM-C.dll", "libLLVM.dll",
                                          "LLVM.dll"};
  for (size_t i = 0; i < sizeof(dll_names) / sizeof(dll_names[0]); ++i) {
    HMODULE h = GetModuleHandleA(dll_names[i]);
    if (!h)
      continue;
    FARPROC p = GetProcAddress(h, name);
    if (p)
      return (void *)p;
  }
  return NULL;
}
#endif

static ny_di_insert_at_end_fn ny_resolve_di_insert(const char *primary,
                                                   const char *fallback) {
#ifdef _WIN32
  void *sym = ny_resolve_symbol_win(primary);
  if (!sym && fallback)
    sym = ny_resolve_symbol_win(fallback);
  return (ny_di_insert_at_end_fn)sym;
#else
  void *sym = dlsym(RTLD_DEFAULT, primary);
  if (!sym && fallback)
    sym = dlsym(RTLD_DEFAULT, fallback);
  return (ny_di_insert_at_end_fn)sym;
#endif
}

static void ny_maybe_enable_new_dbg_info_format(LLVMModuleRef module) {
  if (!module)
    return;
  static bool resolved = false;
  static ny_llvm_is_new_dbg_info_format_fn is_new_dbg = NULL;
  static ny_llvm_set_new_dbg_info_format_fn set_new_dbg = NULL;
  if (!resolved) {
#ifdef _WIN32
    is_new_dbg = (ny_llvm_is_new_dbg_info_format_fn)ny_resolve_symbol_win(
        "LLVMIsNewDbgInfoFormat");
    set_new_dbg = (ny_llvm_set_new_dbg_info_format_fn)ny_resolve_symbol_win(
        "LLVMSetIsNewDbgInfoFormat");
#else
    is_new_dbg = (ny_llvm_is_new_dbg_info_format_fn)dlsym(
        RTLD_DEFAULT, "LLVMIsNewDbgInfoFormat");
    set_new_dbg = (ny_llvm_set_new_dbg_info_format_fn)dlsym(
        RTLD_DEFAULT, "LLVMSetIsNewDbgInfoFormat");
#endif
    resolved = true;
  }
  if (is_new_dbg && set_new_dbg && !is_new_dbg(module))
    set_new_dbg(module, 1);
}

static void
ny_di_insert_declare_at_end(LLVMDIBuilderRef builder, LLVMValueRef storage,
                            LLVMMetadataRef var_info, LLVMMetadataRef expr,
                            LLVMMetadataRef loc, LLVMBasicBlockRef block) {
  static ny_di_insert_at_end_fn fn = NULL;
  static bool resolved = false;
  if (!resolved) {
    fn = ny_resolve_di_insert("LLVMDIBuilderInsertDeclareRecordAtEnd",
                              "LLVMDIBuilderInsertDeclareAtEnd");
    resolved = true;
  }
  if (fn)
    (void)fn(builder, storage, var_info, expr, loc, block);
#ifdef _WIN32
  else
    LLVMDIBuilderInsertDeclareRecordAtEnd(builder, storage, var_info, expr, loc,
                                          block);
#endif
}

static void
ny_di_insert_dbgvalue_at_end(LLVMDIBuilderRef builder, LLVMValueRef value,
                             LLVMMetadataRef var_info, LLVMMetadataRef expr,
                             LLVMMetadataRef loc, LLVMBasicBlockRef block) {
  static ny_di_insert_at_end_fn fn = NULL;
  static bool resolved = false;
  if (!resolved) {
    fn = ny_resolve_di_insert("LLVMDIBuilderInsertDbgValueRecordAtEnd",
                              "LLVMDIBuilderInsertDbgValueAtEnd");
    resolved = true;
  }
  if (fn)
    (void)fn(builder, value, var_info, expr, loc, block);
#ifdef _WIN32
  else
    LLVMDIBuilderInsertDbgValueRecordAtEnd(builder, value, var_info, expr, loc,
                                           block);
#endif
}

static const char *ny_debug_display_name(const char *name, token_t tok,
                                         char *buf, size_t buf_len) {
  if (!buf || buf_len == 0)
    return name;
  const char *raw = (name && *name) ? name : "<anon>";
  if (strncmp(raw, "__lambda", 8) == 0) {
    const char *file = tok.filename ? tok.filename : "";
    const char *base = strrchr(file, '/');
    if (base)
      base++;
    else
      base = file;
    if (tok.line > 0 && *base)
      snprintf(buf, buf_len, "lambda@%s:%d:%d", base, tok.line, tok.col);
    else if (tok.line > 0)
      snprintf(buf, buf_len, "lambda@%d:%d", tok.line, tok.col);
    else
      snprintf(buf, buf_len, "lambda");
    return buf;
  }
  if (strncmp(raw, "__defer", 7) == 0) {
    if (tok.line > 0)
      snprintf(buf, buf_len, "defer@%d:%d", tok.line, tok.col);
    else
      snprintf(buf, buf_len, "defer");
    return buf;
  }
  return raw;
}

static bool ny_eq_icase(const char *a, const char *b) {
  if (!a || !b)
    return false;
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
      return false;
    ++a;
    ++b;
  }
  return *a == '\0' && *b == '\0';
}

static bool ny_debug_emit_locals_enabled(void) {
  const char *v = getenv("NYTRIX_DEBUG_LOCALS");
  if (!v || !*v)
    return true;
  return strcmp(v, "0") != 0 && !ny_eq_icase(v, "false") &&
         !ny_eq_icase(v, "no") && !ny_eq_icase(v, "off");
}

#ifndef _WIN32
static unsigned ny_debug_dwarf_version(void) {
  const char *v = getenv("NYTRIX_DWARF_VERSION");
  if (!v || !*v)
    return 5;
  char *end = NULL;
  long parsed = strtol(v, &end, 10);
  if (end == v || (end && *end != '\0') || parsed < 2 || parsed > 5)
    return 5;
  return (unsigned)parsed;
}
#endif

static bool ny_debug_enabled_default_on(const char *name) {
  const char *v = getenv(name);
  if (!v || !*v)
    return true;
  return strcmp(v, "0") != 0 && !ny_eq_icase(v, "false") &&
         !ny_eq_icase(v, "no") && !ny_eq_icase(v, "off");
}

static bool ny_debug_is_std_alias(const char *filename) {
  if (!filename || !*filename)
    return false;
  return strcmp(filename, "<stdlib>") == 0 ||
         strcmp(filename, "<repl_std>") == 0 ||
         strcmp(filename, "std.ny") == 0 ||
         strcmp(filename, "std_bundle.ny") == 0;
}

static void ny_debug_compile_flags(const codegen_t *cg, char *buf,
                                   size_t buf_len) {
  if (!buf || buf_len == 0)
    return;
  buf[0] = '\0';
  if (!cg) {
    snprintf(buf, buf_len, "-g");
    return;
  }
  if (cg->debug_opt_pipeline && *cg->debug_opt_pipeline) {
    snprintf(buf, buf_len, "-g -passes=%s", cg->debug_opt_pipeline);
    return;
  }
  int lvl = cg->debug_opt_level;
  if (lvl < 0)
    lvl = 0;
  if (lvl > 3)
    lvl = 3;
  snprintf(buf, buf_len, "-g -O%d", lvl);
}

static bool ny_debug_file_exists(const char *path) {
  if (!path || !*path)
    return false;
  FILE *fp = fopen(path, "rb");
  if (!fp)
    return false;
  fclose(fp);
  return true;
}

static bool ny_debug_is_absolute_path(const char *path) {
  if (!path || !*path)
    return false;
#ifdef _WIN32
  if ((path[0] && path[1] == ':') || ((path[0] == '\\' || path[0] == '/') &&
                                      (path[1] == '\\' || path[1] == '/')))
    return true;
  return false;
#else
  return path[0] == '/';
#endif
}

static const char *ny_debug_std_bundle_path(void) {
  static bool resolved = false;
  static char path[PATH_MAX];
  if (resolved)
    return path[0] ? path : NULL;
  resolved = true;
  path[0] = '\0';
  const char *envs[] = {"NYTRIX_BUILD_STD_PATH", "NYTRIX_STD_PREBUILT",
                        "NYTRIX_STD_PATH"};
  for (size_t i = 0; i < sizeof(envs) / sizeof(envs[0]); ++i) {
    const char *v = getenv(envs[i]);
    if (v && *v && ny_debug_file_exists(v)) {
      snprintf(path, sizeof(path), "%s", v);
      return path;
    }
  }
  const char *root = getenv("NYTRIX_ROOT");
  if (root && *root) {
    char cand[PATH_MAX];
    snprintf(cand, sizeof(cand), "%s/std.ny", root);
    if (ny_debug_file_exists(cand)) {
      snprintf(path, sizeof(path), "%s", cand);
      return path;
    }
  }
  {
    const char *home = getenv("HOME");
    if (home && *home) {
      char cand[PATH_MAX];
      snprintf(cand, sizeof(cand), "%s/.cache/nytrix-build/release/std.ny",
               home);
      if (ny_debug_file_exists(cand)) {
        snprintf(path, sizeof(path), "%s", cand);
        return path;
      }
      snprintf(cand, sizeof(cand), "%s/.cache/nytrix-build/debug/std.ny", home);
      if (ny_debug_file_exists(cand)) {
        snprintf(path, sizeof(path), "%s", cand);
        return path;
      }
    }
  }
  const char *const fallbacks[] = {"build/release/std.ny", "build/debug/std.ny",
                                   "build/std.ny", "/usr/share/nytrix/std.ny",
                                   "/usr/local/share/nytrix/std.ny"};
  for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); ++i) {
    if (ny_debug_file_exists(fallbacks[i])) {
      snprintf(path, sizeof(path), "%s", fallbacks[i]);
      return path;
    }
  }
  return NULL;
}

static const char *ny_debug_resolve_filename(codegen_t *cg,
                                             const char *filename,
                                             char *scratch,
                                             size_t scratch_len) {
  const char *use_name =
      (filename && *filename)
          ? filename
          : (cg && cg->debug_main_file ? cg->debug_main_file : "<unknown>");
  if (ny_debug_is_std_alias(use_name)) {
    const char *std_path = ny_debug_std_bundle_path();
    if (std_path && *std_path)
      use_name = std_path;
  }
  if (!scratch || scratch_len == 0)
    return use_name;
  if (!use_name || !*use_name || use_name[0] == '<' ||
      ny_debug_is_absolute_path(use_name))
    return use_name;
#ifdef _WIN32
  if (_fullpath(scratch, use_name, scratch_len))
    return scratch;
#else
  if (realpath(use_name, scratch))
    return scratch;

  /*
   * If a filename is relative (e.g. "std.ny"), resolve it relative to the
   * current main file directory when possible.
   */
  if (cg && cg->debug_main_file &&
      ny_debug_is_absolute_path(cg->debug_main_file)) {
    const char *slash = strrchr(cg->debug_main_file, '/');
    if (slash && slash != cg->debug_main_file) {
      size_t dir_len = (size_t)(slash - cg->debug_main_file);
      char candidate[PATH_MAX];
      if (dir_len + 1 + strlen(use_name) + 1 < sizeof(candidate)) {
        memcpy(candidate, cg->debug_main_file, dir_len);
        candidate[dir_len] = '\0';
        snprintf(candidate + dir_len, sizeof(candidate) - dir_len, "/%s",
                 use_name);
        if (realpath(candidate, scratch))
          return scratch;
      }
    }
  }

  {
    const char *root = ny_src_root();
    if (root && *root) {
      char candidate[PATH_MAX];
      snprintf(candidate, sizeof(candidate), "%s/%s", root, use_name);
      if (realpath(candidate, scratch))
        return scratch;
    }
  }
#endif
  return use_name;
}

static LLVMMetadataRef debug_file_for(codegen_t *cg, const char *filename) {
  if (!cg || !cg->di_builder)
    return NULL;
  char resolved_path[PATH_MAX];
  const char *use_name = ny_debug_resolve_filename(cg, filename, resolved_path,
                                                   sizeof(resolved_path));
  const char *base = use_name;
  const char *dir = ".";
  char *dir_buf = NULL;
  const char *slash = strrchr(use_name, '/');
#ifdef _WIN32
  const char *bslash = strrchr(use_name, '\\');
  if (!slash || (bslash && bslash > slash))
    slash = bslash;
#endif
  if (slash && slash != use_name) {
    base = slash + 1;
    dir_buf = ny_strndup(use_name, (size_t)(slash - use_name));
    dir = dir_buf;
  }
  LLVMMetadataRef file = LLVMDIBuilderCreateFile(
      cg->di_builder, base, strlen(base), dir, strlen(dir));
  if (dir_buf)
    free(dir_buf);
  return file ? file : cg->di_file;
}

void codegen_debug_init(codegen_t *cg, const char *main_file) {
  if (!cg || !cg->debug_symbols || cg->di_builder)
    return;
  ny_maybe_enable_new_dbg_info_format(cg->module);
  char abs_main[PATH_MAX];
  const char *resolved_main = ny_debug_resolve_filename(
      cg, (main_file && *main_file) ? main_file : "<inline>", abs_main,
      sizeof(abs_main));
  if (resolved_main && *resolved_main) {
    LLVMSetSourceFileName(cg->module, resolved_main, strlen(resolved_main));
  }
  cg->debug_main_file = resolved_main && *resolved_main
                            ? ny_strndup(resolved_main, strlen(resolved_main))
                            : ny_strndup("<inline>", strlen("<inline>"));
  cg->di_builder = LLVMCreateDIBuilder(cg->module);
  cg->di_file = debug_file_for(cg, cg->debug_main_file);
  cg->di_subroutine_type = LLVMDIBuilderCreateSubroutineType(
      cg->di_builder, cg->di_file, NULL, 0, LLVMDIFlagZero);

  char producer[96];
#if defined(LLVM_VERSION_MAJOR) && defined(LLVM_VERSION_MINOR)
  snprintf(producer, sizeof(producer), "nytrix (LLVM %d.%d)",
           LLVM_VERSION_MAJOR, LLVM_VERSION_MINOR);
#else
  snprintf(producer, sizeof(producer), "nytrix");
#endif
  char flags[256];
  ny_debug_compile_flags(cg, flags, sizeof(flags));
  const char *split_name_env = getenv("NYTRIX_DWO_NAME");
  const char *split_name =
      split_name_env && *split_name_env ? split_name_env : "";
  const char *sysroot_env = getenv("NYTRIX_SYSROOT");
  const char *sysroot = sysroot_env && *sysroot_env ? sysroot_env : "";
  const char *sdk_env = getenv("NYTRIX_SDKROOT");
  const char *sdk = sdk_env && *sdk_env ? sdk_env : "";
  LLVMBool is_optimized = (cg->debug_opt_level > 0 ||
                           (cg->debug_opt_pipeline && *cg->debug_opt_pipeline))
                              ? 1
                              : 0;
  LLVMBool split_inlining =
      ny_debug_enabled_default_on("NYTRIX_DWARF_SPLIT_INLINING") ? 1 : 0;
  LLVMBool profile_info =
      ny_debug_enabled_default_on("NYTRIX_DWARF_PROFILE_INFO") ? 1 : 0;
  cg->di_cu = LLVMDIBuilderCreateCompileUnit(
      cg->di_builder, LLVMDWARFSourceLanguageC, cg->di_file, producer,
      strlen(producer), is_optimized, flags, strlen(flags), 0, split_name,
      strlen(split_name), LLVMDWARFEmissionFull, 0, split_inlining,
      profile_info, sysroot, strlen(sysroot), sdk, strlen(sdk));

  LLVMMetadataRef dbg_ver = LLVMValueAsMetadata(LLVMConstInt(
      LLVMInt32TypeInContext(cg->ctx), LLVMDebugMetadataVersion(), 0));
  LLVMAddModuleFlag(cg->module, LLVMModuleFlagBehaviorWarning,
                    "Debug Info Version", strlen("Debug Info Version"),
                    dbg_ver);

#ifndef _WIN32
  unsigned dwarf_version = ny_debug_dwarf_version();
  LLVMMetadataRef dwarf_ver = LLVMValueAsMetadata(
      LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), dwarf_version, 0));
  LLVMAddModuleFlag(cg->module, LLVMModuleFlagBehaviorWarning, "Dwarf Version",
                    strlen("Dwarf Version"), dwarf_ver);
#endif

#ifdef _WIN32
  // For Windows, enable CodeView (PDB) support
  LLVMMetadataRef cv =
      LLVMValueAsMetadata(LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 1, 0));
  LLVMAddModuleFlag(cg->module, LLVMModuleFlagBehaviorWarning, "CodeView",
                    strlen("CodeView"), cv);
#endif
}

LLVMMetadataRef codegen_debug_subprogram(codegen_t *cg, LLVMValueRef func,
                                         const char *name, token_t tok) {
  if (!cg || !cg->di_builder || !func)
    return NULL;
  if (!name)
    name = "<anon>";
  LLVMMetadataRef file = debug_file_for(cg, tok.filename);
  if (!cg->di_subroutine_type) {
    cg->di_subroutine_type = LLVMDIBuilderCreateSubroutineType(
        cg->di_builder, file ? file : cg->di_file, NULL, 0, LLVMDIFlagZero);
  }
  char display_buf[96];
  const char *display_name =
      ny_debug_display_name(name, tok, display_buf, sizeof(display_buf));
  unsigned line = tok.line > 0 ? (unsigned)tok.line : 0;
  LLVMMetadataRef sp = LLVMDIBuilderCreateFunction(
      cg->di_builder, file ? file : cg->di_file, display_name,
      strlen(display_name), name, strlen(name), file ? file : cg->di_file, line,
      cg->di_subroutine_type, 0, 1, line, LLVMDIFlagZero, 0);
  if (sp)
    LLVMSetSubprogram(func, sp);
  return sp;
}

void codegen_debug_finalize(codegen_t *cg) {
  if (!cg)
    return;
  if (!cg->di_builder) {
    if (cg->debug_main_file) {
      free((void *)cg->debug_main_file);
      cg->debug_main_file = NULL;
    }
    return;
  }
  LLVMDIBuilderFinalize(cg->di_builder);
  LLVMDisposeDIBuilder(cg->di_builder);
  cg->di_builder = NULL;
  if (cg->debug_main_file) {
    free((void *)cg->debug_main_file);
    cg->debug_main_file = NULL;
  }
}

void codegen_debug_variable(codegen_t *cg, const char *name, LLVMValueRef slot,
                            token_t tok, bool is_param, int param_idx,
                            bool is_slot) {
  if (!ny_debug_emit_locals_enabled())
    return;
  if (!cg || !cg->debug_symbols || !cg->di_builder || !cg->di_scope || !slot)
    return;

  LLVMMetadataRef file = debug_file_for(cg, tok.filename);
  // Nytrix values are tagged 64-bit pointers/integers. 0x01 = DW_ATE_address
  LLVMMetadataRef type = LLVMDIBuilderCreateBasicType(cg->di_builder, "any", 3,
                                                      64, 0x01, LLVMDIFlagZero);

  LLVMMetadataRef var;
  unsigned line = tok.line > 0 ? (unsigned)tok.line : 1;

  if (is_param) {
    var = LLVMDIBuilderCreateParameterVariable(
        cg->di_builder, cg->di_scope, name, strlen(name), (unsigned)param_idx,
        file, line, type, 1, LLVMDIFlagZero);
  } else {
    var = LLVMDIBuilderCreateAutoVariable(cg->di_builder, cg->di_scope, name,
                                          strlen(name), file, line, type, 1,
                                          LLVMDIFlagZero, 0);
  }

  // Bind the storage (alloca) or value to the debug variable
  LLVMMetadataRef expr = LLVMDIBuilderCreateExpression(cg->di_builder, NULL, 0);
  LLVMMetadataRef loc = LLVMDIBuilderCreateDebugLocation(
      cg->ctx, line, (unsigned)tok.col, cg->di_scope, NULL);

  if (is_slot) {
    ny_di_insert_declare_at_end(cg->di_builder, slot, var, expr, loc,
                                LLVMGetInsertBlock(cg->builder));
  } else {
    ny_di_insert_dbgvalue_at_end(cg->di_builder, slot, var, expr, loc,
                                 LLVMGetInsertBlock(cg->builder));
  }
}

void codegen_debug_global_variable(codegen_t *cg, const char *name,
                                   LLVMValueRef global, token_t tok) {
  if (!ny_debug_emit_locals_enabled())
    return;
  if (!cg || !cg->debug_symbols || !cg->di_builder || !cg->di_file || !global)
    return;

  LLVMMetadataRef file = debug_file_for(cg, tok.filename);
  LLVMMetadataRef type = LLVMDIBuilderCreateBasicType(cg->di_builder, "any", 3,
                                                      64, 0x01, LLVMDIFlagZero);

  unsigned line = tok.line > 0 ? (unsigned)tok.line : 1;
  LLVMMetadataRef expr = LLVMDIBuilderCreateExpression(cg->di_builder, NULL, 0);

  LLVMDIBuilderCreateGlobalVariableExpression(
      cg->di_builder, cg->di_cu, name, strlen(name), name, strlen(name), file,
      line, type, 0, expr, NULL, 0);
}

LLVMMetadataRef codegen_debug_push_block(codegen_t *cg, token_t tok) {
  if (!cg || !cg->debug_symbols || !cg->di_builder || !cg->di_scope)
    return NULL;

  LLVMMetadataRef prev = cg->di_scope;
  LLVMMetadataRef file = debug_file_for(cg, tok.filename);
  cg->di_scope = LLVMDIBuilderCreateLexicalBlock(
      cg->di_builder, cg->di_scope, file ? file : cg->di_file,
      (unsigned)tok.line, (unsigned)tok.col);
  return prev;
}

void codegen_debug_pop_block(codegen_t *cg, LLVMMetadataRef prev_scope) {
  if (!cg || !cg->debug_symbols)
    return;
  cg->di_scope = prev_scope;
}

LLVMMetadataRef codegen_debug_loc_scope(codegen_t *cg, token_t tok) {
  if (!cg || !cg->debug_symbols || !cg->di_builder || !cg->di_scope)
    return NULL;
  if (!tok.filename || !*tok.filename)
    return cg->di_scope;
  if (strcmp(tok.filename, "<stdlib>") != 0 &&
      strcmp(tok.filename, "<repl_std>") != 0)
    return cg->di_scope;
  LLVMMetadataRef file = debug_file_for(cg, tok.filename);
  if (!file)
    return cg->di_scope;
  unsigned line = tok.line > 0 ? (unsigned)tok.line : 1;
  unsigned col = tok.col > 0 ? (unsigned)tok.col : 1;
  return LLVMDIBuilderCreateLexicalBlock(cg->di_builder, cg->di_scope, file,
                                         line, col);
}
