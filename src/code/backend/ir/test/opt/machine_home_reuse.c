/*
 * Frame-argument homes must not alias reused temporary homes.
 */
#include "code/backend/ir/machine.h"
#include "code/backend/object/internal.h"
#include "support.h"

#include <sys/mman.h>

static bool check_literal_home_elision(const ny_native_target_info_t *target) {
  nyr_func_t ir;
  nyr_mach_func_t mach = {0};
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[1] = {0};
  nyr_x64_obj_reloc_t relocs[1] = {0};
  size_t def_count = 0, reloc_count = 0;
  char err[256] = {0};
  nyr_opt_begin(&ir);
  int one = nyr_opt_const(&ir, 1);
  int two = nyr_opt_const(&ir, 2);
  int sum = nyr_opt_binop(&ir, NYIR_ADD_I64, one, two);
  nyr_opt_ret(&ir, sum);
  bool ok = nyr_opt_verify(&ir, "machine-literal-home-elision") &&
            nyr_mach_lower_nir(&ir, &mach, target->caps, err, sizeof(err)) &&
            nyr_x64_mach_append_function(
                &code, defs, &def_count, relocs, &reloc_count, &mach, target,
                "literal_home_elision", false, err, sizeof(err));
  int symbol =
      ok ? nyr_x64_obj_def_index(defs, def_count, "literal_home_elision") : -1;
  ok = ok && symbol >= 0 && reloc_count == 0;
  size_t home_stores = 0;
  if (ok) {
    const unsigned char *bytes = code.data + defs[symbol].off;
    for (size_t i = 0; i + 2 < defs[symbol].size; ++i)
      if (bytes[i] == 0x48 && bytes[i + 1] == 0x89 && bytes[i + 2] == 0x85)
        ++home_stores;
    /*
     * The literal RHS is rematerialized. The left input may also forward
     * directly into the ADD without needing a home store.
     */
    ok = home_stores <= 1;
  }
  void *mapping = MAP_FAILED;
  if (ok) {
    mapping = mmap(NULL, code.len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ok = mapping != MAP_FAILED;
  }
  if (ok) {
    memcpy(mapping, code.data, code.len);
    ok = mprotect(mapping, code.len, PROT_READ | PROT_EXEC) == 0;
  }
  if (ok) {
    typedef int64_t (*fn_t)(void);
    fn_t fn = (fn_t)((unsigned char *)mapping + defs[symbol].off);
    ok = fn() == 3;
  }
  if (!ok)
    fprintf(stderr,
            "[machine-home-reuse] literal home elision failed: stores=%zu "
            "relocations=%zu error=%s\n",
            home_stores, reloc_count, err[0] ? err : "runtime or assertion");
  if (mapping != MAP_FAILED)
    munmap(mapping, code.len);
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  nyr_ir_func_free(&ir);
  return ok;
}

static bool
check_indexed_address_forwarding(const ny_native_target_info_t *target) {
  nyr_func_t ir;
  nyr_mach_func_t mach = {0};
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[1] = {0};
  nyr_x64_obj_reloc_t relocs[1] = {0};
  size_t def_count = 0, reloc_count = 0;
  char err[256] = {0};
  nyr_opt_begin(&ir);
  ir.param_count = 2;
  ir.param_types = malloc(2 * sizeof(*ir.param_types));
  bool ok = ir.param_types != NULL;
  if (ok)
    ir.param_types[0] = ir.param_types[1] = NYIR_PARAM_I64;
  int base = ok ? nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .c = -1,
                                               .d = -1,
                                               .e = -1,
                                               .f = -1,
                                               .imm = 0})
                : -1;
  int index = ok ? nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                                .dst = -1,
                                                .a = -1,
                                                .b = -1,
                                                .c = -1,
                                                .d = -1,
                                                .e = -1,
                                                .f = -1,
                                                .imm = 1})
                 : -1;
  int scale = ok ? nyr_opt_const(&ir, 8) : -1;
  int offset = ok ? nyr_opt_binop(&ir, NYIR_MUL_I64, index, scale) : -1;
  int address = ok ? nyr_opt_binop(&ir, NYIR_ADD_I64, base, offset) : -1;
  int loaded = ok ? nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                                 .dst = -1,
                                                 .a = address,
                                                 .b = -1,
                                                 .c = -1,
                                                 .d = -1,
                                                 .e = -1,
                                                 .f = -1})
                  : -1;
  if (ok && loaded >= 0)
    nyr_opt_ret(&ir, loaded);
  ok = ok && nyr_opt_verify(&ir, "machine-indexed-address-forwarding") &&
       nyr_mach_lower_nir(&ir, &mach, target->caps, err, sizeof(err)) &&
       nyr_x64_mach_append_function(
           &code, defs, &def_count, relocs, &reloc_count, &mach, target,
           "indexed_address_forwarding", false, err, sizeof(err));
  int symbol =
      ok ? nyr_x64_obj_def_index(defs, def_count, "indexed_address_forwarding")
         : -1;
  ok = ok && symbol >= 0 && reloc_count == 0;
  size_t lea_count = 0, indexed_load_count = 0;
  if (ok) {
    const unsigned char *bytes = code.data + defs[symbol].off;
    for (size_t i = 0; i + 3 < defs[symbol].size; ++i) {
      if (bytes[i] != 0x4a || bytes[i + 1] != 0x8d || bytes[i + 2] != 0x04 ||
          bytes[i + 3] != 0xd0) {
        if (bytes[i] == 0x4a && bytes[i + 1] == 0x8b && bytes[i + 2] == 0x04 &&
            bytes[i + 3] == 0xd0)
          ++indexed_load_count;
        continue;
      }
      ++lea_count;
    }
    ok = lea_count == 0 && indexed_load_count == 1;
  }
  void *mapping = MAP_FAILED;
  if (ok) {
    mapping = mmap(NULL, code.len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ok = mapping != MAP_FAILED;
  }
  if (ok) {
    memcpy(mapping, code.data, code.len);
    ok = mprotect(mapping, code.len, PROT_READ | PROT_EXEC) == 0;
  }
  if (ok) {
    int64_t cells[] = {11, 17, 29, 41};
    typedef int64_t (*fn_t)(int64_t, int64_t);
    fn_t fn = (fn_t)((unsigned char *)mapping + defs[symbol].off);
    ok = fn((int64_t)(intptr_t)cells, 2) == 29;
  }
  if (!ok)
    fprintf(stderr,
            "[machine-home-reuse] indexed load-address forwarding failed: "
            "leas=%zu indexed-loads=%zu relocations=%zu error=%s\n",
            lea_count, indexed_load_count, reloc_count,
            err[0] ? err : "runtime or assertion");

  if (mapping != MAP_FAILED)
    munmap(mapping, code.len);
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  nyr_ir_func_free(&ir);
  return ok;
}

static nyr_mach_operand_t home_test_vreg(nyr_mach_reg_t reg);

static bool
check_uncolored_add_address_forwarding(const ny_native_target_info_t *target) {
  nyr_mach_func_t mach = {0};
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[1] = {0};
  nyr_x64_obj_reloc_t relocs[1] = {0};
  size_t def_count = 0, reloc_count = 0;
  nyr_mach_reg_t base, index, offset, address, loaded;
  uint32_t base_slot, index_slot, entry;
  char err[256] = {0};
  bool ok =
      nyr_mach_alloc_typed_frame_slot(&mach, sizeof(int64_t), sizeof(int64_t),
                                      NYR_MACH_TYPE_I64, false, &base_slot) &&
      nyr_mach_alloc_typed_frame_slot(&mach, sizeof(int64_t), sizeof(int64_t),
                                      NYR_MACH_TYPE_I64, false, &index_slot);
  mach.param_count = ok ? 2 : 0;
  ok =
      ok && nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &base) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &index) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &offset) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &address) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &loaded) &&
      nyr_mach_begin_block(&mach, 0, &entry) && entry == 0 &&
      nyr_mach_emit(
          &mach, (nyr_mach_inst_t){.opcode = NYR_MACH_LOAD,
                                   .dst = home_test_vreg(base),
                                   .src0 = {.kind = NYR_MACH_OPERAND_FRAME,
                                            .as.frame_index = base_slot},
                                   .effects = NYR_MACH_EFFECT_READ_MEMORY}) &&
      nyr_mach_emit(
          &mach, (nyr_mach_inst_t){.opcode = NYR_MACH_LOAD,
                                   .dst = home_test_vreg(index),
                                   .src0 = {.kind = NYR_MACH_OPERAND_FRAME,
                                            .as.frame_index = index_slot},
                                   .effects = NYR_MACH_EFFECT_READ_MEMORY}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){
                        .opcode = NYR_MACH_SHL,
                        .dst = home_test_vreg(offset),
                        .src0 = home_test_vreg(index),
                        .src1 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 3}}) &&
      nyr_mach_emit(&mach, (nyr_mach_inst_t){.opcode = NYR_MACH_ADD,
                                             .dst = home_test_vreg(address),
                                             .src0 = home_test_vreg(base),
                                             .src1 = home_test_vreg(offset)}) &&
      nyr_mach_emit(
          &mach, (nyr_mach_inst_t){.opcode = NYR_MACH_LOAD,
                                   .dst = home_test_vreg(loaded),
                                   .src0 = home_test_vreg(address),
                                   .effects = NYR_MACH_EFFECT_READ_MEMORY}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){.opcode = NYR_MACH_RET,
                                      .src0 = home_test_vreg(loaded),
                                      .effects = NYR_MACH_EFFECT_CONTROL});
  /*
   * Any vector vreg selects the encoder's canonical-home mode. Keep this
   * unused so the scalar ADD/load pair specifically exercises the scratch SIB
   * forwarding for uncolored base and index values.
   */
  nyr_mach_reg_t vector_vreg = NYR_MACH_REG_INVALID;
  ok = ok &&
       nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_V128_I64, &vector_vreg) &&
       vector_vreg != NYR_MACH_REG_INVALID &&
       nyr_mach_verify(&mach, target->caps, err, sizeof(err)) &&
       nyr_x64_mach_append_function(
           &code, defs, &def_count, relocs, &reloc_count, &mach, target,
           "uncolored_add_address", false, err, sizeof(err));
  int symbol =
      ok ? nyr_x64_obj_def_index(defs, def_count, "uncolored_add_address") : -1;
  ok = ok && symbol >= 0 && reloc_count == 0;
  size_t indexed_loads = 0;
  size_t address_adds = 0;
  if (ok) {
    const unsigned char *bytes = code.data + defs[symbol].off;
    for (size_t i = 0; i + 3 < defs[symbol].size; ++i) {
      unsigned char rex = bytes[i];
      unsigned char modrm = bytes[i + 2];
      unsigned char sib = bytes[i + 3];
      if ((rex & 0xf8) == 0x48 && bytes[i + 1] == 0x8b &&
          (modrm & 0xc7) == 0x04 && ((sib >> 3) & 7) != 4)
        ++indexed_loads;
      if (i + 1 < defs[symbol].size && (rex & 0xf8) == 0x48 &&
          (bytes[i + 1] == 0x01 || bytes[i + 1] == 0x03 ||
           bytes[i + 1] == 0x05))
        ++address_adds;
    }
    ok = indexed_loads == 1 && address_adds == 0;
  }
  void *mapping = MAP_FAILED;
  if (ok) {
    mapping = mmap(NULL, code.len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ok = mapping != MAP_FAILED;
  }
  if (ok) {
    memcpy(mapping, code.data, code.len);
    ok = mprotect(mapping, code.len, PROT_READ | PROT_EXEC) == 0;
  }
  if (ok) {
    int64_t cells[] = {11, 17, 29, 41};
    typedef int64_t (*fn_t)(int64_t *, int64_t);
    fn_t fn = (fn_t)((unsigned char *)mapping + defs[symbol].off);
    ok = fn(cells, 2) == 29;
  }
  if (!ok)
    fprintf(stderr,
            "[machine-home-reuse] uncolored ADD address forwarding failed: "
            "indexed-loads=%zu address-adds=%zu relocations=%zu error=%s\n",
            indexed_loads, address_adds, reloc_count,
            err[0] ? err : "runtime or assertion");
  if (mapping != MAP_FAILED)
    munmap(mapping, code.len);
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  return ok;
}

static bool test_make_scaled_address_leas(nyr_mach_func_t *mach,
                                          size_t expected) {
  size_t converted = 0;
  for (size_t i = 0; i + 1 < mach->inst_len; ++i) {
    nyr_mach_inst_t *address = &mach->insts[i];
    const nyr_mach_inst_t *access = &mach->insts[i + 1];
    bool access_uses_address = address->dst.kind == NYR_MACH_OPERAND_VREG &&
                               ((access->opcode == NYR_MACH_LOAD &&
                                 access->src0.kind == NYR_MACH_OPERAND_VREG &&
                                 access->src0.as.reg == address->dst.as.reg) ||
                                (access->opcode == NYR_MACH_STORE &&
                                 access->dst.kind == NYR_MACH_OPERAND_VREG &&
                                 access->dst.as.reg == address->dst.as.reg));
    if (access_uses_address && address->opcode == NYR_MACH_LEA &&
        address->src0.kind == NYR_MACH_OPERAND_VREG &&
        address->src1.kind == NYR_MACH_OPERAND_VREG &&
        address->src2.kind == NYR_MACH_OPERAND_IMM &&
        address->src2.as.imm == 8) {
      ++converted;
      continue;
    }
    if (address->opcode != NYR_MACH_ADD ||
        address->src0.kind != NYR_MACH_OPERAND_VREG ||
        address->src1.kind != NYR_MACH_OPERAND_VREG || !access_uses_address)
      continue;
    for (size_t j = 0; j < mach->inst_len; ++j) {
      const nyr_mach_inst_t *mul = &mach->insts[j];
      if (mul->opcode != NYR_MACH_MUL ||
          mul->dst.kind != NYR_MACH_OPERAND_VREG ||
          mul->dst.as.reg != address->src1.as.reg ||
          mul->src0.kind != NYR_MACH_OPERAND_VREG ||
          mul->src1.kind != NYR_MACH_OPERAND_VREG)
        continue;
      for (size_t k = 0; k < mach->inst_len; ++k) {
        const nyr_mach_inst_t *literal = &mach->insts[k];
        if (literal->opcode != NYR_MACH_COPY ||
            literal->dst.kind != NYR_MACH_OPERAND_VREG ||
            literal->dst.as.reg != mul->src1.as.reg ||
            literal->src0.kind != NYR_MACH_OPERAND_IMM ||
            literal->src0.as.imm != 8)
          continue;
        address->opcode = NYR_MACH_LEA;
        address->src1 = mul->src0;
        address->src2 =
            (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM, .as.imm = 8};
        ++converted;
        break;
      }
    }
  }
  return converted == expected;
}

static bool
check_float_load_index_reuse(const ny_native_target_info_t *target) {
  nyr_func_t ir;
  nyr_mach_func_t mach = {0};
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[1] = {0};
  nyr_x64_obj_reloc_t relocs[1] = {0};
  size_t def_count = 0, reloc_count = 0;
  char err[256] = {0};
  nyr_opt_begin(&ir);
  ir.param_count = 3;
  ir.param_types = malloc(3 * sizeof(*ir.param_types));
  bool ok = ir.param_types != NULL;
  if (ok) {
    ir.param_types[0] = NYIR_PARAM_I64;
    ir.param_types[1] = NYIR_PARAM_I64;
    ir.param_types[2] = NYIR_PARAM_I64;
  }
  int base = ok ? nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .c = -1,
                                               .d = -1,
                                               .e = -1,
                                               .f = -1,
                                               .imm = 0})
                : -1;
  int index = ok ? nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                                .dst = -1,
                                                .a = -1,
                                                .b = -1,
                                                .c = -1,
                                                .d = -1,
                                                .e = -1,
                                                .f = -1,
                                                .imm = 1})
                 : -1;
  int output = ok ? nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                                 .dst = -1,
                                                 .a = -1,
                                                 .b = -1,
                                                 .c = -1,
                                                 .d = -1,
                                                 .e = -1,
                                                 .f = -1,
                                                 .imm = 2})
                  : -1;
  int scale = ok ? nyr_opt_const(&ir, 8) : -1;
  int offset = ok ? nyr_opt_binop(&ir, NYIR_MUL_I64, index, scale) : -1;
  int address0 = ok ? nyr_opt_binop(&ir, NYIR_ADD_I64, base, offset) : -1;
  int first =
      address0 >= 0
          ? nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                         .dst = -1,
                                         .a = address0,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1,
                                         .scalar_class = NYIR_SCALAR_F64})
          : -1;
  int doubled =
      first >= 0 ? nyr_opt_binop(&ir, NYIR_ADD_F64, first, first) : -1;
  int address1 =
      doubled >= 0 ? nyr_opt_binop(&ir, NYIR_ADD_I64, base, offset) : -1;
  int second =
      address1 >= 0
          ? nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_LOAD_I64,
                                         .dst = -1,
                                         .a = address1,
                                         .b = -1,
                                         .c = -1,
                                         .d = -1,
                                         .e = -1,
                                         .f = -1,
                                         .scalar_class = NYIR_SCALAR_F64})
          : -1;
  int sum =
      second >= 0 ? nyr_opt_binop(&ir, NYIR_ADD_F64, doubled, second) : -1;
  int zero = sum >= 0 ? nyr_opt_const(&ir, 0) : -1;
  if (ok && sum >= 0 && output >= 0 && zero >= 0) {
    nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_STORE_I64,
                                 .dst = -1,
                                 .a = output,
                                 .b = -1,
                                 .c = sum,
                                 .d = -1,
                                 .e = -1,
                                 .f = -1,
                                 .scalar_class = NYIR_SCALAR_F64});
    nyr_opt_ret(&ir, zero);
  } else {
    ok = false;
  }
  ok = ok && nyr_opt_verify(&ir, "machine-float-load-index-reuse") &&
       nyr_mach_lower_nir(&ir, &mach, target->caps, err, sizeof(err));
  ok = ok && test_make_scaled_address_leas(&mach, 2) &&
       nyr_mach_verify(&mach, target->caps, err, sizeof(err));
  ok = ok && nyr_x64_mach_append_function(
                 &code, defs, &def_count, relocs, &reloc_count, &mach, target,
                 "float_load_index_reuse", false, err, sizeof(err));
  int symbol =
      ok ? nyr_x64_obj_def_index(defs, def_count, "float_load_index_reuse")
         : -1;
  ok = ok && symbol >= 0 && reloc_count == 0;
  size_t indexed_loads = 0, index_moves = 0;
  if (ok) {
    const unsigned char *bytes = code.data + defs[symbol].off;
    for (size_t i = 0; i + 5 < defs[symbol].size; ++i) {
      if (bytes[i] == 0xf2) {
        size_t opcode = i + 1;
        if ((bytes[opcode] & 0xf0) == 0x40)
          ++opcode;
        if (opcode + 3 < defs[symbol].size && bytes[opcode] == 0x0f &&
            bytes[opcode + 1] == 0x10 && (bytes[opcode + 2] & 0xc7) == 0x04 &&
            ((bytes[opcode + 3] >> 3) & 7) != 4)
          ++indexed_loads;
      }
      if (bytes[i] == 0x49 && bytes[i + 1] == 0x89 && bytes[i + 2] == 0xc2)
        ++index_moves;
    }
    ok = indexed_loads == 2 && index_moves == 1;
  }
  void *mapping = MAP_FAILED;
  if (ok) {
    mapping = mmap(NULL, code.len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ok = mapping != MAP_FAILED;
  }
  if (ok) {
    memcpy(mapping, code.data, code.len);
    ok = mprotect(mapping, code.len, PROT_READ | PROT_EXEC) == 0;
  }
  if (ok) {
    double cells[4] = {0.0, 3.5, 0.0, 0.0};
    double result = 0.0;
    typedef int64_t (*fn_t)(int64_t, int64_t, int64_t);
    fn_t fn = (fn_t)((unsigned char *)mapping + defs[symbol].off);
    ok = fn((int64_t)(intptr_t)cells, 1, (int64_t)(intptr_t)&result) == 0 &&
         result == 10.5;
  }
  if (!ok)
    fprintf(stderr,
            "[machine-home-reuse] float load index reuse failed: "
            "indexed-loads=%zu index-moves=%zu relocations=%zu error=%s\n",
            indexed_loads, index_moves, reloc_count,
            err[0] ? err : "runtime or assertion");
  if (mapping != MAP_FAILED)
    munmap(mapping, code.len);
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  nyr_ir_func_free(&ir);
  return ok;
}

static bool check_indexed_integer_store_source_preserved(
    const ny_native_target_info_t *target) {
  nyr_func_t ir;
  nyr_mach_func_t mach = {0};
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[1] = {0};
  nyr_x64_obj_reloc_t relocs[1] = {0};
  size_t def_count = 0, reloc_count = 0;
  char err[256] = {0};
  nyr_opt_begin(&ir);
  ir.param_count = 3;
  ir.param_types = malloc(3 * sizeof(*ir.param_types));
  bool ok = ir.param_types != NULL;
  if (ok)
    for (size_t i = 0; i < 3; ++i)
      ir.param_types[i] = NYIR_PARAM_I64;
  int base = ok ? nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .c = -1,
                                               .d = -1,
                                               .e = -1,
                                               .f = -1,
                                               .imm = 0})
                : -1;
  int index = ok ? nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                                .dst = -1,
                                                .a = -1,
                                                .b = -1,
                                                .c = -1,
                                                .d = -1,
                                                .e = -1,
                                                .f = -1,
                                                .imm = 1})
                 : -1;
  int value = ok ? nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                                .dst = -1,
                                                .a = -1,
                                                .b = -1,
                                                .c = -1,
                                                .d = -1,
                                                .e = -1,
                                                .f = -1,
                                                .imm = 2})
                 : -1;
  int zero = ok ? nyr_opt_const(&ir, 0) : -1;
  int scale = ok ? nyr_opt_const(&ir, 8) : -1;
  int offset = ok ? nyr_opt_binop(&ir, NYIR_MUL_I64, index, scale) : -1;
  int address = ok ? nyr_opt_binop(&ir, NYIR_ADD_I64, base, offset) : -1;
  if (ok && address >= 0 && value >= 0 && zero >= 0) {
    nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_STORE_I64,
                                 .dst = -1,
                                 .a = address,
                                 .b = -1,
                                 .c = value,
                                 .d = -1,
                                 .e = -1,
                                 .f = -1,
                                 .effects = NYIR_EFFECT_WRITE_MEMORY |
                                            NYIR_EFFECT_MAY_TRAP,
                                 .scalar_class = NYIR_SCALAR_I64_RAW});
    nyr_opt_ret(&ir, zero);
  } else {
    ok = false;
  }
  ok = ok && nyr_opt_verify(&ir, "machine-indexed-integer-store-source");
  if (!ok)
    fprintf(stderr, "[machine-home-reuse] integer-store IR invalid\n");
  ok = ok && nyr_mach_lower_nir(&ir, &mach, target->caps, err, sizeof(err));
  if (!ok)
    fprintf(stderr, "[machine-home-reuse] integer-store lowering: %s\n",
            err[0] ? err : "failed");
  ok = ok && test_make_scaled_address_leas(&mach, 1);
  if (!ok)
    fprintf(stderr,
            "[machine-home-reuse] integer-store address did not form scaled "
            "LEA\n");
  ok = ok && nyr_mach_verify(&mach, target->caps, err, sizeof(err));
  if (!ok)
    fprintf(stderr, "[machine-home-reuse] integer-store machine IR: %s\n",
            err[0] ? err : "failed");
  ok = ok && nyr_x64_mach_append_function(
                 &code, defs, &def_count, relocs, &reloc_count, &mach, target,
                 "indexed_integer_store_source", false, err, sizeof(err));
  if (!ok)
    fprintf(stderr, "[machine-home-reuse] integer-store encoding: %s\n",
            err[0] ? err : "failed");
  int symbol = ok ? nyr_x64_obj_def_index(defs, def_count,
                                          "indexed_integer_store_source")
                  : -1;
  ok = ok && symbol >= 0 && reloc_count == 0;
  void *mapping = MAP_FAILED;
  if (ok) {
    mapping = mmap(NULL, code.len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ok = mapping != MAP_FAILED;
  }
  if (ok) {
    memcpy(mapping, code.data, code.len);
    ok = mprotect(mapping, code.len, PROT_READ | PROT_EXEC) == 0;
  }
  if (ok) {
    int64_t cells[] = {11, 17, 29, 41};
    int64_t stored = INT64_C(0x123456789abc);
    typedef int64_t (*fn_t)(int64_t, int64_t, int64_t);
    fn_t fn = (fn_t)((unsigned char *)mapping + defs[symbol].off);
    int64_t result = fn((int64_t)(intptr_t)cells, 2, stored);
    ok = result == 0 && cells[0] == 11 && cells[1] == 17 &&
         cells[2] == stored && cells[3] == 41;
    if (!ok)
      fprintf(stderr,
              "[machine-home-reuse] store result=%lld cells=(%lld,%lld,%lld,"
              "%lld) expected value=%lld\n",
              (long long)result, (long long)cells[0], (long long)cells[1],
              (long long)cells[2], (long long)cells[3], (long long)stored);
  }
  if (!ok)
    fprintf(stderr,
            "[machine-home-reuse] indexed integer store clobbered its source: "
            "relocations=%zu error=%s\n",
            reloc_count, err[0] ? err : "runtime or assertion");
  if (mapping != MAP_FAILED)
    munmap(mapping, code.len);
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  nyr_ir_func_free(&ir);
  return ok;
}

static bool check_indexed_float_store_address_forwarding(
    const ny_native_target_info_t *target) {
  nyr_func_t ir;
  nyr_mach_func_t mach = {0};
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[1] = {0};
  nyr_x64_obj_reloc_t relocs[1] = {0};
  size_t def_count = 0, reloc_count = 0;
  char err[256] = {0};
  nyr_opt_begin(&ir);
  ir.param_count = 2;
  ir.param_types = malloc(2 * sizeof(*ir.param_types));
  bool ok = ir.param_types != NULL;
  if (ok) {
    ir.param_types[0] = NYIR_PARAM_I64;
    ir.param_types[1] = NYIR_PARAM_I64;
  }
  int base = ok ? nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .c = -1,
                                               .d = -1,
                                               .e = -1,
                                               .f = -1,
                                               .imm = 0})
                : -1;
  int index = ok ? nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                                .dst = -1,
                                                .a = -1,
                                                .b = -1,
                                                .c = -1,
                                                .d = -1,
                                                .e = -1,
                                                .f = -1,
                                                .imm = 1})
                 : -1;
  int value = ok ? nyr_opt_const_f64(&ir, 7.25) : -1;
  int zero = ok ? nyr_opt_const(&ir, 0) : -1;
  int scale = ok ? nyr_opt_const(&ir, 8) : -1;
  int offset = ok ? nyr_opt_binop(&ir, NYIR_MUL_I64, index, scale) : -1;
  int address = ok ? nyr_opt_binop(&ir, NYIR_ADD_I64, base, offset) : -1;
  if (ok && address >= 0 && value >= 0 && zero >= 0) {
    nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_STORE_I64,
                                 .dst = -1,
                                 .a = address,
                                 .b = -1,
                                 .c = value,
                                 .d = -1,
                                 .e = -1,
                                 .f = -1,
                                 .scalar_class = NYIR_SCALAR_F64});
    nyr_opt_ret(&ir, zero);
  } else {
    ok = false;
  }
  ok = ok && nyr_opt_verify(&ir, "machine-indexed-float-store-forwarding") &&
       nyr_mach_lower_nir(&ir, &mach, target->caps, err, sizeof(err));
  ok = ok && test_make_scaled_address_leas(&mach, 1) &&
       nyr_mach_verify(&mach, target->caps, err, sizeof(err)) &&
       nyr_x64_mach_append_function(
           &code, defs, &def_count, relocs, &reloc_count, &mach, target,
           "indexed_float_store_forwarding", false, err, sizeof(err));
  int symbol = ok ? nyr_x64_obj_def_index(defs, def_count,
                                          "indexed_float_store_forwarding")
                  : -1;
  ok = ok && symbol >= 0 && reloc_count == 0;
  size_t indexed_stores = 0, index_moves = 0;
  if (ok) {
    const unsigned char *bytes = code.data + defs[symbol].off;
    for (size_t i = 0; i + 5 < defs[symbol].size; ++i) {
      if (bytes[i] == 0xf2 && (bytes[i + 1] & 0x43) == 0x42 &&
          bytes[i + 2] == 0x0f && bytes[i + 3] == 0x11 &&
          (bytes[i + 4] & 0xc7) == 0x04 && bytes[i + 5] == 0xd0)
        ++indexed_stores;
      if (bytes[i] == 0x49 && bytes[i + 1] == 0x89 && bytes[i + 2] == 0xc2)
        ++index_moves;
    }
    ok = indexed_stores == 1 && index_moves == 1;
  }
  void *mapping = MAP_FAILED;
  if (ok) {
    mapping = mmap(NULL, code.len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ok = mapping != MAP_FAILED;
  }
  if (ok) {
    memcpy(mapping, code.data, code.len);
    ok = mprotect(mapping, code.len, PROT_READ | PROT_EXEC) == 0;
  }
  if (ok) {
    uint64_t cells[4] = {0, 0, 0, 0};
    uint64_t expected;
    double stored = 7.25;
    memcpy(&expected, &stored, sizeof(expected));
    typedef int64_t (*fn_t)(int64_t, int64_t);
    fn_t fn = (fn_t)((unsigned char *)mapping + defs[symbol].off);
    ok = fn((int64_t)(intptr_t)cells, 2) == 0 && cells[2] == expected;
  }
  if (!ok)
    fprintf(stderr,
            "[machine-home-reuse] indexed float store SIB forwarding failed: "
            "stores=%zu index-moves=%zu relocations=%zu error=%s\n",
            indexed_stores, index_moves, reloc_count,
            err[0] ? err : "runtime or assertion");

  if (mapping != MAP_FAILED)
    munmap(mapping, code.len);
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  nyr_ir_func_free(&ir);
  return ok;
}

static nyr_mach_operand_t home_test_vreg(nyr_mach_reg_t reg) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_VREG,
                              .reg_class = NYR_MACH_REGCLASS_GPR,
                              .as.reg = reg};
}

static nyr_mach_operand_t home_test_block(uint32_t block) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_BLOCK,
                              .as.block_index = block};
}

static bool
check_cross_block_value_register_reuse(const ny_native_target_info_t *target) {
  nyr_mach_func_t mach = {0};
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[1] = {0};
  nyr_x64_obj_reloc_t relocs[1] = {0};
  size_t def_count = 0, reloc_count = 0;
  nyr_mach_reg_t value, condition, one, two, left, right, result;
  uint32_t entry, live_path, dead_path;
  uint32_t param_slot;
  char err[256] = {0};
  bool built =
      nyr_mach_alloc_typed_frame_slot(&mach, sizeof(int64_t), sizeof(int64_t),
                                      NYR_MACH_TYPE_I64, false, &param_slot);
  mach.param_count = built ? 1 : 0;
  built =
      built && nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &value) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &condition) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &one) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &two) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &left) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &right) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &result) &&
      nyr_mach_begin_block(&mach, 0, &entry) &&
      nyr_mach_emit(
          &mach, (nyr_mach_inst_t){.opcode = NYR_MACH_LOAD,
                                   .dst = home_test_vreg(value),
                                   .src0 = {.kind = NYR_MACH_OPERAND_FRAME,
                                            .as.frame_index = param_slot},
                                   .effects = NYR_MACH_EFFECT_READ_MEMORY}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){
                        .opcode = NYR_MACH_COPY,
                        .dst = home_test_vreg(condition),
                        .src0 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 1}}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){.opcode = NYR_MACH_BR_IF,
                                      .src0 = home_test_vreg(condition),
                                      .src1 = home_test_block(1),
                                      .effects = NYR_MACH_EFFECT_CONTROL}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){.opcode = NYR_MACH_BR,
                                      .src1 = home_test_block(2),
                                      .effects = NYR_MACH_EFFECT_CONTROL}) &&
      nyr_mach_begin_block(&mach, 1, &live_path) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){
                        .opcode = NYR_MACH_COPY,
                        .dst = home_test_vreg(one),
                        .src0 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 1}}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){
                        .opcode = NYR_MACH_COPY,
                        .dst = home_test_vreg(two),
                        .src0 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 2}}) &&
      nyr_mach_emit(&mach, (nyr_mach_inst_t){.opcode = NYR_MACH_ADD,
                                             .dst = home_test_vreg(left),
                                             .src0 = home_test_vreg(value),
                                             .src1 = home_test_vreg(one)}) &&
      nyr_mach_emit(&mach, (nyr_mach_inst_t){.opcode = NYR_MACH_ADD,
                                             .dst = home_test_vreg(right),
                                             .src0 = home_test_vreg(value),
                                             .src1 = home_test_vreg(two)}) &&
      nyr_mach_emit(&mach, (nyr_mach_inst_t){.opcode = NYR_MACH_ADD,
                                             .dst = home_test_vreg(result),
                                             .src0 = home_test_vreg(left),
                                             .src1 = home_test_vreg(right)}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){.opcode = NYR_MACH_RET,
                                      .src0 = home_test_vreg(result),
                                      .effects = NYR_MACH_EFFECT_CONTROL}) &&
      nyr_mach_begin_block(&mach, 2, &dead_path) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){.opcode = NYR_MACH_BR,
                                      .src1 = home_test_block(1),
                                      .effects = NYR_MACH_EFFECT_CONTROL}) &&
      entry == 0 && live_path == 1 && dead_path == 2 &&
      nyr_mach_verify(&mach, target->caps, err, sizeof(err)) &&
      nyr_x64_mach_append_function(
          &code, defs, &def_count, relocs, &reloc_count, &mach, target,
          "cross_block_register_reuse", false, err, sizeof(err));
  int symbol = built ? nyr_x64_obj_def_index(defs, def_count,
                                             "cross_block_register_reuse")
                     : -1;
  built = built && symbol >= 0 && reloc_count == 0;
  size_t home_loads = 0;
  size_t max_same_home_loads = 0;
  int32_t loaded_home_offsets[16] = {0};
  size_t loaded_home_counts[16] = {0};
  size_t loaded_home_count = 0;
  if (built) {
    const unsigned char *bytes = code.data + defs[symbol].off;
    for (size_t i = 0; i + 6 < defs[symbol].size; ++i) {
      if ((bytes[i] & 0xf8) == 0x48 && bytes[i + 1] == 0x8b &&
          (bytes[i + 2] & 0xc7) == 0x85) {
        ++home_loads;
        int32_t off;
        memcpy(&off, &bytes[i + 3], sizeof(off));
        size_t slot = 0;
        while (slot < loaded_home_count && loaded_home_offsets[slot] != off)
          ++slot;
        if (slot == loaded_home_count &&
            loaded_home_count <
                sizeof(loaded_home_offsets) / sizeof(loaded_home_offsets[0])) {
          loaded_home_offsets[slot] = off;
          ++loaded_home_count;
        }
        if (slot < loaded_home_count) {
          size_t loads = ++loaded_home_counts[slot];
          if (loads > max_same_home_loads)
            max_same_home_loads = loads;
        }
      }
    }
  }
  void *mapping = MAP_FAILED;
  int64_t actual = INT64_MIN;
  if (built) {
    mapping = mmap(NULL, code.len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    built = mapping != MAP_FAILED;
  }
  if (built) {
    memcpy(mapping, code.data, code.len);
    built = mprotect(mapping, code.len, PROT_READ | PROT_EXEC) == 0;
  }
  if (built) {
    typedef int64_t (*fn_t)(int64_t);
    fn_t fn = (fn_t)((unsigned char *)mapping + defs[symbol].off);
    actual = fn(41);
    built = actual == 85 && home_loads >= 2 && max_same_home_loads == 1;
  }
  if (!built)
    fprintf(stderr,
            "[machine-home-reuse] cross-block value should seed once and "
            "stay resident: result=%lld loads=%zu max-slot-loads=%zu "
            "size=%zu "
            "relocations=%zu error=%s\n",
            (long long)actual, home_loads, max_same_home_loads,
            symbol >= 0 ? defs[symbol].size : 0, reloc_count,
            err[0] ? err : "runtime or encoding");
  if (mapping != MAP_FAILED)
    munmap(mapping, code.len);
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  return built;
}

static nyr_mach_operand_t home_test_vector(nyr_mach_reg_t reg) {
  return (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_VREG,
                              .reg_class = NYR_MACH_REGCLASS_VECTOR,
                              .as.reg = reg};
}

static size_t home_test_frame_loads(const nyr_obj_buf_t *code,
                                    const nyr_x64_obj_symbol_def_t *symbol) {
  size_t count = 0;
  const unsigned char *bytes = code->data + symbol->off;
  for (size_t i = 0; i + 6 < symbol->size; ++i)
    if ((bytes[i] & 0xf8) == 0x48 && bytes[i + 1] == 0x8b &&
        (bytes[i + 2] & 0xc7) == 0x85)
      ++count;
  return count;
}

static bool home_test_symbol_has_bytes(const nyr_obj_buf_t *code,
                                       const nyr_x64_obj_symbol_def_t *symbol,
                                       const unsigned char *needle,
                                       size_t needle_len) {
  if (!code || !symbol || !needle || needle_len == 0 ||
      symbol->off > code->len || symbol->size > code->len - symbol->off)
    return false;
  const unsigned char *bytes = code->data + symbol->off;
  for (size_t i = 0; i + needle_len <= symbol->size; ++i)
    if (memcmp(bytes + i, needle, needle_len) == 0)
      return true;
  return false;
}

static bool
check_vector_disjoint_loop_home(const ny_native_target_info_t *target) {
  nyr_mach_func_t mach = {0};
  nyr_obj_buf_t stack_code = {0}, loop_code = {0};
  nyr_x64_obj_symbol_def_t stack_defs[1] = {0}, loop_defs[1] = {0};
  nyr_x64_obj_reloc_t stack_relocs[1] = {0}, loop_relocs[1] = {0};
  size_t stack_def_count = 0, stack_reloc_count = 0;
  size_t loop_def_count = 0, loop_reloc_count = 0;
  nyr_mach_reg_t index, limit, step, next, condition;
  nyr_mach_reg_t vector_left, vector_right, vector_sum;
  uint32_t param_slot, entry, header, latch, exit;
  char err[256] = {0};

  bool built =
      nyr_mach_alloc_typed_frame_slot(&mach, sizeof(int64_t), sizeof(int64_t),
                                      NYR_MACH_TYPE_I64, false, &param_slot);
  mach.param_count = built ? 1 : 0;
  built =
      built && nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &index) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &limit) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &step) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &next) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_I64, &condition) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_V128_I64, &vector_left) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_V128_I64, &vector_right) &&
      nyr_mach_alloc_typed_vreg(&mach, NYR_MACH_TYPE_V128_I64, &vector_sum) &&
      nyr_mach_begin_block(&mach, 0, &entry) && entry == 0 &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){
                        .opcode = NYR_MACH_COPY,
                        .dst = home_test_vector(vector_left),
                        .src0 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 1}}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){
                        .opcode = NYR_MACH_COPY,
                        .dst = home_test_vector(vector_right),
                        .src0 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 2}}) &&
      nyr_mach_emit(
          &mach, (nyr_mach_inst_t){.opcode = NYR_MACH_ADD,
                                   .dst = home_test_vector(vector_sum),
                                   .src0 = home_test_vector(vector_left),
                                   .src1 = home_test_vector(vector_right)}) &&
      nyr_mach_emit(
          &mach, (nyr_mach_inst_t){.opcode = NYR_MACH_LOAD,
                                   .dst = home_test_vreg(index),
                                   .src0 = {.kind = NYR_MACH_OPERAND_FRAME,
                                            .as.frame_index = param_slot},
                                   .effects = NYR_MACH_EFFECT_READ_MEMORY}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){
                        .opcode = NYR_MACH_COPY,
                        .dst = home_test_vreg(limit),
                        .src0 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 3}}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){
                        .opcode = NYR_MACH_COPY,
                        .dst = home_test_vreg(step),
                        .src0 = {.kind = NYR_MACH_OPERAND_IMM, .as.imm = 1}}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){.opcode = NYR_MACH_BR,
                                      .src1 = home_test_block(1),
                                      .effects = NYR_MACH_EFFECT_CONTROL}) &&
      nyr_mach_begin_block(&mach, 1, &header) && header == 1 &&
      nyr_mach_emit(&mach, (nyr_mach_inst_t){.opcode = NYR_MACH_CMP,
                                             .dst = home_test_vreg(condition),
                                             .src0 = home_test_vreg(index),
                                             .src1 = home_test_vreg(limit),
                                             .condition = NYR_MACH_COND_LT}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){.opcode = NYR_MACH_BR_IF,
                                      .src0 = home_test_vreg(condition),
                                      .src1 = home_test_block(2),
                                      .effects = NYR_MACH_EFFECT_CONTROL}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){.opcode = NYR_MACH_BR,
                                      .src1 = home_test_block(3),
                                      .effects = NYR_MACH_EFFECT_CONTROL}) &&
      nyr_mach_begin_block(&mach, 2, &latch) && latch == 2 &&
      nyr_mach_emit(&mach, (nyr_mach_inst_t){.opcode = NYR_MACH_ADD,
                                             .dst = home_test_vreg(next),
                                             .src0 = home_test_vreg(index),
                                             .src1 = home_test_vreg(step)}) &&
      nyr_mach_emit(&mach, (nyr_mach_inst_t){.opcode = NYR_MACH_COPY,
                                             .dst = home_test_vreg(index),
                                             .src0 = home_test_vreg(next)}) &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){.opcode = NYR_MACH_BR,
                                      .src1 = home_test_block(1),
                                      .effects = NYR_MACH_EFFECT_CONTROL}) &&
      nyr_mach_begin_block(&mach, 3, &exit) && exit == 3 &&
      nyr_mach_emit(&mach,
                    (nyr_mach_inst_t){.opcode = NYR_MACH_RET,
                                      .src0 = home_test_vreg(index),
                                      .effects = NYR_MACH_EFFECT_CONTROL}) &&
      nyr_mach_verify(&mach, target->caps, err, sizeof(err));

  const char *force_prior = getenv("NYTRIX_FORCE_STACK_GPR");
  const char *disable_prior = getenv("NYTRIX_DISABLE_LOOP_GPR");
  const char *enable_prior = getenv("NYTRIX_ENABLE_LOOP_GPR");
  const char *carry_prior = getenv("NYTRIX_GPR_LOOP_CARRY");
  char *force_saved = force_prior ? strdup(force_prior) : NULL;
  char *disable_saved = disable_prior ? strdup(disable_prior) : NULL;
  char *enable_saved = enable_prior ? strdup(enable_prior) : NULL;
  char *carry_saved = carry_prior ? strdup(carry_prior) : NULL;
  bool env_saved =
      (!force_prior || force_saved) && (!disable_prior || disable_saved) &&
      (!enable_prior || enable_saved) && (!carry_prior || carry_saved);
  built = built && env_saved;
  if (built)
    built = setenv("NYTRIX_FORCE_STACK_GPR", "1", 1) == 0 &&
            unsetenv("NYTRIX_DISABLE_LOOP_GPR") == 0 &&
            setenv("NYTRIX_ENABLE_LOOP_GPR", "1", 1) == 0 &&
            setenv("NYTRIX_GPR_LOOP_CARRY", "0", 1) == 0 &&
            nyr_x64_mach_append_function(
                &stack_code, stack_defs, &stack_def_count, stack_relocs,
                &stack_reloc_count, &mach, target, "vector_loop_stack", false,
                err, sizeof(err));
  if (built)
    built = unsetenv("NYTRIX_FORCE_STACK_GPR") == 0 &&
            nyr_x64_mach_append_function(&loop_code, loop_defs, &loop_def_count,
                                         loop_relocs, &loop_reloc_count, &mach,
                                         target, "vector_loop_home", false, err,
                                         sizeof(err));
  if (env_saved) {
    if (force_prior)
      setenv("NYTRIX_FORCE_STACK_GPR", force_saved, 1);
    else
      unsetenv("NYTRIX_FORCE_STACK_GPR");
    if (disable_prior)
      setenv("NYTRIX_DISABLE_LOOP_GPR", disable_saved, 1);
    else
      unsetenv("NYTRIX_DISABLE_LOOP_GPR");
    if (enable_prior)
      setenv("NYTRIX_ENABLE_LOOP_GPR", enable_saved, 1);
    else
      unsetenv("NYTRIX_ENABLE_LOOP_GPR");
    if (carry_prior)
      setenv("NYTRIX_GPR_LOOP_CARRY", carry_saved, 1);
    else
      unsetenv("NYTRIX_GPR_LOOP_CARRY");
  }
  free(force_saved);
  free(disable_saved);
  free(enable_saved);
  free(carry_saved);

  int stack_symbol = built ? nyr_x64_obj_def_index(stack_defs, stack_def_count,
                                                   "vector_loop_stack")
                           : -1;
  int loop_symbol = built ? nyr_x64_obj_def_index(loop_defs, loop_def_count,
                                                  "vector_loop_home")
                          : -1;
  size_t stack_loads =
      stack_symbol >= 0
          ? home_test_frame_loads(&stack_code, &stack_defs[stack_symbol])
          : 0;
  size_t loop_loads =
      loop_symbol >= 0
          ? home_test_frame_loads(&loop_code, &loop_defs[loop_symbol])
          : 0;
  static const unsigned char packed_i64_add[] = {0x66, 0x0f, 0xd4};
  static const unsigned char packed_f64_add[] = {0x66, 0x0f, 0x58};
  void *mapping = MAP_FAILED;
  if (built)
    built =
        stack_symbol >= 0 && loop_symbol >= 0 && stack_reloc_count == 0 &&
        loop_reloc_count == 0 && stack_loads > loop_loads &&
        home_test_symbol_has_bytes(&loop_code, &loop_defs[loop_symbol],
                                   packed_i64_add, sizeof(packed_i64_add)) &&
        !home_test_symbol_has_bytes(&loop_code, &loop_defs[loop_symbol],
                                    packed_f64_add, sizeof(packed_f64_add));
  if (built) {
    mapping = mmap(NULL, loop_code.len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    built = mapping != MAP_FAILED;
  }
  if (built) {
    memcpy(mapping, loop_code.data, loop_code.len);
    built = mprotect(mapping, loop_code.len, PROT_READ | PROT_EXEC) == 0;
  }
  int64_t actual = INT64_MIN;
  if (built) {
    typedef int64_t (*fn_t)(int64_t);
    fn_t fn = (fn_t)((unsigned char *)mapping + loop_defs[loop_symbol].off);
    actual = fn(0);
    built = actual == 3;
  }
  if (!built)
    fprintf(stderr,
            "[machine-home-reuse] vector-disjoint loop home failed: "
            "result=%lld stack-loads=%zu loop-loads=%zu error=%s\n",
            (long long)actual, stack_loads, loop_loads,
            err[0] ? err : "encoding, allocation, or runtime");
  else
    printf("[machine-home-reuse] vector-disjoint loop home reduced frame "
           "loads: %zu -> %zu; result=%lld\n",
           stack_loads, loop_loads, (long long)actual);
  if (mapping != MAP_FAILED)
    munmap(mapping, loop_code.len);
  nyr_obj_free(&stack_code);
  nyr_obj_free(&loop_code);
  nyr_mach_func_free(&mach);
  return built;
}

int main(void) {
#if !defined(__x86_64__)
  return 0;
#else
  nyr_func_t ir;
  nyr_mach_func_t mach = {0};
  nyr_obj_buf_t code = {0};
  nyr_x64_obj_symbol_def_t defs[1] = {0};
  nyr_x64_obj_reloc_t relocs[8] = {0};
  size_t def_count = 0, reloc_count = 0;
  char err[256] = {0};
  nyr_opt_begin(&ir);
  ir.param_count = 1;
  ir.param_types = malloc(sizeof(*ir.param_types));
  bool ok = ir.param_types != NULL;
  if (ok)
    ir.param_types[0] = NYIR_PARAM_I64;

  int arg = ok ? nyr_ir_emit(&ir, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                              .dst = -1,
                                              .a = -1,
                                              .b = -1,
                                              .c = -1,
                                              .d = -1,
                                              .e = -1,
                                              .f = -1,
                                              .imm = 0})
               : -1;
  int one = ok ? nyr_opt_const(&ir, 1) : -1;
  int sum = ok ? nyr_opt_binop(&ir, NYIR_ADD_I64, arg, one) : -1;
  int forty = ok ? nyr_opt_const(&ir, 40) : -1;
  int result = ok ? nyr_opt_binop(&ir, NYIR_ADD_I64, sum, forty) : -1;
  if (ok)
    nyr_opt_ret(&ir, result);

  ny_options opt = {0};
  opt.native_backend = NY_NATIVE_BACKEND_X86_64;
  opt.native_abi = NY_NATIVE_ABI_SYSV;
  opt.host_triple = "x86_64-linux-gnu";
  const nyr_target_request_t target_request = nyr_target_request_make(
      opt.native_backend, opt.native_abi, opt.host_triple, opt.opt_size);
  ny_native_target_info_t target = {0};
  if (!ok || !nyr_opt_verify(&ir, "machine-home-reuse"))
    ok = false;
  if (ok && (!nyr_target_info_init(&target, &target_request) ||
             !nyr_mach_lower_nir(&ir, &mach, target.caps, err, sizeof(err)))) {
    fprintf(stderr, "[machine-home-reuse] machine lowering: %s\n", err);
    ok = false;
  }
  if (ok)
    ok = check_vector_disjoint_loop_home(&target);
  if (ok && !nyr_x64_mach_append_function(
                &code, defs, &def_count, relocs, &reloc_count, &mach, &target,
                "home_reuse", false, err, sizeof(err))) {
    fprintf(stderr, "[machine-home-reuse] object encoding: %s\n", err);
    ok = false;
  }
  int symbol = ok ? nyr_x64_obj_def_index(defs, def_count, "home_reuse") : -1;
  ok = ok && symbol >= 0 && reloc_count == 0;
  if (!ok)
    fprintf(stderr, "[machine-home-reuse] symbol=%d defs=%zu relocations=%zu\n",
            symbol, def_count, reloc_count);
  void *mapping = MAP_FAILED;
  if (ok) {
    mapping = mmap(NULL, code.len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ok = mapping != MAP_FAILED;
  }
  if (ok) {
    memcpy(mapping, code.data, code.len);
    ok = mprotect(mapping, code.len, PROT_READ | PROT_EXEC) == 0;
  }
  if (ok) {
    typedef int64_t (*fn_t)(int64_t);
    fn_t fn = (fn_t)((unsigned char *)mapping + defs[symbol].off);
    int64_t positive = fn(1);
    int64_t negative = fn(-9);
    ok = positive == 42 && negative == 32;
    if (!ok)
      fprintf(stderr,
              "[machine-home-reuse] got (%lld, %lld), expected (42, 32)\n",
              (long long)positive, (long long)negative);
  }
  if (ok)
    ok = check_literal_home_elision(&target);
  if (ok)
    ok = check_indexed_address_forwarding(&target);
  if (ok)
    ok = check_uncolored_add_address_forwarding(&target);
  if (ok)
    ok = check_float_load_index_reuse(&target);
  if (!ok)
    fprintf(stderr, "[machine-home-reuse] %s\n",
            err[0] ? err : "frame argument or reused home was corrupted");
  else
    ok = check_indexed_integer_store_source_preserved(&target) &&
         check_indexed_float_store_address_forwarding(&target) &&
         check_cross_block_value_register_reuse(&target);
  if (ok)
    printf("[machine-home-reuse] frame homes stay distinct; literal homes "
           "rematerialize; cross-block values seed once per block; indexed "
           "loads use direct SIB addressing, including uncolored ADD "
           "addresses; dynamic integer and float stores preserve indexed "
           "values; vector-disjoint loop homes reduce frame loads\n");
  if (mapping != MAP_FAILED)
    munmap(mapping, code.len);
  nyr_obj_free(&code);
  nyr_mach_func_free(&mach);
  nyr_ir_func_free(&ir);
  return ok ? 0 : 1;
#endif
}
