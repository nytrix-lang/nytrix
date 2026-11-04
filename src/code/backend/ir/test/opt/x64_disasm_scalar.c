/*
 * Pin the scalar ModRM/immediate forms used by ordinary x86-64 C functions.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern const char *nyr_internal_x64_disasm(const unsigned char *bytes,
                                           int64_t length, int64_t address);

int main(void) {
  static const unsigned char encoded[] = {
      0x55,                         /* push rbp */
      0x48, 0x89, 0xe5,             /* mov rbp, rsp */
      0x89, 0x7d, 0xfc,             /* mov [rbp-4], edi */
      0x83, 0x7d, 0xfc, 0x00,       /* cmp [rbp-4], 0 */
      0x79, 0x07,                   /* jns */
      0xb8, 0x03, 0x00, 0x00, 0x00, /* mov eax, 3 */
      0xeb, 0x06,                   /* jmp */
      0x8b, 0x45, 0xfc,             /* mov eax, [rbp-4] */
      0x83, 0xc0, 0x01,             /* add eax, 1 */
      0x5d, 0xc3,                   /* pop rbp; ret */
  };
  const char *listing =
      nyr_internal_x64_disasm(encoded, (int64_t)sizeof(encoded), 0x401000);
  static const char *const expected[] = {
      "4198400\tpush\trbp\t1\n",
      "4198401\tmov\trbp, rsp\t3\n",
      "4198404\tmov\tdword ptr [rbp-4], edi\t3\n",
      "4198407\tcmp\tdword ptr [rbp-4], 0x0\t4\n",
      "4198411\tjns\t0x401014\t2\n",
      "4198413\tmov\teax, 0x3\t5\n",
      "4198418\tjmp\t0x40101a\t2\n",
      "4198420\tmov\teax, dword ptr [rbp-4]\t3\n",
      "4198423\tadd\teax, 0x1\t3\n",
      "4198426\tpop\trbp\t1\n",
      "4198427\tret\t\t1\n",
  };
  for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
    if (!strstr(listing, expected[i])) {
      fprintf(stderr, "[x64-disasm-scalar] instruction mismatch:\n%s", listing);
      return 1;
    }
  }
  if (strstr(listing, "\tunsupported\t")) {
    fprintf(stderr, "[x64-disasm-scalar] supported scalar form rejected:\n%s",
            listing);
    return 1;
  }
  static const unsigned char immediate_forms[] = {
      0x66, 0x81, 0xc0, 0x34, 0x12,            /* add ax, 0x1234 */
      0x48, 0xc7, 0xc0, 0xff, 0xff, 0xff, 0xff /* mov rax, -1 */
  };
  listing = nyr_internal_x64_disasm(immediate_forms,
                                    (int64_t)sizeof(immediate_forms), 0x5000);
  if (!strstr(listing, "20480\tadd\tax, 0x1234\t5\n") ||
      !strstr(listing, "20485\tmov\trax, -0x1\t7\n") ||
      strstr(listing, "\tunsupported\t")) {
    fprintf(stderr, "[x64-disasm-scalar] immediate width/sign mismatch:\n%s",
            listing);
    return 1;
  }
  static const unsigned char byte_forms[] = {
      0xc6, 0x07, 0x00,             /* mov byte ptr [rdi], 0 */
      0x8a, 0x06,                   /* mov al, byte ptr [rsi] */
      0x88, 0x07,                   /* mov byte ptr [rdi], al */
      0x48, 0x80, 0x3c, 0x16, 0x00, /* cmp byte ptr [rsi+rdx], 0 */
      0x49, 0xc6, 0x00, 0xff,       /* mov byte ptr [r8], 0xff */
      0x66, 0x80, 0x38, 0x80        /* cmp byte ptr [rax], 0x80 */
  };
  listing =
      nyr_internal_x64_disasm(byte_forms, (int64_t)sizeof(byte_forms), 0x6000);
  if (!strstr(listing, "24576\tmov\tbyte ptr [rdi], 0x0\t3\n") ||
      !strstr(listing, "24579\tmov\tal, byte ptr [rsi]\t2\n") ||
      !strstr(listing, "24581\tmov\tbyte ptr [rdi], al\t2\n") ||
      !strstr(listing, "24583\tcmp\tbyte ptr [rdx*1+rsi], 0x0\t5\n") ||
      !strstr(listing, "24588\tmov\tbyte ptr [r8], 0xff\t4\n") ||
      !strstr(listing, "24592\tcmp\tbyte ptr [rax], 0x80\t4\n") ||
      strstr(listing, "\tunsupported\t")) {
    fprintf(stderr, "[x64-disasm-scalar] emitted byte forms mismatch:\n%s",
            listing);
    return 1;
  }
  static const unsigned char syscall_stub[] = {
      0xb8, 0x3c, 0x00, 0x00, 0x00, /* mov eax, 60 */
      0xbf, 0x2a, 0x00, 0x00, 0x00, /* mov edi, 42 */
      0x0f, 0x05,                   /* syscall */
      0x0f, 0x0b,                   /* ud2 */
  };
  listing = nyr_internal_x64_disasm(syscall_stub, (int64_t)sizeof(syscall_stub),
                                    0x7000);
  if (!strstr(listing, "28672\tmov\teax, 0x3c\t5\n") ||
      !strstr(listing, "28677\tmov\tedi, 0x2a\t5\n") ||
      !strstr(listing, "28682\tsyscall\t\t2\n") ||
      !strstr(listing, "28684\tud2\t\t2\n") ||
      strstr(listing, "\tunsupported\t")) {
    fprintf(stderr, "[x64-disasm-scalar] syscall stub mismatch:\n%s", listing);
    return 1;
  }
  static const unsigned char sse_forms[] = {
      0xf3, 0x0f, 0x10, 0x45, 0xfc, /* movss xmm0, dword ptr [rbp-4] */
      0xf3, 0x0f, 0x58, 0xc1,       /* addss xmm0, xmm1 */
      0x66, 0x0f, 0x58, 0xc1,       /* addpd xmm0, xmm1 */
      0xf2, 0x0f, 0x2a, 0xc1,       /* cvtsi2sd xmm0, ecx */
      0xf2, 0x0f, 0x2c, 0xc1,       /* cvttsd2si eax, xmm1 */
      0xf2, 0x0f, 0x5a, 0xc1,       /* cvtsd2ss xmm0, xmm1 */
      0x66, 0x0f, 0x6c, 0xc1,       /* punpcklqdq xmm0, xmm1 */
      0x0f, 0xc6, 0xc1, 0x1b,       /* shufps xmm0, xmm1, 0x1b */
      0x66, 0x0f, 0x2e, 0xc1,       /* ucomisd xmm0, xmm1 */
      0xf3, 0x0f, 0x6f, 0x45, 0xfc, /* movdqu xmm0, [rbp-4] */
      0x66, 0x0f, 0x7f, 0x4d, 0xf0  /* movdqa [rbp-16], xmm1 */
  };
  listing = nyr_internal_x64_disasm(sse_forms, sizeof(sse_forms), 0x8000);
  if (!strstr(listing, "32768\tmovss\txmm0, dword ptr [rbp-4]\t5\n") ||
      !strstr(listing, "32773\taddss\txmm0, xmm1\t4\n") ||
      !strstr(listing, "32777\taddpd\txmm0, xmm1\t4\n") ||
      !strstr(listing, "32781\tcvtsi2sd\txmm0, ecx\t4\n") ||
      !strstr(listing, "32785\tcvttsd2si\teax, xmm1\t4\n") ||
      !strstr(listing, "32789\tcvtsd2ss\txmm0, xmm1\t4\n") ||
      !strstr(listing, "32793\tpunpcklqdq\txmm0, xmm1\t4\n") ||
      !strstr(listing, "32797\tshufps\txmm0, xmm1, 0x1b\t4\n") ||
      !strstr(listing, "32801\tucomisd\txmm0, xmm1\t4\n") ||
      !strstr(listing, "32805\tmovdqu\txmm0, xmmword ptr [rbp-4]\t5\n") ||
      !strstr(listing, "32810\tmovdqa\txmmword ptr [rbp-16], xmm1\t5\n") ||
      strstr(listing, "\tunsupported\t")) {
    fprintf(stderr,
            "[x64-disasm-scalar] emitted SSE forms decode incorrectly:\n%s",
            listing);
    return 1;
  }
  static const unsigned char shift_forms[] = {
      0x48, 0xc1, 0xfa, 0x3f, /* sar rdx, 0x3f */
      0x48, 0xd1, 0xe0,       /* shl rax, 1 */
      0x48, 0xd3, 0xf8,       /* sar rax, cl */
  };
  listing = nyr_internal_x64_disasm(shift_forms, sizeof(shift_forms), 0x9000);
  if (!strstr(listing, "36864\tsar\trdx, 0x3f\t4\n") ||
      !strstr(listing, "36868\tshl\trax, 1\t3\n") ||
      !strstr(listing, "36871\tsar\trax, cl\t3\n") ||
      strstr(listing, "\tunsupported\t")) {
    fprintf(stderr, "[x64-disasm-scalar] emitted shift form mismatch:\n%s",
            listing);
    return 1;
  }
  static const unsigned char test_immediate_forms[] = {
      0x48, 0xf7, 0xc0, 0x01, 0x00, 0x00, 0x00, /* test rax, 1 */
      0xf6, 0x07, 0x80,                         /* test byte ptr [rdi], 0x80 */
      0x66, 0xf7, 0xc1, 0xff, 0xff,             /* test cx, -1 */
  };
  listing = nyr_internal_x64_disasm(test_immediate_forms,
                                    sizeof(test_immediate_forms), 0x9800);
  if (!strstr(listing, "38912\ttest\trax, 0x1\t7\n") ||
      !strstr(listing, "38919\ttest\tbyte ptr [rdi], 0x80\t3\n") ||
      !strstr(listing, "38922\ttest\tcx, -0x1\t5\n") ||
      strstr(listing, "\tunsupported\t")) {
    fprintf(stderr, "[x64-disasm-scalar] immediate TEST form mismatch:\n%s",
            listing);
    return 1;
  }
  static const unsigned char divide_forms[] = {
      0x48, 0x99,       /* cqo */
      0x49, 0xf7, 0xfa, /* idiv r10 */
      0x48, 0xf7, 0xd8, /* neg rax */
  };
  listing = nyr_internal_x64_disasm(divide_forms, sizeof(divide_forms), 0xa000);
  if (!strstr(listing, "40960\tcqo\t\t2\n") ||
      !strstr(listing, "40962\tidiv\tr10\t3\n") ||
      !strstr(listing, "40965\tneg\trax\t3\n") ||
      strstr(listing, "\tunsupported\t")) {
    fprintf(stderr, "[x64-disasm-scalar] emitted division form mismatch:\n%s",
            listing);
    return 1;
  }
  static const unsigned char rotate_bswap_forms[] = {
      0x48, 0x0f, 0xc8,       /* bswap rax */
      0x0f, 0xc8,             /* bswap eax */
      0x48, 0xc1, 0xc8, 0x08, /* ror rax, 8 */
      0x48, 0xd3, 0xc8,       /* ror rax, cl */
      0xc1, 0xc8, 0x07,       /* ror eax, 7 */
      0xd3, 0xc8,             /* ror eax, cl */
  };
  listing = nyr_internal_x64_disasm(rotate_bswap_forms,
                                    sizeof(rotate_bswap_forms), 0xb000);
  if (!strstr(listing, "45056\tbswap\trax\t3\n") ||
      !strstr(listing, "45059\tbswap\teax\t2\n") ||
      !strstr(listing, "45061\tror\trax, 0x8\t4\n") ||
      !strstr(listing, "45065\tror\trax, cl\t3\n") ||
      !strstr(listing, "45068\tror\teax, 0x7\t3\n") ||
      !strstr(listing, "45071\tror\teax, cl\t2\n") ||
      strstr(listing, "\tunsupported\t")) {
    fprintf(stderr, "[x64-disasm-scalar] rotate/byte-swap mismatch:\n%s",
            listing);
    return 1;
  }
  static const unsigned char x87_and_string_forms[] = {
      0xdd, 0x45, 0xf8, /* fld qword ptr [rbp-8] */
      0xd9, 0xfe,       /* fsin */
      0xd9, 0xff,       /* fcos */
      0xdd, 0x5d, 0xf8, /* fstp qword ptr [rbp-8] */
      0xf3, 0xa4,       /* rep movsb */
  };
  listing = nyr_internal_x64_disasm(x87_and_string_forms,
                                    sizeof(x87_and_string_forms), 0xc000);
  if (!strstr(listing, "49152\tfld\tqword ptr [rbp-8]\t3\n") ||
      !strstr(listing, "49155\tfsin\t\t2\n") ||
      !strstr(listing, "49157\tfcos\t\t2\n") ||
      !strstr(listing, "49159\tfstp\tqword ptr [rbp-8]\t3\n") ||
      !strstr(listing, "49162\trep movsb\t\t2\n") ||
      strstr(listing, "\tunsupported\t")) {
    fprintf(stderr, "[x64-disasm-scalar] x87/string forms mismatch:\n%s",
            listing);
    return 1;
  }
  static const unsigned char packed_integer_forms[] = {
      0x66, 0x0f, 0xd4, 0xc1,       /* paddq xmm0, xmm1 */
      0x66, 0x0f, 0xfb, 0xc1,       /* psubq xmm0, xmm1 */
      0x66, 0x0f, 0xdb, 0xc1,       /* pand xmm0, xmm1 */
      0x66, 0x0f, 0xeb, 0xc1,       /* por xmm0, xmm1 */
      0x66, 0x0f, 0xef, 0xc1,       /* pxor xmm0, xmm1 */
      0x66, 0x0f, 0xf3, 0xc1,       /* psllq xmm0, xmm1 */
      0x66, 0x0f, 0x73, 0xf0, 0x04, /* psllq xmm0, 4 */
  };
  listing = nyr_internal_x64_disasm(packed_integer_forms,
                                    sizeof(packed_integer_forms), 0xd000);
  if (!strstr(listing, "53248\tpaddq\txmm0, xmm1\t4\n") ||
      !strstr(listing, "53252\tpsubq\txmm0, xmm1\t4\n") ||
      !strstr(listing, "53256\tpand\txmm0, xmm1\t4\n") ||
      !strstr(listing, "53260\tpor\txmm0, xmm1\t4\n") ||
      !strstr(listing, "53264\tpxor\txmm0, xmm1\t4\n") ||
      !strstr(listing, "53268\tpsllq\txmm0, xmm1\t4\n") ||
      !strstr(listing, "53272\tpsllq\txmm0, 0x4\t5\n") ||
      strstr(listing, "\tunsupported\t")) {
    fprintf(stderr, "[x64-disasm-scalar] packed integer forms mismatch:\n%s",
            listing);
    return 1;
  }
  static const unsigned char scalar_vector_forms[] = {
      0x0f, 0x28, 0xc1,             /* movaps xmm0, xmm1 */
      0x0f, 0x10, 0x00,             /* movups xmm0, [rax] */
      0x66, 0x0f, 0x11, 0x08,       /* movupd [rax], xmm1 */
      0xf3, 0x0f, 0x51, 0xc1,       /* sqrtss xmm0, xmm1 */
      0xf2, 0x0f, 0x51, 0xc1,       /* sqrtsd xmm0, xmm1 */
      0x0f, 0x58, 0xc1,             /* addps xmm0, xmm1 */
      0x0f, 0x5c, 0xc1,             /* subps xmm0, xmm1 */
      0x0f, 0x59, 0xc1,             /* mulps xmm0, xmm1 */
      0x0f, 0x5e, 0xc1,             /* divps xmm0, xmm1 */
      0x0f, 0x12, 0xc1,             /* movhlps xmm0, xmm1 */
      0x0f, 0x16, 0xc1,             /* movlhps xmm0, xmm1 */
      0x0f, 0x12, 0x00,             /* movlps xmm0, qword ptr [rax] */
      0x0f, 0x17, 0x08,             /* movhps qword ptr [rax], xmm1 */
      0x66, 0x0f, 0x6e, 0xc0,       /* movd xmm0, eax */
      0x66, 0x48, 0x0f, 0x6e, 0xc0, /* movq xmm0, rax */
      0xf3, 0x0f, 0x2a, 0xc0,       /* cvtsi2ss xmm0, eax */
      0xf2, 0x48, 0x0f, 0x2a, 0xc0, /* cvtsi2sd xmm0, rax */
      0xf3, 0x0f, 0x2c, 0xc0,       /* cvttss2si eax, xmm0 */
      0xf3, 0x48, 0x0f, 0x2d, 0xc0, /* cvtss2si rax, xmm0 */
      0x0f, 0x2e, 0xc1,             /* ucomiss xmm0, xmm1 */
      0x0f, 0x2f, 0xc1,             /* comiss xmm0, xmm1 */
  };
  listing = nyr_internal_x64_disasm(scalar_vector_forms,
                                    sizeof(scalar_vector_forms), 0xe000);
  static const char *const scalar_vector_expected[] = {
      "\tmovaps\txmm0, xmm1\t",
      "\tmovups\txmm0, xmmword ptr [rax]\t",
      "\tmovupd\txmmword ptr [rax], xmm1\t",
      "\tsqrtss\txmm0, xmm1\t",
      "\tsqrtsd\txmm0, xmm1\t",
      "\taddps\txmm0, xmm1\t",
      "\tsubps\txmm0, xmm1\t",
      "\tmulps\txmm0, xmm1\t",
      "\tdivps\txmm0, xmm1\t",
      "\tmovhlps\txmm0, xmm1\t",
      "\tmovlhps\txmm0, xmm1\t",
      "\tmovlps\txmm0, qword ptr [rax]\t",
      "\tmovhps\tqword ptr [rax], xmm1\t",
      "\tmovd\txmm0, eax\t",
      "\tmovq\txmm0, rax\t",
      "\tcvtsi2ss\txmm0, eax\t",
      "\tcvtsi2sd\txmm0, rax\t",
      "\tcvttss2si\teax, xmm0\t",
      "\tcvtss2si\trax, xmm0\t",
      "\tucomiss\txmm0, xmm1\t",
      "\tcomiss\txmm0, xmm1\t",
  };
  for (size_t i = 0;
       i < sizeof(scalar_vector_expected) / sizeof(scalar_vector_expected[0]);
       ++i) {
    if (!strstr(listing, scalar_vector_expected[i])) {
      fprintf(stderr,
              "[x64-disasm-scalar] scalar/vector encoding mismatch:\n%s",
              listing);
      return 1;
    }
  }
  if (strstr(listing, "\tunsupported\t")) {
    fprintf(stderr,
            "[x64-disasm-scalar] emitted scalar/vector form rejected:\n%s",
            listing);
    return 1;
  }
  puts("[x64-disasm-scalar] scalar C-function forms decode exactly");
  return 0;
}
