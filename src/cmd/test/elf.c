typedef struct {
  unsigned char e_ident[16];
  uint16_t e_type;
  uint16_t e_machine;
  uint32_t e_version;
  uint64_t e_entry;
  uint64_t e_phoff;
  uint64_t e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize;
  uint16_t e_phentsize;
  uint16_t e_phnum;
  uint16_t e_shentsize;
  uint16_t e_shnum;
  uint16_t e_shstrndx;
} ny_test_elf64_ehdr_t;

typedef struct {
  uint32_t sh_name;
  uint32_t sh_type;
  uint64_t sh_flags;
  uint64_t sh_addr;
  uint64_t sh_offset;
  uint64_t sh_size;
  uint32_t sh_link;
  uint32_t sh_info;
  uint64_t sh_addralign;
  uint64_t sh_entsize;
} ny_test_elf64_shdr_t;

typedef struct {
  uint32_t st_name;
  unsigned char st_info;
  unsigned char st_other;
  uint16_t st_shndx;
  uint64_t st_value;
  uint64_t st_size;
} ny_test_elf64_sym_t;

typedef struct {
  uint64_t r_offset;
  uint64_t r_info;
  int64_t r_addend;
} ny_test_elf64_rela_t;

typedef struct {
  uint32_t p_type;
  uint32_t p_flags;
  uint64_t p_offset;
  uint64_t p_vaddr;
  uint64_t p_paddr;
  uint64_t p_filesz;
  uint64_t p_memsz;
  uint64_t p_align;
} ny_test_elf64_phdr_t;

typedef struct {
  unsigned char e_ident[16];
  uint16_t e_type;
  uint16_t e_machine;
  uint32_t e_version;
  uint32_t e_entry;
  uint32_t e_phoff;
  uint32_t e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize;
  uint16_t e_phentsize;
  uint16_t e_phnum;
  uint16_t e_shentsize;
  uint16_t e_shnum;
  uint16_t e_shstrndx;
} ny_test_elf32_ehdr_t;

typedef struct {
  uint32_t sh_name;
  uint32_t sh_type;
  uint32_t sh_flags;
  uint32_t sh_addr;
  uint32_t sh_offset;
  uint32_t sh_size;
  uint32_t sh_link;
  uint32_t sh_info;
  uint32_t sh_addralign;
  uint32_t sh_entsize;
} ny_test_elf32_shdr_t;

typedef struct {
  uint32_t st_name;
  uint32_t st_value;
  uint32_t st_size;
  unsigned char st_info;
  unsigned char st_other;
  uint16_t st_shndx;
} ny_test_elf32_sym_t;

typedef struct {
  uint32_t r_offset;
  uint32_t r_info;
} ny_test_elf32_rel_t;

typedef struct {
  uint32_t p_type;
  uint32_t p_offset;
  uint32_t p_vaddr;
  uint32_t p_paddr;
  uint32_t p_filesz;
  uint32_t p_memsz;
  uint32_t p_flags;
  uint32_t p_align;
} ny_test_elf32_phdr_t;

typedef struct {
  char name[256];
  uint64_t off;
  uint16_t shndx;
  bool defined;
} ny_test_link_sym_t;

typedef enum {
  NY_TEST_LINK_RET_I64,
  NY_TEST_LINK_RET_F64,
  NY_TEST_LINK_RET_F32,
  NY_TEST_LINK_RET_I32,
  NY_TEST_LINK_RET_U32,
  NY_TEST_LINK_RET_I16,
  NY_TEST_LINK_RET_U16,
  NY_TEST_LINK_RET_I8,
  NY_TEST_LINK_RET_U8,
  NY_TEST_LINK_RET_BOOL,
} ny_test_link_ret_kind_t;

typedef struct {
  char ar_name[16];
  char ar_date[12];
  char ar_uid[6];
  char ar_gid[6];
  char ar_mode[8];
  char ar_size[10];
  char ar_fmag[2];
} ny_test_ar_hdr_t;

typedef struct {
  uint32_t *offsets;
  char     *names;
  uint32_t  count;
} ny_test_ar_symtab_t;

static bool test_link_ret_is_f64(ny_test_link_ret_kind_t kind) {
  return kind == NY_TEST_LINK_RET_F64;
}

static bool test_link_ret_is_f32(ny_test_link_ret_kind_t kind) {
  return kind == NY_TEST_LINK_RET_F32;
}

static unsigned test_link_ret_bits(ny_test_link_ret_kind_t kind) {
  switch (kind) {
  case NY_TEST_LINK_RET_I8:
  case NY_TEST_LINK_RET_U8:
  case NY_TEST_LINK_RET_BOOL:
    return 8;
  case NY_TEST_LINK_RET_I16:
  case NY_TEST_LINK_RET_U16:
    return 16;
  case NY_TEST_LINK_RET_I32:
  case NY_TEST_LINK_RET_U32:
    return 32;
  case NY_TEST_LINK_RET_I64:
  case NY_TEST_LINK_RET_F64:
  case NY_TEST_LINK_RET_F32:
  default:
    return 64;
  }
}

static void path_dir_only(char *path) {
  char *last_slash = strrchr(path, '/');
  char *last_backslash = strrchr(path, '\\');
  char *sep = last_slash;
  if (!sep || (last_backslash && last_backslash > sep)) sep = last_backslash;
  if (sep) *sep = '\0';
  else path[0] = '\0';
}

static void companion_tool_path(char *out, size_t out_sz, const char *bin, const char *tool) {
  char dir[PATH_MAX];
  snprintf(dir, sizeof(dir), "%s", bin ? bin : "");
  path_dir_only(dir);
  nyt_path_join(out, out_sz, dir, tool);
}

static ny_test_proc_t run_one_start(const char *bin, const char *path, const char *std_path,
                                    const char *std_bc, const char *output_path) {
  char flags_buf[1024];
  char *flags = NULL;
  char *expect = NULL;
  read_error_meta(path, &flags, &expect);
  flags_buf[0] = '\0';
  char *flagv[32];
  int flagc = 0;
  int has_native_backend = 0;
  if (flags && *flags) {
    snprintf(flags_buf, sizeof(flags_buf), "%s", flags);
    trim_inplace(flags_buf);
    has_native_backend = native_backend_explicit(flags_buf);
    flagc = split_words(flags_buf, flagv, 32);
  }

  char *argv[80];
  int argc = 0;
  argv[argc++] = (char *)bin;
  push_test_warn_arg(argv, &argc, 80);
  if (std_path) {
    argv[argc++] = "--std";
    argv[argc++] = (char *)std_path;
  }
  if (std_bc) {
    argv[argc++] = "--std-bc";
    argv[argc++] = (char *)std_bc;
  }
  if (path_is_native_runtime_test(path) && !has_native_backend && argc < 78) {
    argv[argc++] = "--native-backend";
    argv[argc++] = "x86_64";
  }
  for (int i = 0; i < flagc && argc < 76; i++)
    argv[argc++] = flagv[i];
  argv[argc++] = (char *)path;
  argv[argc] = NULL;

#ifdef _WIN32
  ny_test_proc_t proc = ny_test_spawn_argv(argv, output_path, output_path ? 0 : 1);
  error_meta_free(flags, expect);
  return proc;
#else
  ny_test_proc_t pid = fork();
  if (pid == 0) {
    apply_test_child_env();
    if (output_path) {
        int fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
    } else {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
    }
    execv(bin, argv);
    _exit(127);
  }
  error_meta_free(flags, expect);
  return pid;
#endif
}

static int child_status_rc(int status) {
#ifdef _WIN32
  return status;
#else
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);
  return 128;
#endif
}

static bool test_write_all_file(const char *path, const unsigned char *data, size_t len) {
  FILE *f = fopen(path, "wb");
  if (!f)
    return false;
  bool ok = fwrite(data, 1, len, f) == len;
  if (fclose(f) != 0)
    ok = false;
  return ok;
}

static bool test_u8(unsigned char **p, size_t *n, unsigned char v) {
  if (!p || !*p || !n || *n == 0)
    return false;
  *(*p)++ = v;
  (*n)--;
  return true;
}

static bool test_emit(unsigned char **p, size_t *n, const unsigned char *data, size_t len) {
  if (!p || !*p || !n || *n < len)
    return false;
  memcpy(*p, data, len);
  *p += len;
  *n -= len;
  return true;
}

static bool test_u32le(unsigned char **p, size_t *n, uint32_t v) {
  unsigned char b[4] = {(unsigned char)v, (unsigned char)(v >> 8),
                        (unsigned char)(v >> 16), (unsigned char)(v >> 24)};
  return test_emit(p, n, b, sizeof(b));
}

static bool test_u64le(unsigned char **p, size_t *n, uint64_t v) {
  unsigned char b[8] = {(unsigned char)v, (unsigned char)(v >> 8),
                        (unsigned char)(v >> 16), (unsigned char)(v >> 24),
                        (unsigned char)(v >> 32), (unsigned char)(v >> 40),
                        (unsigned char)(v >> 48), (unsigned char)(v >> 56)};
  return test_emit(p, n, b, sizeof(b));
}

static int test_link_sym_index(const ny_test_link_sym_t *syms, size_t count,
                               const char *name) {
  if (!name)
    return -1;
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(syms[i].name, name) == 0)
      return (int)i;
  }
  return -1;
}

static bool test_has_unsupported_external_runtime_symbol(const char *obj_path) {
  char *buf = read_small_file(obj_path);
  if (!buf)
    return true;
  bool has = strstr(buf, "printf") || strstr(buf, "fabs");
  free(buf);
  return has;
}

static bool test_link_add_def(ny_test_link_sym_t *defs, size_t *def_count, const char *name,
                              uint64_t off) {
  if (!defs || !def_count || !name)
    return false;
  int existing = test_link_sym_index(defs, *def_count, name);
  if (existing >= 0) {
    defs[existing].off = off;
    defs[existing].defined = true;
    return true;
  }
  if (*def_count >= 256)
    return false;
  snprintf(defs[*def_count].name, sizeof(defs[*def_count].name), "%s", name);
  defs[*def_count].off = off;
  defs[*def_count].shndx = 0;
  defs[*def_count].defined = true;
  (*def_count)++;
  return true;
}

static bool test_link_is_memset_symbol(const char *name) {
  return name && (strcmp(name, "memset") == 0 ||
                  strcmp(name, "__memset") == 0 ||
                  strcmp(name, "ny_fn_memset") == 0);
}

static bool test_link_is_memcpy_symbol(const char *name) {
  return name && (strcmp(name, "memcpy") == 0 ||
                  strcmp(name, "__memcpy") == 0 ||
                  strcmp(name, "ny_fn_memcpy") == 0);
}

static bool test_link_is_memmove_symbol(const char *name) {
  return name && (strcmp(name, "memmove") == 0 ||
                  strcmp(name, "__memmove") == 0 ||
                  strcmp(name, "ny_fn_memmove") == 0);
}

static bool test_link_is_memchr_symbol(const char *name) {
  return name && (strcmp(name, "memchr") == 0 ||
                  strcmp(name, "__memchr") == 0 ||
                  strcmp(name, "ny_fn_memchr") == 0);
}

static bool test_link_is_memcmp_symbol(const char *name) {
  return name && (strcmp(name, "memcmp") == 0 ||
                  strcmp(name, "__memcmp") == 0 ||
                  strcmp(name, "ny_fn_memcmp") == 0);
}

static bool test_link_is_strlen_symbol(const char *name) {
  return name && (strcmp(name, "strlen") == 0 ||
                  strcmp(name, "__strlen") == 0 ||
                  strcmp(name, "ny_fn_strlen") == 0);
}

static bool test_link_is_strcmp_symbol(const char *name) {
  return name && (strcmp(name, "strcmp") == 0 ||
                  strcmp(name, "__strcmp") == 0 ||
                  strcmp(name, "ny_fn_strcmp") == 0);
}

static bool test_link_is_strchr_symbol(const char *name) {
  return name && (strcmp(name, "strchr") == 0 ||
                  strcmp(name, "__strchr") == 0 ||
                  strcmp(name, "ny_fn_strchr") == 0);
}

static bool test_link_is_realloc_symbol(const char *name) {
  return name && (strcmp(name, "realloc") == 0 ||
                  strcmp(name, "__realloc") == 0 ||
                  strcmp(name, "ny_fn_realloc") == 0);
}

static bool test_link_is_calloc_symbol(const char *name) {
  return name && (strcmp(name, "calloc") == 0 ||
                  strcmp(name, "__calloc") == 0 ||
                  strcmp(name, "ny_fn_calloc") == 0);
}

static bool test_emit_malloc_stub(unsigned char *dst, size_t cap, size_t *out_len) {
  unsigned char *p = dst;
  size_t n = cap;
  const unsigned char code[] = {
      0x48, 0x89, 0xfe,                         /* mov %rdi,%rsi */
      0x48, 0x31, 0xff,                         /* xor %rdi,%rdi */
      0xba, 0x03, 0x00, 0x00, 0x00,             /* mov $3,%edx */
      0x41, 0xba, 0x22, 0x00, 0x00, 0x00,       /* mov $0x22,%r10d */
      0x49, 0xc7, 0xc0, 0xff, 0xff, 0xff, 0xff, /* mov $-1,%r8 */
      0x45, 0x31, 0xc9,                         /* xor %r9d,%r9d */
      0xb8, 0x09, 0x00, 0x00, 0x00,             /* mov $9,%eax */
      0x0f, 0x05,                               /* syscall */
      0xc3                                      /* ret */
  };
  if (!test_emit(&p, &n, code, sizeof(code)))
    return false;
  if (out_len)
    *out_len = sizeof(code);
  return true;
}

static bool test_emit_free_stub(unsigned char *dst, size_t cap, size_t *out_len) {
  unsigned char *p = dst;
  size_t n = cap;
  const unsigned char code[] = {
      0x31, 0xc0, /* xor %eax,%eax */
      0xc3        /* ret */
  };
  if (!test_emit(&p, &n, code, sizeof(code)))
    return false;
  if (out_len)
    *out_len = sizeof(code);
  return true;
}

static bool test_emit_realloc_stub(unsigned char *dst, size_t cap, size_t *out_len) {
  unsigned char *p = dst;
  size_t n = cap;
  const unsigned char code[] = {
      0x48, 0x85, 0xf6,                         /* test %rsi,%rsi */
      0x0f, 0x84, 0x80, 0x00, 0x00, 0x00,       /* je zero */
      0x48, 0x85, 0xff,                         /* test %rdi,%rdi */
      0x75, 0x26,                               /* jne have_ptr */
      0x48, 0x89, 0xf7,                         /* mov %rsi,%rdi */
      0x48, 0x89, 0xfe,                         /* mov %rdi,%rsi */
      0x48, 0x31, 0xff,                         /* xor %rdi,%rdi */
      0xba, 0x03, 0x00, 0x00, 0x00,             /* mov $3,%edx */
      0x41, 0xba, 0x22, 0x00, 0x00, 0x00,       /* mov $0x22,%r10d */
      0x49, 0xc7, 0xc0, 0xff, 0xff, 0xff, 0xff, /* mov $-1,%r8 */
      0x45, 0x31, 0xc9,                         /* xor %r9d,%r9d */
      0xb8, 0x09, 0x00, 0x00, 0x00,             /* mov $9,%eax */
      0x0f, 0x05,                               /* syscall */
      0xc3,                                     /* ret */
      0x53,                                     /* push %rbx */
      0x41, 0x54,                               /* push %r12 */
      0x48, 0x89, 0xfb,                         /* mov %rdi,%rbx */
      0x49, 0x89, 0xf4,                         /* mov %rsi,%r12 */
      0x4c, 0x89, 0xe7,                         /* mov %r12,%rdi */
      0x48, 0x89, 0xfe,                         /* mov %rdi,%rsi */
      0x48, 0x31, 0xff,                         /* xor %rdi,%rdi */
      0xba, 0x03, 0x00, 0x00, 0x00,             /* mov $3,%edx */
      0x41, 0xba, 0x22, 0x00, 0x00, 0x00,       /* mov $0x22,%r10d */
      0x49, 0xc7, 0xc0, 0xff, 0xff, 0xff, 0xff, /* mov $-1,%r8 */
      0x45, 0x31, 0xc9,                         /* xor %r9d,%r9d */
      0xb8, 0x09, 0x00, 0x00, 0x00,             /* mov $9,%eax */
      0x0f, 0x05,                               /* syscall */
      0x49, 0x89, 0xc0,                         /* mov %rax,%r8 */
      0x48, 0x89, 0xc7,                         /* mov %rax,%rdi */
      0x48, 0x89, 0xde,                         /* mov %rbx,%rsi */
      0x4c, 0x89, 0xe2,                         /* mov %r12,%rdx */
      0x48, 0x85, 0xd2,                         /* test %rdx,%rdx */
      0x74, 0x0f,                               /* je done */
      0x8a, 0x0e,                               /* mov (%rsi),%cl */
      0x88, 0x0f,                               /* mov %cl,(%rdi) */
      0x48, 0xff, 0xc6,                         /* inc %rsi */
      0x48, 0xff, 0xc7,                         /* inc %rdi */
      0x48, 0xff, 0xca,                         /* dec %rdx */
      0x75, 0xec,                               /* jne copy_loop */
      0x4c, 0x89, 0xc0,                         /* mov %r8,%rax */
      0x41, 0x5c,                               /* pop %r12 */
      0x5b,                                     /* pop %rbx */
      0xc3,                                     /* ret */
      0x31, 0xc0,                               /* xor %eax,%eax */
      0xc3                                      /* ret */
  };
  if (!test_emit(&p, &n, code, sizeof(code)))
    return false;
  if (out_len)
    *out_len = sizeof(code);
  return true;
}

static bool test_emit_calloc_stub(unsigned char *dst, size_t cap, size_t *out_len) {
  unsigned char *p = dst;
  size_t n = cap;
  const unsigned char code[] = {
      0x48, 0x0f, 0xaf, 0xfe,                   /* imul %rsi,%rdi */
      0x70, 0x1f,                               /* jo unsupported */
      0x48, 0x89, 0xfe,                         /* mov %rdi,%rsi */
      0x48, 0x31, 0xff,                         /* xor %rdi,%rdi */
      0xba, 0x03, 0x00, 0x00, 0x00,             /* mov $3,%edx */
      0x41, 0xba, 0x22, 0x00, 0x00, 0x00,       /* mov $0x22,%r10d */
      0x49, 0xc7, 0xc0, 0xff, 0xff, 0xff, 0xff, /* mov $-1,%r8 */
      0x45, 0x31, 0xc9,                         /* xor %r9d,%r9d */
      0xb8, 0x09, 0x00, 0x00, 0x00,             /* mov $9,%eax */
      0x0f, 0x05,                               /* syscall */
      0xc3,                                     /* ret */
      0x31, 0xc0,                               /* xor %eax,%eax */
      0xc3                                      /* ret */
  };
  if (!test_emit(&p, &n, code, sizeof(code)))
    return false;
  if (out_len)
    *out_len = sizeof(code);
  return true;
}

static bool test_emit_memset_stub(unsigned char *dst, size_t cap, size_t *out_len) {
  unsigned char *p = dst;
  size_t n = cap;
  const unsigned char code[] = {
      0x48, 0x89, 0xf8, /* mov %rdi,%rax */
      0x48, 0x85, 0xd2, /* test %rdx,%rdx */
      0x74, 0x0b,       /* je done */
      0x40, 0x88, 0x37, /* mov %sil,(%rdi) */
      0x48, 0xff, 0xc7, /* inc %rdi */
      0x48, 0xff, 0xca, /* dec %rdx */
      0x75, 0xf5,       /* jne loop */
      0xc3              /* ret */
  };
  if (!test_emit(&p, &n, code, sizeof(code)))
    return false;
  if (out_len)
    *out_len = sizeof(code);
  return true;
}

static bool test_emit_memcpy_stub(unsigned char *dst, size_t cap, size_t *out_len) {
  unsigned char *p = dst;
  size_t n = cap;
  const unsigned char code[] = {
      0x48, 0x89, 0xf8, /* mov %rdi,%rax */
      0x48, 0x85, 0xd2, /* test %rdx,%rdx */
      0x74, 0x0f,       /* je done */
      0x8a, 0x0e,       /* mov (%rsi),%cl */
      0x88, 0x0f,       /* mov %cl,(%rdi) */
      0x48, 0xff, 0xc6, /* inc %rsi */
      0x48, 0xff, 0xc7, /* inc %rdi */
      0x48, 0xff, 0xca, /* dec %rdx */
      0x75, 0xf1,       /* jne loop */
      0xc3              /* ret */
  };
  if (!test_emit(&p, &n, code, sizeof(code)))
    return false;
  if (out_len)
    *out_len = sizeof(code);
  return true;
}

static bool test_emit_memmove_stub(unsigned char *dst, size_t cap, size_t *out_len) {
  unsigned char *p = dst;
  size_t n = cap;
  const unsigned char code[] = {
      0x48, 0x89, 0xf8, /* mov %rdi,%rax */
      0x48, 0x85, 0xd2, /* test %rdx,%rdx */
      0x74, 0x1a,       /* je done */
      0x48, 0x39, 0xf7, /* cmp %rsi,%rdi */
      0x72, 0x12,       /* jb forward */
      0x48, 0x01, 0xd7, /* add %rdx,%rdi */
      0x48, 0x01, 0xd6, /* add %rdx,%rsi */
      0x48, 0xff, 0xcf, /* dec %rdi */
      0x48, 0xff, 0xce, /* dec %rsi */
      0x8a, 0x0e,       /* mov (%rsi),%cl */
      0x88, 0x0f,       /* mov %cl,(%rdi) */
      0x48, 0xff, 0xca, /* dec %rdx */
      0x75, 0xf1,       /* jne backward */
      0xc3,             /* ret */
      0x8a, 0x0e,       /* mov (%rsi),%cl */
      0x88, 0x0f,       /* mov %cl,(%rdi) */
      0x48, 0xff, 0xc6, /* inc %rsi */
      0x48, 0xff, 0xc7, /* inc %rdi */
      0x48, 0xff, 0xca, /* dec %rdx */
      0x75, 0xf1,       /* jne forward */
      0xc3              /* ret */
  };
  if (!test_emit(&p, &n, code, sizeof(code)))
    return false;
  if (out_len)
    *out_len = sizeof(code);
  return true;
}

static bool test_emit_memchr_stub(unsigned char *dst, size_t cap, size_t *out_len) {
  unsigned char *p = dst;
  size_t n = cap;
  const unsigned char code[] = {
      0x48, 0x85, 0xd2, /* test %rdx,%rdx */
      0x74, 0x10,       /* je not_found */
      0x0f, 0xb6, 0x0f, /* movzbl (%rdi),%ecx */
      0x40, 0x38, 0xf1, /* cmp %sil,%cl */
      0x74, 0x0b,       /* je found */
      0x48, 0xff, 0xc7, /* inc %rdi */
      0x48, 0xff, 0xca, /* dec %rdx */
      0x75, 0xf0,       /* jne loop */
      0x31, 0xc0,       /* xor %eax,%eax */
      0xc3,             /* ret */
      0x48, 0x89, 0xf8, /* mov %rdi,%rax */
      0xc3              /* ret */
  };
  if (!test_emit(&p, &n, code, sizeof(code)))
    return false;
  if (out_len)
    *out_len = sizeof(code);
  return true;
}

static bool test_emit_memcmp_stub(unsigned char *dst, size_t cap, size_t *out_len) {
  unsigned char *p = dst;
  size_t n = cap;
  const unsigned char code[] = {
      0x48, 0x85, 0xd2, /* test %rdx,%rdx */
      0x74, 0x15,       /* je equal */
      0x0f, 0xb6, 0x07, /* movzbl (%rdi),%eax */
      0x0f, 0xb6, 0x0e, /* movzbl (%rsi),%ecx */
      0x39, 0xc8,       /* cmp %ecx,%eax */
      0x75, 0x0e,       /* jne diff */
      0x48, 0xff, 0xc7, /* inc %rdi */
      0x48, 0xff, 0xc6, /* inc %rsi */
      0x48, 0xff, 0xca, /* dec %rdx */
      0x75, 0xeb,       /* jne loop */
      0x31, 0xc0,       /* xor %eax,%eax */
      0xc3,             /* ret */
      0x29, 0xc8,       /* sub %ecx,%eax */
      0xc3              /* ret */
  };
  if (!test_emit(&p, &n, code, sizeof(code)))
    return false;
  if (out_len)
    *out_len = sizeof(code);
  return true;
}

static bool test_emit_strlen_stub(unsigned char *dst, size_t cap, size_t *out_len) {
  unsigned char *p = dst;
  size_t n = cap;
  const unsigned char code[] = {
      0x48, 0x31, 0xc0,       /* xor %rax,%rax */
      0x80, 0x3c, 0x07, 0x00, /* cmpb $0,(%rdi,%rax) */
      0x74, 0x05,             /* je done */
      0x48, 0xff, 0xc0,       /* inc %rax */
      0xeb, 0xf5,             /* jmp loop */
      0xc3                    /* ret */
  };
  if (!test_emit(&p, &n, code, sizeof(code)))
    return false;
  if (out_len)
    *out_len = sizeof(code);
  return true;
}

static bool test_emit_strcmp_stub(unsigned char *dst, size_t cap, size_t *out_len) {
  unsigned char *p = dst;
  size_t n = cap;
  const unsigned char code[] = {
      0x0f, 0xb6, 0x07, /* movzbl (%rdi),%eax */
      0x0f, 0xb6, 0x0e, /* movzbl (%rsi),%ecx */
      0x39, 0xc8,       /* cmp %ecx,%eax */
      0x75, 0x0f,       /* jne diff */
      0x84, 0xc0,       /* test %al,%al */
      0x74, 0x08,       /* je equal */
      0x48, 0xff, 0xc7, /* inc %rdi */
      0x48, 0xff, 0xc6, /* inc %rsi */
      0xeb, 0xea,       /* jmp loop */
      0x31, 0xc0,       /* xor %eax,%eax */
      0xc3,             /* ret */
      0x48, 0x29, 0xc8, /* sub %rcx,%rax */
      0xc3              /* ret */
  };
  if (!test_emit(&p, &n, code, sizeof(code)))
    return false;
  if (out_len)
    *out_len = sizeof(code);
  return true;
}

static bool test_emit_strchr_stub(unsigned char *dst, size_t cap, size_t *out_len) {
  unsigned char *p = dst;
  size_t n = cap;
  const unsigned char code[] = {
      0x0f, 0xb6, 0x07, /* movzbl (%rdi),%eax */
      0x40, 0x38, 0xf0, /* cmp %sil,%al */
      0x74, 0x0c,       /* je found */
      0x84, 0xc0,       /* test %al,%al */
      0x74, 0x05,       /* je not_found */
      0x48, 0xff, 0xc7, /* inc %rdi */
      0xeb, 0xef,       /* jmp loop */
      0x31, 0xc0,       /* xor %eax,%eax */
      0xc3,             /* ret */
      0x48, 0x89, 0xf8, /* mov %rdi,%rax */
      0xc3              /* ret */
  };
  if (!test_emit(&p, &n, code, sizeof(code)))
    return false;
  if (out_len)
    *out_len = sizeof(code);
  return true;
}

static bool test_emit_malloc_stub32(unsigned char *dst, size_t cap, size_t *out_len) {
  unsigned char *p = dst;
  size_t n = cap;
  const unsigned char code[] = {
      0x53,                         /* push %ebx */
      0x56,                         /* push %esi */
      0x57,                         /* push %edi */
      0x55,                         /* push %ebp */
      0x31, 0xdb,                   /* xor %ebx,%ebx */
      0x8b, 0x4c, 0x24, 0x14,       /* mov 20(%esp),%ecx */
      0xba, 0x03, 0x00, 0x00, 0x00, /* mov $3,%edx */
      0xbe, 0x22, 0x00, 0x00, 0x00, /* mov $0x22,%esi */
      0xbf, 0xff, 0xff, 0xff, 0xff, /* mov $-1,%edi */
      0x31, 0xed,                   /* xor %ebp,%ebp */
      0xb8, 0xc0, 0x00, 0x00, 0x00, /* mov $192,%eax (mmap2) */
      0xcd, 0x80,                   /* int $0x80 */
      0x5d,                         /* pop %ebp */
      0x5f,                         /* pop %edi */
      0x5e,                         /* pop %esi */
      0x5b,                         /* pop %ebx */
      0xc3                          /* ret */
  };
  if (!test_emit(&p, &n, code, sizeof(code)))
    return false;
  if (out_len)
    *out_len = sizeof(code);
  return true;
}

static bool test_emit_memset_stub32(unsigned char *dst, size_t cap, size_t *out_len) {
  unsigned char *p = dst;
  size_t n = cap;
  const unsigned char code[] = {
      0x57,                   /* push %edi */
      0x8b, 0x7c, 0x24, 0x08, /* mov 8(%esp),%edi */
      0x89, 0xf8,             /* mov %edi,%eax */
      0x8b, 0x54, 0x24, 0x0c, /* mov 12(%esp),%edx */
      0x8b, 0x4c, 0x24, 0x10, /* mov 16(%esp),%ecx */
      0x85, 0xc9,             /* test %ecx,%ecx */
      0x74, 0x06,             /* je done */
      0x88, 0x17,             /* mov %dl,(%edi) */
      0x47,                   /* inc %edi */
      0x49,                   /* dec %ecx */
      0x75, 0xfa,             /* jne loop */
      0x5f,                   /* pop %edi */
      0xc3                    /* ret */
  };
  if (!test_emit(&p, &n, code, sizeof(code)))
    return false;
  if (out_len)
    *out_len = sizeof(code);
  return true;
}

static bool test_emit_memcpy_stub32(unsigned char *dst, size_t cap, size_t *out_len) {
  unsigned char *p = dst;
  size_t n = cap;
  const unsigned char code[] = {
      0x56,                   /* push %esi */
      0x57,                   /* push %edi */
      0x8b, 0x7c, 0x24, 0x0c, /* mov 12(%esp),%edi */
      0x89, 0xf8,             /* mov %edi,%eax */
      0x8b, 0x74, 0x24, 0x10, /* mov 16(%esp),%esi */
      0x8b, 0x4c, 0x24, 0x14, /* mov 20(%esp),%ecx */
      0x85, 0xc9,             /* test %ecx,%ecx */
      0x74, 0x09,             /* je done */
      0x8a, 0x16,             /* mov (%esi),%dl */
      0x88, 0x17,             /* mov %dl,(%edi) */
      0x46,                   /* inc %esi */
      0x47,                   /* inc %edi */
      0x49,                   /* dec %ecx */
      0x75, 0xf7,             /* jne loop */
      0x5f,                   /* pop %edi */
      0x5e,                   /* pop %esi */
      0xc3                    /* ret */
  };
  if (!test_emit(&p, &n, code, sizeof(code)))
    return false;
  if (out_len)
    *out_len = sizeof(code);
  return true;
}

static bool test_emit_memmove_stub32(unsigned char *dst, size_t cap, size_t *out_len) {
  unsigned char *p = dst;
  size_t n = cap;
  const unsigned char code[] = {
      0x56,                   /* push %esi */
      0x57,                   /* push %edi */
      0x8b, 0x7c, 0x24, 0x0c, /* mov 12(%esp),%edi */
      0x89, 0xf8,             /* mov %edi,%eax */
      0x8b, 0x74, 0x24, 0x10, /* mov 16(%esp),%esi */
      0x8b, 0x4c, 0x24, 0x14, /* mov 20(%esp),%ecx */
      0x85, 0xc9,             /* test %ecx,%ecx */
      0x74, 0x1d,             /* je done */
      0x39, 0xf7,             /* cmp %esi,%edi */
      0x72, 0x10,             /* jb forward */
      0x01, 0xcf,             /* add %ecx,%edi */
      0x01, 0xce,             /* add %ecx,%esi */
      0x4f,                   /* dec %edi */
      0x4e,                   /* dec %esi */
      0x8a, 0x16,             /* mov (%esi),%dl */
      0x88, 0x17,             /* mov %dl,(%edi) */
      0x49,                   /* dec %ecx */
      0x75, 0xf7,             /* jne backward */
      0x5f,                   /* pop %edi */
      0x5e,                   /* pop %esi */
      0xc3,                   /* ret */
      0x8a, 0x16,             /* mov (%esi),%dl */
      0x88, 0x17,             /* mov %dl,(%edi) */
      0x46,                   /* inc %esi */
      0x47,                   /* inc %edi */
      0x49,                   /* dec %ecx */
      0x75, 0xf7,             /* jne forward */
      0x5f,                   /* pop %edi */
      0x5e,                   /* pop %esi */
      0xc3                    /* ret */
  };
  if (!test_emit(&p, &n, code, sizeof(code)))
    return false;
  if (out_len)
    *out_len = sizeof(code);
  return true;
}

static bool test_emit_memchr_stub32(unsigned char *dst, size_t cap, size_t *out_len) {
  unsigned char *p = dst;
  size_t n = cap;
  const unsigned char code[] = {
      0x57,                   /* push %edi */
      0x8b, 0x7c, 0x24, 0x08, /* mov 8(%esp),%edi */
      0x8b, 0x54, 0x24, 0x0c, /* mov 12(%esp),%edx */
      0x8b, 0x4c, 0x24, 0x10, /* mov 16(%esp),%ecx */
      0x85, 0xc9,             /* test %ecx,%ecx */
      0x74, 0x08,             /* je not_found */
      0x3a, 0x17,             /* cmp (%edi),%dl */
      0x74, 0x08,             /* je found */
      0x47,                   /* inc %edi */
      0x49,                   /* dec %ecx */
      0x75, 0xf8,             /* jne loop */
      0x31, 0xc0,             /* xor %eax,%eax */
      0x5f,                   /* pop %edi */
      0xc3,                   /* ret */
      0x89, 0xf8,             /* mov %edi,%eax */
      0x5f,                   /* pop %edi */
      0xc3                    /* ret */
  };
  if (!test_emit(&p, &n, code, sizeof(code)))
    return false;
  if (out_len)
    *out_len = sizeof(code);
  return true;
}

static bool test_emit_memcmp_stub32(unsigned char *dst, size_t cap, size_t *out_len) {
  unsigned char *p = dst;
  size_t n = cap;
  const unsigned char code[] = {
      0x56,                   /* push %esi */
      0x57,                   /* push %edi */
      0x8b, 0x7c, 0x24, 0x0c, /* mov 12(%esp),%edi */
      0x8b, 0x74, 0x24, 0x10, /* mov 16(%esp),%esi */
      0x8b, 0x54, 0x24, 0x14, /* mov 20(%esp),%edx */
      0x85, 0xd2,             /* test %edx,%edx */
      0x74, 0x0f,             /* je equal */
      0x0f, 0xb6, 0x07,       /* movzbl (%edi),%eax */
      0x0f, 0xb6, 0x0e,       /* movzbl (%esi),%ecx */
      0x39, 0xc8,             /* cmp %ecx,%eax */
      0x75, 0x0a,             /* jne diff */
      0x47,                   /* inc %edi */
      0x46,                   /* inc %esi */
      0x4a,                   /* dec %edx */
      0x75, 0xf1,             /* jne loop */
      0x31, 0xc0,             /* xor %eax,%eax */
      0x5f,                   /* pop %edi */
      0x5e,                   /* pop %esi */
      0xc3,                   /* ret */
      0x29, 0xc8,             /* sub %ecx,%eax */
      0x5f,                   /* pop %edi */
      0x5e,                   /* pop %esi */
      0xc3                    /* ret */
  };
  if (!test_emit(&p, &n, code, sizeof(code)))
    return false;
  if (out_len)
    *out_len = sizeof(code);
  return true;
}

static bool test_emit_strlen_stub32(unsigned char *dst, size_t cap, size_t *out_len) {
  unsigned char *p = dst;
  size_t n = cap;
  const unsigned char code[] = {
      0x57,                   /* push %edi */
      0x8b, 0x7c, 0x24, 0x08, /* mov 8(%esp),%edi */
      0x31, 0xc0,             /* xor %eax,%eax */
      0x80, 0x3c, 0x07, 0x00, /* cmpb $0,(%edi,%eax) */
      0x74, 0x03,             /* je done */
      0x40,                   /* inc %eax */
      0xeb, 0xf7,             /* jmp loop */
      0x5f,                   /* pop %edi */
      0xc3                    /* ret */
  };
  if (!test_emit(&p, &n, code, sizeof(code)))
    return false;
  if (out_len)
    *out_len = sizeof(code);
  return true;
}

static bool test_emit_strcmp_stub32(unsigned char *dst, size_t cap, size_t *out_len) {
  unsigned char *p = dst;
  size_t n = cap;
  const unsigned char code[] = {
      0x56,                   /* push %esi */
      0x57,                   /* push %edi */
      0x8b, 0x7c, 0x24, 0x0c, /* mov 12(%esp),%edi */
      0x8b, 0x74, 0x24, 0x10, /* mov 16(%esp),%esi */
      0x0f, 0xb6, 0x07,       /* movzbl (%edi),%eax */
      0x0f, 0xb6, 0x0e,       /* movzbl (%esi),%ecx */
      0x39, 0xc8,             /* cmp %ecx,%eax */
      0x75, 0x0d,             /* jne diff */
      0x84, 0xc0,             /* test %al,%al */
      0x74, 0x04,             /* je equal */
      0x47,                   /* inc %edi */
      0x46,                   /* inc %esi */
      0xeb, 0xee,             /* jmp loop */
      0x31, 0xc0,             /* xor %eax,%eax */
      0x5f,                   /* pop %edi */
      0x5e,                   /* pop %esi */
      0xc3,                   /* ret */
      0x29, 0xc8,             /* sub %ecx,%eax */
      0x5f,                   /* pop %edi */
      0x5e,                   /* pop %esi */
      0xc3                    /* ret */
  };
  if (!test_emit(&p, &n, code, sizeof(code)))
    return false;
  if (out_len)
    *out_len = sizeof(code);
  return true;
}

static bool test_emit_strchr_stub32(unsigned char *dst, size_t cap, size_t *out_len) {
  unsigned char *p = dst;
  size_t n = cap;
  const unsigned char code[] = {
      0x57,                         /* push %edi */
      0x8b, 0x7c, 0x24, 0x08,       /* mov 8(%esp),%edi */
      0x0f, 0xb6, 0x54, 0x24, 0x0c, /* movzbl 12(%esp),%edx */
      0x0f, 0xb6, 0x07,             /* movzbl (%edi),%eax */
      0x38, 0xd0,                   /* cmp %dl,%al */
      0x74, 0x0b,                   /* je found */
      0x84, 0xc0,                   /* test %al,%al */
      0x74, 0x03,                   /* je not_found */
      0x47,                         /* inc %edi */
      0xeb, 0xf2,                   /* jmp loop */
      0x31, 0xc0,                   /* xor %eax,%eax */
      0x5f,                         /* pop %edi */
      0xc3,                         /* ret */
      0x89, 0xf8,                   /* mov %edi,%eax */
      0x5f,                         /* pop %edi */
      0xc3                          /* ret */
  };
  if (!test_emit(&p, &n, code, sizeof(code)))
    return false;
  if (out_len)
    *out_len = sizeof(code);
  return true;
}

static bool test_emit_realloc_stub32(unsigned char *dst, size_t cap, size_t *out_len) {
  unsigned char *p = dst;
  size_t n = cap;
  const unsigned char code[] = {
      0x8b, 0x4c, 0x24, 0x08,       /* mov 8(%esp),%ecx */
      0x85, 0xc9,                   /* test %ecx,%ecx */
      0x74, 0x6f,                   /* je zero */
      0x8b, 0x44, 0x24, 0x04,       /* mov 4(%esp),%eax */
      0x85, 0xc0,                   /* test %eax,%eax */
      0x75, 0x27,                   /* jne have_ptr */
      0x53,                         /* push %ebx */
      0x56,                         /* push %esi */
      0x57,                         /* push %edi */
      0x55,                         /* push %ebp */
      0x31, 0xdb,                   /* xor %ebx,%ebx */
      0x8b, 0x4c, 0x24, 0x18,       /* mov 24(%esp),%ecx */
      0xba, 0x03, 0x00, 0x00, 0x00, /* mov $3,%edx */
      0xbe, 0x22, 0x00, 0x00, 0x00, /* mov $0x22,%esi */
      0xbf, 0xff, 0xff, 0xff, 0xff, /* mov $-1,%edi */
      0x31, 0xed,                   /* xor %ebp,%ebp */
      0xb8, 0xc0, 0x00, 0x00, 0x00, /* mov $192,%eax (mmap2) */
      0xcd, 0x80,                   /* int $0x80 */
      0x5d,                         /* pop %ebp */
      0x5f,                         /* pop %edi */
      0x5e,                         /* pop %esi */
      0x5b,                         /* pop %ebx */
      0xc3,                         /* ret */
      0x53,                         /* push %ebx */
      0x56,                         /* push %esi */
      0x57,                         /* push %edi */
      0x55,                         /* push %ebp */
      0x8b, 0x74, 0x24, 0x14,       /* mov 20(%esp),%esi */
      0x8b, 0x4c, 0x24, 0x18,       /* mov 24(%esp),%ecx */
      0x51,                         /* push %ecx */
      0x56,                         /* push %esi */
      0x31, 0xdb,                   /* xor %ebx,%ebx */
      0xba, 0x03, 0x00, 0x00, 0x00, /* mov $3,%edx */
      0xbe, 0x22, 0x00, 0x00, 0x00, /* mov $0x22,%esi */
      0xbf, 0xff, 0xff, 0xff, 0xff, /* mov $-1,%edi */
      0x31, 0xed,                   /* xor %ebp,%ebp */
      0xb8, 0xc0, 0x00, 0x00, 0x00, /* mov $192,%eax (mmap2) */
      0xcd, 0x80,                   /* int $0x80 */
      0x5e,                         /* pop %esi */
      0x59,                         /* pop %ecx */
      0x89, 0xc7,                   /* mov %eax,%edi */
      0x50,                         /* push %eax */
      0x85, 0xc9,                   /* test %ecx,%ecx */
      0x74, 0x09,                   /* je done */
      0x8a, 0x16,                   /* mov (%esi),%dl */
      0x88, 0x17,                   /* mov %dl,(%edi) */
      0x46,                         /* inc %esi */
      0x47,                         /* inc %edi */
      0x49,                         /* dec %ecx */
      0x75, 0xf3,                   /* jne copy_loop */
      0x58,                         /* pop %eax */
      0x5d,                         /* pop %ebp */
      0x5f,                         /* pop %edi */
      0x5e,                         /* pop %esi */
      0x5b,                         /* pop %ebx */
      0xc3,                         /* ret */
      0x31, 0xc0,                   /* xor %eax,%eax */
      0xc3                          /* ret */
  };
  if (!test_emit(&p, &n, code, sizeof(code)))
    return false;
  if (out_len)
    *out_len = sizeof(code);
  return true;
}

static bool test_emit_calloc_stub32(unsigned char *dst, size_t cap, size_t *out_len) {
  unsigned char *p = dst;
  size_t n = cap;
  const unsigned char code[] = {
      0x8b, 0x4c, 0x24, 0x04,       /* mov 4(%esp),%ecx */
      0x0f, 0xaf, 0x4c, 0x24, 0x08, /* imul 8(%esp),%ecx */
      0x70, 0x23,                   /* jo unsupported */
      0x53,                         /* push %ebx */
      0x56,                         /* push %esi */
      0x57,                         /* push %edi */
      0x55,                         /* push %ebp */
      0x31, 0xdb,                   /* xor %ebx,%ebx */
      0xba, 0x03, 0x00, 0x00, 0x00, /* mov $3,%edx */
      0xbe, 0x22, 0x00, 0x00, 0x00, /* mov $0x22,%esi */
      0xbf, 0xff, 0xff, 0xff, 0xff, /* mov $-1,%edi */
      0x31, 0xed,                   /* xor %ebp,%ebp */
      0xb8, 0xc0, 0x00, 0x00, 0x00, /* mov $192,%eax (mmap2) */
      0xcd, 0x80,                   /* int $0x80 */
      0x5d,                         /* pop %ebp */
      0x5f,                         /* pop %edi */
      0x5e,                         /* pop %esi */
      0x5b,                         /* pop %ebx */
      0xc3,                         /* ret */
      0x31, 0xc0,                   /* xor %eax,%eax */
      0xc3                          /* ret */
  };
  if (!test_emit(&p, &n, code, sizeof(code)))
    return false;
  if (out_len)
    *out_len = sizeof(code);
  return true;
}

static bool test_emit_free_stub32(unsigned char *dst, size_t cap, size_t *out_len) {
  unsigned char *p = dst;
  size_t n = cap;
  const unsigned char code[] = {
      0x31, 0xc0, /* xor %eax,%eax */
      0xc3        /* ret */
  };
  if (!test_emit(&p, &n, code, sizeof(code)))
    return false;
  if (out_len)
    *out_len = sizeof(code);
  return true;
}

static size_t test_emit_harness(unsigned char *dst, size_t cap,
                                ny_test_link_ret_kind_t ret_kind,
                                const char *expected, uint64_t text_base,
                                uint64_t harness_off,
                                uint64_t rt_main_off) {
  unsigned char *p = dst;
  size_t n = cap;
  if (test_link_ret_is_f64(ret_kind)) {
    double d = strtod(expected, NULL);
    uint64_t bits = 0;
    memcpy(&bits, &d, sizeof(bits));
    if (!test_u8(&p, &n, 0xe8))
      return 0;
    size_t call_disp = (size_t)(p - dst);
    if (!test_u32le(&p, &n, 0))
      return 0;
    if (!test_emit(&p, &n, (const unsigned char[]){0x66, 0x48, 0x0f, 0x7e, 0xc0}, 5) ||
        !test_emit(&p, &n, (const unsigned char[]){0x48, 0xb9}, 2) ||
        !test_u64le(&p, &n, bits) ||
        !test_emit(&p, &n, (const unsigned char[]){0x48, 0x31, 0xff, 0x48, 0x39, 0xc8,
                                                   0x40, 0x0f, 0x95, 0xc7},
                   10) ||
        !test_emit(&p, &n, (const unsigned char[]){0xb8, 0x3c, 0x00, 0x00, 0x00,
                                                   0x0f, 0x05},
                   7))
      return 0;
    uint64_t call_site = text_base + harness_off + call_disp;
    int64_t rel = (int64_t)(text_base + rt_main_off) - (int64_t)(call_site + 4);
    memcpy(dst + call_disp, &(int32_t){(int32_t)rel}, 4);
  } else if (test_link_ret_is_f32(ret_kind)) {
    float f = strtof(expected, NULL);
    uint32_t bits = 0;
    memcpy(&bits, &f, sizeof(bits));
    if (!test_u8(&p, &n, 0xe8))
      return 0;
    size_t call_disp = (size_t)(p - dst);
    if (!test_u32le(&p, &n, 0))
      return 0;
    if (!test_emit(&p, &n, (const unsigned char[]){0x66, 0x0f, 0x7e, 0xc0,
                                                   0x3d},
                   5) ||
        !test_u32le(&p, &n, bits) ||
        !test_emit(&p, &n, (const unsigned char[]){0x48, 0x31, 0xff,
                                                   0x40, 0x0f, 0x95, 0xc7,
                                                   0xb8, 0x3c, 0x00, 0x00, 0x00,
                                                   0x0f, 0x05},
                   14))
      return 0;
    uint64_t call_site = text_base + harness_off + call_disp;
    int64_t rel = (int64_t)(text_base + rt_main_off) - (int64_t)(call_site + 4);
    memcpy(dst + call_disp, &(int32_t){(int32_t)rel}, 4);
  } else {
    uint64_t val = (uint64_t)strtoull(expected, NULL, 0);
    unsigned bits = test_link_ret_bits(ret_kind);
    if (!test_u8(&p, &n, 0xe8))
      return 0;
    size_t call_disp = (size_t)(p - dst);
    if (!test_u32le(&p, &n, 0))
      return 0;
    if (bits == 64) {
      if (!test_emit(&p, &n, (const unsigned char[]){0x48, 0xb9}, 2) ||
          !test_u64le(&p, &n, val) ||
          !test_emit(&p, &n, (const unsigned char[]){0x48, 0x31, 0xff, 0x48, 0x39, 0xc8,
                                                     0x40, 0x0f, 0x95, 0xc7},
                     10))
        return 0;
    } else if (bits == 32) {
      if (!test_emit(&p, &n, (const unsigned char[]){0x3d}, 1) ||
          !test_u32le(&p, &n, (uint32_t)val) ||
          !test_emit(&p, &n, (const unsigned char[]){0x40, 0x0f, 0x95, 0xc7}, 4))
        return 0;
    } else if (bits == 16) {
      if (!test_emit(&p, &n, (const unsigned char[]){0x66, 0x3d}, 2) ||
          !test_emit(&p, &n, (const unsigned char[]){(unsigned char)val,
                                                     (unsigned char)(val >> 8)}, 2) ||
          !test_emit(&p, &n, (const unsigned char[]){0x40, 0x0f, 0x95, 0xc7}, 4))
        return 0;
    } else {
      if (!test_emit(&p, &n, (const unsigned char[]){0x3c, (unsigned char)val,
                                                     0x40, 0x0f, 0x95, 0xc7}, 6))
        return 0;
    }
    if (!test_emit(&p, &n, (const unsigned char[]){0xb8, 0x3c, 0x00, 0x00, 0x00,
                                                   0x0f, 0x05},
                   7))
      return 0;
    uint64_t call_site = text_base + harness_off + call_disp;
    int64_t rel = (int64_t)(text_base + rt_main_off) - (int64_t)(call_site + 4);
    memcpy(dst + call_disp, &(int32_t){(int32_t)rel}, 4);
  }
  return (size_t)(p - dst);
}

static size_t test_emit_harness32(unsigned char *dst, size_t cap,
                                  ny_test_link_ret_kind_t ret_kind,
                                  const char *expected, uint32_t text_base,
                                  uint32_t harness_off,
                                  uint32_t rt_main_off) {
  unsigned char *p = dst;
  size_t n = cap;
  if (!test_u8(&p, &n, 0xe8))
    return 0;
  size_t call_disp = (size_t)(p - dst);
  if (!test_u32le(&p, &n, 0))
    return 0;
  if (test_link_ret_is_f64(ret_kind)) {
    double d = strtod(expected, NULL);
    uint64_t bits = 0;
    memcpy(&bits, &d, sizeof(bits));
    if (!test_emit(&p, &n, (const unsigned char[]){0x83, 0xec, 0x08,
                                                   0xdd, 0x1c, 0x24,
                                                   0x8b, 0x04, 0x24,
                                                   0x3d},
                   10) ||
        !test_u32le(&p, &n, (uint32_t)bits) ||
        !test_emit(&p, &n, (const unsigned char[]){0x0f, 0x85}, 2))
      return 0;
    size_t jne_low_disp = (size_t)(p - dst);
    if (!test_u32le(&p, &n, 0) ||
        !test_emit(&p, &n, (const unsigned char[]){0x8b, 0x44, 0x24, 0x04,
                                                   0x3d},
                   5) ||
        !test_u32le(&p, &n, (uint32_t)(bits >> 32)) ||
        !test_emit(&p, &n, (const unsigned char[]){0x0f, 0x85}, 2))
      return 0;
    size_t jne_high_disp = (size_t)(p - dst);
    if (!test_u32le(&p, &n, 0) ||
        !test_emit(&p, &n, (const unsigned char[]){0x83, 0xc4, 0x08,
                                                   0x31, 0xdb,
                                                   0xb8, 0x01, 0x00, 0x00, 0x00,
                                                   0xcd, 0x80},
                   12))
      return 0;
    size_t fail_off = (size_t)(p - dst);
    if (!test_emit(&p, &n, (const unsigned char[]){0x83, 0xc4, 0x08,
                                                   0xbb, 0x01, 0x00, 0x00, 0x00,
                                                   0xb8, 0x01, 0x00, 0x00, 0x00,
                                                   0xcd, 0x80},
                   16))
      return 0;
    int32_t low_rel = (int32_t)((int64_t)fail_off - (int64_t)(jne_low_disp + 4));
    int32_t high_rel = (int32_t)((int64_t)fail_off - (int64_t)(jne_high_disp + 4));
    memcpy(dst + jne_low_disp, &low_rel, 4);
    memcpy(dst + jne_high_disp, &high_rel, 4);
  } else if (test_link_ret_is_f32(ret_kind)) {
    float f = strtof(expected, NULL);
    uint32_t bits = 0;
    memcpy(&bits, &f, sizeof(bits));
    if (!test_emit(&p, &n, (const unsigned char[]){0x83, 0xec, 0x04,
                                                   0xd9, 0x1c, 0x24,
                                                   0x8b, 0x04, 0x24,
                                                   0x83, 0xc4, 0x04,
                                                   0x3d},
                   13) ||
        !test_u32le(&p, &n, bits) ||
        !test_emit(&p, &n, (const unsigned char[]){0x0f, 0x95, 0xc3,
                                                  0x0f, 0xb6, 0xdb,
                                                  0xb8, 0x01, 0x00, 0x00, 0x00,
                                                  0xcd, 0x80},
                  13))
      return 0;
  } else {
    uint32_t val = (uint32_t)strtoul(expected, NULL, 0);
    unsigned bits = test_link_ret_bits(ret_kind);
    if (!test_emit(&p, &n, (const unsigned char[]){0x3d}, 1) ||
        !test_u32le(&p, &n, val))
      return 0;
    if (bits == 16) {
      p -= 5;
      n += 5;
      if (!test_emit(&p, &n, (const unsigned char[]){0x66, 0x3d,
                                                     (unsigned char)val,
                                                     (unsigned char)(val >> 8)}, 4))
        return 0;
    } else if (bits == 8) {
      p -= 5;
      n += 5;
      if (!test_emit(&p, &n, (const unsigned char[]){0x3c, (unsigned char)val}, 2))
        return 0;
    }
    if (!test_emit(&p, &n, (const unsigned char[]){0x0f, 0x95, 0xc3,
                                                  0x0f, 0xb6, 0xdb,
                                                  0xb8, 0x01, 0x00, 0x00, 0x00,
                                                  0x89, 0xd9,
                                                  0xcd, 0x80},
                  15))
     return 0;
  }
  uint32_t call_site = text_base + harness_off + (uint32_t)call_disp;
  int32_t rel = (int32_t)((int64_t)(text_base + rt_main_off) -
                          (int64_t)(call_site + 4));
  memcpy(dst + call_disp, &rel, 4);
  return (size_t)(p - dst);
}

static bool test_ar_read_symtab(const unsigned char *data, size_t size, ny_test_ar_symtab_t *out) {
  if (!data || size < 8 || memcmp(data, "!<arch>\n", 8) != 0)
    return false;
  size_t off = 8;
  uint32_t first_sym_off = 0;
  while (off + sizeof(ny_test_ar_hdr_t) <= size) {
    ny_test_ar_hdr_t hdr;
    memcpy(&hdr, data + off, sizeof(hdr));
    if (hdr.ar_fmag[0] != 0x60 || hdr.ar_fmag[1] != 0x0a)
      return false;
    char sz_buf[11];
    memcpy(sz_buf, hdr.ar_size, 10); sz_buf[10] = '\0';
    long raw_size = atol(sz_buf);
    if (raw_size < 0)
      return false;
    size_t member_size = (size_t)raw_size;
    size_t data_off = off + sizeof(ny_test_ar_hdr_t);
    if (data_off + member_size > size)
      return false;
    if (hdr.ar_name[0] == '/' && hdr.ar_name[1] == ' ' && !first_sym_off) {
      first_sym_off = (uint32_t)off;
    }
    size_t padded = member_size + (member_size & 1);
    off = data_off + padded;
  }
  if (!first_sym_off)
    return false;
  ny_test_ar_hdr_t sh;
  memcpy(&sh, data + first_sym_off, sizeof(sh));
  char ssz[11];
  memcpy(ssz, sh.ar_size, 10); ssz[10] = '\0';
  long sym_size = atol(ssz);
  if (sym_size < 4)
    return false;
  unsigned char *sym_data = (unsigned char *)(data + first_sym_off + sizeof(ny_test_ar_hdr_t));
  uint32_t count = 0;
  memcpy(&count, sym_data, 4);
  count = __builtin_bswap32(count);
  if ((uint32_t)sym_size < 4 + count * 4)
    return false;
  uint32_t *offsets = (uint32_t *)malloc(count * sizeof(uint32_t));
  if (!offsets)
    return false;
  for (uint32_t i = 0; i < count; ++i)
    offsets[i] = __builtin_bswap32(*(uint32_t *)(sym_data + 4 + i * 4));
  char *names = (char *)malloc((size_t)sym_size - 4 - count * 4);
  if (!names) {
    free(offsets);
    return false;
  }
  size_t names_len = (size_t)sym_size - 4 - count * 4;
  memcpy(names, sym_data + 4 + count * 4, names_len);
  out->offsets = offsets;
  out->names = names;
  out->count = count;
  return true;
}

static void test_ar_free_symtab(ny_test_ar_symtab_t *st) {
  if (st) {
    free(st->offsets);
    free(st->names);
    st->offsets = NULL;
    st->names = NULL;
    st->count = 0;
  }
}

static uint32_t test_ar_find_symbol(const ny_test_ar_symtab_t *st, const char *name) {
  if (!st || !name)
    return 0;
  const char *p = st->names;
  for (uint32_t i = 0; i < st->count; ++i) {
    size_t len = strlen(p);
    if (strcmp(p, name) == 0)
      return st->offsets[i];
    p += len + 1;
  }
  return 0;
}

static unsigned char *test_ar_extract_member(const unsigned char *data, size_t size,
                                              uint32_t member_off, size_t *out_size) {
  if (!data || member_off + sizeof(ny_test_ar_hdr_t) > size) {
    if (out_size) *out_size = 0;
    return NULL;
  }
  ny_test_ar_hdr_t hdr;
  memcpy(&hdr, data + member_off, sizeof(hdr));
  if (hdr.ar_fmag[0] != 0x60 || hdr.ar_fmag[1] != 0x0a) {
    if (out_size) *out_size = 0;
    return NULL;
  }
  char sz_buf[11];
  memcpy(sz_buf, hdr.ar_size, 10); sz_buf[10] = '\0';
  long raw_size = atol(sz_buf);
  if (raw_size < 0) {
    if (out_size) *out_size = 0;
    return NULL;
  }
  size_t member_size = (size_t)raw_size;
  size_t data_off = member_off + sizeof(ny_test_ar_hdr_t);
  if (data_off + member_size > size) {
    if (out_size) *out_size = 0;
    return NULL;
  }
  unsigned char *buf = (unsigned char *)malloc(member_size);
  if (!buf) {
    if (out_size) *out_size = 0;
    return NULL;
  }
  memcpy(buf, data + data_off, member_size);
  if (out_size) *out_size = member_size;
  return buf;
}


static bool test_link_extract_elf64_archive_member(const unsigned char *archive_data,
                                                   size_t archive_size,
                                                   uint32_t member_off,
                                                   unsigned char **text_io,
                                                   size_t *text_cap_io,
                                                   size_t *linked_text_len_io,
                                                   ny_test_link_sym_t *defs,
                                                   size_t *def_count_io) {
  size_t member_size = 0;
  unsigned char *member = test_ar_extract_member(archive_data, archive_size, member_off, &member_size);
  if (!member || member_size == 0) {
    free(member);
    return false;
  }
  bool ok = false;
  ny_test_elf64_ehdr_t meh;
  if (member_size < sizeof(meh))
    goto done;
  memcpy(&meh, member, sizeof(meh));
  if (meh.e_ident[0] != 0x7f || meh.e_ident[1] != 'E' || meh.e_ident[2] != 'L' ||
      meh.e_ident[3] != 'F' || meh.e_ident[4] != 2 || meh.e_ident[5] != 1 ||
      meh.e_type != 1 || meh.e_machine != 62 ||
      meh.e_shentsize != sizeof(ny_test_elf64_shdr_t) ||
      meh.e_shoff + (uint64_t)meh.e_shnum * sizeof(ny_test_elf64_shdr_t) > member_size)
    goto done;
  ny_test_elf64_shdr_t *msh = (ny_test_elf64_shdr_t *)(void *)(member + meh.e_shoff);
  if (meh.e_shstrndx >= meh.e_shnum ||
      msh[meh.e_shstrndx].sh_offset + msh[meh.e_shstrndx].sh_size > member_size)
    goto done;
  const char *mshstr = (const char *)(member + msh[meh.e_shstrndx].sh_offset);
  int mtext_i = -1, mrodata_i = -1, mdata_i = -1, msym_i = -1, mstr_i = -1;
  for (int mi = 0; mi < meh.e_shnum; ++mi) {
    const char *mn = msh[mi].sh_name < msh[meh.e_shstrndx].sh_size ? mshstr + msh[mi].sh_name : "";
    if (strcmp(mn, ".text") == 0) mtext_i = mi;
    else if (strcmp(mn, ".rodata") == 0) mrodata_i = mi;
    else if (strcmp(mn, ".data") == 0) mdata_i = mi;
    else if (strcmp(mn, ".symtab") == 0) msym_i = mi;
    else if (strcmp(mn, ".strtab") == 0) mstr_i = mi;
  }
  if (mtext_i < 0 || msym_i < 0 || mstr_i < 0 ||
      msh[mtext_i].sh_offset + msh[mtext_i].sh_size > member_size ||
      msh[msym_i].sh_offset + msh[msym_i].sh_size > member_size ||
      msh[mstr_i].sh_offset + msh[mstr_i].sh_size > member_size ||
      (mrodata_i >= 0 && msh[mrodata_i].sh_offset + msh[mrodata_i].sh_size > member_size) ||
      (mdata_i >= 0 && msh[mdata_i].sh_offset + msh[mdata_i].sh_size > member_size))
    goto done;

  size_t append_off = *linked_text_len_io;
  size_t mtext_len = (size_t)msh[mtext_i].sh_size;
  size_t mrodata_len = mrodata_i >= 0 ? (size_t)msh[mrodata_i].sh_size : 0;
  size_t mdata_len = mdata_i >= 0 ? (size_t)msh[mdata_i].sh_size : 0;
  size_t rodata_off = append_off + mtext_len;
  size_t data_off = rodata_off + mrodata_len;
  size_t total_needed = data_off + mdata_len;
  if (total_needed > *text_cap_io) {
    size_t new_cap = *text_cap_io ? *text_cap_io : 4096;
    while (new_cap < total_needed + 4096) new_cap *= 2;
    unsigned char *nt = (unsigned char *)realloc(*text_io, new_cap);
    if (!nt)
      goto done;
    *text_io = nt;
    *text_cap_io = new_cap;
  }
  memcpy(*text_io + append_off, member + msh[mtext_i].sh_offset, mtext_len);
  if (mrodata_i >= 0)
    memcpy(*text_io + rodata_off, member + msh[mrodata_i].sh_offset, mrodata_len);
  if (mdata_i >= 0)
    memcpy(*text_io + data_off, member + msh[mdata_i].sh_offset, mdata_len);

  ny_test_elf64_sym_t *msym = (ny_test_elf64_sym_t *)(void *)(member + msh[msym_i].sh_offset);
  size_t msym_count = msh[msym_i].sh_entsize ? (size_t)(msh[msym_i].sh_size / msh[msym_i].sh_entsize) : 0;
  const char *mstrtab = (const char *)(member + msh[mstr_i].sh_offset);
  size_t mstrtab_len = (size_t)msh[mstr_i].sh_size;
  for (size_t mj = 0; mj < msym_count && mj < 4096; ++mj) {
    if (msym[mj].st_name >= mstrtab_len) continue;
    const char *mn = mstrtab + msym[mj].st_name;
    if (!mn[0]) continue;
    uint16_t sym_sec = (uint16_t)msym[mj].st_shndx;
    size_t sym_base_off;
    if (sym_sec == (uint16_t)mtext_i) sym_base_off = append_off;
    else if (mrodata_i >= 0 && sym_sec == (uint16_t)mrodata_i) sym_base_off = rodata_off;
    else if (mdata_i >= 0 && sym_sec == (uint16_t)mdata_i) sym_base_off = data_off;
    else continue;
    if (*def_count_io >= 256)
      goto done;
    if (test_link_sym_index(defs, *def_count_io, mn) < 0) {
      snprintf(defs[*def_count_io].name, sizeof(defs[*def_count_io].name), "%s", mn);
      defs[*def_count_io].off = (uint64_t)(sym_base_off + msym[mj].st_value);
      defs[*def_count_io].shndx = sym_sec == (uint16_t)mtext_i ? 1 : 2;
      defs[*def_count_io].defined = true;
      (*def_count_io)++;
    }
  }
  *linked_text_len_io = append_off + ((total_needed - append_off + 15u) & ~15u);
  ok = true;
done:
  free(member);
  return ok;
}

static bool test_link_extract_elf32_archive_member(const unsigned char *archive_data,
                                                   size_t archive_size,
                                                   uint32_t member_off,
                                                   unsigned char **text_io,
                                                   size_t *text_cap_io,
                                                   size_t *linked_text_len_io,
                                                   ny_test_link_sym_t *defs,
                                                   size_t *def_count_io) {
  size_t member_size = 0;
  unsigned char *member = test_ar_extract_member(archive_data, archive_size, member_off, &member_size);
  if (!member || member_size == 0) {
    free(member);
    return false;
  }
  bool ok = false;
  ny_test_elf32_ehdr_t meh;
  if (member_size < sizeof(meh))
    goto done;
  memcpy(&meh, member, sizeof(meh));
  if (meh.e_ident[0] != 0x7f || meh.e_ident[1] != 'E' || meh.e_ident[2] != 'L' ||
      meh.e_ident[3] != 'F' || meh.e_ident[4] != 1 || meh.e_ident[5] != 1 ||
      meh.e_type != 1 || meh.e_machine != 3 ||
      meh.e_shentsize != sizeof(ny_test_elf32_shdr_t) ||
      meh.e_shoff + (uint64_t)meh.e_shnum * sizeof(ny_test_elf32_shdr_t) > member_size)
    goto done;
  ny_test_elf32_shdr_t *msh = (ny_test_elf32_shdr_t *)(void *)(member + meh.e_shoff);
  if (meh.e_shstrndx >= meh.e_shnum ||
      msh[meh.e_shstrndx].sh_offset + msh[meh.e_shstrndx].sh_size > member_size)
    goto done;
  const char *mshstr = (const char *)(member + msh[meh.e_shstrndx].sh_offset);
  int mtext_i = -1, mrodata_i = -1, mdata_i = -1, msym_i = -1, mstr_i = -1;
  for (int mi = 0; mi < meh.e_shnum; ++mi) {
    const char *mn = msh[mi].sh_name < msh[meh.e_shstrndx].sh_size ? mshstr + msh[mi].sh_name : "";
    if (strcmp(mn, ".text") == 0) mtext_i = mi;
    else if (strcmp(mn, ".rodata") == 0) mrodata_i = mi;
    else if (strcmp(mn, ".data") == 0) mdata_i = mi;
    else if (strcmp(mn, ".symtab") == 0) msym_i = mi;
    else if (strcmp(mn, ".strtab") == 0) mstr_i = mi;
  }
  if (mtext_i < 0 || msym_i < 0 || mstr_i < 0 ||
      msh[mtext_i].sh_offset + msh[mtext_i].sh_size > member_size ||
      msh[msym_i].sh_offset + msh[msym_i].sh_size > member_size ||
      msh[mstr_i].sh_offset + msh[mstr_i].sh_size > member_size ||
      (mrodata_i >= 0 && msh[mrodata_i].sh_offset + msh[mrodata_i].sh_size > member_size) ||
      (mdata_i >= 0 && msh[mdata_i].sh_offset + msh[mdata_i].sh_size > member_size))
    goto done;

  size_t append_off = *linked_text_len_io;
  size_t mtext_len = (size_t)msh[mtext_i].sh_size;
  size_t mrodata_len = mrodata_i >= 0 ? (size_t)msh[mrodata_i].sh_size : 0;
  size_t mdata_len = mdata_i >= 0 ? (size_t)msh[mdata_i].sh_size : 0;
  size_t rodata_off = append_off + mtext_len;
  size_t data_off = rodata_off + mrodata_len;
  size_t total_needed = data_off + mdata_len;
  if (total_needed > *text_cap_io) {
    size_t new_cap = *text_cap_io ? *text_cap_io : 4096;
    while (new_cap < total_needed + 4096) new_cap *= 2;
    unsigned char *nt = (unsigned char *)realloc(*text_io, new_cap);
    if (!nt)
      goto done;
    *text_io = nt;
    *text_cap_io = new_cap;
  }
  memcpy(*text_io + append_off, member + msh[mtext_i].sh_offset, mtext_len);
  if (mrodata_i >= 0)
    memcpy(*text_io + rodata_off, member + msh[mrodata_i].sh_offset, mrodata_len);
  if (mdata_i >= 0)
    memcpy(*text_io + data_off, member + msh[mdata_i].sh_offset, mdata_len);

  ny_test_elf32_sym_t *msym = (ny_test_elf32_sym_t *)(void *)(member + msh[msym_i].sh_offset);
  size_t msym_count = msh[msym_i].sh_entsize ? (size_t)(msh[msym_i].sh_size / msh[msym_i].sh_entsize) : 0;
  const char *mstrtab = (const char *)(member + msh[mstr_i].sh_offset);
  size_t mstrtab_len = (size_t)msh[mstr_i].sh_size;
  for (size_t mj = 0; mj < msym_count && mj < 4096; ++mj) {
    if (msym[mj].st_name >= mstrtab_len) continue;
    const char *mn = mstrtab + msym[mj].st_name;
    if (!mn[0]) continue;
    uint16_t sym_sec = (uint16_t)msym[mj].st_shndx;
    size_t sym_base_off;
    if (sym_sec == (uint16_t)mtext_i) sym_base_off = append_off;
    else if (mrodata_i >= 0 && sym_sec == (uint16_t)mrodata_i) sym_base_off = rodata_off;
    else if (mdata_i >= 0 && sym_sec == (uint16_t)mdata_i) sym_base_off = data_off;
    else continue;
    if (*def_count_io >= 256)
      goto done;
    if (test_link_sym_index(defs, *def_count_io, mn) < 0) {
      snprintf(defs[*def_count_io].name, sizeof(defs[*def_count_io].name), "%s", mn);
      defs[*def_count_io].off = (uint64_t)(sym_base_off + msym[mj].st_value);
      defs[*def_count_io].shndx = 1;
      defs[*def_count_io].defined = true;
      (*def_count_io)++;
    }
  }
  *linked_text_len_io = append_off + ((total_needed - append_off + 15u) & ~15u);
  ok = true;
done:
  free(member);
  return ok;
}

static bool test_link_archive_offset_seen(const uint32_t *offsets, size_t count, uint32_t member_off) {
  for (size_t i = 0; i < count; ++i)
    if (offsets[i] == member_off)
      return true;
  return false;
}

static int test_internal_elf32_link_run(const char *obj_path,
                                        ny_test_link_ret_kind_t ret_kind,
                                        const char *expected, const char *shape_path,
                                        const char *archive_path) {
#ifndef __linux__
  (void)obj_path; (void)ret_kind; (void)expected; (void)shape_path; (void)archive_path;
  return 2;
#else
  ny_test_ar_symtab_t ar_st;
  memset(&ar_st, 0, sizeof(ar_st));
  unsigned char *archive_data = NULL;
  size_t archive_size = 0;
  if (archive_path && *archive_path) {
    struct stat ar_stbuf;
    if (stat(archive_path, &ar_stbuf) != 0 || ar_stbuf.st_size <= 0 ||
        ar_stbuf.st_size > 32 * 1024 * 1024)
      return 2;
    archive_size = (size_t)ar_stbuf.st_size;
    archive_data = (unsigned char *)malloc(archive_size);
    if (!archive_data) return 2;
    FILE *arf = fopen(archive_path, "rb");
    if (!arf) { free(archive_data); return 2; }
    bool arok = fread(archive_data, 1, archive_size, arf) == archive_size;
    fclose(arf);
    if (!arok || !test_ar_read_symtab(archive_data, archive_size, &ar_st)) {
      free(archive_data); archive_data = NULL; ar_st.count = 0;
    }
  }

  struct stat st;
  if (stat(obj_path, &st) != 0 || st.st_size <= 0 || st.st_size > 16 * 1024 * 1024)
    { free(archive_data); test_ar_free_symtab(&ar_st); return 2; }
  FILE *f = fopen(obj_path, "rb");
  if (!f)
    { free(archive_data); test_ar_free_symtab(&ar_st); return 2; }
  unsigned char *obj = (unsigned char *)malloc((size_t)st.st_size);
  if (!obj) {
    fclose(f); free(archive_data); test_ar_free_symtab(&ar_st);
    return 2;
  }
  bool ok = fread(obj, 1, (size_t)st.st_size, f) == (size_t)st.st_size;
  fclose(f);
  if (!ok || (size_t)st.st_size < sizeof(ny_test_elf32_ehdr_t)) {
    free(obj); free(archive_data); test_ar_free_symtab(&ar_st);
    return 2;
  }
  ny_test_elf32_ehdr_t eh;
  memcpy(&eh, obj, sizeof(eh));
  if (eh.e_ident[0] != 0x7f || eh.e_ident[1] != 'E' || eh.e_ident[2] != 'L' ||
      eh.e_ident[3] != 'F' || eh.e_ident[4] != 1 || eh.e_ident[5] != 1 ||
      eh.e_type != 1 || eh.e_machine != 3 ||
      eh.e_shentsize != sizeof(ny_test_elf32_shdr_t) ||
      (uint64_t)eh.e_shoff + (uint64_t)eh.e_shnum * sizeof(ny_test_elf32_shdr_t) >
          (uint64_t)st.st_size) {
    free(obj);
    return 2;
  }
  ny_test_elf32_shdr_t *sh = (ny_test_elf32_shdr_t *)(void *)(obj + eh.e_shoff);
  if (eh.e_shstrndx >= eh.e_shnum ||
      (uint64_t)sh[eh.e_shstrndx].sh_offset + sh[eh.e_shstrndx].sh_size >
          (uint64_t)st.st_size) {
    free(obj);
    return 2;
  }
  const char *shstr = (const char *)(obj + sh[eh.e_shstrndx].sh_offset);
  int text_i = -1, sym_i = -1, str_i = -1, rel_i = -1;
  for (int i = 0; i < eh.e_shnum; ++i) {
    const char *name = sh[i].sh_name < sh[eh.e_shstrndx].sh_size ? shstr + sh[i].sh_name : "";
    if (strcmp(name, ".text") == 0) text_i = i;
    else if (strcmp(name, ".symtab") == 0) sym_i = i;
    else if (strcmp(name, ".strtab") == 0) str_i = i;
    else if (strcmp(name, ".rel.text") == 0) rel_i = i;
  }
  if (text_i < 0 || sym_i < 0 || str_i < 0 ||
      (uint64_t)sh[text_i].sh_offset + sh[text_i].sh_size > (uint64_t)st.st_size ||
      (uint64_t)sh[sym_i].sh_offset + sh[sym_i].sh_size > (uint64_t)st.st_size ||
      (uint64_t)sh[str_i].sh_offset + sh[str_i].sh_size > (uint64_t)st.st_size) {
    free(obj);
    return 2;
  }
  size_t text_len = sh[text_i].sh_size;
  size_t text_cap = text_len + 4096;
  unsigned char *text = (unsigned char *)malloc(text_cap);
  if (!text) {
    free(obj); free(archive_data); test_ar_free_symtab(&ar_st);
    return 2;
  }
  memcpy(text, obj + sh[text_i].sh_offset, text_len);
  ny_test_elf32_sym_t *sym = (ny_test_elf32_sym_t *)(void *)(obj + sh[sym_i].sh_offset);
  size_t sym_count = sh[sym_i].sh_entsize ? (size_t)(sh[sym_i].sh_size / sh[sym_i].sh_entsize) : 0;
  const char *strtab = (const char *)(obj + sh[str_i].sh_offset);
  size_t strtab_len = sh[str_i].sh_size;
  ny_test_link_sym_t defs[256];
  size_t def_count = 0;
  uint32_t rt_main_off = 0;
  bool have_rt_main = false;
  for (size_t i = 0; i < sym_count && i < 4096; ++i) {
    if (sym[i].st_name >= strtab_len)
      continue;
    const char *name = strtab + sym[i].st_name;
    if (!name[0])
      continue;
    if (sym[i].st_shndx == (uint16_t)text_i) {
      if (def_count >= sizeof(defs) / sizeof(defs[0]))
        { free(text); free(obj); free(archive_data); test_ar_free_symtab(&ar_st); return 2; }
      snprintf(defs[def_count].name, sizeof(defs[def_count].name), "%s", name);
      defs[def_count].off = sym[i].st_value;
      defs[def_count].shndx = sym[i].st_shndx;
      defs[def_count].defined = true;
      if (strcmp(name, "rt_main") == 0) {
        rt_main_off = sym[i].st_value;
        have_rt_main = true;
      }
      def_count++;
    }
  }
  if (!have_rt_main)
    { free(text); free(obj); free(archive_data); test_ar_free_symtab(&ar_st); return 2; }
  const uint32_t base = 0x08048000u + 0x1000u;
  size_t linked_text_len = text_len;
#define ELF32_AR_FAIL do { free(text); free(obj); free(archive_data); test_ar_free_symtab(&ar_st); return 2; } while(0)
  if (rel_i >= 0) {
    if ((uint64_t)sh[rel_i].sh_offset + sh[rel_i].sh_size > (uint64_t)st.st_size)
      ELF32_AR_FAIL;
    ny_test_elf32_rel_t *rel = (ny_test_elf32_rel_t *)(void *)(obj + sh[rel_i].sh_offset);
    size_t rel_count = sh[rel_i].sh_entsize ? (size_t)(sh[rel_i].sh_size / sh[rel_i].sh_entsize) : 0;
    for (size_t i = 0; i < rel_count; ++i) {
      uint32_t type = rel[i].r_info & 0xffu;
      uint32_t si = rel[i].r_info >> 8;
      if (type != 2 || si >= sym_count || rel[i].r_offset + 4 > text_len ||
          sym[si].st_name >= strtab_len)
        ELF32_AR_FAIL;
      const char *name = strtab + sym[si].st_name;
      if (test_link_sym_index(defs, def_count, name) >= 0)
        continue;
      bool resolved = false;
      if (archive_data && ar_st.count > 0) {
        uint32_t member_off = test_ar_find_symbol(&ar_st, name);
        if (member_off) {
          size_t member_size = 0;
          unsigned char *member = test_ar_extract_member(archive_data, archive_size,
                                                         member_off, &member_size);
          if (member && member_size > 0) {
            ny_test_elf32_ehdr_t meh;
            if (member_size >= sizeof(meh)) {
              memcpy(&meh, member, sizeof(meh));
              if (meh.e_ident[0] == 0x7f && meh.e_ident[1] == 'E' && meh.e_ident[2] == 'L' &&
                  meh.e_ident[3] == 'F' && meh.e_ident[4] == 1 && meh.e_ident[5] == 1 &&
                  meh.e_type == 1 && meh.e_machine == 3 &&
                  meh.e_shentsize == sizeof(ny_test_elf32_shdr_t) &&
                  meh.e_shoff + (uint64_t)meh.e_shnum * sizeof(ny_test_elf32_shdr_t) <= member_size) {
                ny_test_elf32_shdr_t *msh = (ny_test_elf32_shdr_t *)(void *)(member + meh.e_shoff);
                if (meh.e_shstrndx < meh.e_shnum &&
                    msh[meh.e_shstrndx].sh_offset + msh[meh.e_shstrndx].sh_size <= member_size) {
                  const char *mshstr = (const char *)(member + msh[meh.e_shstrndx].sh_offset);
                  int mtext_i = -1, mrodata_i = -1, mdata_i = -1, msym_i = -1, mstr_i = -1, mrel_i = -1;
                  for (int mi = 0; mi < meh.e_shnum; ++mi) {
                    const char *mn = msh[mi].sh_name < msh[meh.e_shstrndx].sh_size ? mshstr + msh[mi].sh_name : "";
                    if (strcmp(mn, ".text") == 0) mtext_i = mi;
                    else if (strcmp(mn, ".rodata") == 0) mrodata_i = mi;
                    else if (strcmp(mn, ".data") == 0) mdata_i = mi;
                    else if (strcmp(mn, ".symtab") == 0) msym_i = mi;
                    else if (strcmp(mn, ".strtab") == 0) mstr_i = mi;
                    else if (strcmp(mn, ".rel.text") == 0) mrel_i = mi;
                  }
                  if (mtext_i >= 0 && msym_i >= 0 && mstr_i >= 0 &&
                      msh[mtext_i].sh_size <= member_size &&
                      msh[msym_i].sh_offset + msh[msym_i].sh_size <= member_size &&
                      msh[mstr_i].sh_offset + msh[mstr_i].sh_size <= member_size &&
                      (mrodata_i < 0 || msh[mrodata_i].sh_offset + msh[mrodata_i].sh_size <= member_size) &&
                      (mdata_i < 0 || msh[mdata_i].sh_offset + msh[mdata_i].sh_size <= member_size)) {
                    size_t append_off = linked_text_len;
                    size_t mtext_len = (size_t)msh[mtext_i].sh_size;
                    size_t mrodata_len = mrodata_i >= 0 ? (size_t)msh[mrodata_i].sh_size : 0;
                    size_t mdata_len = mdata_i >= 0 ? (size_t)msh[mdata_i].sh_size : 0;
                    size_t rodata_off = append_off + mtext_len;
                    size_t data_off = rodata_off + mrodata_len;
                    size_t total_needed = data_off + mdata_len;
                    if (total_needed > text_cap) {
                      size_t new_cap = text_cap;
                      while (new_cap < total_needed + 4096) new_cap *= 2;
                      unsigned char *nt = (unsigned char *)realloc(text, new_cap);
                      if (!nt) { free(member); ELF32_AR_FAIL; }
                      text = nt; text_cap = new_cap;
                    }
                    memcpy(text + append_off, member + msh[mtext_i].sh_offset, mtext_len);
                    if (mrodata_i >= 0)
                      memcpy(text + rodata_off, member + msh[mrodata_i].sh_offset, mrodata_len);
                    if (mdata_i >= 0)
                      memcpy(text + data_off, member + msh[mdata_i].sh_offset, mdata_len);
                    ny_test_elf32_sym_t *msym = (ny_test_elf32_sym_t *)(void *)(member + msh[msym_i].sh_offset);
                    size_t msym_count = msh[msym_i].sh_entsize ? (size_t)(msh[msym_i].sh_size / msh[msym_i].sh_entsize) : 0;
                    const char *mstrtab = (const char *)(member + msh[mstr_i].sh_offset);
                    size_t mstrtab_len = (size_t)msh[mstr_i].sh_size;
                    for (size_t mj = 0; mj < msym_count && mj < 4096; ++mj) {
                      if (msym[mj].st_name >= mstrtab_len) continue;
                      const char *mn = mstrtab + msym[mj].st_name;
                      if (!mn[0]) continue;
                      uint16_t sym_sec = (uint16_t)msym[mj].st_shndx;
                      size_t sym_base_off;
                      if (sym_sec == (uint16_t)mtext_i) {
                        sym_base_off = append_off;
                      } else if (mrodata_i >= 0 && sym_sec == (uint16_t)mrodata_i) {
                        sym_base_off = rodata_off;
                      } else if (mdata_i >= 0 && sym_sec == (uint16_t)mdata_i) {
                        sym_base_off = data_off;
                      } else {
                        continue;
                      }
                      if (def_count >= 256) { free(member); ELF32_AR_FAIL; }
                      if (def_count == 0 || test_link_sym_index(defs, def_count, mn) < 0) {
                        snprintf(defs[def_count].name, sizeof(defs[def_count].name), "%s", mn);
                        defs[def_count].off = (uint32_t)(sym_base_off + msym[mj].st_value);
                        defs[def_count].shndx = 1;
                        defs[def_count].defined = true;
                        def_count++;
                      }
                    }
                    size_t member_linked_len = data_off + mdata_len;
                    member_linked_len = append_off + ((member_linked_len - append_off + 15u) & ~15u);
                    /* Reserve this member before satisfying its own archive dependencies.
                       Otherwise a dependency member can be appended at the same offset and
                       overwrite the member we just copied, making the original symbol point
                       at the dependency body on ELF32 transitive archive links. */
                    if (linked_text_len < member_linked_len)
                      linked_text_len = member_linked_len;
                    if (mrel_i >= 0 && msh[mrel_i].sh_offset + msh[mrel_i].sh_size <= member_size) {
                      ny_test_elf32_rel_t *mrel = (ny_test_elf32_rel_t *)(void *)(member + msh[mrel_i].sh_offset);
                      size_t mrel_count = msh[mrel_i].sh_entsize ? (size_t)(msh[mrel_i].sh_size / msh[mrel_i].sh_entsize) : 0;
                      for (size_t mj = 0; mj < mrel_count; ++mj) {
                        uint32_t mtype = mrel[mj].r_info & 0xffu;
                        uint32_t msi = mrel[mj].r_info >> 8;
                        if ((mtype != 1 && mtype != 2 && mtype != 4) || msi >= msym_count ||
                            msym[msi].st_name >= mstrtab_len ||
                            mrel[mj].r_offset + 4 > mtext_len) { free(member); ELF32_AR_FAIL; }
                        uint16_t sym_ndx = (uint16_t)msym[msi].st_shndx;
                        uint64_t msaddr = 0;
                        bool msec_sym = false;
                        if (sym_ndx == (uint16_t)mtext_i) {
                          msaddr = base + append_off + msym[msi].st_value;
                          msec_sym = true;
                        } else if (mrodata_i >= 0 && sym_ndx == (uint16_t)mrodata_i) {
                          msaddr = base + rodata_off + msym[msi].st_value;
                          msec_sym = true;
                        } else if (mdata_i >= 0 && sym_ndx == (uint16_t)mdata_i) {
                          msaddr = base + data_off + msym[msi].st_value;
                          msec_sym = true;
                        }
                        if (!msec_sym) {
                          int mdi = test_link_sym_index(defs, def_count, mstrtab + msym[msi].st_name);
                          if (mdi < 0) {
                            const char *mtarget = mstrtab + msym[msi].st_name;
                            if (archive_data && ar_st.count > 0) {
                              uint32_t dep_off = test_ar_find_symbol(&ar_st, mtarget);
                              if (dep_off && dep_off != member_off) {
                                if (!test_link_extract_elf32_archive_member(archive_data, archive_size, dep_off,
                                                                            &text, &text_cap, &linked_text_len,
                                                                            defs, &def_count)) {
                                  free(member); ELF32_AR_FAIL;
                                }
                                member_linked_len = linked_text_len;
                                mdi = test_link_sym_index(defs, def_count, mtarget);
                              }
                            }
                            if (mdi < 0 &&
                                (strcmp(mtarget, "malloc") == 0 || test_link_is_memset_symbol(mtarget) ||
                                test_link_is_memcpy_symbol(mtarget) || test_link_is_memmove_symbol(mtarget) ||
                                test_link_is_memchr_symbol(mtarget) || test_link_is_memcmp_symbol(mtarget) ||
                                test_link_is_strlen_symbol(mtarget) || test_link_is_strcmp_symbol(mtarget) ||
                                test_link_is_strchr_symbol(mtarget) || test_link_is_realloc_symbol(mtarget) ||
                                test_link_is_calloc_symbol(mtarget) || strcmp(mtarget, "free") == 0)) {
                              size_t stub_off = (member_linked_len + 15u) & ~15u;
                              if (stub_off + 256 > text_cap) {
                                size_t new_cap = text_cap;
                                while (new_cap < stub_off + 4096) new_cap *= 2;
                                unsigned char *nt = (unsigned char *)realloc(text, new_cap);
                                if (!nt) { free(member); ELF32_AR_FAIL; }
                                text = nt; text_cap = new_cap;
                              }
                              if (stub_off > member_linked_len)
                                memset(text + member_linked_len, 0x90, stub_off - member_linked_len);
                              size_t slen = 0;
                              bool emitted = false;
                              if (strcmp(mtarget, "malloc") == 0) emitted = test_emit_malloc_stub32(text + stub_off, text_cap - stub_off, &slen);
                              else if (strcmp(mtarget, "free") == 0) emitted = test_emit_free_stub32(text + stub_off, text_cap - stub_off, &slen);
                              else if (test_link_is_memset_symbol(mtarget)) emitted = test_emit_memset_stub32(text + stub_off, text_cap - stub_off, &slen);
                              else if (test_link_is_memcpy_symbol(mtarget)) emitted = test_emit_memcpy_stub32(text + stub_off, text_cap - stub_off, &slen);
                              else if (test_link_is_memmove_symbol(mtarget)) emitted = test_emit_memmove_stub32(text + stub_off, text_cap - stub_off, &slen);
                              else if (test_link_is_memchr_symbol(mtarget)) emitted = test_emit_memchr_stub32(text + stub_off, text_cap - stub_off, &slen);
                              else if (test_link_is_memcmp_symbol(mtarget)) emitted = test_emit_memcmp_stub32(text + stub_off, text_cap - stub_off, &slen);
                              else if (test_link_is_strlen_symbol(mtarget)) emitted = test_emit_strlen_stub32(text + stub_off, text_cap - stub_off, &slen);
                              else if (test_link_is_strcmp_symbol(mtarget)) emitted = test_emit_strcmp_stub32(text + stub_off, text_cap - stub_off, &slen);
                              else if (test_link_is_strchr_symbol(mtarget)) emitted = test_emit_strchr_stub32(text + stub_off, text_cap - stub_off, &slen);
                              else if (test_link_is_realloc_symbol(mtarget)) emitted = test_emit_realloc_stub32(text + stub_off, text_cap - stub_off, &slen);
                              else if (test_link_is_calloc_symbol(mtarget)) emitted = test_emit_calloc_stub32(text + stub_off, text_cap - stub_off, &slen);
                              if (!emitted || slen == 0 || !test_link_add_def(defs, &def_count, mtarget, (uint32_t)stub_off))
                                { free(member); ELF32_AR_FAIL; }
                              member_linked_len = stub_off + slen;
                              mdi = test_link_sym_index(defs, def_count, mtarget);
                            }
                          }
                          if (mdi < 0) { free(member); ELF32_AR_FAIL; }
                          msaddr = base + defs[mdi].off;
                        }
                        int32_t old_val = 0;
                        memcpy(&old_val, member + msh[mtext_i].sh_offset + mrel[mj].r_offset, 4);
                        if (mtype == 1) {
                          int64_t aval = (int64_t)msaddr + (int64_t)old_val;
                          if (aval < INT32_MIN || aval > INT32_MAX) { free(member); ELF32_AR_FAIL; }
                          int32_t av32 = (int32_t)aval;
                          memcpy(text + append_off + mrel[mj].r_offset, &av32, 4);
                        } else {
                          uint64_t mpaddr = base + append_off + mrel[mj].r_offset;
                          int64_t mval = (int64_t)msaddr - (int64_t)mpaddr + (int64_t)old_val;
                          if (mval < INT32_MIN || mval > INT32_MAX) { free(member); ELF32_AR_FAIL; }
                          int32_t mv32 = (int32_t)mval;
                          memcpy(text + append_off + mrel[mj].r_offset, &mv32, 4);
                        }
                      }
                    }
                    linked_text_len = member_linked_len;
                    resolved = true;
                  }
                }
              }
            }
            free(member);
          }
        }
      }
      if (!resolved) {
        size_t stub_off = (linked_text_len + 15u) & ~15u;
        if (stub_off + 256 > text_cap) {
          size_t new_cap = text_cap;
          while (new_cap < stub_off + 4096) new_cap *= 2;
          unsigned char *nt = (unsigned char *)realloc(text, new_cap);
          if (!nt) ELF32_AR_FAIL;
          text = nt; text_cap = new_cap;
        }
        if (stub_off > linked_text_len)
          memset(text + linked_text_len, 0x90, stub_off - linked_text_len);
        size_t stub_len = 0;
        bool emitted = false;
        if (strcmp(name, "malloc") == 0)
          emitted = test_emit_malloc_stub32(text + stub_off, text_cap - stub_off, &stub_len);
        else if (strcmp(name, "free") == 0)
          emitted = test_emit_free_stub32(text + stub_off, text_cap - stub_off, &stub_len);
        else if (test_link_is_memset_symbol(name))
          emitted = test_emit_memset_stub32(text + stub_off, text_cap - stub_off, &stub_len);
        else if (test_link_is_memcpy_symbol(name))
          emitted = test_emit_memcpy_stub32(text + stub_off, text_cap - stub_off, &stub_len);
        else if (test_link_is_memmove_symbol(name))
          emitted = test_emit_memmove_stub32(text + stub_off, text_cap - stub_off, &stub_len);
        else if (test_link_is_memchr_symbol(name))
          emitted = test_emit_memchr_stub32(text + stub_off, text_cap - stub_off, &stub_len);
        else if (test_link_is_memcmp_symbol(name))
          emitted = test_emit_memcmp_stub32(text + stub_off, text_cap - stub_off, &stub_len);
        else if (test_link_is_strlen_symbol(name))
          emitted = test_emit_strlen_stub32(text + stub_off, text_cap - stub_off, &stub_len);
        else if (test_link_is_strcmp_symbol(name))
          emitted = test_emit_strcmp_stub32(text + stub_off, text_cap - stub_off, &stub_len);
        else if (test_link_is_strchr_symbol(name))
          emitted = test_emit_strchr_stub32(text + stub_off, text_cap - stub_off, &stub_len);
        else if (test_link_is_realloc_symbol(name))
          emitted = test_emit_realloc_stub32(text + stub_off, text_cap - stub_off, &stub_len);
        else if (test_link_is_calloc_symbol(name))
          emitted = test_emit_calloc_stub32(text + stub_off, text_cap - stub_off, &stub_len);
        else
          ELF32_AR_FAIL;
        if (!emitted || stub_len == 0 || !test_link_add_def(defs, &def_count, name, (uint32_t)stub_off))
          ELF32_AR_FAIL;
        linked_text_len = stub_off + stub_len;
      }
    }
    for (size_t i = 0; i < rel_count; ++i) {
      uint32_t type = rel[i].r_info & 0xffu;
      uint32_t si = rel[i].r_info >> 8;
      if (type != 2 || si >= sym_count || rel[i].r_offset + 4 > text_len ||
          sym[si].st_name >= strtab_len) ELF32_AR_FAIL;
      const char *name = strtab + sym[si].st_name;
      int di = test_link_sym_index(defs, def_count, name);
      if (di < 0) ELF32_AR_FAIL;
      int32_t addend = 0;
      memcpy(&addend, text + rel[i].r_offset, 4);
      int64_t val = (int64_t)(base + (uint32_t)defs[di].off) + addend -
                    (int64_t)(base + rel[i].r_offset);
      if (val < INT32_MIN || val > INT32_MAX) ELF32_AR_FAIL;
      int32_t v32 = (int32_t)val;
      memcpy(text + rel[i].r_offset, &v32, 4);
    }
  }
  size_t harness_off = (linked_text_len + 15u) & ~15u;
  if (harness_off >= text_cap) ELF32_AR_FAIL;
  memset(text + linked_text_len, 0x90, harness_off - linked_text_len);
  size_t harness_len = test_emit_harness32(text + harness_off, text_cap - harness_off,
                                           ret_kind, expected, base, (uint32_t)harness_off,
                                           rt_main_off);
  if (harness_len == 0) ELF32_AR_FAIL;
  size_t code_len = harness_off + harness_len;
  size_t file_off = 0x1000;
  size_t file_len = file_off + code_len;
  unsigned char *exe = (unsigned char *)calloc(1, file_len);
  if (!exe) ELF32_AR_FAIL;
  ny_test_elf32_ehdr_t oh = {0};
  oh.e_ident[0] = 0x7f; oh.e_ident[1] = 'E'; oh.e_ident[2] = 'L'; oh.e_ident[3] = 'F';
  oh.e_ident[4] = 1; oh.e_ident[5] = 1; oh.e_ident[6] = 1;
  oh.e_type = 2; oh.e_machine = 3; oh.e_version = 1;
  oh.e_entry = base + (uint32_t)harness_off; oh.e_phoff = sizeof(oh);
  oh.e_ehsize = sizeof(oh); oh.e_phentsize = sizeof(ny_test_elf32_phdr_t); oh.e_phnum = 1;
  ny_test_elf32_phdr_t ph = {0};
  ph.p_type = 1; ph.p_offset = (uint32_t)file_off; ph.p_vaddr = base;
  ph.p_paddr = base; ph.p_filesz = (uint32_t)code_len; ph.p_memsz = (uint32_t)code_len;
  ph.p_flags = 5; ph.p_align = 0x1000;
  memcpy(exe, &oh, sizeof(oh));
  memcpy(exe + oh.e_phoff, &ph, sizeof(ph));
  memcpy(exe + file_off, text, code_len);
  char exe_path[PATH_MAX];
  snprintf(exe_path, sizeof(exe_path), "%s/ny-internal-link-run-elf32-%ld-%ld",
           nyt_temp_dir(), (long)getpid(), (long)now_ms());
  ok = test_write_all_file(exe_path, exe, file_len);
  free(exe); free(text); free(obj); free(archive_data); test_ar_free_symtab(&ar_st);
  if (!ok)
    return 2;
  chmod(exe_path, 0755);
  char *run_argv[] = {exe_path, NULL};
  int rc = run_debug_argv(run_argv, 30, 0);
  if (test_env_truthy("NYTRIX_TEST_KEEP_INTERNAL_LINK"))
    fprintf(stderr, "object link/run: kept internal ELF32 executable %s\n", exe_path);
  else
    remove(exe_path);
  if (rc == 126 || rc == 127 || rc == 128 + SIGSEGV || rc == 128 + SIGILL || rc == 128 + SIGBUS)
    return 2;
  if (rc != 0) {
    fprintf(stderr, "object link/run: internal ELF32 executable failed rc=%d for %s\n",
            rc, disp_path(shape_path));
    return 1;
  }
  return 0;
#endif
}

static int test_internal_elf64_link_run(const char *obj_path,
                                        ny_test_link_ret_kind_t ret_kind,
                                        const char *expected, const char *shape_path,
                                        const char *archive_path) {
#ifndef __linux__
  (void)obj_path; (void)ret_kind; (void)expected; (void)shape_path; (void)archive_path;
  return 2;
#else
  if (test_has_unsupported_external_runtime_symbol(obj_path))
    return 2;
  ny_test_ar_symtab_t ar_st;
  memset(&ar_st, 0, sizeof(ar_st));
  unsigned char *archive_data = NULL;
  size_t archive_size = 0;
  if (archive_path && *archive_path) {
    struct stat ar_stbuf;
    if (stat(archive_path, &ar_stbuf) != 0 || ar_stbuf.st_size <= 0 ||
        ar_stbuf.st_size > 32 * 1024 * 1024) {
      fprintf(stderr, "AR: cannot stat archive %s\n", archive_path);
      return 2;
    }
    archive_size = (size_t)ar_stbuf.st_size;
    archive_data = (unsigned char *)malloc(archive_size);
    if (!archive_data) return 2;
    FILE *arf = fopen(archive_path, "rb");
    if (!arf) { free(archive_data); return 2; }
    bool arok = fread(archive_data, 1, archive_size, arf) == archive_size;
    fclose(arf);
    if (!arok || !test_ar_read_symtab(archive_data, archive_size, &ar_st)) {
      fprintf(stderr, "AR: failed to read symtab from %s (arok=%d)\n", archive_path, arok);
      free(archive_data);
      archive_data = NULL;
      ar_st.count = 0;
    }
  }
  struct stat st;
  if (stat(obj_path, &st) != 0 || st.st_size <= 0 || st.st_size > 16 * 1024 * 1024)
    { free(archive_data); test_ar_free_symtab(&ar_st); return 2; }
  FILE *f = fopen(obj_path, "rb");
  if (!f) { free(archive_data); test_ar_free_symtab(&ar_st); return 2; }
  unsigned char *obj = (unsigned char *)malloc((size_t)st.st_size);
  if (!obj) { fclose(f); free(archive_data); test_ar_free_symtab(&ar_st); return 2; }
  bool ok = fread(obj, 1, (size_t)st.st_size, f) == (size_t)st.st_size;
  fclose(f);
  if (!ok) { free(obj); free(archive_data); test_ar_free_symtab(&ar_st); return 2; }
  if ((size_t)st.st_size < sizeof(ny_test_elf64_ehdr_t))
    { free(obj); free(archive_data); test_ar_free_symtab(&ar_st); return 2; }
  ny_test_elf64_ehdr_t eh;
  memcpy(&eh, obj, sizeof(eh));
  if (eh.e_ident[0] != 0x7f || eh.e_ident[1] != 'E' || eh.e_ident[2] != 'L' ||
      eh.e_ident[3] != 'F' || eh.e_ident[4] != 2 || eh.e_ident[5] != 1 ||
      eh.e_type != 1 || eh.e_machine != 62 || eh.e_shentsize != sizeof(ny_test_elf64_shdr_t) ||
      eh.e_shoff + (uint64_t)eh.e_shnum * sizeof(ny_test_elf64_shdr_t) > (uint64_t)st.st_size)
    { free(obj); free(archive_data); test_ar_free_symtab(&ar_st); return 2; }
  ny_test_elf64_shdr_t *sh = (ny_test_elf64_shdr_t *)(void *)(obj + eh.e_shoff);
  if (eh.e_shstrndx >= eh.e_shnum || sh[eh.e_shstrndx].sh_offset + sh[eh.e_shstrndx].sh_size > (uint64_t)st.st_size)
    { free(obj); free(archive_data); test_ar_free_symtab(&ar_st); return 2; }
  const char *shstr = (const char *)(obj + sh[eh.e_shstrndx].sh_offset);
  int text_i = -1, sym_i = -1, str_i = -1, rela_i = -1;
  for (int i = 0; i < eh.e_shnum; ++i) {
    const char *name = sh[i].sh_name < sh[eh.e_shstrndx].sh_size ? shstr + sh[i].sh_name : "";
    if (strcmp(name, ".text") == 0) text_i = i;
    else if (strcmp(name, ".symtab") == 0) sym_i = i;
    else if (strcmp(name, ".strtab") == 0) str_i = i;
    else if (strcmp(name, ".rela.text") == 0) rela_i = i;
  }
  if (text_i < 0 || sym_i < 0 || str_i < 0 ||
      sh[text_i].sh_offset + sh[text_i].sh_size > (uint64_t)st.st_size ||
      sh[sym_i].sh_offset + sh[sym_i].sh_size > (uint64_t)st.st_size ||
      sh[str_i].sh_offset + sh[str_i].sh_size > (uint64_t)st.st_size)
    { free(obj); free(archive_data); test_ar_free_symtab(&ar_st); return 2; }
  size_t text_len = (size_t)sh[text_i].sh_size;
  size_t text_cap = text_len + 4096;
  unsigned char *text = (unsigned char *)malloc(text_cap);
  if (!text) { free(obj); free(archive_data); test_ar_free_symtab(&ar_st); return 2; }
  memcpy(text, obj + sh[text_i].sh_offset, text_len);
  ny_test_elf64_sym_t *sym = (ny_test_elf64_sym_t *)(void *)(obj + sh[sym_i].sh_offset);
  size_t sym_count = sh[sym_i].sh_entsize ? (size_t)(sh[sym_i].sh_size / sh[sym_i].sh_entsize) : 0;
  const char *strtab = (const char *)(obj + sh[str_i].sh_offset);
  size_t strtab_len = (size_t)sh[str_i].sh_size;
  ny_test_link_sym_t defs[256];
  size_t def_count = 0;
  uint64_t rt_main_off = 0;
  bool have_rt_main = false;
  for (size_t i = 0; i < sym_count && i < 4096; ++i) {
    if (sym[i].st_name >= strtab_len) continue;
    const char *name = strtab + sym[i].st_name;
    if (!name[0]) continue;
    if (sym[i].st_shndx == (uint16_t)text_i) {
      if (def_count >= sizeof(defs) / sizeof(defs[0]))
        { free(text); free(obj); free(archive_data); test_ar_free_symtab(&ar_st); return 2; }
      snprintf(defs[def_count].name, sizeof(defs[def_count].name), "%s", name);
      defs[def_count].off = sym[i].st_value;
      defs[def_count].shndx = sym[i].st_shndx;
      defs[def_count].defined = true;
      if (strcmp(name, "rt_main") == 0) { rt_main_off = sym[i].st_value; have_rt_main = true; }
      def_count++;
    }
  }
  if (!have_rt_main)
    { free(text); free(obj); free(archive_data); test_ar_free_symtab(&ar_st); return 2; }
  const uint64_t base = 0x400000u + 0x1000u;
  size_t linked_text_len = text_len;
  size_t max_rela = rela_i >= 0 ? (sh[rela_i].sh_entsize ? (size_t)(sh[rela_i].sh_size / sh[rela_i].sh_entsize) : 0) : 0;
#define TEST_AR_FAIL \
  do { free(text); free(obj); free(archive_data); test_ar_free_symtab(&ar_st); return 2; } while(0)

  uint32_t extracted_offsets[256];
  size_t extracted_count = 0;

  { // Pass 1: extract all members that define symbols reachable from main object
    bool more;
    do {
      more = false;
      for (size_t ri = 0; ri < max_rela; ++ri) {
        ny_test_elf64_rela_t *r = (ny_test_elf64_rela_t *)(void *)(obj + sh[rela_i].sh_offset) + ri;
        uint32_t rtype = (uint32_t)(r->r_info & 0xffffffffu);
        uint32_t rsi = (uint32_t)(r->r_info >> 32);
        if ((rtype != 2 && rtype != 4) || rsi >= sym_count ||
            r->r_offset + 4 > text_len || sym[rsi].st_name >= strtab_len) TEST_AR_FAIL;
        const char *rname = strtab + sym[rsi].st_name;
        if (test_link_sym_index(defs, def_count, rname) >= 0)
          continue;
        if (archive_data && ar_st.count > 0) {
          uint32_t member_off = test_ar_find_symbol(&ar_st, rname);
          if (member_off) {
            bool already = false;
            for (size_t ei = 0; ei < extracted_count; ++ei) {
              if (extracted_offsets[ei] == member_off) { already = true; break; }
            }
            if (!already) {
              size_t member_size = 0;
              unsigned char *member = test_ar_extract_member(archive_data, archive_size,
                                                             member_off, &member_size);
              if (member && member_size > 0) {
                ny_test_elf64_ehdr_t meh;
                if (member_size >= sizeof(meh)) {
                  memcpy(&meh, member, sizeof(meh));
                  if (meh.e_ident[0] == 0x7f && meh.e_ident[1] == 'E' && meh.e_ident[2] == 'L' &&
                      meh.e_ident[3] == 'F' && meh.e_ident[4] == 2 && meh.e_ident[5] == 1 &&
                      meh.e_type == 1 && meh.e_machine == 62 &&
                      meh.e_shentsize == sizeof(ny_test_elf64_shdr_t) &&
                      meh.e_shoff + (uint64_t)meh.e_shnum * sizeof(ny_test_elf64_shdr_t) <= member_size) {
                    ny_test_elf64_shdr_t *msh = (ny_test_elf64_shdr_t *)(void *)(member + meh.e_shoff);
                    if (meh.e_shstrndx < meh.e_shnum &&
                        msh[meh.e_shstrndx].sh_offset + msh[meh.e_shstrndx].sh_size <= member_size) {
                      const char *mshstr = (const char *)(member + msh[meh.e_shstrndx].sh_offset);
                      int mtext_i = -1, mrodata_i = -1, mdata_i = -1,
                          msym_i = -1, mstr_i = -1;
                      for (int mi = 0; mi < meh.e_shnum; ++mi) {
                        const char *mn = msh[mi].sh_name < msh[meh.e_shstrndx].sh_size
                                             ? mshstr + msh[mi].sh_name : "";
                        if (strcmp(mn, ".text") == 0) mtext_i = mi;
                        else if (strcmp(mn, ".rodata") == 0) mrodata_i = mi;
                        else if (strcmp(mn, ".data") == 0) mdata_i = mi;
                        else if (strcmp(mn, ".symtab") == 0) msym_i = mi;
                        else if (strcmp(mn, ".strtab") == 0) mstr_i = mi;
                      }
                      if (mtext_i >= 0 && msym_i >= 0 && mstr_i >= 0 &&
                          msh[mtext_i].sh_offset + msh[mtext_i].sh_size <= member_size &&
                          msh[msym_i].sh_offset + msh[msym_i].sh_size <= member_size &&
                          msh[mstr_i].sh_offset + msh[mstr_i].sh_size <= member_size &&
                          (mrodata_i < 0 || msh[mrodata_i].sh_offset + msh[mrodata_i].sh_size <= member_size) &&
                          (mdata_i < 0 || msh[mdata_i].sh_offset + msh[mdata_i].sh_size <= member_size)) {
                        size_t append_off = linked_text_len;
                        size_t mtext_len = (size_t)msh[mtext_i].sh_size;
                        size_t mrodata_len = mrodata_i >= 0 ? (size_t)msh[mrodata_i].sh_size : 0;
                        size_t mdata_len = mdata_i >= 0 ? (size_t)msh[mdata_i].sh_size : 0;
                        size_t rodata_off = append_off + mtext_len;
                        size_t data_off = rodata_off + mrodata_len;
                        size_t total_needed = data_off + mdata_len;
                        if (total_needed > text_cap) {
                          size_t new_cap = text_cap;
                          while (new_cap < total_needed + 4096) new_cap *= 2;
                          unsigned char *nt = (unsigned char *)realloc(text, new_cap);
                          if (!nt) { free(member); TEST_AR_FAIL; }
                          text = nt; text_cap = new_cap;
                        }
                        memcpy(text + append_off, member + msh[mtext_i].sh_offset, mtext_len);
                        if (mrodata_i >= 0)
                          memcpy(text + rodata_off, member + msh[mrodata_i].sh_offset, mrodata_len);
                        if (mdata_i >= 0)
                          memcpy(text + data_off, member + msh[mdata_i].sh_offset, mdata_len);
                        ny_test_elf64_sym_t *msym =
                            (ny_test_elf64_sym_t *)(void *)(member + msh[msym_i].sh_offset);
                        size_t msym_count = msh[msym_i].sh_entsize
                                                ? (size_t)(msh[msym_i].sh_size / msh[msym_i].sh_entsize) : 0;
                        const char *mstrtab = (const char *)(member + msh[mstr_i].sh_offset);
                        size_t mstrtab_len = (size_t)msh[mstr_i].sh_size;
                        for (size_t mj = 0; mj < msym_count && mj < 4096; ++mj) {
                          if (msym[mj].st_name >= mstrtab_len) continue;
                          const char *mn = mstrtab + msym[mj].st_name;
                          if (!mn[0]) continue;
                          uint16_t sym_sec = (uint16_t)msym[mj].st_shndx;
                          if (sym_sec != (uint16_t)mtext_i &&
                              (mrodata_i < 0 || sym_sec != (uint16_t)mrodata_i) &&
                              (mdata_i < 0 || sym_sec != (uint16_t)mdata_i))
                            continue;
                          if (def_count >= 256) { free(member); TEST_AR_FAIL; }
                          if (def_count == 0 || test_link_sym_index(defs, def_count, mn) < 0) {
                            snprintf(defs[def_count].name, sizeof(defs[def_count].name), "%s", mn);
                            defs[def_count].off = sym_sec == (uint16_t)mtext_i
                                                      ? (append_off + msym[mj].st_value)
                                                      : (sym_sec == (uint16_t)mrodata_i
                                                             ? (rodata_off + msym[mj].st_value)
                                                             : (data_off + msym[mj].st_value));
                            defs[def_count].shndx = sym_sec == (uint16_t)mtext_i ? 1 : 2;
                            defs[def_count].defined = true;
                            def_count++;
                          }
                        }
                        extracted_offsets[extracted_count++] = member_off;
                        linked_text_len = data_off + mdata_len;
                        linked_text_len = append_off + ((linked_text_len - append_off + 15u) & ~15u);
                        more = true;
                      }
                    }
                  }
                }
              }
              free(member);
            }
          }
        }
      }
    } while (more);
  }

  { // Pass 2: process relocation for all extracted members
    for (size_t ei = 0; ei < extracted_count; ++ei) {
      size_t member_size = 0;
      unsigned char *member = test_ar_extract_member(archive_data, archive_size,
                                                     extracted_offsets[ei], &member_size);
      if (!member || member_size == 0) TEST_AR_FAIL;
      ny_test_elf64_ehdr_t meh;
      if (member_size < sizeof(meh)) { free(member); TEST_AR_FAIL; }
      memcpy(&meh, member, sizeof(meh));
      if (meh.e_ident[0] != 0x7f || meh.e_ident[1] != 'E' || meh.e_ident[2] != 'L' ||
          meh.e_ident[3] != 'F' || meh.e_ident[4] != 2 || meh.e_ident[5] != 1 ||
          meh.e_type != 1 || meh.e_machine != 62 ||
          meh.e_shentsize != sizeof(ny_test_elf64_shdr_t) ||
          meh.e_shoff + (uint64_t)meh.e_shnum * sizeof(ny_test_elf64_shdr_t) > member_size) {
        free(member); TEST_AR_FAIL;
      }
      ny_test_elf64_shdr_t *msh = (ny_test_elf64_shdr_t *)(void *)(member + meh.e_shoff);
      if (meh.e_shstrndx >= meh.e_shnum ||
          msh[meh.e_shstrndx].sh_offset + msh[meh.e_shstrndx].sh_size > member_size) {
        free(member); TEST_AR_FAIL;
      }
      const char *mshstr = (const char *)(member + msh[meh.e_shstrndx].sh_offset);
      int mtext_i = -1, mrodata_i = -1, mdata_i = -1, msym_i = -1, mstr_i = -1, mrela_i = -1;
      for (int mi = 0; mi < meh.e_shnum; ++mi) {
        const char *mn = msh[mi].sh_name < msh[meh.e_shstrndx].sh_size ? mshstr + msh[mi].sh_name : "";
        if (strcmp(mn, ".text") == 0) mtext_i = mi;
        else if (strcmp(mn, ".rodata") == 0) mrodata_i = mi;
        else if (strcmp(mn, ".data") == 0) mdata_i = mi;
        else if (strcmp(mn, ".symtab") == 0) msym_i = mi;
        else if (strcmp(mn, ".strtab") == 0) mstr_i = mi;
        else if (strcmp(mn, ".rela.text") == 0) mrela_i = mi;
      }
      if (mtext_i < 0 || msym_i < 0 || mstr_i < 0 ||
          msh[mtext_i].sh_offset + msh[mtext_i].sh_size > member_size ||
          msh[msym_i].sh_offset + msh[msym_i].sh_size > member_size ||
          msh[mstr_i].sh_offset + msh[mstr_i].sh_size > member_size ||
          (mrodata_i >= 0 && msh[mrodata_i].sh_offset + msh[mrodata_i].sh_size > member_size) ||
          (mdata_i >= 0 && msh[mdata_i].sh_offset + msh[mdata_i].sh_size > member_size)) {
        free(member); TEST_AR_FAIL;
      }
      size_t mtext_len = (size_t)msh[mtext_i].sh_size;
      size_t mrodata_len = mrodata_i >= 0 ? (size_t)msh[mrodata_i].sh_size : 0;
      size_t mdata_len = mdata_i >= 0 ? (size_t)msh[mdata_i].sh_size : 0;
      ny_test_elf64_sym_t *msym = (ny_test_elf64_sym_t *)(void *)(member + msh[msym_i].sh_offset);
      size_t msym_count = msh[msym_i].sh_entsize ? (size_t)(msh[msym_i].sh_size / msh[msym_i].sh_entsize) : 0;
      const char *mstrtab = (const char *)(member + msh[mstr_i].sh_offset);
      size_t mstrtab_len = (size_t)msh[mstr_i].sh_size;
      size_t append_off = UINTPTR_MAX;
      for (size_t mj = 0; mj < msym_count && append_off == UINTPTR_MAX; ++mj) {
        if (msym[mj].st_name >= mstrtab_len) continue;
        const char *mn = mstrtab + msym[mj].st_name;
        if (!mn[0]) continue;
        int mdi = test_link_sym_index(defs, def_count, mn);
        if (mdi < 0) continue;
        uint16_t sym_sec = (uint16_t)msym[mj].st_shndx;
        if (sym_sec == (uint16_t)mtext_i) {
          append_off = (size_t)(defs[mdi].off - msym[mj].st_value);
        } else if (mrodata_i >= 0 && sym_sec == (uint16_t)mrodata_i) {
          append_off = (size_t)(defs[mdi].off - msym[mj].st_value - mtext_len);
        } else if (mdata_i >= 0 && sym_sec == (uint16_t)mdata_i) {
          append_off = (size_t)(defs[mdi].off - msym[mj].st_value - mtext_len - mrodata_len);
        }
      }
      if (append_off == UINTPTR_MAX) { free(member); TEST_AR_FAIL; }
      size_t rodata_off = append_off + mtext_len;
      size_t data_off = rodata_off + mrodata_len;
      size_t member_linked_len = data_off + mdata_len;
      member_linked_len = append_off + ((member_linked_len - append_off + 15u) & ~15u);
      if (mrela_i >= 0 && msh[mrela_i].sh_offset + msh[mrela_i].sh_size <= member_size) {
        ny_test_elf64_rela_t *mrela = (ny_test_elf64_rela_t *)(void *)(member + msh[mrela_i].sh_offset);
        size_t mrela_count = msh[mrela_i].sh_entsize
                                 ? (size_t)(msh[mrela_i].sh_size / msh[mrela_i].sh_entsize) : 0;
        for (size_t mj = 0; mj < mrela_count; ++mj) {
          uint32_t mtype = (uint32_t)(mrela[mj].r_info & 0xffffffffu);
          uint32_t msi = (uint32_t)(mrela[mj].r_info >> 32);
          if ((mtype != 2 && mtype != 4 && mtype != 11) || msi >= msym_count ||
              msym[msi].st_name >= mstrtab_len ||
              mrela[mj].r_offset + 4 > mtext_len) { free(member); TEST_AR_FAIL; }
          uint16_t sym_ndx = (uint16_t)msym[msi].st_shndx;
          uint64_t msaddr = 0;
          bool msec_sym = false;
          if (mrela[mj].r_offset < mtext_len && sym_ndx == (uint16_t)mtext_i) {
            msaddr = base + append_off + msym[msi].st_value;
            msec_sym = true;
          } else if (mrodata_i >= 0 && sym_ndx == (uint16_t)mrodata_i) {
            msaddr = base + rodata_off + msym[msi].st_value;
            msec_sym = true;
          } else if (mdata_i >= 0 && sym_ndx == (uint16_t)mdata_i) {
            msaddr = base + data_off + msym[msi].st_value;
            msec_sym = true;
          }
          if (!msec_sym) {
            int mdi = test_link_sym_index(defs, def_count, mstrtab + msym[msi].st_name);
            if (mdi < 0) {
              const char *mtarget = mstrtab + msym[msi].st_name;
              if (archive_data && ar_st.count > 0) {
                uint32_t dep_off = test_ar_find_symbol(&ar_st, mtarget);
                if (dep_off && !test_link_archive_offset_seen(extracted_offsets, extracted_count, dep_off)) {
                  if (extracted_count >= sizeof(extracted_offsets) / sizeof(extracted_offsets[0]) ||
                      !test_link_extract_elf64_archive_member(archive_data, archive_size, dep_off,
                                                              &text, &text_cap, &linked_text_len,
                                                              defs, &def_count)) {
                    free(member); TEST_AR_FAIL;
                  }
                  extracted_offsets[extracted_count++] = dep_off;
                  member_linked_len = linked_text_len;
                  mdi = test_link_sym_index(defs, def_count, mtarget);
                }
              }
              if (mdi < 0 &&
                  (strcmp(mtarget, "malloc") == 0 || test_link_is_memset_symbol(mtarget) ||
                  test_link_is_memcpy_symbol(mtarget) || test_link_is_memmove_symbol(mtarget) ||
                  test_link_is_memchr_symbol(mtarget) || test_link_is_memcmp_symbol(mtarget) ||
                  test_link_is_strlen_symbol(mtarget) || test_link_is_strcmp_symbol(mtarget) ||
                  test_link_is_strchr_symbol(mtarget) || test_link_is_realloc_symbol(mtarget) ||
                  test_link_is_calloc_symbol(mtarget) || strcmp(mtarget, "free") == 0)) {
                size_t stub_off = (member_linked_len + 15u) & ~15u;
                if (stub_off + 256 > text_cap) {
                  size_t new_cap = text_cap;
                  while (new_cap < stub_off + 4096) new_cap *= 2;
                  unsigned char *nt = (unsigned char *)realloc(text, new_cap);
                  if (!nt) { free(member); TEST_AR_FAIL; }
                  text = nt; text_cap = new_cap;
                }
                if (stub_off > member_linked_len)
                  memset(text + member_linked_len, 0x90, stub_off - member_linked_len);
                size_t slen = 0;
                bool emitted = false;
                if (strcmp(mtarget, "malloc") == 0) emitted = test_emit_malloc_stub(text + stub_off, text_cap - stub_off, &slen);
                else if (strcmp(mtarget, "free") == 0) emitted = test_emit_free_stub(text + stub_off, text_cap - stub_off, &slen);
                else if (test_link_is_memset_symbol(mtarget)) emitted = test_emit_memset_stub(text + stub_off, text_cap - stub_off, &slen);
                else if (test_link_is_memcpy_symbol(mtarget)) emitted = test_emit_memcpy_stub(text + stub_off, text_cap - stub_off, &slen);
                else if (test_link_is_memmove_symbol(mtarget)) emitted = test_emit_memmove_stub(text + stub_off, text_cap - stub_off, &slen);
                else if (test_link_is_memchr_symbol(mtarget)) emitted = test_emit_memchr_stub(text + stub_off, text_cap - stub_off, &slen);
                else if (test_link_is_memcmp_symbol(mtarget)) emitted = test_emit_memcmp_stub(text + stub_off, text_cap - stub_off, &slen);
                else if (test_link_is_strlen_symbol(mtarget)) emitted = test_emit_strlen_stub(text + stub_off, text_cap - stub_off, &slen);
                else if (test_link_is_strcmp_symbol(mtarget)) emitted = test_emit_strcmp_stub(text + stub_off, text_cap - stub_off, &slen);
                else if (test_link_is_strchr_symbol(mtarget)) emitted = test_emit_strchr_stub(text + stub_off, text_cap - stub_off, &slen);
                else if (test_link_is_realloc_symbol(mtarget)) emitted = test_emit_realloc_stub(text + stub_off, text_cap - stub_off, &slen);
                else if (test_link_is_calloc_symbol(mtarget)) emitted = test_emit_calloc_stub(text + stub_off, text_cap - stub_off, &slen);
                if (!emitted || slen == 0 || !test_link_add_def(defs, &def_count, mtarget, stub_off))
                  { free(member); TEST_AR_FAIL; }
                member_linked_len = stub_off + slen;
                mdi = test_link_sym_index(defs, def_count, mtarget);
              }
            }
            if (mdi < 0) { free(member); TEST_AR_FAIL; }
            msaddr = base + defs[mdi].off;
          }
          if (mtype == 11) {
            int64_t aval = (int64_t)msaddr + mrela[mj].r_addend;
            if (aval < INT32_MIN || aval > INT32_MAX) { free(member); TEST_AR_FAIL; }
            int32_t av32 = (int32_t)aval;
            memcpy(text + append_off + mrela[mj].r_offset, &av32, 4);
          } else {
            uint64_t mpaddr = base + append_off + mrela[mj].r_offset;
            int64_t mval = (int64_t)msaddr + mrela[mj].r_addend - (int64_t)mpaddr;
            if (mval < INT32_MIN || mval > INT32_MAX) { free(member); TEST_AR_FAIL; }
            int32_t mv32 = (int32_t)mval;
            memcpy(text + append_off + mrela[mj].r_offset, &mv32, 4);
          }
        }
      }
      linked_text_len = linked_text_len > member_linked_len ? linked_text_len : member_linked_len;
      free(member);
    }
  }
  for (size_t i = 0; i < max_rela; ++i) {
    ny_test_elf64_rela_t *r = (ny_test_elf64_rela_t *)(void *)(obj + sh[rela_i].sh_offset) + i;
    uint32_t type = (uint32_t)(r->r_info & 0xffffffffu);
    uint32_t si = (uint32_t)(r->r_info >> 32);
    if ((type != 2 && type != 4) || si >= sym_count ||
        r->r_offset + 4 > text_len || sym[si].st_name >= strtab_len) TEST_AR_FAIL;
    const char *name = strtab + sym[si].st_name;
    if (test_link_sym_index(defs, def_count, name) >= 0)
      continue;
    size_t stub_off = (linked_text_len + 15u) & ~15u;
    if (stub_off + 256 > text_cap) {
      size_t new_cap = text_cap;
      while (new_cap < stub_off + 4096) new_cap *= 2;
      unsigned char *nt = (unsigned char *)realloc(text, new_cap);
      if (!nt) TEST_AR_FAIL;
      text = nt; text_cap = new_cap;
    }
    if (stub_off > linked_text_len)
      memset(text + linked_text_len, 0x90, stub_off - linked_text_len);
    size_t stub_len = 0;
    bool emitted = false;
    if (strcmp(name, "malloc") == 0) emitted = test_emit_malloc_stub(text + stub_off, text_cap - stub_off, &stub_len);
    else if (strcmp(name, "free") == 0) emitted = test_emit_free_stub(text + stub_off, text_cap - stub_off, &stub_len);
    else if (test_link_is_memset_symbol(name)) emitted = test_emit_memset_stub(text + stub_off, text_cap - stub_off, &stub_len);
    else if (test_link_is_memcpy_symbol(name)) emitted = test_emit_memcpy_stub(text + stub_off, text_cap - stub_off, &stub_len);
    else if (test_link_is_memmove_symbol(name)) emitted = test_emit_memmove_stub(text + stub_off, text_cap - stub_off, &stub_len);
    else if (test_link_is_memchr_symbol(name)) emitted = test_emit_memchr_stub(text + stub_off, text_cap - stub_off, &stub_len);
    else if (test_link_is_memcmp_symbol(name)) emitted = test_emit_memcmp_stub(text + stub_off, text_cap - stub_off, &stub_len);
    else if (test_link_is_strlen_symbol(name)) emitted = test_emit_strlen_stub(text + stub_off, text_cap - stub_off, &stub_len);
    else if (test_link_is_strcmp_symbol(name)) emitted = test_emit_strcmp_stub(text + stub_off, text_cap - stub_off, &stub_len);
    else if (test_link_is_strchr_symbol(name)) emitted = test_emit_strchr_stub(text + stub_off, text_cap - stub_off, &stub_len);
    else if (test_link_is_realloc_symbol(name)) emitted = test_emit_realloc_stub(text + stub_off, text_cap - stub_off, &stub_len);
    else if (test_link_is_calloc_symbol(name)) emitted = test_emit_calloc_stub(text + stub_off, text_cap - stub_off, &stub_len);
    else
      TEST_AR_FAIL;
    if (!emitted || stub_len == 0 || !test_link_add_def(defs, &def_count, name, stub_off))
      TEST_AR_FAIL;
    linked_text_len = stub_off + stub_len;
  }
  for (size_t i = 0; i < max_rela; ++i) {
    ny_test_elf64_rela_t *r = (ny_test_elf64_rela_t *)(void *)(obj + sh[rela_i].sh_offset) + i;
    uint32_t type = (uint32_t)(r->r_info & 0xffffffffu);
    uint32_t si = (uint32_t)(r->r_info >> 32);
    if ((type != 2 && type != 4) || si >= sym_count ||
        r->r_offset + 4 > text_len || sym[si].st_name >= strtab_len) TEST_AR_FAIL;
    const char *name = strtab + sym[si].st_name;
    int di = test_link_sym_index(defs, def_count, name);
    if (di < 0) TEST_AR_FAIL;
    uint64_t saddr = base + defs[di].off;
    uint64_t paddr = base + r->r_offset;
    int64_t val = (int64_t)saddr + r->r_addend - (int64_t)paddr;
    if (val < INT32_MIN || val > INT32_MAX) TEST_AR_FAIL;
    int32_t v32 = (int32_t)val;
    memcpy(text + r->r_offset, &v32, 4);
  }
  size_t harness_off = (linked_text_len + 15u) & ~15u;
  if (harness_off + 256 > text_cap) {
    size_t new_cap = text_cap;
    while (new_cap < harness_off + 4096) new_cap *= 2;
    unsigned char *new_text = (unsigned char *)realloc(text, new_cap);
    if (!new_text) TEST_AR_FAIL;
    text = new_text;
    text_cap = new_cap;
  }
  memset(text + linked_text_len, 0x90, harness_off - linked_text_len);
  size_t harness_len = test_emit_harness(text + harness_off, text_cap - harness_off,
                                         ret_kind, expected, base, harness_off, rt_main_off);
  if (harness_len == 0) TEST_AR_FAIL;
  size_t code_len = harness_off + harness_len;
  size_t file_off = 0x1000;
  size_t file_len = file_off + code_len;
  unsigned char *exe = (unsigned char *)calloc(1, file_len);
  if (!exe) TEST_AR_FAIL;
  ny_test_elf64_ehdr_t oh = {0};
  oh.e_ident[0] = 0x7f; oh.e_ident[1] = 'E'; oh.e_ident[2] = 'L'; oh.e_ident[3] = 'F';
  oh.e_ident[4] = 2; oh.e_ident[5] = 1; oh.e_ident[6] = 1;
  oh.e_type = 2; oh.e_machine = 62; oh.e_version = 1;
  oh.e_entry = base + harness_off; oh.e_phoff = sizeof(oh);
  oh.e_ehsize = sizeof(oh); oh.e_phentsize = sizeof(ny_test_elf64_phdr_t); oh.e_phnum = 1;
  ny_test_elf64_phdr_t ph = {0};
  ph.p_type = 1; ph.p_flags = 5; ph.p_offset = file_off; ph.p_vaddr = base;
  ph.p_paddr = base; ph.p_filesz = code_len; ph.p_memsz = code_len; ph.p_align = 0x1000;
  memcpy(exe, &oh, sizeof(oh));
  memcpy(exe + oh.e_phoff, &ph, sizeof(ph));
  memcpy(exe + file_off, text, code_len);
  char exe_path[PATH_MAX];
  snprintf(exe_path, sizeof(exe_path), "%s/ny-internal-link-run-%ld-%ld",
           nyt_temp_dir(), (long)getpid(), (long)now_ms());
  ok = test_write_all_file(exe_path, exe, file_len);
  free(exe); free(text); free(obj); free(archive_data); test_ar_free_symtab(&ar_st);
  if (!ok) return 2;
  chmod(exe_path, 0755);
  char *run_argv[] = {exe_path, NULL};
  int rc = run_debug_argv(run_argv, 30, 0);
  if (test_env_truthy("NYTRIX_TEST_KEEP_INTERNAL_LINK"))
    fprintf(stderr, "object link/run: kept internal ELF executable %s\n", exe_path);
  else
    remove(exe_path);
  if (rc != 0) {
    fprintf(stderr, "object link/run: internal ELF linker executable failed rc=%d for %s\n",
            rc, disp_path(shape_path));
    return 1;
  }
  return 0;
#endif
}
