/*
 * Bounded instruction decoder used by the Ny decompiler and disassembly tools.
 */
#include "shared.h"
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Mnemonic and operand buffers are fixed arrays in the decoder frames.
 */
#define DIS_SET(buf, text) snprintf((buf), sizeof(buf), "%s", (text))
#define DIS_APPEND(buf, text)                                                  \
  snprintf((buf) + strlen(buf), sizeof(buf) - strlen(buf), "%s", (text))

const char *nyr_internal_aarch64_disasm(const unsigned char *b, int64_t n,
                                        int64_t base);

/*
 * Small, fail-closed decoder used by the Ny decompiler.  The accepted
 * instruction set is deliberately explicit: adding an encoding here also
 * makes it available to the language-level decoder without a host library.
 */
static _Thread_local char *output = NULL;
static _Thread_local size_t output_cap = 0;
static _Thread_local const char *dis_x64_seg = NULL;

static bool output_ensure(size_t need) {
  if (output_cap >= need)
    return true;
  size_t cap = output_cap ? output_cap : 16384;
  while (cap < need) {
    if (cap > SIZE_MAX / 2)
      return false;
    cap *= 2;
  }
  char *nb = realloc(output, cap);
  if (!nb)
    return false;
  output = nb;
  output_cap = cap;
  return true;
}
static const char *const regs64[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp",
                                       "rsi", "rdi", "r8",  "r9",  "r10", "r11",
                                       "r12", "r13", "r14", "r15"};
static const char *const regs32[16] = {
    "eax", "ecx", "edx",  "ebx",  "esp",  "ebp",  "esi",  "edi",
    "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};
static const char *const regs16[16] = {
    "ax",  "cx",  "dx",   "bx",   "sp",   "bp",   "si",   "di",
    "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"};
static const char *const regs8_rex[16] = {
    "al",  "cl",  "dl",   "bl",   "spl",  "bpl",  "sil",  "dil",
    "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"};
static const char *const regs8_legacy[8] = {"al", "cl", "dl", "bl",
                                            "ah", "ch", "dh", "bh"};
const char *nyr_internal_x64_disasm(const unsigned char *b, int64_t n,
                                    int64_t base);

static uint64_t rd(const unsigned char *p, unsigned n) {
  uint64_t v = 0;
  for (unsigned i = 0; i < n; i++)
    v |= (uint64_t)p[i] << (8 * i);
  return v;
}

static const char *nyr_x64_reg(unsigned r, unsigned width, unsigned rex) {
  r &= 15;
  if (width == 1)
    return (rex ? regs8_rex : regs8_legacy)[r & (rex ? 15 : 7)];
  return width == 8 ? regs64[r] : width == 2 ? regs16[r] : regs32[r];
}

static int nyr_x64_rm(const unsigned char *b, int64_t n, int64_t *ip,
                      unsigned modrm, unsigned rex, unsigned width, char *out,
                      size_t out_size) {
  unsigned mod = modrm >> 6;
  unsigned rm = (modrm & 7) | ((rex & 1) ? 8 : 0);
  if (mod == 3) {
    snprintf(out, out_size, "%s", nyr_x64_reg(rm, width, rex));
    return 1;
  }
  char expr[160] = "";
  int64_t disp_value = 0;
  unsigned disp_size = mod == 1 ? 1 : mod == 2 ? 4 : 0;
  int rip_relative = mod == 0 && (modrm & 7) == 5;
  if ((modrm & 7) == 4) {
    if (*ip >= n)
      return 0;
    unsigned sib = b[(*ip)++];
    unsigned index = ((sib >> 3) & 7) | ((rex & 2) ? 8 : 0);
    unsigned base = (sib & 7) | ((rex & 1) ? 8 : 0);
    unsigned scale = 1u << (sib >> 6);
    int has_index = ((sib >> 3) & 7) != 4 || (rex & 2);
    int has_base = !(mod == 0 && (sib & 7) == 5 && !(rex & 1));
    if (has_base) {
      DIS_SET(expr, regs64[base]);
      if (has_index) {
        char idx[32];
        if (scale != 1)
          snprintf(idx, sizeof(idx), "%s*%u", regs64[index], scale);
        else
          snprintf(idx, sizeof(idx), "%s", regs64[index]);
        strncat(expr, "+", sizeof(expr) - strlen(expr) - 1);
        strncat(expr, idx, sizeof(expr) - strlen(expr) - 1);
      }
    } else if (has_index) {
      if (scale != 1)
        snprintf(expr, sizeof(expr), "%s*%u", regs64[index], scale);
      else
        snprintf(expr, sizeof(expr), "%s", regs64[index]);
    }
    if (!has_base)
      disp_size = 4;
  } else if (rip_relative) {
    DIS_SET(expr, "rip");
    disp_size = 4;
  } else {
    snprintf(expr, sizeof(expr), "%s", regs64[rm]);
  }
  if (disp_size) {
    if (*ip + disp_size > n)
      return 0;
    disp_value = disp_size == 1 ? (int8_t)b[*ip] : (int32_t)rd(b + *ip, 4);
    *ip += disp_size;
  }
  const char *sp = dis_x64_seg ? dis_x64_seg : "";
  if (width == 0) {
    if (disp_size && expr[0])
      snprintf(out, out_size, "%s[%s%s%" PRId64 "]", sp, expr,
               disp_value >= 0 ? "+" : "", disp_value);
    else if (disp_size)
      snprintf(out, out_size, "%s[%" PRId64 "]", sp, disp_value);
    else
      snprintf(out, out_size, "%s[%s]", sp, expr);
  } else if (disp_size && expr[0]) {
    snprintf(out, out_size, "%s ptr %s[%s%s%" PRId64 "]",
             width == 8   ? "qword"
             : width == 2 ? "word"
             : width == 1 ? "byte"
                          : "dword",
             sp, expr, disp_value >= 0 ? "+" : "", disp_value);
  } else if (disp_size) {
    snprintf(out, out_size, "%s ptr %s[%" PRId64 "]",
             width == 8   ? "qword"
             : width == 2 ? "word"
             : width == 1 ? "byte"
                          : "dword",
             sp, disp_value);
  } else {
    snprintf(out, out_size, "%s ptr %s[%s]",
             width == 8   ? "qword"
             : width == 2 ? "word"
             : width == 1 ? "byte"
                          : "dword",
             sp, expr);
  }
  return 1;
}

static int nyr_x64_vec_rm(const unsigned char *b, int64_t n, int64_t *ip,
                          unsigned modrm, unsigned rex, unsigned vector_width,
                          unsigned memory_width, char *out, size_t out_size) {
  if ((modrm >> 6) == 3) {
    unsigned reg = (modrm & 7) | ((rex & 1) ? 8 : 0);
    snprintf(out, out_size, "%s%u", vector_width == 256 ? "ymm" : "xmm", reg);
    return 1;
  }
  char address[160];
  if (!nyr_x64_rm(b, n, ip, modrm, rex, 0, address, sizeof(address)))
    return 0;
  const char *size = memory_width == 1     ? "byte"
                     : memory_width == 2   ? "word"
                     : memory_width == 4   ? "dword"
                     : memory_width == 8   ? "qword"
                     : vector_width == 256 ? "ymmword"
                                           : "xmmword";
  snprintf(out, out_size, "%s ptr %s", size, address);
  return 1;
}

const char *nyr_internal_x64_disasm(const unsigned char *b, int64_t n,
                                    int64_t base) {
  size_t used = 0;
  if (!output_ensure(256))
    return "";
  output[0] = 0;
  if (!b || n < 0)
    return "";
  for (int64_t i = 0; i < n;) {
    int64_t start = i;
    unsigned char op = b[i++];
    unsigned rex = 0, op16 = 0, rep = 0, lockp = 0;
    const char *seg = NULL;
    while (op == 0x66 || op == 0x67 || op == 0xf0 || op == 0xf3 || op == 0xf2 ||
           op == 0x26 || op == 0x2e || op == 0x36 || op == 0x3e || op == 0x64 ||
           op == 0x65 || (op >= 0x40 && op <= 0x4f)) {
      if (op == 0x66)
        op16 = 1;
      else if (op == 0x67) {
      } else if (op == 0xf0)
        lockp = 1;
      else if (op == 0xf3 || op == 0xf2)
        rep = op;
      else if (op == 0x26)
        seg = "es:";
      else if (op == 0x2e)
        seg = "cs:";
      else if (op == 0x36)
        seg = "ss:";
      else if (op == 0x3e)
        seg = "ds:";
      else if (op == 0x64)
        seg = "fs:";
      else if (op == 0x65)
        seg = "gs:";
      else if (op >= 0x40 && op <= 0x4f)
        rex = op;
      if (i >= n)
        goto unsupported;
      op = b[i++];
    }
    dis_x64_seg = seg;
    unsigned rb = (rex & 1) ? 8 : 0;
    char m[32] = "", args[512] = "";
    int reg = 0;
    int64_t rel = 0;
    unsigned imm = 0;
    unsigned w = (rex & 8) ? 8 : (op16 ? 2 : 4);
    if (op == 0x90)
      DIS_SET(m, rep == 0xf3 ? "pause" : "nop");
    else if (op == 0xf1)
      DIS_SET(m, "int1");
    else if (op == 0xf4)
      DIS_SET(m, "hlt");
    else if (op == 0xf5)
      DIS_SET(m, "cmc");
    else if (op == 0xf8)
      DIS_SET(m, "clc");
    else if (op == 0xf9)
      DIS_SET(m, "stc");
    else if (op == 0xfa)
      DIS_SET(m, "cli");
    else if (op == 0xfb)
      DIS_SET(m, "sti");
    else if (op == 0xfc)
      DIS_SET(m, "cld");
    else if (op == 0xfd)
      DIS_SET(m, "std");
    else if (op == 0x9b)
      DIS_SET(m, "wait");
    else if (op == 0x9c)
      DIS_SET(m, w == 8 ? "pushfq" : "pushf");
    else if (op == 0x9d)
      DIS_SET(m, w == 8 ? "popfq" : "popf");
    else if (op == 0x9e)
      DIS_SET(m, "sahf");
    else if (op == 0x9f)
      DIS_SET(m, "lahf");
    else if (op == 0xd7)
      DIS_SET(m, "xlatb");
    else if (op == 0xcd) {
      if (i >= n)
        goto unsupported;
      DIS_SET(m, "int");
      snprintf(args, sizeof(args), "0x%x", b[i++]);
    } else if (op == 0xce)
      DIS_SET(m, "into");
    else if (op == 0xcf)
      DIS_SET(m, (rex & 8) ? "iretq" : op16 ? "iretw" : "iretd");
    else if (op == 0xa0 || op == 0xa1 || op == 0xa2 || op == 0xa3) {
      /*
       * mov moffs accumulator forms
       */
      unsigned addr_sz = 8; /* simplified: 64-bit mode default */
      imm = addr_sz;
      if (i + imm > n)
        goto unsupported;
      uint64_t addr = rd(b + i, imm);
      i += imm;
      unsigned width = (op & 1) ? w : 1;
      DIS_SET(m, "mov");
      if (op == 0xa0 || op == 0xa1)
        snprintf(args, sizeof(args), "%s, %s ptr [0x%" PRIx64 "]",
                 nyr_x64_reg(0, width, rex),
                 width == 8   ? "qword"
                 : width == 2 ? "word"
                 : width == 1 ? "byte"
                              : "dword",
                 addr);
      else
        snprintf(args, sizeof(args), "%s ptr [0x%" PRIx64 "], %s",
                 width == 8   ? "qword"
                 : width == 2 ? "word"
                 : width == 1 ? "byte"
                              : "dword",
                 addr, nyr_x64_reg(0, width, rex));
    } else if ((op >= 0xa4 && op <= 0xa7) || (op >= 0xaa && op <= 0xaf) ||
               (op >= 0x6c && op <= 0x6f)) {
      const char *string_op = NULL;
      const char *pfx = "";
      switch (op) {
      case 0xa4:
        string_op = "movsb";
        pfx = rep == 0xf3 ? "rep " : "";
        break;
      case 0xa5:
        string_op = (rex & 8) ? "movsq" : op16 ? "movsw" : "movsd";
        pfx = rep == 0xf3 ? "rep " : "";
        break;
      case 0xa6:
        string_op = "cmpsb";
        pfx = rep == 0xf3 ? "repe " : rep == 0xf2 ? "repne " : "";
        break;
      case 0xa7:
        string_op = (rex & 8) ? "cmpsq" : op16 ? "cmpsw" : "cmpsd";
        pfx = rep == 0xf3 ? "repe " : rep == 0xf2 ? "repne " : "";
        break;
      case 0xaa:
        string_op = "stosb";
        pfx = rep == 0xf3 ? "rep " : "";
        break;
      case 0xab:
        string_op = (rex & 8) ? "stosq" : op16 ? "stosw" : "stosd";
        pfx = rep == 0xf3 ? "rep " : "";
        break;
      case 0xac:
        string_op = "lodsb";
        pfx = rep == 0xf3 ? "rep " : "";
        break;
      case 0xad:
        string_op = (rex & 8) ? "lodsq" : op16 ? "lodsw" : "lodsd";
        pfx = rep == 0xf3 ? "rep " : "";
        break;
      case 0xae:
        string_op = "scasb";
        pfx = rep == 0xf3 ? "repe " : rep == 0xf2 ? "repne " : "";
        break;
      case 0xaf:
        string_op = (rex & 8) ? "scasq" : op16 ? "scasw" : "scasd";
        pfx = rep == 0xf3 ? "repe " : rep == 0xf2 ? "repne " : "";
        break;
      case 0x6c:
        string_op = "insb";
        pfx = rep == 0xf3 ? "rep " : "";
        break;
      case 0x6d:
        string_op = (rex & 8) ? "insq" : op16 ? "insw" : "insd";
        pfx = rep == 0xf3 ? "rep " : "";
        break;
      case 0x6e:
        string_op = "outsb";
        pfx = rep == 0xf3 ? "rep " : "";
        break;
      case 0x6f:
        string_op = (rex & 8) ? "outsq" : op16 ? "outsw" : "outsd";
        pfx = rep == 0xf3 ? "rep " : "";
        break;
      default:
        break;
      }
      if (string_op)
        snprintf(m, sizeof(m), "%s%s", pfx, string_op);
      else
        goto unsupported;
    } else if (op == 0xd9 && i < n && (b[i] == 0xfe || b[i] == 0xff)) {
      DIS_SET(m, b[i++] == 0xfe ? "fsin" : "fcos");
    } else if (op == 0xdd && i < n) {
      unsigned char mr = b[i++];
      unsigned group = (mr >> 3) & 7;
      char operand[128];
      if ((mr >> 6) == 3 || (group != 0 && group != 3) ||
          !nyr_x64_rm(b, n, &i, mr, rex, 8, operand, sizeof(operand)))
        goto unsupported;
      DIS_SET(m, group == 0 ? "fld" : "fstp");
      snprintf(args, sizeof(args), "%s", operand);
    } else if (op == 0xc3)
      DIS_SET(m, "ret");
    else if (op == 0xc2) {
      if (i + 2 > n)
        goto unsupported;
      unsigned imm16 = b[i] | ((unsigned)b[i + 1] << 8);
      i += 2;
      DIS_SET(m, "ret");
      snprintf(args, sizeof(args), "0x%x", imm16);
    } else if (op == 0xcb)
      DIS_SET(m, "retf");
    else if (op == 0xca) {
      if (i + 2 > n)
        goto unsupported;
      unsigned imm16 = b[i] | ((unsigned)b[i + 1] << 8);
      i += 2;
      DIS_SET(m, "retf");
      snprintf(args, sizeof(args), "0x%x", imm16);
    } else if (op == 0xc8) {
      /*
       * enter imm16, imm8
       */
      if (i + 3 > n)
        goto unsupported;
      unsigned imm16 = b[i] | ((unsigned)b[i + 1] << 8);
      unsigned imm8 = b[i + 2];
      i += 3;
      DIS_SET(m, "enter");
      snprintf(args, sizeof(args), "0x%x, 0x%x", imm16, imm8);
    } else if (op == 0xc9)
      DIS_SET(m, "leave");
    else if (op == 0xcc)
      DIS_SET(m, "int3");
    else if (op == 0x98)
      DIS_SET(m, (rex & 8) ? "cdqe" : "cwde");
    else if (op == 0x99)
      DIS_SET(m, (rex & 8) ? "cqo" : "cdq");
    else if (op == 0x0f && i < n) {
      unsigned char op2 = b[i++];
      static const char *cc[] = {"o", "no", "b", "ae", "e", "ne", "be", "a",
                                 "s", "ns", "p", "np", "l", "ge", "le", "g"};
      if ((op2 == 0x10 || op2 == 0x11 || op2 == 0x28 || op2 == 0x29) && i < n &&
          !(op2 >= 0x28 && rep)) {
        unsigned char mr = b[i++];
        unsigned scalar_width = rep == 0xf3 ? 4 : rep == 0xf2 ? 8 : 0;
        unsigned vector_width = 128;
        char reg_name[16], rm_name[192];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        snprintf(reg_name, sizeof(reg_name), "xmm%u", reg);
        unsigned mem_width = scalar_width ? scalar_width : 16;
        if (!nyr_x64_vec_rm(b, n, &i, mr, rex, vector_width, mem_width, rm_name,
                            sizeof(rm_name)))
          goto unsupported;
        if (op2 == 0x10 || op2 == 0x11) {
          DIS_SET(m, scalar_width ? (rep == 0xf3 ? "movss" : "movsd")
                                  : (op16 ? "movupd" : "movups"));
          snprintf(args, sizeof(args), "%s, %s",
                   op2 == 0x10 ? reg_name : rm_name,
                   op2 == 0x10 ? rm_name : reg_name);
        } else {
          DIS_SET(m, op16 ? "movapd" : "movaps");
          snprintf(args, sizeof(args), "%s, %s",
                   op2 == 0x28 ? reg_name : rm_name,
                   op2 == 0x28 ? rm_name : reg_name);
        }
      } else if ((op2 == 0x6f || op2 == 0x7f) && i < n &&
                 ((rep == 0xf3 && !op16) || (op16 && !rep))) {
        unsigned char mr = b[i++];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        char reg_name[16], rm_name[192];
        snprintf(reg_name, sizeof(reg_name), "xmm%u", reg);
        if (!nyr_x64_vec_rm(b, n, &i, mr, rex, 128, 16, rm_name,
                            sizeof(rm_name)))
          goto unsupported;
        DIS_SET(m, rep == 0xf3 ? "movdqu" : "movdqa");
        snprintf(args, sizeof(args), "%s, %s", op2 == 0x6f ? reg_name : rm_name,
                 op2 == 0x6f ? rm_name : reg_name);
      } else if ((op2 == 0x6e || op2 == 0x7e) && i < n && op16 && !rep) {
        unsigned char mr = b[i++];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        unsigned move_width = (rex & 8) ? 8 : 4;
        char reg_name[16], rm_name[192];
        snprintf(reg_name, sizeof(reg_name), "xmm%u", reg);
        if (!nyr_x64_rm(b, n, &i, mr, rex, move_width, rm_name,
                        sizeof(rm_name)))
          goto unsupported;
        DIS_SET(m, move_width == 8 ? "movq" : "movd");
        snprintf(args, sizeof(args), "%s, %s", op2 == 0x6e ? reg_name : rm_name,
                 op2 == 0x6e ? rm_name : reg_name);
      } else if ((op2 == 0x14 || op2 == 0x15) && i < n && !rep) {
        unsigned char mr = b[i++];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        char reg_name[16], rm_name[192];
        snprintf(reg_name, sizeof(reg_name), "xmm%u", reg);
        if (!nyr_x64_vec_rm(b, n, &i, mr, rex, 128, 16, rm_name,
                            sizeof(rm_name)))
          goto unsupported;
        DIS_SET(m, op16 ? (op2 == 0x14 ? "unpcklpd" : "unpckhpd")
                        : (op2 == 0x14 ? "unpcklps" : "unpckhps"));
        snprintf(args, sizeof(args), "%s, %s", reg_name, rm_name);
      } else if ((op2 == 0x12 || op2 == 0x13 || op2 == 0x16 || op2 == 0x17) &&
                 i < n && !op16) {
        unsigned char mr = b[i++];
        unsigned modrm_mod = mr >> 6;
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        char reg_name[16], rm_name[192];
        snprintf(reg_name, sizeof(reg_name), "xmm%u", reg);
        if (!nyr_x64_vec_rm(b, n, &i, mr, rex, 128, 8, rm_name,
                            sizeof(rm_name)))
          goto unsupported;
        if (rep == 0xf2 && op2 == 0x12)
          DIS_SET(m, "movddup");
        else if (rep == 0xf3 && op2 == 0x12)
          DIS_SET(m, "movsldup");
        else if (rep == 0xf3 && op2 == 0x16)
          DIS_SET(m, "movshdup");
        else if (rep)
          goto unsupported;
        else if (modrm_mod == 3 && op2 == 0x12)
          DIS_SET(m, "movhlps");
        else if (modrm_mod == 3 && op2 == 0x16)
          DIS_SET(m, "movlhps");
        else if (op2 == 0x12 || op2 == 0x13)
          DIS_SET(m, "movlps");
        else
          DIS_SET(m, "movhps");
        int store = op2 == 0x13 || op2 == 0x17;
        snprintf(args, sizeof(args), "%s, %s", store ? rm_name : reg_name,
                 store ? reg_name : rm_name);
      } else if ((op2 == 0x58 || op2 == 0x5c || op2 == 0x59 || op2 == 0x5e ||
                  op2 == 0x51) &&
                 i < n) {
        unsigned char mr = b[i++];
        unsigned scalar_width = rep == 0xf3 ? 4 : rep == 0xf2 ? 8 : 0;
        const char *suffix = rep == 0xf3   ? "ss"
                             : rep == 0xf2 ? "sd"
                             : op16        ? "pd"
                                           : "ps";
        const char *stem = op2 == 0x58   ? "add"
                           : op2 == 0x5c ? "sub"
                           : op2 == 0x59 ? "mul"
                           : op2 == 0x5e ? "div"
                                         : "sqrt";
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        char reg_name[16], rm_name[192];
        snprintf(reg_name, sizeof(reg_name), "xmm%u", reg);
        if (!nyr_x64_vec_rm(b, n, &i, mr, rex, 128,
                            scalar_width ? scalar_width : 16, rm_name,
                            sizeof(rm_name)))
          goto unsupported;
        DIS_SET(m, stem);
        DIS_APPEND(m, suffix);
        if (op2 == 0x51)
          snprintf(args, sizeof(args), "%s, %s", reg_name, rm_name);
        else
          snprintf(args, sizeof(args), "%s, %s", reg_name, rm_name);
      } else if (op2 == 0xc6 && i < n && !rep) {
        unsigned char mr = b[i++];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        char reg_name[16], rm_name[192];
        snprintf(reg_name, sizeof(reg_name), "xmm%u", reg);
        if (!nyr_x64_vec_rm(b, n, &i, mr, rex, 128, 16, rm_name,
                            sizeof(rm_name)) ||
            i >= n)
          goto unsupported;
        DIS_SET(m, op16 ? "shufpd" : "shufps");
        snprintf(args, sizeof(args), "%s, %s, 0x%x", reg_name, rm_name, b[i++]);
      } else if ((op2 == 0x2e || op2 == 0x2f) && i < n && !rep) {
        unsigned char mr = b[i++];
        char reg_name[16], rm_name[192];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        snprintf(reg_name, sizeof(reg_name), "xmm%u", reg);
        if (!nyr_x64_vec_rm(b, n, &i, mr, rex, 128, op16 ? 8 : 4, rm_name,
                            sizeof(rm_name)))
          goto unsupported;
        DIS_SET(m, op2 == 0x2e ? (op16 ? "ucomisd" : "ucomiss")
                               : (op16 ? "comisd" : "comiss"));
        snprintf(args, sizeof(args), "%s, %s", reg_name, rm_name);
      } else if (op2 == 0x6c && op16 && i < n) {
        unsigned char mr = b[i++];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        char reg_name[16], rm_name[192];
        snprintf(reg_name, sizeof(reg_name), "xmm%u", reg);
        if (!nyr_x64_vec_rm(b, n, &i, mr, rex, 128, 16, rm_name,
                            sizeof(rm_name)))
          goto unsupported;
        DIS_SET(m, "punpcklqdq");
        snprintf(args, sizeof(args), "%s, %s", reg_name, rm_name);
      } else if (op2 == 0x70 && op16 && !rep && i < n) {
        unsigned char mr = b[i++];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        char reg_name[16], rm_name[192];
        snprintf(reg_name, sizeof(reg_name), "xmm%u", reg);
        if (!nyr_x64_vec_rm(b, n, &i, mr, rex, 128, 16, rm_name,
                            sizeof(rm_name)) ||
            i >= n)
          goto unsupported;
        DIS_SET(m, "pshufd");
        snprintf(args, sizeof(args), "%s, %s, 0x%x", reg_name, rm_name, b[i++]);
      } else if (op16 && !rep && i < n &&
                 (op2 == 0xd4 || op2 == 0xfb || op2 == 0xdb || op2 == 0xeb ||
                  op2 == 0xef || op2 == 0xf3)) {
        unsigned char mr = b[i++];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        char reg_name[16], rm_name[192];
        snprintf(reg_name, sizeof(reg_name), "xmm%u", reg);
        if (!nyr_x64_vec_rm(b, n, &i, mr, rex, 128, 16, rm_name,
                            sizeof(rm_name)))
          goto unsupported;
        DIS_SET(m, op2 == 0xd4   ? "paddq"
                   : op2 == 0xfb ? "psubq"
                   : op2 == 0xdb ? "pand"
                   : op2 == 0xeb ? "por"
                   : op2 == 0xef ? "pxor"
                                 : "psllq");
        snprintf(args, sizeof(args), "%s, %s", reg_name, rm_name);
      } else if (op2 == 0x73 && op16 && !rep && i + 2 <= n) {
        unsigned char mr = b[i++];
        if ((mr >> 6) != 3 || ((mr >> 3) & 7) != 6)
          goto unsupported;
        unsigned dest = (mr & 7) | ((rex & 1) ? 8 : 0);
        DIS_SET(m, "psllq");
        snprintf(args, sizeof(args), "xmm%u, 0x%x", dest, b[i++]);
      } else if (op2 == 0x12 && rep == 0xf2 && i < n) {
        unsigned char mr = b[i++];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        char reg_name[16], rm_name[192];
        snprintf(reg_name, sizeof(reg_name), "xmm%u", reg);
        if (!nyr_x64_vec_rm(b, n, &i, mr, rex, 128, 8, rm_name,
                            sizeof(rm_name)))
          goto unsupported;
        DIS_SET(m, "movddup");
        snprintf(args, sizeof(args), "%s, %s", reg_name, rm_name);
      } else if ((op2 == 0x2a || op2 == 0x2c || op2 == 0x2d) &&
                 (rep == 0xf2 || rep == 0xf3) && i < n) {
        unsigned char mr = b[i++];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        if (op2 == 0x2a) {
          char src[128];
          if (!nyr_x64_rm(b, n, &i, mr, rex, w, src, sizeof(src)))
            goto unsupported;
          snprintf(m, sizeof(m), "cvtsi2%s", rep == 0xf3 ? "ss" : "sd");
          snprintf(args, sizeof(args), "xmm%d, %s", reg, src);
        } else {
          char src[192];
          if (!nyr_x64_vec_rm(b, n, &i, mr, rex, 128, rep == 0xf3 ? 4 : 8, src,
                              sizeof(src)))
            goto unsupported;
          snprintf(m, sizeof(m), "%s%s2si", op2 == 0x2c ? "cvtt" : "cvt",
                   rep == 0xf3 ? "ss" : "sd");
          snprintf(args, sizeof(args), "%s, %s", nyr_x64_reg(reg, w, rex), src);
        }
      } else if (op2 == 0x5a && i < n && (rep == 0xf2 || rep == 0xf3)) {
        unsigned char mr = b[i++];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        char reg_name[16], src[192];
        snprintf(reg_name, sizeof(reg_name), "xmm%u", reg);
        if (!nyr_x64_vec_rm(b, n, &i, mr, rex, 128, rep == 0xf3 ? 4 : 8, src,
                            sizeof(src)))
          goto unsupported;
        DIS_SET(m, rep == 0xf3 ? "cvtss2sd" : "cvtsd2ss");
        snprintf(args, sizeof(args), "%s, %s", reg_name, src);
      } else if (op2 >= 0x80 && op2 <= 0x8f && i + 4 <= n) {
        rel = (int32_t)rd(b + i, 4);
        i += 4;
        DIS_SET(m, "j");
        DIS_APPEND(m, cc[op2 & 15]);
        snprintf(args, sizeof(args), "0x%" PRIx64, (uint64_t)(base + i + rel));
      } else if (op2 >= 0x40 && op2 <= 0x4f && i < n) {
        unsigned char mr = b[i++];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        char src[128];
        if (!nyr_x64_rm(b, n, &i, mr, rex, w, src, sizeof(src)))
          goto unsupported;
        DIS_SET(m, "cmov");
        DIS_APPEND(m, cc[op2 & 15]);
        snprintf(args, sizeof(args), "%s, %s", nyr_x64_reg(reg, w, rex), src);
      } else if (op2 >= 0x90 && op2 <= 0x9f && i < n) {
        unsigned char mr = b[i++];
        char dst[128];
        if (!nyr_x64_rm(b, n, &i, mr, rex, 1, dst, sizeof(dst)))
          goto unsupported;
        DIS_SET(m, "set");
        DIS_APPEND(m, cc[op2 & 15]);
        snprintf(args, sizeof(args), "%s", dst);
      } else if (op2 == 0xaf && i < n) {
        unsigned char mr = b[i++];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        char src[128];
        if (!nyr_x64_rm(b, n, &i, mr, rex, w, src, sizeof(src)))
          goto unsupported;
        DIS_SET(m, "imul");
        snprintf(args, sizeof(args), "%s, %s", nyr_x64_reg(reg, w, rex), src);
      } else if ((op2 == 0xb6 || op2 == 0xb7 || op2 == 0xbe || op2 == 0xbf) &&
                 i < n) {
        unsigned char mr = b[i++];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        unsigned sw = (op2 == 0xb6 || op2 == 0xbe) ? 1 : 2;
        char src[128];
        if (!nyr_x64_rm(b, n, &i, mr, rex, sw, src, sizeof(src)))
          goto unsupported;
        DIS_SET(m, (op2 == 0xb6 || op2 == 0xb7) ? "movzx" : "movsx");
        snprintf(args, sizeof(args), "%s, %s", nyr_x64_reg(reg, w, rex), src);
      } else if (op2 == 0x1f && i < n) {
        unsigned char mr = b[i++];
        char discard[128];
        if (!nyr_x64_rm(b, n, &i, mr, rex, 0, discard, sizeof(discard)))
          goto unsupported;
        DIS_SET(m, "nop");
      } else if (op2 == 0x1e && rep == 0xf3 && i < n && b[i++] == 0xfa) {
        DIS_SET(m, "endbr64");
      } else if (op2 == 0x05) {
        DIS_SET(m, "syscall");
      } else if (op2 == 0x07) {
        DIS_SET(m, "sysret");
      } else if (op2 == 0x0b) {
        DIS_SET(m, "ud2");
      } else if (op2 == 0x31) {
        DIS_SET(m, "rdtsc");
      } else if (op2 == 0x32) {
        DIS_SET(m, "rdmsr");
      } else if (op2 == 0x30) {
        DIS_SET(m, "wrmsr");
      } else if (op2 == 0x34) {
        DIS_SET(m, "sysenter");
      } else if (op2 == 0x35) {
        DIS_SET(m, "sysexit");
      } else if (op2 == 0xa2) {
        DIS_SET(m, "cpuid");
      } else if (op2 >= 0xc8 && op2 <= 0xcf) {
        DIS_SET(m, "bswap");
        snprintf(args, sizeof(args), "%s", nyr_x64_reg((op2 & 7) + rb, w, rex));
      } else if ((op2 == 0xb0 || op2 == 0xb1) && i < n) {
        unsigned char mr = b[i++];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        unsigned width = op2 == 0xb0 ? 1 : w;
        char rmop[128];
        if (!nyr_x64_rm(b, n, &i, mr, rex, width, rmop, sizeof(rmop)))
          goto unsupported;
        DIS_SET(m, lockp ? "lock cmpxchg" : "cmpxchg");
        lockp = 0;
        snprintf(args, sizeof(args), "%s, %s", rmop,
                 nyr_x64_reg(reg, width, rex));
      } else if ((op2 == 0xc0 || op2 == 0xc1) && i < n) {
        unsigned char mr = b[i++];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        unsigned width = op2 == 0xc0 ? 1 : w;
        char rmop[128];
        if (!nyr_x64_rm(b, n, &i, mr, rex, width, rmop, sizeof(rmop)))
          goto unsupported;
        DIS_SET(m, lockp ? "lock xadd" : "xadd");
        lockp = 0;
        snprintf(args, sizeof(args), "%s, %s", rmop,
                 nyr_x64_reg(reg, width, rex));
      } else if ((op2 == 0xb8 || op2 == 0xbc || op2 == 0xbd) && i < n &&
                 (rep == 0xf3 || op2 != 0xb8)) {
        unsigned char mr = b[i++];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        char src[128];
        if (!nyr_x64_rm(b, n, &i, mr, rex, w, src, sizeof(src)))
          goto unsupported;
        if (rep == 0xf3 && op2 == 0xb8) {
          DIS_SET(m, "popcnt");
          rep = 0;
        } else if (rep == 0xf3) {
          DIS_SET(m, op2 == 0xbc ? "tzcnt" : "lzcnt");
          rep = 0;
        } else if (op16 && op2 == 0xbd) {
          DIS_SET(m, "lzcnt");
        } else {
          DIS_SET(m, op2 == 0xbc ? "bsf" : op2 == 0xbd ? "bsr" : "popcnt");
        }
        snprintf(args, sizeof(args), "%s, %s", nyr_x64_reg(reg, w, rex), src);
      } else if (op2 == 0xa3 && i < n) {
        unsigned char mr = b[i++];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        char rmop[128];
        if (!nyr_x64_rm(b, n, &i, mr, rex, w, rmop, sizeof(rmop)))
          goto unsupported;
        DIS_SET(m, "bt");
        snprintf(args, sizeof(args), "%s, %s", rmop, nyr_x64_reg(reg, w, rex));
      } else if (op2 == 0xab && i < n) {
        unsigned char mr = b[i++];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        char rmop[128];
        if (!nyr_x64_rm(b, n, &i, mr, rex, w, rmop, sizeof(rmop)))
          goto unsupported;
        DIS_SET(m, "bts");
        snprintf(args, sizeof(args), "%s, %s", rmop, nyr_x64_reg(reg, w, rex));
      } else if (op2 == 0xb3 && i < n) {
        unsigned char mr = b[i++];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        char rmop[128];
        if (!nyr_x64_rm(b, n, &i, mr, rex, w, rmop, sizeof(rmop)))
          goto unsupported;
        DIS_SET(m, "btr");
        snprintf(args, sizeof(args), "%s, %s", rmop, nyr_x64_reg(reg, w, rex));
      } else if (op2 == 0xbb && i < n) {
        unsigned char mr = b[i++];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        char rmop[128];
        if (!nyr_x64_rm(b, n, &i, mr, rex, w, rmop, sizeof(rmop)))
          goto unsupported;
        DIS_SET(m, "btc");
        snprintf(args, sizeof(args), "%s, %s", rmop, nyr_x64_reg(reg, w, rex));
      } else if (op2 == 0xba && i < n) {
        unsigned char mr = b[i++];
        unsigned grp = (mr >> 3) & 7;
        char rmop[128];
        if (!nyr_x64_rm(b, n, &i, mr, rex, w, rmop, sizeof(rmop)) || i >= n)
          goto unsupported;
        const char *nm = grp == 4   ? "bt"
                         : grp == 5 ? "bts"
                         : grp == 6 ? "btr"
                         : grp == 7 ? "btc"
                                    : NULL;
        if (!nm)
          goto unsupported;
        DIS_SET(m, nm);
        snprintf(args, sizeof(args), "%s, 0x%x", rmop, b[i++]);
      } else if (op2 == 0x01 && i < n) {
        unsigned char mr = b[i++];
        unsigned grp = (mr >> 3) & 7;
        if ((mr >> 6) == 3 && mr == 0xf9) {
          DIS_SET(m, "rdtscp");
        } else if ((mr >> 6) == 3 && mr == 0xf8) {
          DIS_SET(m, "swapgs");
        } else if (grp == 2) {
          DIS_SET(m, "lgdt");
        } else if (grp == 3) {
          DIS_SET(m, "lidt");
        } else if (grp == 7) {
          DIS_SET(m, "invlpg");
        } else {
          goto unsupported;
        }
      } else if (op2 == 0xae && i < n) {
        unsigned char mr = b[i++];
        unsigned grp = (mr >> 3) & 7;
        if ((mr >> 6) == 3) {
          if (grp == 5)
            DIS_SET(m, "lfence");
          else if (grp == 6)
            DIS_SET(m, "mfence");
          else if (grp == 7)
            DIS_SET(m, "sfence");
          else
            goto unsupported;
        } else {
          char rmop[128];
          if (!nyr_x64_rm(b, n, &i, mr, rex, 0, rmop, sizeof(rmop)))
            goto unsupported;
          if (grp == 7)
            DIS_SET(m, "clflush");
          else
            goto unsupported;
          snprintf(args, sizeof(args), "%s", rmop);
        }
      } else if ((op2 == 0xc7) && i < n) {
        unsigned char mr = b[i++];
        unsigned grp = (mr >> 3) & 7;
        if (grp == 1) {
          char rmop[128];
          if (!nyr_x64_rm(b, n, &i, mr, rex, 0, rmop, sizeof(rmop)))
            goto unsupported;
          DIS_SET(m, (rex & 8) ? "cmpxchg16b" : "cmpxchg8b");
          snprintf(args, sizeof(args), "%s", rmop);
        } else
          goto unsupported;
      } else {
        goto unsupported;
      }
    } else if (op == 0xc4 || op == 0xc5) {
      unsigned vex2, vex3, map, opcode;
      if (op == 0xc4) {
        if (i + 3 > n)
          goto unsupported;
        vex2 = b[i++];
        vex3 = b[i++];
        map = vex2 & 0x1f;
        opcode = b[i++];
      } else {
        if (i + 2 > n)
          goto unsupported;
        unsigned vex2_byte = b[i++];
        vex2 = (vex2_byte & 0x80) | 0x61;
        vex3 = vex2_byte & 0x7f;
        map = 1;
        opcode = b[i++];
      }
      unsigned pp = vex3 & 3;
      unsigned vex_rex = (unsigned)(~vex2 >> 5) & 7;
      /*
       * VZEROUPPER/VZEROALL have no ModRM byte.
       */
      if (map == 1 && opcode == 0x77 && pp == 0 && (vex3 & 0x78) == 0x78 &&
          !(vex3 & 0x80)) {
        DIS_SET(m, (vex3 & 4) ? "vzeroall" : "vzeroupper");
        goto decoded;
      }
      if (i >= n)
        goto unsupported;
      unsigned char mr = b[i++];
      /*
       * vmovq/vmovd (VEX.66.0F 6E/7E): xmm <-> full-width gpr move.
       */
      if (map == 1 && (opcode == 0x6e || opcode == 0x7e) && pp == 1) {
        unsigned gw = (vex3 & 0x80) ? 8 : 4;
        char vreg[16], rmb[192];
        unsigned vreg_n = ((mr >> 3) & 7) + ((vex_rex & 4) ? 8 : 0);
        snprintf(vreg, sizeof(vreg), "xmm%u", vreg_n);
        if ((mr >> 6) == 3) {
          snprintf(rmb, sizeof(rmb), "%s",
                   nyr_x64_reg((mr & 7) + ((vex_rex & 1) ? 8 : 0), gw,
                               (vex_rex & 1)));
        } else {
          if (!nyr_x64_rm(b, n, &i, mr,
                          (unsigned)(vex_rex & 1) | ((vex3 & 0x80) ? 8 : 0), gw,
                          rmb, sizeof(rmb)))
            goto unsupported;
        }
        DIS_SET(m, gw == 8 ? "vmovq" : "vmovd");
        if (opcode == 0x6e)
          snprintf(args, sizeof(args), "%s, %s", vreg, rmb);
        else
          snprintf(args, sizeof(args), "%s, %s", rmb, vreg);
        goto decoded;
      }
      /*
       * VEX scalar <-> general-purpose conversions (VCVTSI2SS/SD,
       * VCVTTSS2SI/SD, VCVTSS2SI/SD).  The native backend emits the
       * three-operand VCVTSI2SD form for int-to-double, which the legacy
       * 0F path decoded but the VEX path used to reject.
       */
      if (map == 1 && (opcode == 0x2a || opcode == 0x2c || opcode == 0x2d) &&
          (pp == 2 || pp == 3)) {
        unsigned wide = (vex3 & 0x80) ? 8 : 4;
        unsigned vreg_n = ((mr >> 3) & 7) + ((vex_rex & 4) ? 8 : 0);
        unsigned rmw = opcode == 0x2a ? wide : (pp == 2 ? 4 : 8);
        char vreg[16], vvvv_name[16], rmb[192];
        unsigned rm_n = (mr & 7) + ((vex_rex & 1) ? 8 : 0);
        snprintf(vreg, sizeof(vreg), "xmm%u", vreg_n);
        if ((mr >> 6) == 3) {
          if (opcode == 0x2a)
            snprintf(rmb, sizeof(rmb), "%s",
                     nyr_x64_reg(rm_n, rmw, (vex_rex & 1)));
          else
            snprintf(rmb, sizeof(rmb), "xmm%u", rm_n);
        } else if (!nyr_x64_rm(b, n, &i, mr,
                              (unsigned)(vex_rex & 1) |
                                  (opcode == 0x2a ? (wide == 8 ? 8 : 0) : 0),
                              rmw, rmb, sizeof(rmb))) {
          goto unsupported;
        }
        if (opcode == 0x2a) {
          snprintf(vvvv_name, sizeof(vvvv_name), "xmm%u",
                   (unsigned)(~vex3 >> 3) & 15);
          DIS_SET(m, pp == 2 ? "vcvtsi2ss" : "vcvtsi2sd");
          snprintf(args, sizeof(args), "%s, %s, %s", vreg, vvvv_name, rmb);
        } else {
          DIS_SET(m, opcode == 0x2c
                         ? (pp == 2 ? "vcvttss2si" : "vcvttsd2si")
                         : (pp == 2 ? "vcvtss2si" : "vcvtsd2si"));
          snprintf(args, sizeof(args), "%s, %s",
                   nyr_x64_reg(vreg_n, wide, vex_rex), rmb);
        }
        goto decoded;
      }
      unsigned vector_reg = ((mr >> 3) & 7) + ((vex_rex & 4) ? 8 : 0);
      unsigned vector_width = (vex3 & 4) ? 256 : 128;
      unsigned vector_vvvv = (unsigned)(~vex3 >> 3) & 15;
      char reg_name[16], rm_name[192];
      snprintf(reg_name, sizeof(reg_name), "%s%u",
               vector_width == 256 ? "ymm" : "xmm", vector_reg);
      unsigned rm_width =
          map == 1 && (opcode == 0x10 || opcode == 0x11) && (pp == 2 || pp == 3)
              ? (pp == 2 ? 4 : 8)
          : map == 1 && (opcode == 0x2e || opcode == 0x2f) &&
                  (pp == 0 || pp == 1)
              ? (pp == 0 ? 4 : 8)
          : map == 2 && opcode == 0x19 ? 8
          : map == 2 && opcode == 0x59 ? 8
          : map == 1 && opcode >= 0x58 && opcode <= 0x5e &&
                  (opcode == 0x58 || opcode == 0x59 || opcode == 0x5c ||
                   opcode == 0x5e) &&
                  (pp == 2 || pp == 3)
              ? (pp == 2 ? 4 : 8)
          : map == 1 && opcode == 0x51 && (pp == 2 || pp == 3)
              ? (pp == 2 ? 4 : 8)
          : map == 1 && opcode == 0x5a && (pp == 2 || pp == 3)
              ? (pp == 2 ? 4 : 8)
              : 0;
      if ((mr >> 6) == 3) {
        unsigned rm = (mr & 7) + ((vex_rex & 1) ? 8 : 0);
        snprintf(rm_name, sizeof(rm_name), "%s%u",
                 vector_width == 256 ? "ymm" : "xmm", rm);
      } else {
        if (!nyr_x64_vec_rm(b, n, &i, mr, vex_rex, vector_width, rm_width,
                            rm_name, sizeof(rm_name)))
          goto unsupported;
      }
      if (map == 1 &&
          (opcode == 0x10 || opcode == 0x11 || opcode == 0x28 ||
           opcode == 0x29) &&
          (pp == 0 || pp == 1) && !(vex3 & 0x80) && (vex3 & 0x78) == 0x78) {
        DIS_SET(m, opcode == 0x28 || opcode == 0x29
                       ? (pp == 0 ? "vmovaps" : "vmovapd")
                       : (pp == 0 ? "vmovups" : "vmovupd"));
        int store = opcode == 0x11 || opcode == 0x29;
        snprintf(args, sizeof(args), "%s, %s", store ? rm_name : reg_name,
                 store ? reg_name : rm_name);
      } else if (map == 1 && (opcode == 0x2e || opcode == 0x2f) &&
                 (pp == 0 || pp == 1) && vector_width == 128 &&
                 !(vex3 & 0x80) && (vex3 & 0x78) == 0x78) {
        DIS_SET(m, opcode == 0x2e ? (pp == 0 ? "vucomiss" : "vucomisd")
                                  : (pp == 0 ? "vcomiss" : "vcomisd"));
        snprintf(args, sizeof(args), "%s, %s", reg_name, rm_name);
      } else if (map == 1 && (opcode == 0x6f || opcode == 0x7f) &&
                 (pp == 1 || pp == 2) && !(vex3 & 0x80) &&
                 (vex3 & 0x78) == 0x78) {
        DIS_SET(m, pp == 1 ? "vmovdqa" : "vmovdqu");
        if (opcode == 0x6f)
          snprintf(args, sizeof(args), "%s, %s", reg_name, rm_name);
        else
          snprintf(args, sizeof(args), "%s, %s", rm_name, reg_name);
      } else if (map == 1 && opcode == 0x70 && pp == 1 && !(vex3 & 0x80) &&
                 (vex3 & 0x78) == 0x78 && i < n) {
        DIS_SET(m, "vpshufd");
        snprintf(args, sizeof(args), "%s, %s, 0x%x", reg_name, rm_name, b[i++]);
      } else if (map == 1 && pp == 1 && !(vex3 & 0x80) &&
                 (opcode == 0x71 || opcode == 0x72 || opcode == 0x73) &&
                 (mr >> 6) == 3 && i < n) {
        /*
         * VEX packed integer shifts by an immediate (VPSRL/VPSRA/VPSLL).
         */
        unsigned group = (mr >> 3) & 7;
        const char *name = NULL;
        if (group == 2)
          name = "vpsrl";
        else if (group == 4 && opcode != 0x73)
          name = "vpsra";
        else if (group == 6)
          name = "vpsll";
        if (!name)
          goto unsupported;
        const char *suffix = opcode == 0x71 ? "w" : opcode == 0x72 ? "d" : "q";
        DIS_SET(m, name);
        DIS_APPEND(m, suffix);
        snprintf(args, sizeof(args), "%s, 0x%x", rm_name, b[i++]);
      } else if (map == 1 && pp == 1 && !(vex3 & 0x80) &&
                 (opcode == 0xd4 || opcode == 0xfb || opcode == 0xdb ||
                  opcode == 0xeb || opcode == 0xef)) {
        const char *name = opcode == 0xd4   ? "vpaddq"
                           : opcode == 0xfb ? "vpsubq"
                           : opcode == 0xdb ? "vpand"
                           : opcode == 0xeb ? "vpor"
                                            : "vpxor";
        char vvvv_name[16];
        snprintf(vvvv_name, sizeof(vvvv_name), "%s%u",
                 vector_width == 256 ? "ymm" : "xmm", vector_vvvv);
        DIS_SET(m, name);
        snprintf(args, sizeof(args), "%s, %s, %s", reg_name, vvvv_name,
                 rm_name);
      } else if (map == 1 && (pp == 0 || pp == 1) && !(vex3 & 0x80) &&
                 opcode >= 0x54 && opcode <= 0x57) {
        /*
         * VEX.0F 54-57 packed floating-point bitwise operations.
         */
        const char *name = opcode == 0x54   ? "vand"
                           : opcode == 0x55 ? "vandn"
                           : opcode == 0x56 ? "vor"
                                            : "vxor";
        char vvvv_name[16];
        snprintf(vvvv_name, sizeof(vvvv_name), "%s%u",
                 vector_width == 256 ? "ymm" : "xmm", vector_vvvv);
        DIS_SET(m, name);
        DIS_APPEND(m, pp == 0 ? "ps" : "pd");
        snprintf(args, sizeof(args), "%s, %s, %s", reg_name, vvvv_name,
                 rm_name);
      } else if (map == 1 && pp == 1 && !(vex3 & 0x80) &&
                 (opcode == 0x64 || opcode == 0x65 || opcode == 0x66 ||
                  opcode == 0x74 || opcode == 0x75 || opcode == 0x76)) {
        const char *name = opcode >= 0x74 ? (opcode == 0x74   ? "vpcmpeqb"
                                             : opcode == 0x75 ? "vpcmpeqw"
                                                              : "vpcmpeqd")
                                          : (opcode == 0x64   ? "vpcmpgtb"
                                             : opcode == 0x65 ? "vpcmpgtw"
                                                              : "vpcmpgtd");
        char vvvv_name[16];
        snprintf(vvvv_name, sizeof(vvvv_name), "%s%u",
                 vector_width == 256 ? "ymm" : "xmm", vector_vvvv);
        DIS_SET(m, name);
        snprintf(args, sizeof(args), "%s, %s, %s", reg_name, vvvv_name,
                 rm_name);
      } else if (map == 1 && (opcode == 0x10 || opcode == 0x11) &&
                 (pp == 2 || pp == 3)) {
        /*
         * VEX.F2/F3.0F 10/11: scalar single/double move (reg or mem).
         */
        DIS_SET(m, opcode == 0x10 ? (pp == 3 ? "vmovsd" : "vmovss")
                                  : (pp == 3 ? "vmovsd" : "vmovss"));
        int store = opcode == 0x11;
        if ((mr >> 6) == 3) {
          /*
           * reg-reg is 3-operand: dest, vvvv, src.
           */
          char vvvv_name[16];
          snprintf(vvvv_name, sizeof(vvvv_name), "%s%u",
                   vector_width == 256 ? "ymm" : "xmm", vector_vvvv);
          snprintf(args, sizeof(args), "%s, %s, %s", reg_name, vvvv_name,
                   rm_name);
        } else {
          snprintf(args, sizeof(args), "%s, %s", store ? rm_name : reg_name,
                   store ? reg_name : rm_name);
        }
      } else if (map == 2 && pp == 1 && !(vex3 & 0x80) && opcode == 0x19 &&
                 (vex3 & 0x78) == 0x78 && vector_width == 256) {
        DIS_SET(m, "vbroadcastsd");
        if ((mr >> 6) == 3) {
          unsigned rm = (mr & 7) + ((vex_rex & 1) ? 8 : 0);
          snprintf(args, sizeof(args), "%s, xmm%u", reg_name, rm);
        } else {
          snprintf(args, sizeof(args), "%s, %s", reg_name, rm_name);
        }
      } else if (map == 2 && pp == 1 && (vex3 & 0x80) && opcode == 0xa8) {
        char vvvv_name[16];
        snprintf(vvvv_name, sizeof(vvvv_name), "%s%u",
                 vector_width == 256 ? "ymm" : "xmm", vector_vvvv);
        DIS_SET(m, "vfmadd213pd");
        snprintf(args, sizeof(args), "%s, %s, %s", reg_name, vvvv_name,
                 rm_name);
      } else if (map == 1 && opcode >= 0x58 && opcode <= 0x5e &&
                 (opcode == 0x58 || opcode == 0x59 || opcode == 0x5c ||
                  opcode == 0x5e) &&
                 (pp <= 3) && !(vex3 & 0x80) &&
                 (!(pp == 2 || pp == 3) || vector_width == 128)) {
        const char *suffix = pp == 0   ? "ps"
                             : pp == 1 ? "pd"
                             : pp == 2 ? "ss"
                                       : "sd";
        const char *stem = opcode == 0x58   ? "vadd"
                           : opcode == 0x59 ? "vmul"
                           : opcode == 0x5c ? "vsub"
                                            : "vdiv";
        char vvvv_name[16];
        snprintf(vvvv_name, sizeof(vvvv_name), "%s%u",
                 vector_width == 256 ? "ymm" : "xmm", vector_vvvv);
        DIS_SET(m, stem);
        DIS_APPEND(m, suffix);
        snprintf(args, sizeof(args), "%s, %s, %s", reg_name, vvvv_name,
                 rm_name);
      } else if (map == 1 && opcode == 0x51 && (pp <= 3) && !(vex3 & 0x80) &&
                 ((pp >= 2) || (vex3 & 0x78) == 0x78)) {
        const char *suffix = pp == 0   ? "ps"
                             : pp == 1 ? "pd"
                             : pp == 2 ? "ss"
                                       : "sd";
        DIS_SET(m, "vsqrt");
        DIS_APPEND(m, suffix);
        if (pp >= 2) {
          char vvvv_name[16];
          snprintf(vvvv_name, sizeof(vvvv_name), "%s%u",
                   vector_width == 256 ? "ymm" : "xmm", vector_vvvv);
          snprintf(args, sizeof(args), "%s, %s, %s", reg_name, vvvv_name,
                   rm_name);
        } else {
          snprintf(args, sizeof(args), "%s, %s", reg_name, rm_name);
        }
      } else if (map == 1 && opcode == 0x5a && (pp == 2 || pp == 3) &&
                 vector_width == 128) {
        /*
         * VEX.F3/F2.0F 5A: scalar single <-> double conversion.
         */
        char vvvv_name[16];
        snprintf(vvvv_name, sizeof(vvvv_name), "xmm%u", vector_vvvv);
        DIS_SET(m, pp == 2 ? "vcvtss2sd" : "vcvtsd2ss");
        snprintf(args, sizeof(args), "%s, %s, %s", reg_name, vvvv_name,
                 rm_name);
      } else if (map == 2 && pp == 1 && (vex3 & 0x80) && opcode == 0x59 &&
                 (vex3 & 0x78) == 0x78 && vector_width == 256) {
        DIS_SET(m, "vpbroadcastq");
        if ((mr >> 6) == 3) {
          unsigned rm = (mr & 7) + ((vex_rex & 1) ? 8 : 0);
          snprintf(args, sizeof(args), "%s, xmm%u", reg_name, rm);
        } else {
          snprintf(args, sizeof(args), "%s, %s", reg_name, rm_name);
        }
      } else if (map == 3 && pp == 1 && vector_width == 256 && !(vex3 & 0x80) &&
                 opcode == 0x18) {
        char src_name[192], vvvv_name[16];
        if (!nyr_x64_vec_rm(b, n, &i, mr, vex_rex, 128, 16, src_name,
                            sizeof(src_name)) ||
            i >= n)
          goto unsupported;
        snprintf(vvvv_name, sizeof(vvvv_name), "ymm%u", vector_vvvv);
        DIS_SET(m, "vinsertf128");
        snprintf(args, sizeof(args), "%s, %s, %s, %u", reg_name, vvvv_name,
                 src_name, b[i++]);
      } else if (map == 3 && pp == 1 && vector_width == 256 && !(vex3 & 0x80) &&
                 opcode == 0x19 && (vex3 & 0x78) == 0x78 && (mr >> 6) == 3 &&
                 i < n) {
        unsigned xmm_dest = (mr & 7) + ((vex_rex & 1) ? 8 : 0);
        unsigned half = b[i++];
        if (half > 1)
          goto unsupported;
        DIS_SET(m, "vextractf128");
        snprintf(args, sizeof(args), "xmm%u, %s, %u", xmm_dest, reg_name, half);
      } else {
        goto unsupported;
      }
    } else if (op == 0xe8 || op == 0xe9) {
      if (i + 4 > n)
        goto unsupported;
      rel = (int32_t)rd(b + i, 4);
      i += 4;
      DIS_SET(m, op == 0xe8 ? "call" : "jmp");
      snprintf(args, sizeof(args), "0x%" PRIx64, (uint64_t)(base + i + rel));
    } else if (op == 0xeb || (op >= 0x70 && op <= 0x7f)) {
      if (i >= n)
        goto unsupported;
      rel = (int8_t)b[i++];
      if (op == 0xeb)
        DIS_SET(m, "jmp");
      else {
        static const char *cc[] = {"jo",  "jno", "jb",  "jae", "je", "jne",
                                   "jbe", "ja",  "js",  "jns", "jp", "jnp",
                                   "jl",  "jge", "jle", "jg"};
        DIS_SET(m, cc[op & 15]);
      }
      snprintf(args, sizeof(args), "0x%" PRIx64, (uint64_t)(base + i + rel));
    } else if (op >= 0x50 && op <= 0x5f) {
      DIS_SET(m, op < 0x58 ? "push" : "pop");
      snprintf(args, sizeof(args), "%s", regs64[(op & 7) + rb]);
    } else if (op >= 0xb0 && op <= 0xb7) {
      imm = 1;
      if (i + imm > n)
        goto unsupported;
      DIS_SET(m, "mov");
      snprintf(args, sizeof(args), "%s, 0x%" PRIx64,
               nyr_x64_reg((op & 7) + rb, 1, rex), rd(b + i, 1));
      i += 1;
    } else if (op >= 0xb8 && op <= 0xbf) {
      imm = w;
      if (i + imm > n)
        goto unsupported;
      DIS_SET(m, "mov");
      {
        uint64_t iv = rd(b + i, imm);
        if (imm == 8 && (iv >> 63))
          snprintf(args, sizeof(args), "%s, -0x%" PRIx64, regs64[(op & 7) + rb],
                   (uint64_t)0 - iv);
        else
          snprintf(args, sizeof(args), "%s, 0x%" PRIx64,
                   (rex & 8 ? regs64 : regs32)[(op & 7) + rb], iv);
      }
      i += imm;
    } else if (op == 0x68 || op == 0x6a) {
      imm = op == 0x6a ? 1 : 4;
      if (i + imm > n)
        goto unsupported;
      DIS_SET(m, "push");
      snprintf(args, sizeof(args), "0x%" PRIx64, rd(b + i, imm));
      i += imm;
    } else {
      const char *mn = NULL;
      int dir = 0;
      int acc = 0;
      switch (op) {
      case 0x04:
      case 0x05:
        mn = "add";
        acc = 1;
        break;
      case 0x0c:
      case 0x0d:
        mn = "or";
        acc = 1;
        break;
      case 0x14:
      case 0x15:
        mn = "adc";
        acc = 1;
        break;
      case 0x1c:
      case 0x1d:
        mn = "sbb";
        acc = 1;
        break;
      case 0x24:
      case 0x25:
        mn = "and";
        acc = 1;
        break;
      case 0x2c:
      case 0x2d:
        mn = "sub";
        acc = 1;
        break;
      case 0x34:
      case 0x35:
        mn = "xor";
        acc = 1;
        break;
      case 0x3c:
      case 0x3d:
        mn = "cmp";
        acc = 1;
        break;
      case 0x89:
        mn = "mov";
        break;
      case 0x88:
        mn = "mov";
        break;
      case 0x8b:
        mn = "mov";
        dir = 1;
        break;
      case 0x8a:
        mn = "mov";
        dir = 1;
        break;
      case 0x8d:
        mn = "lea";
        dir = 1;
        break;
      case 0x01:
        mn = "add";
        break;
      case 0x03:
        mn = "add";
        dir = 1;
        break;
      case 0x09:
        mn = "or";
        break;
      case 0x0b:
        mn = "or";
        dir = 1;
        break;
      case 0x21:
        mn = "and";
        break;
      case 0x23:
        mn = "and";
        dir = 1;
        break;
      case 0x29:
        mn = "sub";
        break;
      case 0x2b:
        mn = "sub";
        dir = 1;
        break;
      case 0x31:
        mn = "xor";
        break;
      case 0x33:
        mn = "xor";
        dir = 1;
        break;
      case 0x39:
        mn = "cmp";
        break;
      case 0x3b:
        mn = "cmp";
        dir = 1;
        break;
      case 0x84:
        mn = "test";
        break;
      case 0x85:
        mn = "test";
        break;
      case 0x87:
        mn = "xchg";
        break;
      default:
        break;
      }
      if (acc) {
        /*
         * ALU imm-acc: `48 05 imm32` = add rax, imm32 (sign extended).
         */
        unsigned width = (op & 1) ? w : 1;
        unsigned acc_imm = width == 1 ? 1 : (op16 ? 2 : 4);
        if (i + acc_imm > n)
          goto unsupported;
        int64_t iv = width == 1     ? (int8_t)b[i]
                     : acc_imm == 2 ? (int16_t)rd(b + i, 2)
                                    : (int32_t)rd(b + i, 4);
        i += acc_imm;
        DIS_SET(m, mn);
        if (iv < 0)
          snprintf(args, sizeof(args), "%s, -0x%" PRIx64,
                   nyr_x64_reg((op & 1) ? rb : 0, width, rex), (uint64_t)-iv);
        else
          snprintf(args, sizeof(args), "%s, 0x%" PRIx64,
                   nyr_x64_reg((op & 1) ? rb : 0, width, rex), (uint64_t)iv);
        goto decoded;
      }
      if (mn) {
        if (i >= n)
          goto unsupported;
        unsigned char mr = b[i++];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        char lhs[128], rhs[128];
        unsigned width = op == 0x84 || op == 0x88 || op == 0x8a ? 1 : w;
        if (!nyr_x64_rm(b, n, &i, mr, rex, op == 0x8d ? 0 : width, lhs,
                        sizeof(lhs)))
          goto unsupported;
        snprintf(rhs, sizeof(rhs), "%s", nyr_x64_reg(reg, width, rex));
        DIS_SET(m, mn);
        if (dir)
          snprintf(args, sizeof(args), "%s, %s", rhs, lhs);
        else
          snprintf(args, sizeof(args), "%s, %s", lhs, rhs);
      } else if (op == 0x69 || op == 0x6b) {
        if (i >= n)
          goto unsupported;
        unsigned char mr = b[i++];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        char src[128];
        if (!nyr_x64_rm(b, n, &i, mr, rex, w, src, sizeof(src)))
          goto unsupported;
        imm = op == 0x6b ? 1 : 4;
        if (i + imm > n)
          goto unsupported;
        int64_t iv = imm == 1 ? (int8_t)b[i] : (int32_t)rd(b + i, 4);
        i += imm;
        DIS_SET(m, "imul");
        snprintf(args, sizeof(args), "%s, %s, %s%" PRIx64,
                 nyr_x64_reg(reg, w, rex), src, iv < 0 ? "-0x" : "0x",
                 (uint64_t)(iv < 0 ? -iv : iv));
      } else if (op == 0x63) {
        if (i >= n)
          goto unsupported;
        unsigned char mr = b[i++];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        unsigned dst_width = (rex & 8) ? 8 : (op16 ? 2 : 4);
        char src[128];
        if (!nyr_x64_rm(b, n, &i, mr, rex, 4, src, sizeof(src)))
          goto unsupported;
        DIS_SET(m, "movsxd");
        snprintf(args, sizeof(args), "%s, %s", nyr_x64_reg(reg, dst_width, rex),
                 src);
      } else if (op == 0xc0 || op == 0xc1 || op == 0xd0 || op == 0xd1 ||
                 op == 0xd2 || op == 0xd3) {
        if (i >= n)
          goto unsupported;
        unsigned char mr = b[i++];
        reg = (mr >> 3) & 7;
        unsigned width = op == 0xc0 || op == 0xd0 || op == 0xd2 ? 1 : w;
        char dst[128];
        if (!nyr_x64_rm(b, n, &i, mr, rex, width, dst, sizeof(dst)))
          goto unsupported;
        const char *name = reg == 0   ? "rol"
                           : reg == 1 ? "ror"
                           : reg == 4 ? "shl"
                           : reg == 5 ? "shr"
                           : reg == 7 ? "sar"
                                      : NULL;
        if (!name)
          goto unsupported;
        DIS_SET(m, name);
        if (op == 0xc0 || op == 0xc1) {
          if (i >= n)
            goto unsupported;
          snprintf(args, sizeof(args), "%s, 0x%x", dst, b[i++]);
        } else if (op == 0xd2 || op == 0xd3) {
          snprintf(args, sizeof(args), "%s, cl", dst);
        } else {
          snprintf(args, sizeof(args), "%s", dst);
        }
      } else if (op == 0x80 || op == 0x81 || op == 0x83 || op == 0xc6 ||
                 op == 0xc7) {
        if (i >= n)
          goto unsupported;
        unsigned char mr = b[i++];
        reg = (mr >> 3) & 7;
        char dst[128];
        unsigned width = op == 0x80 || op == 0xc6 ? 1 : w;
        if (!nyr_x64_rm(b, n, &i, mr, rex, width, dst, sizeof(dst)))
          goto unsupported;
        if (op == 0xc6 && reg != 0)
          goto unsupported;
        imm = op == 0x80 || op == 0x83 || op == 0xc6
                  ? 1
                  : (op16 ? 2 : (op == 0xc7 && w != 8 ? w : 4));
        if (i + imm > n)
          goto unsupported;
        int64_t iv = op == 0x80 || op == 0xc6 ? b[i]
                     : op == 0x83             ? (int8_t)b[i]
                     : w == 8                 ? (int32_t)rd(b + i, 4)
                     : op16                   ? (int16_t)rd(b + i, 2)
                                              : (int64_t)rd(b + i, imm);
        i += imm;
        const char *imn = op == 0xc6 || op == 0xc7 ? "mov"
                          : reg == 0               ? "add"
                          : reg == 5               ? "sub"
                          : reg == 7               ? "cmp"
                          : reg == 4               ? "and"
                          : reg == 1               ? "or"
                          : reg == 6               ? "xor"
                                                   : NULL;
        if (!imn)
          goto unsupported;
        DIS_SET(m, imn);
        if (iv < 0)
          snprintf(args, sizeof(args), "%s, -0x%" PRIx64, dst, (uint64_t)-iv);
        else
          snprintf(args, sizeof(args), "%s, 0x%" PRIx64, dst, (uint64_t)iv);
      } else if (op == 0xff) {
        if (i >= n)
          goto unsupported;
        unsigned char mr = b[i++];
        reg = (mr >> 3) & 7;
        const char *name = reg == 0   ? "inc"
                           : reg == 1 ? "dec"
                           : reg == 2 ? "call"
                           : reg == 4 ? "jmp"
                           : reg == 6 ? "push"
                                      : NULL;
        if (!name)
          goto unsupported;
        unsigned width = reg == 0 || reg == 1 ? w : 8;
        char operand[160];
        if (!nyr_x64_rm(b, n, &i, mr, rex, width, operand, sizeof(operand)))
          goto unsupported;
        DIS_SET(m, name);
        snprintf(args, sizeof(args), "%s", operand);
      } else if (op == 0xf6 || op == 0xf7) {
        if (i >= n)
          goto unsupported;
        unsigned char mr = b[i++];
        reg = (mr >> 3) & 7;
        unsigned width = op == 0xf6 ? 1 : w;
        char operand[128];
        if (!nyr_x64_rm(b, n, &i, mr, rex, width, operand, sizeof(operand)))
          goto unsupported;
        if (reg == 0) {
          imm = op == 0xf6 ? 1 : op16 ? 2 : 4;
          if (i + imm > n)
            goto unsupported;
          int64_t iv = op == 0xf6 ? b[i]
                       : op16     ? (int16_t)rd(b + i, 2)
                                  : (int32_t)rd(b + i, 4);
          i += imm;
          DIS_SET(m, "test");
          if (iv < 0)
            snprintf(args, sizeof(args), "%s, -0x%" PRIx64, operand,
                     (uint64_t)-iv);
          else
            snprintf(args, sizeof(args), "%s, 0x%" PRIx64, operand,
                     (uint64_t)iv);
          goto decoded;
        }
        const char *name = reg == 2   ? "not"
                           : reg == 3 ? "neg"
                           : reg == 4 ? "mul"
                           : reg == 5 ? "imul"
                           : reg == 6 ? "div"
                           : reg == 7 ? "idiv"
                                      : NULL;
        if (!name)
          goto unsupported;
        DIS_SET(m, name);
        snprintf(args, sizeof(args), "%s", operand);
      } else
        goto unsupported;
    }
    if (!m[0])
      goto unsupported;
  decoded:
    {
      char locked_m[40];
      const char *emit_m = m;
      if (lockp) {
        snprintf(locked_m, sizeof(locked_m), "lock %s", m);
        emit_m = locked_m;
      }
      size_t sz = (size_t)(i - start);
      char bytes_hex[64] = "";
      for (size_t k = 0; k < sz && k < 16; ++k) {
        char byte_hex[4];
        snprintf(byte_hex, sizeof(byte_hex), "%02x ", b[start + k]);
        strncat(bytes_hex, byte_hex, sizeof(bytes_hex) - strlen(bytes_hex) - 1);
      }
      if (used > SIZE_MAX - 700 || !output_ensure(used + 700))
        break;
      int z = snprintf(output + used, output_cap - used,
                       "%" PRId64 "\t%s\t%s\t%" PRId64 "\t%s\n", base + start,
                       emit_m, args, i - start, bytes_hex);
      if (z < 0 || (size_t)z >= output_cap - used)
        break;
      used += (size_t)z;
    }
    continue;
  unsupported:
    {
      /*
       * Emit one explicit unsupported row for the offending byte, then
       * resume decoding at the next byte instead of truncating the listing.
       * This keeps long functions from being cut short by a single
       * unhandled encoding (e.g. a VEX/EVEX form the decoder lacks).
       */
      unsigned v = (i > start ? b[start] : op);
      if (used > SIZE_MAX - 700 || !output_ensure(used + 700))
        break;
      int z = snprintf(output + used, output_cap - used,
                       "%" PRId64 "\tunsupported\topcode 0x%02x at 0x%" PRIx64
                       "\t1\t%02x \n",
                       base + start, v, (uint64_t)(base + start), v);
      if (z < 0 || (size_t)z >= output_cap - used)
        break;
      used += (size_t)z;
      i = start + 1;
      continue;
    }
  }
  return output;
}

static _Thread_local char a64_output[131072];

static int64_t nyr_sign_extend(uint64_t v, unsigned bits) {
  uint64_t sign = UINT64_C(1) << (bits - 1);
  return (int64_t)((v ^ sign) - sign);
}

static const char *nyr_a64_reg(unsigned r, int wide, int stack) {
  static const char *const x[] = {
      "x0",  "x1",  "x2",  "x3",  "x4",  "x5",  "x6",  "x7",
      "x8",  "x9",  "x10", "x11", "x12", "x13", "x14", "x15",
      "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23",
      "x24", "x25", "x26", "x27", "x28", "x29", "x30", "xzr"};
  static const char *const w[] = {
      "w0",  "w1",  "w2",  "w3",  "w4",  "w5",  "w6",  "w7",
      "w8",  "w9",  "w10", "w11", "w12", "w13", "w14", "w15",
      "w16", "w17", "w18", "w19", "w20", "w21", "w22", "w23",
      "w24", "w25", "w26", "w27", "w28", "w29", "w30", "wzr"};
  if (r == 31 && stack)
    return "sp";
  return (wide ? x : w)[r & 31];
}

static void nyr_a64_vec_reg(char *out, size_t cap, unsigned r,
                            const char *shape) {
  snprintf(out, cap, "v%u.%s", r & 31, shape);
}

static int nyr_a64_logical_imm(uint32_t insn, unsigned width, uint64_t *out) {
  unsigned n = (insn >> 22) & 1;
  unsigned immr = (insn >> 16) & 0x3f;
  unsigned imms = (insn >> 10) & 0x3f;
  unsigned selector = (n << 6) | ((~imms) & 0x3f);
  unsigned len = 0;
  for (unsigned v = selector; v >>= 1;)
    ++len;
  if (len < 1 || len > 6)
    return 0;
  unsigned elem_bits = 1u << len;
  if (elem_bits > width)
    return 0;
  unsigned levels = elem_bits - 1;
  unsigned s = imms & levels;
  unsigned r = immr & levels;
  if (s == levels)
    return 0;
  uint64_t elem_mask =
      elem_bits == 64 ? UINT64_MAX : ((UINT64_C(1) << elem_bits) - 1);
  uint64_t elem = s == 63 ? UINT64_MAX : ((UINT64_C(1) << (s + 1)) - 1);
  elem = r == 0 ? elem : ((elem >> r) | (elem << (elem_bits - r)));
  elem &= elem_mask;
  uint64_t value = 0;
  for (unsigned shift = 0; shift < width; shift += elem_bits)
    value |= elem << shift;
  *out = value;
  return 1;
}

const char *nyr_internal_aarch64_disasm(const unsigned char *b, int64_t n,
                                        int64_t base) {
  size_t used = 0;
  a64_output[0] = 0;
  if (!b || n < 0 || (n & 3))
    return "";
  for (int64_t off = 0; off < n; off += 4) {
    uint32_t insn = (uint32_t)rd(b + off, 4);
    unsigned rd0 = insn & 31, rn = (insn >> 5) & 31, rm = (insn >> 16) & 31;
    unsigned width = insn >> 31, size = 4;
    char mnemonic[20] = "", operands[192] = "";
    char vd[8], vn[8], vm[8];
    int64_t target = 0;
    if (insn == 0xd503201f)
      DIS_SET(mnemonic, "nop");
    else if ((insn & 0x3a000000) == 0x28000000 && !(insn & 0x04000000) &&
             ((insn >> 30) & 3) >= 2) {
      unsigned load = (insn >> 22) & 1;
      unsigned mode = (insn >> 23) & 3;
      unsigned bytes = ((insn >> 30) & 3) == 2 ? 8 : 4;
      unsigned rt2 = (insn >> 10) & 31;
      int64_t imm = nyr_sign_extend((insn >> 15) & 0x7f, 7) * bytes;
      const char *a = nyr_a64_reg(rd0, bytes == 8, 0);
      const char *breg = nyr_a64_reg(rt2, bytes == 8, 0);
      const char *bas = nyr_a64_reg(rn, 1, 1);
      DIS_SET(mnemonic, load ? "ldp" : "stp");
      if (mode == 1)
        snprintf(operands, sizeof(operands), "%s, %s, [%s], #%" PRId64, a, breg,
                 bas, imm);
      else if (mode == 3)
        snprintf(operands, sizeof(operands), "%s, %s, [%s, #%" PRId64 "]!", a,
                 breg, bas, imm);
      else if (mode == 2)
        snprintf(operands, sizeof(operands), "%s, %s, [%s, #%" PRId64 "]", a,
                 breg, bas, imm);
      else
        goto unsupported_a64;
    } else if ((insn & 0xffe0fc00) == 0x9ac00c00 ||
               (insn & 0xffe0fc00) == 0x1ac00c00) {
      DIS_SET(mnemonic, "sdiv");
      snprintf(operands, sizeof(operands), "%s, %s, %s",
               nyr_a64_reg(rd0, width, 0), nyr_a64_reg(rn, width, 0),
               nyr_a64_reg(rm, width, 0));
    } else if ((insn & 0xffe0fc00) == 0x9ac02000 ||
               (insn & 0xffe0fc00) == 0x1ac02000 ||
               (insn & 0xffe0fc00) == 0x9ac02800 ||
               (insn & 0xffe0fc00) == 0x1ac02800 ||
               (insn & 0xffe0fc00) == 0x9ac02c00 ||
               (insn & 0xffe0fc00) == 0x1ac02c00) {
      unsigned shift_op = (insn >> 10) & 0x3f;
      DIS_SET(mnemonic, shift_op == 10   ? "asrv"
                        : shift_op == 11 ? "rorv"
                                         : "lslv");
      snprintf(operands, sizeof(operands), "%s, %s, %s",
               nyr_a64_reg(rd0, width, 0), nyr_a64_reg(rn, width, 0),
               nyr_a64_reg(rm, width, 0));
    } else if ((insn & 0xffe0fc00) == 0x9b407c00) {
      DIS_SET(mnemonic, "umulh");
      snprintf(operands, sizeof(operands), "%s, %s, %s", nyr_a64_reg(rd0, 1, 0),
               nyr_a64_reg(rn, 1, 0), nyr_a64_reg(rm, 1, 0));
    } else if ((insn & 0x7fe08000) == 0x1b008000 ||
               (insn & 0x7fe08000) == 0x1b000000) {
      unsigned acc = (insn >> 10) & 31;
      DIS_SET(mnemonic, (insn & 0x8000) ? "msub" : "madd");
      snprintf(operands, sizeof(operands), "%s, %s, %s, %s",
               nyr_a64_reg(rd0, width, 0), nyr_a64_reg(rn, width, 0),
               nyr_a64_reg(rm, width, 0), nyr_a64_reg(acc, width, 0));
    } else if ((insn & 0x1fe00c00) == 0x1a800000) {
      static const char *cc[] = {"eq", "ne", "cs", "cc", "mi", "pl",
                                 "vs", "vc", "hi", "ls", "ge", "lt",
                                 "gt", "le", "al", "nv"};
      DIS_SET(mnemonic, "csel");
      snprintf(operands, sizeof(operands), "%s, %s, %s, %s",
               nyr_a64_reg(rd0, width, 0), nyr_a64_reg(rn, width, 0),
               nyr_a64_reg(rm, width, 0), cc[(insn >> 12) & 15]);
    } else if ((insn & 0xffe0fc00) == 0x1e202800 ||
               (insn & 0xffe0fc00) == 0x1e602800 ||
               (insn & 0xffe0fc00) == 0x1e203800 ||
               (insn & 0xffe0fc00) == 0x1e603800 ||
               (insn & 0xffe0fc00) == 0x1e200800 ||
               (insn & 0xffe0fc00) == 0x1e600800 ||
               (insn & 0xffe0fc00) == 0x1e201800 ||
               (insn & 0xffe0fc00) == 0x1e601800) {
      uint32_t op = insn & 0xffe0fc00;
      DIS_SET(mnemonic, op == 0x1e202800 || op == 0x1e602800   ? "fadd"
                        : op == 0x1e203800 || op == 0x1e603800 ? "fsub"
                        : op == 0x1e200800 || op == 0x1e600800 ? "fmul"
                                                               : "fdiv");
      char fp = (op & 0x00400000) ? 'd' : 's';
      snprintf(operands, sizeof(operands), "%c%u, %c%u, %c%u", fp, rd0, fp, rn,
               fp, rm);
    } else if ((insn & 0xffe0fc00) == 0x1e202000 ||
               (insn & 0xffe0fc00) == 0x1e602000) {
      DIS_SET(mnemonic, "fcmp");
      char fp = ((insn & 0xffe0fc00) == 0x1e602000) ? 'd' : 's';
      snprintf(operands, sizeof(operands), "%c%u, %c%u", fp, rn, fp, rm);
    } else if ((insn & 0xfffffc00) == 0x1e204000 ||
               (insn & 0xfffffc00) == 0x1e604000) {
      DIS_SET(mnemonic, "fmov");
      char fp = ((insn & 0xfffffc00) == 0x1e604000) ? 'd' : 's';
      snprintf(operands, sizeof(operands), "%c%u, %c%u", fp, rd0, fp, rn);
    } else if ((insn & 0xfffffc00) == 0x9e660000 ||
               (insn & 0xfffffc00) == 0x9e260000) {
      DIS_SET(mnemonic, "fmov");
      snprintf(operands, sizeof(operands), "%s, %c%u", nyr_a64_reg(rd0, 1, 0),
               (insn & 0x00400000) ? 'd' : 's', rn);
    } else if ((insn & 0xfffffc00) == 0x9e670000 ||
               (insn & 0xfffffc00) == 0x9e270000) {
      DIS_SET(mnemonic, "fmov");
      snprintf(operands, sizeof(operands), "%c%u, %s",
               (insn & 0x00400000) ? 'd' : 's', rd0, nyr_a64_reg(rn, 1, 0));
    } else if ((insn & 0xffe00c00) == 0xf8400000 ||
               (insn & 0xffe00c00) == 0xf8000000 ||
               (insn & 0xffe00c00) == 0xfc400000 ||
               (insn & 0xffe00c00) == 0xfc000000 ||
               (insn & 0xffe00c00) == 0xbc400000 ||
               (insn & 0xffe00c00) == 0xbc000000 ||
               (insn & 0xffe00c00) == 0x3cc00000 ||
               (insn & 0xffe00c00) == 0x3c800000) {
      uint32_t op = insn & 0xffe00c00;
      int64_t imm = nyr_sign_extend((insn >> 12) & 0x1ff, 9);
      int load = op == 0xf8400000 || op == 0xfc400000 || op == 0xbc400000 ||
                 op == 0x3cc00000;
      const char *base_reg = nyr_a64_reg(rn, 1, 1);
      if (op == 0x3cc00000 || op == 0x3c800000) {
        DIS_SET(mnemonic, load ? "ldur" : "stur");
        if (imm)
          snprintf(operands, sizeof(operands), "q%u, [%s, #%" PRId64 "]", rd0,
                   base_reg, imm);
        else
          snprintf(operands, sizeof(operands), "q%u, [%s]", rd0, base_reg);
      } else if (op == 0xfc400000 || op == 0xfc000000 || op == 0xbc400000 ||
                 op == 0xbc000000) {
        char width_name = op == 0xfc400000 || op == 0xfc000000 ? 'd' : 's';
        DIS_SET(mnemonic, load ? "ldur" : "stur");
        if (imm)
          snprintf(operands, sizeof(operands), "%c%u, [%s, #%" PRId64 "]",
                   width_name, rd0, base_reg, imm);
        else
          snprintf(operands, sizeof(operands), "%c%u, [%s]", width_name, rd0,
                   base_reg);
      } else {
        DIS_SET(mnemonic, load ? "ldur" : "stur");
        if (imm)
          snprintf(operands, sizeof(operands), "%s, [%s, #%" PRId64 "]",
                   nyr_a64_reg(rd0, 1, 0), base_reg, imm);
        else
          snprintf(operands, sizeof(operands), "%s, [%s]",
                   nyr_a64_reg(rd0, 1, 0), base_reg);
      }
    } else if ((insn & 0xfffffc00) == 0x9e620000 ||
               (insn & 0xfffffc00) == 0x9e220000) {
      DIS_SET(mnemonic, "scvtf");
      snprintf(operands, sizeof(operands), "%c%u, %s",
               (insn & 0x00400000) ? 'd' : 's', rd0, nyr_a64_reg(rn, 1, 0));
    } else if ((insn & 0xfffffc00) == 0x9e780000 ||
               (insn & 0xfffffc00) == 0x9e380000) {
      DIS_SET(mnemonic, "fcvtzs");
      snprintf(operands, sizeof(operands), "%s, %c%u", nyr_a64_reg(rd0, 1, 0),
               (insn & 0x00400000) ? 'd' : 's', rn);
    } else if ((insn & 0xfffffc00) == 0x1e22c000 ||
               (insn & 0xfffffc00) == 0x1e624000) {
      DIS_SET(mnemonic, "fcvt");
      int to_double = (insn & 0xfffffc00) == 0x1e22c000;
      snprintf(operands, sizeof(operands), "%c%u, %c%u", to_double ? 'd' : 's',
               rd0, to_double ? 's' : 'd', rn);
    } else if ((insn & 0xfffffc1f) == 0xd65f0000) {
      DIS_SET(mnemonic, "ret");
      if (rn != 30)
        snprintf(operands, sizeof(operands), "%s", nyr_a64_reg(rn, 1, 0));
    } else if ((insn & 0xfffffc1f) == 0xd61f0000 ||
               (insn & 0xfffffc1f) == 0xd63f0000) {
      DIS_SET(mnemonic, (insn & 0x00200000) ? "blr" : "br");
      snprintf(operands, sizeof(operands), "%s", nyr_a64_reg(rn, 1, 0));
    } else if ((insn & 0xffe0001f) == 0xd4000001) {
      DIS_SET(mnemonic, "svc");
      snprintf(operands, sizeof(operands), "#%u", (insn >> 5) & 0xffff);
    } else if ((insn & 0xffe0001f) == 0xd4200000) {
      DIS_SET(mnemonic, "brk");
      snprintf(operands, sizeof(operands), "#%u", (insn >> 5) & 0xffff);
    } else if ((insn & 0x7c000000) == 0x14000000) {
      DIS_SET(mnemonic, (insn & 0x80000000) ? "bl" : "b");
      target = base + off + nyr_sign_extend(insn & 0x03ffffff, 26) * 4;
      snprintf(operands, sizeof(operands), "0x%" PRIx64, (uint64_t)target);
    } else if ((insn & 0xff000010) == 0x54000000) {
      static const char *cc[] = {"eq", "ne", "cs", "cc", "mi", "pl",
                                 "vs", "vc", "hi", "ls", "ge", "lt",
                                 "gt", "le", "al", "nv"};
      DIS_SET(mnemonic, "b.");
      DIS_APPEND(mnemonic, cc[insn & 15]);
      target = base + off + nyr_sign_extend((insn >> 5) & 0x7ffff, 19) * 4;
      snprintf(operands, sizeof(operands), "0x%" PRIx64, (uint64_t)target);
    } else if ((insn & 0x7e000000) == 0x34000000) {
      DIS_SET(mnemonic, (insn & 0x01000000) ? "cbnz" : "cbz");
      target = base + off + nyr_sign_extend((insn >> 5) & 0x7ffff, 19) * 4;
      snprintf(operands, sizeof(operands), "%s, 0x%" PRIx64,
               nyr_a64_reg(rd0, width, 0), (uint64_t)target);
    } else if ((insn & 0x7e000000) == 0x36000000) {
      DIS_SET(mnemonic, (insn & 0x01000000) ? "tbnz" : "tbz");
      target = base + off + nyr_sign_extend((insn >> 5) & 0x3fff, 14) * 4;
      unsigned bit = ((insn >> 26) & 0x20) | ((insn >> 19) & 0x1f);
      snprintf(operands, sizeof(operands), "%s, #%u, 0x%" PRIx64,
               nyr_a64_reg(rd0, width, 0), bit, (uint64_t)target);
    } else if ((insn & 0x1f000000) == 0x11000000) {
      unsigned sub = (insn >> 30) & 1, flags = (insn >> 29) & 1;
      unsigned imm = (insn >> 10) & 0xfff;
      if (insn & (1u << 22))
        imm <<= 12;
      if (flags && rd0 == 31)
        DIS_SET(mnemonic, sub ? "cmp" : "cmn");
      else
        DIS_SET(mnemonic,
                sub ? (flags ? "subs" : "sub") : (flags ? "adds" : "add"));
      if (flags && rd0 == 31)
        snprintf(operands, sizeof(operands), "%s, #%u",
                 nyr_a64_reg(rn, width, 1), imm);
      else
        snprintf(operands, sizeof(operands), "%s, %s, #%u",
                 nyr_a64_reg(rd0, width, 1), nyr_a64_reg(rn, width, 1), imm);
    } else if ((insn & 0x1f200000) == 0x0b000000) {
      unsigned sub = (insn >> 30) & 1, flags = (insn >> 29) & 1;
      if (flags && rd0 == 31)
        DIS_SET(mnemonic, sub ? "cmp" : "cmn");
      else
        DIS_SET(mnemonic,
                sub ? (flags ? "subs" : "sub") : (flags ? "adds" : "add"));
      if (flags && rd0 == 31)
        snprintf(operands, sizeof(operands), "%s, %s",
                 nyr_a64_reg(rn, width, 1), nyr_a64_reg(rm, width, 0));
      else
        snprintf(operands, sizeof(operands), "%s, %s, %s",
                 nyr_a64_reg(rd0, width, 1), nyr_a64_reg(rn, width, 1),
                 nyr_a64_reg(rm, width, 0));
    } else if ((insn & 0x7f800000) == 0x52800000 ||
               (insn & 0x7f800000) == 0x72800000 ||
               (insn & 0x7f800000) == 0x12800000) {
      unsigned opc = (insn >> 29) & 3, shift = ((insn >> 21) & 3) * 16;
      uint64_t imm = (insn >> 5) & 0xffff;
      DIS_SET(mnemonic, opc == 3 ? "movk" : opc == 0 ? "movn" : "movz");
      if (shift)
        snprintf(operands, sizeof(operands), "%s, #0x%" PRIx64 ", lsl #%u",
                 nyr_a64_reg(rd0, width, 0), imm, shift);
      else
        snprintf(operands, sizeof(operands), "%s, #0x%" PRIx64,
                 nyr_a64_reg(rd0, width, 0), imm);
    } else if ((insn & 0xfffffc00) == 0xdac00c00 ||
               (insn & 0xfffffc00) == 0x5ac00800) {
      DIS_SET(mnemonic, "rev");
      snprintf(operands, sizeof(operands), "%s, %s", nyr_a64_reg(rd0, width, 0),
               nyr_a64_reg(rn, width, 0));
    } else if ((insn & 0x9f000000) == 0x90000000) {
      uint64_t imm =
          ((uint64_t)((insn >> 5) & 0x7ffff) << 2) | ((insn >> 29) & 3);
      int64_t pages = nyr_sign_extend(imm, 21);
      uint64_t pc_page = (uint64_t)(base + off) & ~UINT64_C(0xfff);
      target = (int64_t)(pc_page + pages * INT64_C(4096));
      DIS_SET(mnemonic, "adrp");
      snprintf(operands, sizeof(operands), "%s, 0x%" PRIx64,
               nyr_a64_reg(rd0, 1, 0), (uint64_t)target);
    } else if ((insn & 0x7f800000) == 0x53000000 ||
               (insn & 0x7f800000) == 0x13000000 ||
               (insn & 0xff800000) == 0xd3000000 ||
               (insn & 0xff800000) == 0x93000000) {
      unsigned bits = width ? 64 : 32;
      unsigned immr = (insn >> 16) & (bits - 1);
      unsigned imms = (insn >> 10) & (bits - 1);
      int signed_op = (insn & 0x7f800000) == 0x13000000 ||
                      (insn & 0xff800000) == 0x93000000;
      const char *shift = NULL;
      unsigned amount = 0;
      if (imms == bits - 1) {
        shift = signed_op ? "asr" : "lsr";
        amount = immr;
      } else if (!signed_op && immr != 0 && imms == bits - 1 - (bits - immr)) {
        shift = "lsl";
        amount = bits - immr;
      }
      if (!shift)
        goto unsupported_a64;
      DIS_SET(mnemonic, shift);
      snprintf(operands, sizeof(operands), "%s, %s, #%u",
               nyr_a64_reg(rd0, width, 0), nyr_a64_reg(rn, width, 0), amount);
    } else if ((insn & 0x1f800000) == 0x12000000) {
      uint64_t immediate = 0;
      unsigned op = (insn >> 29) & 3;
      if (!nyr_a64_logical_imm(insn, width ? 64 : 32, &immediate))
        goto unsupported_a64;
      const char *name = op == 0   ? "and"
                         : op == 1 ? "orr"
                         : op == 2 ? "eor"
                                   : "ands";
      if (op == 3 && rd0 == 31)
        name = "tst";
      DIS_SET(mnemonic, name);
      if (name[0] == 't')
        snprintf(operands, sizeof(operands), "%s, #0x%" PRIx64,
                 nyr_a64_reg(rn, width, 0), immediate);
      else
        snprintf(operands, sizeof(operands), "%s, %s, #0x%" PRIx64,
                 nyr_a64_reg(rd0, width, 0), nyr_a64_reg(rn, width, 0),
                 immediate);
    } else if ((insn & 0x1f000000) == 0x0a000000) {
      unsigned opc = (insn >> 29) & 3;
      if (opc == 1 && rn == 31) {
        DIS_SET(mnemonic, "mov");
        snprintf(operands, sizeof(operands), "%s, %s",
                 nyr_a64_reg(rd0, width, 0), nyr_a64_reg(rm, width, 0));
      } else {
        DIS_SET(mnemonic, opc == 0   ? "and"
                          : opc == 1 ? "orr"
                          : opc == 2 ? "eor"
                                     : "ands");
        snprintf(operands, sizeof(operands), "%s, %s, %s",
                 nyr_a64_reg(rd0, width, 0), nyr_a64_reg(rn, width, 0),
                 nyr_a64_reg(rm, width, 0));
      }
    } else if ((insn & 0xffc00000) == 0x3dc00000 ||
               (insn & 0xffc00000) == 0x3d800000 ||
               (insn & 0xffc00000) == 0xfd400000 ||
               (insn & 0xffc00000) == 0xfd000000 ||
               (insn & 0xffc00000) == 0xbd400000 ||
               (insn & 0xffc00000) == 0xbd000000) {
      uint32_t op = insn & 0xffc00000;
      unsigned scale = op == 0x3dc00000 || op == 0x3d800000   ? 16
                       : op == 0xfd400000 || op == 0xfd000000 ? 8
                                                              : 4;
      unsigned imm = ((insn >> 10) & 0xfff) * scale;
      const char *reg_name = scale == 16 ? "q" : scale == 8 ? "d" : "s";
      DIS_SET(mnemonic,
              (op == 0x3dc00000 || op == 0xfd400000 || op == 0xbd400000)
                  ? "ldr"
                  : "str");
      if (imm)
        snprintf(operands, sizeof(operands), "%s%u, [%s, #%u]", reg_name, rd0,
                 nyr_a64_reg(rn, 1, 1), imm);
      else
        snprintf(operands, sizeof(operands), "%s%u, [%s]", reg_name, rd0,
                 nyr_a64_reg(rn, 1, 1));
    } else if ((insn & 0xbf201c00) == 0x0e000000) {
      unsigned table_len = ((insn >> 14) & 3) + 1;
      char table[96] = "{";
      size_t table_used = 1;
      for (unsigned ti = 0; ti < table_len; ++ti) {
        int z = snprintf(table + table_used, sizeof(table) - table_used,
                         "%sv%u.16b", ti ? ", " : "", (rn + ti) & 31);
        if (z < 0 || (size_t)z >= sizeof(table) - table_used)
          goto unsupported_a64;
        table_used += (size_t)z;
      }
      if (table_used + 2 > sizeof(table))
        goto unsupported_a64;
      table[table_used++] = '}';
      table[table_used] = 0;
      DIS_SET(mnemonic, "tbl");
      snprintf(operands, sizeof(operands), "v%u.16b, %s, v%u.16b", rd0, table,
               rm);
    } else {
      uint32_t vec_op = insn & 0xffe0fc00;
      const char *shape = NULL;
      if ((insn & 0xfffffc00) == 0x7e70d800) {
        DIS_SET(mnemonic, "faddp");
        snprintf(operands, sizeof(operands), "d%u, v%u.2d", rd0, rn);
      } else if ((insn & 0xfffffc00) == 0x4e080400 ||
                 (insn & 0xfffffc00) == 0x4e040400) {
        int is64 = (insn & 0x00080000) != 0;
        nyr_a64_vec_reg(vd, sizeof(vd), rd0, is64 ? "2d" : "4s");
        DIS_SET(mnemonic, "dup");
        snprintf(operands, sizeof(operands), "%s, v%u.%s[0]", vd, rn,
                 is64 ? "d" : "s");
      } else if (vec_op == 0x4ee08400 || vec_op == 0x6ee08400)
        shape = "2d";
      else if (vec_op == 0x4e201c00 || vec_op == 0x4ea01c00 ||
               vec_op == 0x6e201c00)
        shape = "16b";
      else if (vec_op == 0x4e60d400 || vec_op == 0x4ee0d400 ||
               vec_op == 0x6e60dc00 || vec_op == 0x6e60fc00 ||
               vec_op == 0x4e60cc00)
        shape = "2d";
      else if (vec_op == 0x4e20d400 || vec_op == 0x4ea0d400 ||
               vec_op == 0x6e20dc00 || vec_op == 0x6e20fc00 ||
               vec_op == 0x4e20cc00)
        shape = "4s";
      if (mnemonic[0]) {
        /*
         * A specialized SIMD form was already decoded above.
         */
      } else if (shape) {
        if (vec_op == 0x4ee08400)
          DIS_SET(mnemonic, "add");
        else if (vec_op == 0x6ee08400)
          DIS_SET(mnemonic, "sub");
        else if (vec_op == 0x4e201c00)
          DIS_SET(mnemonic, "and");
        else if (vec_op == 0x4ea01c00)
          DIS_SET(mnemonic, "orr");
        else if (vec_op == 0x6e201c00)
          DIS_SET(mnemonic, "eor");
        else if (vec_op == 0x4e60d400 || vec_op == 0x4e20d400)
          DIS_SET(mnemonic, "fadd");
        else if (vec_op == 0x4ee0d400 || vec_op == 0x4ea0d400)
          DIS_SET(mnemonic, "fsub");
        else if (vec_op == 0x6e60dc00 || vec_op == 0x6e20dc00)
          DIS_SET(mnemonic, "fmul");
        else if (vec_op == 0x6e60fc00 || vec_op == 0x6e20fc00)
          DIS_SET(mnemonic, "fdiv");
        else
          DIS_SET(mnemonic, "fmla");
        nyr_a64_vec_reg(vd, sizeof(vd), rd0, shape);
        nyr_a64_vec_reg(vn, sizeof(vn), rn, shape);
        nyr_a64_vec_reg(vm, sizeof(vm), rm, shape);
        snprintf(operands, sizeof(operands), "%s, %s, %s", vd, vn, vm);
      } else if ((insn & 0xfffffc00) == 0x4e080c00 ||
                 (insn & 0xfffffc00) == 0x4e040c00) {
        unsigned is64 = (insn & 0x00080000) != 0;
        nyr_a64_vec_reg(vd, sizeof(vd), rd0, is64 ? "2d" : "4s");
        DIS_SET(mnemonic, "dup");
        snprintf(operands, sizeof(operands), "%s, %s", vd,
                 nyr_a64_reg(rn, is64, 0));
      } else if ((insn & 0xfffffc00) == 0x9e670000) {
        DIS_SET(mnemonic, "fmov");
        snprintf(operands, sizeof(operands), "d%u, %s", rd0,
                 nyr_a64_reg(rn, 1, 0));
      } else if ((insn & 0xfffffc00) == 0x4e181c00) {
        DIS_SET(mnemonic, "ins");
        snprintf(operands, sizeof(operands), "v%u.d[1], %s", rd0,
                 nyr_a64_reg(rn, 1, 0));
      } else if (vec_op == 0x6ee0b800) {
        nyr_a64_vec_reg(vd, sizeof(vd), rd0, "2d");
        nyr_a64_vec_reg(vn, sizeof(vn), rn, "2d");
        DIS_SET(mnemonic, "neg");
        snprintf(operands, sizeof(operands), "%s, %s", vd, vn);
      } else if (vec_op == 0x4ee04400) {
        nyr_a64_vec_reg(vd, sizeof(vd), rd0, "2d");
        nyr_a64_vec_reg(vn, sizeof(vn), rn, "2d");
        nyr_a64_vec_reg(vm, sizeof(vm), rm, "2d");
        DIS_SET(mnemonic, "sshl");
        snprintf(operands, sizeof(operands), "%s, %s, %s", vd, vn, vm);
      } else if ((insn & 0x3b200c00) == 0x38200800) {
        unsigned opc = (insn >> 22) & 3;
        unsigned bytes = 1u << ((insn >> 30) & 3);
        unsigned option = (insn >> 13) & 7;
        unsigned scaled = (insn >> 12) & 1;
        const char *idx = nyr_a64_reg(rm, option == 3 || option == 7, 0);
        const char *base_reg = nyr_a64_reg(rn, 1, 1);
        char address[96];
        if (option != 2 && option != 3 && option != 6 && option != 7)
          goto unsupported_a64;
        if (scaled) {
          unsigned shift = bytes == 8 ? 3 : bytes == 4 ? 2 : bytes == 2 ? 1 : 0;
          snprintf(address, sizeof(address), "[%s, %s, lsl #%u]", base_reg, idx,
                   shift);
        } else {
          snprintf(address, sizeof(address), "[%s, %s]", base_reg, idx);
        }
        if (opc > 1)
          goto unsupported_a64;
        DIS_SET(mnemonic, opc ? (bytes == 1   ? "ldrb"
                                 : bytes == 2 ? "ldrh"
                                              : "ldr")
                              : (bytes == 1   ? "strb"
                                 : bytes == 2 ? "strh"
                                              : "str"));
        snprintf(operands, sizeof(operands), "%s, %s",
                 nyr_a64_reg(rd0, bytes == 8, 0), address);
      } else if ((insn & 0x3f000000) == 0x39000000) {
        unsigned opc = (insn >> 22) & 3, bytes = 1u << ((insn >> 30) & 3);
        unsigned imm = ((insn >> 10) & 0xfff) * bytes;
        const char *reg = nyr_a64_reg(rd0, bytes == 8, 0);
        const char *bas = nyr_a64_reg(rn, 1, 1);
        if (opc > 2 || (opc == 2 && bytes != 4))
          goto unsupported_a64;
        DIS_SET(mnemonic, opc == 2   ? "ldrsw"
                          : opc == 1 ? (bytes == 1   ? "ldrb"
                                        : bytes == 2 ? "ldrh"
                                                     : "ldr")
                                     : (bytes == 1   ? "strb"
                                        : bytes == 2 ? "strh"
                                                     : "str"));
        if (imm)
          snprintf(operands, sizeof(operands), "%s, [%s, #%u]",
                   opc == 2 ? nyr_a64_reg(rd0, 1, 0) : reg, bas, imm);
        else
          snprintf(operands, sizeof(operands), "%s, [%s]",
                   opc == 2 ? nyr_a64_reg(rd0, 1, 0) : reg, bas);
      } else {
        goto unsupported_a64;
      }
    }
    {
      char bytes_hex[16];
      snprintf(bytes_hex, sizeof(bytes_hex), "%02x %02x %02x %02x", b[off],
               b[off + 1], b[off + 2], b[off + 3]);
      int z = snprintf(a64_output + used, sizeof(a64_output) - used,
                       "%" PRId64 "\t%s\t%s\t%u\t%s\n", base + off, mnemonic,
                       operands, size, bytes_hex);
      if (z < 0 || (size_t)z >= sizeof(a64_output) - used)
        break;
      used += (size_t)z;
    }
    continue;
  unsupported_a64:
    snprintf(a64_output + used, sizeof(a64_output) - used,
             "%" PRId64 "\tunsupported\tinstruction word 0x%08" PRIx32
             " at 0x%" PRIx64 "\t4\t%02x %02x %02x %02x\n",
             base + off, insn, (uint64_t)(base + off), b[off], b[off + 1],
             b[off + 2], b[off + 3]);
    return a64_output;
  }
  return a64_output;
}
