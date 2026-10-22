#include <nyr/nyr.h>

#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static bool write_duplicate_member_archive(const char *path,
                                           const char *object_path) {
  FILE *object = fopen(object_path, "rb");
  if (!object)
    return false;
  bool ok = fseek(object, 0, SEEK_END) == 0;
  long end = ok ? ftell(object) : -1;
  ok = ok && end >= 0 && fseek(object, 0, SEEK_SET) == 0;
  unsigned char *bytes = ok ? malloc((size_t)end) : NULL;
  ok = ok && bytes && fread(bytes, 1, (size_t)end, object) == (size_t)end;
  fclose(object);
  if (!ok) {
    free(bytes);
    return false;
  }
  static const char magic[] = "!<arch>\n";
  FILE *archive = fopen(path, "wb");
  ok = archive &&
       fwrite(magic, 1, sizeof(magic) - 1, archive) == sizeof(magic) - 1;
  for (size_t i = 0; ok && i < 2; ++i) {
    char header[60];
    memset(header, ' ', sizeof(header));
    int name_len = snprintf(header, 16, "helper%zu.o/", i);
    memcpy(header + 16, "0", 1);
    memcpy(header + 28, "0", 1);
    memcpy(header + 34, "0", 1);
    memcpy(header + 40, "100644", 6);
    char size_field[32];
    int size_len = snprintf(size_field, sizeof(size_field), "%ld", end);
    if (name_len <= 0 || name_len >= 16 || size_len <= 0 || size_len >= 10) {
      ok = false;
      break;
    }
    memcpy(header + 48, size_field, (size_t)size_len);
    memcpy(header + 58, "`\n", 2);
    ok = fwrite(header, 1, sizeof(header), archive) == sizeof(header) &&
         fwrite(bytes, 1, (size_t)end, archive) == (size_t)end;
    if (ok && (end & 1))
      ok = fputc('\n', archive) != EOF;
  }
  if (archive && fclose(archive) != 0)
    ok = false;
  free(bytes);
  if (!ok)
    remove(path);
  return ok;
}

static bool mark_undefined_symbol_weak(const char *object_path,
                                       const char *symbol_name) {
  FILE *file = fopen(object_path, "r+b");
  if (!file)
    return false;
  bool ok = fseek(file, 0, SEEK_END) == 0;
  long end = ok ? ftell(file) : -1;
  ok = ok && end >= (long)sizeof(Elf64_Ehdr) && fseek(file, 0, SEEK_SET) == 0;
  unsigned char *bytes = ok ? malloc((size_t)end) : NULL;
  ok = ok && bytes && fread(bytes, 1, (size_t)end, file) == (size_t)end;
  bool changed = false;
  if (ok) {
    Elf64_Ehdr eh;
    memcpy(&eh, bytes, sizeof(eh));
    ok =
        eh.e_ident[EI_CLASS] == ELFCLASS64 &&
        eh.e_ident[EI_DATA] == ELFDATA2LSB &&
        eh.e_shentsize == sizeof(Elf64_Shdr) && eh.e_shnum &&
        eh.e_shstrndx < eh.e_shnum && eh.e_shoff <= (uint64_t)end &&
        (uint64_t)eh.e_shnum * sizeof(Elf64_Shdr) <= (uint64_t)end - eh.e_shoff;
    for (size_t i = 0; ok && i < eh.e_shnum; ++i) {
      Elf64_Shdr sh;
      memcpy(&sh, bytes + eh.e_shoff + i * sizeof(sh), sizeof(sh));
      if (sh.sh_type != SHT_SYMTAB)
        continue;
      ok = sh.sh_entsize == sizeof(Elf64_Sym) &&
           sh.sh_size % sizeof(Elf64_Sym) == 0 && sh.sh_link < eh.e_shnum &&
           sh.sh_offset <= (uint64_t)end &&
           sh.sh_size <= (uint64_t)end - sh.sh_offset;
      if (!ok)
        break;
      Elf64_Shdr strings;
      memcpy(&strings, bytes + eh.e_shoff + sh.sh_link * sizeof(strings),
             sizeof(strings));
      ok = strings.sh_type == SHT_STRTAB &&
           strings.sh_offset <= (uint64_t)end &&
           strings.sh_size <= (uint64_t)end - strings.sh_offset;
      if (!ok)
        break;
      for (size_t si = 0; si < sh.sh_size / sizeof(Elf64_Sym); ++si) {
        Elf64_Sym sym;
        size_t offset = (size_t)sh.sh_offset + si * sizeof(sym);
        memcpy(&sym, bytes + offset, sizeof(sym));
        if (sym.st_name >= strings.sh_size ||
            !memchr(bytes + strings.sh_offset + sym.st_name, 0,
                    strings.sh_size - sym.st_name)) {
          ok = false;
          break;
        }
        const char *name =
            (const char *)(bytes + strings.sh_offset + sym.st_name);
        if (sym.st_shndx == SHN_UNDEF && !strcmp(name, symbol_name)) {
          sym.st_info = ELF64_ST_INFO(STB_WEAK, ELF64_ST_TYPE(sym.st_info));
          memcpy(bytes + offset, &sym, sizeof(sym));
          changed = true;
          break;
        }
      }
      break;
    }
    ok = ok && changed && fseek(file, 0, SEEK_SET) == 0 &&
         fwrite(bytes, 1, (size_t)end, file) == (size_t)end &&
         fflush(file) == 0;
  }
  free(bytes);
  if (fclose(file) != 0)
    ok = false;
  return ok;
}

static bool test_memory_emitters(char *error, size_t error_len) {
  nyr_func_t function;
  nyr_func_init(&function);
  int address = -1, value = -1, local = -1, loaded = -1;
  bool ok = nyr_emit_const_i64(&function, 0, &address, error, error_len) &&
            nyr_emit_const_i64(&function, 42, &value, error, error_len) &&
            nyr_emit_store_local(&function, 0, value, error, error_len) &&
            nyr_emit_load_local(&function, 0, &local, error, error_len) &&
            nyr_emit_store_i64(&function, address, local, 8, error,
                               error_len) &&
            nyr_emit_load_i64(&function, address, 8, &loaded, error,
                              error_len) &&
            nyr_emit_return(&function, loaded, error, error_len) &&
            nyr_verify(&function, error, error_len);
  ok = ok && function.len == 7 &&
       function.data[2].op == NYIR_STORE_LOCAL &&
       function.data[3].op == NYIR_LOAD_LOCAL &&
       function.data[4].op == NYIR_STORE_I64 && function.data[4].imm == 8 &&
       function.data[5].op == NYIR_LOAD_I64 && function.data[5].imm == 8;
  if (ok &&
      (nyr_emit_load_local(&function, -1, &loaded, error, error_len) ||
       !error[0]))
    ok = false;
  nyr_func_destroy(&function);
  nyr_func_t *allocated = nyr_func_create();
  if (!allocated)
    return false;
  int call_result = -1;
  bool external_call =
      nyr_emit_extern_call(allocated, "getchar", NULL, 0,
                           NYIR_SCALAR_I64_RAW, &call_result, error,
                           error_len) &&
      allocated->len == 1 &&
      (allocated->data[0].flags & NYIR_INST_F_EXTERN) != 0;
  nyr_func_free(allocated);
  return ok && external_call;
}

int main(void) {
  char error[256] = {0};
  if (!test_memory_emitters(error, sizeof(error))) {
    fprintf(stderr, "backend NYIR memory emitter helpers failed: %s\n",
            error[0] ? error : "instruction contract mismatch");
    return 1;
  }
  nyr_target_t host = {0}, cross = {0};
  if (!nyr_host_target(&host, error, sizeof(error)) ||
      !nyr_target_query("aarch64-unknown-linux-gnu", &cross, error,
                        sizeof(error)) ||
      !(host.capabilities & NYR_CAP_ELF_OBJECT) ||
      !(cross.capabilities & NYR_CAP_ELF_OBJECT) || cross.pointer_bits != 64) {
    fprintf(stderr, "backend NYIR target query failed: %s\n",
            error[0] ? error : "missing target capabilities");
    return 1;
  }
  if (nyr_target_query("unknown-test-target", &cross, error, sizeof(error)) ||
      !error[0]) {
    fprintf(stderr, "backend NYIR accepted an unknown target triple\n");
    return 1;
  }
  if (nyr_target_query("x86_64bogus-linux-gnu", &cross, error, sizeof(error)) ||
      !error[0]) {
    fprintf(stderr, "backend NYIR accepted a malformed target architecture\n");
    return 1;
  }
  static const char *const malformed_targets[] = {
      "wasm96-unknown-unknown",   "riscvfoo-unknown-linux-gnu",
      "bpfwhatever-unknown-none", "armbogus-unknown-linux-gnueabi",
      "amdgcnfoo-amd-amdhsa",     "x86_64--linux-gnu"};
  for (size_t i = 0;
       i < sizeof(malformed_targets) / sizeof(malformed_targets[0]); ++i) {
    if (nyr_target_query(malformed_targets[i], &cross, error, sizeof(error)) ||
        !error[0]) {
      fprintf(stderr, "backend NYIR accepted malformed target: %s\n",
              malformed_targets[i]);
      return 1;
    }
  }
  if (!nyr_target_query("riscv32-unknown-linux-gnu", &cross, error,
                        sizeof(error)) ||
      cross.pointer_bits != 32 || !(cross.capabilities & NYR_CAP_VM) ||
      (cross.capabilities & NYR_CAP_ELF_OBJECT) ||
      !nyr_target_query("wasm64-unknown-unknown", &cross, error,
                        sizeof(error)) ||
      cross.pointer_bits != 64 || !(cross.capabilities & NYR_CAP_VM) ||
      (cross.capabilities & NYR_CAP_ELF_OBJECT)) {
    fprintf(stderr,
            "backend NYIR reported incorrect portable target metadata: %s\n",
            error[0] ? error : "unexpected pointer width or capabilities");
    return 1;
  }
  if (!nyr_target_query("riscv64gc-unknown-linux-gnu", &cross, error,
                        sizeof(error)) ||
      cross.pointer_bits != 64 ||
      !nyr_target_query("armv7-unknown-linux-gnueabihf", &cross, error,
                        sizeof(error)) ||
      cross.pointer_bits != 32 ||
      !nyr_target_query("mips64el-unknown-linux-gnu", &cross, error,
                        sizeof(error)) ||
      cross.pointer_bits != 64 ||
      !nyr_target_query("ppc64le-unknown-linux-gnu", &cross, error,
                        sizeof(error)) ||
      cross.pointer_bits != 64) {
    fprintf(stderr, "backend NYIR rejected a supported target variant: %s\n",
            error[0] ? error : "unexpected target pointer width");
    return 1;
  }

  nyr_func_t function;
  nyr_func_init(&function);
  function.len = SIZE_MAX;
  function.cap = SIZE_MAX;
  int overflow_value = -1;
  if (nyr_emit_const_i64(&function, 1, &overflow_value, error, sizeof(error)) ||
      !error[0]) {
    fprintf(stderr,
            "backend NYIR accepted an overflowing instruction capacity\n");
    function.len = 0;
    function.cap = 0;
    nyr_func_destroy(&function);
    return 1;
  }
  function.len = 0;
  function.cap = 0;
  nyr_func_destroy(&function);

  nyr_func_t oversized_source = {.len = SIZE_MAX, .cap = SIZE_MAX};
  nyr_func_t rejected_clone;
  nyr_func_init(&rejected_clone);
  if (nyr_func_clone(&oversized_source, &rejected_clone, error,
                     sizeof(error)) ||
      rejected_clone.len || rejected_clone.data) {
    fprintf(stderr, "backend NYIR cloned an overflowing function length\n");
    nyr_func_destroy(&rejected_clone);
    return 1;
  }
  nyr_func_destroy(&rejected_clone);

  nyr_func_init(&function);
  int left = -1, right = -1, value = -1;
  if (!nyr_emit_const_i64(&function, 19, &left, error, sizeof(error)) ||
      !nyr_emit_const_i64(&function, 23, &right, error, sizeof(error)) ||
      !nyr_emit_binary(&function, NYIR_ADD_I64, left, right,
                       NYIR_SCALAR_I64_RAW, &value, error, sizeof(error)) ||
      !nyr_emit_return(&function, value, error, sizeof(error)) ||
      !nyr_verify(&function, error, sizeof(error))) {
    fprintf(stderr, "backend NYIR acceptance failed: %s\n",
            error[0] ? error : "invalid function result");
    nyr_func_destroy(&function);
    return 1;
  }
  size_t original_len = function.len;
  if (nyr_optimize(&function, (nyr_opt_level_t)99, error, sizeof(error)) ||
      function.len != original_len || !error[0] ||
      !nyr_verify(&function, error, sizeof(error))) {
    fprintf(stderr, "backend NYIR invalid optimization level changed IR: %s\n",
            error[0] ? error : "expected rejection without mutation");
    nyr_func_destroy(&function);
    return 1;
  }
  if (!nyr_optimize(&function, NYR_OPT_O2, error, sizeof(error)) ||
      !nyr_verify(&function, error, sizeof(error))) {
    fprintf(stderr, "backend NYIR optimization failed: %s\n",
            error[0] ? error : "optimized function did not verify");
    nyr_func_destroy(&function);
    return 1;
  }
  char object_path[128];
  snprintf(object_path, sizeof(object_path), "/tmp/nyr-public-%ld.o",
           (long)getpid());
  if (!nyr_emit_object(&function, object_path, "nyr_acceptance", error,
                       sizeof(error))) {
    fprintf(stderr, "backend NYIR object emission failed: %s\n", error);
    nyr_func_destroy(&function);
    return 1;
  }
  char assembly_path[128];
  snprintf(assembly_path, sizeof(assembly_path), "/tmp/nyr-public-%ld.s",
           (long)getpid());
  if (!nyr_emit_assembly(&function, assembly_path, "nyr_acceptance", error,
                         sizeof(error)) || access(assembly_path, R_OK) != 0) {
    fprintf(stderr, "backend NYIR assembly emission failed: %s\n", error);
    remove(object_path);
    remove(assembly_path);
    nyr_func_destroy(&function);
    return 1;
  }
  int64_t jit_result = -1;
  if (!nyr_jit_run(&function, &jit_result, error, sizeof(error)) ||
      jit_result != 42) {
    fprintf(stderr, "backend NYIR in-process JIT failed: %s\n",
            error[0] ? error : "wrong function result");
    remove(object_path);
    remove(assembly_path);
    nyr_func_destroy(&function);
    return 1;
  }
  remove(assembly_path);
  char executable_path[128];
  snprintf(executable_path, sizeof(executable_path), "/tmp/nyr-public-exe-%ld",
           (long)getpid());
  if (!nyr_link_elf64_executable(object_path, "nyr_acceptance", executable_path,
                                 error, sizeof(error))) {
    fprintf(stderr, "backend NYIR internal executable link failed: %s\n",
            error);
    remove(object_path);
    nyr_func_destroy(&function);
    return 1;
  }
  pid_t child = fork();
  if (child == 0) {
    execl(executable_path, executable_path, (char *)NULL);
    _exit(127);
  }
  int child_status = 0;
  bool executable_ok = child > 0 && waitpid(child, &child_status, 0) == child &&
                       WIFEXITED(child_status) &&
                       WEXITSTATUS(child_status) == 42;
  remove(executable_path);
  if (!executable_ok) {
    fprintf(stderr,
            "backend NYIR internal executable returned the wrong value\n");
    remove(object_path);
    nyr_func_destroy(&function);
    return 1;
  }
  char generic_executable_path[128];
  snprintf(generic_executable_path, sizeof(generic_executable_path),
           "/tmp/nyr-link-api-%ld", (long)getpid());
  const char *generic_inputs[] = {object_path};
  nyr_link_request_t link_request = {
      .input_paths = generic_inputs,
      .input_count = 1,
      .output_path = generic_executable_path,
      .target_triple = NULL,
      .entry_symbol = "nyr_acceptance",
      .kind = NYR_LINK_EXECUTABLE,
  };
  bool generic_linked = nyr_link(&link_request, error, sizeof(error));
  if (generic_linked) {
    pid_t generic_child = fork();
    if (generic_child == 0) {
      execl(generic_executable_path, generic_executable_path, (char *)NULL);
      _exit(127);
    }
    int generic_status = 0;
    generic_linked = generic_child > 0 &&
                     waitpid(generic_child, &generic_status, 0) == generic_child &&
                     WIFEXITED(generic_status) &&
                     WEXITSTATUS(generic_status) == 42;
  }
  remove(generic_executable_path);
  if (!generic_linked) {
    fprintf(stderr, "backend NYR generic linker API failed: %s\n",
            error[0] ? error : "linked executable returned the wrong value");
    remove(object_path);
    nyr_func_destroy(&function);
    return 1;
  }
  remove(object_path);
  nyr_func_destroy(&function);

  nyr_func_t caller, callee;
  nyr_func_init(&caller);
  nyr_func_init(&callee);
  int call_value = -1, callee_value = -1;
  char caller_object[128], callee_object[128];
  snprintf(caller_object, sizeof(caller_object), "/tmp/nyr-caller-%ld.o",
           (long)getpid());
  snprintf(callee_object, sizeof(callee_object), "/tmp/nyr-callee-%ld.o",
           (long)getpid());
  const char *link_inputs[] = {caller_object, callee_object};
  snprintf(executable_path, sizeof(executable_path), "/tmp/nyr-many-%ld",
           (long)getpid());
  bool many_built =
      nyr_emit_call(&caller, "nyr_helper", NULL, 0, NYIR_SCALAR_I64_RAW,
                    &call_value, error, sizeof(error)) &&
      nyr_emit_return(&caller, call_value, error, sizeof(error)) &&
      nyr_emit_const_i64(&callee, 42, &callee_value, error, sizeof(error)) &&
      nyr_emit_return(&callee, callee_value, error, sizeof(error)) &&
      nyr_verify(&caller, error, sizeof(error)) &&
      nyr_verify(&callee, error, sizeof(error)) &&
      nyr_emit_object(&caller, caller_object, "nyr_entry", error,
                      sizeof(error)) &&
      nyr_emit_object(&callee, callee_object, "ny_fn_nyr_helper", error,
                      sizeof(error)) &&
      nyr_link_elf64_executable_objects(link_inputs, 2, "nyr_entry",
                                        executable_path, error, sizeof(error));
  if (many_built) {
    pid_t many_child = fork();
    if (many_child == 0) {
      execl(executable_path, executable_path, (char *)NULL);
      _exit(127);
    }
    int many_status = 0;
    many_built = many_child > 0 &&
                 waitpid(many_child, &many_status, 0) == many_child &&
                 WIFEXITED(many_status) && WEXITSTATUS(many_status) == 42;
  }
  char archive_path[128], archive_output[128];
  snprintf(archive_path, sizeof(archive_path), "/tmp/nyr-helpers-%ld.a",
           (long)getpid());
  snprintf(archive_output, sizeof(archive_output), "/tmp/nyr-archive-%ld",
           (long)getpid());
  const char *archive_inputs[] = {caller_object, archive_path};
  bool archive_built =
      many_built &&
      write_duplicate_member_archive(archive_path, callee_object) &&
      nyr_link_elf64_executable_objects(archive_inputs, 2, "nyr_entry",
                                        archive_output, error, sizeof(error));
  if (archive_built) {
    pid_t archive_child = fork();
    if (archive_child == 0) {
      execl(archive_output, archive_output, (char *)NULL);
      _exit(127);
    }
    int archive_status = 0;
    archive_built =
        archive_child > 0 &&
        waitpid(archive_child, &archive_status, 0) == archive_child &&
        WIFEXITED(archive_status) && WEXITSTATUS(archive_status) == 42;
  }
  char weak_caller_object[128], weak_callee_object[128];
  char weak_archive_path[128], weak_archive_output[128];
  snprintf(weak_caller_object, sizeof(weak_caller_object),
           "/tmp/nyr-weak-caller-%ld.o", (long)getpid());
  snprintf(weak_callee_object, sizeof(weak_callee_object),
           "/tmp/nyr-weak-callee-%ld.o", (long)getpid());
  snprintf(weak_archive_path, sizeof(weak_archive_path),
           "/tmp/nyr-weak-helpers-%ld.a", (long)getpid());
  snprintf(weak_archive_output, sizeof(weak_archive_output),
           "/tmp/nyr-weak-archive-%ld", (long)getpid());
  nyr_func_t weak_callee;
  nyr_func_init(&weak_callee);
  int missing_call = -1;
  bool weak_archive_built =
      nyr_emit_call(&weak_callee, "host_missing", NULL, 0, NYIR_SCALAR_I64_RAW,
                    &missing_call, error, sizeof(error)) &&
      nyr_emit_return(&weak_callee, missing_call, error, sizeof(error)) &&
      nyr_emit_object(&caller, weak_caller_object, "nyr_entry", error,
                      sizeof(error)) &&
      mark_undefined_symbol_weak(weak_caller_object, "ny_fn_nyr_helper") &&
      nyr_emit_object(&weak_callee, weak_callee_object, "ny_fn_nyr_helper",
                      error, sizeof(error)) &&
      write_duplicate_member_archive(weak_archive_path, weak_callee_object);
  const char *weak_link_inputs[] = {weak_caller_object, weak_archive_path};
  weak_archive_built =
      weak_archive_built && nyr_link_elf64_executable_objects(
                                weak_link_inputs, 2, "nyr_entry",
                                weak_archive_output, error, sizeof(error));
  remove(weak_caller_object);
  remove(weak_callee_object);
  remove(weak_archive_path);
  remove(weak_archive_output);
  nyr_func_destroy(&weak_callee);
  char duplicate_object[128], duplicate_output[128];
  snprintf(duplicate_object, sizeof(duplicate_object),
           "/tmp/nyr-duplicate-%ld.o", (long)getpid());
  snprintf(duplicate_output, sizeof(duplicate_output), "/tmp/nyr-duplicate-%ld",
           (long)getpid());
  const char *duplicate_inputs[] = {caller_object, callee_object,
                                    duplicate_object};
  bool duplicate_rejected =
      many_built &&
      nyr_emit_object(&callee, duplicate_object, "ny_fn_nyr_helper", error,
                      sizeof(error)) &&
      !nyr_link_elf64_executable_objects(duplicate_inputs, 3, "nyr_entry",
                                         duplicate_output, error,
                                         sizeof(error)) &&
      strstr(error, "duplicate strong definition") &&
      access(duplicate_output, F_OK) != 0;
  remove(caller_object);
  remove(callee_object);
  remove(archive_path);
  remove(archive_output);
  remove(duplicate_object);
  remove(executable_path);
  remove(duplicate_output);
  nyr_func_destroy(&caller);
  nyr_func_destroy(&callee);
  if (!many_built || !archive_built || !weak_archive_built ||
      !duplicate_rejected) {
    fprintf(stderr,
            "backend NYIR multi-object link failed: objects=%d archive=%d "
            "weak-archive=%d duplicate=%d: %s\n",
            many_built, archive_built, weak_archive_built, duplicate_rejected,
            error[0] ? error : "linked executable returned the wrong value");
    return 1;
  }

  nyr_func_init(&function);
  nyr_param_desc_t parameter = {.type = NYIR_PARAM_I64};
  int loaded = -1;
  nyr_inst_t load = nyr_inst_init(NYIR_LOAD_LOCAL);
  load.imm = 0;
  load.scalar_class = NYIR_SCALAR_I64_RAW;
  if (!nyr_func_set_signature(&function, &parameter, 1, 0, NULL, error,
                              sizeof(error))) {
    fprintf(stderr, "backend NYIR signature setup failed: %s\n", error);
    nyr_func_destroy(&function);
    return 1;
  }
  parameter.type = NYIR_PARAM_F64;
  if (!function.param_types || function.param_types[0] != NYIR_PARAM_I64 ||
      !nyr_emit(&function, load, &loaded, error, sizeof(error)) ||
      !nyr_emit_return(&function, loaded, error, sizeof(error)) ||
      !nyr_verify(&function, error, sizeof(error))) {
    fprintf(stderr, "backend NYIR signature builder failed: %s\n", error);
    nyr_func_destroy(&function);
    return 1;
  }
  snprintf(object_path, sizeof(object_path), "/tmp/nyr-param-%ld.o",
           (long)getpid());
  if (!nyr_emit_object(&function, object_path, "nyr_parameter", error,
                       sizeof(error))) {
    fprintf(stderr, "backend NYIR parameter object failed: %s\n", error);
    nyr_func_destroy(&function);
    return 1;
  }
  remove(object_path);
  nyr_func_destroy(&function);

  nyr_func_init(&function);
  int arguments[8];
  for (size_t i = 0; i < 8; ++i)
    if (!nyr_emit_const_i64(&function, (int64_t)i, &arguments[i], error,
                            sizeof(error))) {
      fprintf(stderr, "backend NYIR call setup failed: %s\n", error);
      nyr_func_destroy(&function);
      return 1;
    }
  int call_result = -1;
  char call_symbol[] = "host_sum8";
  if (!nyr_emit_call(&function, call_symbol, arguments, 8, NYIR_SCALAR_I64_RAW,
                     &call_result, error, sizeof(error)) ||
      function.data[8].extra_args_len != 2 ||
      function.data[8].extra_args[0] != arguments[6] ||
      function.data[8].extra_args[1] != arguments[7] ||
      function.data[8].effects !=
          (NYIR_EFFECT_CALL | NYIR_EFFECT_UNKNOWN_SIDE_EFFECT)) {
    fprintf(stderr, "backend NYIR call builder failed: %s\n",
            error[0] ? error : "stack arguments were not copied");
    nyr_func_destroy(&function);
    return 1;
  }
  call_symbol[0] = 'X';
  arguments[6] = -1;
  if (strcmp(function.data[8].symbol, "host_sum8") != 0 ||
      function.data[8].extra_args[0] < 0 ||
      !nyr_emit_return(&function, call_result, error, sizeof(error)) ||
      !nyr_verify(&function, error, sizeof(error))) {
    fprintf(stderr, "backend NYIR owned call metadata failed: %s\n",
            error[0] ? error : "call arguments changed with caller storage");
    nyr_func_destroy(&function);
    return 1;
  }
  char debug_file[] = "nyr-api-source.ny";
  function.data[0].debug.file = debug_file;
  function.data[0].debug.line = 4;
  nyr_func_t clone;
  nyr_func_init(&clone);
  if (!nyr_func_clone(&function, &clone, error, sizeof(error)) ||
      clone.data[8].extra_args == function.data[8].extra_args ||
      clone.data[8].symbol == function.data[8].symbol ||
      clone.data[0].debug.file == function.data[0].debug.file ||
      strcmp(clone.data[0].debug.file, debug_file) != 0 ||
      !nyr_verify(&clone, error, sizeof(error))) {
    fprintf(stderr, "backend NYIR function clone failed: %s\n",
            error[0] ? error : "clone did not own copied metadata");
    nyr_func_destroy(&clone);
    nyr_func_destroy(&function);
    return 1;
  }
  debug_file[0] = 'X';
  if (strcmp(clone.data[0].debug.file, "nyr-api-source.ny") != 0) {
    fprintf(stderr, "backend NYIR clone borrowed its debug filename\n");
    nyr_func_destroy(&clone);
    nyr_func_destroy(&function);
    return 1;
  }
  size_t clone_len = clone.len;
  if (nyr_func_clone(&function, &clone, error, sizeof(error)) ||
      clone.len != clone_len ||
      nyr_func_clone(&clone, &clone, error, sizeof(error)) ||
      clone.len != clone_len || !nyr_verify(&clone, error, sizeof(error))) {
    fprintf(stderr, "backend NYIR clone accepted an invalid destination: %s\n",
            error[0] ? error : "clone destination was modified");
    nyr_func_destroy(&clone);
    nyr_func_destroy(&function);
    return 1;
  }
  char unresolved_object_path[128];
  snprintf(unresolved_object_path, sizeof(unresolved_object_path),
           "/tmp/nyr-unresolved-%ld.o", (long)getpid());
  bool rejected_unresolved =
      nyr_emit_object(&function, unresolved_object_path, "nyr_call", error,
                      sizeof(error)) &&
      !nyr_link_elf64_executable(unresolved_object_path, "nyr_call",
                                 executable_path, error, sizeof(error)) &&
      strstr(error, "unresolved external symbol") &&
      access(executable_path, F_OK) != 0;
  remove(unresolved_object_path);
  remove(executable_path);
  if (!rejected_unresolved) {
    fprintf(
        stderr,
        "backend NYIR internal linker did not reject unresolved symbols: %s\n",
        error[0] ? error : "unexpected output executable");
    nyr_func_destroy(&clone);
    nyr_func_destroy(&function);
    return 1;
  }
  nyr_func_destroy(&function);
  if (!nyr_verify(&clone, error, sizeof(error))) {
    fprintf(stderr, "backend NYIR clone depended on its source: %s\n", error);
    nyr_func_destroy(&clone);
    return 1;
  }
  nyr_func_destroy(&clone);

  nyr_func_init(&function);
  nyr_inst_t known_effect_call = nyr_inst_init(NYIR_CALL);
  known_effect_call.imm = 0;
  known_effect_call.symbol = "frontend_pure_call";
  known_effect_call.flags = NYIR_INST_F_EFFECTS_KNOWN;
  known_effect_call.effects = NYIR_EFFECT_CALL;
  known_effect_call.scalar_class = NYIR_SCALAR_I64_RAW;
  int known_effect_result = -1;
  bool known_effects_preserved =
      nyr_emit(&function, known_effect_call, &known_effect_result, error,
               sizeof(error)) &&
      function.data[0].effects == NYIR_EFFECT_CALL &&
      (function.data[0].flags & NYIR_INST_F_EFFECTS_KNOWN) &&
      nyr_emit_return(&function, known_effect_result, error, sizeof(error)) &&
      nyr_verify(&function, error, sizeof(error));
  if (!known_effects_preserved) {
    fprintf(stderr, "backend NYIR call-effect summary was not preserved: %s\n",
            error[0] ? error : "call effect mask was replaced");
    nyr_func_destroy(&function);
    return 1;
  }
  nyr_func_destroy(&function);

  nyr_func_init(&function);
  int aggregate_argument = -1, aggregate_call = -1;
  uint32_t aggregate_size =
      8u | (NYIR_ARG_CLASS_INTEGER << NYIR_ARG_AGG_CLASS0_SHIFT);
  nyr_inst_t aggregate_call_inst = nyr_inst_init(NYIR_CALL);
  aggregate_call_inst.a = -1;
  aggregate_call_inst.imm = 1;
  aggregate_call_inst.symbol = "host_aggregate";
  aggregate_call_inst.arg_sizes = &aggregate_size;
  nyr_call_arg_class_set(aggregate_call_inst.arg_scalar_classes, 0,
                          NYIR_SCALAR_AGGREGATE);
  bool aggregate_metadata_copied = nyr_emit_const_i64(
      &function, 0, &aggregate_argument, error, sizeof(error));
  aggregate_call_inst.a = aggregate_argument;
  aggregate_metadata_copied = aggregate_metadata_copied &&
                              nyr_emit(&function, aggregate_call_inst,
                                       &aggregate_call, error, sizeof(error));
  aggregate_size = 0;
  aggregate_metadata_copied =
      aggregate_metadata_copied && function.data[1].arg_sizes &&
      function.data[1].arg_sizes[0] ==
          (8u | (NYIR_ARG_CLASS_INTEGER << NYIR_ARG_AGG_CLASS0_SHIFT)) &&
      nyr_emit_return(&function, aggregate_call, error, sizeof(error)) &&
      nyr_verify(&function, error, sizeof(error));
  if (!aggregate_metadata_copied) {
    fprintf(stderr, "backend NYIR aggregate metadata ownership failed: %s\n",
            error[0] ? error : "aggregate call metadata was not copied");
    nyr_func_destroy(&function);
    return 1;
  }
  nyr_func_destroy(&function);

  nyr_func_init(&function);
  int condition = -1, left_value = -1, right_value = -1;
  int merged = -1;
  nyr_phi_incoming_t incoming[2];
  bool built =
      nyr_emit_const_i64(&function, 1, &condition, error, sizeof(error)) &&
      nyr_emit_branch_if(&function, condition, 10, error, sizeof(error)) &&
      nyr_emit_branch(&function, 20, error, sizeof(error)) &&
      nyr_emit_label(&function, 10, error, sizeof(error)) &&
      nyr_emit_const_i64(&function, 10, &left_value, error, sizeof(error)) &&
      nyr_emit_branch(&function, 30, error, sizeof(error)) &&
      nyr_emit_label(&function, 20, error, sizeof(error)) &&
      nyr_emit_const_i64(&function, 20, &right_value, error, sizeof(error)) &&
      nyr_emit_branch(&function, 30, error, sizeof(error)) &&
      nyr_emit_label(&function, 30, error, sizeof(error));
  incoming[0] =
      (nyr_phi_incoming_t){.predecessor_label = 10, .value = left_value};
  incoming[1] =
      (nyr_phi_incoming_t){.predecessor_label = 20, .value = right_value};
  built = built &&
          nyr_emit_phi(&function, incoming, 2, &merged, error, sizeof(error));
  incoming[0].value = -1;
  built = built &&
          function.data[function.len - 1].phi_incoming[0].value == left_value &&
          nyr_emit_return(&function, merged, error, sizeof(error)) &&
          nyr_verify(&function, error, sizeof(error));
  if (!built) {
    fprintf(stderr, "backend NYIR PHI builder failed: %s\n", error);
    nyr_func_destroy(&function);
    return 1;
  }
  nyr_func_destroy(&function);
  return 0;
}
