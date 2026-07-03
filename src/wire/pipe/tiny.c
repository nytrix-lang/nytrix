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
  return (len > 2 && strcmp(path + len - 2, ".o") == 0) ||
         (len > 4 && strcmp(path + len - 4, ".obj") == 0);
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
  if (opt->native_backend != NY_NATIVE_BACKEND_LLVM || opt->native_dump_ir ||
      opt->nyir_run || opt->nyir_metadata_report)
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

