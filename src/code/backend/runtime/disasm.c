#include "shared.h"
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef size_t nyr_cs_handle;
typedef struct {
  unsigned id;
  uint64_t address;
  uint16_t size;
  uint8_t bytes[24];
  char mnemonic[32];
  char op_str[160];
  void *detail;
} nyr_cs_insn_t;
_Static_assert(offsetof(nyr_cs_insn_t, mnemonic) == 42 &&
                   offsetof(nyr_cs_insn_t, op_str) == 74,
               "optional Capstone fallback expects the public cs_insn layout");
typedef int (*nyr_cs_open_fn)(int, int, nyr_cs_handle *);
typedef size_t (*nyr_cs_disasm_fn)(nyr_cs_handle, const uint8_t *, size_t,
                                   uint64_t, size_t, nyr_cs_insn_t **);
typedef void (*nyr_cs_free_fn)(nyr_cs_insn_t *, size_t);
typedef int (*nyr_cs_close_fn)(nyr_cs_handle *);
typedef struct {
  nyr_cs_open_fn open;
  nyr_cs_disasm_fn disasm;
  nyr_cs_free_fn free;
  nyr_cs_close_fn close;
  void *library;
} nyr_cs_api_t;
static _Thread_local char fallback_output[131072];

static nyr_cs_api_t *nyr_capstone_api(void) {
  static nyr_cs_api_t api;
  static int tried;
  if (tried)
    return api.library ? &api : NULL;
  const char *disabled = getenv("NYTRIX_NO_CAPSTONE");
  if (disabled && disabled[0] && strcmp(disabled, "0") != 0) {
    tried = 1;
    return NULL;
  }
  tried = 1;
#ifdef _WIN32
  const char *names[] = {"capstone.dll", "libcapstone.dll"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && !api.library; ++i)
    api.library = (void *)LoadLibraryA(names[i]);
  if (!api.library)
    return NULL;
  api.open = (nyr_cs_open_fn)GetProcAddress((HMODULE)api.library, "cs_open");
  api.disasm =
      (nyr_cs_disasm_fn)GetProcAddress((HMODULE)api.library, "cs_disasm");
  api.free = (nyr_cs_free_fn)GetProcAddress((HMODULE)api.library, "cs_free");
  api.close = (nyr_cs_close_fn)GetProcAddress((HMODULE)api.library, "cs_close");
#else
  const char *names[] = {"libcapstone.so.6", "libcapstone.so.5",
                         "libcapstone.so.4", "libcapstone.so",
                         "libcapstone.dylib"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && !api.library; ++i)
    api.library = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
  if (!api.library)
    return NULL;
  api.open = (nyr_cs_open_fn)dlsym(api.library, "cs_open");
  api.disasm = (nyr_cs_disasm_fn)dlsym(api.library, "cs_disasm");
  api.free = (nyr_cs_free_fn)dlsym(api.library, "cs_free");
  api.close = (nyr_cs_close_fn)dlsym(api.library, "cs_close");
#endif
  if (!api.open || !api.disasm || !api.free || !api.close) {
#ifdef _WIN32
    FreeLibrary((HMODULE)api.library);
#else
    dlclose(api.library);
#endif
    memset(&api, 0, sizeof(api));
    return NULL;
  }
  return &api;
}

int nyr_capstone_fallback_available(void) { return nyr_capstone_api() != NULL; }

const char *nyr_capstone_fallback_disasm(const unsigned char *bytes,
                                         int64_t length, int64_t address,
                                         int arch, int mode) {
  fallback_output[0] = 0;
  nyr_cs_api_t *api = nyr_capstone_api();
  if (!api || !bytes || length <= 0)
    return fallback_output;
  nyr_cs_handle handle = 0;
  if (api->open(arch, mode, &handle) != 0)
    return fallback_output;
  nyr_cs_insn_t *insns = NULL;
  size_t count =
      api->disasm(handle, bytes, (size_t)length, (uint64_t)address, 0, &insns);
  size_t used = 0;
  uint64_t cursor = (uint64_t)address;
  for (size_t i = 0; i < count; ++i) {
    if (insns[i].address > cursor) {
      int gap = snprintf(fallback_output + used, sizeof(fallback_output) - used,
                         "%" PRIu64
                         "\tunsupported\tundecoded byte at 0x%" PRIx64 "\t1\n",
                         cursor, cursor);
      if (gap < 0 || (size_t)gap >= sizeof(fallback_output) - used)
        break;
      used += (size_t)gap;
    }
    int n =
        snprintf(fallback_output + used, sizeof(fallback_output) - used,
                 "%" PRId64 "\t%s\t%s\t%u\n", (int64_t)insns[i].address,
                 insns[i].mnemonic, insns[i].op_str, (unsigned)insns[i].size);
    if (n < 0 || (size_t)n >= sizeof(fallback_output) - used)
      break;
    used += (size_t)n;
    cursor = insns[i].address + insns[i].size;
  }
  if (cursor < (uint64_t)address + (uint64_t)length &&
      used < sizeof(fallback_output)) {
    int tail = snprintf(fallback_output + used, sizeof(fallback_output) - used,
                        "%" PRIu64 "\tunsupported\tundecoded byte at 0x%" PRIx64
                        "\t1\n",
                        cursor, cursor);
    if (tail > 0 && (size_t)tail < sizeof(fallback_output) - used)
      used += (size_t)tail;
  }
  if (insns)
    api->free(insns, count);
  api->close(&handle);
  return fallback_output;
}

/*
 * Small, fail-closed decoder used by the Ny decompiler.  The accepted
 * instruction set is deliberately explicit: adding an encoding here also
 * makes it available to the language-level decoder without a host library.
 */
static _Thread_local char output[131072];
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
    if (((sib >> 3) & 7) != 4 || (rex & 2))
      snprintf(expr, sizeof(expr), "%s*%u", regs64[index], scale);
    if (mod == 0 && (sib & 7) == 5 && !(rex & 1)) {
      disp_size = 4;
    } else {
      if (expr[0])
        strncat(expr, "+", sizeof(expr) - strlen(expr) - 1);
      strncat(expr, regs64[base], sizeof(expr) - strlen(expr) - 1);
    }
  } else if (rip_relative) {
    strcpy(expr, "rip");
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
  if (width == 0) {
    if (disp_size && expr[0])
      snprintf(out, out_size, "[%s%s%" PRId64 "]", expr,
               disp_value >= 0 ? "+" : "", disp_value);
    else if (disp_size)
      snprintf(out, out_size, "[%" PRId64 "]", disp_value);
    else
      snprintf(out, out_size, "[%s]", expr);
  } else if (disp_size && expr[0]) {
    snprintf(out, out_size, "%s ptr [%s%s%" PRId64 "]",
             width == 8   ? "qword"
             : width == 2 ? "word"
             : width == 1 ? "byte"
                          : "dword",
             expr, disp_value >= 0 ? "+" : "", disp_value);
  } else if (disp_size) {
    snprintf(out, out_size, "%s ptr [%" PRId64 "]",
             width == 8   ? "qword"
             : width == 2 ? "word"
             : width == 1 ? "byte"
                          : "dword",
             disp_value);
  } else {
    snprintf(out, out_size, "%s ptr [%s]",
             width == 8   ? "qword"
             : width == 2 ? "word"
             : width == 1 ? "byte"
                          : "dword",
             expr);
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
  output[0] = 0;
  if (!b || n < 0)
    return "";
  for (int64_t i = 0; i < n;) {
    int64_t start = i;
    unsigned char op = b[i++];
    unsigned rex = 0, op16 = 0, rep = 0;
    while (op == 0x66 || op == 0x67 || op == 0xf3 || op == 0xf2 ||
           (op >= 0x40 && op <= 0x4f)) {
      if (op == 0x66)
        op16 = 1;
      else if (op == 0xf3 || op == 0xf2)
        rep = op;
      else if (op >= 0x40 && op <= 0x4f)
        rex = op;
      if (i >= n)
        goto unsupported;
      op = b[i++];
    }
    unsigned rb = (rex & 1) ? 8 : 0;
    char m[20] = "", args[512] = "";
    int reg = 0, rmreg = 0, mod = 0;
    int64_t rel = 0;
    unsigned imm = 0;
    unsigned w = (rex & 8) ? 8 : (op16 ? 2 : 4);
    if (op == 0x90)
      strcpy(m, "nop");
    else if (op == 0xa4) {
      strcpy(m, rep == 0xf3 ? "rep movsb" : "movsb");
    } else if (op == 0xd9 && i < n && (b[i] == 0xfe || b[i] == 0xff)) {
      strcpy(m, b[i++] == 0xfe ? "fsin" : "fcos");
    } else if (op == 0xdd && i < n) {
      unsigned char mr = b[i++];
      unsigned group = (mr >> 3) & 7;
      char operand[128];
      if ((mr >> 6) == 3 || (group != 0 && group != 3) ||
          !nyr_x64_rm(b, n, &i, mr, rex, 8, operand, sizeof(operand)))
        goto unsupported;
      strcpy(m, group == 0 ? "fld" : "fstp");
      snprintf(args, sizeof(args), "%s", operand);
    } else if (op == 0xc3)
      strcpy(m, "ret");
    else if (op == 0xc9)
      strcpy(m, "leave");
    else if (op == 0xcc)
      strcpy(m, "int3");
    else if (op == 0x98)
      strcpy(m, (rex & 8) ? "cdqe" : "cwde");
    else if (op == 0x99)
      strcpy(m, (rex & 8) ? "cqo" : "cdq");
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
          strcpy(m, scalar_width ? (rep == 0xf3 ? "movss" : "movsd")
                                 : (op16 ? "movupd" : "movups"));
          snprintf(args, sizeof(args), "%s, %s",
                   op2 == 0x10 ? reg_name : rm_name,
                   op2 == 0x10 ? rm_name : reg_name);
        } else {
          strcpy(m, op16 ? "movapd" : "movaps");
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
        strcpy(m, rep == 0xf3 ? "movdqu" : "movdqa");
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
        strcpy(m, move_width == 8 ? "movq" : "movd");
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
        strcpy(m, op16 ? (op2 == 0x14 ? "unpcklpd" : "unpckhpd")
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
          strcpy(m, "movddup");
        else if (rep == 0xf3 && op2 == 0x12)
          strcpy(m, "movsldup");
        else if (rep == 0xf3 && op2 == 0x16)
          strcpy(m, "movshdup");
        else if (rep)
          goto unsupported;
        else if (modrm_mod == 3 && op2 == 0x12)
          strcpy(m, "movhlps");
        else if (modrm_mod == 3 && op2 == 0x16)
          strcpy(m, "movlhps");
        else if (op2 == 0x12 || op2 == 0x13)
          strcpy(m, "movlps");
        else
          strcpy(m, "movhps");
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
        strcpy(m, stem);
        strcat(m, suffix);
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
        strcpy(m, op16 ? "shufpd" : "shufps");
        snprintf(args, sizeof(args), "%s, %s, 0x%x", reg_name, rm_name, b[i++]);
      } else if ((op2 == 0x2e || op2 == 0x2f) && i < n && !rep) {
        unsigned char mr = b[i++];
        char reg_name[16], rm_name[192];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        snprintf(reg_name, sizeof(reg_name), "xmm%u", reg);
        if (!nyr_x64_vec_rm(b, n, &i, mr, rex, 128, op16 ? 8 : 4, rm_name,
                            sizeof(rm_name)))
          goto unsupported;
        strcpy(m, op2 == 0x2e ? (op16 ? "ucomisd" : "ucomiss")
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
        strcpy(m, "punpcklqdq");
        snprintf(args, sizeof(args), "%s, %s", reg_name, rm_name);
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
        strcpy(m, op2 == 0xd4   ? "paddq"
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
        strcpy(m, "psllq");
        snprintf(args, sizeof(args), "xmm%u, 0x%x", dest, b[i++]);
      } else if (op2 == 0x12 && rep == 0xf2 && i < n) {
        unsigned char mr = b[i++];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        char reg_name[16], rm_name[192];
        snprintf(reg_name, sizeof(reg_name), "xmm%u", reg);
        if (!nyr_x64_vec_rm(b, n, &i, mr, rex, 128, 8, rm_name,
                            sizeof(rm_name)))
          goto unsupported;
        strcpy(m, "movddup");
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
        strcpy(m, rep == 0xf3 ? "cvtss2sd" : "cvtsd2ss");
        snprintf(args, sizeof(args), "%s, %s", reg_name, src);
      } else if (op2 >= 0x80 && op2 <= 0x8f && i + 4 <= n) {
        rel = (int32_t)rd(b + i, 4);
        i += 4;
        strcpy(m, "j");
        strcat(m, cc[op2 & 15]);
        snprintf(args, sizeof(args), "0x%" PRIx64, (uint64_t)(base + i + rel));
      } else if (op2 >= 0x40 && op2 <= 0x4f && i < n) {
        unsigned char mr = b[i++];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        char src[128];
        if (!nyr_x64_rm(b, n, &i, mr, rex, w, src, sizeof(src)))
          goto unsupported;
        strcpy(m, "cmov");
        strcat(m, cc[op2 & 15]);
        snprintf(args, sizeof(args), "%s, %s", nyr_x64_reg(reg, w, rex), src);
      } else if (op2 >= 0x90 && op2 <= 0x9f && i < n) {
        unsigned char mr = b[i++];
        char dst[128];
        if (!nyr_x64_rm(b, n, &i, mr, rex, 1, dst, sizeof(dst)))
          goto unsupported;
        strcpy(m, "set");
        strcat(m, cc[op2 & 15]);
        snprintf(args, sizeof(args), "%s", dst);
      } else if (op2 == 0xaf && i < n) {
        unsigned char mr = b[i++];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        char src[128];
        if (!nyr_x64_rm(b, n, &i, mr, rex, w, src, sizeof(src)))
          goto unsupported;
        strcpy(m, "imul");
        snprintf(args, sizeof(args), "%s, %s", nyr_x64_reg(reg, w, rex), src);
      } else if ((op2 == 0xb6 || op2 == 0xb7 || op2 == 0xbe || op2 == 0xbf) &&
                 i < n) {
        unsigned char mr = b[i++];
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        unsigned sw = (op2 == 0xb6 || op2 == 0xbe) ? 1 : 2;
        char src[128];
        if (!nyr_x64_rm(b, n, &i, mr, rex, sw, src, sizeof(src)))
          goto unsupported;
        strcpy(m, (op2 == 0xb6 || op2 == 0xb7) ? "movzx" : "movsx");
        snprintf(args, sizeof(args), "%s, %s", nyr_x64_reg(reg, w, rex), src);
      } else if (op2 == 0x1f && i < n) {
        unsigned char mr = b[i++];
        char discard[128];
        if (!nyr_x64_rm(b, n, &i, mr, rex, 0, discard, sizeof(discard)))
          goto unsupported;
        strcpy(m, "nop");
      } else if (op2 == 0x1e && rep == 0xf3 && i < n && b[i++] == 0xfa) {
        strcpy(m, "endbr64");
      } else if (op2 == 0x05) {
        strcpy(m, "syscall");
      } else if (op2 == 0x0b) {
        strcpy(m, "ud2");
      } else if (op2 >= 0xc8 && op2 <= 0xcf) {
        strcpy(m, "bswap");
        snprintf(args, sizeof(args), "%s", nyr_x64_reg((op2 & 7) + rb, w, rex));
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
      if (i >= n)
        goto unsupported;
      unsigned char mr = b[i++];
      unsigned vector_reg = ((mr >> 3) & 7) + ((vex_rex & 4) ? 8 : 0);
      unsigned vector_width = (vex3 & 4) ? 256 : 128;
      unsigned vector_vvvv = (unsigned)(~vex3 >> 3) & 15;
      char reg_name[16], rm_name[192];
      snprintf(reg_name, sizeof(reg_name), "%s%u",
               vector_width == 256 ? "ymm" : "xmm", vector_reg);
      unsigned rm_width =
          map == 1 && (opcode == 0x10 || opcode == 0x11) && (pp == 2 || pp == 3)
              ? (pp == 2 ? 4 : 8)
          : map == 2 && opcode == 0x19 ? 8
          : map == 2 && opcode == 0x59 ? 8
          : map == 1 && opcode >= 0x58 && opcode <= 0x5e &&
                  (opcode == 0x58 || opcode == 0x59 || opcode == 0x5c ||
                   opcode == 0x5e) &&
                  (pp == 2 || pp == 3)
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
        strcpy(m, opcode == 0x28 || opcode == 0x29
                      ? (pp == 0 ? "vmovaps" : "vmovapd")
                      : (pp == 0 ? "vmovups" : "vmovupd"));
        int store = opcode == 0x11 || opcode == 0x29;
        snprintf(args, sizeof(args), "%s, %s", store ? rm_name : reg_name,
                 store ? reg_name : rm_name);
      } else if (map == 1 && (opcode == 0x6f || opcode == 0x7f) &&
                 (pp == 1 || pp == 2) && !(vex3 & 0x80) &&
                 (vex3 & 0x78) == 0x78) {
        strcpy(m, pp == 1 ? "vmovdqa" : "vmovdqu");
        if (opcode == 0x6f)
          snprintf(args, sizeof(args), "%s, %s", reg_name, rm_name);
        else
          snprintf(args, sizeof(args), "%s, %s", rm_name, reg_name);
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
        strcpy(m, name);
        snprintf(args, sizeof(args), "%s, %s, %s", reg_name, vvvv_name,
                 rm_name);
      } else if (map == 2 && pp == 1 && !(vex3 & 0x80) && opcode == 0x19 &&
                 (vex3 & 0x78) == 0x78 && (mr >> 6) != 3) {
        strcpy(m, "vbroadcastsd");
        snprintf(args, sizeof(args), "%s, %s", reg_name, rm_name);
      } else if (map == 2 && pp == 1 && (vex3 & 0x80) && opcode == 0xa8) {
        char vvvv_name[16];
        snprintf(vvvv_name, sizeof(vvvv_name), "%s%u",
                 vector_width == 256 ? "ymm" : "xmm", vector_vvvv);
        strcpy(m, "vfmadd213pd");
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
        strcpy(m, stem);
        strcat(m, suffix);
        snprintf(args, sizeof(args), "%s, %s, %s", reg_name, vvvv_name,
                 rm_name);
      } else if (map == 1 && opcode == 0x51 && (pp <= 3) && !(vex3 & 0x80) &&
                 (vex3 & 0x78) == 0x78) {
        const char *suffix = pp == 0   ? "ps"
                             : pp == 1 ? "pd"
                             : pp == 2 ? "ss"
                                       : "sd";
        strcpy(m, "vsqrt");
        strcat(m, suffix);
        snprintf(args, sizeof(args), "%s, %s", reg_name, rm_name);
      } else if (map == 2 && pp == 1 && (vex3 & 0x80) && opcode == 0x59 &&
                 (vex3 & 0x78) == 0x78 && vector_width == 256 &&
                 (mr >> 6) != 3) {
        strcpy(m, "vpbroadcastq");
        snprintf(args, sizeof(args), "%s, %s", reg_name, rm_name);
      } else if (map == 3 && pp == 1 && vector_width == 256 && !(vex3 & 0x80) &&
                 opcode == 0x19 && (vex3 & 0x78) == 0x78 && (mr >> 6) == 3 &&
                 i < n) {
        unsigned xmm_dest = (mr & 7) + ((vex_rex & 1) ? 8 : 0);
        unsigned half = b[i++];
        if (half > 1)
          goto unsupported;
        strcpy(m, "vextractf128");
        snprintf(args, sizeof(args), "xmm%u, %s, %u", xmm_dest, reg_name, half);
      } else {
        goto unsupported;
      }
    } else if (op == 0xe8 || op == 0xe9) {
      if (i + 4 > n)
        goto unsupported;
      rel = (int32_t)rd(b + i, 4);
      i += 4;
      strcpy(m, op == 0xe8 ? "call" : "jmp");
      snprintf(args, sizeof(args), "0x%" PRIx64, (uint64_t)(base + i + rel));
    } else if (op == 0xeb || (op >= 0x70 && op <= 0x7f)) {
      if (i >= n)
        goto unsupported;
      rel = (int8_t)b[i++];
      if (op == 0xeb)
        strcpy(m, "jmp");
      else {
        static const char *cc[] = {"jo",  "jno", "jb",  "jae", "je", "jne",
                                   "jbe", "ja",  "js",  "jns", "jp", "jnp",
                                   "jl",  "jge", "jle", "jg"};
        strcpy(m, cc[op & 15]);
      }
      snprintf(args, sizeof(args), "0x%" PRIx64, (uint64_t)(base + i + rel));
    } else if (op >= 0x50 && op <= 0x5f) {
      strcpy(m, op < 0x58 ? "push" : "pop");
      snprintf(args, sizeof(args), "%s", regs64[(op & 7) + rb]);
    } else if (op >= 0xb8 && op <= 0xbf) {
      imm = w;
      if (i + imm > n)
        goto unsupported;
      strcpy(m, "mov");
      snprintf(args, sizeof(args), "%s, 0x%" PRIx64,
               (rex & 8 ? regs64 : regs32)[(op & 7) + rb], rd(b + i, imm));
      i += imm;
    } else if (op == 0x68 || op == 0x6a) {
      imm = op == 0x6a ? 1 : 4;
      if (i + imm > n)
        goto unsupported;
      strcpy(m, "push");
      snprintf(args, sizeof(args), "0x%" PRIx64, rd(b + i, imm));
      i += imm;
    } else {
      const char *mn = NULL;
      int dir = 0;
      switch (op) {
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
      if (mn) {
        if (i >= n)
          goto unsupported;
        unsigned char mr = b[i++];
        mod = mr >> 6;
        reg = ((mr >> 3) & 7) + ((rex & 4) ? 8 : 0);
        rmreg = (mr & 7) + ((rex & 1) ? 8 : 0);
        char lhs[128], rhs[128];
        unsigned width = op == 0x84 || op == 0x88 || op == 0x8a ? 1 : w;
        if (!nyr_x64_rm(b, n, &i, mr, rex, op == 0x8d ? 0 : width, lhs,
                        sizeof(lhs)))
          goto unsupported;
        snprintf(rhs, sizeof(rhs), "%s", nyr_x64_reg(reg, width, rex));
        strcpy(m, mn);
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
        strcpy(m, "imul");
        snprintf(args, sizeof(args), "%s, %s, %s%" PRIx64,
                 nyr_x64_reg(reg, w, rex), src, iv < 0 ? "-0x" : "0x",
                 (uint64_t)(iv < 0 ? -iv : iv));
      } else if (op == 0xc0 || op == 0xc1 || op == 0xd0 || op == 0xd1 ||
                 op == 0xd2 || op == 0xd3) {
        if (i >= n)
          goto unsupported;
        unsigned char mr = b[i++];
        mod = mr >> 6;
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
        strcpy(m, name);
        if (op == 0xc0 || op == 0xc1) {
          if (i >= n)
            goto unsupported;
          snprintf(args, sizeof(args), "%s, 0x%x", dst, b[i++]);
        } else if (op == 0xd2 || op == 0xd3) {
          snprintf(args, sizeof(args), "%s, cl", dst);
        } else {
          snprintf(args, sizeof(args), "%s, 1", dst);
        }
      } else if (op == 0x80 || op == 0x81 || op == 0x83 || op == 0xc6 ||
                 op == 0xc7) {
        if (i >= n)
          goto unsupported;
        unsigned char mr = b[i++];
        mod = mr >> 6;
        reg = (mr >> 3) & 7;
        rmreg = (mr & 7) + rb;
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
        strcpy(m, imn);
        if (iv < 0)
          snprintf(args, sizeof(args), "%s, -0x%" PRIx64, dst, (uint64_t)-iv);
        else
          snprintf(args, sizeof(args), "%s, 0x%" PRIx64, dst, (uint64_t)iv);
      } else if (op == 0xff) {
        if (i >= n)
          goto unsupported;
        unsigned char mr = b[i++];
        mod = mr >> 6;
        reg = (mr >> 3) & 7;
        rmreg = (mr & 7) + rb;
        if (mod != 3)
          goto unsupported;
        strcpy(m, reg == 2   ? "call"
                  : reg == 4 ? "jmp"
                  : reg == 6 ? "push"
                             : "");
        if (!m[0])
          goto unsupported;
        snprintf(args, sizeof(args), "%s", regs64[rmreg]);
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
          strcpy(m, "test");
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
        strcpy(m, name);
        snprintf(args, sizeof(args), "%s", operand);
      } else
        goto unsupported;
    }
    if (!m[0])
      goto unsupported;
  decoded:
    {
      int z = snprintf(output + used, sizeof(output) - used,
                       "%" PRId64 "\t%s\t%s\t%" PRId64 "\n", base + start, m,
                       args, i - start);
      if (z < 0 || (size_t)z >= sizeof(output) - used)
        break;
      used += (size_t)z;
    }
    continue;
  unsupported:
    {
      unsigned v = (i > start ? b[start] : op);
      snprintf(output + used, sizeof(output) - used,
               "%" PRId64 "\tunsupported\topcode 0x%02x at 0x%" PRIx64 "\t1\n",
               base + start, v, (uint64_t)(base + start));
      return output;
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
      strcpy(mnemonic, "nop");
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
      strcpy(mnemonic, load ? "ldp" : "stp");
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
      strcpy(mnemonic, "sdiv");
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
      strcpy(mnemonic, shift_op == 10   ? "asrv"
                       : shift_op == 11 ? "rorv"
                                        : "lslv");
      snprintf(operands, sizeof(operands), "%s, %s, %s",
               nyr_a64_reg(rd0, width, 0), nyr_a64_reg(rn, width, 0),
               nyr_a64_reg(rm, width, 0));
    } else if ((insn & 0xffe0fc00) == 0x9b407c00) {
      strcpy(mnemonic, "umulh");
      snprintf(operands, sizeof(operands), "%s, %s, %s", nyr_a64_reg(rd0, 1, 0),
               nyr_a64_reg(rn, 1, 0), nyr_a64_reg(rm, 1, 0));
    } else if ((insn & 0x7fe08000) == 0x1b008000 ||
               (insn & 0x7fe08000) == 0x1b000000) {
      unsigned acc = (insn >> 10) & 31;
      strcpy(mnemonic, (insn & 0x8000) ? "msub" : "madd");
      snprintf(operands, sizeof(operands), "%s, %s, %s, %s",
               nyr_a64_reg(rd0, width, 0), nyr_a64_reg(rn, width, 0),
               nyr_a64_reg(rm, width, 0), nyr_a64_reg(acc, width, 0));
    } else if ((insn & 0x1fe00c00) == 0x1a800000) {
      static const char *cc[] = {"eq", "ne", "cs", "cc", "mi", "pl",
                                 "vs", "vc", "hi", "ls", "ge", "lt",
                                 "gt", "le", "al", "nv"};
      strcpy(mnemonic, "csel");
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
      strcpy(mnemonic, op == 0x1e202800 || op == 0x1e602800   ? "fadd"
                       : op == 0x1e203800 || op == 0x1e603800 ? "fsub"
                       : op == 0x1e200800 || op == 0x1e600800 ? "fmul"
                                                              : "fdiv");
      char fp = (op & 0x00400000) ? 'd' : 's';
      snprintf(operands, sizeof(operands), "%c%u, %c%u, %c%u", fp, rd0, fp, rn,
               fp, rm);
    } else if ((insn & 0xffe0fc00) == 0x1e202000 ||
               (insn & 0xffe0fc00) == 0x1e602000) {
      strcpy(mnemonic, "fcmp");
      char fp = ((insn & 0xffe0fc00) == 0x1e602000) ? 'd' : 's';
      snprintf(operands, sizeof(operands), "%c%u, %c%u", fp, rn, fp, rm);
    } else if ((insn & 0xfffffc00) == 0x1e204000 ||
               (insn & 0xfffffc00) == 0x1e604000) {
      strcpy(mnemonic, "fmov");
      char fp = ((insn & 0xfffffc00) == 0x1e604000) ? 'd' : 's';
      snprintf(operands, sizeof(operands), "%c%u, %c%u", fp, rd0, fp, rn);
    } else if ((insn & 0xfffffc00) == 0x9e660000 ||
               (insn & 0xfffffc00) == 0x9e260000) {
      strcpy(mnemonic, "fmov");
      snprintf(operands, sizeof(operands), "%s, %c%u", nyr_a64_reg(rd0, 1, 0),
               (insn & 0x00400000) ? 'd' : 's', rn);
    } else if ((insn & 0xfffffc00) == 0x9e670000 ||
               (insn & 0xfffffc00) == 0x9e270000) {
      strcpy(mnemonic, "fmov");
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
        strcpy(mnemonic, load ? "ldur" : "stur");
        if (imm)
          snprintf(operands, sizeof(operands), "q%u, [%s, #%" PRId64 "]", rd0,
                   base_reg, imm);
        else
          snprintf(operands, sizeof(operands), "q%u, [%s]", rd0, base_reg);
      } else if (op == 0xfc400000 || op == 0xfc000000 || op == 0xbc400000 ||
                 op == 0xbc000000) {
        char width_name = op == 0xfc400000 || op == 0xfc000000 ? 'd' : 's';
        strcpy(mnemonic, load ? "ldur" : "stur");
        if (imm)
          snprintf(operands, sizeof(operands), "%c%u, [%s, #%" PRId64 "]",
                   width_name, rd0, base_reg, imm);
        else
          snprintf(operands, sizeof(operands), "%c%u, [%s]", width_name, rd0,
                   base_reg);
      } else {
        strcpy(mnemonic, load ? "ldur" : "stur");
        if (imm)
          snprintf(operands, sizeof(operands), "%s, [%s, #%" PRId64 "]",
                   nyr_a64_reg(rd0, 1, 0), base_reg, imm);
        else
          snprintf(operands, sizeof(operands), "%s, [%s]",
                   nyr_a64_reg(rd0, 1, 0), base_reg);
      }
    } else if ((insn & 0xfffffc00) == 0x9e620000 ||
               (insn & 0xfffffc00) == 0x9e220000) {
      strcpy(mnemonic, "scvtf");
      snprintf(operands, sizeof(operands), "%c%u, %s",
               (insn & 0x00400000) ? 'd' : 's', rd0, nyr_a64_reg(rn, 1, 0));
    } else if ((insn & 0xfffffc00) == 0x9e780000 ||
               (insn & 0xfffffc00) == 0x9e380000) {
      strcpy(mnemonic, "fcvtzs");
      snprintf(operands, sizeof(operands), "%s, %c%u", nyr_a64_reg(rd0, 1, 0),
               (insn & 0x00400000) ? 'd' : 's', rn);
    } else if ((insn & 0xfffffc00) == 0x1e22c000 ||
               (insn & 0xfffffc00) == 0x1e624000) {
      strcpy(mnemonic, "fcvt");
      int to_double = (insn & 0xfffffc00) == 0x1e22c000;
      snprintf(operands, sizeof(operands), "%c%u, %c%u", to_double ? 'd' : 's',
               rd0, to_double ? 's' : 'd', rn);
    } else if ((insn & 0xfffffc1f) == 0xd65f0000) {
      strcpy(mnemonic, "ret");
      if (rn != 30)
        snprintf(operands, sizeof(operands), "%s", nyr_a64_reg(rn, 1, 0));
    } else if ((insn & 0xfffffc1f) == 0xd61f0000 ||
               (insn & 0xfffffc1f) == 0xd63f0000) {
      strcpy(mnemonic, (insn & 0x00200000) ? "blr" : "br");
      snprintf(operands, sizeof(operands), "%s", nyr_a64_reg(rn, 1, 0));
    } else if ((insn & 0xffe0001f) == 0xd4000001) {
      strcpy(mnemonic, "svc");
      snprintf(operands, sizeof(operands), "#%u", (insn >> 5) & 0xffff);
    } else if ((insn & 0xffe0001f) == 0xd4200000) {
      strcpy(mnemonic, "brk");
      snprintf(operands, sizeof(operands), "#%u", (insn >> 5) & 0xffff);
    } else if ((insn & 0x7c000000) == 0x14000000) {
      strcpy(mnemonic, (insn & 0x80000000) ? "bl" : "b");
      target = base + off + nyr_sign_extend(insn & 0x03ffffff, 26) * 4;
      snprintf(operands, sizeof(operands), "0x%" PRIx64, (uint64_t)target);
    } else if ((insn & 0xff000010) == 0x54000000) {
      static const char *cc[] = {"eq", "ne", "cs", "cc", "mi", "pl",
                                 "vs", "vc", "hi", "ls", "ge", "lt",
                                 "gt", "le", "al", "nv"};
      strcpy(mnemonic, "b.");
      strcat(mnemonic, cc[insn & 15]);
      target = base + off + nyr_sign_extend((insn >> 5) & 0x7ffff, 19) * 4;
      snprintf(operands, sizeof(operands), "0x%" PRIx64, (uint64_t)target);
    } else if ((insn & 0x7e000000) == 0x34000000) {
      strcpy(mnemonic, (insn & 0x01000000) ? "cbnz" : "cbz");
      target = base + off + nyr_sign_extend((insn >> 5) & 0x7ffff, 19) * 4;
      snprintf(operands, sizeof(operands), "%s, 0x%" PRIx64,
               nyr_a64_reg(rd0, width, 0), (uint64_t)target);
    } else if ((insn & 0x7e000000) == 0x36000000) {
      strcpy(mnemonic, (insn & 0x01000000) ? "tbnz" : "tbz");
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
        strcpy(mnemonic, sub ? "cmp" : "cmn");
      else
        strcpy(mnemonic,
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
        strcpy(mnemonic, sub ? "cmp" : "cmn");
      else
        strcpy(mnemonic,
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
      strcpy(mnemonic, opc == 3 ? "movk" : opc == 0 ? "movn" : "movz");
      if (shift)
        snprintf(operands, sizeof(operands), "%s, #0x%" PRIx64 ", lsl #%u",
                 nyr_a64_reg(rd0, width, 0), imm, shift);
      else
        snprintf(operands, sizeof(operands), "%s, #0x%" PRIx64,
                 nyr_a64_reg(rd0, width, 0), imm);
    } else if ((insn & 0xfffffc00) == 0xdac00c00 ||
               (insn & 0xfffffc00) == 0x5ac00800) {
      strcpy(mnemonic, "rev");
      snprintf(operands, sizeof(operands), "%s, %s", nyr_a64_reg(rd0, width, 0),
               nyr_a64_reg(rn, width, 0));
    } else if ((insn & 0x9f000000) == 0x90000000) {
      uint64_t imm =
          ((uint64_t)((insn >> 5) & 0x7ffff) << 2) | ((insn >> 29) & 3);
      int64_t pages = nyr_sign_extend(imm, 21);
      uint64_t pc_page = (uint64_t)(base + off) & ~UINT64_C(0xfff);
      target = (int64_t)(pc_page + pages * INT64_C(4096));
      strcpy(mnemonic, "adrp");
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
      strcpy(mnemonic, shift);
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
      strcpy(mnemonic, name);
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
        strcpy(mnemonic, "mov");
        snprintf(operands, sizeof(operands), "%s, %s",
                 nyr_a64_reg(rd0, width, 0), nyr_a64_reg(rm, width, 0));
      } else {
        strcpy(mnemonic, opc == 0   ? "and"
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
      strcpy(mnemonic,
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
      strcpy(mnemonic, "tbl");
      snprintf(operands, sizeof(operands), "v%u.16b, %s, v%u.16b", rd0, table,
               rm);
    } else {
      uint32_t vec_op = insn & 0xffe0fc00;
      const char *shape = NULL;
      if ((insn & 0xfffffc00) == 0x7e70d800) {
        strcpy(mnemonic, "faddp");
        snprintf(operands, sizeof(operands), "d%u, v%u.2d", rd0, rn);
      } else if ((insn & 0xfffffc00) == 0x4e080400 ||
                 (insn & 0xfffffc00) == 0x4e040400) {
        int is64 = (insn & 0x00080000) != 0;
        nyr_a64_vec_reg(vd, sizeof(vd), rd0, is64 ? "2d" : "4s");
        strcpy(mnemonic, "dup");
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
          strcpy(mnemonic, "add");
        else if (vec_op == 0x6ee08400)
          strcpy(mnemonic, "sub");
        else if (vec_op == 0x4e201c00)
          strcpy(mnemonic, "and");
        else if (vec_op == 0x4ea01c00)
          strcpy(mnemonic, "orr");
        else if (vec_op == 0x6e201c00)
          strcpy(mnemonic, "eor");
        else if (vec_op == 0x4e60d400 || vec_op == 0x4e20d400)
          strcpy(mnemonic, "fadd");
        else if (vec_op == 0x4ee0d400 || vec_op == 0x4ea0d400)
          strcpy(mnemonic, "fsub");
        else if (vec_op == 0x6e60dc00 || vec_op == 0x6e20dc00)
          strcpy(mnemonic, "fmul");
        else if (vec_op == 0x6e60fc00 || vec_op == 0x6e20fc00)
          strcpy(mnemonic, "fdiv");
        else
          strcpy(mnemonic, "fmla");
        nyr_a64_vec_reg(vd, sizeof(vd), rd0, shape);
        nyr_a64_vec_reg(vn, sizeof(vn), rn, shape);
        nyr_a64_vec_reg(vm, sizeof(vm), rm, shape);
        snprintf(operands, sizeof(operands), "%s, %s, %s", vd, vn, vm);
      } else if ((insn & 0xfffffc00) == 0x4e080c00 ||
                 (insn & 0xfffffc00) == 0x4e040c00) {
        unsigned is64 = (insn & 0x00080000) != 0;
        nyr_a64_vec_reg(vd, sizeof(vd), rd0, is64 ? "2d" : "4s");
        strcpy(mnemonic, "dup");
        snprintf(operands, sizeof(operands), "%s, %s", vd,
                 nyr_a64_reg(rn, is64, 0));
      } else if ((insn & 0xfffffc00) == 0x9e670000) {
        strcpy(mnemonic, "fmov");
        snprintf(operands, sizeof(operands), "d%u, %s", rd0,
                 nyr_a64_reg(rn, 1, 0));
      } else if ((insn & 0xfffffc00) == 0x4e181c00) {
        strcpy(mnemonic, "ins");
        snprintf(operands, sizeof(operands), "v%u.d[1], %s", rd0,
                 nyr_a64_reg(rn, 1, 0));
      } else if (vec_op == 0x6ee0b800) {
        nyr_a64_vec_reg(vd, sizeof(vd), rd0, "2d");
        nyr_a64_vec_reg(vn, sizeof(vn), rn, "2d");
        strcpy(mnemonic, "neg");
        snprintf(operands, sizeof(operands), "%s, %s", vd, vn);
      } else if (vec_op == 0x4ee04400) {
        nyr_a64_vec_reg(vd, sizeof(vd), rd0, "2d");
        nyr_a64_vec_reg(vn, sizeof(vn), rn, "2d");
        nyr_a64_vec_reg(vm, sizeof(vm), rm, "2d");
        strcpy(mnemonic, "sshl");
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
        strcpy(mnemonic, opc ? (bytes == 1   ? "ldrb"
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
        strcpy(mnemonic, opc == 2   ? "ldrsw"
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
      int z = snprintf(a64_output + used, sizeof(a64_output) - used,
                       "%" PRId64 "\t%s\t%s\t%u\n", base + off, mnemonic,
                       operands, size);
      if (z < 0 || (size_t)z >= sizeof(a64_output) - used)
        break;
      used += (size_t)z;
    }
    continue;
  unsupported_a64:
    snprintf(a64_output + used, sizeof(a64_output) - used,
             "%" PRId64 "\tunsupported\tinstruction word 0x%08" PRIx32
             " at 0x%" PRIx64 "\t4\n",
             base + off, insn, (uint64_t)(base + off));
    return a64_output;
  }
  return a64_output;
}
