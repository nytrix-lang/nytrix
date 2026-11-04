/*
 * Differentially execute direct x86-64 object code and the machine-form
 * assembly path for float conditions, immediate materialization, and
 * power-of-two division. The paths use different register allocation, so
 * compare behavior rather than incidental instruction bytes.
 */
#include "code/backend/ir/interface.h"
#include "code/backend/ir/machine.h"
#include "code/backend/object/internal.h"
#include "support.h"

#include <elf.h>
#include <inttypes.h>
#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>
#include <unistd.h>
#endif

extern const char *nyr_internal_x64_disasm(const unsigned char *bytes,
                                           int64_t length, int64_t address);

static bool within(size_t length, uint64_t offset, uint64_t count) {
  return offset <= length && count <= length - (size_t)offset;
}

static bool read_function_bytes(const char *path, const char *name,
                                unsigned char **file_data,
                                const unsigned char **function,
                                size_t *function_len) {
  *file_data = NULL;
  *function = NULL;
  *function_len = 0;
  FILE *file = fopen(path, "rb");
  if (!file)
    return false;
  bool ok = fseek(file, 0, SEEK_END) == 0;
  long size = ok ? ftell(file) : -1;
  ok = ok && size >= (long)sizeof(Elf64_Ehdr) && fseek(file, 0, SEEK_SET) == 0;
  unsigned char *bytes = ok ? malloc((size_t)size) : NULL;
  ok = ok && bytes && fread(bytes, 1, (size_t)size, file) == (size_t)size;
  fclose(file);
  if (!ok) {
    free(bytes);
    return false;
  }
  const Elf64_Ehdr *header = (const Elf64_Ehdr *)bytes;
  ok = memcmp(header->e_ident, ELFMAG, SELFMAG) == 0 &&
       header->e_ident[EI_CLASS] == ELFCLASS64 &&
       header->e_machine == EM_X86_64 &&
       header->e_shentsize == sizeof(Elf64_Shdr) && header->e_shnum > 0 &&
       within((size_t)size, header->e_shoff,
              (uint64_t)header->e_shnum * sizeof(Elf64_Shdr));
  const Elf64_Shdr *sections =
      ok ? (const Elf64_Shdr *)(bytes + header->e_shoff) : NULL;
  if (ok) {
    for (size_t i = 0; i < header->e_shnum && !*function; ++i) {
      if (sections[i].sh_type != SHT_SYMTAB ||
          sections[i].sh_link >= header->e_shnum ||
          sections[i].sh_entsize != sizeof(Elf64_Sym) ||
          !within((size_t)size, sections[i].sh_offset, sections[i].sh_size))
        continue;
      const Elf64_Shdr *strings = &sections[sections[i].sh_link];
      if (strings->sh_type != SHT_STRTAB ||
          !within((size_t)size, strings->sh_offset, strings->sh_size))
        continue;
      const char *strtab = (const char *)(bytes + strings->sh_offset);
      const Elf64_Sym *symbols =
          (const Elf64_Sym *)(bytes + sections[i].sh_offset);
      size_t count = (size_t)(sections[i].sh_size / sizeof(*symbols));
      for (size_t si = 0; si < count; ++si) {
        if (symbols[si].st_name >= strings->sh_size ||
            strcmp(strtab + symbols[si].st_name, name) != 0 ||
            symbols[si].st_shndx >= header->e_shnum)
          continue;
        const Elf64_Shdr *section = &sections[symbols[si].st_shndx];
        if (!within((size_t)size, section->sh_offset, section->sh_size) ||
            symbols[si].st_value > section->sh_size ||
            symbols[si].st_size > section->sh_size - symbols[si].st_value)
          continue;
        *function = bytes + section->sh_offset + symbols[si].st_value;
        *function_len = (size_t)symbols[si].st_size;
        break;
      }
    }
  }
  if (!*function) {
    free(bytes);
    return false;
  }
  *file_data = bytes;
  return true;
}

static bool write_assembly(const char *path, const nyr_writer_t *assembly) {
  FILE *file = fopen(path, "wb");
  if (!file)
    return false;
  bool ok = fwrite(assembly->data, 1, assembly->len, file) == assembly->len;
  return fclose(file) == 0 && ok;
}

static void print_bytes(const char *label, const unsigned char *bytes,
                        size_t length) {
  fprintf(stderr, "[x64-encoder-oracle] %s (%zu):", label, length);
  for (size_t i = 0; i < length; ++i)
    fprintf(stderr, " %02x", bytes[i]);
  fputc('\n', stderr);
}

static void *make_executable(const unsigned char *bytes, size_t length) {
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0 || length == 0)
    return NULL;
  size_t mapping_size = ((length + (size_t)page_size - 1) / (size_t)page_size) *
                        (size_t)page_size;
  void *mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED)
    return NULL;
  memcpy(mapping, bytes, length);
  if (mprotect(mapping, mapping_size, PROT_READ | PROT_EXEC) != 0) {
    munmap(mapping, mapping_size);
    return NULL;
  }
  return mapping;
}

static bool compare_function(const ny_native_target_info_t *target,
                             nyr_func_t *ir, const char *name, unsigned serial,
                             bool float_args, unsigned argument_count,
                             uint64_t expected) {
  nyr_mach_func_t machine = {0};
  nyr_writer_t assembly = {0};
  nyr_obj_buf_t encoded = {0}, repeated = {0};
  nyr_x64_obj_symbol_def_t definitions[1] = {0};
  nyr_x64_obj_symbol_def_t repeated_definitions[1] = {0};
  nyr_x64_obj_reloc_t relocations[16] = {0};
  nyr_x64_obj_reloc_t repeated_relocations[16] = {0};
  size_t definition_count = 0, relocation_count = 0;
  size_t repeated_definition_count = 0, repeated_relocation_count = 0;
  char err[256] = {0};
  char source_path[128], object_path[128], command[512];
  snprintf(source_path, sizeof(source_path), "/tmp/nyr-x64-oracle-%ld-%u.s",
           (long)getpid(), serial);
  snprintf(object_path, sizeof(object_path), "/tmp/nyr-x64-oracle-%ld-%u.o",
           (long)getpid(), serial);

  const nyr_backend_emitter_t *emitter =
      nyr_backend_find_emitter(NY_NATIVE_TARGET_X86_64);
  bool ok =
      emitter && emitter->emit_mach_scalar &&
      nyr_mach_lower_nir(ir, &machine, target->caps, err, sizeof(err)) &&
      emitter->emit_mach_scalar(&assembly, target, &machine, name, false, err,
                                sizeof(err)) &&
      nyr_x64_mach_append_function(&encoded, definitions, &definition_count,
                                   relocations, &relocation_count, &machine,
                                   target, name, false, err, sizeof(err)) &&
      definition_count == 1 && relocation_count == 0 &&
      nyr_x64_mach_append_function(
          &repeated, repeated_definitions, &repeated_definition_count,
          repeated_relocations, &repeated_relocation_count, &machine, target,
          name, false, err, sizeof(err)) &&
      repeated_definition_count == 1 && repeated_relocation_count == 0 &&
      definitions[0].size == repeated_definitions[0].size &&
      definitions[0].off <= encoded.len &&
      repeated_definitions[0].off <= repeated.len &&
      definitions[0].size <= encoded.len - definitions[0].off &&
      repeated_definitions[0].size <=
          repeated.len - repeated_definitions[0].off &&
      memcmp(encoded.data + definitions[0].off,
             repeated.data + repeated_definitions[0].off,
             definitions[0].size) == 0 &&
      write_assembly(source_path, &assembly);
  if (ok) {
    int n = snprintf(command, sizeof(command), "clang -c '%s' -o '%s'",
                     source_path, object_path);
    ok = n > 0 && (size_t)n < sizeof(command) && system(command) == 0;
  }
  unsigned char *object_data = NULL;
  size_t assembled_len = 0;
  const unsigned char *assembled = NULL;
  if (ok)
    ok = read_function_bytes(object_path, name, &object_data, &assembled,
                             &assembled_len);
  int symbol =
      ok ? nyr_x64_obj_def_index(definitions, definition_count, name) : -1;
  ok = ok && symbol >= 0 && definitions[symbol].off <= encoded.len &&
       definitions[symbol].size <= encoded.len - definitions[symbol].off;
  if (ok) {
    const char *listing = nyr_internal_x64_disasm(
        encoded.data + definitions[symbol].off, definitions[symbol].size, 0);
    if (strstr(listing, "\tunsupported\t")) {
      fprintf(stderr,
              "[x64-encoder-oracle] %s contains an undecoded emitted form:\n%s",
              name, listing);
      ok = false;
    }
  }
  void *encoded_exec =
      ok ? make_executable(encoded.data + definitions[symbol].off,
                           definitions[symbol].size)
         : NULL;
  void *assembled_exec = ok ? make_executable(assembled, assembled_len) : NULL;
  ok = ok && encoded_exec && assembled_exec;
  uint64_t encoded_result = 0, assembled_result = 0;
  if (ok && float_args) {
    typedef int64_t (*float_function_t)(double, double);
    float_function_t encoded_function = (float_function_t)encoded_exec;
    float_function_t assembled_function = (float_function_t)assembled_exec;
    encoded_result = (uint64_t)encoded_function(2.5, 3.0);
    assembled_result = (uint64_t)assembled_function(2.5, 3.0);
    ok = encoded_result == expected && assembled_result == expected;
  } else if (ok && argument_count == 1) {
    typedef int64_t (*unary_function_t)(int64_t);
    unary_function_t encoded_function = (unary_function_t)encoded_exec;
    unary_function_t assembled_function = (unary_function_t)assembled_exec;
    encoded_result = (uint64_t)encoded_function(37);
    assembled_result = (uint64_t)assembled_function(37);
    ok = encoded_result == expected && assembled_result == expected;
    if (ok && strcmp(name, "return_add") == 0)
      ok = encoded_function(-1) == 0 && assembled_function(-1) == 0 &&
           encoded_function(INT64_MAX) == INT64_MIN &&
           assembled_function(INT64_MAX) == INT64_MIN &&
           encoded_function(INT64_MIN) == INT64_MIN + 1 &&
           assembled_function(INT64_MIN) == INT64_MIN + 1;
  } else if (ok) {
    typedef int64_t (*binary_function_t)(int64_t, int64_t);
    binary_function_t encoded_function = (binary_function_t)encoded_exec;
    binary_function_t assembled_function = (binary_function_t)assembled_exec;
    encoded_result = (uint64_t)encoded_function(37, -11);
    assembled_result = (uint64_t)assembled_function(37, -11);
    ok = encoded_result == expected && assembled_result == expected;
  }
  if (!ok) {
    fprintf(stderr, "[x64-encoder-oracle] %s: %s\n", name,
            err[0] ? err : "machine paths disagree or returned wrong value");
    if (encoded_exec && assembled_exec)
      fprintf(stderr,
              "[x64-encoder-oracle] expected=%" PRIu64 " encoded=%" PRIu64
              " assembled=%" PRIu64 "\n",
              expected, encoded_result, assembled_result);
    if (symbol >= 0 && definitions[symbol].off <= encoded.len &&
        definitions[symbol].size <= encoded.len - definitions[symbol].off)
      print_bytes("encoder", encoded.data + definitions[symbol].off,
                  definitions[symbol].size);
    if (assembled)
      print_bytes("assembler", assembled, assembled_len);
    if (assembly.data)
      fprintf(stderr, "[x64-encoder-oracle] assembly:\n%s\n", assembly.data);
  }
  if (encoded_exec)
    munmap(encoded_exec,
           ((definitions[symbol].size + (size_t)sysconf(_SC_PAGESIZE) - 1) /
            (size_t)sysconf(_SC_PAGESIZE)) *
               (size_t)sysconf(_SC_PAGESIZE));
  if (assembled_exec)
    munmap(assembled_exec,
           ((assembled_len + (size_t)sysconf(_SC_PAGESIZE) - 1) /
            (size_t)sysconf(_SC_PAGESIZE)) *
               (size_t)sysconf(_SC_PAGESIZE));

  free(object_data);
  free(assembly.data);
  nyr_obj_free(&encoded);
  nyr_obj_free(&repeated);
  nyr_mach_func_free(&machine);
  nyr_ir_func_free(ir);
  unlink(source_path);
  unlink(object_path);
  return ok;
}

static bool make_immediate(nyr_func_t *ir, int64_t value) {
  nyr_opt_begin(ir);
  ir->param_count = 2;
  ir->param_types = malloc(2 * sizeof(*ir->param_types));
  if (!ir->param_types)
    return false;
  ir->param_types[0] = ir->param_types[1] = NYIR_PARAM_I64;
  int lhs = nyr_opt_emit_imm(ir, NYIR_LOAD_LOCAL, 0);
  int rhs = nyr_opt_emit_imm(ir, NYIR_LOAD_LOCAL, 1);
  int constant = nyr_opt_const(ir, value);
  int sum = lhs < 0 || constant < 0
                ? -1
                : nyr_opt_binop(ir, NYIR_ADD_I64, lhs, constant);
  int result =
      sum < 0 || rhs < 0 ? -1 : nyr_opt_binop(ir, NYIR_XOR_I64, sum, rhs);
  nyr_opt_ret(ir, result);
  return result >= 0 && nyr_opt_verify(ir, "x64-encoder-immediate");
}

static bool make_division(nyr_func_t *ir, nyr_op_t op, int64_t divisor) {
  nyr_opt_begin(ir);
  ir->param_count = 1;
  ir->param_types = malloc(sizeof(*ir->param_types));
  if (!ir->param_types)
    return false;
  ir->param_types[0] = NYIR_PARAM_I64;
  int input = nyr_opt_emit_imm(ir, NYIR_LOAD_LOCAL, 0);
  int constant = nyr_opt_const(ir, divisor);
  int result =
      input < 0 || constant < 0 ? -1 : nyr_opt_binop(ir, op, input, constant);
  nyr_opt_ret(ir, result);
  return result >= 0 && nyr_opt_verify(ir, "x64-encoder-pow2-div");
}

static bool make_return_add(nyr_func_t *ir) {
  nyr_opt_begin(ir);
  ir->param_count = 1;
  ir->param_types = malloc(sizeof(*ir->param_types));
  if (!ir->param_types)
    return false;
  ir->param_types[0] = NYIR_PARAM_I64;
  int input = nyr_opt_emit_imm(ir, NYIR_LOAD_LOCAL, 0);
  int one = nyr_opt_const(ir, 1);
  int result =
      input < 0 || one < 0 ? -1 : nyr_opt_binop(ir, NYIR_ADD_I64, input, one);
  if (result < 0)
    return false;
  nyr_opt_ret(ir, result);
  return nyr_opt_verify(ir, "x64-encoder-return-add");
}

static bool make_float_compare(nyr_func_t *ir, nyr_cmp_t condition) {
  nyr_opt_begin(ir);
  ir->param_count = 2;
  ir->param_types = malloc(2 * sizeof(*ir->param_types));
  if (!ir->param_types)
    return false;
  ir->param_types[0] = ir->param_types[1] = NYIR_PARAM_F64;
  int lhs = nyr_opt_emit_imm(ir, NYIR_LOAD_LOCAL, 0);
  int rhs = nyr_opt_emit_imm(ir, NYIR_LOAD_LOCAL, 1);
  int result = lhs < 0 || rhs < 0
                   ? -1
                   : nyr_ir_emit(ir, (nyr_inst_t){.op = NYIR_CMP_F64,
                                                  .dst = -1,
                                                  .a = lhs,
                                                  .b = rhs,
                                                  .cmp = condition});
  nyr_opt_ret(ir, result);
  return result >= 0 && nyr_opt_verify(ir, "x64-encoder-float-condition");
}

int main(void) {
#if !defined(__linux__) || !defined(__x86_64__)
  return 0;
#else
  if (system("command -v clang >/dev/null 2>&1") != 0) {
    fprintf(stderr, "[x64-encoder-oracle] clang unavailable; skipping\n");
    return 0;
  }
  const nyr_target_request_t request = nyr_target_request_make(
      NY_NATIVE_BACKEND_X86_64, NY_NATIVE_ABI_SYSV, "x86_64-linux-gnu", false);
  ny_native_target_info_t target = {0};
  bool ok = nyr_target_info_init(&target, &request);
  unsigned serial = 0;
  const int64_t immediates[] = {0,         1,         -1,       127,
                                128,       -128,      -129,     INT32_MIN,
                                INT32_MAX, INT64_MIN, INT64_MAX};
  for (size_t i = 0; ok && i < sizeof(immediates) / sizeof(immediates[0]);
       ++i) {
    nyr_func_t ir;
    ok = make_immediate(&ir, immediates[i]);
    char name[32];
    snprintf(name, sizeof(name), "imm_%u", serial);
    if (ok)
      ok = compare_function(
          &target, &ir, name, serial++, false, 2,
          (((uint64_t)37 + (uint64_t)immediates[i]) ^ (uint64_t)-11));
    else
      nyr_ir_func_free(&ir);
  }
  const struct {
    nyr_op_t op;
    int64_t divisor;
  } divisions[] = {{NYIR_DIV_I64, 2}, {NYIR_DIV_I64, 8},  {NYIR_DIV_I64, -8},
                   {NYIR_MOD_I64, 2}, {NYIR_MOD_I64, 16}, {NYIR_MOD_I64, -16}};
  for (size_t i = 0; ok && i < sizeof(divisions) / sizeof(divisions[0]); ++i) {
    nyr_func_t ir;
    ok = make_division(&ir, divisions[i].op, divisions[i].divisor);
    char name[32];
    snprintf(name, sizeof(name), "pow2_%u", serial);
    if (ok)
      ok = compare_function(&target, &ir, name, serial++, false, 1,
                            (uint64_t)(divisions[i].op == NYIR_DIV_I64
                                           ? 37 / divisions[i].divisor
                                           : 37 % divisions[i].divisor));
    else
      nyr_ir_func_free(&ir);
  }
  if (ok) {
    nyr_func_t ir;
    ok = make_return_add(&ir);
    if (ok)
      ok = compare_function(&target, &ir, "return_add", serial++, false, 1, 38);
    else
      nyr_ir_func_free(&ir);
  }
  const nyr_cmp_t conditions[] = {NYIR_CMP_EQ, NYIR_CMP_NE, NYIR_CMP_LT,
                                  NYIR_CMP_LE, NYIR_CMP_GT, NYIR_CMP_GE};
  for (size_t i = 0; ok && i < sizeof(conditions) / sizeof(conditions[0]);
       ++i) {
    nyr_func_t ir;
    ok = make_float_compare(&ir, conditions[i]);
    char name[32];
    snprintf(name, sizeof(name), "fcmp_%u", serial);
    if (ok) {
      bool result = false;
      switch (conditions[i]) {
      case NYIR_CMP_EQ:
        result = 2.5 == 3.0;
        break;
      case NYIR_CMP_NE:
        result = 2.5 != 3.0;
        break;
      case NYIR_CMP_LT:
        result = 2.5 < 3.0;
        break;
      case NYIR_CMP_LE:
        result = 2.5 <= 3.0;
        break;
      case NYIR_CMP_GT:
        result = 2.5 > 3.0;
        break;
      case NYIR_CMP_GE:
        result = 2.5 >= 3.0;
        break;
      default:
        ok = false;
        break;
      }
      if (ok)
        ok = compare_function(&target, &ir, name, serial++, true, 2, result);
    } else
      nyr_ir_func_free(&ir);
  }
  return ok ? 0 : 1;
#endif
}
