/*
 * Pin internal AArch64 scalar, control-flow, memory, and Linux syscall forms.
 */
#include "code/backend/mach/object.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const char *nyr_internal_aarch64_disasm(const unsigned char *bytes,
                                               int64_t length, int64_t address);

static bool build_constant(nyr_mach_func_t *func, int64_t value) {
  nyr_mach_reg_t dst;
  uint32_t block;
  return nyr_mach_begin_block(func, 0, &block) &&
         nyr_mach_alloc_typed_vreg(func, NYR_MACH_TYPE_I64, &dst) &&
         nyr_mach_emit(
             func, (nyr_mach_inst_t){.opcode = NYR_MACH_COPY,
                                     .dst = {.kind = NYR_MACH_OPERAND_VREG,
                                             .reg_class = NYR_MACH_REGCLASS_GPR,
                                             .as.reg = dst},
                                     .src0 = {.kind = NYR_MACH_OPERAND_IMM,
                                              .as.imm = value}}) &&
         nyr_mach_emit(func,
                       (nyr_mach_inst_t){.opcode = NYR_MACH_RET,
                                         .effects = NYR_MACH_EFFECT_CONTROL});
}

static bool check_a64_encoding_roundtrip(void) {
  ny_options options;
  ny_options_init(&options);
  options.native_backend = NY_NATIVE_BACKEND_AARCH64;
  nyr_target_request_t request =
      nyr_target_request_make(options.native_backend, options.native_abi,
                              options.host_triple, options.opt_size);
  ny_native_target_info_t target = {0};
  nyr_mach_func_t entry = {0}, func = {0};
  nyr_obj_buf_t first = {0}, second = {0};
  nyr_x64_obj_symbol_def_t defs_a[4] = {0}, defs_b[4] = {0};
  nyr_x64_obj_reloc_t relocs_a[4] = {0}, relocs_b[4] = {0};
  size_t def_count_a = 0, def_count_b = 0, reloc_count_a = 0, reloc_count_b = 0;
  char err[256] = {0};
  const char *const names[] = {"decoder_roundtrip"};
  bool ok =
      nyr_target_info_init(&target, &request) && build_constant(&entry, 0) &&
      build_constant(&func, 42) &&
      nyr_a64_mach_build_bundle(&entry, &func, names, 1, &target, "rt_main",
                                false, &first, defs_a, &def_count_a, relocs_a,
                                &reloc_count_a, err, sizeof(err)) &&
      nyr_a64_mach_build_bundle(&entry, &func, names, 1, &target, "rt_main",
                                false, &second, defs_b, &def_count_b, relocs_b,
                                &reloc_count_b, err, sizeof(err));
  int a =
      ok ? nyr_x64_obj_def_index(defs_a, def_count_a, "ny_fn_decoder_roundtrip")
         : -1;
  int b =
      ok ? nyr_x64_obj_def_index(defs_b, def_count_b, "ny_fn_decoder_roundtrip")
         : -1;
  ok = ok && a >= 0 && b >= 0 && reloc_count_a == 0 && reloc_count_b == 0 &&
       defs_a[a].size == defs_b[b].size && defs_a[a].off <= first.len &&
       defs_b[b].off <= second.len &&
       defs_a[a].size <= first.len - defs_a[a].off &&
       defs_b[b].size <= second.len - defs_b[b].off &&
       memcmp(first.data + defs_a[a].off, second.data + defs_b[b].off,
              defs_a[a].size) == 0;
  if (ok) {
    const char *listing = nyr_internal_aarch64_disasm(
        first.data + defs_a[a].off, defs_a[a].size, 0x5000);
    ok = !strstr(listing, "\tunsupported\t") && strstr(listing, "\tstp\t") &&
         strstr(listing, "\tldp\t") && strstr(listing, "\tret\t");
    if (!ok)
      fprintf(stderr,
              "[aarch64-disasm-scalar] emitted A64 code not decoded:\n%s",
              listing);
  }
  if (!ok && err[0])
    fprintf(stderr, "[aarch64-disasm-scalar] encoder: %s\n", err);
  nyr_mach_func_free(&entry);
  nyr_mach_func_free(&func);
  nyr_obj_free(&first);
  nyr_obj_free(&second);
  return ok;
}

int main(void) {
  static const unsigned char encoded[] = {
      0x40, 0x05, 0x80, 0xd2, /* movz x0, #42 */
      0x00, 0x04, 0x00, 0x91, /* add x0, x0, #1 */
      0x1f, 0x00, 0x00, 0xf1, /* cmp x0, #0 */
      0x40, 0x00, 0x00, 0x54, /* b.eq +8 */
      0xe0, 0x07, 0x00, 0xf9, /* str x0, [sp, #8] */
      0xe0, 0x07, 0x40, 0xf9, /* ldr x0, [sp, #8] */
      0xa8, 0x0b, 0x80, 0xd2, /* movz x8, #93 */
      0x01, 0x00, 0x00, 0xd4, /* svc #0 */
      0xc0, 0x03, 0x5f, 0xd6, /* ret */
  };
  const char *listing =
      nyr_internal_aarch64_disasm(encoded, (int64_t)sizeof(encoded), 0x1000);
  static const char *const expected[] = {
      "4096\tmovz\tx0, #0x2a\t4\t",
      "4100\tadd\tx0, x0, #1\t4\t",
      "4104\tcmp\tx0, #0\t4\t",
      "4108\tb.eq\t0x1014\t4\t",
      "4112\tstr\tx0, [sp, #8]\t4\t",
      "4116\tldr\tx0, [sp, #8]\t4\t",
      "4120\tmovz\tx8, #0x5d\t4\t",
      "4124\tsvc\t#0\t4\t",
      "4128\tret\t\t4\t",
  };
  for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
    if (!strstr(listing, expected[i])) {
      fprintf(stderr, "[aarch64-disasm-scalar] instruction mismatch:\n%s",
              listing);
      return 1;
    }
  }
  if (strstr(listing, "\tunsupported\t")) {
    fprintf(stderr,
            "[aarch64-disasm-scalar] supported scalar form rejected:\n%s",
            listing);
    return 1;
  }

  static const unsigned char unknown[] = {0xff, 0xff, 0xff, 0xff};
  listing = nyr_internal_aarch64_disasm(unknown, sizeof(unknown), 0x2000);
  if (!strstr(
          listing,
          "8192\tunsupported\tinstruction word 0xffffffff at 0x2000\t4\t")) {
    fprintf(stderr,
            "[aarch64-disasm-scalar] unsupported word was not explicit:\n%s",
            listing);
    return 1;
  }
  if (nyr_internal_aarch64_disasm(encoded, sizeof(encoded) - 1, 0x1000)[0]) {
    fprintf(
        stderr,
        "[aarch64-disasm-scalar] accepted a non-instruction-aligned size\n");
    return 1;
  }

  static const unsigned char vectors[] = {
      0x00, 0x00, 0xc0, 0x3d, /* ldr q0, [x0] */
      0x01, 0x04, 0x80, 0x3d, /* str q1, [x0, #16] */
      0x00, 0x0c, 0x08, 0x4e, /* dup v0.2d, x0 */
      0x20, 0xd4, 0x62, 0x4e, /* fadd v0.2d, v1.2d, v2.2d */
      0x20, 0xcc, 0x22, 0x4e, /* fmla v0.4s, v1.4s, v2.4s */
      0x03, 0x00, 0x67, 0x9e, /* fmov d3, x0 */
      0x23, 0x1c, 0x18, 0x4e, /* ins v3.d[1], x1 */
      0x41, 0x0c, 0x04, 0x4e, /* dup v1.4s, w2 */
      0x20, 0x84, 0xe2, 0x4e, /* add v0.2d, v1.2d, v2.2d */
      0x41, 0x1c, 0x23, 0x4e, /* and v1.16b, v2.16b, v3.16b */
      0x20, 0xfc, 0x22, 0x6e, /* fdiv v0.4s, v1.4s, v2.4s */
      0x20, 0xcc, 0x62, 0x4e, /* fmla v0.2d, v1.2d, v2.2d */
  };
  listing = nyr_internal_aarch64_disasm(vectors, sizeof(vectors), 0x3000);
  static const char *const vector_expected[] = {
      "12288\tldr\tq0, [x0]\t4\t",
      "12292\tstr\tq1, [x0, #16]\t4\t",
      "12296\tdup\tv0.2d, x0\t4\t",
      "12300\tfadd\tv0.2d, v1.2d, v2.2d\t4\t",
      "12304\tfmla\tv0.4s, v1.4s, v2.4s\t4\t",
      "12308\tfmov\td3, x0\t4\t",
      "12312\tins\tv3.d[1], x1\t4\t",
      "12316\tdup\tv1.4s, w2\t4\t",
      "12320\tadd\tv0.2d, v1.2d, v2.2d\t4\t",
      "12324\tand\tv1.16b, v2.16b, v3.16b\t4\t",
      "12328\tfdiv\tv0.4s, v1.4s, v2.4s\t4\t",
      "12332\tfmla\tv0.2d, v1.2d, v2.2d\t4\t",
  };
  for (size_t i = 0; i < sizeof(vector_expected) / sizeof(vector_expected[0]);
       ++i) {
    if (!strstr(listing, vector_expected[i])) {
      fprintf(stderr, "[aarch64-disasm-scalar] vector form mismatch:\n%s",
              listing);
      return 1;
    }
  }
  if (strstr(listing, "\tunsupported\t")) {
    fprintf(stderr, "[aarch64-disasm-scalar] emitted vector form rejected:\n%s",
            listing);
    return 1;
  }
  static const unsigned char emitted_forms[] = {
      0xfd, 0x7b, 0xbf, 0xa9, /* stp x29, x30, [sp, #-16]! */
      0xfd, 0x7b, 0xc1, 0xa8, /* ldp x29, x30, [sp], #16 */
      0x20, 0x28, 0x62, 0x1e, /* fadd d0, d1, d2 */
      0x20, 0x08, 0x22, 0x1e, /* fmul s0, s1, s2 */
      0x00, 0x00, 0x66, 0x9e, /* fmov x0, d0 */
      0x00, 0x00, 0x67, 0x9e, /* fmov d0, x0 */
      0xa0, 0x83, 0x5f, 0xf8, /* ldur x0, [x29, #-8] */
      0xa0, 0x83, 0x00, 0xf8, /* stur x0, [x29, #8] */
      0x20, 0x0c, 0xc2, 0x9a, /* sdiv x0, x1, x2 */
      0x20, 0x20, 0xc2, 0x9a, /* lslv x0, x1, x2 */
      0x20, 0x28, 0xc2, 0x9a, /* asrv x0, x1, x2 */
      0x20, 0x8c, 0x02, 0x9b, /* msub x0, x1, x2, x3 */
      0x20, 0x10, 0x82, 0x9a, /* csel x0, x1, x2, ne */
      0x00, 0x00, 0x62, 0x9e, /* scvtf d0, x0 */
      0x00, 0x00, 0x78, 0x9e, /* fcvtzs x0, d0 */
      0x00, 0x40, 0x62, 0x1e, /* fcvt s0, d0 */
      0x20, 0x2c, 0xc2, 0x9a, /* rorv x0, x1, x2 */
      0x00, 0x0c, 0xc0, 0xda, /* rev x0, x0 */
      0x00, 0x08, 0xc0, 0x5a, /* rev w0, w0 */
      0x20, 0x04, 0x80, 0xb9, /* ldrsw x0, [x1, #4] */
      0x00, 0x00, 0x00, 0x90, /* adrp x0, current page */
  };
  listing =
      nyr_internal_aarch64_disasm(emitted_forms, sizeof(emitted_forms), 0x4000);
  static const char *const emitted_expected[] = {
      "16384\tstp\tx29, x30, [sp, #-16]!\t4\t",
      "16388\tldp\tx29, x30, [sp], #16\t4\t",
      "16392\tfadd\td0, d1, d2\t4\t",
      "16396\tfmul\ts0, s1, s2\t4\t",
      "16400\tfmov\tx0, d0\t4\t",
      "16404\tfmov\td0, x0\t4\t",
      "16408\tldur\tx0, [x29, #-8]\t4\t",
      "16412\tstur\tx0, [x29, #8]\t4\t",
      "16416\tsdiv\tx0, x1, x2\t4\t",
      "16420\tlslv\tx0, x1, x2\t4\t",
      "16424\tasrv\tx0, x1, x2\t4\t",
      "16428\tmsub\tx0, x1, x2, x3\t4\t",
      "16432\tcsel\tx0, x1, x2, ne\t4\t",
      "16436\tscvtf\td0, x0\t4\t",
      "16440\tfcvtzs\tx0, d0\t4\t",
      "16444\tfcvt\ts0, d0\t4\t",
      "16448\trorv\tx0, x1, x2\t4\t",
      "16452\trev\tx0, x0\t4\t",
      "16456\trev\tw0, w0\t4\t",
      "16460\tldrsw\tx0, [x1, #4]\t4\t",
      "16464\tadrp\tx0, 0x4000\t4\t",
  };
  for (size_t i = 0; i < sizeof(emitted_expected) / sizeof(emitted_expected[0]);
       ++i) {
    if (!strstr(listing, emitted_expected[i])) {
      fprintf(stderr, "[aarch64-disasm-scalar] encoder form mismatch:\n%s",
              listing);
      return 1;
    }
  }
  if (strstr(listing, "\tunsupported\t")) {
    fprintf(stderr,
            "[aarch64-disasm-scalar] emitted AArch64 form rejected:\n%s",
            listing);
    return 1;
  }
  if (!check_a64_encoding_roundtrip())
    return 1;
  static const unsigned char vector_lane_forms[] = {
      0x00, 0x04, 0x08, 0x4e, /* dup v0.2d, v0.d[0] */
      0x20, 0x40, 0x03, 0x4e, /* tbl v0.16b,{v1.16b,v2.16b},v3.16b */
      0x25, 0x40, 0x04, 0x4e, /* tbl v5.16b,{v1.16b,v2.16b},v4.16b */
      0x02, 0xd8, 0x70, 0x7e, /* faddp d2, v0.2d */
      0x23, 0xd8, 0x70, 0x7e, /* faddp d3, v1.2d */
      0x42, 0x28, 0x63, 0x1e, /* fadd d2, d2, d3 */
  };
  listing = nyr_internal_aarch64_disasm(vector_lane_forms,
                                        sizeof(vector_lane_forms), 0x6000);
  static const char *const vector_lane_expected[] = {
      "24576\tdup\tv0.2d, v0.d[0]\t4\t",
      "24580\ttbl\tv0.16b, {v1.16b, v2.16b}, v3.16b\t4\t",
      "24584\ttbl\tv5.16b, {v1.16b, v2.16b}, v4.16b\t4\t",
      "24588\tfaddp\td2, v0.2d\t4\t",
      "24592\tfaddp\td3, v1.2d\t4\t",
      "24596\tfadd\td2, d2, d3\t4\t",
  };
  for (size_t i = 0;
       i < sizeof(vector_lane_expected) / sizeof(vector_lane_expected[0]);
       ++i) {
    if (!strstr(listing, vector_lane_expected[i])) {
      fprintf(stderr,
              "[aarch64-disasm-scalar] vector-lane decode mismatch:\n%s",
              listing);
      return 1;
    }
  }
  if (strstr(listing, "\tunsupported\t")) {
    fprintf(stderr,
            "[aarch64-disasm-scalar] emitted vector-lane form rejected:\n%s",
            listing);
    return 1;
  }
  static const unsigned char bare_runtime_forms[] = {
      0x69, 0x6a, 0x75, 0x38, /* ldrb w9, [x19, x21] */
      0x49, 0x69, 0x3a, 0x38, /* strb w9, [x10, x26] */
      0x8f, 0x7d, 0x4a, 0x9b, /* umulh x15, x12, x10 */
      0x2c, 0xfd, 0x52, 0xd3, /* lsr x12, x9, #18 */
      0x2c, 0x7d, 0x0c, 0x53, /* lsr w12, w9, #12 */
      0x2c, 0x15, 0x00, 0x12, /* and w12, w9, #0x3f */
      0x00, 0x00, 0x20, 0xd4, /* brk #0 */
      0x42, 0xb8, 0xe0, 0x6e, /* neg v2.2d, v2.2d */
      0x20, 0x44, 0xe2, 0x4e, /* sshl v0.2d, v1.2d, v2.2d */
      0x2c, 0x15, 0x00, 0x32, /* orr w12, w9, #0x3f */
      0x2c, 0x15, 0x00, 0x52, /* eor w12, w9, #0x3f */
      0x2c, 0x15, 0x00, 0x72, /* ands w12, w9, #0x3f */
      0x3f, 0x15, 0x00, 0x72, /* tst w9, #0x3f */
  };
  listing = nyr_internal_aarch64_disasm(bare_runtime_forms,
                                        sizeof(bare_runtime_forms), 0x7000);
  static const char *const bare_runtime_expected[] = {
      "28672\tldrb\tw9, [x19, x21]\t4\t",
      "28676\tstrb\tw9, [x10, x26]\t4\t",
      "28680\tumulh\tx15, x12, x10\t4\t",
      "28684\tlsr\tx12, x9, #18\t4\t",
      "28688\tlsr\tw12, w9, #12\t4\t",
      "28692\tand\tw12, w9, #0x3f\t4\t",
      "28696\tbrk\t#0\t4\t",
      "28700\tneg\tv2.2d, v2.2d\t4\t",
      "28704\tsshl\tv0.2d, v1.2d, v2.2d\t4\t",
      "28708\torr\tw12, w9, #0x3f\t4\t",
      "28712\teor\tw12, w9, #0x3f\t4\t",
      "28716\tands\tw12, w9, #0x3f\t4\t",
      "28720\ttst\tw9, #0x3f\t4\t",
  };
  for (size_t i = 0;
       i < sizeof(bare_runtime_expected) / sizeof(bare_runtime_expected[0]);
       ++i) {
    if (!strstr(listing, bare_runtime_expected[i])) {
      fprintf(stderr, "[aarch64-disasm-scalar] bare runtime form mismatch:\n%s",
              listing);
      return 1;
    }
  }
  if (strstr(listing, "\tunsupported\t")) {
    fprintf(stderr, "[aarch64-disasm-scalar] bare runtime form rejected:\n%s",
            listing);
    return 1;
  }
  static const unsigned char scaled_fp_memory_forms[] = {
      0xe0, 0x07, 0x40, 0xbd, /* ldr s0, [sp, #4] */
      0xe1, 0x0b, 0x00, 0xbd, /* str s1, [sp, #8] */
      0xe2, 0x0b, 0x40, 0xfd, /* ldr d2, [sp, #16] */
      0xe3, 0x0f, 0x00, 0xfd, /* str d3, [sp, #24] */
  };
  listing = nyr_internal_aarch64_disasm(scaled_fp_memory_forms,
                                        sizeof(scaled_fp_memory_forms), 0x7100);
  static const char *const scaled_fp_expected[] = {
      "28928\tldr\ts0, [sp, #4]\t4\t",
      "28932\tstr\ts1, [sp, #8]\t4\t",
      "28936\tldr\td2, [sp, #16]\t4\t",
      "28940\tstr\td3, [sp, #24]\t4\t",
  };
  for (size_t i = 0;
       i < sizeof(scaled_fp_expected) / sizeof(scaled_fp_expected[0]); ++i) {
    if (!strstr(listing, scaled_fp_expected[i])) {
      fprintf(stderr, "[aarch64-disasm-scalar] scaled FP memory mismatch:\n%s",
              listing);
      return 1;
    }
  }
  if (strstr(listing, "\tunsupported\t")) {
    fprintf(stderr,
            "[aarch64-disasm-scalar] scaled FP memory form rejected:\n%s",
            listing);
    return 1;
  }
  puts("[aarch64-disasm-scalar] scalar, vector, and syscall forms decode "
       "exactly");
  return 0;
}
