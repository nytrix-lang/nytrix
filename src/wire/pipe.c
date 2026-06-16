#define _CRT_RAND_S
#include "wire/pipe.h"
#include "base/common.h"
#include "base/hash.h"
#include "base/loader.h"
#include "base/time.h"
#include "base/util.h"
#include "code/code.h"
#include "code/jit.h"
#include "code/llvm.h"
#include "code/priv.h"
#include "code/typepipeline.h"
#include "parse/json.h"
#include "parse/parser.h"
#include "repl/repl.h"
#include "wire/build.h"
#include "wire/bundle.h"
#include "wire/cache.h"
#include <limits.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitReader.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/DebugInfo.h>
#include <llvm-c/Error.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/IRReader.h>
#include <llvm-c/Linker.h>
#include <llvm-c/Support.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <llvm/Config/llvm-config.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <dlfcn.h>
#include <sys/wait.h>
#endif
#if defined(__linux__) && defined(__x86_64__)
#include <elf.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <io.h>
#endif
#include <ctype.h>
#include <stdarg.h>

extern int64_t rt_free(int64_t ptr);

#if defined(__GNUC__) || defined(__clang__)
#define NY_UNUSED_FUNC __attribute__((unused))
#else
#define NY_UNUSED_FUNC
#endif

typedef struct ny_ir_stats_t {
  uint64_t funcs;
  uint64_t blocks;
  uint64_t insts;
  uint64_t allocas;
  uint64_t phis;
} ny_ir_stats_t;

static LLVMModuleRef ny_prepare_ir_dump_module(const ny_options *opt,
                                               LLVMModuleRef module);
static bool ny_is_llvm_special_global(const char *name);
static void ny_ensure_parent_dir_for_path(const char *path);

static bool ny_trace_compile_enabled(void) {
  return ny_env_enabled("NYTRIX_TRACE_COMPILE") ||
         ny_env_enabled("NYTRIX_TRACE_CODEGEN");
}

static void ny_collect_ir_stats(LLVMModuleRef module, ny_ir_stats_t *out) {
  if (!out)
    return;
  memset(out, 0, sizeof(*out));
  if (!module)
    return;
  for (LLVMValueRef fn = LLVMGetFirstFunction(module); fn;
       fn = LLVMGetNextFunction(fn)) {
    if (LLVMCountBasicBlocks(fn) == 0)
      continue;
    out->funcs++;
    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn); bb;
         bb = LLVMGetNextBasicBlock(bb)) {
      out->blocks++;
      for (LLVMValueRef inst = LLVMGetFirstInstruction(bb); inst;
           inst = LLVMGetNextInstruction(inst)) {
        out->insts++;
        LLVMOpcode op = LLVMGetInstructionOpcode(inst);
        if (op == LLVMAlloca)
          out->allocas++;
        else if (op == LLVMPHI)
          out->phis++;
      }
    }
  }
}

static void ny_trace_ir_stats(const char *phase, LLVMModuleRef module) {
  if (!ny_trace_compile_enabled())
    return;
  ny_ir_stats_t st = {0};
  ny_collect_ir_stats(module, &st);
  fprintf(stderr,
          "TRACE_COMPILE %s funcs=%" PRIu64 " blocks=%" PRIu64 " insts=%" PRIu64
          " allocas=%" PRIu64 " phis=%" PRIu64 "\n",
          phase ? phase : "ir", st.funcs, st.blocks, st.insts, st.allocas,
          st.phis);
}

static void ny_trace_file_size(const char *label, const char *path) {
  if (!ny_trace_compile_enabled() || !path || !*path)
    return;
  struct stat st;
  if (stat(path, &st) != 0)
    return;
  fprintf(stderr, "TRACE_COMPILE %s path=%s size=%lld bytes\n",
          label ? label : "artifact", path, (long long)st.st_size);
}

static const char *ny_skip_ws(const char *s) {
  while (s && *s && isspace((unsigned char)*s))
    s++;
  return s;
}

static bool ny_decode_tiny_string_literal(const char *src, const char **end_out,
                                          char **out_text) {
  if (!src || !(*src == '"' || *src == '\'') || !end_out || !out_text)
    return false;
  char quote = *src++;
  size_t cap = strlen(src) + 1;
  char *out = malloc(cap ? cap : 1);
  if (!out)
    return false;
  size_t len = 0;
  while (*src) {
    char ch = *src++;
    if (ch == quote) {
      out[len] = '\0';
      *end_out = src;
      *out_text = out;
      return true;
    }
    if (ch == '\\') {
      char esc = *src++;
      if (!esc) {
        free(out);
        return false;
      }
      switch (esc) {
      case 'n':
        ch = '\n';
        break;
      case 'r':
        ch = '\r';
        break;
      case 't':
        ch = '\t';
        break;
      case '0':
        ch = '\0';
        break;
      case '\\':
      case '\'':
      case '"':
        ch = esc;
        break;
      default:
        ch = esc;
        break;
      }
    }
    out[len++] = ch;
  }
  free(out);
  return false;
}

static bool ny_parse_tiny_print_arg(const char *src, const char **end_out,
                                    char **out_text) {
  const char *p = ny_skip_ws(src);
  if (!p || !*p)
    return false;
  if (*p == '"' || *p == '\'')
    return ny_decode_tiny_string_literal(p, end_out, out_text);
  const char *start = p;
  if (*p == '+' || *p == '-')
    p++;
  bool saw_digit = false;
  while (isdigit((unsigned char)*p)) {
    saw_digit = true;
    p++;
  }
  if (*p == '.') {
    p++;
    while (isdigit((unsigned char)*p)) {
      saw_digit = true;
      p++;
    }
  }
  if (!saw_digit)
    return false;
  if (*p == 'e' || *p == 'E') {
    const char *exp = p++;
    if (*p == '+' || *p == '-')
      p++;
    bool exp_digit = false;
    while (isdigit((unsigned char)*p)) {
      exp_digit = true;
      p++;
    }
    if (!exp_digit)
      p = exp;
  }
  *out_text = ny_strndup(start, (size_t)(p - start));
  if (!*out_text)
    return false;
  *end_out = p;
  return true;
}

typedef enum {
  NY_TINY_CMD_NONE = 0,
  NY_TINY_CMD_NOOP,
  NY_TINY_CMD_PRINT,
  NY_TINY_CMD_EPRINT,
} ny_tiny_cmd_kind_t;

typedef struct {
  ny_tiny_cmd_kind_t kind;
  char *text;
} ny_tiny_command_t;

static void ny_tiny_command_free(ny_tiny_command_t *cmd) {
  if (!cmd)
    return;
  free(cmd->text);
  cmd->text = NULL;
  cmd->kind = NY_TINY_CMD_NONE;
}

static bool ny_parse_tiny_command_string(const char *src,
                                         ny_tiny_command_t *out) {
  if (!src || !out)
    return false;
  memset(out, 0, sizeof(*out));
  const char *p = ny_skip_ws(src);
  ny_tiny_cmd_kind_t kind = NY_TINY_CMD_NOOP;
  bool wrapped_call = false;
  if (strncmp(p, "print", 5) == 0 && !isalnum((unsigned char)p[5]) &&
      p[5] != '_') {
    p += 5;
    kind = NY_TINY_CMD_PRINT;
    wrapped_call = true;
  } else if (strncmp(p, "eprint", 6) == 0 && !isalnum((unsigned char)p[6]) &&
             p[6] != '_') {
    p += 6;
    kind = NY_TINY_CMD_EPRINT;
    wrapped_call = true;
  }
  if (wrapped_call) {
    p = ny_skip_ws(p);
    if (*p != '(')
      return false;
    p++;
  }

  char *text = NULL;
  if (!ny_parse_tiny_print_arg(p, &p, &text))
    return false;
  p = ny_skip_ws(p);
  if (wrapped_call) {
    if (*p != ')') {
      free(text);
      return false;
    }
    p++;
    p = ny_skip_ws(p);
  }
  if (*p == ';') {
    p++;
    p = ny_skip_ws(p);
  }
  if (*p != '\0') {
    free(text);
    return false;
  }
  out->kind = kind;
  out->text = text;
  return true;
}

static char *ny_c_escape_string_literal(const char *src) {
  if (!src)
    src = "";
  size_t cap = strlen(src) * 4 + 1;
  char *out = malloc(cap ? cap : 1);
  if (!out)
    return NULL;
  size_t len = 0;
  for (const unsigned char *p = (const unsigned char *)src; *p; ++p) {
    unsigned char ch = *p;
    switch (ch) {
    case '\\':
      out[len++] = '\\';
      out[len++] = '\\';
      break;
    case '"':
      out[len++] = '\\';
      out[len++] = '"';
      break;
    case '\n':
      out[len++] = '\\';
      out[len++] = 'n';
      break;
    case '\r':
      out[len++] = '\\';
      out[len++] = 'r';
      break;
    case '\t':
      out[len++] = '\\';
      out[len++] = 't';
      break;
    default:
      if (ch < 32 || ch >= 127) {
        snprintf(out + len, cap - len, "\\%03o", ch);
        len += 4;
      } else {
        out[len++] = (char)ch;
      }
      break;
    }
  }
  out[len] = '\0';
  return out;
}

static bool ny_output_path_is_object(const char *path) {
  if (!path)
    return false;
  size_t len = strlen(path);
  return len > 2 && strcmp(path + len - 2, ".o") == 0;
}

#if defined(__linux__) && defined(__x86_64__)
static void ny_emit_u32le(unsigned char *dst, uint32_t v) {
  dst[0] = (unsigned char)(v & 0xffu);
  dst[1] = (unsigned char)((v >> 8) & 0xffu);
  dst[2] = (unsigned char)((v >> 16) & 0xffu);
  dst[3] = (unsigned char)((v >> 24) & 0xffu);
}

static bool ny_write_linux_x64_tiny_exe(const ny_options *opt,
                                        const ny_tiny_command_t *cmd) {
  if (!opt || !opt->output_file || !cmd)
    return false;
  bool do_write =
      (cmd->kind == NY_TINY_CMD_PRINT || cmd->kind == NY_TINY_CMD_EPRINT);
  const char *text = cmd->text ? cmd->text : "";
  size_t text_len = do_write ? strlen(text) : 0;
  if (text_len > UINT32_MAX - 1u)
    return false;
  size_t msg_len = do_write ? text_len + 1u : 0u;

  unsigned char code[64];
  size_t c = 0;
  size_t disp_pos = 0;
  if (do_write) {
    code[c++] = 0xb8;
    ny_emit_u32le(code + c, 1u);
    c += 4;
    code[c++] = 0xbf;
    ny_emit_u32le(code + c, cmd->kind == NY_TINY_CMD_EPRINT ? 2u : 1u);
    c += 4;
    code[c++] = 0x48;
    code[c++] = 0x8d;
    code[c++] = 0x35;
    disp_pos = c;
    c += 4;
    code[c++] = 0xba;
    ny_emit_u32le(code + c, (uint32_t)msg_len);
    c += 4;
    code[c++] = 0x0f;
    code[c++] = 0x05;
  }
  code[c++] = 0x31;
  code[c++] = 0xff;
  code[c++] = 0xb8;
  ny_emit_u32le(code + c, 60u);
  c += 4;
  code[c++] = 0x0f;
  code[c++] = 0x05;

  const uint64_t base = UINT64_C(0x400000);
  const size_t code_off = sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr);
  if (do_write) {
    size_t msg_off = code_off + c;
    size_t rip_after_lea = code_off + disp_pos + 4u;
    int64_t disp = (int64_t)msg_off - (int64_t)rip_after_lea;
    if (disp < INT32_MIN || disp > INT32_MAX)
      return false;
    ny_emit_u32le(code + disp_pos, (uint32_t)(int32_t)disp);
  }

  Elf64_Ehdr eh;
  memset(&eh, 0, sizeof(eh));
  eh.e_ident[EI_MAG0] = ELFMAG0;
  eh.e_ident[EI_MAG1] = ELFMAG1;
  eh.e_ident[EI_MAG2] = ELFMAG2;
  eh.e_ident[EI_MAG3] = ELFMAG3;
  eh.e_ident[EI_CLASS] = ELFCLASS64;
  eh.e_ident[EI_DATA] = ELFDATA2LSB;
  eh.e_ident[EI_VERSION] = EV_CURRENT;
  eh.e_ident[EI_OSABI] = ELFOSABI_SYSV;
  eh.e_type = ET_EXEC;
  eh.e_machine = EM_X86_64;
  eh.e_version = EV_CURRENT;
  eh.e_entry = base + code_off;
  eh.e_phoff = sizeof(Elf64_Ehdr);
  eh.e_ehsize = sizeof(Elf64_Ehdr);
  eh.e_phentsize = sizeof(Elf64_Phdr);
  eh.e_phnum = 1;

  Elf64_Phdr ph;
  memset(&ph, 0, sizeof(ph));
  ph.p_type = PT_LOAD;
  ph.p_flags = PF_R | PF_X;
  ph.p_offset = 0;
  ph.p_vaddr = base;
  ph.p_paddr = base;
  ph.p_filesz = code_off + c + msg_len;
  ph.p_memsz = ph.p_filesz;
  ph.p_align = 0x1000;

  ny_ensure_parent_dir_for_path(opt->output_file);
  FILE *f = fopen(opt->output_file, "wb");
  if (!f)
    return false;
  bool ok = fwrite(&eh, 1, sizeof(eh), f) == sizeof(eh) &&
            fwrite(&ph, 1, sizeof(ph), f) == sizeof(ph) &&
            fwrite(code, 1, c, f) == c;
  if (ok && do_write) {
    ok = fwrite(text, 1, text_len, f) == text_len && fputc('\n', f) != EOF;
  }
  if (fclose(f) != 0)
    ok = false;
  if (!ok) {
    unlink(opt->output_file);
    return false;
  }
  chmod(opt->output_file, 0755);
  return true;
}
#endif

static bool ny_tiny_aot_cache_path(const ny_options *opt, bool object_only,
                                   char *out, size_t out_len) {
  if (!opt || !out || out_len == 0)
    return false;
  const char *root = ny_cache_root_dir();
  if (!root || !*root)
    return false;
  uint64_t h = NY_FNV1A64_OFFSET_BASIS;
  h = ny_fnv1a64_cstr("tiny-aot-v2", h);
  h = ny_fnv1a64_cstr(opt->command_string ? opt->command_string : "", h);
  h = ny_fnv1a64_cstr(ny_builder_choose_cc(), h);
  h = ny_fnv1a64(&object_only, sizeof(object_only), h);
  h = ny_fnv1a64(&opt->strip_override, sizeof(opt->strip_override), h);
  char dir[4096];
  snprintf(dir, sizeof(dir), "%s/tiny-aot", root);
  ny_ensure_dir_recursive(dir);
  snprintf(out, out_len, "%s/tiny_%016" PRIx64 "%s", dir, h,
           object_only ? ".o" : ".bin");
  return true;
}

static bool ny_write_tiny_c_source(const char *src_path,
                                   const ny_tiny_command_t *cmd) {
  if (!src_path || !cmd)
    return false;
  FILE *f = fopen(src_path, "wb");
  if (!f)
    return false;

  char *escaped = NULL;
  if (cmd->kind == NY_TINY_CMD_PRINT || cmd->kind == NY_TINY_CMD_EPRINT) {
    escaped = ny_c_escape_string_literal(cmd->text);
    if (!escaped) {
      fclose(f);
      unlink(src_path);
      return false;
    }
  }

  fputs("#include <stdio.h>\nint main(void) {\n", f);
  if (cmd->kind == NY_TINY_CMD_PRINT || cmd->kind == NY_TINY_CMD_EPRINT) {
    const char *stream = cmd->kind == NY_TINY_CMD_EPRINT ? "stderr" : "stdout";
    fprintf(f, "  fputs(\"%s\", %s);\n", escaped, stream);
    fprintf(f, "  fputc('\\n', %s);\n", stream);
  }
  fputs("  return 0;\n}\n", f);
  free(escaped);
  if (fclose(f) != 0) {
    unlink(src_path);
    return false;
  }
  return true;
}

static bool ny_compile_tiny_c_fallback(const ny_options *opt, bool object_only,
                                       const char *src_path) {
  if (!opt || !opt->output_file || !src_path)
    return false;
  ny_ensure_parent_dir_for_path(opt->output_file);
  const char *cc = ny_builder_choose_cc();
  const char *argv_exe[] = {cc,   "-std=c11",       "-O0", "-x", "c", src_path,
                            "-o", opt->output_file, NULL};
  const char *argv_obj[] = {
      cc,   "-std=c11",       "-O0", "-x", "c", "-c", src_path,
      "-o", opt->output_file, NULL};
  int rc = ny_exec_spawn(object_only ? argv_obj : argv_exe);
  if (rc != 0)
    return false;
  if (!object_only && opt->strip_override == 1)
    (void)ny_builder_strip(opt->output_file);
  return true;
}

static bool ny_compile_tiny_command(const ny_options *opt,
                                    const ny_tiny_command_t *cmd) {
  if (!opt || !opt->output_file || !cmd)
    return false;
  bool object_only = ny_output_path_is_object(opt->output_file);
  char cache_path[4096] = {0};
  bool have_cache_path =
      ny_tiny_aot_cache_path(opt, object_only, cache_path, sizeof(cache_path));
  if (have_cache_path && ny_access(cache_path, R_OK) == 0) {
    ny_ensure_parent_dir_for_path(opt->output_file);
    if (ny_copy_file(cache_path, opt->output_file) == 0)
      return true;
  }

#if defined(__linux__) && defined(__x86_64__)
  if (!object_only && ny_write_linux_x64_tiny_exe(opt, cmd)) {
    if (have_cache_path)
      (void)ny_copy_file(opt->output_file, cache_path);
    return true;
  }
#endif

  char src_path[4096];
  snprintf(src_path, sizeof(src_path), "%s/ny_tiny_%ld.c", ny_get_temp_dir(),
           (long)getpid());
  if (!ny_write_tiny_c_source(src_path, cmd))
    return false;
  bool ok = ny_compile_tiny_c_fallback(opt, object_only, src_path);
  unlink(src_path);
  if (!ok)
    return false;
  if (have_cache_path)
    (void)ny_copy_file(opt->output_file, cache_path);
  return true;
}

static void ny_tiny_timing_report(const ny_options *opt, ny_tick_t t_start,
                                  bool aot) {
  if (!opt || !opt->do_timing)
    return;
  fprintf(stderr, "Read file:   0.0000s\n");
  fprintf(stderr, "Scan imports: 0.0000s\n");
  fprintf(stderr, "Stdlib load: 0.0000s\n");
  fprintf(stderr, "Parsing:     0.0000s\n");
  fprintf(stderr, "Codegen:     0.0000s\n");
  if (aot) {
    fprintf(stderr, "Tiny AOT:    %.4fs\n", ny_ticks_elapsed_sec(t_start));
  } else {
    fprintf(stderr, "JIT Init:    0.0000s\n");
    fprintf(stderr, "JIT Compile: 0.0000s\n");
    fprintf(stderr, "JIT Run:     0.0000s\n");
  }
  fprintf(stderr, "Total time:  %.4fs\n", ny_ticks_elapsed_sec(t_start));
}

static bool ny_try_fast_command_string(const ny_options *opt,
                                       ny_tick_t t_start) {
  if (!opt || !opt->command_string || opt->input_file || opt->run_aot ||
      opt->expand || opt->mode != NY_MODE_RUN)
    return false;
  if (opt->emit_only && !opt->output_file)
    return false;

  ny_tiny_command_t cmd;
  if (!ny_parse_tiny_command_string(opt->command_string, &cmd))
    return false;

  if (opt->output_file) {
    bool ok = ny_compile_tiny_command(opt, &cmd);
    ny_tiny_command_free(&cmd);
    if (!ok)
      return false;
    ny_tiny_timing_report(opt, t_start, true);
    return true;
  }

  if (cmd.kind == NY_TINY_CMD_PRINT || cmd.kind == NY_TINY_CMD_EPRINT) {
    FILE *out = cmd.kind == NY_TINY_CMD_EPRINT ? stderr : stdout;
    fputs(cmd.text, out);
    fputc('\n', out);
    fflush(out);
  }
  ny_tiny_command_free(&cmd);
  ny_tiny_timing_report(opt, t_start, false);
  return true;
}

static char *ny_read_stdin_all(void) {
  size_t cap = 4096;
  size_t len = 0;
  char *buf = malloc(cap);
  if (!buf)
    return NULL;
  int ch;
  while ((ch = fgetc(stdin)) != EOF) {
    if (len + 1 >= cap) {
      size_t next_cap = cap * 2;
      char *next = realloc(buf, next_cap);
      if (!next) {
        free(buf);
        return NULL;
      }
      buf = next;
      cap = next_cap;
    }
    buf[len++] = (char)ch;
  }
  buf[len] = '\0';
  return buf;
}

static bool ny_repl_batch_can_fast_run(const char *src) {
  if (!src)
    return false;
  if (!ny_env_enabled("NYTRIX_REPL_FAST_PIPE"))
    return false;
  const char *p = src;
  while (*p) {
    while (*p && isspace((unsigned char)*p))
      p++;
    if (*p == '\0')
      return true;
    if (*p == ';') {
      while (*p && *p != '\n')
        p++;
      continue;
    }
    if (*p == '#') {
      if (p[1] == '!') {
        while (*p && *p != '\n')
          p++;
        continue;
      }
      while (*p && *p != '\n')
        p++;
      continue;
    }
    if (*p == ':')
      return false;
    if (strncmp(p, "module", 6) == 0 &&
        (p[6] == '\0' || isspace((unsigned char)p[6]) || p[6] == '('))
      return false;
    return true;
  }
  return true;
}

static bool ny_valid_native_artifact(const char *path) {
  if (!path || !*path)
    return false;
  struct stat st;
  if (stat(path, &st) != 0)
    return false;
  if (st.st_size <= 0)
    return false;
#ifndef _WIN32
  if (ny_access(path, X_OK) != 0)
    return false;
#endif
  FILE *f = fopen(path, "rb");
  if (!f)
    return false;
  unsigned char hdr[4] = {0, 0, 0, 0};
  size_t n = fread(hdr, 1, sizeof(hdr), f);
  fclose(f);
  if (n < 2)
    return false;
#ifdef _WIN32
  return hdr[0] == 'M' && hdr[1] == 'Z';
#else
  return n >= 4 && hdr[0] == 0x7f && hdr[1] == 'E' && hdr[2] == 'L' &&
         hdr[3] == 'F';
#endif
}

static const char *ny_dump_dir(const ny_options *opt) {
  if (opt && opt->dump_dir && *opt->dump_dir)
    return opt->dump_dir;
  return "build/debug";
}

static void ny_dump_path(char *out, size_t out_len, const ny_options *opt,
                         const char *name) {
  if (!out || out_len == 0) {
    return;
  }
  ny_join_path(out, out_len, ny_dump_dir(opt), name ? name : "");
}

static void ny_ensure_parent_dir_for_path(const char *path) {
  if (!path || !*path)
    return;
  char tmp[4096];
  snprintf(tmp, sizeof(tmp), "%s", path);
  char *slash = strrchr(tmp, '/');
#ifdef _WIN32
  char *bslash = strrchr(tmp, '\\');
  if (!slash || (bslash && bslash > slash))
    slash = bslash;
#endif
  if (!slash)
    return;
  if (slash == tmp)
    return;
  *slash = '\0';
  ny_ensure_dir_recursive(tmp);
}

static void ny_write_ir_stats_file(const ny_options *opt, const char *name,
                                   LLVMModuleRef module) {
  if (!module || !name || !*name)
    return;
  ny_ir_stats_t st = {0};
  ny_collect_ir_stats(module, &st);
  char path[4096];
  ny_dump_path(path, sizeof(path), opt, name);
  char buf[1024];
  int n = snprintf(
      buf, sizeof(buf),
      "scope=%s\nfuncs=%" PRIu64 "\nblocks=%" PRIu64 "\ninsts=%" PRIu64
      "\nallocas=%" PRIu64 "\nphis=%" PRIu64 "\n",
      (opt && opt->dump_scope == NY_DUMP_SCOPE_LIB)
          ? "lib"
          : ((opt && opt->dump_scope == NY_DUMP_SCOPE_BOTH) ? "both"
                                                            : "program"),
      st.funcs, st.blocks, st.insts, st.allocas, st.phis);
  if (n > 0)
    ny_write_file(path, buf, (size_t)n);
}

static const char *ny_stage_name(ny_stop_after_stage_t stage) {
  switch (stage) {
  case NY_STOP_AFTER_PARSE:
    return "parse";
  case NY_STOP_AFTER_HM:
    return "hm";
  case NY_STOP_AFTER_TRAIT:
    return "trait";
  case NY_STOP_AFTER_FLOW:
    return "flow";
  case NY_STOP_AFTER_ABI:
    return "abi";
  case NY_STOP_AFTER_OPT:
    return "opt";
  case NY_STOP_AFTER_NONE:
  default:
    return "none";
  }
}

static const char *ny_stage_schema(ny_stop_after_stage_t stage) {
  switch (stage) {
  case NY_STOP_AFTER_PARSE:
    return "ast.v1";
  case NY_STOP_AFTER_HM:
    return "typed_ast.v1";
  case NY_STOP_AFTER_TRAIT:
    return "resolved.v1";
  case NY_STOP_AFTER_FLOW:
    return "refined.v1";
  case NY_STOP_AFTER_ABI:
    return "lowered.v1";
  case NY_STOP_AFTER_OPT:
    return "optimized.v1";
  case NY_STOP_AFTER_NONE:
  default:
    return "artifact.v1";
  }
}

static const char *ny_stage_default_file(ny_stop_after_stage_t stage) {
  switch (stage) {
  case NY_STOP_AFTER_PARSE:
    return "ast.v1.json";
  case NY_STOP_AFTER_HM:
    return "typed_ast.v1.json";
  case NY_STOP_AFTER_TRAIT:
    return "resolved.v1.json";
  case NY_STOP_AFTER_FLOW:
    return "refined.v1.json";
  case NY_STOP_AFTER_ABI:
    return "lowered.v1.json";
  case NY_STOP_AFTER_OPT:
    return "optimized.v1.json";
  case NY_STOP_AFTER_NONE:
  default:
    return "artifact.v1.json";
  }
}

static bool ny_stop_after_is(const ny_options *opt,
                             ny_stop_after_stage_t stage) {
  return opt && opt->stop_after == stage;
}

static void ny_stage_append(char **buf, size_t *len, size_t *cap,
                            const char *fmt, ...) {
  if (!buf || !len || !cap || !fmt)
    return;
  if (!*buf) {
    *cap = 2048;
    *buf = malloc(*cap);
    if (!*buf)
      return;
    (*buf)[0] = '\0';
    *len = 0;
  }
  for (;;) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*buf + *len, *cap - *len, fmt, ap);
    va_end(ap);
    if (n < 0)
      return;
    if (*len + (size_t)n < *cap) {
      *len += (size_t)n;
      return;
    }
    size_t new_cap = *cap * 2 + (size_t)n + 1;
    char *tmp = realloc(*buf, new_cap);
    if (!tmp)
      return;
    *buf = tmp;
    *cap = new_cap;
  }
}

static void ny_stage_json_str(char **buf, size_t *len, size_t *cap,
                              const char *s) {
  ny_stage_append(buf, len, cap, "\"");
  if (s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
      switch (*p) {
      case '"':
        ny_stage_append(buf, len, cap, "\\\"");
        break;
      case '\\':
        ny_stage_append(buf, len, cap, "\\\\");
        break;
      case '\n':
        ny_stage_append(buf, len, cap, "\\n");
        break;
      case '\r':
        ny_stage_append(buf, len, cap, "\\r");
        break;
      case '\t':
        ny_stage_append(buf, len, cap, "\\t");
        break;
      default:
        if (*p < 32)
          ny_stage_append(buf, len, cap, "\\u%04x", (unsigned)*p);
        else
          ny_stage_append(buf, len, cap, "%c", *p);
        break;
      }
    }
  }
  ny_stage_append(buf, len, cap, "\"");
}

static void ny_stage_append_raw_json(char **buf, size_t *len, size_t *cap,
                                     const char *json) {
  ny_stage_append(buf, len, cap, "%s", (json && *json) ? json : "null");
}

static bool ny_stage_write_default_artifact(const ny_options *opt,
                                            const char *default_name,
                                            const char *json) {
  char path[4096];
  ny_dump_path(path, sizeof(path), opt,
               default_name ? default_name : "artifact.v1.json");
  ny_ensure_parent_dir_for_path(path);
  return ny_write_file(path, json, strlen(json)) == 0;
}

static bool ny_stage_write_artifact(const ny_options *opt,
                                    const char *default_name, const char *json,
                                    bool stdout_if_stopping) {
  if (!json)
    return false;
  if (opt && opt->emit_artifact_path && *opt->emit_artifact_path) {
    ny_ensure_parent_dir_for_path(opt->emit_artifact_path);
    return ny_write_file(opt->emit_artifact_path, json, strlen(json)) == 0;
  }
  if (stdout_if_stopping && opt && opt->stop_after != NY_STOP_AFTER_NONE) {
    fputs(json, stdout);
    if (json[0] && json[strlen(json) - 1] != '\n')
      fputc('\n', stdout);
    return true;
  }
  return ny_stage_write_default_artifact(opt, default_name, json);
}

static char *ny_stage_errors_json(ny_stop_after_stage_t stage,
                                  const char *source_name, const char *message,
                                  int count) {
  char *buf = NULL;
  size_t len = 0, cap = 0;
  ny_stage_append(&buf, &len, &cap, "{\"schema\":\"errors.v1\",\"stage\":");
  ny_stage_json_str(&buf, &len, &cap, ny_stage_name(stage));
  ny_stage_append(&buf, &len, &cap, ",\"source\":");
  ny_stage_json_str(&buf, &len, &cap, source_name ? source_name : "");
  ny_stage_append(&buf, &len, &cap,
                  ",\"error_count\":%d,\"errors\":[{\"stage\":", count);
  ny_stage_json_str(&buf, &len, &cap, ny_stage_name(stage));
  ny_stage_append(&buf, &len, &cap, ",\"code\":");
  ny_stage_json_str(&buf, &len, &cap, "stage-failed");
  ny_stage_append(&buf, &len, &cap, ",\"message\":");
  ny_stage_json_str(&buf, &len, &cap, message ? message : "stage failed");
  ny_stage_append(&buf, &len, &cap, "}]}");
  return buf ? buf : ny_strdup("{\"schema\":\"errors.v1\",\"errors\":[]}");
}

static void ny_stage_maybe_emit_errors(const ny_options *opt,
                                       ny_stop_after_stage_t stage,
                                       const char *source_name,
                                       const char *message, int count) {
  if (!opt || !opt->collect_errors)
    return;
  char *json = ny_stage_errors_json(stage, source_name, message, count);
  if (!json)
    return;
  ny_stage_write_default_artifact(opt, "errors.v1.json", json);
  free(json);
}

static bool ny_stage_stmt_in_dump_scope(const ny_options *opt, const stmt_t *s);

static bool ny_stage_expr_ident_literal_eq(expr_t *e, const char **out_name) {
  if (!e || e->kind != NY_E_BINARY || !e->as.binary.op ||
      strcmp(e->as.binary.op, "==") != 0)
    return false;
  expr_t *l = e->as.binary.left;
  expr_t *r = e->as.binary.right;
  if (l && r && l->kind == NY_E_IDENT && r->kind == NY_E_LITERAL) {
    if (out_name)
      *out_name = l->as.ident.name;
    return true;
  }
  if (l && r && r->kind == NY_E_IDENT && l->kind == NY_E_LITERAL) {
    if (out_name)
      *out_name = r->as.ident.name;
    return true;
  }
  return false;
}

static bool ny_stage_collect_eq_or(expr_t *e, const char **name,
                                   size_t *terms) {
  if (!e || !name || !terms)
    return false;
  if (e->kind == NY_E_LOGICAL && e->as.logical.op &&
      strcmp(e->as.logical.op, "||") == 0) {
    return ny_stage_collect_eq_or(e->as.logical.left, name, terms) &&
           ny_stage_collect_eq_or(e->as.logical.right, name, terms);
  }
  const char *leaf_name = NULL;
  if (!ny_stage_expr_ident_literal_eq(e, &leaf_name) || !leaf_name)
    return false;
  if (!*name)
    *name = leaf_name;
  if (strcmp(*name, leaf_name) != 0)
    return false;
  (*terms)++;
  return true;
}

static size_t ny_stage_flow_eqset_candidates_expr(expr_t *e);

static size_t ny_stage_flow_eqset_candidates_args(ny_call_arg_list *args) {
  size_t n = 0;
  for (size_t i = 0; args && i < args->len; ++i)
    n += ny_stage_flow_eqset_candidates_expr(args->data[i].val);
  return n;
}

static size_t ny_stage_flow_eqset_candidates_expr(expr_t *e) {
  if (!e)
    return 0;
  const char *name = NULL;
  size_t terms = 0;
  if (ny_stage_collect_eq_or(e, &name, &terms) && terms >= 3)
    return 1;
  size_t n = 0;
  switch (e->kind) {
  case NY_E_UNARY:
    n += ny_stage_flow_eqset_candidates_expr(e->as.unary.right);
    break;
  case NY_E_BINARY:
    n += ny_stage_flow_eqset_candidates_expr(e->as.binary.left);
    n += ny_stage_flow_eqset_candidates_expr(e->as.binary.right);
    break;
  case NY_E_LOGICAL:
    n += ny_stage_flow_eqset_candidates_expr(e->as.logical.left);
    n += ny_stage_flow_eqset_candidates_expr(e->as.logical.right);
    break;
  case NY_E_TERNARY:
    n += ny_stage_flow_eqset_candidates_expr(e->as.ternary.cond);
    n += ny_stage_flow_eqset_candidates_expr(e->as.ternary.true_expr);
    n += ny_stage_flow_eqset_candidates_expr(e->as.ternary.false_expr);
    break;
  case NY_E_CALL:
    n += ny_stage_flow_eqset_candidates_expr(e->as.call.callee);
    n += ny_stage_flow_eqset_candidates_args(&e->as.call.args);
    break;
  case NY_E_MEMCALL:
    n += ny_stage_flow_eqset_candidates_expr(e->as.memcall.target);
    n += ny_stage_flow_eqset_candidates_args(&e->as.memcall.args);
    break;
  case NY_E_INDEX:
    n += ny_stage_flow_eqset_candidates_expr(e->as.index.target);
    n += ny_stage_flow_eqset_candidates_expr(e->as.index.start);
    n += ny_stage_flow_eqset_candidates_expr(e->as.index.stop);
    n += ny_stage_flow_eqset_candidates_expr(e->as.index.step);
    break;
  case NY_E_MEMBER:
    n += ny_stage_flow_eqset_candidates_expr(e->as.member.target);
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      n += ny_stage_flow_eqset_candidates_expr(e->as.list_like.data[i]);
    break;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      n += ny_stage_flow_eqset_candidates_expr(e->as.dict.pairs.data[i].key);
      n += ny_stage_flow_eqset_candidates_expr(e->as.dict.pairs.data[i].value);
    }
    break;
  case NY_E_MATCH:
    n += ny_stage_flow_eqset_candidates_expr(e->as.match.test);
    for (size_t i = 0; i < e->as.match.arms.len; ++i) {
      match_arm_t *arm = &e->as.match.arms.data[i];
      n += ny_stage_flow_eqset_candidates_expr(arm->guard);
    }
    break;
  case NY_E_FN:
  case NY_E_LAMBDA:
  case NY_E_COMPTIME:
  case NY_E_LITERAL:
  case NY_E_IDENT:
  case NY_E_ASM:
  case NY_E_FSTRING:
  case NY_E_INFERRED_MEMBER:
  case NY_E_EMBED:
  case NY_E_PTR_TYPE:
  case NY_E_DEREF:
  case NY_E_SIZEOF:
  case NY_E_TRY:
  default:
    break;
  }
  return n;
}

static size_t ny_stage_flow_eqset_candidates_stmt(stmt_t *s) {
  if (!s)
    return 0;
  size_t n = 0;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      n += ny_stage_flow_eqset_candidates_stmt(s->as.block.body.data[i]);
    break;
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; ++i)
      n += ny_stage_flow_eqset_candidates_expr(s->as.var.exprs.data[i]);
    break;
  case NY_S_EXPR:
    n += ny_stage_flow_eqset_candidates_expr(s->as.expr.expr);
    break;
  case NY_S_IF:
    n += ny_stage_flow_eqset_candidates_expr(s->as.iff.test);
    n += ny_stage_flow_eqset_candidates_stmt(s->as.iff.init);
    n += ny_stage_flow_eqset_candidates_stmt(s->as.iff.conseq);
    n += ny_stage_flow_eqset_candidates_stmt(s->as.iff.alt);
    break;
  case NY_S_WHILE:
    n += ny_stage_flow_eqset_candidates_expr(s->as.whl.test);
    n += ny_stage_flow_eqset_candidates_stmt(s->as.whl.init);
    n += ny_stage_flow_eqset_candidates_stmt(s->as.whl.body);
    n += ny_stage_flow_eqset_candidates_stmt(s->as.whl.update);
    break;
  case NY_S_FOR:
    n += ny_stage_flow_eqset_candidates_stmt(s->as.fr.init);
    n += ny_stage_flow_eqset_candidates_expr(s->as.fr.cond);
    n += ny_stage_flow_eqset_candidates_expr(s->as.fr.iterable);
    n += ny_stage_flow_eqset_candidates_stmt(s->as.fr.body);
    n += ny_stage_flow_eqset_candidates_stmt(s->as.fr.update);
    break;
  case NY_S_FUNC:
    n += ny_stage_flow_eqset_candidates_stmt(s->as.fn.body);
    break;
  case NY_S_RETURN:
    n += ny_stage_flow_eqset_candidates_expr(s->as.ret.value);
    break;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      n += ny_stage_flow_eqset_candidates_stmt(s->as.module.body.data[i]);
    break;
  case NY_S_IMPL:
    for (size_t i = 0; i < s->as.impl.methods.len; ++i)
      n += ny_stage_flow_eqset_candidates_stmt(s->as.impl.methods.data[i]);
    break;
  case NY_S_LAYOUT:
    for (size_t i = 0; i < s->as.layout.methods.len; ++i)
      n += ny_stage_flow_eqset_candidates_stmt(s->as.layout.methods.data[i]);
    break;
  case NY_S_STRUCT:
    for (size_t i = 0; i < s->as.struc.methods.len; ++i)
      n += ny_stage_flow_eqset_candidates_stmt(s->as.struc.methods.data[i]);
    break;
  case NY_S_MATCH:
    n += ny_stage_flow_eqset_candidates_expr(s->as.match.test);
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      n += ny_stage_flow_eqset_candidates_stmt(s->as.match.arms.data[i].conseq);
    n += ny_stage_flow_eqset_candidates_stmt(s->as.match.default_conseq);
    break;
  default:
    break;
  }
  return n;
}

static size_t ny_stage_flow_eqset_candidates_program(const ny_options *opt,
                                                     program_t *prog) {
  size_t n = 0;
  if (!prog)
    return 0;
  for (size_t i = 0; i < prog->body.len; ++i) {
    if (!ny_stage_stmt_in_dump_scope(opt, prog->body.data[i]))
      continue;
    n += ny_stage_flow_eqset_candidates_stmt(prog->body.data[i]);
  }
  return n;
}

static bool ny_stage_stmt_in_dump_scope(const ny_options *opt,
                                        const stmt_t *s) {
  ny_dump_scope_t scope = opt ? opt->dump_scope : NY_DUMP_SCOPE_PROGRAM;
  if (!s)
    return scope == NY_DUMP_SCOPE_BOTH;
  bool is_std = ny_is_stdlib_tok(s->tok);
  if (scope == NY_DUMP_SCOPE_BOTH)
    return true;
  if (scope == NY_DUMP_SCOPE_LIB)
    return is_std;
  return !is_std;
}

static void ny_stage_append_shapes_json(char **buf, size_t *len, size_t *cap,
                                        const ny_options *opt, codegen_t *cg,
                                        LLVMModuleRef module) {
  ny_stage_append(buf, len, cap, "{\"functions\":[");
  size_t emitted = 0;
  for (size_t i = 0; cg && i < cg->fun_sigs.len; ++i) {
    fun_sig *sig = &cg->fun_sigs.data[i];
    if (!ny_stage_stmt_in_dump_scope(opt, sig->stmt_t))
      continue;
    if (emitted++)
      ny_stage_append(buf, len, cap, ",");
    ny_stage_append(buf, len, cap, "{\"name\":");
    ny_stage_json_str(buf, len, cap, sig->name ? sig->name : "");
    ny_stage_append(buf, len, cap, ",\"arity\":%d,\"return\":", sig->arity);
    ny_stage_json_str(
        buf, len, cap,
        sig->return_type
            ? sig->return_type
            : (sig->inferred_return_type ? sig->inferred_return_type : ""));
    ny_stage_append(
        buf, len, cap,
        ",\"extern\":%s,\"native_abi\":%s,\"effects\":%u,\"effects_known\":%s}",
        sig->is_extern ? "true" : "false",
        sig->is_native_abi ? "true" : "false", sig->effects,
        sig->effects_known ? "true" : "false");
  }
  ny_stage_append(buf, len, cap, "],\"globals\":[");
  emitted = 0;
  for (size_t i = 0; cg && i < cg->global_vars.len; ++i) {
    binding *b = &cg->global_vars.data[i];
    if (!ny_stage_stmt_in_dump_scope(opt, b->stmt_t))
      continue;
    if (emitted++)
      ny_stage_append(buf, len, cap, ",");
    ny_stage_append(buf, len, cap, "{\"name\":");
    ny_stage_json_str(buf, len, cap, b->name ? b->name : "");
    ny_stage_append(buf, len, cap, ",\"type\":");
    ny_stage_json_str(buf, len, cap, b->type_name ? b->type_name : "");
    ny_stage_append(
        buf, len, cap, ",\"int\":%s,\"f64\":%s,\"slot\":%s,\"escapes\":%s}",
        (b->is_int_slot || b->is_int_direct) ? "true" : "false",
        (b->is_f64_slot || b->is_f64_direct) ? "true" : "false",
        b->is_slot ? "true" : "false", b->escapes ? "true" : "false");
  }
  ny_stage_append(buf, len, cap, "],\"layouts\":[");
  emitted = 0;
  for (size_t i = 0; cg && i < cg->layouts.len; ++i) {
    layout_def_t *layout = cg->layouts.data[i];
    if (!ny_stage_stmt_in_dump_scope(opt, layout ? layout->stmt : NULL))
      continue;
    if (emitted++)
      ny_stage_append(buf, len, cap, ",");
    ny_stage_append(buf, len, cap, "{\"name\":");
    ny_stage_json_str(buf, len, cap,
                      layout && layout->name ? layout->name : "");
    ny_stage_append(
        buf, len, cap,
        ",\"size\":%zu,\"align\":%zu,\"pack\":%zu,\"layout\":%s,\"fields\":[",
        layout ? layout->size : 0, layout ? layout->align : 0,
        layout ? layout->pack : 0,
        layout && layout->is_layout ? "true" : "false");
    for (size_t j = 0; layout && j < layout->fields.len; ++j) {
      layout_field_info_t *field = &layout->fields.data[j];
      if (j)
        ny_stage_append(buf, len, cap, ",");
      ny_stage_append(buf, len, cap, "{\"name\":");
      ny_stage_json_str(buf, len, cap, field->name ? field->name : "");
      ny_stage_append(buf, len, cap, ",\"type\":");
      ny_stage_json_str(buf, len, cap,
                        field->type_name ? field->type_name : "");
      ny_stage_append(buf, len, cap,
                      ",\"offset\":%zu,\"size\":%zu,\"align\":%zu}",
                      field->offset, field->size, field->align);
    }
    ny_stage_append(buf, len, cap, "]}");
  }
  ny_stage_append(buf, len, cap, "],\"operators\":[");
  emitted = 0;
  for (size_t i = 0; cg && i < cg->operators.len; ++i) {
    ny_operator_def_t *op = &cg->operators.data[i];
    if (!ny_stage_stmt_in_dump_scope(opt, op->stmt))
      continue;
    if (emitted++)
      ny_stage_append(buf, len, cap, ",");
    ny_stage_append(buf, len, cap, "{\"op\":");
    ny_stage_json_str(buf, len, cap, op->op ? op->op : "");
    ny_stage_append(buf, len, cap, ",\"left\":");
    ny_stage_json_str(buf, len, cap, op->left_type ? op->left_type : "");
    ny_stage_append(buf, len, cap, ",\"right\":");
    ny_stage_json_str(buf, len, cap, op->right_type ? op->right_type : "");
    ny_stage_append(buf, len, cap, ",\"return\":");
    ny_stage_json_str(buf, len, cap, op->return_type ? op->return_type : "");
    ny_stage_append(buf, len, cap, ",\"target\":");
    ny_stage_json_str(buf, len, cap, op->target_name ? op->target_name : "");
    ny_stage_append(buf, len, cap, "}");
  }
  ny_stage_append(buf, len, cap, "],\"tagged_types\":[");
  emitted = 0;
  for (size_t i = 0; cg && i < cg->tagged_types.len; ++i) {
    const char *tag = cg->tagged_types.data[i];
    bool is_std_tag = tag && strncmp(tag, "std.", 4) == 0;
    ny_dump_scope_t scope = opt ? opt->dump_scope : NY_DUMP_SCOPE_PROGRAM;
    if ((scope == NY_DUMP_SCOPE_PROGRAM && is_std_tag) ||
        (scope == NY_DUMP_SCOPE_LIB && !is_std_tag))
      continue;
    if (emitted++)
      ny_stage_append(buf, len, cap, ",");
    ny_stage_json_str(buf, len, cap, tag);
  }
  ny_stage_append(buf, len, cap, "],\"monomorphizations\":[");
  for (size_t i = 0; cg && i < cg->mono_specs.len; ++i) {
    ny_mono_specialization_t *spec = &cg->mono_specs.data[i];
    if (i)
      ny_stage_append(buf, len, cap, ",");
    ny_stage_append(buf, len, cap, "{\"base\":");
    ny_stage_json_str(buf, len, cap, spec->base_name ? spec->base_name : "");
    ny_stage_append(buf, len, cap, ",\"specialized\":");
    ny_stage_json_str(buf, len, cap,
                      spec->specialized_name ? spec->specialized_name : "");
    ny_stage_append(buf, len, cap, ",\"arity\":%d}", spec->arity);
  }
  ny_stage_append(buf, len, cap, "]");
  if (module) {
    ny_ir_stats_t st = {0};
    ny_collect_ir_stats(module, &st);
    ny_stage_append(buf, len, cap,
                    ",\"ir_stats\":{\"funcs\":%" PRIu64 ",\"blocks\":%" PRIu64
                    ",\"insts\":%" PRIu64 ",\"allocas\":%" PRIu64
                    ",\"phis\":%" PRIu64 "}",
                    st.funcs, st.blocks, st.insts, st.allocas, st.phis);
  }
  ny_stage_append(buf, len, cap, "}");
}

static char *ny_stage_artifact_json(const ny_options *opt,
                                    ny_stop_after_stage_t stage,
                                    program_t *prog, codegen_t *cg,
                                    const char *source_name,
                                    LLVMModuleRef module) {
  char *buf = NULL;
  size_t len = 0, cap = 0;
  char *ast_json = NULL;
  char *type_json = NULL;
  char *resolved_json = NULL;
  char *refined_json = NULL;
  char *lowered_json = NULL;
  ny_stage_append(&buf, &len, &cap, "{\"schema\":");
  ny_stage_json_str(&buf, &len, &cap, ny_stage_schema(stage));
  ny_stage_append(&buf, &len, &cap, ",\"stage\":");
  ny_stage_json_str(&buf, &len, &cap, ny_stage_name(stage));
  ny_stage_append(&buf, &len, &cap, ",\"source\":");
  ny_stage_json_str(&buf, &len, &cap, source_name ? source_name : "");
  ny_stage_append(
      &buf, &len, &cap,
      ",\"pipeline\":[\"parse\",\"hm\",\"trait\",\"flow\",\"abi\",\"opt\"]");
  ny_stage_append(
      &buf, &len, &cap,
      ",\"type_groups\":{\"number\":[\"int\",\"i8\",\"i16\",\"i32\",\"i64\","
      "\"i128\",\"u8\",\"u16\",\"u32\",\"u64\",\"u128\",\"f32\",\"f64\","
      "\"f128\",\"bigint\"],\"numeric\":[\"int\",\"i8\",\"i16\",\"i32\","
      "\"i64\",\"i128\",\"u8\",\"u16\",\"u32\",\"u64\",\"u128\",\"f32\","
      "\"f64\",\"f128\",\"bigint\"],\"integer\":[\"int\",\"i8\",\"i16\","
      "\"i32\","
      "\"i64\",\"i128\",\"u8\",\"u16\",\"u32\",\"u64\",\"u128\",\"bigint\"],"
      "\"float\":[\"f32\",\"f64\",\"f128\"],\"scalar\":[\"number\",\"bool\","
      "\"str\",\"char\",\"complex\",\"c64\",\"c128\"],\"seq\":[\"list\","
      "\"tuple\",\"str\",\"bytes\",\"range\"],\"sequence\":[\"seq\"],"
      "\"iterable\":[\"seq\",\"set\",\"dict\",\"bytes\"],\"indexable\":"
      "[\"seq\",\"dict\",\"bytes\"],\"allocator\":[\"ptr\",\"handle\"]}");
  if (prog) {
    ast_json = ny_ast_to_json_filtered(prog, source_name);
    ny_stage_append(&buf, &len, &cap, ",\"ast\":");
    ny_stage_append_raw_json(&buf, &len, &cap, ast_json ? ast_json : "[]");
    char *symbols_json = ny_ast_symbols_to_json_filtered(prog, source_name);
    ny_stage_append(&buf, &len, &cap, ",\"symbols\":");
    ny_stage_append_raw_json(&buf, &len, &cap,
                             symbols_json ? symbols_json : "[]");
    if (symbols_json)
      rt_free((int64_t)(uintptr_t)symbols_json);
  }
  if (stage >= NY_STOP_AFTER_HM && prog) {
    type_json = ny_type_pipeline_typed_json(
        prog, cg, source_name, opt && opt->dump_scope != NY_DUMP_SCOPE_PROGRAM);
    ny_stage_append(&buf, &len, &cap, ",\"typed\":");
    ny_stage_append_raw_json(&buf, &len, &cap, type_json ? type_json : "{}");
  }
  if (stage >= NY_STOP_AFTER_TRAIT && prog) {
    resolved_json = ny_type_pipeline_resolved_json(
        prog, cg, source_name, opt && opt->dump_scope != NY_DUMP_SCOPE_PROGRAM);
    ny_stage_append(&buf, &len, &cap, ",\"resolved\":");
    ny_stage_append_raw_json(&buf, &len, &cap,
                             resolved_json ? resolved_json : "{}");
  }
  if (stage >= NY_STOP_AFTER_FLOW && prog) {
    refined_json = ny_type_pipeline_refined_json(
        prog, cg, source_name, opt && opt->dump_scope != NY_DUMP_SCOPE_PROGRAM);
    ny_stage_append(
        &buf, &len, &cap,
        ",\"flow\":{\"eq_or_candidates\":%zu,"
        "\"range_facts\":\"shared refined.v1 range_refinements/index_proofs\"}",
        ny_stage_flow_eqset_candidates_program(opt, prog));
    ny_stage_append(&buf, &len, &cap, ",\"refined\":");
    ny_stage_append_raw_json(&buf, &len, &cap,
                             refined_json ? refined_json : "{}");
  }
  if (stage >= NY_STOP_AFTER_ABI && prog) {
    lowered_json = ny_type_pipeline_lowered_json(
        prog, cg, source_name, opt && opt->dump_scope != NY_DUMP_SCOPE_PROGRAM);
    ny_stage_append(&buf, &len, &cap, ",\"lowered\":");
    ny_stage_append_raw_json(&buf, &len, &cap,
                             lowered_json ? lowered_json : "{}");
  }
  if ((opt && opt->emit_shapes) || stage >= NY_STOP_AFTER_TRAIT || cg) {
    ny_stage_append(&buf, &len, &cap, ",\"shapes\":");
    ny_stage_append_shapes_json(&buf, &len, &cap, opt, cg, module);
  }
  ny_stage_append(&buf, &len, &cap, ",\"errors\":[]}");
  if (ast_json)
    rt_free((int64_t)(uintptr_t)ast_json);
  free(type_json);
  free(resolved_json);
  free(refined_json);
  free(lowered_json);
  return buf ? buf : ny_strdup("{\"schema\":\"artifact.v1\"}");
}

static bool ny_stage_emit_artifact(const ny_options *opt,
                                   ny_stop_after_stage_t stage, program_t *prog,
                                   codegen_t *cg, const char *source_name,
                                   LLVMModuleRef module,
                                   bool stdout_if_stopping) {
  char *json =
      ny_stage_artifact_json(opt, stage, prog, cg, source_name, module);
  if (!json)
    return false;
  bool ok = ny_stage_write_artifact(opt, ny_stage_default_file(stage), json,
                                    stdout_if_stopping);
  free(json);
  return ok;
}

typedef struct ny_safe_raw_ptr_fact_t {
  const char *name;
  int64_t size;
} ny_safe_raw_ptr_fact_t;

typedef struct ny_safe_raw_int_fact_t {
  const char *name;
  int64_t min;
  int64_t max;
} ny_safe_raw_int_fact_t;

typedef VEC(ny_safe_raw_ptr_fact_t) ny_safe_raw_ptr_fact_list;
typedef VEC(ny_safe_raw_int_fact_t) ny_safe_raw_int_fact_list;

typedef struct ny_safe_raw_ctx_t {
  ny_safe_raw_ptr_fact_list ptrs;
  ny_safe_raw_int_fact_list ints;
  bool ok;
} ny_safe_raw_ctx_t;

typedef struct ny_safe_raw_scope_t {
  size_t ptr_len;
  size_t int_len;
} ny_safe_raw_scope_t;

static void ny_safe_raw_validate_expr(ny_safe_raw_ctx_t *ctx, expr_t *e);
static void ny_safe_raw_validate_stmt(ny_safe_raw_ctx_t *ctx, stmt_t *s);

static ny_safe_raw_scope_t ny_safe_raw_scope_mark(ny_safe_raw_ctx_t *ctx) {
  ny_safe_raw_scope_t mark = {0};
  if (ctx) {
    mark.ptr_len = ctx->ptrs.len;
    mark.int_len = ctx->ints.len;
  }
  return mark;
}

static void ny_safe_raw_scope_restore(ny_safe_raw_ctx_t *ctx,
                                      ny_safe_raw_scope_t mark) {
  if (!ctx)
    return;
  if (mark.ptr_len <= ctx->ptrs.len)
    ctx->ptrs.len = mark.ptr_len;
  if (mark.int_len <= ctx->ints.len)
    ctx->ints.len = mark.int_len;
}

static const char *ny_safe_raw_leaf(const char *name) {
  const char *tail = name ? strrchr(name, '.') : NULL;
  return tail ? tail + 1 : name;
}

static const char *ny_safe_raw_callee_leaf(expr_t *callee) {
  if (!callee)
    return NULL;
  if (callee->kind == NY_E_IDENT)
    return ny_safe_raw_leaf(callee->as.ident.name);
  if (callee->kind == NY_E_MEMBER)
    return ny_safe_raw_leaf(callee->as.member.name);
  return NULL;
}

static bool ny_safe_raw_name_is(const char *name, const char *want) {
  const char *leaf = ny_safe_raw_leaf(name);
  return leaf && want && strcmp(leaf, want) == 0;
}

static bool ny_safe_raw_call_name_is(expr_t *e, const char *want) {
  return e && e->kind == NY_E_CALL &&
         ny_safe_raw_name_is(ny_safe_raw_callee_leaf(e->as.call.callee), want);
}

static bool ny_safe_raw_is_malloc_like(const char *leaf, bool *size_arg_is_one) {
  if (size_arg_is_one)
    *size_arg_is_one = false;
  if (!leaf)
    return false;
  if (strcmp(leaf, "malloc") == 0 || strcmp(leaf, "malloc_raw") == 0 ||
      strcmp(leaf, "zalloc") == 0 || strcmp(leaf, "__malloc") == 0) {
    return true;
  }
  if (strcmp(leaf, "realloc") == 0) {
    if (size_arg_is_one)
      *size_arg_is_one = true;
    return true;
  }
  return false;
}

static bool ny_safe_raw_alloc_size(expr_t *e, int64_t *out) {
  if (!e || e->kind != NY_E_CALL)
    return false;
  const char *leaf = ny_safe_raw_callee_leaf(e->as.call.callee);
  bool size_arg_is_one = false;
  if (!ny_safe_raw_is_malloc_like(leaf, &size_arg_is_one))
    return false;
  size_t idx = size_arg_is_one ? 1u : 0u;
  if (e->as.call.args.len <= idx)
    return false;
  int64_t n = 0;
  if (!ny_expr_literal_i64(e->as.call.args.data[idx].val, &n) || n < 0)
    return false;
  if (out)
    *out = n;
  return true;
}

static void ny_safe_raw_push_ptr(ny_safe_raw_ctx_t *ctx, const char *name,
                                 int64_t size) {
  if (!ctx || !name || size < 0)
    return;
  ny_safe_raw_ptr_fact_t fact = {.name = name, .size = size};
  vec_push(&ctx->ptrs, fact);
}

static void ny_safe_raw_push_int(ny_safe_raw_ctx_t *ctx, const char *name,
                                 int64_t min, int64_t max) {
  if (!ctx || !name || min > max)
    return;
  ny_safe_raw_int_fact_t fact = {.name = name, .min = min, .max = max};
  vec_push(&ctx->ints, fact);
}

static bool ny_safe_raw_lookup_ptr(ny_safe_raw_ctx_t *ctx, const char *name,
                                   int64_t *out_size) {
  if (!ctx || !name)
    return false;
  for (size_t i = ctx->ptrs.len; i > 0; --i) {
    ny_safe_raw_ptr_fact_t *fact = &ctx->ptrs.data[i - 1];
    if (fact->name && strcmp(fact->name, name) == 0) {
      if (out_size)
        *out_size = fact->size;
      return true;
    }
  }
  return false;
}

static bool ny_safe_raw_lookup_int(ny_safe_raw_ctx_t *ctx, const char *name,
                                   int64_t *out_min, int64_t *out_max) {
  if (!ctx || !name)
    return false;
  for (size_t i = ctx->ints.len; i > 0; --i) {
    ny_safe_raw_int_fact_t *fact = &ctx->ints.data[i - 1];
    if (fact->name && strcmp(fact->name, name) == 0) {
      if (out_min)
        *out_min = fact->min;
      if (out_max)
        *out_max = fact->max;
      return true;
    }
  }
  return false;
}

static bool ny_safe_raw_expr_int_range(ny_safe_raw_ctx_t *ctx, expr_t *e,
                                       int64_t *out_min, int64_t *out_max) {
  int64_t lit = 0;
  if (ny_expr_literal_i64(e, &lit)) {
    if (out_min)
      *out_min = lit;
    if (out_max)
      *out_max = lit;
    return true;
  }
  if (!e)
    return false;
  if (e->kind == NY_E_IDENT)
    return ny_safe_raw_lookup_int(ctx, e->as.ident.name, out_min, out_max);
  if (e->kind == NY_E_UNARY && e->as.unary.op &&
      strcmp(e->as.unary.op, "-") == 0) {
    int64_t lo = 0, hi = 0;
    if (!ny_safe_raw_expr_int_range(ctx, e->as.unary.right, &lo, &hi))
      return false;
    if (hi == INT64_MIN || lo == INT64_MIN)
      return false;
    if (out_min)
      *out_min = -hi;
    if (out_max)
      *out_max = -lo;
    return true;
  }
  if (e->kind == NY_E_BINARY && e->as.binary.op) {
    int64_t lmin = 0, lmax = 0, rmin = 0, rmax = 0;
    if (!ny_safe_raw_expr_int_range(ctx, e->as.binary.left, &lmin, &lmax) ||
        !ny_safe_raw_expr_int_range(ctx, e->as.binary.right, &rmin, &rmax))
      return false;
    const char *op = e->as.binary.op;
    if (strcmp(op, "+") == 0) {
      if ((rmin > 0 && lmin > INT64_MAX - rmin) ||
          (rmax < 0 && lmax < INT64_MIN - rmax))
        return false;
      if (out_min)
        *out_min = lmin + rmin;
      if (out_max)
        *out_max = lmax + rmax;
      return true;
    }
    if (strcmp(op, "-") == 0) {
      if ((rmax < 0 && lmin > INT64_MAX + rmax) ||
          (rmin > 0 && lmax < INT64_MIN + rmin))
        return false;
      if (out_min)
        *out_min = lmin - rmax;
      if (out_max)
        *out_max = lmax - rmin;
      return true;
    }
    if (strcmp(op, "*") == 0 && lmin == lmax && rmin == rmax) {
      if (lmin != 0 &&
          (rmin > INT64_MAX / lmin || rmin < INT64_MIN / lmin))
        return false;
      int64_t v = lmin * rmin;
      if (out_min)
        *out_min = v;
      if (out_max)
        *out_max = v;
      return true;
    }
  }
  return false;
}

static int ny_safe_raw_width_for_load(const char *leaf) {
  if (!leaf)
    return 0;
  if (strcmp(leaf, "load8") == 0 || strcmp(leaf, "__load8_idx") == 0)
    return 1;
  if (strcmp(leaf, "load16") == 0 || strcmp(leaf, "__load16_idx") == 0)
    return 2;
  if (strcmp(leaf, "load32") == 0 || strcmp(leaf, "load32_h") == 0 ||
      strcmp(leaf, "load32_f32") == 0 || strcmp(leaf, "__load32_idx") == 0 ||
      strcmp(leaf, "__load32_h") == 0)
    return 4;
  if (strcmp(leaf, "load64") == 0 || strcmp(leaf, "load64_h") == 0 ||
      strcmp(leaf, "load64_i") == 0 || strcmp(leaf, "load64_f64") == 0 ||
      strcmp(leaf, "__load64_idx") == 0 || strcmp(leaf, "__load64_h") == 0)
    return 8;
  return 0;
}

static int ny_safe_raw_width_for_store(const char *leaf, bool *intrinsic) {
  if (intrinsic)
    *intrinsic = false;
  if (!leaf)
    return 0;
  if (strncmp(leaf, "__store", 7) == 0 && intrinsic)
    *intrinsic = true;
  if (strcmp(leaf, "store8") == 0 || strcmp(leaf, "__store8_idx") == 0)
    return 1;
  if (strcmp(leaf, "store16") == 0 || strcmp(leaf, "__store16_idx") == 0)
    return 2;
  if (strcmp(leaf, "store32") == 0 || strcmp(leaf, "store32_h") == 0 ||
      strcmp(leaf, "store32_f32") == 0 ||
      strcmp(leaf, "__store32_idx") == 0)
    return 4;
  if (strcmp(leaf, "store64") == 0 || strcmp(leaf, "store64_h") == 0 ||
      strcmp(leaf, "store64_i") == 0 || strcmp(leaf, "store64_f64") == 0 ||
      strcmp(leaf, "__store64_idx") == 0)
    return 8;
  if (intrinsic)
    *intrinsic = false;
  return 0;
}

static bool ny_safe_raw_api_shape(expr_t *e, expr_t **out_ptr,
                                  expr_t **out_idx, int64_t *out_width) {
  if (out_ptr)
    *out_ptr = NULL;
  if (out_idx)
    *out_idx = NULL;
  if (out_width)
    *out_width = 0;
  if (!e || e->kind != NY_E_CALL)
    return false;
  const char *leaf = ny_safe_raw_callee_leaf(e->as.call.callee);
  int width = ny_safe_raw_width_for_load(leaf);
  if (width > 0) {
    if (e->as.call.args.len < 1)
      return false;
    if (out_ptr)
      *out_ptr = e->as.call.args.data[0].val;
    if (out_idx && e->as.call.args.len >= 2)
      *out_idx = e->as.call.args.data[1].val;
    if (out_width)
      *out_width = width;
    return true;
  }
  bool intrinsic_store = false;
  width = ny_safe_raw_width_for_store(leaf, &intrinsic_store);
  if (width > 0) {
    if (e->as.call.args.len < 2)
      return false;
    if (out_ptr)
      *out_ptr = e->as.call.args.data[0].val;
    if (out_idx) {
      size_t idx_pos = intrinsic_store ? 1u : 2u;
      if (e->as.call.args.len > idx_pos)
        *out_idx = e->as.call.args.data[idx_pos].val;
    }
    if (out_width)
      *out_width = width;
    return true;
  }
  return false;
}

static void ny_safe_raw_validate_call(ny_safe_raw_ctx_t *ctx, expr_t *e) {
  if (!ctx || !e || e->kind != NY_E_CALL || ny_is_stdlib_tok(e->tok))
    return;
  expr_t *ptr = NULL;
  expr_t *idx = NULL;
  int64_t width = 0;
  if (!ny_safe_raw_api_shape(e, &ptr, &idx, &width) || !ptr ||
      ptr->kind != NY_E_IDENT || width <= 0)
    return;
  int64_t alloc_size = 0;
  if (!ny_safe_raw_lookup_ptr(ctx, ptr->as.ident.name, &alloc_size))
    return;
  int64_t idx_min = 0;
  int64_t idx_max = 0;
  bool has_index = idx ? ny_safe_raw_expr_int_range(ctx, idx, &idx_min, &idx_max)
                       : true;
  if (!has_index) {
    ny_diag_error(idx ? idx->tok : e->tok,
                  "safe-mode raw memory access requires a proven byte range "
                  "for index");
    ctx->ok = false;
    return;
  }
  if (idx_min < 0 || width > alloc_size || idx_max > alloc_size - width) {
    ny_diag_error(idx ? idx->tok : e->tok,
                  "safe-mode raw memory access out of bounds");
    ctx->ok = false;
  }
}

static void ny_safe_raw_record_compile_range(ny_safe_raw_ctx_t *ctx,
                                             expr_t *e) {
  if (!ctx || !e || e->kind != NY_E_CALL ||
      !ny_safe_raw_call_name_is(e, "assert_compile_range") ||
      e->as.call.args.len < 3)
    return;
  expr_t *value = e->as.call.args.data[0].val;
  expr_t *lo_expr = e->as.call.args.data[1].val;
  expr_t *hi_expr = e->as.call.args.data[2].val;
  if (!value || value->kind != NY_E_IDENT)
    return;
  int64_t lo = 0;
  int64_t hi = 0;
  if (!ny_expr_literal_i64(lo_expr, &lo) || !ny_expr_literal_i64(hi_expr, &hi) ||
      lo > hi)
    return;
  ny_safe_raw_push_int(ctx, value->as.ident.name, lo, hi);
}

static void ny_safe_raw_validate_expr(ny_safe_raw_ctx_t *ctx, expr_t *e) {
  if (!ctx || !e)
    return;
  ny_safe_raw_validate_call(ctx, e);
  switch (e->kind) {
  case NY_E_UNARY:
    ny_safe_raw_validate_expr(ctx, e->as.unary.right);
    break;
  case NY_E_BINARY:
  case NY_E_LOGICAL:
    ny_safe_raw_validate_expr(ctx, e->as.binary.left);
    ny_safe_raw_validate_expr(ctx, e->as.binary.right);
    break;
  case NY_E_TERNARY:
    ny_safe_raw_validate_expr(ctx, e->as.ternary.cond);
    ny_safe_raw_validate_expr(ctx, e->as.ternary.true_expr);
    ny_safe_raw_validate_expr(ctx, e->as.ternary.false_expr);
    break;
  case NY_E_CALL:
    ny_safe_raw_validate_expr(ctx, e->as.call.callee);
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      ny_safe_raw_validate_expr(ctx, e->as.call.args.data[i].val);
    ny_safe_raw_record_compile_range(ctx, e);
    break;
  case NY_E_MEMCALL:
    ny_safe_raw_validate_expr(ctx, e->as.memcall.target);
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      ny_safe_raw_validate_expr(ctx, e->as.memcall.args.data[i].val);
    break;
  case NY_E_INDEX:
    ny_safe_raw_validate_expr(ctx, e->as.index.target);
    ny_safe_raw_validate_expr(ctx, e->as.index.start);
    ny_safe_raw_validate_expr(ctx, e->as.index.stop);
    ny_safe_raw_validate_expr(ctx, e->as.index.step);
    break;
  case NY_E_MEMBER:
    ny_safe_raw_validate_expr(ctx, e->as.member.target);
    break;
  case NY_E_PTR_TYPE:
    ny_safe_raw_validate_expr(ctx, e->as.ptr_type.target);
    break;
  case NY_E_DEREF:
    ny_safe_raw_validate_expr(ctx, e->as.deref.target);
    break;
  case NY_E_SIZEOF:
    if (!e->as.szof.is_type)
      ny_safe_raw_validate_expr(ctx, e->as.szof.target);
    break;
  case NY_E_TRY:
    ny_safe_raw_validate_expr(ctx, e->as.try_expr.target);
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      ny_safe_raw_validate_expr(ctx, e->as.list_like.data[i]);
    break;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      ny_safe_raw_validate_expr(ctx, e->as.dict.pairs.data[i].key);
      ny_safe_raw_validate_expr(ctx, e->as.dict.pairs.data[i].value);
    }
    break;
  case NY_E_COMPTIME:
    ny_safe_raw_validate_stmt(ctx, e->as.comptime_expr.body);
    break;
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; ++i) {
      if (e->as.fstring.parts.data[i].kind == NY_FSP_EXPR)
        ny_safe_raw_validate_expr(ctx, e->as.fstring.parts.data[i].as.e);
    }
    break;
  case NY_E_MATCH:
    ny_safe_raw_validate_expr(ctx, e->as.match.test);
    for (size_t i = 0; i < e->as.match.arms.len; ++i) {
      match_arm_t *arm = &e->as.match.arms.data[i];
      for (size_t j = 0; j < arm->patterns.len; ++j)
        ny_safe_raw_validate_expr(ctx, arm->patterns.data[j]);
      ny_safe_raw_validate_expr(ctx, arm->guard);
      ny_safe_raw_validate_stmt(ctx, arm->conseq);
    }
    ny_safe_raw_validate_stmt(ctx, e->as.match.default_conseq);
    break;
  case NY_E_ASM:
    for (size_t i = 0; i < e->as.as_asm.args.len; ++i)
      ny_safe_raw_validate_expr(ctx, e->as.as_asm.args.data[i]);
    break;
  case NY_E_LAMBDA:
  case NY_E_FN: {
    ny_safe_raw_scope_t mark = ny_safe_raw_scope_mark(ctx);
    ctx->ptrs.len = 0;
    ctx->ints.len = 0;
    ny_safe_raw_validate_stmt(ctx, e->as.lambda.body);
    ny_safe_raw_scope_restore(ctx, mark);
    break;
  }
  case NY_E_IDENT:
  case NY_E_LITERAL:
  case NY_E_INFERRED_MEMBER:
  case NY_E_EMBED:
    break;
  }
}

static void ny_safe_raw_record_var_facts(ny_safe_raw_ctx_t *ctx, stmt_t *s) {
  if (!ctx || !s || s->kind != NY_S_VAR)
    return;
  stmt_var_t *var = &s->as.var;
  if (!var->is_decl || var->is_mut)
    return;
  size_t n = var->names.len < var->exprs.len ? var->names.len : var->exprs.len;
  for (size_t i = 0; i < n; ++i) {
    const char *name = var->names.data[i];
    expr_t *init = var->exprs.data[i];
    int64_t alloc_size = 0;
    int64_t exact_int = 0;
    if (ny_safe_raw_alloc_size(init, &alloc_size))
      ny_safe_raw_push_ptr(ctx, name, alloc_size);
    if (ny_expr_literal_i64(init, &exact_int))
      ny_safe_raw_push_int(ctx, name, exact_int, exact_int);
  }
}

static void ny_safe_raw_validate_stmt_list(ny_safe_raw_ctx_t *ctx,
                                           ny_stmt_list *list) {
  if (!ctx || !list)
    return;
  for (size_t i = 0; i < list->len; ++i)
    ny_safe_raw_validate_stmt(ctx, list->data[i]);
}

static void ny_safe_raw_validate_stmt(ny_safe_raw_ctx_t *ctx, stmt_t *s) {
  if (!ctx || !s || ny_is_stdlib_tok(s->tok))
    return;
  switch (s->kind) {
  case NY_S_BLOCK: {
    ny_safe_raw_scope_t mark = ny_safe_raw_scope_mark(ctx);
    ny_safe_raw_validate_stmt_list(ctx, &s->as.block.body);
    ny_safe_raw_scope_restore(ctx, mark);
    break;
  }
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; ++i)
      ny_safe_raw_validate_expr(ctx, s->as.var.exprs.data[i]);
    ny_safe_raw_record_var_facts(ctx, s);
    break;
  case NY_S_EXPR:
    ny_safe_raw_validate_expr(ctx, s->as.expr.expr);
    break;
  case NY_S_RETURN:
    ny_safe_raw_validate_expr(ctx, s->as.ret.value);
    break;
  case NY_S_IF: {
    ny_safe_raw_scope_t mark = ny_safe_raw_scope_mark(ctx);
    ny_safe_raw_validate_stmt(ctx, s->as.iff.init);
    ny_safe_raw_validate_expr(ctx, s->as.iff.test);
    ny_safe_raw_validate_stmt(ctx, s->as.iff.conseq);
    ny_safe_raw_validate_stmt(ctx, s->as.iff.alt);
    ny_safe_raw_scope_restore(ctx, mark);
    break;
  }
  case NY_S_GUARD:
    ny_safe_raw_validate_expr(ctx, s->as.guard.value);
    ny_safe_raw_validate_stmt(ctx, s->as.guard.fallback);
    break;
  case NY_S_WHILE: {
    ny_safe_raw_scope_t mark = ny_safe_raw_scope_mark(ctx);
    ny_safe_raw_validate_stmt(ctx, s->as.whl.init);
    ny_safe_raw_validate_expr(ctx, s->as.whl.test);
    ny_safe_raw_validate_stmt(ctx, s->as.whl.body);
    ny_safe_raw_validate_stmt(ctx, s->as.whl.update);
    ny_safe_raw_scope_restore(ctx, mark);
    break;
  }
  case NY_S_FOR: {
    ny_safe_raw_scope_t mark = ny_safe_raw_scope_mark(ctx);
    ny_safe_raw_validate_stmt(ctx, s->as.fr.init);
    ny_safe_raw_validate_expr(ctx, s->as.fr.cond);
    ny_safe_raw_validate_expr(ctx, s->as.fr.iterable);
    ny_safe_raw_validate_stmt(ctx, s->as.fr.body);
    ny_safe_raw_validate_stmt(ctx, s->as.fr.update);
    ny_safe_raw_scope_restore(ctx, mark);
    break;
  }
  case NY_S_MATCH:
    ny_safe_raw_validate_expr(ctx, s->as.match.test);
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      match_arm_t *arm = &s->as.match.arms.data[i];
      ny_safe_raw_scope_t mark = ny_safe_raw_scope_mark(ctx);
      for (size_t j = 0; j < arm->patterns.len; ++j)
        ny_safe_raw_validate_expr(ctx, arm->patterns.data[j]);
      ny_safe_raw_validate_expr(ctx, arm->guard);
      ny_safe_raw_validate_stmt(ctx, arm->conseq);
      ny_safe_raw_scope_restore(ctx, mark);
    }
    ny_safe_raw_validate_stmt(ctx, s->as.match.default_conseq);
    break;
  case NY_S_TRY:
    ny_safe_raw_validate_stmt(ctx, s->as.tr.body);
    ny_safe_raw_validate_stmt(ctx, s->as.tr.handler);
    break;
  case NY_S_DEFER:
    ny_safe_raw_validate_stmt(ctx, s->as.de.body);
    break;
  case NY_S_FUNC: {
    ny_safe_raw_scope_t mark = ny_safe_raw_scope_mark(ctx);
    ctx->ptrs.len = 0;
    ctx->ints.len = 0;
    ny_safe_raw_validate_stmt(ctx, s->as.fn.body);
    ny_safe_raw_scope_restore(ctx, mark);
    break;
  }
  case NY_S_LAYOUT:
    ny_safe_raw_validate_stmt_list(ctx, &s->as.layout.methods);
    break;
  case NY_S_STRUCT:
    ny_safe_raw_validate_stmt_list(ctx, &s->as.struc.methods);
    break;
  case NY_S_IMPL:
    ny_safe_raw_validate_stmt_list(ctx, &s->as.impl.methods);
    break;
  case NY_S_MODULE:
    ny_safe_raw_validate_stmt_list(ctx, &s->as.module.body);
    break;
  case NY_S_MACRO: {
    ny_safe_raw_scope_t mark = ny_safe_raw_scope_mark(ctx);
    for (size_t i = 0; i < s->as.macro.args.len; ++i)
      ny_safe_raw_validate_expr(ctx, s->as.macro.args.data[i]);
    ny_safe_raw_validate_stmt(ctx, s->as.macro.body);
    ny_safe_raw_scope_restore(ctx, mark);
    break;
  }
  case NY_S_USE:
  case NY_S_EXTERN:
  case NY_S_LINK:
  case NY_S_LABEL:
  case NY_S_GOTO:
  case NY_S_BREAK:
  case NY_S_CONTINUE:
  case NY_S_EXPORT:
  case NY_S_ENUM:
  case NY_S_INCLUDE:
  case NY_S_DEFINE:
  case NY_S_OPERATOR:
    break;
  }
}

static bool ny_safe_mode_validate_raw_memory(program_t *prog) {
  if (!prog)
    return true;
  ny_safe_raw_ctx_t ctx = {0};
  ctx.ok = true;
  for (size_t i = 0; i < prog->body.len; ++i)
    ny_safe_raw_validate_stmt(&ctx, prog->body.data[i]);
  bool ok = ctx.ok;
  vec_free(&ctx.ptrs);
  vec_free(&ctx.ints);
  return ok;
}

static void dump_debug_bundle(const ny_options *opt, const char *source,
                              LLVMModuleRef module) {
  if (!opt || !opt->dump_on_error)
    return;
  ny_ensure_dir_recursive(ny_dump_dir(opt));
  char src_path[4096];
  char ir_path[4096];
  char asm_path[4096];
  ny_dump_path(src_path, sizeof(src_path), opt, "last_source.ny");
  ny_dump_path(ir_path, sizeof(ir_path), opt, "last_ir.ll");
  ny_dump_path(asm_path, sizeof(asm_path), opt, "last_asm.s");
  if (source) {
    ny_write_file(src_path, source, strlen(source));
  }
  if (module) {
    LLVMModuleRef dump_mod = ny_prepare_ir_dump_module(opt, module);
    char *err = NULL;
    if (LLVMPrintModuleToFile(dump_mod ? dump_mod : module, ir_path, &err) !=
        0) {
      if (err) {
        NY_LOG_ERR("Failed to write IR dump: %s\n", err);
        LLVMDisposeMessage(err);
      }
    }
    ny_llvm_emit_file(dump_mod ? dump_mod : module, asm_path, LLVMAssemblyFile,
                      opt->opt_level);
    ny_write_ir_stats_file(opt, "last_stats.txt", dump_mod ? dump_mod : module);
    if (dump_mod)
      LLVMDisposeModule(dump_mod);
  }
  NY_LOG_ERR("Debug bundle saved under %s/\n", ny_dump_dir(opt));
  {
    const size_t max_lines = 14;
    const char *paths[] = {ir_path, asm_path};
    const char *labels[] = {"IR snippet", "ASM snippet"};
    for (size_t i = 0; i < 2; i++) {
      char *content = ny_read_file(paths[i]);
      if (!content)
        continue;
      NY_LOG_ERR("--- %s (%s) ---\n", labels[i], paths[i]);
      size_t lines = 0;
      for (char *p = content; *p && lines < max_lines; p++) {
        fputc(*p, stderr);
        if (*p == '\n')
          lines++;
      }
      if (lines >= max_lines)
        NY_LOG_ERR("...\n");
      free(content);
    }
  }
}

#ifdef _WIN32
static const char *ny_windows_output_path(const char *raw, char *buf,
                                          size_t buflen) {
  if (!raw || !*raw)
    return raw;
  const char *base = strrchr(raw, '\\');
  const char *slash = strrchr(raw, '/');
  if (!base || (slash && slash > base))
    base = slash;
  base = base ? base + 1 : raw;
  const char *dot = strrchr(base, '.');
  if (dot && dot[1] != '\0')
    return raw;
  if (snprintf(buf, buflen, "%s.exe", raw) >= (int)buflen)
    return buf;
  return buf;
}
#endif

static LLVMCodeGenOptLevel ny_jit_codegen_opt_level(const ny_options *opt)
    __attribute__((unused));
static LLVMCodeGenOptLevel ny_jit_codegen_opt_level(const ny_options *opt) {
  const char *raw = getenv("NYTRIX_JIT_CODEGEN_OPT");
  if (!raw || !*raw)
    raw = getenv("NYTRIX_JIT_OPT_LEVEL");
  if (raw && *raw) {
    if (strcmp(raw, "0") == 0 || strcasecmp(raw, "o0") == 0 ||
        strcasecmp(raw, "none") == 0)
      return LLVMCodeGenLevelNone;
    if (strcmp(raw, "1") == 0 || strcasecmp(raw, "o1") == 0 ||
        strcasecmp(raw, "less") == 0)
      return LLVMCodeGenLevelLess;
    if (strcmp(raw, "2") == 0 || strcasecmp(raw, "o2") == 0 ||
        strcasecmp(raw, "default") == 0)
      return LLVMCodeGenLevelDefault;
    if (strcmp(raw, "3") == 0 || strcasecmp(raw, "o3") == 0 ||
        strcasecmp(raw, "aggressive") == 0)
      return LLVMCodeGenLevelAggressive;
  }
  ny_opt_profile_kind_t profile_kind = ny_opt_profile_kind_from_env();
  switch (profile_kind) {
  case NY_OPT_PROFILE_NONE:
  case NY_OPT_PROFILE_COMPILE:
    return LLVMCodeGenLevelNone;
  case NY_OPT_PROFILE_SIZE:
    return LLVMCodeGenLevelLess;
  case NY_OPT_PROFILE_PEAK:
  case NY_OPT_PROFILE_SPEED:
    return LLVMCodeGenLevelAggressive;
  case NY_OPT_PROFILE_BALANCED:
    return LLVMCodeGenLevelDefault;
  case NY_OPT_PROFILE_CUSTOM:
  case NY_OPT_PROFILE_DEFAULT:
  default:
    break;
  }
  int level = opt ? opt->opt_level : 2;
  if (level <= 0)
    return LLVMCodeGenLevelNone;
  if (level == 1)
    return LLVMCodeGenLevelLess;
  if (level >= 3)
    return LLVMCodeGenLevelAggressive;
  return LLVMCodeGenLevelDefault;
}

static bool ny_jit_triple_is_apple_arm64(const char *triple) {
  if (!triple || !*triple)
    return false;
  bool apple = strstr(triple, "apple") || strstr(triple, "darwin") ||
               strstr(triple, "macos");
  bool arm64 = strstr(triple, "arm64") || strstr(triple, "aarch64");
  return apple && arm64;
}

static bool ny_jit_module_is_apple_arm64(LLVMModuleRef module) {
  const char *triple = module ? LLVMGetTarget(module) : NULL;
  if (triple && *triple)
    return ny_jit_triple_is_apple_arm64(triple);
  const char *env_triple = getenv("NYTRIX_HOST_TRIPLE");
  if (env_triple && *env_triple)
    return ny_jit_triple_is_apple_arm64(env_triple);
  char *default_triple = LLVMGetDefaultTargetTriple();
  bool result = ny_jit_triple_is_apple_arm64(default_triple);
  if (default_triple)
    LLVMDisposeMessage(default_triple);
  return result;
}

static bool ny_env_has_value(const char *name) {
  const char *raw = getenv(name);
  return raw && *raw;
}

static int ny_clamp_llvm_opt_level(int level) {
  if (level < 0)
    return 0;
  if (level > 3)
    return 3;
  return level;
}

static LLVMCodeGenOptLevel
ny_jit_effective_codegen_opt_level(const ny_options *opt,
                                   LLVMModuleRef module) {
  if (ny_jit_module_is_apple_arm64(module) &&
      !ny_env_has_value("NYTRIX_JIT_CODEGEN_OPT") &&
      !ny_env_has_value("NYTRIX_JIT_OPT_LEVEL") &&
      !ny_env_has_value("NYTRIX_OPT_PROFILE")) {

    return LLVMCodeGenLevelNone;
  }
  return ny_jit_codegen_opt_level(opt);
}

static int ny_jit_effective_ir_opt_level(const ny_options *opt,
                                         LLVMModuleRef module, int fallback) {
  int level = ny_env_int("NYTRIX_JIT_IR_OPT_LEVEL", -1);
  if (level >= 0)
    return ny_clamp_llvm_opt_level(level);
  level = ny_env_int("NYTRIX_JIT_OPT_LEVEL", -1);
  if (level >= 0)
    return ny_clamp_llvm_opt_level(level);
  if (ny_jit_module_is_apple_arm64(module) &&
      !ny_env_has_value("NYTRIX_OPT_PROFILE") && !(opt && opt->opt_pipeline)) {

    return 0;
  }
  return ny_clamp_llvm_opt_level(fallback);
}

static bool ny_should_use_aot_cache(const ny_options *opt) {
  if (!opt || !opt->output_file || opt->run_jit || !opt->emit_only)
    return false;
  if (opt->stop_after != NY_STOP_AFTER_NONE || opt->emit_artifact_path ||
      opt->collect_errors || opt->emit_shapes)
    return false;
  if (opt->dump_diagnose)
    return false;
  if (opt->trace_exec || ny_env_enabled("NYTRIX_TRACE"))
    return false;
  if (!ny_env_enabled_default_on("NYTRIX_AOT_CACHE"))
    return false;
  if (opt->dump_ast || opt->expand || opt->dump_llvm || opt->dump_tokens ||
      opt->dump_docs || opt->dump_funcs || opt->dump_symbols ||
      opt->dump_stats || opt->emit_ir_path || opt->emit_asm_path)
    return false;
  return true;
}

static bool ny_should_use_jit_cache(const ny_options *opt) {
  if (!opt)
    return false;
  if (opt->emit_only && !opt->output_file && !opt->run_jit && !opt->run_aot)
    return false;
  if (ny_opt_profile_kind_from_name(opt->opt_profile) ==
          NY_OPT_PROFILE_COMPILE &&
      !ny_env_enabled("NYTRIX_COMPILE_PROFILE_CACHE"))
    return false;
  if (opt->stop_after != NY_STOP_AFTER_NONE || opt->emit_artifact_path ||
      opt->collect_errors || opt->emit_shapes)
    return false;
  if (opt->compiler_asserts > 0 || ny_compiler_asserts_enabled())
    return false;
  if (opt->output_file && !opt->run_jit &&
      !ny_env_enabled("NYTRIX_AOT_IR_CACHE"))
    return false;
  if (opt->run_jit && !opt->output_file &&
      !ny_env_enabled("NYTRIX_JIT_CACHE_RUN"))
    return false;
  return ny_jit_cache_enabled();
}

static bool ny_should_write_compile_caches(const ny_options *opt) {
  if (!opt)
    return false;
  return true;
}

static bool ny_std_bc_cache_preverify_enabled(void) {
  return ny_env_enabled("NYTRIX_STD_BC_CACHE_VERIFY") ||
         ny_env_enabled("NYTRIX_CACHE_STRICT_VERIFY");
}

static bool ny_use_name_is_project_std_module(const char *name) {
  if (!name)
    return false;
  return strcmp(name, "std.demo") == 0 || strncmp(name, "std.demo.", 9) == 0;
}

static time_t ny_file_mtime_or_zero(const char *path) {
  if (!path || !*path)
    return 0;
  struct stat st;
  if (stat(path, &st) != 0)
    return 0;
  return st.st_mtime;
}

static uint64_t ny_file_cache_stamp(const char *path) {
  if (!path || !*path)
    return 0;
  struct stat st;
  if (stat(path, &st) != 0)
    return 0;
  uint64_t h = NY_FNV1A64_OFFSET_BASIS;
  const uint64_t vals[] = {
      (uint64_t)st.st_mtime,
      (uint64_t)st.st_size,
      ny_stat_mtime_nsec(&st),
  };
  return ny_hash_u64v(h, vals, sizeof(vals) / sizeof(vals[0]));
}

static time_t ny_runtime_latest_mtime(const char *root) {
  static char cached_root[4096];
  static time_t cached_latest = 0;
  static int cached_valid = 0;
  if (root && *root && cached_valid && strcmp(cached_root, root) == 0)
    return cached_latest;
  if (!root || !*root)
    return 0;
  static const char *const deps[] = {
      "src/rt/init.c",      "src/rt/ast.c",      "src/rt/bigint.c",
      "src/rt/core.c",      "src/rt/ffi.c",      "src/rt/ffigates.c",
      "src/rt/gc.c",        "src/rt/math.c",     "src/rt/memory.c",
      "src/rt/os.c",        "src/rt/simmd.c",    "src/rt/gltf.c",
      "src/rt/string.c",    "src/rt/shared.h",   "src/rt/runtime.h",
      "src/rt/defs.h",      "src/parse/ast.h",   "src/parse/json.h",
      "src/parse/parser.h", "src/parse/lexer.h", "src/code/types.h",
      "src/base/common.h",  "src/base/compat.h",
  };
  time_t latest = 0;
  char full[4096];
  for (size_t i = 0; i < sizeof(deps) / sizeof(deps[0]); ++i) {
    snprintf(full, sizeof(full), "%s/%s", root, deps[i]);
    time_t mt = ny_file_mtime_or_zero(full);
    if (mt > latest)
      latest = mt;
  }
  snprintf(cached_root, sizeof(cached_root), "%s", root);
  cached_latest = latest;
  cached_valid = 1;
  return latest;
}

static void ny_build_aot_cache_path(const ny_options *opt, const char *source,
                                    const char *parse_name,
                                    const char *std_path,
                                    const char *output_path, char *out,
                                    size_t out_len) {
  if (!out || out_len == 0) {
    return;
  }
  out[0] = '\0';
  if (!opt || !source || !output_path)
    return;
  uint64_t h = NY_FNV1A64_OFFSET_BASIS;
  h = ny_fnv1a64_cstr("aot-cache-v9", h);
  h = ny_fnv1a64_cstr(VERSION, h);
#ifdef NYTRIX_VERSION_COMMIT
  h = ny_fnv1a64_cstr(NYTRIX_VERSION_COMMIT, h);
#endif
#ifdef NYTRIX_VERSION_DIRTY
  h = ny_hash64_u64(h, (uint64_t)NYTRIX_VERSION_DIRTY);
#endif
#ifdef NYTRIX_BUILD_HASH
  h = ny_fnv1a64_cstr(NYTRIX_BUILD_HASH, h);
#endif
#ifdef LLVM_VERSION_STRING
  h = ny_fnv1a64_cstr(LLVM_VERSION_STRING, h);
#endif
  h = ny_fnv1a64_cstr(source, h);
  h = ny_fnv1a64_cstr(parse_name ? parse_name : "<inline>", h);
  h = ny_fnv1a64_cstr(opt->opt_profile, h);
  {
    const unsigned opt_fields[] = {
        (unsigned)opt->opt_level,       (unsigned)opt->debug_symbols,
        (unsigned)opt->strip_override,  (unsigned)opt->std_mode,
        (unsigned)opt->no_std,          (unsigned)opt->opt_dce,
        (unsigned)opt->opt_internalize, (unsigned)opt->opt_loops,
        (unsigned)opt->opt_autotune,    (unsigned)opt->ownership,
        (unsigned)opt->ownership_strict};
    h = ny_hash_u32v(h, opt_fields, sizeof(opt_fields) / sizeof(opt_fields[0]));
  }
  h = ny_fnv1a64_cstr(opt->opt_pipeline, h);
  h = ny_fnv1a64_cstr(ny_builder_choose_cc(), h);
  h = ny_hash_cstrv(h, (const char *const *)opt->link_dirs.data,
                    opt->link_dirs.len);
  h = ny_hash_cstrv(h, (const char *const *)opt->link_libs.data,
                    opt->link_libs.len);
  {
    const char *const host_envs[] = {
        "NYTRIX_HOST_CFLAGS",
        "NYTRIX_HOST_LDFLAGS",
        "NYTRIX_ASSUME_INT",
        "NYTRIX_COMPILER_ASSERTS",
        "NYTRIX_DEBUG_LOCALS",
        "NYTRIX_DWARF_VERSION",
        "NYTRIX_DWARF_SPLIT_INLINING",
        "NYTRIX_DWARF_PROFILE_INFO",
        "NYTRIX_OPT_PROFILE",
        "NYTRIX_INDEX_READ_PARITY",
        "NYTRIX_DISABLE_FAST_INDEX_READ",
        "NYTRIX_GUARDED_FAST_GET",
        "NYTRIX_TRUSTED_FAST_GET",
        "NYTRIX_GUARDED_FAST_SET",
        "NYTRIX_TRUSTED_FAST_SET",
        "NYTRIX_GUARDED_FAST_DICT_GET",
        "NYTRIX_TRUSTED_FAST_DICT_GET",
        "NYTRIX_MONO_TYPES",
        "NYTRIX_ENABLE_MONOMORPHIZATION",
        "NYTRIX_DISABLE_MONO_TYPES",
        "NYTRIX_DISABLE_MONOMORPHIZATION",
        "NYTRIX_MONO_IMPERATIVE",
        "NYTRIX_SIMPLE_RAW_INT_CALL_FAST",
        "NYTRIX_FAST_ALL_PROFILES",
        "NYTRIX_PROVEN_RAW_INT_EXPR_FAST",
        "NYTRIX_RAW_INT_EXPR_FAST",
        "NYTRIX_RAW_INT_SLOT_EXPR_FAST",
        "NYTRIX_RAW_INT_EXPR_FAST_OPS",
        "NYTRIX_RAW_INT_EXPR_ADDSUB_FAST",
        "NYTRIX_RAW_INT_EXPR_MUL_FAST",
        "NYTRIX_RAW_INT_HELPERS",
        "NYTRIX_UNTAGGED_INT_LIST_STORAGE",
        "NYTRIX_CONST_STRING_GLOBAL_INIT",
        "NYTRIX_PROVEN_INT_CAST_FAST",
        "NYTRIX_PROVEN_INT_BRANCH_EQ_FAST",
        "NYTRIX_PROVEN_INT_BRANCH_FAST",
        "NYTRIX_PROVEN_INT_MOD_FAST",
        "NYTRIX_PRINT_PROVEN_INT_FAST",
        "NYTRIX_PRINT_PROVEN_STR_FAST",
    };
    h = ny_hash_envv(h, host_envs, sizeof(host_envs) / sizeof(host_envs[0]));
  }
  h = ny_fnv1a64_cstr(std_path, h);
  {
    char *exe_path = ny_get_executable_path();
    const uint64_t mtimes[] = {
        ny_file_cache_stamp(opt->argv0),
        ny_file_cache_stamp(exe_path),
        (uint64_t)ny_file_mtime_or_zero(std_path),
        (uint64_t)ny_runtime_latest_mtime(ny_src_root()),
    };
    h = ny_hash_u64v(h, mtimes, sizeof(mtimes) / sizeof(mtimes[0]));
  }
  const char *tmp = ny_get_temp_dir();
#ifdef _WIN32
  snprintf(out, out_len, "%s/ny_aot_cache_%016llx.exe", tmp,
           (unsigned long long)h);
#else
  snprintf(out, out_len, "%s/ny_aot_cache_%016llx", tmp, (unsigned long long)h);
#endif
}

static time_t ny_std_latest_mtime(void) {
  static int cached = 0;
  static time_t latest = 0;
  if (cached)
    return latest;
  latest = ny_std_latest_source_mtime();
  {
    const char *root = ny_src_root();
    if (root && *root) {
      char path[4096];
      snprintf(path, sizeof(path), "%s/src/base/loader.c", root);
      time_t mt = ny_file_mtime_or_zero(path);
      if (mt > latest)
        latest = mt;
      snprintf(path, sizeof(path), "%s/src/base/loader.h", root);
      mt = ny_file_mtime_or_zero(path);
      if (mt > latest)
        latest = mt;
    }
  }
  cached = 1;
  return latest;
}

static bool ny_std_path_is_generated_build_artifact(const char *path) {
  if (!path || !*path)
    return false;
  const char *root = ny_src_root();
  if (!root || !*root)
    return false;
  char prefix[4096];
  int n = snprintf(prefix, sizeof(prefix), "%s/build/", root);
  if (n > 0 && (size_t)n < sizeof(prefix) &&
      strncmp(path, prefix, (size_t)n) == 0)
    return true;
#ifdef _WIN32
  n = snprintf(prefix, sizeof(prefix), "%s\\build\\", root);
  if (n > 0 && (size_t)n < sizeof(prefix) &&
      strncmp(path, prefix, (size_t)n) == 0)
    return true;
#endif
  return false;
}

static int ny_write_file_atomic(const char *path, const char *content,
                                size_t len) {
  if (!path || !*path || !content)
    return -1;
  char tmp[4096];
#ifndef _WIN32
  snprintf(tmp, sizeof(tmp), "%s.XXXXXX", path);
  int fd = mkstemp(tmp);
  if (fd < 0)
    return -1;
  FILE *f = fdopen(fd, "wb");
  if (!f) {
    close(fd);
    unlink(tmp);
    return -1;
  }
#else
  int fd = -1;
  int retries = 100;
  while (retries > 0) {
    unsigned int r = 0;
    if (rand_s(&r) != 0) {
      r = (unsigned int)rand();
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp.%u.%llu", path, r,
             (unsigned long long)ny_ticks_now());
    fd = _open(tmp, _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY,
               _S_IREAD | _S_IWRITE);
    if (fd >= 0) {
      break;
    }
    if (errno != EEXIST) {
      return -1;
    }
    retries--;
  }
  if (fd < 0) {
    return -1;
  }
  FILE *f = _fdopen(fd, "wb");
  if (!f) {
    _close(fd);
    (void)unlink(tmp);
    return -1;
  }
#endif
  size_t written = fwrite(content, 1, len, f);
  int close_rc = fclose(f);
  if (written != len || close_rc != 0) {
    (void)unlink(tmp);
    return -1;
  }
#ifdef _WIN32
  (void)unlink(path);
#endif
  if (rename(tmp, path) != 0) {
    (void)unlink(tmp);
    return -1;
  }
  return 0;
}

static bool ny_std_sources_available(void) {
  const char *root = ny_src_root();
  if (!root || !*root)
    return false;
  char std_src[4096];
  struct stat st;
  const char *cands[] = {"src/std", "std", "src/lib", "lib"};
  for (size_t i = 0; i < 4; i++) {
    snprintf(std_src, sizeof(std_src), "%s/%s", root, cands[i]);
    if (stat(std_src, &st) == 0 && S_ISDIR(st.st_mode))
      return true;
  }
  return false;
}

static uint64_t ny_build_std_cache_path(const ny_options *opt,
                                        const char *const *uses,
                                        size_t use_count, std_mode_t std_mode,
                                        const char *prebuilt_path, char *out,
                                        size_t out_len) {
  if (!out || out_len == 0)
    return 0;
  out[0] = '\0';
  uint64_t h = NY_FNV1A64_OFFSET_BASIS;
  h = ny_fnv1a64_cstr("std-cache-v10", h);
  h = ny_hash64_u64(h, (uint64_t)std_mode);
  if (opt) {
    const unsigned opt_fields[] = {
        (unsigned)opt->opt_level,       (unsigned)opt->opt_dce,
        (unsigned)opt->opt_internalize, (unsigned)opt->no_std,
        (unsigned)opt->debug_symbols,   (unsigned)opt->trace_exec,
        (unsigned)opt->ownership,       (unsigned)opt->ownership_strict};
    h = ny_hash_u32v(h, opt_fields, sizeof(opt_fields) / sizeof(opt_fields[0]));
  }
  h = ny_hash_cstrv(h, uses, use_count);
  h = ny_fnv1a64_cstr(prebuilt_path, h);

  if (prebuilt_path && *prebuilt_path &&
      !ny_std_path_is_generated_build_artifact(prebuilt_path)) {
    struct stat pst;
    if (stat(prebuilt_path, &pst) == 0) {
      h = ny_hash64_u64(h, (uint64_t)pst.st_mtime);

      h = ny_hash64_u64(h, (uint64_t)pst.st_ctime);
      h = ny_hash64_u64(h, (uint64_t)pst.st_ino);
      h = ny_hash64_u64(h, (uint64_t)pst.st_dev);
      h = ny_hash64_u64(h, (uint64_t)pst.st_size);
    }
  }
  h = ny_hash64_u64(h, (uint64_t)ny_std_latest_mtime());
  h = ny_hash64_u64(h, ny_std_source_fingerprint());
  h = ny_fnv1a64_cstr(VERSION, h);
#ifdef NYTRIX_VERSION_COMMIT
  h = ny_fnv1a64_cstr(NYTRIX_VERSION_COMMIT, h);
#endif
#ifdef NYTRIX_VERSION_DIRTY
  h = ny_hash64_u64(h, (uint64_t)NYTRIX_VERSION_DIRTY);
#endif
  {
    const char *const envs[] = {"NYTRIX_HOST_TRIPLE", "NYTRIX_HOST_CFLAGS",
                                "NYTRIX_HOST_LDFLAGS", "NYTRIX_ARM_FLOAT_ABI",
                                "NYTRIX_ASSUME_INT"};
    h = ny_hash_envv(h, envs, sizeof(envs) / sizeof(envs[0]));
  }
  h = ny_fnv1a64_cstr(ny_src_root(), h);
  h = ny_fnv1a64_cstr(opt ? opt->argv0 : NULL, h);
  h = ny_hash64_u64(h,
                    (uint64_t)ny_file_mtime_or_zero(opt ? opt->argv0 : NULL));

  h = ny_fnv1a64_cstr(opt ? opt->input_file : NULL, h);
  h = ny_hash64_u64(
      h, (uint64_t)ny_file_mtime_or_zero(opt ? opt->input_file : NULL));
  char std_cache_dir[4096];
  snprintf(std_cache_dir, sizeof(std_cache_dir), "%s/std-src",
           ny_cache_root_dir());
  ny_ensure_dir_recursive(std_cache_dir);
  snprintf(out, out_len, "%s/ny_std_cache_%016llx.ny", std_cache_dir,
           (unsigned long long)h);
  return h;
}

static void append_use(char ***uses, size_t *len, size_t *cap,
                       const char *name) {
  for (size_t i = 0; i < *len; ++i) {
    if (strcmp((*uses)[i], name) == 0)
      return;
  }
  if (*len == *cap) {
    size_t new_cap = *cap ? (*cap * 2) : 8;
    char **tmp = realloc(*uses, new_cap * sizeof(char *));
    if (!tmp)
      return;
    *uses = tmp;
    *cap = new_cap;
  }
  (*uses)[(*len)++] = ny_strdup(name);
}

static bool ny_module_file_declares(const char *path, const char *module_name) {
  if (!path || !*path || !module_name || !*module_name ||
      ny_access(path, R_OK) != 0)
    return false;
  char *declared = ny_read_declared_module_name(path);
  bool ok = declared && strcmp(declared, module_name) == 0;
  free(declared);
  return ok;
}

static bool ny_check_child_module_path(const char *base, const char *leaf,
                                       const char *full_name) {
  char name[512];
  if (snprintf(name, sizeof(name), "%s.ny", leaf) >= (int)sizeof(name))
    return false;
  char path[4096];
  ny_join_path(path, sizeof(path), base, name);
  if (ny_module_file_declares(path, full_name))
    return true;

  char child_dir[4096];
  ny_join_path(child_dir, sizeof(child_dir), base, leaf);
  ny_join_path(path, sizeof(path), child_dir, "mod.ny");
  return ny_module_file_declares(path, full_name);
}

static bool ny_entry_child_module_exists(const char *entry_path,
                                         const char *full_name) {
  if (!entry_path || !*entry_path || entry_path[0] == '<' || !full_name ||
      !*full_name)
    return false;
  const char *leaf = strrchr(full_name, '.');
  leaf = leaf ? leaf + 1 : full_name;
  if (!leaf || !*leaf)
    return false;
  char path_copy[4096];
  if (snprintf(path_copy, sizeof(path_copy), "%s", entry_path) >=
      (int)sizeof(path_copy))
    return false;
  char *slash = strrchr(path_copy, '/');
  char *file = slash ? slash + 1 : path_copy;
  char stem[512];
  if (snprintf(stem, sizeof(stem), "%s", file) >= (int)sizeof(stem))
    return false;
  char *dot = strrchr(stem, '.');
  if (dot)
    *dot = '\0';
  if (slash) {
    if (slash == path_copy)
      slash[1] = '\0';
    else
      *slash = '\0';
  } else {
    snprintf(path_copy, sizeof(path_copy), ".");
  }

  if (ny_check_child_module_path(path_copy, leaf, full_name))
    return true;

  if (strcmp(stem, "mod") == 0)
    return false;
  char base[4096];
  ny_join_path(base, sizeof(base), path_copy, stem);
  return ny_check_child_module_path(base, leaf, full_name);
}

static token_t ny_collect_module_export_imports(lexer_t *lx, char ***uses,
                                                size_t *len, size_t *cap,
                                                const char *entry_path,
                                                const char *module_name,
                                                token_t t) {
  if (!lx || !module_name || !*module_name || t.kind != NY_T_LPAREN)
    return t;
  int paren_depth = 1;
  for (;;) {
    t = lexer_next(lx);
    if (t.kind == NY_T_EOF)
      return t;
    if (t.kind == NY_T_LPAREN) {
      paren_depth++;
      continue;
    }
    if (t.kind == NY_T_RPAREN) {
      paren_depth--;
      if (paren_depth == 0)
        return lexer_next(lx);
      continue;
    }
    if (paren_depth != 1 || t.kind != NY_T_IDENT)
      continue;
    char *leaf = dup_token_lexeme(t);
    if (!leaf)
      continue;
    size_t full_len = strlen(module_name) + 1 + strlen(leaf);
    char *full = malloc(full_len + 1);
    if (full) {
      snprintf(full, full_len + 1, "%s.%s", module_name, leaf);
      if (ny_entry_child_module_exists(entry_path, full))
        append_use(uses, len, cap, full);
      free(full);
    }
    free(leaf);
  }
}

static bool ny_ir_is_std_symbol(const char *name);
static bool ny_is_llvm_special_global(const char *name);
static bool ny_ir_is_std_value(LLVMValueRef v);
static void ny_build_llvm_used(LLVMModuleRef module, const LLVMValueRef *values,
                               size_t count);

static bool ny_ir_is_string_global(const char *name) {
  return name && (strncmp(name, ".str.data.", 10) == 0 ||
                  strncmp(name, ".str.runtime.", 13) == 0);
}

static bool ny_std_bc_symbol_is_mixed_codegen_artifact(const char *name) {
  if (!name || !*name)
    return false;
  return strncmp(name, "__ny_callable_adapter_", 22) == 0 ||
         strncmp(name, "__ny_callable_adapter_env_", 26) == 0;
}

static bool ny_std_bc_value_is_global_ref(LLVMValueRef v) {
  if (!v)
    return false;
  LLVMValueKind kind = LLVMGetValueKind(v);
  return kind == LLVMFunctionValueKind ||
         kind == LLVMGlobalAliasValueKind ||
         kind == LLVMGlobalIFuncValueKind ||
         kind == LLVMGlobalVariableValueKind;
}

static bool ny_std_bc_value_kind_has_operands(LLVMValueKind kind) {
  return kind == LLVMConstantExprValueKind ||
         kind == LLVMConstantArrayValueKind ||
         kind == LLVMConstantStructValueKind ||
         kind == LLVMConstantVectorValueKind ||
#if defined(NYTRIX_HAS_LLVM_CONSTANT_PTR_AUTH_VALUE_KIND)
         kind == LLVMConstantPtrAuthValueKind ||
#endif
         kind == LLVMInstructionValueKind;
}

static bool ny_std_bc_value_refs_mixed_codegen_artifact(LLVMValueRef v,
                                                        unsigned depth) {
  if (!v || depth > 32)
    return false;
  LLVMValueKind kind = LLVMGetValueKind(v);
  if (ny_std_bc_value_is_global_ref(v)) {
    const char *name = LLVMGetValueName(v);
    return ny_std_bc_symbol_is_mixed_codegen_artifact(name);
  }
  if (!ny_std_bc_value_kind_has_operands(kind))
    return false;
  int n = LLVMGetNumOperands(v);
  if (n <= 0)
    return false;
  for (int i = 0; i < n; ++i) {
    LLVMValueRef op = LLVMGetOperand(v, (unsigned)i);
    if (ny_std_bc_value_refs_mixed_codegen_artifact(op, depth + 1))
      return true;
  }
  return false;
}

static bool ny_std_bc_module_is_link_safe(LLVMModuleRef module,
                                          const char **bad_symbol) {
  if (bad_symbol)
    *bad_symbol = NULL;
  if (!module)
    return false;
  for (LLVMValueRef fn = LLVMGetFirstFunction(module); fn;
       fn = LLVMGetNextFunction(fn)) {
    const char *name = LLVMGetValueName(fn);
    if (ny_std_bc_symbol_is_mixed_codegen_artifact(name)) {
      if (bad_symbol)
        *bad_symbol = name;
      return false;
    }
    if (!LLVMIsDeclaration(fn)) {
      for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn); bb;
           bb = LLVMGetNextBasicBlock(bb)) {
        for (LLVMValueRef inst = LLVMGetFirstInstruction(bb); inst;
             inst = LLVMGetNextInstruction(inst)) {
          if (ny_std_bc_value_refs_mixed_codegen_artifact(inst, 0)) {
            if (bad_symbol)
              *bad_symbol = LLVMGetValueName(inst);
            return false;
          }
        }
      }
    }
  }
  for (LLVMValueRef gv = LLVMGetFirstGlobal(module); gv;
       gv = LLVMGetNextGlobal(gv)) {
    const char *name = LLVMGetValueName(gv);
    if (ny_std_bc_symbol_is_mixed_codegen_artifact(name)) {
      if (bad_symbol)
        *bad_symbol = name;
      return false;
    }
    LLVMValueRef init = LLVMGetInitializer(gv);
    if (init && ny_std_bc_value_refs_mixed_codegen_artifact(init, 0)) {
      if (bad_symbol)
        *bad_symbol = name;
      return false;
    }
  }
  return true;
}

static void ny_drop_llvm_used_globals(LLVMModuleRef module) {
  if (!module)
    return;
  const char *names[] = {"llvm.used", "llvm.compiler.used"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    LLVMValueRef gv = LLVMGetNamedGlobal(module, names[i]);
    if (gv)
      LLVMDeleteGlobal(gv);
  }
}

static void ny_preserve_std_values_for_dce(LLVMModuleRef module) {
  if (!module)
    return;
  VEC(LLVMValueRef) values;
  vec_init(&values);
  for (LLVMValueRef fn = LLVMGetFirstFunction(module); fn;
       fn = LLVMGetNextFunction(fn)) {
    if (ny_ir_is_std_value(fn) && LLVMCountBasicBlocks(fn) > 0)
      vec_push(&values, fn);
  }
  for (LLVMValueRef gv = LLVMGetFirstGlobal(module); gv;
       gv = LLVMGetNextGlobal(gv)) {
    if (LLVMIsDeclaration(gv))
      continue;
    if (ny_ir_is_std_value(gv))
      vec_push(&values, gv);
  }
  if (values.len)
    ny_build_llvm_used(module, values.data, values.len);
  vec_free(&values);
}

static bool ny_std_bc_cache_links_path(const char *cache_path, char *out,
                                       size_t out_len) {
  if (!cache_path || !*cache_path || !out || out_len == 0)
    return false;
  int n = snprintf(out, out_len, "%s.libs", cache_path);
  return n > 0 && (size_t)n < out_len;
}

static bool ny_std_bc_cache_has_links(const char *cache_path) {
  char path[4096];
  return ny_std_bc_cache_links_path(cache_path, path, sizeof(path)) &&
         ny_access(path, R_OK) == 0;
}

static void ny_codegen_add_link_lib(codegen_t *cg, const char *lib) {
  if (!cg || !lib || !*lib)
    return;
  for (size_t i = 0; i < cg->links.len; ++i) {
    if (cg->links.data[i] && strcmp(cg->links.data[i], lib) == 0)
      return;
  }
  vec_push(&cg->links, ny_strdup(lib));
}

static bool ny_std_bc_cache_load_links(const char *cache_path, codegen_t *cg) {
  char path[4096];
  if (!ny_std_bc_cache_links_path(cache_path, path, sizeof(path)))
    return false;
  FILE *f = fopen(path, "r");
  if (!f)
    return false;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                       line[len - 1] == ' ' || line[len - 1] == '\t'))
      line[--len] = '\0';
    char *p = line;
    while (*p == ' ' || *p == '\t')
      p++;
    if (*p)
      ny_codegen_add_link_lib(cg, p);
  }
  fclose(f);
  return true;
}

static bool ny_std_bc_cache_save_links(const char *cache_path,
                                       const codegen_t *cg) {
  char path[4096];
  if (!ny_std_bc_cache_links_path(cache_path, path, sizeof(path)) || !cg)
    return false;
  size_t total = 0;
  for (size_t i = 0; i < cg->links.len; ++i) {
    const char *lib = cg->links.data[i];
    if (lib && *lib)
      total += strlen(lib) + 1;
  }
  char *buf = malloc(total ? total : 1);
  if (!buf)
    return false;
  size_t off = 0;
  for (size_t i = 0; i < cg->links.len; ++i) {
    const char *lib = cg->links.data[i];
    if (!lib || !*lib)
      continue;
    size_t len = strlen(lib);
    memcpy(buf + off, lib, len);
    off += len;
    buf[off++] = '\n';
  }
  bool ok = ny_write_file_atomic(path, buf, off) == 0;
  free(buf);
  return ok;
}

static bool ny_save_std_bc_cache_from_module(LLVMModuleRef module,
                                             const char *cache_path) {
  if (!module || !cache_path || !*cache_path)
    return false;
  LLVMModuleRef std_mod = LLVMCloneModule(module);
  if (!std_mod)
    return false;
  LLVMStripModuleDebugInfo(std_mod);
  ny_drop_llvm_used_globals(std_mod);
  ny_preserve_std_values_for_dce(std_mod);
  for (LLVMValueRef fn = LLVMGetFirstFunction(std_mod); fn;) {
    LLVMValueRef next_fn = LLVMGetNextFunction(fn);
    const char *name = LLVMGetValueName(fn);
    if (name && *name && !ny_ir_is_std_value(fn) &&
        LLVMCountBasicBlocks(fn) > 0) {
      for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn); bb;) {
        LLVMBasicBlockRef next_bb = LLVMGetNextBasicBlock(bb);
        LLVMDeleteBasicBlock(bb);
        bb = next_bb;
      }
      LLVMSetLinkage(fn, LLVMExternalLinkage);
      LLVMSetVisibility(fn, LLVMDefaultVisibility);
    }
    fn = next_fn;
  }
  for (LLVMValueRef gv = LLVMGetFirstGlobal(std_mod); gv;) {
    LLVMValueRef next_gv = LLVMGetNextGlobal(gv);
    const char *name = LLVMGetValueName(gv);
    if (name && *name && !ny_ir_is_std_value(gv) &&
        !ny_ir_is_string_global(name) &&
        !ny_is_llvm_special_global(name) && !LLVMIsDeclaration(gv)) {
      LLVMSetInitializer(gv, NULL);
      LLVMSetLinkage(gv, LLVMExternalLinkage);
      LLVMSetVisibility(gv, LLVMDefaultVisibility);
      LLVMSetGlobalConstant(gv, false);
    }
    gv = next_gv;
  }
  LLVMPassBuilderOptionsRef popt = LLVMCreatePassBuilderOptions();
  if (popt) {
    LLVMErrorRef perr = LLVMRunPasses(std_mod, "globaldce", NULL, popt);
    if (perr) {
      char *msg = LLVMGetErrorMessage(perr);
      if (msg)
        LLVMDisposeErrorMessage(msg);
    }
    LLVMDisposePassBuilderOptions(popt);
  }
  const char *bad_symbol = NULL;
  if (!ny_std_bc_module_is_link_safe(std_mod, &bad_symbol)) {
    if (verbose_enabled >= 2 && bad_symbol && *bad_symbol)
      NY_LOG_INFO("skipping stdlib bitcode cache: mixed codegen artifact %s\n",
                  bad_symbol);
    LLVMDisposeModule(std_mod);
    (void)unlink(cache_path);
    return false;
  }
  char *verify_msg = NULL;
  if (LLVMVerifyModule(std_mod, LLVMReturnStatusAction, &verify_msg) != 0) {
    if (verify_msg)
      LLVMDisposeMessage(verify_msg);
    LLVMDisposeModule(std_mod);
    (void)unlink(cache_path);
    return false;
  }
  bool ok = ny_jit_cache_save(cache_path, std_mod);
  LLVMDisposeModule(std_mod);
  return ok;
}

typedef struct {
  char **names;
  size_t len;
  size_t cap;
} ny_module_list;

typedef struct {
  char *name;
  char *bc_path;
#ifndef _WIN32
  pid_t pid;
#endif
  int exit_code;
} ny_module_job;

static void ny_module_list_add(ny_module_list *list, const char *name) {
  if (!list || !name || !*name)
    return;
  for (size_t i = 0; i < list->len; i++) {
    if (strcmp(list->names[i], name) == 0)
      return;
  }
  if (list->len == list->cap) {
    size_t nc = list->cap ? list->cap * 2 : 8;
    char **nn = realloc(list->names, nc * sizeof(char *));
    if (!nn)
      return;
    list->names = nn;
    list->cap = nc;
  }
  list->names[list->len++] = ny_strdup(name);
}

static NY_UNUSED_FUNC void ny_collect_top_modules(const program_t *prog,
                                                  ny_module_list *out) {
  if (!prog || !out)
    return;
  for (size_t i = 0; i < prog->body.len; i++) {
    stmt_t *s = prog->body.data[i];
    if (s && s->kind == NY_S_MODULE && s->as.module.name) {
      ny_module_list_add(out, s->as.module.name);
    }
  }
}

static NY_UNUSED_FUNC void ny_free_module_list(ny_module_list *list) {
  if (!list)
    return;
  for (size_t i = 0; i < list->len; i++)
    free(list->names[i]);
  free(list->names);
  list->names = NULL;
  list->len = list->cap = 0;
}

static int ny_parallel_default_jobs(void) {
  long ncpu = ny_cpu_count();
  if (ncpu > 0 && ncpu < 1024)
    return (int)ncpu;
  return 4;
}

static NY_UNUSED_FUNC int ny_parallel_module_jobs(const ny_options *opt,
                                                  size_t total) {
  if (!opt)
    return 1;
  if (opt->thread_count > 0)
    return opt->thread_count;
  int jobs = ny_parallel_default_jobs();
  if (opt->parallel_mode && strcmp(opt->parallel_mode, "auto") == 0 && jobs > 8)
    jobs = 8;
  if (jobs < 1)
    jobs = 1;
  if ((size_t)jobs > total)
    jobs = (int)total;
  if (jobs < 1)
    jobs = 1;
  return jobs;
}

static NY_UNUSED_FUNC bool ny_parallel_modules_enabled(const ny_options *opt) {
  if (!opt || !opt->parallel_mode)
    return false;

  if (opt->emit_only && !opt->output_file && !opt->run_jit)
    return false;
  bool explicit_modules = strcmp(opt->parallel_mode, "modules") == 0;
  bool auto_modules =
      strcmp(opt->parallel_mode, "auto") == 0 &&
      (opt->std_mode == STD_MODE_FULL || opt->std_mode == STD_MODE_BC);
  if (!explicit_modules && !auto_modules)
    return false;
  if (getenv("NYTRIX_PARALLEL_DISABLE"))
    return false;
  if (opt->run_jit && !ny_env_enabled("NYTRIX_PARALLEL_JIT"))
    return false;
  if (!opt->input_file)
    return false;
  return true;
}

#ifndef _WIN32
static void ny_module_job_free(ny_module_job *job) {
  if (!job)
    return;
  free(job->name);
  free(job->bc_path);
  job->name = NULL;
  job->bc_path = NULL;
}

static char *ny_sanitize_modname(const char *name) {
  if (!name)
    return ny_strdup("mod");
  size_t n = strlen(name);
  char *out = malloc(n + 1);
  if (!out)
    return NULL;
  for (size_t i = 0; i < n; i++) {
    char c = name[i];
    out[i] = (c == '.') ? '_' : c;
  }
  out[n] = '\0';
  return out;
}

static bool ny_spawn_module_job(const ny_options *opt, const char *module_name,
                                const char *tmp_dir, ny_module_job *job) {
  if (!opt || !module_name || !tmp_dir || !job)
    return false;
  char *san = ny_sanitize_modname(module_name);
  if (!san)
    return false;
  static unsigned long long ny_mod_seq = 0;
  char bc_path[1024];
  unsigned long long seq = ++ny_mod_seq;
  snprintf(bc_path, sizeof(bc_path), "%s/ny_mod_%s_%ld_%llu.bc", tmp_dir, san,
           (long)getpid(), (unsigned long long)seq);
  free(san);
  char emit_bc_arg[1100];
  char emit_mod_arg[1100];
  snprintf(emit_bc_arg, sizeof(emit_bc_arg), "--emit-bc=%s", bc_path);
  snprintf(emit_mod_arg, sizeof(emit_mod_arg), "--emit-module=%s", module_name);

  char std_path_arg[1100];
  const char *std_path = NULL;
  if (opt->std_path) {
    snprintf(std_path_arg, sizeof(std_path_arg), "--std-path=%s",
             opt->std_path);
    std_path = std_path_arg;
  }

  char opt_arg[16];
  const char *argv[20];
  int idx = 0;
  argv[idx++] = opt->argv0 ? opt->argv0 : "ny";
  if (opt->opt_level > 0) {
    snprintf(opt_arg, sizeof(opt_arg), "-O%d", opt->opt_level);
    argv[idx++] = opt_arg;
  } else {
    argv[idx++] = "-O0";
  }
  argv[idx++] = "-emit-only";
  argv[idx++] = emit_bc_arg;
  argv[idx++] = emit_mod_arg;
  argv[idx++] = "--parallel=off";
  if (opt->opt_pipeline && *opt->opt_pipeline) {
    argv[idx++] = "-passes";
    argv[idx++] = opt->opt_pipeline;
  }
  if (opt->no_std)
    argv[idx++] = "--no-std";
  if (std_path)
    argv[idx++] = std_path;
  argv[idx++] = opt->input_file;
  argv[idx++] = NULL;

  pid_t pid = fork();
  if (pid < 0)
    return false;
  if (pid == 0) {
    ny_setenv("NYTRIX_PARALLEL_DISABLE", "1", 1);
    ny_setenv("NYTRIX_WORKER", "1", 1);
    if (opt->opt_level > 0 || (opt->opt_pipeline && *opt->opt_pipeline))
      ny_setenv("NYTRIX_WORKER_OPT", "1", 1);
    execvp(argv[0], (char *const *)argv);
    _exit(1);
  }
  job->name = ny_strdup(module_name);
  job->bc_path = ny_strdup(bc_path);
  job->pid = pid;
  job->exit_code = -1;
  return true;
}

#endif

static bool ny_link_module_cache(LLVMContextRef ctx, LLVMModuleRef main_mod,
                                 const char *cache_path) {
  if (!ctx || !main_mod || !cache_path)
    return false;
  LLVMMemoryBufferRef buf = NULL;
  char *msg = NULL;
  if (LLVMCreateMemoryBufferWithContentsOfFile(cache_path, &buf, &msg) != 0) {
    if (msg)
      LLVMDisposeMessage(msg);
    return false;
  }
  LLVMModuleRef mod = NULL;
  bool parsed = false;
  bool buf_owned_by_module = false;
  if (ny_cache_path_is_ir(cache_path)) {
    parsed = (LLVMParseIRInContext(ctx, buf, &mod, &msg) == 0);
    buf_owned_by_module = parsed;
  } else {
    parsed = (LLVMParseBitcodeInContext2(ctx, buf, &mod) == 0);
    buf_owned_by_module = parsed;
  }
  if (!buf_owned_by_module)
    LLVMDisposeMemoryBuffer(buf);
  if (!parsed && msg) {
    LLVMDisposeMessage(msg);
    msg = NULL;
  }
  if (!parsed) {
    (void)unlink(cache_path);
    return false;
  }
  LLVMStripModuleDebugInfo(mod);

  char *verify_msg = NULL;
  if (LLVMVerifyModule(mod, LLVMReturnStatusAction, &verify_msg) != 0) {
    if (verify_msg)
      LLVMDisposeMessage(verify_msg);
    LLVMDisposeModule(mod);
    (void)unlink(cache_path);
    return false;
  }
  const char *bad_symbol = NULL;
  if (!ny_std_bc_module_is_link_safe(mod, &bad_symbol)) {
    if (bad_symbol && *bad_symbol)
      NY_LOG_WARN("Ignoring unsafe std cache %s: mixed codegen artifact %s\n",
                  cache_path, bad_symbol);
    LLVMDisposeModule(mod);
    (void)unlink(cache_path);
    return false;
  }
  if (LLVMLinkModules2(main_mod, mod) != 0) {
    LLVMDisposeModule(mod);
    (void)unlink(cache_path);
    return false;
  }
  verify_msg = NULL;
  if (LLVMVerifyModule(main_mod, LLVMReturnStatusAction, &verify_msg) != 0) {
    if (verify_msg) {
      NY_LOG_WARN("Linked module cache verification failed: %s\n",
                  verify_msg);
      LLVMDisposeMessage(verify_msg);
    }
    (void)unlink(cache_path);
    return false;
  }
  return true;
}

static bool ny_verify_bitcode(LLVMContextRef ctx, const char *bc_path) {
  if (!ctx || !bc_path)
    return false;
  LLVMMemoryBufferRef buf = NULL;
  char *msg = NULL;
  if (LLVMCreateMemoryBufferWithContentsOfFile(bc_path, &buf, &msg) != 0) {
    if (msg)
      LLVMDisposeMessage(msg);
    return false;
  }
  LLVMModuleRef mod = NULL;
  bool ok = (LLVMParseBitcodeInContext2(ctx, buf, &mod) == 0);
  if (!ok || !mod) {
    LLVMDisposeMemoryBuffer(buf);
    return false;
  }
  char *verify_msg = NULL;
  if (LLVMVerifyModule(mod, LLVMReturnStatusAction, &verify_msg) != 0) {
    if (verify_msg)
      LLVMDisposeMessage(verify_msg);
    LLVMDisposeModule(mod);
    return false;
  }
  LLVMDisposeModule(mod);
  return true;
}

static bool ny_reemit_bitcode_via_ir(LLVMModuleRef module,
                                     const char *bc_path) {
  if (!module || !bc_path)
    return false;
  char *ir = LLVMPrintModuleToString(module);
  if (!ir)
    return false;
  size_t ir_len = strlen(ir);
  LLVMMemoryBufferRef buf =
      LLVMCreateMemoryBufferWithMemoryRangeCopy(ir, ir_len, "nytrix_ir");
  LLVMDisposeMessage(ir);
  if (!buf)
    return false;
  LLVMContextRef fresh_ctx = LLVMContextCreate();
  LLVMModuleRef parsed = NULL;
  char *msg = NULL;
  if (!fresh_ctx || LLVMParseIRInContext(fresh_ctx, buf, &parsed, &msg) != 0) {
    if (msg)
      LLVMDisposeMessage(msg);
    LLVMDisposeMemoryBuffer(buf);
    if (fresh_ctx)
      LLVMContextDispose(fresh_ctx);
    return false;
  }
  bool ok = (LLVMWriteBitcodeToFile(parsed, bc_path) == 0);
  LLVMDisposeModule(parsed);
  if (fresh_ctx)
    LLVMContextDispose(fresh_ctx);
  return ok;
}

static char **ny_collect_import_names(const char *src, const char *entry_path,
                                      size_t *out_count) {
  lexer_t lx;
  lexer_init(&lx, src, "<collect_use>");
  lx.quiet = true;
  int depth = 0;
  int module_depths[128];
  int module_depth_count = 0;
  bool pending_module_brace = false;
  char **uses = NULL;
  size_t len = 0, cap = 0;
  token_t t = lexer_next(&lx);
  for (;;) {
    if (t.kind == NY_T_EOF)
      break;
    if (t.kind == NY_T_MODULE) {
      pending_module_brace = true;
      token_t mod_tok = lexer_next(&lx);
      token_t next_tok = mod_tok;
      char *module_name = NULL;
      if (mod_tok.kind == NY_T_IDENT || mod_tok.kind == NY_T_NUMBER)
        module_name = parse_use_name(&lx, &mod_tok, &next_tok);
      if (module_name && next_tok.kind == NY_T_LPAREN)
        next_tok = ny_collect_module_export_imports(
            &lx, &uses, &len, &cap, entry_path, module_name, next_tok);
      free(module_name);
      t = next_tok;
    } else if (t.kind == NY_T_LBRACE) {
      depth++;
      if (pending_module_brace) {
        if (module_depth_count <
            (int)(sizeof(module_depths) / sizeof(module_depths[0]))) {
          module_depths[module_depth_count++] = depth;
        }
        pending_module_brace = false;
      }
      t = lexer_next(&lx);
    } else if (t.kind == NY_T_RBRACE) {
      if (module_depth_count > 0 &&
          depth == module_depths[module_depth_count - 1]) {
        module_depth_count--;
      }
      if (depth > 0)
        depth--;
      pending_module_brace = false;
      t = lexer_next(&lx);
    } else if (t.kind == NY_T_USE && (depth == 0 || module_depth_count > 0)) {
      t = lexer_next(&lx);
      token_t next_tok;
      char *name = parse_use_name(&lx, &t, &next_tok);
      if (name) {
        append_use(&uses, &len, &cap, name);
        free(name);
      }
      t = next_tok;
    } else {
      t = lexer_next(&lx);
    }
  }
  if (out_count)
    *out_count = len;
  return uses;
}

static const char *resolve_std_path(const char *compile_time_path) {
  const char *env = getenv("NYTRIX_STD_PREBUILT");
  if (env && *env && ny_access(env, R_OK) == 0)
    return env;
  env = getenv("NYTRIX_BUILD_STD_PATH");
  if (env && *env && ny_access(env, R_OK) == 0)
    return env;
  if (compile_time_path && ny_access(compile_time_path, R_OK) == 0)
    return compile_time_path;
  static char path[4096];
  if (ny_access("build/release/std.ny", R_OK) == 0) {
    snprintf(path, sizeof(path), "%s", "build/release/std.ny");
    return path;
  }
  if (ny_access("build/debug/std.ny", R_OK) == 0) {
    snprintf(path, sizeof(path), "%s", "build/debug/std.ny");
    return path;
  }
  if (ny_access("build/std.ny", R_OK) == 0) {
    snprintf(path, sizeof(path), "%s", "build/std.ny");
    return path;
  }
  char *exe_dir = ny_get_executable_dir();
  if (exe_dir) {
    snprintf(path, sizeof(path), "%s/std.ny", exe_dir);
    if (ny_access(path, R_OK) == 0)
      return path;
    snprintf(path, sizeof(path), "%s/../share/nytrix/std.ny", exe_dir);
    if (ny_access(path, R_OK) == 0)
      return path;
  }
  const char *root = ny_src_root();
  snprintf(path, sizeof(path), "%s/build/std.ny", root);
  if (ny_access(path, R_OK) == 0)
    return path;
  snprintf(path, sizeof(path), "%s/std.ny", root);
  if (ny_access(path, R_OK) == 0)
    return path;
  const char *common[] = {"/usr/share/nytrix/std.ny",
                          "/usr/local/share/nytrix/std.ny"};
  for (int i = 0; i < 2; i++)
    if (ny_access(common[i], R_OK) == 0)
      return common[i];
  return NULL;
}

static void ensure_aot_entry(codegen_t *cg, LLVMValueRef script_fn) {
  if (!cg || !cg->module || !script_fn)
    return;
  LLVMTypeRef i32 = LLVMInt32TypeInContext(cg->ctx);
  LLVMTypeRef i64 = LLVMInt64TypeInContext(cg->ctx);
  LLVMTypeRef ptr = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
  LLVMTypeRef ptrptr = LLVMPointerType(ptr, 0);
  LLVMTypeRef main_ty =
      LLVMFunctionType(i32, (LLVMTypeRef[]){i32, ptrptr, ptrptr}, 3, 0);
  LLVMValueRef existing_main = LLVMGetNamedFunction(cg->module, "main");
  LLVMValueRef user_main = NULL;
  fun_sig *user_main_sig = NULL;
  bool explicit_main_entry = ny_program_has_explicit_main_entry(cg, cg->prog);
  if (existing_main) {
    LLVMTypeRef existing_ty = LLVMGlobalGetValueType(existing_main);
    unsigned paramc = LLVMCountParamTypes(existing_ty);
    LLVMTypeRef param_types[3] = {0};
    if (paramc == 3)
      LLVMGetParamTypes(existing_ty, param_types);
    bool already_c_main = LLVMGetReturnType(existing_ty) == i32 &&
                          paramc == 3 && param_types[0] == i32 &&
                          param_types[1] == ptrptr && param_types[2] == ptrptr;
    if (already_c_main)
      return;
    user_main_sig = lookup_fun(cg, "main", 0);
    LLVMSetValueName2(existing_main, "_ny_user_main", strlen("_ny_user_main"));
    if (!explicit_main_entry)
      user_main = existing_main;
  }
  LLVMValueRef main_fn = LLVMAddFunction(cg->module, "main", main_ty);
  LLVMBasicBlockRef entry =
      LLVMAppendBasicBlockInContext(cg->ctx, main_fn, "entry");
  LLVMBuilderRef builder = LLVMCreateBuilderInContext(cg->ctx);
  LLVMPositionBuilderAtEnd(builder, entry);
  LLVMValueRef argc = LLVMGetParam(main_fn, 0);
  LLVMValueRef argv = LLVMGetParam(main_fn, 1);
  LLVMValueRef envp = LLVMGetParam(main_fn, 2);

  LLVMTypeRef set_args_ty =
      LLVMFunctionType(i64, (LLVMTypeRef[]){i32, ptrptr, ptrptr}, 3, 0);
  LLVMValueRef set_args_fn =
      LLVMGetNamedFunction(cg->module, "_ny_aot_set_args");
  if (!set_args_fn) {
    set_args_fn = LLVMAddFunction(cg->module, "_ny_aot_set_args", set_args_ty);
    LLVMSetLinkage(set_args_fn, LLVMExternalLinkage);
  }
  LLVMBuildCall2(builder, set_args_ty, set_args_fn,
                 (LLVMValueRef[]){argc, argv, envp}, 3, "");
  LLVMValueRef script_res = LLVMBuildCall2(
      builder, LLVMGlobalGetValueType(script_fn), script_fn, NULL, 0, "");
  LLVMValueRef status_i32 = NULL;
  LLVMTypeRef script_ret_ty =
      LLVMGetReturnType(LLVMGlobalGetValueType(script_fn));
  if (LLVMGetTypeKind(script_ret_ty) == LLVMIntegerTypeKind) {
    LLVMValueRef script_int =
        LLVMGetIntTypeWidth(script_ret_ty) == 64
            ? script_res
            : LLVMBuildSExtOrBitCast(builder, script_res, i64, "script_i64");
    LLVMValueRef script_status =
        LLVMBuildAShr(builder, script_int, LLVMConstInt(i64, 1, 0), "");
    status_i32 = LLVMBuildTrunc(builder, script_status, i32, "");
  } else {
    status_i32 = LLVMConstInt(i32, 0, false);
  }
  if (user_main) {
    LLVMTypeRef user_ty = LLVMGlobalGetValueType(user_main);
    unsigned user_argc = LLVMCountParamTypes(user_ty);
    if (user_argc == 0) {
      LLVMTypeRef user_ret_ty = LLVMGetReturnType(user_ty);
      LLVMValueRef user_res =
          LLVMBuildCall2(builder, user_ty, user_main, NULL, 0, "");
      LLVMTypeKind user_ret_kind = LLVMGetTypeKind(user_ret_ty);
      if (user_ret_kind == LLVMVoidTypeKind) {
        status_i32 = LLVMConstInt(i32, 0, false);
      } else if (user_ret_kind == LLVMIntegerTypeKind) {
        LLVMValueRef user_i64 =
            LLVMGetIntTypeWidth(user_ret_ty) == 64
                ? user_res
                : LLVMBuildSExtOrBitCast(builder, user_res, i64,
                                         "user_main_i64");
        bool raw_status =
            user_main_sig && user_main_sig->return_type &&
            *user_main_sig->return_type &&
            ny_is_native_abi_type_name(user_main_sig->return_type) &&
            !ny_type_is_tagged(user_main_sig->return_type);
        LLVMValueRef exit_i64 =
            raw_status
                ? user_i64
                : LLVMBuildAShr(builder, user_i64, LLVMConstInt(i64, 1, 0), "");
        status_i32 = LLVMBuildTrunc(builder, exit_i64, i32, "");
      } else if (user_ret_kind == LLVMPointerTypeKind) {
        LLVMValueRef raw =
            LLVMBuildPtrToInt(builder, user_res, i64, "user_main_ptr_i64");
        status_i32 = LLVMBuildTrunc(builder, raw, i32, "");
      } else if (user_ret_kind == LLVMFloatTypeKind ||
                 user_ret_kind == LLVMDoubleTypeKind) {
        status_i32 =
            LLVMBuildFPToSI(builder, user_res, i32, "user_main_fp_i32");
      }
    }
  }

  LLVMTypeRef flush_ty = LLVMFunctionType(i64, NULL, 0, 0);
  LLVMValueRef flush_fn = LLVMGetNamedFunction(cg->module, "rt_print_flush");
  if (!flush_fn)
    flush_fn = LLVMAddFunction(cg->module, "rt_print_flush", flush_ty);
  bool skip_cleanup = ny_env_enabled("NYTRIX_AOT_SKIP_CLEANUP") ||
                      (ny_codegen_speed_profile_enabled(cg) &&
                       !ny_env_enabled("NYTRIX_AOT_KEEP_CLEANUP"));
  if (skip_cleanup) {
    LLVMBuildCall2(builder, flush_ty, flush_fn, NULL, 0, "");
  } else {
    LLVMValueRef cleanup_fn =
        LLVMGetNamedFunction(cg->module, "rt_runtime_cleanup");
    if (!cleanup_fn) {
      LLVMTypeRef cleanup_ty = LLVMFunctionType(i64, NULL, 0, 0);
      cleanup_fn =
          LLVMAddFunction(cg->module, "rt_runtime_cleanup", cleanup_ty);
    }
    LLVMBuildCall2(builder, LLVMGlobalGetValueType(cleanup_fn), cleanup_fn,
                   NULL, 0, "");
  }
  LLVMBuildRet(builder, status_i32 ? status_i32 : LLVMConstInt(i32, 0, false));
  LLVMDisposeBuilder(builder);
}

static void maybe_log_phase_time(bool enabled, const char *label,
                                 ny_tick_t start_time) {
  if (!enabled)
    return;
  fprintf(stderr, "%-12s %.4fs\n", label, ny_ticks_elapsed_sec(start_time));
}

static bool handle_non_compile_modes(ny_options *opt, int *exit_code) {
  if (opt->mode == NY_MODE_VERSION) {
    printf("Nytrix %s\n", VERSION);
    *exit_code = 0;
    return true;
  }
  if (opt->mode == NY_MODE_BUNDLE) {
    *exit_code = ny_bundle_save(opt);
    return true;
  }
  if (opt->mode == NY_MODE_CLEAN_CACHE) {
    int rc = ny_cache_clean();
    if (rc == 0)
      printf("Removed Nytrix cache: %s\n", ny_cache_root_dir());
    else
      fprintf(
          stderr,
          "warning: some Nytrix cache artifacts could not be removed from %s\n",
          ny_cache_root_dir());
    *exit_code = rc == 0 ? 0 : 1;
    return true;
  }
  if (opt->mode == NY_MODE_HELP) {
    if (opt->help_env)
      ny_options_usage_env(opt->argv0 ? opt->argv0 : "ny");
    else
      ny_options_usage(opt->argv0 ? opt->argv0 : "ny");
    *exit_code = 0;
    return true;
  }
  if (opt->mode == NY_MODE_REPL) {
    ny_jit_init_native_once();
    LLVMLoadLibraryPermanently(NULL);
    int repl_batch = 0;
#ifdef _WIN32
    repl_batch = (_isatty(_fileno(stdin)) == 0);
#else
    repl_batch = (isatty(STDIN_FILENO) == 0);
#endif
    std_mode_t repl_std_mode = opt->no_std ? STD_MODE_NONE : opt->std_mode;
    if (repl_batch && ny_env_enabled("NYTRIX_REPL_BATCH_NO_STD") &&
        !opt->repl_explicit && !opt->std_mode_explicit) {
      const char *env_std = getenv("NYTRIX_REPL_STD");
      const char *env_no_std = getenv("NYTRIX_REPL_NO_STD");
      if ((!env_std || !*env_std) && (!env_no_std || !*env_no_std)) {
        repl_std_mode = STD_MODE_NONE;
      }
    }
    char *repl_stdin_src = NULL;
    if (repl_batch && !opt->command_string) {
      char *stdin_src = ny_read_stdin_all();
      if (!stdin_src) {
        NY_LOG_ERR("Failed to read REPL stdin\n");
        *exit_code = 1;
        return true;
      }
      if (ny_env_enabled("NYTRIX_REPL_TRACE"))
        fprintf(stderr, "[repl-batch] bytes=%zu fast_candidate=%d\n",
                strlen(stdin_src),
                ny_repl_batch_can_fast_run(stdin_src) ? 1 : 0);
      if (ny_repl_batch_can_fast_run(stdin_src)) {
        if (ny_env_enabled("NYTRIX_REPL_TRACE"))
          fprintf(stderr, "[repl-batch] dispatch=run\n");
        ny_options run_opt = *opt;
        run_opt.mode = NY_MODE_RUN;
        run_opt.command_string = stdin_src;
        run_opt.input_file = NULL;
        int rc = ny_pipeline_run(&run_opt);
        free(stdin_src);
        *exit_code = rc;
        return true;
      }
      if (ny_env_enabled("NYTRIX_REPL_TRACE"))
        fprintf(stderr, "[repl-batch] dispatch=repl\n");
      repl_stdin_src = stdin_src;
    }
    ny_repl_set_std_mode(repl_std_mode);
    ny_repl_set_plain(opt->repl_plain ? 1 : 0);
    ny_repl_set_max_errors(opt->max_errors);
    ny_repl_run(opt->opt_level, opt->opt_pipeline,
                opt->command_string ? opt->command_string : repl_stdin_src,
                repl_batch);
    free(repl_stdin_src);
    *exit_code = 0;
    return true;
  }
  return false;
}

static char *ny_normalize_command_source(const char *src) {
  if (!src)
    return ny_strdup("");

  size_t len = strlen(src);
  char *out = (char *)malloc(len + 2);
  if (!out)
    return NULL;
  memcpy(out, src, len);
  size_t j = len;
  if (j == 0 || out[j - 1] != '\n')
    out[j++] = '\n';
  out[j] = '\0';
  return out;
}

static char *load_user_source(const ny_options *opt) {
  if (opt->command_string)
    return ny_normalize_command_source(opt->command_string);
  if (opt->input_file) {
    if (strncmp(opt->input_file, "http://", 7) == 0 ||
        strncmp(opt->input_file, "https://", 8) == 0) {
      return ny_read_url(opt->input_file);
    }
    return ny_read_file(opt->input_file);
  }
  return ny_strdup("fn main() { return 0\n }");
}

static bool ny_md_extract_name_eq(const char *a, size_t a_len, const char *b,
                                  size_t b_len) {
  if (!a || !b || a_len != b_len)
    return false;
  for (size_t i = 0; i < a_len; ++i) {
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
      return false;
  }
  return true;
}

static bool ny_md_extract_lang_matches(const char *langs, const char *lang,
                                       size_t lang_len) {
  if (!lang || lang_len == 0)
    return false;
  if (!langs || !*langs)
    langs = "ny,nytrix";
  const char *p = langs;
  while (*p) {
    while (*p == ',' || *p == ';' || isspace((unsigned char)*p))
      p++;
    const char *start = p;
    while (*p && *p != ',' && *p != ';' && !isspace((unsigned char)*p))
      p++;
    size_t len = (size_t)(p - start);
    if ((len == 3 && ny_md_extract_name_eq(start, len, "all", 3)) ||
        ny_md_extract_name_eq(start, len, lang, lang_len))
      return true;
  }
  return false;
}

static bool ny_md_parse_open_fence(const char *line, size_t line_len,
                                   char *out_ch, size_t *out_len,
                                   const char **out_lang,
                                   size_t *out_lang_len) {
  if (!line || !out_ch || !out_len || !out_lang || !out_lang_len)
    return false;
  size_t i = 0;
  while (i < line_len && (line[i] == ' ' || line[i] == '\t'))
    i++;
  if (i >= line_len || (line[i] != '`' && line[i] != '~'))
    return false;
  char ch = line[i];
  size_t n = 0;
  while (i + n < line_len && line[i + n] == ch)
    n++;
  if (n < 3)
    return false;
  i += n;
  while (i < line_len && (line[i] == ' ' || line[i] == '\t'))
    i++;
  size_t lang_start = i;
  while (i < line_len && !isspace((unsigned char)line[i]) && line[i] != '`' &&
         line[i] != '~')
    i++;
  *out_ch = ch;
  *out_len = n;
  *out_lang = line + lang_start;
  *out_lang_len = i - lang_start;
  return true;
}

static bool ny_md_is_close_fence(const char *line, size_t line_len, char ch,
                                 size_t fence_len) {
  if (!line || fence_len < 3)
    return false;
  size_t i = 0;
  while (i < line_len && (line[i] == ' ' || line[i] == '\t'))
    i++;
  size_t n = 0;
  while (i + n < line_len && line[i + n] == ch)
    n++;
  if (n < fence_len)
    return false;
  i += n;
  while (i < line_len && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r'))
    i++;
  return i >= line_len;
}

static void ny_md_append_code_block_json(char **json, size_t *json_len,
                                         size_t *json_cap, bool *first,
                                         const char *lang, size_t lang_len,
                                         int start_line, int end_line,
                                         const char *code, size_t code_len) {
  if (!json || !json_len || !json_cap || !first)
    return;
  if (!*first)
    ny_stage_append(json, json_len, json_cap, ",");
  *first = false;
  ny_stage_append(json, json_len, json_cap, "{\"lang\":");
  char *lang_copy = ny_strndup(lang ? lang : "", lang_len);
  ny_stage_json_str(json, json_len, json_cap, lang_copy ? lang_copy : "");
  free(lang_copy);
  ny_stage_append(json, json_len, json_cap,
                  ",\"start_line\":%d,\"end_line\":%d,\"code\":", start_line,
                  end_line);
  char *code_copy = ny_strndup(code ? code : "", code_len);
  ny_stage_json_str(json, json_len, json_cap, code_copy ? code_copy : "");
  free(code_copy);
  ny_stage_append(json, json_len, json_cap, "}");
}

static int ny_run_code_extractor(const ny_options *opt) {
  char *src = load_user_source(opt);
  if (!src) {
    NY_LOG_ERR("Failed to read input for --extract-code\n");
    return 1;
  }

  const char *source_name =
      opt && opt->input_file ? opt->input_file : "<inline>";
  char *json = NULL;
  size_t json_len = 0, json_cap = 0;
  bool first_json = true;
  size_t match_count = 0;
  bool wrote_raw = false;
  if (opt && opt->extract_json) {
    ny_stage_append(&json, &json_len, &json_cap,
                    "{\"schema\":\"code_blocks.v1\",\"source\":");
    ny_stage_json_str(&json, &json_len, &json_cap, source_name);
    ny_stage_append(&json, &json_len, &json_cap, ",\"blocks\":[");
  }

  bool in_block = false;
  bool block_lang_ok = false;
  char fence_ch = 0;
  size_t fence_len = 0;
  const char *block_lang = NULL;
  size_t block_lang_len = 0;
  const char *code_start = NULL;
  int block_start_line = 0;

  const char *p = src;
  int line_no = 1;
  while (*p) {
    const char *line_start = p;
    while (*p && *p != '\n')
      p++;
    const char *line_end = p;
    size_t line_len = (size_t)(line_end - line_start);
    const char *next = (*p == '\n') ? p + 1 : p;

    if (!in_block) {
      const char *lang = NULL;
      size_t lang_len = 0;
      char open_ch = 0;
      size_t open_len = 0;
      if (ny_md_parse_open_fence(line_start, line_len, &open_ch, &open_len,
                                 &lang, &lang_len)) {
        in_block = true;
        fence_ch = open_ch;
        fence_len = open_len;
        block_lang = lang;
        block_lang_len = lang_len;
        block_lang_ok = ny_md_extract_lang_matches(
            opt ? opt->extract_lang : NULL, lang, lang_len);
        code_start = next;
        block_start_line = line_no;
      }
    } else if (ny_md_is_close_fence(line_start, line_len, fence_ch,
                                    fence_len)) {
      int block_end_line = line_no;
      bool selected =
          block_lang_ok && (!opt || opt->extract_line <= 0 ||
                            (opt->extract_line >= block_start_line &&
                             opt->extract_line <= block_end_line));
      if (selected) {
        size_t code_len = (size_t)(line_start - code_start);
        match_count++;
        if (opt && opt->extract_json) {
          ny_md_append_code_block_json(&json, &json_len, &json_cap, &first_json,
                                       block_lang, block_lang_len,
                                       block_start_line, block_end_line,
                                       code_start, code_len);
        } else {
          if (wrote_raw)
            fputc('\n', stdout);
          fwrite(code_start, 1, code_len, stdout);
          wrote_raw = true;
        }
      }
      in_block = false;
      block_lang_ok = false;
      fence_ch = 0;
      fence_len = 0;
      block_lang = NULL;
      block_lang_len = 0;
      code_start = NULL;
      block_start_line = 0;
    }

    if (*p == '\n') {
      p = next;
      line_no++;
    }
  }

  if (in_block && block_lang_ok) {
    bool selected =
        !opt || opt->extract_line <= 0 || opt->extract_line >= block_start_line;
    if (selected) {
      size_t code_len = strlen(code_start ? code_start : "");
      match_count++;
      if (opt && opt->extract_json) {
        ny_md_append_code_block_json(
            &json, &json_len, &json_cap, &first_json, block_lang,
            block_lang_len, block_start_line, line_no, code_start, code_len);
      } else {
        if (wrote_raw)
          fputc('\n', stdout);
        fwrite(code_start, 1, code_len, stdout);
        wrote_raw = true;
      }
    }
  }

  if (opt && opt->extract_json) {
    ny_stage_append(&json, &json_len, &json_cap, "],\"count\":%zu}\n",
                    match_count);
    fputs(json ? json
               : "{\"schema\":\"code_blocks.v1\",\"blocks\":[],\"count\":0}\n",
          stdout);
    free(json);
  }
  free(src);
  return (opt && opt->extract_json) || match_count > 0 ? 0 : 1;
}

static bool verify_module_if_needed(const ny_options *opt,
                                    LLVMModuleRef module) {
  if (!opt->verify_module)
    return true;
  char *err = NULL;
  if (LLVMVerifyModule(module, LLVMPrintMessageAction, &err)) {
    NY_LOG_ERR("Verification failed: %s\n", err);
    LLVMDisposeMessage(err);
    return false;
  }
  return true;
}

static NY_UNUSED_FUNC void ny_dump_ir_if_requested(LLVMModuleRef module,
                                                   const char *path,
                                                   const char *stage) {
  if (!module || !path || !*path)
    return;
  char *err = NULL;
  if (LLVMPrintModuleToFile(module, path, &err) != 0) {
    NY_LOG_WARN("failed to write %s IR to %s: %s\n", stage ? stage : "module",
                path, err ? err : "<unknown>");
    if (err)
      LLVMDisposeMessage(err);
    return;
  }
  if (err)
    LLVMDisposeMessage(err);
}

static void ny_dump_diagnose_ir_stage(const ny_options *opt,
                                      LLVMModuleRef module,
                                      const char *file_name,
                                      const char *stage) {
  if (!opt || !opt->dump_diagnose || !module || !file_name || !*file_name)
    return;
  ny_ensure_dir_recursive(ny_dump_dir(opt));
  char out_path[4096];
  ny_dump_path(out_path, sizeof(out_path), opt, file_name);
  LLVMModuleRef dump_mod = ny_prepare_ir_dump_module(opt, module);
  ny_dump_ir_if_requested(dump_mod ? dump_mod : module, out_path, stage);
  if (dump_mod)
    LLVMDisposeModule(dump_mod);
}

static void ny_dump_diagnose_finalize(const ny_options *opt,
                                      LLVMModuleRef module, int opt_level) {
  if (!opt || !opt->dump_diagnose || !module)
    return;
  ny_ensure_dir_recursive(ny_dump_dir(opt));
  char asm_path[4096];
  char bc_path[4096];
  char summary_path[4096];
  ny_dump_path(asm_path, sizeof(asm_path), opt, "diag.s");
  ny_dump_path(bc_path, sizeof(bc_path), opt, "diag.bc");
  ny_dump_path(summary_path, sizeof(summary_path), opt, "diag.summary.txt");
  LLVMModuleRef dump_mod = ny_prepare_ir_dump_module(opt, module);
  LLVMModuleRef art_mod = dump_mod ? dump_mod : module;
  (void)ny_llvm_emit_file(art_mod, asm_path, LLVMAssemblyFile, opt_level);
  (void)ny_reemit_bitcode_via_ir(art_mod, bc_path);
  ny_write_ir_stats_file(opt, "diag.stats.txt", art_mod);
  {
    const char *scope =
        (opt->dump_scope == NY_DUMP_SCOPE_LIB)
            ? "lib"
            : ((opt->dump_scope == NY_DUMP_SCOPE_BOTH) ? "both" : "program");
    char summary[1024];
    int n = snprintf(
        summary, sizeof(summary),
        "dump_dir=%s\nscope=%s\nwarn_level=%d\ndiag_compact=%d\nopt_level=%d\n",
        ny_dump_dir(opt), scope, opt->warn_level, opt->diag_compact ? 1 : 0,
        opt_level);
    if (n > 0)
      ny_write_file(summary_path, summary, (size_t)n);
  }
  if (dump_mod)
    LLVMDisposeModule(dump_mod);
}

static bool ny_ir_is_std_symbol(const char *name) {
  if (!name || !*name)
    return false;
  return (strncmp(name, "std.", 4) == 0 || strncmp(name, "lib.", 4) == 0 ||
          strncmp(name, "src.std.", 8) == 0 ||
          strncmp(name, "src.lib.", 8) == 0);
}

static void ny_sanitize_platform_sections(LLVMModuleRef module) {
#ifdef __APPLE__
  if (!module)
    return;
  for (LLVMValueRef fn = LLVMGetFirstFunction(module); fn;
       fn = LLVMGetNextFunction(fn)) {
    const char *sec = LLVMGetSection(fn);
    if (sec && strcmp(sec, "ny.std") == 0)
      LLVMSetSection(fn, "__TEXT,ny_std");
    else if (sec && strcmp(sec, "ny.user") == 0)
      LLVMSetSection(fn, "__TEXT,ny_user");
  }
  for (LLVMValueRef gv = LLVMGetFirstGlobal(module); gv;
       gv = LLVMGetNextGlobal(gv)) {
    const char *sec = LLVMGetSection(gv);
    if (sec && strcmp(sec, "ny.std") == 0)
      LLVMSetSection(gv, "__DATA,ny_std");
    else if (sec && strcmp(sec, "ny.user") == 0)
      LLVMSetSection(gv, "__DATA,ny_user");
  }
#else
  (void)module;
#endif
}

static void ny_clear_origin_sections(LLVMModuleRef module) {
  if (!module)
    return;
  for (LLVMValueRef fn = LLVMGetFirstFunction(module); fn;
       fn = LLVMGetNextFunction(fn)) {
    const char *sec = LLVMGetSection(fn);
    if (sec && (strcmp(sec, "ny.std") == 0 || strcmp(sec, "ny.user") == 0 ||
                strcmp(sec, "__TEXT,ny_std") == 0 ||
                strcmp(sec, "__TEXT,ny_user") == 0))
      LLVMSetSection(fn, "");
  }
  for (LLVMValueRef gv = LLVMGetFirstGlobal(module); gv;
       gv = LLVMGetNextGlobal(gv)) {
    const char *sec = LLVMGetSection(gv);
    if (sec && (strcmp(sec, "ny.std") == 0 || strcmp(sec, "ny.user") == 0 ||
                strcmp(sec, "__TEXT,ny_std") == 0 ||
                strcmp(sec, "__TEXT,ny_user") == 0 ||
                strcmp(sec, "__DATA,ny_std") == 0 ||
                strcmp(sec, "__DATA,ny_user") == 0))
      LLVMSetSection(gv, "");
  }
}

static bool ny_ir_is_std_value(LLVMValueRef v) {
  if (!v)
    return false;
  const char *sec = LLVMGetSection(v);
  if (sec && *sec) {

    if (strcmp(sec, "ny.std") == 0 || strcmp(sec, "__TEXT,ny_std") == 0 ||
        strcmp(sec, "__DATA,ny_std") == 0)
      return true;
    if (strcmp(sec, "ny.user") == 0 || strcmp(sec, "__TEXT,ny_user") == 0 ||
        strcmp(sec, "__DATA,ny_user") == 0)
      return false;
  }
  const char *name = LLVMGetValueName(v);
  return ny_ir_is_std_symbol(name);
}

static void ny_ir_externalize_std_definitions(const ny_options *opt,
                                              LLVMModuleRef module) {
  (void)opt;
  if (!module)
    return;
  for (LLVMValueRef fn = LLVMGetFirstFunction(module); fn;) {
    LLVMValueRef next_fn = LLVMGetNextFunction(fn);
    if (ny_ir_is_std_value(fn) && LLVMCountBasicBlocks(fn) > 0) {
      for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn); bb;) {
        LLVMBasicBlockRef next_bb = LLVMGetNextBasicBlock(bb);
        LLVMDeleteBasicBlock(bb);
        bb = next_bb;
      }
      LLVMSetLinkage(fn, LLVMExternalLinkage);
      LLVMSetVisibility(fn, LLVMDefaultVisibility);
    }
    fn = next_fn;
  }
  for (LLVMValueRef gv = LLVMGetFirstGlobal(module); gv;) {
    LLVMValueRef next_gv = LLVMGetNextGlobal(gv);
    if (ny_ir_is_std_value(gv) && !LLVMIsDeclaration(gv)) {
      LLVMSetInitializer(gv, NULL);
      LLVMSetLinkage(gv, LLVMExternalLinkage);
      LLVMSetVisibility(gv, LLVMDefaultVisibility);
      LLVMSetGlobalConstant(gv, false);
    }
    gv = next_gv;
  }
}

static void ny_ir_externalize_user_definitions(const ny_options *opt,
                                               LLVMModuleRef module) {
  if (!module)
    return;
  for (LLVMValueRef fn = LLVMGetFirstFunction(module); fn;) {
    LLVMValueRef next_fn = LLVMGetNextFunction(fn);
    if (!ny_ir_is_std_value(fn) && LLVMCountBasicBlocks(fn) > 0) {
      for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(fn); bb;) {
        LLVMBasicBlockRef next_bb = LLVMGetNextBasicBlock(bb);
        LLVMDeleteBasicBlock(bb);
        bb = next_bb;
      }
      LLVMSetLinkage(fn, LLVMExternalLinkage);
      LLVMSetVisibility(fn, LLVMDefaultVisibility);
    }
    fn = next_fn;
  }
  for (LLVMValueRef gv = LLVMGetFirstGlobal(module); gv;) {
    LLVMValueRef next_gv = LLVMGetNextGlobal(gv);
    const char *name = LLVMGetValueName(gv);
    if (!ny_ir_is_std_value(gv) && !ny_is_llvm_special_global(name) &&
        !LLVMIsDeclaration(gv)) {
      LLVMSetInitializer(gv, NULL);
      LLVMSetLinkage(gv, LLVMExternalLinkage);
      LLVMSetVisibility(gv, LLVMDefaultVisibility);
      LLVMSetGlobalConstant(gv, false);
    }
    gv = next_gv;
  }
  LLVMPassBuilderOptionsRef popt = LLVMCreatePassBuilderOptions();
  if (popt) {
    bool enable_dce = true;
    if (opt && opt->opt_level == 0)
      enable_dce = false;
    if (enable_dce) {
      LLVMErrorRef perr = LLVMRunPasses(module, "globaldce", NULL, popt);
      if (perr) {
        char *msg = LLVMGetErrorMessage(perr);
        NY_LOG_WARN("IR user-prune pass failed: %s\n", msg ? msg : "<unknown>");
        if (msg)
          LLVMDisposeErrorMessage(msg);
      }
    }
    LLVMDisposePassBuilderOptions(popt);
  }
}

static LLVMModuleRef ny_prepare_ir_dump_module(const ny_options *opt,
                                               LLVMModuleRef module) {
  if (!module)
    return NULL;
  LLVMModuleRef dump_mod = LLVMCloneModule(module);
  if (!dump_mod)
    return NULL;
  if (!opt || !opt->debug_symbols)
    LLVMStripModuleDebugInfo(dump_mod);
  ny_dump_scope_t scope = NY_DUMP_SCOPE_PROGRAM;
  if (opt)
    scope = opt->dump_scope;
  if (scope == NY_DUMP_SCOPE_PROGRAM)
    ny_ir_externalize_std_definitions(opt, dump_mod);
  else if (scope == NY_DUMP_SCOPE_LIB)
    ny_ir_externalize_user_definitions(opt, dump_mod);
  return dump_mod;
}

static bool ny_is_llvm_special_global(const char *name) {
  return name && strncmp(name, "llvm.", 5) == 0;
}

static bool ny_should_preserve_symbol(const codegen_t *cg, const char *name,
                                      bool is_jit) {
  if (!name || !*name)
    return false;
  if (strcmp(name, "main") == 0 || strcmp(name, "_ny_top_entry") == 0)
    return true;
  if (strncmp(name, "__std_init", 10) == 0)
    return true;
  if (name[0] == '.')
    return true;
  if (is_jit)
    return false;

  const char *dot = strchr(name, '.');
  if (dot) {
    if (!cg)
      return true;
    for (size_t i = 0; i < cg->link_allowed_modules.len; i++) {
      const char *use_name = cg->link_allowed_modules.data[i];
      if (!use_name)
        continue;
      size_t use_len = strlen(use_name);
      if (strncmp(name, use_name, use_len) == 0 && name[use_len] == '.') {
        return true;
      }
    }
    return false;
  }
  return false;
}

static bool ny_should_preserve_aot_symbol(const codegen_t *cg,
                                          const char *name) {
  return ny_should_preserve_symbol(cg, name, false);
}

static bool ny_should_preserve_jit_symbol(const codegen_t *cg,
                                          const char *name) {
  return ny_should_preserve_symbol(cg, name, true);
}

static void ny_build_llvm_used(LLVMModuleRef module, const LLVMValueRef *values,
                               size_t count) {
  if (!module || !values || count == 0)
    return;
  LLVMTypeRef i8ptr =
      LLVMPointerType(LLVMInt8TypeInContext(LLVMGetModuleContext(module)), 0);
  VEC(LLVMValueRef) entries;
  vec_init(&entries);

  LLVMValueRef used = LLVMGetNamedGlobal(module, "llvm.used");
  if (used) {
    LLVMValueRef init = LLVMGetInitializer(used);
    if (init && LLVMIsAConstantArray(init)) {
      unsigned n = LLVMGetNumOperands(init);
      for (unsigned i = 0; i < n; i++) {
        LLVMValueRef op = LLVMGetOperand(init, i);
        if (op)
          vec_push(&entries, op);
      }
    }
  }

  for (size_t i = 0; i < count; i++) {
    LLVMValueRef v = values[i];
    if (!v)
      continue;
    LLVMValueRef cast = LLVMConstBitCast(v, i8ptr);
    vec_push(&entries, cast);
  }

  if (entries.len == 0) {
    vec_free(&entries);
    return;
  }
  if (entries.len > UINT_MAX) {
    vec_free(&entries);
    return;
  }

  LLVMTypeRef arr_ty = LLVMArrayType(i8ptr, (unsigned)entries.len);
  LLVMValueRef arr = LLVMConstArray(i8ptr, entries.data, (unsigned)entries.len);
  if (used)
    LLVMDeleteGlobal(used);
  used = LLVMAddGlobal(module, arr_ty, "llvm.used");
  LLVMSetLinkage(used, LLVMAppendingLinkage);
  LLVMSetSection(used, "llvm.metadata");
  LLVMSetGlobalConstant(used, true);
  LLVMSetInitializer(used, arr);
  vec_free(&entries);
}

static void ny_drop_jit_llvm_used_metadata(LLVMModuleRef module) {
  ny_drop_llvm_used_globals(module);
}

static void ny_prepare_internalize(LLVMModuleRef module, const ny_options *opt,
                                   const codegen_t *cg, bool is_jit) {
  if (!module || !opt)
    return;
  VEC(LLVMValueRef) preserve;
  vec_init(&preserve);
  for (LLVMValueRef fn = LLVMGetFirstFunction(module); fn;
       fn = LLVMGetNextFunction(fn)) {
    if (LLVMIsDeclaration(fn))
      continue;
    size_t name_len = 0;
    const char *name = LLVMGetValueName2(fn, &name_len);
    if (!name || name_len == 0)
      continue;
    if (is_jit ? ny_should_preserve_jit_symbol(cg, name)
               : ny_should_preserve_aot_symbol(cg, name)) {
      vec_push(&preserve, fn);
    }
  }
  for (LLVMValueRef gv = LLVMGetFirstGlobal(module); gv;
       gv = LLVMGetNextGlobal(gv)) {
    if (LLVMIsDeclaration(gv))
      continue;
    size_t name_len = 0;
    const char *name = LLVMGetValueName2(gv, &name_len);
    if (!name || name_len == 0)
      continue;
    if (ny_is_llvm_special_global(name))
      continue;
    if (is_jit ? ny_should_preserve_jit_symbol(cg, name)
               : ny_should_preserve_aot_symbol(cg, name)) {
      vec_push(&preserve, gv);
    }
  }
  if (preserve.len > 0)
    ny_build_llvm_used(module, preserve.data, preserve.len);
  vec_free(&preserve);
}

static void run_dead_strip_if_needed(const ny_options *opt, codegen_t *cg,
                                     LLVMModuleRef module) {
  if (!opt || !module)
    return;
  bool is_aot = (opt->output_file != NULL);
  bool is_jit = opt->run_jit && (opt->mode != NY_MODE_REPL);
  if (!is_aot && !is_jit)
    return;

  if (is_jit && ny_jit_module_is_apple_arm64(module) &&
      !ny_env_enabled("NYTRIX_JIT_APPLE_ARM64_DCE")) {
    if (verbose_enabled >= 1)
      NY_LOG_INFO("%s", "JIT dead-strip: disabled for Apple arm64 MCJIT\n");
    return;
  }

  bool dce_enabled = false;
  if (is_aot || is_jit) {
    dce_enabled = (opt->opt_dce != 0) && !ny_env_enabled("NYTRIX_JIT_NO_DCE");
  }

  if (!dce_enabled)
    return;

  bool internalize_enabled = false;
  if (is_aot || is_jit) {
    internalize_enabled =
        (opt->opt_internalize != 0) && !ny_env_enabled("NYTRIX_JIT_NO_DCE");
  }

  if (internalize_enabled) {
    ny_prepare_internalize(module, opt, cg, is_jit);
    if (verbose_enabled >= 1)
      NY_LOG_INFO("%s internalize: enabled via llvm.used\n",
                  is_aot ? "AOT" : "JIT");
  } else if (verbose_enabled >= 1) {
    NY_LOG_INFO("%s internalize: DISABLED (opt->opt_internalize=%d)\n",
                is_jit ? "JIT" : "AOT", opt->opt_internalize);
  }
  const char *pipeline = NULL;
  if (internalize_enabled) {
    pipeline = getenv(is_aot ? "NYTRIX_AOT_INTERNALIZE_PIPELINE"
                             : "NYTRIX_JIT_INTERNALIZE_PIPELINE");
    if (!pipeline || !*pipeline)
      pipeline = "internalize,globaldce";
  } else {
    pipeline =
        getenv(is_aot ? "NYTRIX_AOT_DCE_PIPELINE" : "NYTRIX_JIT_DCE_PIPELINE");
    if (!pipeline || !*pipeline)
      pipeline = "globaldce";
  }
  LLVMPassBuilderOptionsRef popt = LLVMCreatePassBuilderOptions();
  if (!popt)
    return;
  LLVMErrorRef perr = LLVMRunPasses(module, pipeline, NULL, popt);
  if (perr) {
    char *msg = LLVMGetErrorMessage(perr);
    NY_LOG_WARN("AOT dead-strip pipeline '%s' failed: %s\n", pipeline,
                msg ? msg : "<unknown>");
    if (msg)
      LLVMDisposeErrorMessage(msg);
    if (strcmp(pipeline, "globaldce") != 0) {
      LLVMErrorRef ferr = LLVMRunPasses(module, "globaldce", NULL, popt);
      if (ferr) {
        char *fmsg = LLVMGetErrorMessage(ferr);
        NY_LOG_WARN("%s dead-strip fallback 'globaldce' failed: %s\n",
                    is_aot ? "AOT" : "JIT", fmsg ? fmsg : "<unknown>");
        if (fmsg)
          LLVMDisposeErrorMessage(fmsg);
      } else if (verbose_enabled >= 2) {
        NY_LOG_V2("%s dead-strip passes: globaldce\n", is_aot ? "AOT" : "JIT");
      }
    }
  } else if (verbose_enabled >= 2) {
    NY_LOG_V2("%s dead-strip passes: %s\n", is_aot ? "AOT" : "JIT", pipeline);
  }
  LLVMDisposePassBuilderOptions(popt);
}

typedef VEC(char *) ny_link_lib_vec;

static bool ny_link_lib_basename(const char *lib, const char **base_out,
                                 size_t *len_out) {
  if (!lib || strncmp(lib, "lib", 3) != 0 || !base_out || !len_out)
    return false;
  const char *base = lib + 3;
  size_t len = strlen(base);
  const char *dot = strstr(base, ".so");
#ifdef __APPLE__
  const char *dylib = strstr(base, ".dylib");
  if (dylib && dylib > base && (!dot || dylib < dot))
    dot = dylib;
#endif
  if (dot && dot > base)
    len = (size_t)(dot - base);
  if (len == 0)
    return false;
  *base_out = base;
  *len_out = len;
  return true;
}

static bool ny_link_lib_vec_contains_exact(const ny_link_lib_vec *libs,
                                           const char *name) {
  if (!libs || !name)
    return false;
  for (size_t i = 0; i < libs->len; i++) {
    const char *existing = libs->data[i];
    if (existing && strcmp(existing, name) == 0)
      return true;
  }
  return false;
}

static bool ny_link_lib_vec_contains_dash_l(const ny_link_lib_vec *libs,
                                            const char *name, size_t len) {
  if (!libs || !name)
    return false;
  for (size_t i = 0; i < libs->len; i++) {
    const char *existing = libs->data[i];
    if (existing && existing[0] == '-' && existing[1] == 'l' &&
        strncmp(existing + 2, name, len) == 0 && existing[2 + len] == '\0')
      return true;
  }
  return false;
}

static void ny_link_lib_vec_add_option(ny_link_lib_vec *libs,
                                       const char *lib) {
  const char *name = NULL;
  size_t len = 0;
  if (ny_link_lib_basename(lib, &name, &len) && len < 256) {
    char buf[260];
    snprintf(buf, sizeof(buf), "-l%.*s", (int)len, name);
    vec_push(libs, ny_strdup(buf));
    return;
  }
  vec_push(libs, ny_strdup(lib));
}

static void ny_link_lib_vec_add_codegen(ny_link_lib_vec *libs,
                                        const char *name) {
  if (!name)
    return;
  if (name[0] == '-' || strchr(name, '/') || strchr(name, '\\')) {
    if (!ny_link_lib_vec_contains_exact(libs, name))
      vec_push(libs, ny_strdup(name));
    return;
  }
  const char *lib_name = name;
  size_t lib_len = strlen(name);
  const char *base = NULL;
  size_t base_len = 0;
  if (ny_link_lib_basename(name, &base, &base_len)) {
    lib_name = base;
    lib_len = base_len;
  }
  if (!ny_link_lib_vec_contains_dash_l(libs, lib_name, lib_len)) {
    char buf[260];
    snprintf(buf, sizeof(buf), "-l%.*s", (int)lib_len, lib_name);
    vec_push(libs, ny_strdup(buf));
  }
}

static void ny_link_lib_vec_merge(ny_link_lib_vec *libs, const ny_options *opt,
                                  const codegen_t *cg) {
  for (size_t i = 0; opt && i < opt->link_libs.len; i++)
    ny_link_lib_vec_add_option(libs, opt->link_libs.data[i]);
  for (size_t i = 0; cg && i < cg->links.len; i++)
    ny_link_lib_vec_add_codegen(libs, cg->links.data[i]);
}

static void ny_link_lib_vec_dispose(ny_link_lib_vec *libs) {
  if (!libs)
    return;
  for (size_t i = 0; i < libs->len; i++)
    free(libs->data[i]);
  vec_free(libs);
}

static bool ny_pipeline_configure_fast_compiler(ny_options *opt) {
  if (!opt)
    return false;
  bool low_overhead = ny_env_enabled("NYTRIX_FAST_COMPILE");
  bool fast_compiler = low_overhead || ny_env_enabled("NYTRIX_FAST_COMPILER");
  if (!fast_compiler)
    return false;
  opt->opt_level = low_overhead ? 0 : 1;
  opt->opt_pipeline = NULL;
  opt->opt_loops = 0;
  opt->opt_autotune = 0;
  opt->verify_module = false;
  opt->debug_symbols = false;
  ny_setenv("NYTRIX_JIT_CACHE", "1", 0);
  ny_setenv("NYTRIX_JIT_OPT_LEVEL", low_overhead ? "0" : "1", 0);
  if (!ny_jit_module_is_apple_arm64(NULL))
    ny_setenv("NYTRIX_JIT_FAST_ISEL", "1", 0);
  return true;
}

static void ny_pipeline_configure_worker(ny_options *opt) {
  if (!opt || !getenv("NYTRIX_WORKER") || getenv("NYTRIX_WORKER_OPT"))
    return;
  opt->opt_level = 0;
  opt->opt_pipeline = NULL;
  opt->opt_dce = 0;
}

static bool ny_pipeline_prepare_aot_run_output(ny_options *opt,
                                               const char **output_path,
                                               char *path, size_t path_len) {
  if (!opt || !output_path || !path || path_len == 0 || !opt->run_aot)
    return false;
  if (*output_path && **output_path)
    return false;
#ifdef _WIN32
  snprintf(path, path_len, "%s/ny_aot_run_%d.exe", ny_get_temp_dir(),
           (int)getpid());
#else
  snprintf(path, path_len, "%s/ny_aot_run_%d", ny_get_temp_dir(),
           (int)getpid());
#endif
  opt->output_file = path;
  *output_path = opt->output_file;
  opt->emit_only = true;
  opt->run_jit = false;
  return true;
}

typedef struct {
  std_mode_t mode;
  const char *prebuilt_path;
  char *src;
  char *auto_bc_cache;
  const char *bc_cache;
  bool use_bc_cache;
  bool auto_bc_cache_needs_links;
  bool has_local;
} ny_pipeline_std_load;

static void ny_pipeline_scan_std_imports(char **uses, size_t use_count,
                                         bool *has_local,
                                         bool *has_project_std) {
  if (has_local)
    *has_local = false;
  if (has_project_std)
    *has_project_std = false;
  for (size_t i = 0; uses && i < use_count; i++) {
    const char *u = uses[i];
    bool is_std = (strcmp(u, "std") == 0 || strncmp(u, "std.", 4) == 0);
    bool is_lib = (strcmp(u, "lib") == 0 || strncmp(u, "lib.", 4) == 0);
    if (has_project_std && ny_use_name_is_project_std_module(u))
      *has_project_std = true;
    if (has_local && !is_std && !is_lib) {
      *has_local = true;
      break;
    }
  }
}

static bool ny_pipeline_load_stdlib(ny_options *opt, char **uses,
                                    size_t use_count, char *std_cache_path,
                                    size_t std_cache_path_len,
                                    ny_pipeline_std_load *std) {
  if (!opt || !std)
    return false;
  memset(std, 0, sizeof(*std));
  std->mode = opt->std_mode;
  std->prebuilt_path = resolve_std_path(
      opt->std_path ? opt->std_path
                    : (NYTRIX_STD_PATH ? NYTRIX_STD_PATH : "build/std.ny"));
  if (opt->no_std)
    std->mode = STD_MODE_NONE;
  if (std->mode == STD_MODE_DEFAULT && use_count == 0)
    std->mode = STD_MODE_NONE;

  bool has_project_std = false;
  ny_pipeline_scan_std_imports(uses, use_count, &std->has_local,
                               &has_project_std);
  bool std_sources_ok = ny_std_sources_available();
  bool prebuilt_ok =
      std->prebuilt_path && ny_access(std->prebuilt_path, R_OK) == 0;
  bool prefer_prebuilt = ny_env_enabled("NYTRIX_STD_PREFER_PREBUILT");
  bool prebuilt_preferred =
      (std->mode == STD_MODE_FULL ||
       (prefer_prebuilt && std->mode == STD_MODE_DEFAULT)) &&
      !std->has_local;
  bool prebuilt_required = !std_sources_ok;

  if (opt->std_bc_path && ny_access(opt->std_bc_path, R_OK) == 0) {
    std->bc_cache = opt->std_bc_path;
    std->use_bc_cache = true;
  } else {
    const char *env_bc = getenv("NYTRIX_STD_BC_CACHE");
    if (std->mode != STD_MODE_NONE && env_bc && *env_bc &&
        ny_access(env_bc, R_OK) == 0) {
      std->bc_cache = env_bc;
      std->use_bc_cache = true;
    } else if (std->mode != STD_MODE_NONE && !opt->run_jit &&
               !std->has_local && !has_project_std &&
               ny_env_enabled_default_on("NYTRIX_STD_BC_CACHE_AUTO")) {
      std->auto_bc_cache = ny_std_bc_cache_path(
          std->prebuilt_path, (const char *const *)uses, use_count,
          (int)std->mode, opt->debug_symbols,
          (unsigned long)ny_std_latest_mtime(), opt->argv0);
      if (std->auto_bc_cache && ny_access(std->auto_bc_cache, R_OK) == 0) {
        bool cache_ok = true;
        if (ny_std_bc_cache_preverify_enabled()) {
          LLVMContextRef cache_ctx = LLVMContextCreate();
          cache_ok =
              cache_ctx && ny_verify_bitcode(cache_ctx, std->auto_bc_cache);
          if (cache_ctx)
            LLVMContextDispose(cache_ctx);
        }
        if (cache_ok) {
          bool needs_link_sidecar =
              opt->output_file && !ny_output_path_is_object(opt->output_file);
          if (needs_link_sidecar &&
              !ny_std_bc_cache_has_links(std->auto_bc_cache)) {
            std->auto_bc_cache_needs_links = true;
          } else {
            std->bc_cache = std->auto_bc_cache;
            std->use_bc_cache = true;
          }
        } else {
          (void)unlink(std->auto_bc_cache);
        }
      }
    }
  }

  if (std->mode != STD_MODE_NONE && prebuilt_ok &&
      (prebuilt_preferred || prebuilt_required)) {
    if (verbose_enabled)
      NY_LOG_INFO("Using prebuilt std.ny: %s\n", std->prebuilt_path);
    std->src = ny_read_file(std->prebuilt_path);
    if (!std->src && verbose_enabled) {
      NY_LOG_WARN("Failed to read prebuilt std.ny: %s (falling back)\n",
                  std->prebuilt_path);
    }
  }

  if (std->mode != STD_MODE_NONE && !std->src) {
    bool use_std_cache =
        !std->has_local && ny_env_enabled_default_on("NYTRIX_STD_CACHE");
    uint64_t std_cache_sig = 0;
    if (use_std_cache && std_cache_path && std_cache_path_len > 0) {
      std_cache_sig = ny_build_std_cache_path(
          opt, (const char *const *)uses, use_count, std->mode,
          std->prebuilt_path, std_cache_path, std_cache_path_len);
      if (std_cache_path[0] != '\0' && ny_access(std_cache_path, R_OK) == 0) {
        std->src = ny_read_file(std_cache_path);
        if (std->src && std->src[0] == '\0') {
          free(std->src);
          std->src = NULL;
          (void)unlink(std_cache_path);
        }
        if (std->src && std_cache_sig) {
          char expect[128];
          int nw =
              snprintf(expect, sizeof(expect), "; ny_std_cache_v10 %016llx\n",
                       (unsigned long long)std_cache_sig);
          if (nw <= 0 || (size_t)nw >= sizeof(expect) ||
              strncmp(std->src, expect, (size_t)nw) != 0) {
            free(std->src);
            std->src = NULL;
            (void)unlink(std_cache_path);
          }
        }
        if (std->src && verbose_enabled >= 2)
          NY_LOG_INFO("Using std cache: %s\n", std_cache_path);
      }
    }
    if (!std->src) {
      std->src = ny_build_std_source((const char **)uses, use_count, std->mode,
                                     opt->verbose, opt->input_file);
      if (std->src && use_std_cache && std_cache_path &&
          std_cache_path[0] != '\0' && std_cache_sig) {
        char header[128];
        int hn = snprintf(header, sizeof(header), "; ny_std_cache_v10 %016llx\n",
                          (unsigned long long)std_cache_sig);
        if (hn > 0 && (size_t)hn < sizeof(header)) {
          size_t sl = strlen(std->src);
          char *wrapped = malloc((size_t)hn + sl + 1);
          if (wrapped) {
            memcpy(wrapped, header, (size_t)hn);
            memcpy(wrapped + hn, std->src, sl + 1);
            (void)ny_write_file_atomic(std_cache_path, wrapped,
                                       (size_t)hn + sl);
            free(wrapped);
          }
        }
      }
    }
  }

  if (std->mode == STD_MODE_NONE || std->src)
    return true;

  NY_LOG_ERR("Could not load std.ny or standard library source files.\n");
  NY_LOG_ERR("Checked paths: %s and %s/std\n",
             std->prebuilt_path ? std->prebuilt_path : "NULL", ny_src_root());
  free(std->auto_bc_cache);
  std->auto_bc_cache = NULL;
  return false;
}

static char *ny_pipeline_join_sources(const char *std_src, const char *user_src,
                                      const char *parse_name,
                                      size_t *user_len_out,
                                      size_t *split_pos_out) {
  if (user_len_out)
    *user_len_out = 0;
  if (split_pos_out)
    *split_pos_out = 0;
  if (!user_src)
    return NULL;

  size_t slen = std_src ? strlen(std_src) : 0;
  size_t ulen = strlen(user_src);
  size_t line_directive_len = 0;
  if (parse_name && parse_name[0] != '<') {
    size_t parse_len = strlen(parse_name);
    if (parse_len > SIZE_MAX - (sizeof("#line 1 \"\"\n") - 1)) {
      NY_LOG_ERR("Source file name too large for #line directive\n");
      return NULL;
    }
    line_directive_len = parse_len + (sizeof("#line 1 \"\"\n") - 1);
  }

  size_t total = slen;
  if (ulen > SIZE_MAX - total ||
      line_directive_len > SIZE_MAX - total - ulen ||
      4 > SIZE_MAX - total - ulen - line_directive_len) {
    NY_LOG_ERR("Source code too large to concatenate\n");
    return NULL;
  }
  total += ulen + line_directive_len + 4;

  char *source = malloc(total);
  if (!source) {
    NY_LOG_ERR("Failed to allocate combined source input\n");
    return NULL;
  }

  char *ptr = source;
  if (std_src) {
    memcpy(ptr, std_src, slen);
    ptr += slen;
    if (ptr > source && ptr[-1] != '\n')
      *ptr++ = '\n';
    if (split_pos_out)
      *split_pos_out = (size_t)(ptr - source);
  }
  if (line_directive_len > 0) {
    int n = snprintf(ptr, line_directive_len + 1, "#line 1 \"%s\"\n",
                     parse_name);
    if (n < 0 || (size_t)n != line_directive_len) {
      free(source);
      NY_LOG_ERR("Failed to build #line directive\n");
      return NULL;
    }
    ptr += line_directive_len;
  }
  memcpy(ptr, user_src, ulen + 1);
  if (user_len_out)
    *user_len_out = ulen;
  return source;
}

int ny_pipeline_run(ny_options *opt) {
  int exit_code = 0;
  ny_tick_t pipeline_prof_t0 = ny_ticks_now();
  ny_lookup_prof_register_atexit();
  if (handle_non_compile_modes(opt, &exit_code))
    return exit_code;
  ny_diag_configure(opt ? opt->warn_level : 1, opt ? opt->diag_compact : false);
  if (opt && opt->dump_diagnose)
    ny_ensure_dir_recursive(ny_dump_dir(opt));
  ny_pipeline_configure_worker(opt);
  bool fast_compiler = ny_pipeline_configure_fast_compiler(opt);
  verbose_enabled = opt->verbose;
  ny_tick_t t_start = 0;
  ny_tick_t t0 = 0;
  if (opt->do_timing) {
    t_start = ny_ticks_now();
    t0 = ny_ticks_now();
  }
  if (opt->extract_code)
    return ny_run_code_extractor(opt);
  if (ny_try_fast_command_string(opt, t_start))
    return 0;
  char *user_src = NULL;
  char *std_src = NULL;
  char *source = NULL;
  char **uses = NULL;
  size_t use_count = 0;
  arena_t *arena = NULL;
  char aot_cache_path[4096] = {0};
  char std_cache_path[4096] = {0};
  program_t prog = {0};
  codegen_t cg;
  memset(&cg, 0, sizeof(cg));
  const char *parse_name = opt->input_file ? opt->input_file : "<inline>";
  const char *output_path = opt->output_file;
  LLVMValueRef script_fn = NULL;
  char aot_run_path[4096] = {0};
  bool aot_run_temp = false;
  bool loaded_from_cache = false;
  char *jit_cache_file = NULL;
  char *type_errors_json = NULL;
#ifndef _WIN32
  char *native_cache_file = NULL;
  void *native_cache_handle = NULL;
  void (*native_cache_entry)(void) = NULL;
#endif
  aot_run_temp = ny_pipeline_prepare_aot_run_output(
      opt, &output_path, aot_run_path, sizeof(aot_run_path));
#ifdef _WIN32
  char output_win[4096];
  if (output_path)
    output_path =
        ny_windows_output_path(output_path, output_win, sizeof(output_win));
#endif
  user_src = load_user_source(opt);
  if (!user_src) {
    if (opt->input_file) {
      if (strncmp(opt->input_file, "http://", 7) == 0 ||
          strncmp(opt->input_file, "https://", 8) == 0) {
        NY_LOG_ERR("Failed to fetch URL '%s'\n", opt->input_file);
      } else {
        NY_LOG_ERR("Failed to read file '%s'\n", opt->input_file);
      }
    } else {
      NY_LOG_ERR("Failed to allocate source input\n");
    }
    return 1;
  }
  maybe_log_phase_time(opt->do_timing, "Read file:", t0);
  if (opt->do_timing)
    t0 = ny_ticks_now();
  uses = ny_collect_import_names(user_src, opt->input_file, &use_count);
  maybe_log_phase_time(opt->do_timing, "Scan imports:", t0);
  if (opt->do_timing)
    t0 = ny_ticks_now();
  if (ny_env_enabled("NYTRIX_TRACE_IMPORTS")) {
    fprintf(stderr, "[trace] imports (%zu):", use_count);
    for (size_t i = 0; i < use_count; i++) {
      fprintf(stderr, " %s", uses[i] ? uses[i] : "<null>");
    }
    fprintf(stderr, "\n");
  }
  ny_tick_t t_std = opt->do_timing ? ny_ticks_now() : 0;
  ny_pipeline_std_load std_load = {0};
  std_mode_t std_mode = STD_MODE_NONE;
  const char *prebuilt_path = NULL;
  char *auto_std_bc_cache = NULL;
  const char *std_bc_cache = NULL;
  bool use_std_bc_cache = false;
  bool auto_std_bc_cache_needs_links = false;
  bool has_local = false;
  bool auto_std_bc_cache_saved = false;
  bool write_compile_caches = ny_should_write_compile_caches(opt);
  if (!ny_pipeline_load_stdlib(opt, uses, use_count, std_cache_path,
                               sizeof(std_cache_path), &std_load)) {
    exit_code = 1;
    goto exit_success;
  }
  std_mode = std_load.mode;
  prebuilt_path = std_load.prebuilt_path;
  std_src = std_load.src;
  auto_std_bc_cache = std_load.auto_bc_cache;
  std_bc_cache = std_load.bc_cache;
  use_std_bc_cache = std_load.use_bc_cache;
  auto_std_bc_cache_needs_links = std_load.auto_bc_cache_needs_links;
  has_local = std_load.has_local;
  maybe_log_phase_time(opt->do_timing, "Stdlib load:", t_std);
  size_t ulen = 0;
  size_t split_pos = 0;
  source = ny_pipeline_join_sources(std_src, user_src, parse_name, &ulen,
                                    &split_pos);
  if (!source) {
    exit_code = 1;
    goto exit_success;
  }
#ifndef _WIN32
  if (opt->run_jit && !opt->command_string && ny_should_use_jit_cache(opt) &&
      ny_jit_native_cache_enabled() && !opt->dump_ast && !opt->expand &&
      !opt->dump_llvm && !opt->emit_ir_path && !opt->emit_bc_path &&
      !opt->dump_tokens && !opt->dump_diagnose) {
    char *early_bc = ny_jit_cache_path(source, prebuilt_path, 0, opt->opt_level,
                                       opt->opt_dce, opt->opt_internalize,
                                       opt->debug_symbols,
                                       (unsigned long)ny_std_latest_mtime());
    if (early_bc) {
      char *early_so = ny_jit_native_cache_path(early_bc);
      if (early_so) {
        if (ny_jit_native_cache_load(early_so, &native_cache_handle,
                                     &native_cache_entry)) {
          if (opt->verbose)
            fprintf(stderr, "JIT native cache hit (early): %s\n", early_so);
          loaded_from_cache = true;
          free(early_so);
          free(early_bc);
          goto skip_compilation;
        }
        free(early_so);
      }
      free(early_bc);
    }
  }
#endif
  if (ny_should_use_aot_cache(opt)) {
    ny_build_aot_cache_path(opt, source, parse_name, prebuilt_path, output_path,
                            aot_cache_path, sizeof(aot_cache_path));
    if (aot_cache_path[0] != '\0' && ny_access(aot_cache_path, R_OK) == 0 &&
        strcmp(aot_cache_path, output_path) != 0) {
      if (ny_valid_native_artifact(aot_cache_path)) {
        if (ny_copy_file(aot_cache_path, output_path) == 0 &&
            ny_valid_native_artifact(output_path)) {
          NY_LOG_V2("AOT cache hit: %s\n", aot_cache_path);
#ifdef _WIN32
          NY_LOG_SUCCESS("Saved EXE: %s\n", output_path);
#else
          NY_LOG_SUCCESS("Saved ELF: %s\n", output_path);
#endif
          ny_trace_file_size("emit_native_cache_hit", output_path);
          if (opt->run_aot) {
            const char *argv_exec[] = {output_path, NULL};
            int rc = ny_exec_spawn(argv_exec);
            if (rc != 0)
              exit_code = rc;
            if (aot_run_temp)
              (void)unlink(output_path);
          }
          goto exit_success;
        }
      } else {
        (void)unlink(aot_cache_path);
      }
    }
  }
  if (opt->dump_tokens) {
    lexer_t lx;
    lexer_init(&lx, source, parse_name);
    for (;;) {
      token_t t = lexer_next(&lx);
      printf("%d:%d kind=%d lexeme='%.*s'\n", t.line, t.col, t.kind, (int)t.len,
             t.lexeme);
      if (t.kind == NY_T_EOF)
        break;
    }
    goto exit_success;
  }
  ny_tick_t t_parse = opt->do_timing ? ny_ticks_now() : 0;
  parser_t parser;
  arena = (arena_t *)malloc(sizeof(arena_t));
  memset(arena, 0, sizeof(arena_t));
  parser_init_with_arena(&parser, source, std_src ? "<stdlib>" : parse_name,
                         arena);
  if (opt->max_errors >= 0)
    parser.error_limit = opt->max_errors;
  if (std_src) {
    parser.lex.split_pos = split_pos;
    ny_sym_id split_file_id = ny_intern_cstr(parse_name);
    parser.lex.split_filename =
        split_file_id ? ny_intern_get(split_file_id) : parse_name;
  }
  prog = parse_program(&parser);
  maybe_log_phase_time(opt->do_timing, "Parsing:", t_parse);
  if (parser.had_error) {
    NY_LOG_ERR("Compilation failed: %d errors\n", parser.error_count);
    ny_stage_maybe_emit_errors(opt, NY_STOP_AFTER_PARSE, parse_name,
                               "parse failed", parser.error_count);
    dump_debug_bundle(opt, source, NULL);
    exit_code = 1;
    goto exit_success;
  }
  ny_ast_verify_program(&prog, "parse");
  if (ny_stop_after_is(opt, NY_STOP_AFTER_PARSE)) {
    ny_stage_emit_artifact(opt, NY_STOP_AFTER_PARSE, &prog, NULL, parse_name,
                           NULL, true);
    goto exit_success;
  }
  if (opt->expand) {
    char *report = ny_ast_expand_report(&prog, parse_name, opt->expand_only,
                                        opt->explain_specialization,
                                        opt->meta_trace, opt->expand_json);
    if (report) {
      fputs(report, stdout);
      rt_free((int64_t)(uintptr_t)report);
    }
    goto exit_success;
  }
  if (opt->safe_mode && !ny_safe_mode_validate_raw_memory(&prog)) {
    dump_debug_bundle(opt, source, NULL);
    exit_code = 1;
    goto exit_success;
  }

  ny_tick_t t_codegen = opt->do_timing ? ny_ticks_now() : 0;
  NY_LOG_V2("Initializing codegen_t for module 'nytrix'\n");
  codegen_init(&cg, &prog, arena, "nytrix");
  cg.source_main_file = parse_name;
  cg.type_solver = opt->type_solver_raw ? opt->type_solver_raw : "auto";
  cg.strict_types = opt->strict_types;
  if (opt->safe_mode) {
    cg.strict_diagnostics = true;
    cg.ownership_enabled = true;
    cg.ownership_strict = true;
  }
  codegen_collect_links(&cg, &prog);

  cg.debug_symbols =
      opt->debug_symbols &&
      (opt->output_file || opt->emit_ir_path || opt->emit_bc_path ||
       opt->emit_asm_path || opt->emit_only);
  cg.debug_opt_level = opt->opt_level;
  if (cg.debug_symbols && parse_name && *parse_name)
    cg.debug_main_file = parse_name;

  cg.user_source = user_src;
  cg.user_source_len = ulen;
  if ((opt->run_jit || opt->output_file) && opt->mode != NY_MODE_REPL &&
      ny_should_use_jit_cache(opt) && !opt->dump_ast && !opt->expand &&
      !opt->dump_llvm && !opt->emit_ir_path && !opt->emit_bc_path &&
      !opt->dump_diagnose) {
    jit_cache_file = ny_jit_cache_path(source, prebuilt_path, 0, opt->opt_level,
                                       opt->opt_dce, opt->opt_internalize,
                                       opt->debug_symbols,
                                       (unsigned long)ny_std_latest_mtime());
#ifndef _WIN32
    if (jit_cache_file && opt->run_jit && !opt->command_string &&
        ny_jit_native_cache_enabled()) {
      native_cache_file = ny_jit_native_cache_path(jit_cache_file);
      if (native_cache_file &&
          ny_jit_native_cache_load(native_cache_file, &native_cache_handle,
                                   &native_cache_entry)) {
        if (opt->verbose)
          fprintf(stderr, "JIT native cache hit: %s\n", native_cache_file);
        loaded_from_cache = true;
        goto skip_compilation;
      }
    }
#endif
    if (jit_cache_file) {
      LLVMModuleRef cached_mod = NULL;
      if (ny_jit_cache_load(jit_cache_file, cg.ctx, &cached_mod)) {
        if (opt->verbose)
          fprintf(stderr, "JIT cache hit: %s\n", jit_cache_file);
        loaded_from_cache = true;
        LLVMDisposeModule(cg.module);
        cg.module = cached_mod;
        script_fn = LLVMGetNamedFunction(cg.module, "_ny_top_entry");
        if (!script_fn) {
          if (opt->verbose)
            fprintf(stderr, "JIT cache corrupt (missing entry): %s\n",
                    jit_cache_file);
          loaded_from_cache = false;
          cg.module = LLVMModuleCreateWithNameInContext("nytrix", cg.ctx);
          ny_llvm_prepare_module(cg.module, 3);
        } else {
          codegen_repopulate_interns(&cg);
          goto skip_compilation;
        }
      }
    }
  }

  if (opt->dump_ast) {
    for (size_t i = 0; i < prog.body.len; i++) {
      stmt_t *s = prog.body.data[i];
      printf("  [%zu] Kind=%d\n", i, s->kind);
    }
  }
  bool parallel_modules = false;
#ifndef _WIN32
  ny_module_list mods = {0};
  if (!fast_compiler && !use_std_bc_cache && ny_parallel_modules_enabled(opt)) {
    ny_collect_top_modules(&prog, &mods);
    if (mods.len > 0)
      parallel_modules = true;
  }
#endif

  if (opt->std_bc_path && ny_access(opt->std_bc_path, R_OK) == 0) {
    cg.skip_stdlib = true;
    use_std_bc_cache = true;
    std_bc_cache = opt->std_bc_path;
  }
  if (use_std_bc_cache)
    cg.skip_stdlib = true;
  cg.emit_cached_stdlib_init = use_std_bc_cache;
  if (use_std_bc_cache) {
    NY_LOG_INFO("linking stdlib bitcode cache: %s\n", std_bc_cache);
    (void)ny_std_bc_cache_load_links(std_bc_cache, &cg);
  }
  if (opt->emit_module) {
    cg.emit_module_name = opt->emit_module;
    cg.emit_module_decls_only = true;
    cg.emit_script = false;
  }
#ifndef _WIN32
  if (!opt->emit_module && parallel_modules) {
    cg.emit_module_name = "";
    cg.emit_module_decls_only = true;
  }
#endif
  cg.source_string = source;
  cg.prog_owned = false;
  NY_LOG_V2("Preparing codegen (analysis & links)...\n");
  codegen_prepare(&cg);
  if (cg.had_error) {
    NY_LOG_ERR("Codegen prepare failed\n");
    ny_stage_maybe_emit_errors(opt, NY_STOP_AFTER_TRAIT, parse_name,
                               "codegen prepare failed", 1);
    exit_code = 1;
    goto exit_success;
  }
  ny_type_pipeline_stage_t max_type_stage = NY_TYPE_PIPELINE_STAGE_ABI;
  if (opt->stop_after == NY_STOP_AFTER_HM)
    max_type_stage = NY_TYPE_PIPELINE_STAGE_HM;
  else if (opt->stop_after == NY_STOP_AFTER_TRAIT ||
           opt->stop_after == NY_STOP_AFTER_FLOW)
    max_type_stage = NY_TYPE_PIPELINE_STAGE_TRAIT;
  ny_type_pipeline_stage_t failed_type_stage = NY_TYPE_PIPELINE_STAGE_OK;
  int type_errors = ny_type_pipeline_validate_semantics(
      &prog, &cg, parse_name, opt && opt->dump_scope != NY_DUMP_SCOPE_PROGRAM,
      max_type_stage, &failed_type_stage, true, &type_errors_json);
  if (type_errors > 0) {
    ny_stop_after_stage_t error_stage = NY_STOP_AFTER_HM;
    const char *error_message = "HM type validation failed";
    if (failed_type_stage == NY_TYPE_PIPELINE_STAGE_TRAIT) {
      error_stage = NY_STOP_AFTER_TRAIT;
      error_message = "trait validation failed";
      NY_LOG_ERR("Trait/type validation failed\n");
    } else if (failed_type_stage == NY_TYPE_PIPELINE_STAGE_ABI) {
      error_stage = NY_STOP_AFTER_ABI;
      error_message = "ABI validation failed";
      NY_LOG_ERR("ABI/layout validation failed\n");
    } else {
      NY_LOG_ERR("HM type validation failed\n");
    }
    if ((failed_type_stage == NY_TYPE_PIPELINE_STAGE_HM &&
         (opt->stop_after == NY_STOP_AFTER_HM ||
          opt->stop_after == NY_STOP_AFTER_TRAIT ||
          opt->stop_after == NY_STOP_AFTER_FLOW ||
          opt->stop_after == NY_STOP_AFTER_ABI)) ||
        (failed_type_stage == NY_TYPE_PIPELINE_STAGE_TRAIT &&
         (opt->stop_after == NY_STOP_AFTER_TRAIT ||
          opt->stop_after == NY_STOP_AFTER_FLOW ||
          opt->stop_after == NY_STOP_AFTER_ABI)) ||
        (failed_type_stage == NY_TYPE_PIPELINE_STAGE_ABI &&
         opt->stop_after == NY_STOP_AFTER_ABI)) {
      ny_stage_emit_artifact(opt, opt->stop_after, &prog, &cg, parse_name,
                             cg.module, true);
    }
    if (opt && opt->collect_errors && type_errors_json)
      ny_stage_write_default_artifact(opt, "errors.v1.json", type_errors_json);
    else
      ny_stage_maybe_emit_errors(opt, error_stage, parse_name, error_message,
                                 type_errors);
    exit_code = 1;
    goto exit_success;
  }

  if (opt->stop_after == NY_STOP_AFTER_HM ||
      opt->stop_after == NY_STOP_AFTER_TRAIT ||
      opt->stop_after == NY_STOP_AFTER_FLOW ||
      opt->stop_after == NY_STOP_AFTER_ABI) {
    ny_stage_emit_artifact(opt, opt->stop_after, &prog, &cg, parse_name,
                           cg.module, true);
    goto exit_success;
  }

#ifndef _WIN32
  ny_module_job *mod_jobs = NULL;
  size_t mod_job_count = 0;
  ny_tick_t t_parallel = 0;
  size_t mods_len = 0;
  if (!fast_compiler && !use_std_bc_cache && ny_parallel_modules_enabled(opt)) {
    if (opt->do_timing)
      t_parallel = ny_ticks_now();
    if (parallel_modules && mods.len > 0) {
      mods_len = mods.len;
      mod_jobs = calloc(mods.len, sizeof(ny_module_job));
      if (mod_jobs) {
        mod_job_count = mods.len;
        int max_jobs = ny_parallel_module_jobs(opt, mods.len);
        size_t started = 0;
        size_t finished = 0;
        size_t running = 0;
        const char *tmp_dir = ny_get_temp_dir();
        while (finished < mods.len) {
          while (started < mods.len && running < (size_t)max_jobs) {
            if (!ny_spawn_module_job(opt, mods.names[started], tmp_dir,
                                     &mod_jobs[started])) {
              parallel_modules = false;
              break;
            }
            started++;
            running++;
          }
          if (!parallel_modules)
            break;
          int status = 0;
          pid_t pid = wait(&status);
          if (pid < 0) {
            parallel_modules = false;
            break;
          }
          for (size_t i = 0; i < started; i++) {
            if (mod_jobs[i].pid != pid)
              continue;
            if (WIFEXITED(status))
              mod_jobs[i].exit_code = WEXITSTATUS(status);
            else
              mod_jobs[i].exit_code = 1;
            if (mod_jobs[i].exit_code != 0)
              parallel_modules = false;
            break;
          }
          running--;
          finished++;
        }
        if (!parallel_modules) {
          for (size_t i = 0; i < started; i++) {
            if (mod_jobs[i].pid <= 0)
              continue;
            int st = 0;
            (void)waitpid(mod_jobs[i].pid, &st, 0);
          }
        }
      } else {
        parallel_modules = false;
      }
    } else {
      parallel_modules = false;
    }
  }
  ny_free_module_list(&mods);
  if (mods_len > 0 && !parallel_modules) {
    NY_LOG_ERR("Parallel module build failed\n");
    exit_code = 1;
    goto exit_success;
  }
  if (!parallel_modules && mod_jobs) {
    for (size_t i = 0; i < mod_job_count; i++) {
      if (mod_jobs[i].bc_path)
        (void)unlink(mod_jobs[i].bc_path);
      ny_module_job_free(&mod_jobs[i]);
    }
    free(mod_jobs);
    mod_jobs = NULL;
    mod_job_count = 0;
  }
#endif
  NY_LOG_V2("Emitting IR...\n");
  codegen_emit(&cg);
  fflush(stderr);
  if (cg.had_error) {
    NY_LOG_ERR("Codegen failed\n");
    ny_stage_maybe_emit_errors(opt, NY_STOP_AFTER_ABI, parse_name,
                               "codegen failed", 1);
    dump_debug_bundle(opt, source, cg.module);
    exit_code = 1;
    goto exit_success;
  }
  fflush(stderr);
  if (use_std_bc_cache) {
    ny_ir_externalize_std_definitions(opt, cg.module);
    if (!ny_link_module_cache(cg.ctx, cg.module, std_bc_cache)) {
      NY_LOG_ERR("Failed to link std cache: %s\n", std_bc_cache);
      exit_code = 1;
      goto exit_success;
    }
    codegen_rebind_llvm_symbols(&cg);
    ny_drop_jit_llvm_used_metadata(cg.module);
  }
  if (cg.emit_script) {
    fflush(stderr);
    NY_LOG_V2("Emitting script entry point...\n");
    script_fn = codegen_emit_script(&cg, opt->entry_name ? opt->entry_name
                                                         : "_ny_top_entry");
    if (cg.had_error) {
      NY_LOG_ERR("Codegen script entry failed\n");
      ny_stage_maybe_emit_errors(opt, NY_STOP_AFTER_ABI, parse_name,
                                 "codegen script entry failed", 1);
      dump_debug_bundle(opt, source, cg.module);
      exit_code = 1;
      goto exit_success;
    }
  }
#ifndef _WIN32
  if (parallel_modules && mod_jobs) {
    fflush(stderr);
    for (size_t i = 0; i < mod_job_count; i++) {
      if (!mod_jobs[i].bc_path)
        continue;
      if (!ny_link_module_cache(cg.ctx, cg.module, mod_jobs[i].bc_path)) {
        NY_LOG_ERR("Failed to link module cache for %s\n",
                   mod_jobs[i].name ? mod_jobs[i].name : "<module>");
        exit_code = 1;
        goto exit_success;
      }
      codegen_rebind_llvm_symbols(&cg);
      (void)unlink(mod_jobs[i].bc_path);
      ny_module_job_free(&mod_jobs[i]);
    }
    free(mod_jobs);
    mod_jobs = NULL;
    mod_job_count = 0;
    if (opt->do_timing && t_parallel)
      fprintf(stderr, "Parallel modules: %.4fs\n",
              ny_ticks_elapsed_sec(t_parallel));
  }
#endif
  fflush(stderr);
  codegen_debug_finalize(&cg);
  maybe_log_phase_time(opt->do_timing, "Codegen:", t_codegen);
  ny_trace_ir_stats("post_codegen", cg.module);
  ny_sanitize_platform_sections(cg.module);

  if (opt->dump_llvm) {
    LLVMModuleRef dump_mod = ny_prepare_ir_dump_module(opt, cg.module);
    LLVMDumpModule(dump_mod ? dump_mod : cg.module);
    if (dump_mod)
      LLVMDisposeModule(dump_mod);
  }
  if (ny_env_enabled("NY_IR_DUMP")) {
    LLVMModuleRef dump_mod = ny_prepare_ir_dump_module(opt, cg.module);
    LLVMDumpModule(dump_mod ? dump_mod : cg.module);
    if (dump_mod)
      LLVMDisposeModule(dump_mod);
  }
  ny_tick_t t_ver = (opt->do_timing && opt->verify_module) ? ny_ticks_now() : 0;
  fflush(stderr);
  if (!verify_module_if_needed(opt, cg.module)) {
    ny_stage_maybe_emit_errors(opt, NY_STOP_AFTER_ABI, parse_name,
                               "LLVM verification failed", 1);
    dump_debug_bundle(opt, source, cg.module);
    exit_code = 1;
    goto exit_success;
  }
  if (opt->do_timing && opt->verify_module)
    fprintf(stderr, "Verify:       %.4fs\n", ny_ticks_elapsed_sec(t_ver));
  if (write_compile_caches && !use_std_bc_cache && auto_std_bc_cache &&
      cg.module &&
      !loaded_from_cache && !opt->emit_module && std_mode != STD_MODE_NONE &&
      !has_local) {
    bool need_bc = ny_access(auto_std_bc_cache, R_OK) != 0;
    bool need_links = auto_std_bc_cache_needs_links ||
                      !ny_std_bc_cache_has_links(auto_std_bc_cache);
    bool cache_ok = true;
    if (need_bc) {
      cache_ok = ny_save_std_bc_cache_from_module(cg.module, auto_std_bc_cache);
      if (cache_ok && opt->verbose)
        fprintf(stderr, "Stdlib bitcode cache saved: %s\n", auto_std_bc_cache);
    }
    if (cache_ok && need_links)
      (void)ny_std_bc_cache_save_links(auto_std_bc_cache, &cg);
    auto_std_bc_cache_saved = cache_ok;
  }
  ny_tick_t t_opt = opt->do_timing ? ny_ticks_now() : 0;
  if (!(opt->run_jit && ny_jit_module_is_apple_arm64(cg.module)) ||
      ny_env_enabled("NYTRIX_JIT_HOST_ATTRS")) {
    ny_llvm_apply_host_attrs(cg.module);
  }
  run_dead_strip_if_needed(opt, &cg, cg.module);
  ny_dump_diagnose_ir_stage(opt, cg.module, "diag.pre.ll", "pre-opt");
  if (opt->emit_ir_pre_path) {
    LLVMModuleRef dump_mod = ny_prepare_ir_dump_module(opt, cg.module);
    ny_dump_ir_if_requested(dump_mod ? dump_mod : cg.module,
                            opt->emit_ir_pre_path, "pre-opt");
    if (dump_mod)
      LLVMDisposeModule(dump_mod);
  }

  if (!fast_compiler) {
    int eff_opt = opt->opt_level;
    if (parallel_modules && !ny_env_enabled("NYTRIX_PARALLEL_OPT_LINK")) {
      eff_opt = 0;
    }
    if (opt->run_jit) {
      eff_opt = ny_jit_effective_ir_opt_level(opt, cg.module, eff_opt);
    }

    if (cg.di_builder) {
      codegen_debug_finalize(&cg);
    }
    ny_llvm_optimize_module(cg.module, eff_opt, opt->opt_loops,
                            opt->opt_pipeline);
    ny_trace_ir_stats("post_opt", cg.module);
    if (opt->do_timing && (opt->opt_level > 0 || opt->opt_pipeline))
      fprintf(stderr, "Optimization: %.4fs\n", ny_ticks_elapsed_sec(t_opt));
  }
  ny_dump_diagnose_ir_stage(opt, cg.module, "diag.post.ll", "post-opt");
  if (opt->stop_after == NY_STOP_AFTER_OPT) {
    ny_stage_emit_artifact(opt, NY_STOP_AFTER_OPT, &prog, &cg, parse_name,
                           cg.module, true);
    goto exit_success;
  }
  if ((opt->emit_artifact_path || opt->emit_shapes) &&
      opt->stop_after == NY_STOP_AFTER_NONE) {
    ny_stage_emit_artifact(opt, NY_STOP_AFTER_OPT, &prog, &cg, parse_name,
                           cg.module, false);
  }
  if (write_compile_caches && jit_cache_file && !loaded_from_cache) {
#ifndef _WIN32
    if (native_cache_file && opt->run_jit && !opt->command_string) {
      ny_tick_t t_native = opt->do_timing ? ny_ticks_now() : 0;
      if (ny_jit_native_cache_save(native_cache_file, cg.module, opt->opt_level,
                                   (const char *const *)cg.links.data,
                                   cg.links.len)) {
        if (opt->verbose)
          fprintf(stderr, "JIT native cache saved: %s\n", native_cache_file);
      }
      maybe_log_phase_time(opt->do_timing, "Native Cache:", t_native);
    }
#endif
    if (ny_jit_cache_save(jit_cache_file, cg.module)) {
      if (opt->verbose)
        fprintf(stderr, "JIT cache saved: %s\n", jit_cache_file);
    }
  }
skip_compilation:
  if (jit_cache_file) {
    free(jit_cache_file);
    jit_cache_file = NULL;
  }
  if (write_compile_caches && !auto_std_bc_cache_saved && !use_std_bc_cache &&
      auto_std_bc_cache &&
      cg.module &&
      !loaded_from_cache && !opt->emit_module && std_mode != STD_MODE_NONE &&
      !has_local) {
    bool need_bc = ny_access(auto_std_bc_cache, R_OK) != 0;
    bool can_save_links = !need_bc || ny_save_std_bc_cache_from_module(
                                          cg.module, auto_std_bc_cache);
    if (can_save_links &&
        (need_bc || auto_std_bc_cache_needs_links ||
         !ny_std_bc_cache_has_links(auto_std_bc_cache))) {
      (void)ny_std_bc_cache_save_links(auto_std_bc_cache, &cg);
    }
    if (need_bc && can_save_links && opt->verbose) {
      fprintf(stderr, "Stdlib bitcode cache saved: %s\n", auto_std_bc_cache);
    }
  }
  free(auto_std_bc_cache);
  auto_std_bc_cache = NULL;
  if (loaded_from_cache && cg.module)
    codegen_prepare(&cg);
  ny_clear_origin_sections(cg.module);

  if (opt->emit_ir_path) {
    ny_ensure_parent_dir_for_path(opt->emit_ir_path);
    LLVMModuleRef dump_mod = ny_prepare_ir_dump_module(opt, cg.module);
    char *err = NULL;
    if (LLVMPrintModuleToFile(dump_mod ? dump_mod : cg.module,
                              opt->emit_ir_path, &err) != 0) {
      NY_LOG_ERR("Failed to write IR to %s\n", opt->emit_ir_path);
      if (err) {
        NY_LOG_ERR("%s\n", err);
        LLVMDisposeMessage(err);
      }
      exit_code = 1;
      goto exit_success;
    }
    if (dump_mod)
      LLVMDisposeModule(dump_mod);
    ny_trace_file_size("emit_ir", opt->emit_ir_path);
  }
  if (opt->emit_bc_path) {
    ny_ensure_parent_dir_for_path(opt->emit_bc_path);
    bool wrote = (LLVMWriteBitcodeToFile(cg.module, opt->emit_bc_path) == 0);
    if (!wrote)
      wrote = ny_reemit_bitcode_via_ir(cg.module, opt->emit_bc_path);
    if (wrote && !ny_verify_bitcode(cg.ctx, opt->emit_bc_path)) {
      wrote = false;
    }
    if (!wrote) {
      NY_LOG_ERR("Failed to write bitcode to %s\n", opt->emit_bc_path);
      exit_code = 1;
      goto exit_success;
    }
    ny_trace_file_size("emit_bc", opt->emit_bc_path);
  }
  if (opt->emit_asm_path) {
    ny_ensure_parent_dir_for_path(opt->emit_asm_path);
    if (!ny_llvm_emit_file(cg.module, opt->emit_asm_path, LLVMAssemblyFile,
                           opt->opt_level)) {
      NY_LOG_ERR("Failed to write assembly to %s\n", opt->emit_asm_path);
      exit_code = 1;
      goto exit_success;
    }
    ny_trace_file_size("emit_asm", opt->emit_asm_path);
  }
  ny_dump_diagnose_finalize(opt, cg.module, opt->opt_level);
  if (opt->output_file) {
    char obj[4096];
    char obj_name[64];
    snprintf(obj_name, sizeof(obj_name), "ny_tmp_%ld_%llu.o", (long)getpid(),
             (unsigned long long)ny_ticks_now());
    ny_join_path(obj, sizeof(obj), ny_get_temp_dir(), obj_name);
    ensure_aot_entry(&cg, script_fn);
    bool is_obj_only =
        (output_path && strlen(output_path) > 2 &&
         strcmp(output_path + strlen(output_path) - 2, ".o") == 0);
    if (is_obj_only) {
      ny_tick_t t_emit_obj = opt->do_timing ? ny_ticks_now() : 0;
      if (ny_llvm_emit_object(cg.module, output_path, opt->opt_level)) {
        maybe_log_phase_time(opt->do_timing, "Emit obj:", t_emit_obj);
        NY_LOG_SUCCESS("Saved object: %s\n", output_path);
        ny_trace_file_size("emit_obj", output_path);
        if (opt->run_aot) {
          NY_LOG_ERR("Cannot run AOT from object file\n");
          exit_code = 1;
          goto exit_success;
        }
      } else {
        maybe_log_phase_time(opt->do_timing, "Emit obj:", t_emit_obj);
        NY_LOG_ERR("Failed to emit object file\n");
        exit_code = 1;
        goto exit_success;
      }
    } else {
      ny_tick_t t_emit_obj = opt->do_timing ? ny_ticks_now() : 0;
      bool emitted_obj = ny_llvm_emit_object(cg.module, obj, opt->opt_level);
      maybe_log_phase_time(opt->do_timing, "Emit obj:", t_emit_obj);
      if (!emitted_obj) {
        NY_LOG_ERR("Failed to emit object file\n");
        dump_debug_bundle(opt, source, cg.module);
        exit_code = 1;
        goto exit_success;
      }
      ny_trace_file_size("emit_obj_tmp", obj);
      const char *cc = ny_builder_choose_cc();
      char rto[4096];
      char rto_name[64];
      snprintf(rto_name, sizeof(rto_name), "ny_rt_%ld_%llu.o", (long)getpid(),
               (unsigned long long)ny_ticks_now());
      ny_join_path(rto, sizeof(rto), ny_get_temp_dir(), rto_name);
      ny_opt_profile_kind_t runtime_profile =
          ny_opt_profile_kind_from_name(opt->opt_profile);
      bool runtime_speed = opt->opt_level >= 3 ||
                           runtime_profile == NY_OPT_PROFILE_SPEED ||
                           runtime_profile == NY_OPT_PROFILE_PEAK ||
                           ny_env_enabled("NYTRIX_RUNTIME_SPEED");
      const char *runtime_opt_env = ny_env_str_nonempty("NYTRIX_RUNTIME_OPT");
      int runtime_speed_level = runtime_speed ? 3 : 0;
      if (runtime_opt_env) {
        if (strcmp(runtime_opt_env, "0") == 0 ||
            strcmp(runtime_opt_env, "size") == 0)
          runtime_speed_level = 0;
        else if (strcmp(runtime_opt_env, "2") == 0)
          runtime_speed_level = 2;
        else if (strcmp(runtime_opt_env, "3") == 0 ||
                 strcmp(runtime_opt_env, "speed") == 0)
          runtime_speed_level = 3;
      }
      bool runtime_native = runtime_speed_level >= 3 &&
                            ny_env_enabled_default_on("NYTRIX_RUNTIME_NATIVE");
      NY_LOG_V2(
          "Compiling runtime to %s using %s (debug=%d speed=%d native=%d)...\n",
          rto, cc, opt->debug_symbols, runtime_speed_level,
          runtime_native ? 1 : 0);
      ny_tick_t t_runtime_obj = opt->do_timing ? ny_ticks_now() : 0;
      if (!ny_builder_compile_runtime(cc, rto, NULL, opt->debug_symbols,
                                      opt->gprof == 1, runtime_speed_level,
                                      runtime_native)) {
        maybe_log_phase_time(opt->do_timing, "Runtime obj:", t_runtime_obj);
        unlink(obj);
        dump_debug_bundle(opt, source, cg.module);
        exit_code = 1;
        goto exit_success;
      }
      maybe_log_phase_time(opt->do_timing, "Runtime obj:", t_runtime_obj);
      bool link_strip = (opt->strip_override == 1);
      if (output_path && *output_path) {
        char out_dir[1024];
        snprintf(out_dir, sizeof(out_dir), "%s", output_path);
        char *slash = strrchr(out_dir, '/');
        if (slash && slash != out_dir) {
          *slash = '\0';
          ny_ensure_dir_recursive(out_dir);
        }
      }
      NY_LOG_V2("Linking executable %s (strip=%d, debug=%d)...\n", output_path,
                link_strip, opt->debug_symbols);
      ny_link_lib_vec merged_libs;
      vec_init(&merged_libs);
      ny_link_lib_vec_merge(&merged_libs, opt, &cg);
      ny_tick_t t_link = opt->do_timing ? ny_ticks_now() : 0;
      if (!ny_builder_link(
              cc, obj, rto, NULL, NULL, 0,
              (const char *const *)opt->link_dirs.data, opt->link_dirs.len,
              (const char *const *)merged_libs.data, merged_libs.len,
              output_path, link_strip, opt->debug_symbols, opt->gprof == 1)) {
        maybe_log_phase_time(opt->do_timing, "Link:", t_link);
        unlink(obj);
        unlink(rto);
        dump_debug_bundle(opt, source, cg.module);
        exit_code = 1;
        ny_link_lib_vec_dispose(&merged_libs);
        goto exit_success;
      }
      maybe_log_phase_time(opt->do_timing, "Link:", t_link);
      ny_link_lib_vec_dispose(&merged_libs);
      unlink(obj);
      unlink(rto);
      if (aot_cache_path[0] != '\0' &&
          strcmp(aot_cache_path, output_path) != 0 &&
          ny_valid_native_artifact(output_path)) {
        (void)ny_copy_file(output_path, aot_cache_path);
      }
#ifdef _WIN32
      NY_LOG_SUCCESS("Saved EXE: %s\n", output_path);
#else
      NY_LOG_SUCCESS("Saved ELF: %s\n", output_path);
#endif
      ny_trace_file_size("emit_native", output_path);
    }
    if (opt->run_aot) {
      const char *argv_exec[] = {output_path, NULL};
      int rc = ny_exec_spawn(argv_exec);
      if (rc != 0)
        exit_code = rc;
      if (aot_run_temp)
        (void)unlink(output_path);
    }
  }
  if (opt->run_jit) {
#ifndef _WIN32
    if (native_cache_entry) {
      ny_tick_t t_run = opt->do_timing ? ny_ticks_now() : 0;
      native_cache_entry();
      extern void rt_print_flush(void);
      rt_print_flush();
      maybe_log_phase_time(opt->do_timing, "JIT Run:", t_run);
    } else
#endif
    {
      ny_tick_t t_jit = opt->do_timing ? ny_ticks_now() : 0;
      ny_jit_init_native_once();
      LLVMExecutionEngineRef ee;
      char *err = NULL;
      LLVMModuleRef jmod = cg.module;
      LLVMValueRef jit_script_fn = LLVMGetNamedFunction(jmod, "_ny_top_entry");
      LLVMValueRef jit_main_fn = LLVMGetNamedFunction(jmod, "main");

      if (opt->debug_symbols)
        LLVMStripModuleDebugInfo(jmod);
      if (ny_jit_module_is_apple_arm64(jmod))
        ny_drop_jit_llvm_used_metadata(jmod);
      struct LLVMMCJITCompilerOptions jopt;
      ny_jit_init_options(&jopt, jmod);
      jopt.OptLevel = (unsigned)ny_jit_effective_codegen_opt_level(opt, jmod);
      {
        const char *fast_isel_env = getenv("NYTRIX_JIT_FAST_ISEL");
        if (fast_isel_env && *fast_isel_env) {
          jopt.EnableFastISel = ny_env_is_truthy(fast_isel_env) ? 1 : 0;
        } else if (!ny_jit_module_is_apple_arm64(jmod)) {

          jopt.EnableFastISel = 1;
        }
      }
      if (LLVMCreateMCJITCompilerForModule(&ee, jmod, &jopt, sizeof(jopt),
                                           &err)) {
        NY_LOG_ERR("JIT failed: %s\n", err);
        dump_debug_bundle(opt, source, jmod);
        exit_code = 1;
        goto exit_success;
      }
      cg.module = NULL;
      {
        if (ny_env_enabled("NYTRIX_JIT_MAP_STRINGS")) {
          for (size_t i = 0; i < cg.interns.len; i++) {
            if (cg.interns.data[i].gv) {
              LLVMAddGlobalMapping(
                  ee, cg.interns.data[i].gv,
                  (void *)((char *)cg.interns.data[i].data - 64));
            }
            if (cg.interns.data[i].val &&
                cg.interns.data[i].val != cg.interns.data[i].gv) {
              LLVMAddGlobalMapping(ee, cg.interns.data[i].val,
                                   &cg.interns.data[i].data);
            }
          }
        }
      }
      register_jit_symbols(ee, jmod, &cg);
      ny_jit_map_unresolved_symbols(ee, jmod, NULL);
      ny_jit_write_perf_map(ee, jmod);
      maybe_log_phase_time(opt->do_timing, "JIT Init:", t_jit);
      ny_tick_t t_exec = opt->do_timing ? ny_ticks_now() : 0;
      uint64_t saddr = jit_script_fn
                           ? (uint64_t)LLVMGetPointerToGlobal(ee, jit_script_fn)
                           : 0;
      if (!saddr)
        saddr = LLVMGetFunctionAddress(ee, "_ny_top_entry");
      maybe_log_phase_time(opt->do_timing, "JIT Compile:", t_exec);
      ny_tick_t t_run = opt->do_timing ? ny_ticks_now() : 0;
      if (saddr) {
        ny_jit_prepare_execution();
        if (verbose_enabled >= 3)
          fprintf(stderr, "TRACE: Executing script...\n");
        ((void (*)(void))saddr)();
        uint64_t main_addr =
            (jit_main_fn && !ny_program_has_explicit_main_entry(&cg, cg.prog))
                ? (uint64_t)LLVMGetPointerToGlobal(ee, jit_main_fn)
                : 0;
        if (main_addr) {
          if (verbose_enabled >= 3)
            fprintf(stderr, "TRACE: Executing main...\n");
          (void)((int64_t (*)(void))main_addr)();
        }
        extern void rt_print_flush(void);
        rt_print_flush();
        if (verbose_enabled >= 3)
          fprintf(stderr, "TRACE: Script finished.\n");
      } else {
        if (verbose_enabled >= 3)
          fprintf(stderr, "TRACE: __script_top NOT FOUND\n");
      }
      maybe_log_phase_time(opt->do_timing, "JIT Run:", t_run);
      LLVMDisposeExecutionEngine(ee);
    }
  }
exit_success:
#ifndef _WIN32
  if (native_cache_handle)
    dlclose(native_cache_handle);
  if (native_cache_file)
    free(native_cache_file);
#endif
  if (user_src)
    free(user_src);
  if (std_src)
    free(std_src);
  if (source)
    free(source);
  if (type_errors_json)
    free(type_errors_json);
  if (uses)
    ny_str_list_free(uses, use_count);
  free(jit_cache_file);
  free(auto_std_bc_cache);
  codegen_dispose(&cg);
  program_free(&prog, arena);
  ny_lookup_prof_note_pipeline_ms(ny_ticks_elapsed_ms(pipeline_prof_t0));
  maybe_log_phase_time(opt->do_timing, "Total time:", t_start);
  return exit_code;
}
