#include "code/native/internal.h"
#include "code/native/ir.h"
#include "code/c/c.h"
#include "base/common.h"
#include "base/util.h"
#include "base/time.h"
#include "wire/build.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Non-LLVM native backend entry point and target registry.
 *
 * LLVM remains the default backend. When a native backend is explicitly
 * selected, unsupported registered targets must fail with a precise diagnostic
 * instead of silently falling back. x86-64 is the only assembly emitter today;
 * other registered target names exist so the roadmap can add emitters
 * incrementally behind stable option parsing and tests.
 */

void ny_native_set_err(char *err, size_t err_len, const char *fmt, ...) {
  if (!err || err_len == 0)
    return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, err_len, fmt, ap);
  va_end(ap);
}

static bool ny_native_reserve(ny_native_writer_t *w, size_t add) {
  if (!w)
    return false;
  if (add > SIZE_MAX - w->len - 1)
    return false;
  size_t need = w->len + add + 1;
  if (need <= w->cap)
    return true;
  size_t cap = w->cap ? w->cap : 4096;
  while (cap < need) {
    if (cap > SIZE_MAX / 2)
      return false;
    cap *= 2;
  }
  char *data = realloc(w->data, cap);
  if (!data)
    return false;
  w->data = data;
  w->cap = cap;
  return true;
}

bool ny_native_put(ny_native_writer_t *w, const char *s) {
  if (!s)
    return true;
  size_t n = strlen(s);
  if (!ny_native_reserve(w, n))
    return false;
  memcpy(w->data + w->len, s, n + 1);
  w->len += n;
  return true;
}

bool ny_native_printf(ny_native_writer_t *w, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0) {
    va_end(ap2);
    return false;
  }
  if (!ny_native_reserve(w, (size_t)n)) {
    va_end(ap2);
    return false;
  }
  vsnprintf(w->data + w->len, w->cap - w->len, fmt, ap2);
  va_end(ap2);
  w->len += (size_t)n;
  return true;
}

static bool ny_native_triple_is_windows(const char *triple) {
  return triple && (strstr(triple, "windows") || strstr(triple, "mingw") ||
                    strstr(triple, "msvc") || strstr(triple, "win32"));
}

static bool ny_native_triple_is_macho(const char *triple) {
  return triple && (strstr(triple, "apple") || strstr(triple, "darwin") ||
                    strstr(triple, "macos"));
}

static const char *ny_native_arm_float_abi_name(void) {
  const char *abi = getenv("NYTRIX_ARM_FLOAT_ABI");
  if (abi && *abi) {
    if (strcmp(abi, "hard") == 0 || strcmp(abi, "softfp") == 0 ||
        strcmp(abi, "soft") == 0)
      return abi;
  }
  return "softfp";
}

static const char *ny_native_abi_label(ny_native_abi_t abi) {
  switch (abi) {
  case NY_NATIVE_ABI_SYSV:
    return "sysv";
  case NY_NATIVE_ABI_WIN64:
    return "win64";
  case NY_NATIVE_ABI_AAPCS:
    return "aapcs";
  default:
    return "auto";
  }
}

static bool ny_native_backend_target(ny_native_backend_t backend,
                                     ny_native_target_t *target,
                                     const char **name) {
  if (!target || !name)
    return false;
  switch (backend) {
  case NY_NATIVE_BACKEND_X86_64:
    *target = NY_NATIVE_TARGET_X86_64;
    *name = "x86_64";
    return true;
  case NY_NATIVE_BACKEND_X86:
    *target = NY_NATIVE_TARGET_X86;
    *name = "x86";
    return true;
  case NY_NATIVE_BACKEND_AARCH64:
    *target = NY_NATIVE_TARGET_AARCH64;
    *name = "aarch64";
    return true;
  case NY_NATIVE_BACKEND_AMDGPU:
    *target = NY_NATIVE_TARGET_AMDGPU;
    *name = "amdgpu";
    return true;
  case NY_NATIVE_BACKEND_ARM:
    *target = NY_NATIVE_TARGET_ARM;
    *name = "arm";
    return true;
  case NY_NATIVE_BACKEND_AVR:
    *target = NY_NATIVE_TARGET_AVR;
    *name = "avr";
    return true;
  case NY_NATIVE_BACKEND_BPF:
    *target = NY_NATIVE_TARGET_BPF;
    *name = "bpf";
    return true;
  case NY_NATIVE_BACKEND_MIPS:
    *target = NY_NATIVE_TARGET_MIPS;
    *name = "mips";
    return true;
  case NY_NATIVE_BACKEND_POWERPC:
    *target = NY_NATIVE_TARGET_POWERPC;
    *name = "powerpc";
    return true;
  case NY_NATIVE_BACKEND_RISCV:
    *target = NY_NATIVE_TARGET_RISCV;
    *name = "riscv";
    return true;
  case NY_NATIVE_BACKEND_WASM:
    *target = NY_NATIVE_TARGET_WASM;
    *name = "wasm";
    return true;
  default:
    *target = NY_NATIVE_TARGET_UNKNOWN;
    *name = "unknown";
    return false;
  }
}

static bool ny_native_nir_binop(const char *op, ny_nir_op_t *out) {
  if (!op || !out)
    return false;
  if (strcmp(op, "+") == 0)
    *out = NY_NIR_ADD_I64;
  else if (strcmp(op, "-") == 0)
    *out = NY_NIR_SUB_I64;
  else if (strcmp(op, "*") == 0)
    *out = NY_NIR_MUL_I64;
  else if (strcmp(op, "/") == 0)
    *out = NY_NIR_DIV_I64;
  else if (strcmp(op, "%") == 0)
    *out = NY_NIR_MOD_I64;
  else if (strcmp(op, "&") == 0)
    *out = NY_NIR_AND_I64;
  else if (strcmp(op, "|") == 0)
    *out = NY_NIR_OR_I64;
  else if (strcmp(op, "^^") == 0)
    *out = NY_NIR_XOR_I64;
  else if (strcmp(op, "<<") == 0)
    *out = NY_NIR_SHL_I64;
  else if (strcmp(op, ">>") == 0)
    *out = NY_NIR_SAR_I64;
  else
    return false;
  return true;
}

static bool ny_native_nir_cmp(const char *op, ny_nir_cmp_t *out) {
  if (!op || !out)
    return false;
  if (strcmp(op, "==") == 0)
    *out = NY_NIR_CMP_EQ;
  else if (strcmp(op, "!=") == 0)
    *out = NY_NIR_CMP_NE;
  else if (strcmp(op, "<") == 0)
    *out = NY_NIR_CMP_LT;
  else if (strcmp(op, "<=") == 0)
    *out = NY_NIR_CMP_LE;
  else if (strcmp(op, ">") == 0)
    *out = NY_NIR_CMP_GT;
  else if (strcmp(op, ">=") == 0)
    *out = NY_NIR_CMP_GE;
  else
    return false;
  return true;
}

static const char *ny_native_leaf_name(const char *name) {
  if (!name)
    return NULL;
  const char *dot = strrchr(name, '.');
  return dot ? dot + 1 : name;
}

typedef struct {
  const char *name;
  int slot;
  bool is_f64;
  bool is_f32;
} ny_native_nir_local_t;

#define NY_EXTERN_MAX 256

typedef struct {
  const char *ny_name;
  const char *c_symbol;
  unsigned param_count;
  bool owned;
} ny_extern_entry_t;

typedef struct {
  ny_extern_entry_t entries[NY_EXTERN_MAX];
  size_t count;
} ny_extern_table_t;

static void ny_extern_table_init(ny_extern_table_t *t) {
  if (t)
    t->count = 0;
}

static bool ny_extern_table_add(ny_extern_table_t *t, const char *ny_name,
                                const char *c_symbol, unsigned param_count,
                                bool owned) {
  if (!t || !ny_name || !c_symbol)
    return false;
  /* Dedup: identical redeclarations are silently accepted. */
  for (size_t i = 0; i < t->count; ++i) {
    if (t->entries[i].ny_name && strcmp(t->entries[i].ny_name, ny_name) == 0) {
      if (t->entries[i].c_symbol &&
          strcmp(t->entries[i].c_symbol, c_symbol) == 0)
        return true; /* exact duplicate — ok */
      return false; /* conflicting extern: same NY name, different C symbol */
    }
  }
  if (t->count >= NY_EXTERN_MAX)
    return false;
  t->entries[t->count].ny_name = ny_name;
  t->entries[t->count].c_symbol = c_symbol;
  t->entries[t->count].param_count = param_count;
  t->entries[t->count].owned = owned;
  t->count++;
  return true;
}

static const ny_extern_entry_t *ny_extern_table_lookup(
    const ny_extern_table_t *t, const char *ny_name) {
  if (!t || !ny_name)
    return NULL;
  for (size_t i = 0; i < t->count; ++i) {
    if (t->entries[i].ny_name &&
        strcmp(t->entries[i].ny_name, ny_name) == 0)
      return &t->entries[i];
  }
  return NULL;
}

typedef struct {
  ny_nir_func_t nir;
  ny_native_nir_local_t locals[256];
  size_t local_count;
  int next_local_slot;
  int next_label;
  int last_value;
  int loop_head_labels[64];
  int loop_continue_labels[64];
  int loop_end_labels[64];
  size_t loop_depth;
  bool emitted_return;
  const ny_extern_table_t *externs;
  const program_t *prog;
  char *err;
  size_t err_len;
} ny_native_nir_builder_t;

static int ny_native_nir_temp_slot(ny_native_nir_builder_t *b) {
  return b ? b->next_local_slot++ : -1;
}

static size_t ny_native_nir_scope_mark(ny_native_nir_builder_t *b) {
  return b ? b->local_count : 0;
}

static void ny_native_nir_scope_restore(ny_native_nir_builder_t *b,
                                        size_t mark) {
  if (b && mark <= b->local_count)
    b->local_count = mark;
}

static bool ny_native_nir_fail(ny_native_nir_builder_t *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static bool ny_native_nir_fail(ny_native_nir_builder_t *b, const char *fmt, ...) {
  if (!b || !b->err || b->err_len == 0)
    return false;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b->err, b->err_len, fmt, ap);
  va_end(ap);
  return false;
}

static bool ny_native_nir_ignored_stmt(const stmt_t *s) {
  return !s || s->kind == NY_S_USE || s->kind == NY_S_LINK ||
         s->kind == NY_S_INCLUDE || s->kind == NY_S_DEFINE ||
         s->kind == NY_S_EXPORT || s->kind == NY_S_MODULE ||
         s->kind == NY_S_EXTERN;
}

static ny_native_nir_local_t *ny_native_nir_find_local(ny_native_nir_builder_t *b,
                                                       const char *name) {
  if (!b || !name)
    return NULL;
  for (size_t i = b->local_count; i > 0; --i) {
    ny_native_nir_local_t *l = &b->locals[i - 1];
    if (l->name && strcmp(l->name, name) == 0)
      return l;
  }
  return NULL;
}

static bool ny_native_type_name_is_f64(const char *name) {
  return name && (strcmp(name, "f64") == 0 || strcmp(name, "float") == 0);
}

static bool ny_native_type_name_is_f32(const char *name) {
  return name && (strcmp(name, "f32") == 0 || strcmp(name, "float32") == 0);
}

static bool ny_native_type_name_is_float(const char *name) {
  return ny_native_type_name_is_f64(name) || ny_native_type_name_is_f32(name);
}

static int64_t ny_native_f64_bits(double v) {
  int64_t bits = 0;
  memcpy(&bits, &v, sizeof(bits));
  return bits;
}

static int64_t ny_native_f32_bits(float v) {
  uint32_t bits = 0;
  memcpy(&bits, &v, sizeof(bits));
  return (int64_t)bits;
}

static ny_native_nir_local_t *ny_native_nir_bind_local_typed(
    ny_native_nir_builder_t *b, const char *name, bool is_f64, bool is_f32) {
  if (!name || !name[0] || strcmp(name, "_") == 0)
    return NULL;
  if (b->local_count >= sizeof(b->locals) / sizeof(b->locals[0])) {
    ny_native_nir_fail(b, "native NYIR lower: local limit exceeded");
    return NULL;
  }
  ny_native_nir_local_t *l = &b->locals[b->local_count];
  l->name = name;
  l->slot = b->next_local_slot++;
  l->is_f64 = is_f64;
  l->is_f32 = is_f32;
  b->local_count++;
  return l;
}

static ny_native_nir_local_t *ny_native_nir_bind_local(ny_native_nir_builder_t *b,
                                                       const char *name) {
  return ny_native_nir_bind_local_typed(b, name, false, false);
}

static ny_native_nir_local_t *ny_native_nir_add_local(ny_native_nir_builder_t *b,
                                                      const char *name) {
  if (!name || !name[0] || strcmp(name, "_") == 0)
    return NULL;
  ny_native_nir_local_t *old = ny_native_nir_find_local(b, name);
  return old ? old : ny_native_nir_bind_local(b, name);
}

static bool ny_native_nir_emit_label(ny_native_nir_builder_t *b, int label) {
  size_t before = b->nir.len;
  ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_LABEL,
                                       .dst = -1,
                                       .a = -1,
                                       .b = -1,
                                       .imm = label});
  return b->nir.len != before ||
         ny_native_nir_fail(b, "native NYIR lower: allocation failed");
}

static bool ny_native_nir_emit_br(ny_native_nir_builder_t *b, int label) {
  size_t before = b->nir.len;
  ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_BR,
                                       .dst = -1,
                                       .a = -1,
                                       .b = -1,
                                       .imm = label});
  return b->nir.len != before ||
         ny_native_nir_fail(b, "native NYIR lower: allocation failed");
}

static bool ny_native_nir_emit_br_if(ny_native_nir_builder_t *b, int value,
                                     int label) {
  size_t before = b->nir.len;
  ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_BR_IF,
                                       .dst = -1,
                                       .a = value,
                                       .b = -1,
                                       .imm = label});
  return b->nir.len != before ||
         ny_native_nir_fail(b, "native NYIR lower: allocation failed");
}

static bool ny_native_nir_emit_ret(ny_native_nir_builder_t *b, int value) {
  size_t before = b->nir.len;
  ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_RET,
                                       .dst = -1,
                                       .a = value,
                                       .b = -1});
  if (b->nir.len == before)
    return ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  b->emitted_return = true;
  b->last_value = value;
  return true;
}

static int ny_native_nir_emit_const(ny_native_nir_builder_t *b, int64_t value) {
  int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_CONST_I64,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .imm = value});
  if (v < 0)
    ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  return v;
}

static int ny_native_nir_emit_const_f64(ny_native_nir_builder_t *b, double value) {
  int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NYIR_CONST_F64,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .imm = ny_native_f64_bits(value)});
  if (v < 0)
    ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  return v;
}

static int ny_native_nir_emit_const_f32(ny_native_nir_builder_t *b, double value) {
  int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NYIR_CONST_F32,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .imm = ny_native_f32_bits((float)value)});
  if (v < 0)
    ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  return v;
}

static int ny_native_nir_emit_i64_to_f64(ny_native_nir_builder_t *b, int value) {
  int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NYIR_I64_TO_F64,
                                               .dst = -1,
                                               .a = value,
                                               .b = -1});
  if (v < 0)
    ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  return v;
}

static int ny_native_nir_emit_i64_to_f32(ny_native_nir_builder_t *b, int value) {
  int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NYIR_I64_TO_F32,
                                               .dst = -1,
                                               .a = value,
                                               .b = -1});
  if (v < 0)
    ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  return v;
}

static int ny_native_nir_emit_f32_to_f64(ny_native_nir_builder_t *b, int value) {
  int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NYIR_F32_TO_F64,
                                               .dst = -1,
                                               .a = value,
                                               .b = -1});
  if (v < 0)
    ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  return v;
}

static int ny_native_nir_emit_f64_to_f32(ny_native_nir_builder_t *b, int value) {
  int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NYIR_F64_TO_F32,
                                               .dst = -1,
                                               .a = value,
                                               .b = -1});
  if (v < 0)
    ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  return v;
}

static int ny_native_nir_emit_add_i64(ny_native_nir_builder_t *b, int a,
                                      int rhs) {
  int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_ADD_I64,
                                               .dst = -1,
                                               .a = a,
                                               .b = rhs});
  if (v < 0)
    ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  return v;
}

static int ny_native_nir_emit_load_i64(ny_native_nir_builder_t *b, int addr) {
  int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NYIR_LOAD_I64,
                                               .dst = -1,
                                               .a = addr,
                                               .b = -1});
  if (v < 0)
    ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  return v;
}

static int ny_native_nir_emit_addr_local(ny_native_nir_builder_t *b, int slot,
                                         const char *symbol) {
  int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NYIR_ADDR_LOCAL,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .imm = slot,
                                               .symbol = symbol});
  if (v < 0)
    ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  return v;
}

static bool ny_native_nir_emit_store_i64(ny_native_nir_builder_t *b, int addr,
                                         int value) {
  size_t before = b->nir.len;
  ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NYIR_STORE_I64,
                                       .dst = -1,
                                       .a = addr,
                                       .b = -1,
                                       .c = value});
  return b->nir.len != before ||
         ny_native_nir_fail(b, "native NYIR lower: allocation failed");
}

static bool ny_native_nir_expr_is_f64(ny_native_nir_builder_t *b, const expr_t *e) {
  if (!e)
    return false;
  switch (e->kind) {
  case NY_E_LITERAL:
    return e->as.literal.kind == NY_LIT_FLOAT;
  case NY_E_IDENT: {
    ny_native_nir_local_t *l = ny_native_nir_find_local(b, e->as.ident.name);
    return l && l->is_f64;
  }
  case NY_E_BINARY:
    return ny_native_nir_expr_is_f64(b, e->as.binary.left) ||
           ny_native_nir_expr_is_f64(b, e->as.binary.right);
  case NY_E_UNARY:
    return ny_native_nir_expr_is_f64(b, e->as.unary.right);
  case NY_E_CALL:
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT && b && b->prog) {
      const char *name = e->as.call.callee->as.ident.name;
      for (size_t i = 0; i < b->prog->body.len; ++i) {
        const stmt_t *s = b->prog->body.data[i];
        if (s && s->kind == NY_S_FUNC && s->as.fn.name &&
            strcmp(s->as.fn.name, name) == 0)
          return ny_native_type_name_is_f64(s->as.fn.return_type);
      }
    }
    return false;
  default:
    return false;
  }
}

static bool ny_native_nir_expr_is_f32(ny_native_nir_builder_t *b, const expr_t *e) {
  if (!e)
    return false;
  switch (e->kind) {
  case NY_E_IDENT: {
    ny_native_nir_local_t *l = ny_native_nir_find_local(b, e->as.ident.name);
    return l && l->is_f32;
  }
  case NY_E_BINARY:
    return ny_native_nir_expr_is_f32(b, e->as.binary.left) ||
           ny_native_nir_expr_is_f32(b, e->as.binary.right);
  case NY_E_UNARY:
    return ny_native_nir_expr_is_f32(b, e->as.unary.right);
  case NY_E_CALL:
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT && b && b->prog) {
      const char *name = e->as.call.callee->as.ident.name;
      for (size_t i = 0; i < b->prog->body.len; ++i) {
        const stmt_t *s = b->prog->body.data[i];
        if (s && s->kind == NY_S_FUNC && s->as.fn.name &&
            strcmp(s->as.fn.name, name) == 0)
          return ny_native_type_name_is_f32(s->as.fn.return_type);
      }
    }
    return false;
  default:
    return false;
  }
}

static const stmt_t *ny_native_nir_find_user_function(ny_native_nir_builder_t *b,
                                                     const char *name) {
  if (!b || !b->prog || !name)
    return NULL;
  for (size_t i = 0; i < b->prog->body.len; ++i) {
    const stmt_t *s = b->prog->body.data[i];
    if (s && s->kind == NY_S_FUNC && s->as.fn.name &&
        strcmp(s->as.fn.name, name) == 0)
      return s;
  }
  return NULL;
}

static bool ny_native_nir_store_local_value(ny_native_nir_builder_t *b,
                                            int slot, int value) {
  size_t before = b->nir.len;
  ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_STORE_LOCAL,
                                       .dst = -1,
                                       .a = value,
                                       .b = -1,
                                       .imm = slot});
  return b->nir.len != before ||
         ny_native_nir_fail(b, "native NYIR lower: allocation failed");
}

static int ny_native_nir_load_local_value(ny_native_nir_builder_t *b,
                                          int slot) {
  int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_LOAD_LOCAL,
                                               .dst = -1,
                                               .a = -1,
                                               .b = -1,
                                               .imm = slot});
  if (v < 0)
    ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  return v;
}

static int ny_native_nir_emit_is_zero(ny_native_nir_builder_t *b, int value) {
  int zero = ny_native_nir_emit_const(b, 0);
  if (zero < 0)
    return -1;
  int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_CMP_I64,
                                               .dst = -1,
                                               .a = value,
                                               .b = zero,
                                               .cmp = NY_NIR_CMP_EQ});
  if (v < 0)
    ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  return v;
}

static int ny_native_nir_lower_logical(ny_native_nir_builder_t *b,
                                       const expr_t *left,
                                       const expr_t *right,
                                       bool is_or);
static int ny_native_nir_lower_ternary(ny_native_nir_builder_t *b,
                                       const expr_t *cond,
                                       const expr_t *true_expr,
                                       const expr_t *false_expr);
static bool ny_native_nir_lower_match(ny_native_nir_builder_t *b,
                                      const stmt_t *s);

static int ny_native_nir_lower_expr(ny_native_nir_builder_t *b, const expr_t *e) {
  if (!e) {
    ny_native_nir_fail(b, "native NYIR lower: missing expression");
    return -1;
  }
  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->as.literal.kind == NY_LIT_BOOL)
      return ny_native_nir_emit_const(b, e->as.literal.as.b ? 1 : 0);
    if (e->as.literal.kind == NY_LIT_FLOAT)
      return ny_native_nir_emit_const_f64(b, e->as.literal.as.f);
    if (e->tok.kind == NY_T_NIL)
      return ny_native_nir_emit_const(b, 0);
    if (e->as.literal.kind != NY_LIT_INT) {
      ny_native_nir_fail(b, "native NYIR lower: only int/bool/f64/nil literals are supported");
      return -1;
    }
    return ny_native_nir_emit_const(b, e->as.literal.as.i);
  case NY_E_IDENT: {
    ny_native_nir_local_t *l = ny_native_nir_find_local(b, e->as.ident.name);
    if (!l) {
      ny_native_nir_fail(b, "native NYIR lower: unknown local '%s'",
                         e->as.ident.name ? e->as.ident.name : "(null)");
      return -1;
    }
    int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_LOAD_LOCAL,
                                                 .dst = -1,
                                                 .a = -1,
                                                 .b = -1,
                                                 .imm = l->slot,
                                                 .symbol = l->name});
    if (v < 0)
      ny_native_nir_fail(b, "native NYIR lower: allocation failed");
    return v;
  }
  case NY_E_UNARY: {
    if (!e->as.unary.op || !e->as.unary.right) {
      ny_native_nir_fail(b, "native NYIR lower: malformed unary");
      return -1;
    }
    int rv = ny_native_nir_lower_expr(b, e->as.unary.right);
    if (rv < 0)
      return -1;
    if (strcmp(e->as.unary.op, "+") == 0)
      return rv;
    if (strcmp(e->as.unary.op, "-") == 0) {
      int zero = ny_native_nir_emit_const(b, 0);
      if (zero < 0)
        return -1;
      return ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_SUB_I64,
                                                  .dst = -1,
                                                  .a = zero,
                                                  .b = rv});
    }
    if (strcmp(e->as.unary.op, "!") == 0) {
      int zero = ny_native_nir_emit_const(b, 0);
      if (zero < 0)
        return -1;
      return ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_CMP_I64,
                                                  .dst = -1,
                                                  .a = rv,
                                                  .b = zero,
                                                  .cmp = NY_NIR_CMP_EQ});
    }
    if (strcmp(e->as.unary.op, "~") == 0) {
      int mask = ny_native_nir_emit_const(b, -1);
      if (mask < 0)
        return -1;
      return ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_XOR_I64,
                                                  .dst = -1,
                                                  .a = rv,
                                                  .b = mask});
    }
    ny_native_nir_fail(b, "native NYIR lower: unsupported unary operator '%s'",
                       e->as.unary.op);
    return -1;
  }
  case NY_E_BINARY: {
    ny_nir_op_t op = NY_NIR_NOP;
    ny_nir_cmp_t cmp = NY_NIR_CMP_EQ;
    bool is_cmp = ny_native_nir_cmp(e->as.binary.op, &cmp);
    if (!is_cmp && !ny_native_nir_binop(e->as.binary.op, &op)) {
      ny_native_nir_fail(b, "native NYIR lower: unsupported binary operator '%s'",
                         e->as.binary.op ? e->as.binary.op : "(null)");
      return -1;
    }
    bool expr_f32 = !ny_native_nir_expr_is_f64(b, e) &&
                    ny_native_nir_expr_is_f32(b, e);
    if (!is_cmp && expr_f32 &&
        (strcmp(e->as.binary.op, "+") == 0 || strcmp(e->as.binary.op, "-") == 0 ||
         strcmp(e->as.binary.op, "*") == 0 || strcmp(e->as.binary.op, "/") == 0)) {
      if (strcmp(e->as.binary.op, "+") == 0)
        op = NYIR_ADD_F32;
      else if (strcmp(e->as.binary.op, "-") == 0)
        op = NYIR_SUB_F32;
      else if (strcmp(e->as.binary.op, "*") == 0)
        op = NYIR_MUL_F32;
      else
        op = NYIR_DIV_F32;
    } else if (!is_cmp && ny_native_nir_expr_is_f64(b, e) &&
        (strcmp(e->as.binary.op, "+") == 0 || strcmp(e->as.binary.op, "-") == 0 ||
         strcmp(e->as.binary.op, "*") == 0 || strcmp(e->as.binary.op, "/") == 0)) {
      if (strcmp(e->as.binary.op, "+") == 0)
        op = NYIR_ADD_F64;
      else if (strcmp(e->as.binary.op, "-") == 0)
        op = NYIR_SUB_F64;
      else if (strcmp(e->as.binary.op, "*") == 0)
        op = NYIR_MUL_F64;
      else
        op = NYIR_DIV_F64;
    }
    bool left_f64 = ny_native_nir_expr_is_f64(b, e->as.binary.left);
    bool right_f64 = ny_native_nir_expr_is_f64(b, e->as.binary.right);
    bool left_f32 = ny_native_nir_expr_is_f32(b, e->as.binary.left);
    bool right_f32 = ny_native_nir_expr_is_f32(b, e->as.binary.right);
    int a = ny_native_nir_lower_expr(b, e->as.binary.left);
    int rhs = ny_native_nir_lower_expr(b, e->as.binary.right);
    if (a < 0 || rhs < 0)
      return -1;
    bool use_f64_cmp = is_cmp && ny_native_nir_expr_is_f64(b, e);
    bool use_f32_cmp = is_cmp && !use_f64_cmp && expr_f32;
    if ((!is_cmp && (op == NYIR_ADD_F32 || op == NYIR_SUB_F32 ||
                     op == NYIR_MUL_F32 || op == NYIR_DIV_F32)) ||
        use_f32_cmp) {
      if (!left_f32) {
        a = ny_native_nir_emit_i64_to_f32(b, a);
        if (a < 0)
          return -1;
      }
      if (!right_f32) {
        rhs = ny_native_nir_emit_i64_to_f32(b, rhs);
        if (rhs < 0)
          return -1;
      }
    } else if ((!is_cmp && (op == NYIR_ADD_F64 || op == NYIR_SUB_F64 ||
                     op == NYIR_MUL_F64 || op == NYIR_DIV_F64)) ||
        use_f64_cmp) {
      if (!left_f64) {
        a = ny_native_nir_emit_i64_to_f64(b, a);
        if (a < 0)
          return -1;
      }
      if (!right_f64) {
        rhs = ny_native_nir_emit_i64_to_f64(b, rhs);
        if (rhs < 0)
          return -1;
      }
    }
    int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = use_f64_cmp ? NYIR_CMP_F64
                                                       : use_f32_cmp ? NYIR_CMP_F32
                                                       : is_cmp    ? NY_NIR_CMP_I64
                                                                   : op,
                                                 .dst = -1,
                                                 .a = a,
                                                 .b = rhs,
                                                 .cmp = cmp});
    if (v < 0)
      ny_native_nir_fail(b, "native NYIR lower: allocation failed");
    return v;
  }
  case NY_E_LOGICAL: {
    if (!e->as.logical.op ||
        (strcmp(e->as.logical.op, "&&") != 0 &&
         strcmp(e->as.logical.op, "||") != 0)) {
      ny_native_nir_fail(b, "native NYIR lower: unsupported logical operator '%s'",
                         e->as.logical.op ? e->as.logical.op : "(null)");
      return -1;
    }
    return ny_native_nir_lower_logical(
        b, e->as.logical.left, e->as.logical.right,
        strcmp(e->as.logical.op, "||") == 0);
  }
  case NY_E_TERNARY:
    return ny_native_nir_lower_ternary(b, e->as.ternary.cond,
                                       e->as.ternary.true_expr,
                                       e->as.ternary.false_expr);
  case NY_E_CALL: {
    if (!e->as.call.callee || e->as.call.callee->kind != NY_E_IDENT) {
      ny_native_nir_fail(b, "native NYIR lower: only direct calls are supported");
      return -1;
    }
    const char *name = e->as.call.callee->as.ident.name;
    const char *leaf = ny_native_leaf_name(name);
    if (leaf && (strcmp(leaf, "addr_of") == 0 || strcmp(leaf, "borrow") == 0)) {
      if (e->as.call.args.len != 1 || e->as.call.args.data[0].name ||
          !e->as.call.args.data[0].val ||
          e->as.call.args.data[0].val->kind != NY_E_IDENT) {
        ny_native_nir_fail(b, "native NYIR lower: %s requires one local identifier",
                           leaf);
        return -1;
      }
      const char *local_name = e->as.call.args.data[0].val->as.ident.name;
      ny_native_nir_local_t *l = ny_native_nir_find_local(b, local_name);
      if (!l) {
        ny_native_nir_fail(b, "native NYIR lower: %s target '%s' is not a local",
                           leaf,
                           local_name ? local_name : "<null>");
        return -1;
      }
      return ny_native_nir_emit_addr_local(b, l->slot, local_name);
    }
    if (leaf && (strcmp(leaf, "load64_i") == 0 ||
                 strcmp(leaf, "load64_h") == 0 ||
                 strcmp(leaf, "__load64_h") == 0 ||
                 strcmp(leaf, "__load64_idx") == 0)) {
      if (e->as.call.args.len < 1 || e->as.call.args.len > 2 ||
          e->as.call.args.data[0].name ||
          (e->as.call.args.len > 1 && e->as.call.args.data[1].name)) {
        ny_native_nir_fail(b, "native NYIR lower: load64_i/load64_h require positional pointer and optional offset");
        return -1;
      }
      int addr = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
      if (addr < 0)
        return -1;
      if (e->as.call.args.len > 1) {
        int off = ny_native_nir_lower_expr(b, e->as.call.args.data[1].val);
        if (off < 0)
          return -1;
        addr = ny_native_nir_emit_add_i64(b, addr, off);
        if (addr < 0)
          return -1;
      }
      return ny_native_nir_emit_load_i64(b, addr);
    }
    if (leaf && (strcmp(leaf, "store64_i") == 0 ||
                 strcmp(leaf, "store64_h") == 0 ||
                 strcmp(leaf, "__store64_h") == 0 ||
                 strcmp(leaf, "__store64_idx") == 0)) {
      bool intrinsic_order = strcmp(leaf, "__store64_h") == 0 ||
                             strcmp(leaf, "__store64_idx") == 0;
      if (e->as.call.args.len < 2 || e->as.call.args.len > 3 ||
          (intrinsic_order && e->as.call.args.len != 3) ||
          e->as.call.args.data[0].name || e->as.call.args.data[1].name ||
          (e->as.call.args.len > 2 && e->as.call.args.data[2].name)) {
        ny_native_nir_fail(b, "native NYIR lower: store64_i/store64_h require positional pointer, value, and optional offset");
        return -1;
      }
      size_t val_idx = intrinsic_order ? 2u : 1u;
      size_t off_idx = intrinsic_order ? 1u : 2u;
      int addr = ny_native_nir_lower_expr(b, e->as.call.args.data[0].val);
      int value = ny_native_nir_lower_expr(b, e->as.call.args.data[val_idx].val);
      if (addr < 0 || value < 0)
        return -1;
      if (e->as.call.args.len > off_idx) {
        int off = ny_native_nir_lower_expr(b, e->as.call.args.data[off_idx].val);
        if (off < 0)
          return -1;
        addr = ny_native_nir_emit_add_i64(b, addr, off);
        if (addr < 0)
          return -1;
      }
      if (!ny_native_nir_emit_store_i64(b, addr, value))
        return -1;
      return ny_native_nir_emit_const(b, 0);
    }
    if (e->as.call.args.len > NY_NIR_CALL_MAX_ARGS) {
      ny_native_nir_fail(b,
                         "native NYIR lower: call exceeds the maximum supported argument count (%d)",
                         NY_NIR_CALL_MAX_ARGS);
      return -1;
    }
    const stmt_t *callee_fn = ny_native_nir_find_user_function(b, name);
    int args[NY_NIR_CALL_MAX_ARGS];
    for (size_t i = 0; i < e->as.call.args.len; ++i) {
      if (e->as.call.args.data[i].name) {
        ny_native_nir_fail(b, "native NYIR lower: named call args are not supported");
        return -1;
      }
      bool arg_expr_f64 = ny_native_nir_expr_is_f64(b, e->as.call.args.data[i].val);
      bool arg_expr_f32 = ny_native_nir_expr_is_f32(b, e->as.call.args.data[i].val);
      args[i] = ny_native_nir_lower_expr(b, e->as.call.args.data[i].val);
      if (args[i] < 0)
        return -1;
      if (callee_fn && i < callee_fn->as.fn.params.len) {
        const char *param_type = callee_fn->as.fn.params.data[i].type;
        if (ny_native_type_name_is_f32(param_type) && !arg_expr_f32) {
          args[i] = arg_expr_f64 ? ny_native_nir_emit_f64_to_f32(b, args[i])
                                 : ny_native_nir_emit_i64_to_f32(b, args[i]);
          if (args[i] < 0)
            return -1;
        } else if (ny_native_type_name_is_f64(param_type) && !arg_expr_f64) {
          args[i] = arg_expr_f32 ? ny_native_nir_emit_f32_to_f64(b, args[i])
                                 : ny_native_nir_emit_i64_to_f64(b, args[i]);
          if (args[i] < 0)
            return -1;
        }
      }
    }
    const ny_extern_entry_t *ext =
        b->externs ? ny_extern_table_lookup(b->externs, name) : NULL;
    bool builtin_c_call = leaf && (strcmp(leaf, "malloc") == 0 ||
                                   strcmp(leaf, "__malloc") == 0 ||
                                   strcmp(leaf, "realloc") == 0 ||
                                   strcmp(leaf, "__realloc") == 0 ||
                                   strcmp(leaf, "free") == 0 ||
                                   strcmp(leaf, "__free") == 0);
    const char *symbol = ext ? ext->c_symbol :
                         builtin_c_call && strstr(leaf, "realloc") ? "realloc" :
                         builtin_c_call && strstr(leaf, "malloc") ? "malloc" :
                         builtin_c_call ? "free" : name;
    unsigned flags = (ext || builtin_c_call) ? NY_NIR_INST_F_EXTERN : 0;
    if (callee_fn && ny_native_type_name_is_f64(callee_fn->as.fn.return_type)) {
      flags |= NY_NIR_INST_F_RET_F64;
    } else if (callee_fn && ny_native_type_name_is_f32(callee_fn->as.fn.return_type)) {
      flags |= NYIR_INST_F_RET_F32;
    } else if (ny_native_nir_expr_is_f64(b, e)) {
      flags |= NY_NIR_INST_F_RET_F64;
    }
    size_t argc = e->as.call.args.len;
    int *extra = NULL;
    if (argc > 6) {
      size_t extra_len = argc - 6;
      extra = (int *)malloc(extra_len * sizeof(*extra));
      if (!extra) {
        ny_native_nir_fail(b, "native NYIR lower: allocation failed");
        return -1;
      }
      memcpy(extra, &args[6], extra_len * sizeof(*extra));
    }
    int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_CALL,
                                                 .dst = -1,
                                                 .a = argc > 0 ? args[0] : -1,
                                                 .b = argc > 1 ? args[1] : -1,
                                                 .c = argc > 2 ? args[2] : -1,
                                                 .d = argc > 3 ? args[3] : -1,
                                                 .e = argc > 4 ? args[4] : -1,
                                                 .f = argc > 5 ? args[5] : -1,
                                                 .imm = (int64_t)argc,
                                                 .flags = flags,
                                                 .symbol = symbol,
                                                 .extra_args = extra,
                                                 .extra_args_len = argc > 6 ? argc - 6 : 0});
    if (v < 0) {
      free(extra);
      ny_native_nir_fail(b, "native NYIR lower: allocation failed");
    }
    return v;
  }
  case NY_E_DEREF: {
    int addr = ny_native_nir_lower_expr(b, e->as.deref.target);
    if (addr < 0)
      return -1;
    int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NYIR_LOAD_I64,
                                                 .dst = -1,
                                                 .a = addr,
                                                 .b = -1,
                                                 .c = -1});
    if (v < 0)
      ny_native_nir_fail(b, "native NYIR lower: deref load failed");
    return v;
  }
  default:
    ny_native_nir_fail(b, "native NYIR lower: expression kind %d is not in shared NYIR yet",
                       (int)e->kind);
    return -1;
  }
}

static int ny_native_nir_lower_logical(ny_native_nir_builder_t *b,
                                       const expr_t *left,
                                       const expr_t *right,
                                       bool is_or) {
  int result_slot = ny_native_nir_temp_slot(b);
  int zero = ny_native_nir_emit_const(b, 0);
  int one = ny_native_nir_emit_const(b, 1);
  if (zero < 0 || one < 0)
    return -1;
  int true_label = b->next_label++;
  int end_label = b->next_label++;

  if (!ny_native_nir_store_local_value(b, result_slot, zero))
    return -1;

  int lhs = ny_native_nir_lower_expr(b, left);
  if (lhs < 0)
    return -1;

  if (is_or) {
    if (!ny_native_nir_emit_br_if(b, lhs, true_label))
      return -1;
  } else {
    int lhs_zero = ny_native_nir_emit_is_zero(b, lhs);
    if (lhs_zero < 0 || !ny_native_nir_emit_br_if(b, lhs_zero, end_label))
      return -1;
  }

  int rhs = ny_native_nir_lower_expr(b, right);
  if (rhs < 0)
    return -1;

  if (is_or) {
    if (!ny_native_nir_emit_br_if(b, rhs, true_label) ||
        !ny_native_nir_emit_br(b, end_label))
      return -1;
  } else {
    int rhs_zero = ny_native_nir_emit_is_zero(b, rhs);
    if (rhs_zero < 0 || !ny_native_nir_emit_br_if(b, rhs_zero, end_label))
      return -1;
  }

  if (!ny_native_nir_emit_label(b, true_label) ||
      !ny_native_nir_store_local_value(b, result_slot, one) ||
      !ny_native_nir_emit_label(b, end_label))
    return -1;

  return ny_native_nir_load_local_value(b, result_slot);
}

static int ny_native_nir_lower_ternary(ny_native_nir_builder_t *b,
                                       const expr_t *cond,
                                       const expr_t *true_expr,
                                       const expr_t *false_expr) {
  if (!cond || !true_expr || !false_expr) {
    ny_native_nir_fail(b, "native NYIR lower: malformed ternary expression");
    return -1;
  }
  int result_slot = ny_native_nir_temp_slot(b);
  int else_label = b->next_label++;
  int end_label = b->next_label++;

  int cond_val = ny_native_nir_lower_expr(b, cond);
  if (cond_val < 0)
    return -1;
  int cond_zero = ny_native_nir_emit_is_zero(b, cond_val);
  if (cond_zero < 0 || !ny_native_nir_emit_br_if(b, cond_zero, else_label))
    return -1;

  int true_val = ny_native_nir_lower_expr(b, true_expr);
  if (true_val < 0 ||
      !ny_native_nir_store_local_value(b, result_slot, true_val) ||
      !ny_native_nir_emit_br(b, end_label))
    return -1;

  if (!ny_native_nir_emit_label(b, else_label))
    return -1;
  int false_val = ny_native_nir_lower_expr(b, false_expr);
  if (false_val < 0 ||
      !ny_native_nir_store_local_value(b, result_slot, false_val) ||
      !ny_native_nir_emit_label(b, end_label))
    return -1;

  return ny_native_nir_load_local_value(b, result_slot);
}

static bool ny_native_nir_lower_stmt(ny_native_nir_builder_t *b, const stmt_t *s);

static bool ny_native_nir_lower_var(ny_native_nir_builder_t *b, const stmt_t *s) {
  const stmt_var_t *v = &s->as.var;
  if (v->is_del || v->is_destructure)
    return ny_native_nir_fail(b,
                              "native NYIR lower: only simple def/mut bindings are supported");
  for (size_t i = 0; i < v->names.len; ++i) {
    const char *name = v->names.data[i];
    if (!name || strcmp(name, "_") == 0)
      continue;
    if (i >= v->exprs.len || !v->exprs.data[i])
      return ny_native_nir_fail(b,
                                "native NYIR lower: local '%s' needs an initializer",
                                name);
    bool is_f64 = i < v->types.len && ny_native_type_name_is_f64(v->types.data[i]);
    bool is_f32 = i < v->types.len && ny_native_type_name_is_f32(v->types.data[i]);
    if (!is_f64 && !is_f32 && i < v->exprs.len)
      is_f64 = ny_native_nir_expr_is_f64(b, v->exprs.data[i]);
    if (!is_f64 && !is_f32 && i < v->exprs.len)
      is_f32 = ny_native_nir_expr_is_f32(b, v->exprs.data[i]);
    ny_native_nir_local_t *l =
        v->is_decl ? ny_native_nir_bind_local_typed(b, name, is_f64, is_f32)
                   : ny_native_nir_add_local(b, name);
    if (l && is_f64)
      l->is_f64 = true;
    if (l && is_f32)
      l->is_f32 = true;
    if (!l)
      return false;
    expr_t *init = v->exprs.data[i];
    int val = -1;
    if (is_f32 && init && init->kind == NY_E_LITERAL &&
        init->as.literal.kind == NY_LIT_FLOAT)
      val = ny_native_nir_emit_const_f32(b, init->as.literal.as.f);
    else
      val = ny_native_nir_lower_expr(b, init);
    if (val < 0)
      return false;
    if (is_f64 && init && ny_native_nir_expr_is_f32(b, init)) {
      val = ny_native_nir_emit_f32_to_f64(b, val);
      if (val < 0)
        return false;
    }
    size_t before = b->nir.len;
    ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_STORE_LOCAL,
                                         .dst = -1,
                                         .a = val,
                                         .b = -1,
                                         .imm = l->slot,
                                         .symbol = l->name});
    if (b->nir.len == before)
      return ny_native_nir_fail(b, "native NYIR lower: allocation failed");
    b->last_value = val;
  }
  return true;
}

static bool ny_native_nir_lower_if(ny_native_nir_builder_t *b, const stmt_t *s) {
  if (s->as.iff.init && !ny_native_nir_lower_stmt(b, s->as.iff.init))
    return false;
  int cond = ny_native_nir_lower_expr(b, s->as.iff.test);
  if (cond < 0)
    return false;
  int zero = ny_native_nir_emit_const(b, 0);
  if (zero < 0)
    return false;
  int is_false = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_CMP_I64,
                                                       .dst = -1,
                                                       .a = cond,
                                                       .b = zero,
                                                       .cmp = NY_NIR_CMP_EQ});
  if (is_false < 0)
    return ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  int else_label = b->next_label++;
  int merge_label = b->next_label++;
  int end_label = b->next_label++;
  if (!ny_native_nir_emit_br_if(b, is_false, else_label))
    return false;

  int result_slot = ny_native_nir_temp_slot(b);
  bool has_alt = s->as.iff.alt != NULL;
  bool entry_return = b->emitted_return;
  int entry_last_value = b->last_value;

  /* If no else, pre-store entry_last_value as the false-branch result. */
  if (!has_alt && !entry_return && entry_last_value >= 0) {
    size_t before = b->nir.len;
    ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_STORE_LOCAL,
                                         .dst = -1,
                                         .a = entry_last_value,
                                         .b = -1,
                                         .imm = result_slot});
    if (b->nir.len == before)
      return ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  }

  /* Then branch. */
  b->emitted_return = false;
  if (!ny_native_nir_lower_stmt(b, s->as.iff.conseq))
    return false;
  bool conseq_returns = b->emitted_return;
  if (!conseq_returns) {
    int conseq_val = b->last_value;
    if (conseq_val >= 0) {
      size_t before = b->nir.len;
      ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_STORE_LOCAL,
                                           .dst = -1,
                                           .a = conseq_val,
                                           .b = -1,
                                           .imm = result_slot});
      if (b->nir.len == before)
        return ny_native_nir_fail(b, "native NYIR lower: allocation failed");
    }
    /* Jump to the merge point (before end_label) so both branches converge
       before the shared load.local. */
    if (!ny_native_nir_emit_br(b, merge_label))
      return false;
  }
  if (!ny_native_nir_emit_label(b, else_label))
    return false;

  /* Else branch. */
  b->emitted_return = false;
  b->last_value = entry_last_value;
  if (has_alt && !ny_native_nir_lower_stmt(b, s->as.iff.alt))
    return false;
  bool alt_returns = b->emitted_return;
  if (!alt_returns && has_alt) {
    int alt_val = b->last_value;
    if (alt_val >= 0) {
      size_t before = b->nir.len;
      ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_STORE_LOCAL,
                                           .dst = -1,
                                           .a = alt_val,
                                           .b = -1,
                                           .imm = result_slot});
      if (b->nir.len == before)
        return ny_native_nir_fail(b, "native NYIR lower: allocation failed");
    }
  }

  /* Merge point: both branches converge here. */
  b->emitted_return = entry_return || (has_alt && conseq_returns && alt_returns);
  if (!b->emitted_return) {
    if (!ny_native_nir_emit_label(b, merge_label))
      return false;
    int loaded = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_LOAD_LOCAL,
                                                       .dst = -1,
                                                       .a = -1,
                                                       .b = -1,
                                                       .imm = result_slot});
    if (loaded < 0)
      return ny_native_nir_fail(b, "native NYIR lower: allocation failed");
    b->last_value = loaded;
  }
  return ny_native_nir_emit_label(b, end_label);
}

static bool ny_native_nir_lower_while(ny_native_nir_builder_t *b, const stmt_t *s) {
  if (s->as.whl.init && !ny_native_nir_lower_stmt(b, s->as.whl.init))
    return false;
  int head_label = b->next_label++;
  int update_label = s->as.whl.update ? b->next_label++ : head_label;
  int end_label = b->next_label++;
  if (!ny_native_nir_emit_label(b, head_label))
    return false;
  int cond = ny_native_nir_lower_expr(b, s->as.whl.test);
  if (cond < 0)
    return false;
  int zero = ny_native_nir_emit_const(b, 0);
  if (zero < 0)
    return false;
  int is_false = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_CMP_I64,
                                                      .dst = -1,
                                                      .a = cond,
                                                      .b = zero,
                                                      .cmp = NY_NIR_CMP_EQ});
  if (is_false < 0)
    return ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  if (!ny_native_nir_emit_br_if(b, is_false, end_label))
    return false;
  bool entry_return = b->emitted_return;
  int entry_last_value = b->last_value;
  if (b->loop_depth >= sizeof(b->loop_head_labels) / sizeof(b->loop_head_labels[0]))
    return ny_native_nir_fail(b, "native NYIR lower: loop nesting limit exceeded");
  size_t loop_i = b->loop_depth++;
  b->loop_head_labels[loop_i] = head_label;
  b->loop_continue_labels[loop_i] = update_label;
  b->loop_end_labels[loop_i] = end_label;
  b->emitted_return = false;
  bool body_ok = ny_native_nir_lower_stmt(b, s->as.whl.body);
  if (body_ok && s->as.whl.update) {
    b->emitted_return = false;
    body_ok = ny_native_nir_emit_label(b, update_label) &&
              ny_native_nir_lower_stmt(b, s->as.whl.update);
  }
  b->loop_depth = loop_i;
  if (!body_ok)
    return false;
  b->emitted_return = entry_return;
  b->last_value = entry_last_value;
  return ny_native_nir_emit_br(b, head_label) &&
         ny_native_nir_emit_label(b, end_label);
}


static bool ny_native_nir_lower_for_header(ny_native_nir_builder_t *b,
                                           const stmt_t *s) {
  if (s->as.fr.init && !ny_native_nir_lower_stmt(b, s->as.fr.init))
    return false;
  int head_label = b->next_label++;
  int update_label = s->as.fr.update ? b->next_label++ : head_label;
  int end_label = b->next_label++;
  if (!ny_native_nir_emit_label(b, head_label))
    return false;
  int cond = s->as.fr.cond ? ny_native_nir_lower_expr(b, s->as.fr.cond)
                           : ny_native_nir_emit_const(b, 1);
  if (cond < 0)
    return false;
  int is_false = ny_native_nir_emit_is_zero(b, cond);
  if (is_false < 0 || !ny_native_nir_emit_br_if(b, is_false, end_label))
    return false;
  if (b->loop_depth >= sizeof(b->loop_head_labels) / sizeof(b->loop_head_labels[0]))
    return ny_native_nir_fail(b, "native NYIR lower: loop nesting limit exceeded");
  size_t loop_i = b->loop_depth++;
  b->loop_head_labels[loop_i] = head_label;
  b->loop_continue_labels[loop_i] = update_label;
  b->loop_end_labels[loop_i] = end_label;
  bool entry_return = b->emitted_return;
  int entry_last_value = b->last_value;
  b->emitted_return = false;
  bool body_ok = ny_native_nir_lower_stmt(b, s->as.fr.body);
  if (body_ok && s->as.fr.update) {
    b->emitted_return = false;
    body_ok = ny_native_nir_emit_label(b, update_label) &&
              ny_native_nir_lower_stmt(b, s->as.fr.update);
  }
  b->loop_depth = loop_i;
  if (!body_ok)
    return false;
  b->emitted_return = entry_return;
  b->last_value = entry_last_value;
  return ny_native_nir_emit_br(b, head_label) &&
         ny_native_nir_emit_label(b, end_label);
}

static bool ny_native_nir_iterable_is_range(const expr_t *iterable,
                                            const expr_t **lo,
                                            const expr_t **hi) {
  if (!iterable || iterable->kind != NY_E_BINARY || !iterable->as.binary.op ||
      strcmp(iterable->as.binary.op, "..") != 0)
    return false;
  if (lo)
    *lo = iterable->as.binary.left;
  if (hi)
    *hi = iterable->as.binary.right;
  return true;
}

static bool ny_native_nir_lower_for_range(ny_native_nir_builder_t *b,
                                          const stmt_t *s) {
  const expr_t *lo_expr = NULL;
  const expr_t *hi_expr = NULL;
  if (!s->as.fr.iter_var || !ny_native_nir_iterable_is_range(s->as.fr.iterable,
                                                             &lo_expr, &hi_expr)) {
    return ny_native_nir_fail(
        b, "native NYIR lower: for loops currently support `for name in lo..hi` ranges");
  }

  size_t loop_scope_mark = ny_native_nir_scope_mark(b);
  ny_native_nir_local_t *iter = ny_native_nir_bind_local(b, s->as.fr.iter_var);
  if (!iter)
    return false;
  ny_native_nir_local_t *index = NULL;
  if (s->as.fr.iter_index_var) {
    index = ny_native_nir_bind_local(b, s->as.fr.iter_index_var);
    if (!index)
      return false;
  }

  int lo = ny_native_nir_lower_expr(b, lo_expr);
  int hi = ny_native_nir_lower_expr(b, hi_expr);
  int hi_slot = ny_native_nir_temp_slot(b);
  if (lo < 0 || hi < 0 || !ny_native_nir_store_local_value(b, iter->slot, lo) ||
      !ny_native_nir_store_local_value(b, hi_slot, hi))
    return false;
  if (index) {
    int zero = ny_native_nir_emit_const(b, 0);
    if (zero < 0 || !ny_native_nir_store_local_value(b, index->slot, zero))
      return false;
  }

  int head_label = b->next_label++;
  int update_label = b->next_label++;
  int end_label = b->next_label++;
  if (!ny_native_nir_emit_label(b, head_label))
    return false;

  int cur = ny_native_nir_load_local_value(b, iter->slot);
  int end = ny_native_nir_load_local_value(b, hi_slot);
  int in_range = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_CMP_I64,
                                                      .dst = -1,
                                                      .a = cur,
                                                      .b = end,
                                                      .cmp = NY_NIR_CMP_LE});
  int done = in_range >= 0 ? ny_native_nir_emit_is_zero(b, in_range) : -1;
  if (done < 0 || !ny_native_nir_emit_br_if(b, done, end_label))
    return false;

  if (b->loop_depth >= sizeof(b->loop_head_labels) / sizeof(b->loop_head_labels[0]))
    return ny_native_nir_fail(b, "native NYIR lower: loop nesting limit exceeded");
  size_t loop_i = b->loop_depth++;
  b->loop_head_labels[loop_i] = head_label;
  b->loop_continue_labels[loop_i] = update_label;
  b->loop_end_labels[loop_i] = end_label;
  bool entry_return = b->emitted_return;
  int entry_last_value = b->last_value;
  b->emitted_return = false;
  bool body_ok = ny_native_nir_lower_stmt(b, s->as.fr.body);
  b->loop_depth = loop_i;
  if (!body_ok)
    return false;

  b->emitted_return = false;
  if (!ny_native_nir_emit_label(b, update_label))
    return false;
  int one = ny_native_nir_emit_const(b, 1);
  cur = ny_native_nir_load_local_value(b, iter->slot);
  int next = one >= 0 && cur >= 0
                 ? ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_ADD_I64,
                                                         .dst = -1,
                                                         .a = cur,
                                                         .b = one})
                 : -1;
  if (next < 0 || !ny_native_nir_store_local_value(b, iter->slot, next))
    return false;
  if (index) {
    int old_idx = ny_native_nir_load_local_value(b, index->slot);
    int next_idx = old_idx >= 0
                       ? ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_ADD_I64,
                                                               .dst = -1,
                                                               .a = old_idx,
                                                               .b = one})
                       : -1;
    if (next_idx < 0 || !ny_native_nir_store_local_value(b, index->slot, next_idx))
      return false;
  }

  b->emitted_return = entry_return;
  b->last_value = entry_last_value;
  bool ok = ny_native_nir_emit_br(b, head_label) &&
            ny_native_nir_emit_label(b, end_label);
  ny_native_nir_scope_restore(b, loop_scope_mark);
  return ok;
}

static bool ny_native_nir_lower_for(ny_native_nir_builder_t *b, const stmt_t *s) {
  if (s->as.fr.init || s->as.fr.cond || s->as.fr.update)
    return ny_native_nir_fail(
        b, "native NYIR lower: only Nytrix iterator loops are supported here; use `for name in lo..hi { ... }` because ';' starts a comment");
  return ny_native_nir_lower_for_range(b, s);
}

static bool ny_native_nir_pattern_is_wildcard(const expr_t *pat) {
  return pat && pat->kind == NY_E_IDENT && pat->as.ident.name &&
         strcmp(pat->as.ident.name, "_") == 0;
}

static int ny_native_nir_emit_cmp(ny_native_nir_builder_t *b, int a, int rhs,
                                  ny_nir_cmp_t cmp) {
  int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = NY_NIR_CMP_I64,
                                               .dst = -1,
                                               .a = a,
                                               .b = rhs,
                                               .cmp = cmp});
  if (v < 0)
    ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  return v;
}

static int ny_native_nir_emit_bool_binop(ny_native_nir_builder_t *b,
                                         ny_nir_op_t op, int a, int rhs) {
  int v = ny_nir_emit(&b->nir, (ny_nir_inst_t){.op = op,
                                               .dst = -1,
                                               .a = a,
                                               .b = rhs});
  if (v < 0)
    ny_native_nir_fail(b, "native NYIR lower: allocation failed");
  return v;
}

static int ny_native_nir_lower_match_pattern(ny_native_nir_builder_t *b,
                                             int test_value,
                                             const expr_t *pat) {
  if (ny_native_nir_pattern_is_wildcard(pat))
    return ny_native_nir_emit_const(b, 1);

  if (pat && pat->kind == NY_E_BINARY && pat->as.binary.op &&
      strcmp(pat->as.binary.op, "..") == 0) {
    int lo = ny_native_nir_lower_expr(b, pat->as.binary.left);
    int hi = ny_native_nir_lower_expr(b, pat->as.binary.right);
    if (lo < 0 || hi < 0)
      return -1;
    int ge = ny_native_nir_emit_cmp(b, test_value, lo, NY_NIR_CMP_GE);
    int le = ny_native_nir_emit_cmp(b, test_value, hi, NY_NIR_CMP_LE);
    if (ge < 0 || le < 0)
      return -1;
    return ny_native_nir_emit_bool_binop(b, NY_NIR_AND_I64, ge, le);
  }

  int rhs = ny_native_nir_lower_expr(b, pat);
  if (rhs < 0)
    return -1;
  return ny_native_nir_emit_cmp(b, test_value, rhs, NY_NIR_CMP_EQ);
}

static int ny_native_nir_lower_match_patterns(ny_native_nir_builder_t *b,
                                              int test_value,
                                              const match_arm_t *arm) {
  if (!arm || arm->patterns.len == 0)
    return ny_native_nir_emit_const(b, 0);
  int combined = -1;
  for (size_t i = 0; i < arm->patterns.len; ++i) {
    int cur = ny_native_nir_lower_match_pattern(b, test_value,
                                                arm->patterns.data[i]);
    if (cur < 0)
      return -1;
    combined = combined < 0
                   ? cur
                   : ny_native_nir_emit_bool_binop(b, NY_NIR_OR_I64,
                                                   combined, cur);
    if (combined < 0)
      return -1;
  }
  return combined;
}

static bool ny_native_nir_lower_match(ny_native_nir_builder_t *b,
                                      const stmt_t *s) {
  if (!s || s->kind != NY_S_MATCH || !s->as.match.test)
    return ny_native_nir_fail(b, "native NYIR lower: malformed case/match");

  int test_value = ny_native_nir_lower_expr(b, s->as.match.test);
  if (test_value < 0)
    return false;

  int result_slot = ny_native_nir_temp_slot(b);
  int merge_label = b->next_label++;
  bool entry_return = b->emitted_return;
  int entry_last_value = b->last_value;
  bool all_taken_paths_return = true;

  int initial = entry_last_value >= 0 ? entry_last_value
                                      : ny_native_nir_emit_const(b, 0);
  if (initial < 0 || !ny_native_nir_store_local_value(b, result_slot, initial))
    return false;

  for (size_t i = 0; i < s->as.match.arms.len; ++i) {
    match_arm_t *arm = &s->as.match.arms.data[i];
    int next_label = b->next_label++;
    int pat = ny_native_nir_lower_match_patterns(b, test_value, arm);
    if (pat < 0)
      return false;
    int pat_false = ny_native_nir_emit_is_zero(b, pat);
    if (pat_false < 0 || !ny_native_nir_emit_br_if(b, pat_false, next_label))
      return false;

    if (arm->guard) {
      int guard = ny_native_nir_lower_expr(b, arm->guard);
      int guard_false = guard >= 0 ? ny_native_nir_emit_is_zero(b, guard) : -1;
      if (guard_false < 0 ||
          !ny_native_nir_emit_br_if(b, guard_false, next_label))
        return false;
    }

    b->emitted_return = false;
    if (!ny_native_nir_lower_stmt(b, arm->conseq))
      return false;
    bool arm_returns = b->emitted_return;
    if (!arm_returns) {
      all_taken_paths_return = false;
      if (b->last_value >= 0 &&
          !ny_native_nir_store_local_value(b, result_slot, b->last_value))
        return false;
      if (!ny_native_nir_emit_br(b, merge_label))
        return false;
    }

    if (!ny_native_nir_emit_label(b, next_label))
      return false;
  }

  if (s->as.match.default_conseq) {
    b->emitted_return = false;
    if (!ny_native_nir_lower_stmt(b, s->as.match.default_conseq))
      return false;
    bool default_returns = b->emitted_return;
    if (!default_returns) {
      all_taken_paths_return = false;
      if (b->last_value >= 0 &&
          !ny_native_nir_store_local_value(b, result_slot, b->last_value))
        return false;
      if (!ny_native_nir_emit_br(b, merge_label))
        return false;
    }
  } else {
    all_taken_paths_return = false;
  }

  b->emitted_return = entry_return || all_taken_paths_return;
  if (!b->emitted_return) {
    if (!ny_native_nir_emit_label(b, merge_label))
      return false;
    int loaded = ny_native_nir_load_local_value(b, result_slot);
    if (loaded < 0)
      return false;
    b->last_value = loaded;
  }
  return true;
}

static bool ny_native_nir_lower_stmt(ny_native_nir_builder_t *b, const stmt_t *s) {
  if (ny_native_nir_ignored_stmt(s) || (s && s->kind == NY_S_FUNC))
    return true;
  switch (s->kind) {
  case NY_S_BLOCK: {
    size_t mark = s->as.block.transparent ? b->local_count
                                           : ny_native_nir_scope_mark(b);
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      if (!ny_native_nir_lower_stmt(b, s->as.block.body.data[i])) {
        if (!s->as.block.transparent)
          ny_native_nir_scope_restore(b, mark);
        return false;
      }
      if (b->emitted_return)
        break;
    }
    if (!s->as.block.transparent)
      ny_native_nir_scope_restore(b, mark);
    return true;
  }
  case NY_S_VAR:
    return ny_native_nir_lower_var(b, s);
  case NY_S_EXPR: {
    int v = ny_native_nir_lower_expr(b, s->as.expr.expr);
    if (v < 0)
      return false;
    b->last_value = v;
    return true;
  }
  case NY_S_IF:
    return ny_native_nir_lower_if(b, s);
  case NY_S_WHILE:
    return ny_native_nir_lower_while(b, s);
  case NY_S_FOR:
    return ny_native_nir_lower_for(b, s);
  case NY_S_MATCH:
    return ny_native_nir_lower_match(b, s);
  case NY_S_BREAK:
    if (b->loop_depth == 0)
      return ny_native_nir_fail(b, "native NYIR lower: break outside loop");
    b->emitted_return = true;
    return ny_native_nir_emit_br(b, b->loop_end_labels[b->loop_depth - 1]);
  case NY_S_CONTINUE:
    if (b->loop_depth == 0)
      return ny_native_nir_fail(b, "native NYIR lower: continue outside loop");
    b->emitted_return = true;
    return ny_native_nir_emit_br(b, b->loop_continue_labels[b->loop_depth - 1]);
  case NY_S_RETURN: {
    int v = s->as.ret.value ? ny_native_nir_lower_expr(b, s->as.ret.value)
                            : ny_native_nir_emit_const(b, 0);
    return v >= 0 && ny_native_nir_emit_ret(b, v);
  }
  default:
    return ny_native_nir_fail(b,
                              "native NYIR lower: statement kind %d is not in shared NYIR yet",
                              (int)s->kind);
  }
}

/*
 * Shared NYIR optimization + verification step.  After calling this the
 * builder's NYIR is ready for codegen or diagnostics.
 */
static bool ny_native_nir_finalize(ny_native_nir_builder_t *b,
                                     char *err, size_t err_len) {
  ny_nir_opt_stats_t stats;
  if (!ny_nir_optimize_with_stats(&b->nir, &stats)) {
    if (verbose_enabled >= 1)
      fprintf(stderr, "nyir opt FAILED\n");
    if (err && err_len > 0 && err[0] == '\0')
      snprintf(err, err_len, "native NYIR: optimization failed");
    goto fail;
  }
  if (!ny_nir_verify(&b->nir, err, err_len))
    goto fail;
  if (verbose_enabled >= 1 && stats.pass_time_ms[8] > 0.001) {
    size_t removed = stats.before_insts - stats.after_insts;
    double pct = stats.before_insts > 0
                     ? 100.0 * (double)removed / stats.before_insts
                     : 0.0;
    fprintf(stderr, "nyir finalize: %zu→%zu insts (-%zu, %.1f%%) in %.2fms\n",
            stats.before_insts, stats.after_insts, removed, pct,
            stats.pass_time_ms[8]);
  }
  return true;
fail:
  /* Reduced repro dump on failure. */
  fprintf(stderr, "native NYIR repro (optimize/verify failed):\n");
  ny_nir_dump(stderr, &b->nir, "<failed>");
  return false;
}

static bool ny_native_nir_opt_dump(FILE *out, ny_native_nir_builder_t *b,
                                   const char *name, const ny_options *opt) {
  if (opt && opt->nyir_dump_raw) {
    fprintf(out, "nyir raw %s\n", name && name[0] ? name : "<anon>");
    ny_nir_dump(out, &b->nir, name);
  }
  ny_nir_opt_stats_t stats;
  if (!ny_nir_optimize_with_stats(&b->nir, &stats) ||
      !ny_nir_verify(&b->nir, b->err, b->err_len)) {
    if (b->err && b->err_len > 0 && b->err[0] == '\0')
      ny_native_set_err(b->err, b->err_len, "native NYIR dump: optimization failed");
    return false;
  }
  if (opt && opt->nyir_dump_stats)
    ny_nir_dump_stats(out, &stats);
  ny_nir_dump(out, &b->nir, name);
  return true;
}

static bool ny_native_nir_opt_dump_binary(FILE *out, ny_native_nir_builder_t *b,
                                          const char *name, char *err,
                                          size_t err_len) {
  ny_nir_opt_stats_t stats;
  if (!ny_nir_optimize_with_stats(&b->nir, &stats) ||
      !ny_nir_verify(&b->nir, b->err, b->err_len)) {
    if (err && err_len > 0 && err[0] == '\0')
      ny_native_set_err(err, err_len, "native NYIR binary dump: optimization failed");
    return false;
  }
  if (!ny_nir_dump_binary(out, &b->nir, name)) {
    ny_native_set_err(err, err_len, "native NYIR binary dump: write failed");
    return false;
  }
  return true;
}

/*
 * Lower a single function stmt into a finalized ny_nir_func_t.
 * Returns true on success; caller must ny_nir_func_free(out) when done.
 */
static bool ny_native_nir_build_function(const program_t *prog, const stmt_t *fn,
                                        ny_nir_func_t *out, char *err,
                                        size_t err_len) {
  if (!fn || fn->kind != NY_S_FUNC || !out)
    return false;
  memset(out, 0, sizeof(*out));
  ny_native_nir_builder_t b = {.last_value = -1, .err = err, .err_len = err_len,
                               .prog = prog};
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    if (!ny_native_nir_bind_local_typed(
            &b, fn->as.fn.params.data[i].name,
            ny_native_type_name_is_f64(fn->as.fn.params.data[i].type),
            ny_native_type_name_is_f32(fn->as.fn.params.data[i].type))) {
      ny_nir_func_free(&b.nir);
      return false;
    }
  }
  bool ok = ny_native_nir_lower_stmt(&b, fn->as.fn.body);
  if (ok && !b.emitted_return) {
    int ret = b.last_value >= 0 ? b.last_value : ny_native_nir_emit_const(&b, 0);
    ok = ret >= 0 && ny_native_nir_emit_ret(&b, ret);
  }
  if (ok)
    ok = ny_native_nir_finalize(&b, err, err_len);
  if (ok)
    *out = b.nir;
  else
    ny_nir_func_free(&b.nir);
  return ok;
}

/*
 * Build extern table from #include and extern top-level statements.
 * Populates the table with NY name → C symbol mappings so the call
 * lowerer can emit correct linker symbols for extern C functions.
 *
 * Like codegen_collect_links()/process_links(), this recurses through
 * the program: extern declarations are not guaranteed to be top-level.
 * The prelude/stdlib and the script wrapper nest user declarations
 * inside NY_S_MODULE/NY_S_BLOCK (and control-flow bodies), so a flat
 * scan of prog->body would silently miss them.
 */
static bool ny_native_nir_collect_extern(const stmt_t *s, ny_extern_table_t *t,
                                         char *err, size_t err_len) {
  if (!s)
    return true;
  if (s->kind == NY_S_EXTERN) {
    const char *ny_name = s->as.ext.name;
    const char *c_sym = s->as.ext.link_name ? s->as.ext.link_name : ny_name;
    unsigned pc = (unsigned)s->as.ext.params.len;
    if (!ny_extern_table_add(t, ny_name, c_sym, pc, false)) {
      if (err && err_len > 0)
        snprintf(err, err_len,
                 "NYIR extern: conflicting or duplicate extern '%s' "
                 "(C symbol '%s' conflicts with earlier declaration)",
                 ny_name, c_sym);
      return false;
    }
    return true;
  }
  if (s->kind == NY_S_INCLUDE) {
    const char *prefix = s->as.inc.prefix;
    char *src = ny_read_file(s->as.inc.path);
    if (!src)
      return true;
    size_t srclen = strlen(src);
    ny_parser_t parser;
    ny_parse_init(&parser, src, srclen);
    ny_cdecl_t decl;
    while (ny_parse_decl(&parser, &decl) > 0) {
      if (decl.kind != NY_CDECL_FUNC)
        continue;
      size_t nlen = decl.name.len;
      char cname[256];
      if (nlen >= sizeof(cname))
        nlen = sizeof(cname) - 1;
      memcpy(cname, decl.name.start, nlen);
      cname[nlen] = '\0';
      char ny_name[512];
      if (prefix && prefix[0]) {
        int nn = snprintf(ny_name, sizeof(ny_name), "%s.%s", prefix, cname);
        if (nn < 0 || (size_t)nn >= sizeof(ny_name))
          continue;
      } else {
        size_t nn = nlen;
        if (nn >= sizeof(ny_name))
          nn = sizeof(ny_name) - 1;
        memcpy(ny_name, cname, nn);
        ny_name[nn] = '\0';
      }
      char *ny_name_dup = ny_strdup(ny_name);
      char *c_sym = ny_strdup(cname);
      if (!ny_name_dup || !c_sym ||
          !ny_extern_table_add(t, ny_name_dup, c_sym, decl.param_count, true)) {
        free(ny_name_dup);
        free(c_sym);
        free(src);
        if (err && err_len > 0)
          snprintf(err, err_len, "NYIR extern: table full from #include");
        return false;
      }
    }
    free(src);
    return true;
  }
  /* Recurse through container statements, mirroring process_links(). */
  if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      if (!ny_native_nir_collect_extern(s->as.module.body.data[i], t, err, err_len))
        return false;
    return true;
  }
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (!ny_native_nir_collect_extern(s->as.block.body.data[i], t, err, err_len))
        return false;
    return true;
  }
  if (s->kind == NY_S_IF) {
    if (s->as.iff.conseq &&
        !ny_native_nir_collect_extern(s->as.iff.conseq, t, err, err_len))
      return false;
    if (s->as.iff.alt &&
        !ny_native_nir_collect_extern(s->as.iff.alt, t, err, err_len))
      return false;
    return true;
  }
  if (s->kind == NY_S_WHILE) {
    if (s->as.whl.body &&
        !ny_native_nir_collect_extern(s->as.whl.body, t, err, err_len))
      return false;
    if (s->as.whl.update &&
        !ny_native_nir_collect_extern(s->as.whl.update, t, err, err_len))
      return false;
    if (s->as.whl.init &&
        !ny_native_nir_collect_extern(s->as.whl.init, t, err, err_len))
      return false;
    return true;
  }
  if (s->kind == NY_S_FOR) {
    if (s->as.fr.init &&
        !ny_native_nir_collect_extern(s->as.fr.init, t, err, err_len))
      return false;
    if (s->as.fr.body &&
        !ny_native_nir_collect_extern(s->as.fr.body, t, err, err_len))
      return false;
    if (s->as.fr.update &&
        !ny_native_nir_collect_extern(s->as.fr.update, t, err, err_len))
      return false;
    return true;
  }
  if (s->kind == NY_S_TRY) {
    if (s->as.tr.body &&
        !ny_native_nir_collect_extern(s->as.tr.body, t, err, err_len))
      return false;
    if (s->as.tr.handler &&
        !ny_native_nir_collect_extern(s->as.tr.handler, t, err, err_len))
      return false;
    return true;
  }
  if (s->kind == NY_S_DEFER) {
    if (s->as.de.body &&
        !ny_native_nir_collect_extern(s->as.de.body, t, err, err_len))
      return false;
    return true;
  }
  if (s->kind == NY_S_MATCH) {
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      if (s->as.match.arms.data[i].conseq &&
          !ny_native_nir_collect_extern(s->as.match.arms.data[i].conseq, t, err,
                                        err_len))
        return false;
    if (s->as.match.default_conseq &&
        !ny_native_nir_collect_extern(s->as.match.default_conseq, t, err, err_len))
      return false;
    return true;
  }
  return true;
}

static bool ny_native_nir_build_extern_table(const program_t *prog,
                                              ny_extern_table_t *t,
                                              char *err, size_t err_len) {
  if (!t)
    return false;
  ny_extern_table_init(t);
  if (!prog)
    return true;
  for (size_t i = 0; i < prog->body.len; ++i) {
    const stmt_t *s = prog->body.data[i];
    if (!ny_native_nir_collect_extern(s, t, err, err_len))
      return false;
  }
  return true;
}

/*
 * Lower the top-level program statements into a finalized ny_nir_func_t for
 * rt_main.  Returns true on success; caller must ny_nir_func_free(out).
 */
static bool ny_native_nir_build_rt_main(const program_t *prog, ny_nir_func_t *out,
                                        const ny_extern_table_t *externs,
                                        char *err, size_t err_len) {
  if (!out)
    return false;
  memset(out, 0, sizeof(*out));
  ny_native_nir_builder_t b = {.last_value = -1, .err = err, .err_len = err_len,
                               .externs = externs, .prog = prog};
  for (size_t i = 0; prog && i < prog->body.len; ++i) {
    if (!ny_native_nir_lower_stmt(&b, prog->body.data[i])) {
      ny_nir_func_free(&b.nir);
      return false;
    }
    if (b.emitted_return)
      break;
  }
  if (!b.emitted_return) {
    if (b.last_value < 0) {
      ny_native_nir_fail(&b, "native NYIR: program has no raw expression result");
      ny_nir_func_free(&b.nir);
      return false;
    }
    if (!ny_native_nir_emit_ret(&b, b.last_value)) {
      ny_nir_func_free(&b.nir);
      return false;
    }
  }
  bool ok = ny_native_nir_finalize(&b, err, err_len);
  if (ok)
    *out = b.nir;
  else
    ny_nir_func_free(&b.nir);
  return ok;
}

bool ny_native_build_nir(const program_t *prog, const ny_options *opt,
                         ny_nir_func_t *rt_main_out,
                         ny_nir_func_t *funcs_out, size_t *func_count,
                         size_t max_funcs, char *err, size_t err_len) {
  if (!prog || !rt_main_out)
    return false;
  (void)opt; /* reserved for future NYIR pass-level options */
  memset(rt_main_out, 0, sizeof(*rt_main_out));
  if (func_count)
    *func_count = 0;

  /* Build extern table from #include and extern statements. */
  ny_extern_table_t externs;
  char extern_err[256] = {0};
  if (!ny_native_nir_build_extern_table(prog, &externs, extern_err,
                                        sizeof(extern_err))) {
    ny_native_set_err(err, err_len, "NYIR extern: %s", extern_err);
    return false;
  }

  /* Build user functions first. */
  if (funcs_out && func_count && max_funcs > 0) {
    size_t count = 0;
    for (size_t i = 0; i < prog->body.len && count < max_funcs; ++i) {
      const stmt_t *s = prog->body.data[i];
      if (!s || s->kind != NY_S_FUNC)
        continue;
      char local_err[256] = {0};
      if (!ny_native_nir_build_function(prog, s, &funcs_out[count], local_err,
                                       sizeof(local_err))) {
        /* If lowering failed, free any already-built functions. */
        for (size_t j = 0; j < count; ++j)
          ny_nir_func_free(&funcs_out[j]);
        *func_count = 0;
        /* Non-fatal: function NYIR build failure is not an error for
         * diagnostics-only mode.  Fall through to rt_main. */
        if (err && err_len > 0 && local_err[0])
          ny_native_set_err(err, err_len, "%s", local_err);
      } else {
        count++;
      }
    }
    *func_count = count;
  }

  /* Build rt_main with extern table. */
  bool ok = ny_native_nir_build_rt_main(prog, rt_main_out, &externs, err, err_len);
  for (size_t i = 0; i < externs.count; ++i) {
    if (externs.entries[i].owned) {
      free((void *)externs.entries[i].ny_name);
      free((void *)externs.entries[i].c_symbol);
    }
  }
  return ok;
}

static bool ny_native_nir_dump_function(FILE *out, const stmt_t *fn, char *err,
                                        size_t err_len,
                                        const ny_options *opt) {
  if (!fn || fn->kind != NY_S_FUNC)
    return true;
  ny_native_nir_builder_t b = {.last_value = -1, .err = err, .err_len = err_len};
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    if (!ny_native_nir_bind_local_typed(
            &b, fn->as.fn.params.data[i].name,
            ny_native_type_name_is_f64(fn->as.fn.params.data[i].type),
            ny_native_type_name_is_f32(fn->as.fn.params.data[i].type))) {
      ny_nir_func_free(&b.nir);
      return false;
    }
  }
  bool ok = ny_native_nir_lower_stmt(&b, fn->as.fn.body);
  if (ok && !b.emitted_return) {
    int ret = b.last_value >= 0 ? b.last_value : ny_native_nir_emit_const(&b, 0);
    ok = ret >= 0 && ny_native_nir_emit_ret(&b, ret);
  }
  if (ok)
    ok = ny_native_nir_opt_dump(out, &b,
                                fn->as.fn.name ? fn->as.fn.name : "<fn>", opt);
  ny_nir_func_free(&b.nir);
  return ok;
}

static bool ny_native_nir_dump_rt_main(FILE *out, const program_t *prog, char *err,
                                       size_t err_len,
                                       const ny_options *opt) {
  ny_native_nir_builder_t b = {.last_value = -1, .err = err, .err_len = err_len};
  for (size_t i = 0; prog && i < prog->body.len; ++i) {
    if (!ny_native_nir_lower_stmt(&b, prog->body.data[i])) {
      ny_nir_func_free(&b.nir);
      return false;
    }
    if (b.emitted_return)
      break;
  }
  if (!b.emitted_return) {
    if (b.last_value < 0) {
      ny_native_nir_fail(&b, "native NYIR dump unavailable: program has no raw expression result");
      ny_nir_func_free(&b.nir);
      return false;
    }
    if (!ny_native_nir_emit_ret(&b, b.last_value)) {
      ny_nir_func_free(&b.nir);
      return false;
    }
  }
  bool ok = ny_native_nir_opt_dump(out, &b, "rt_main", opt);
  ny_nir_func_free(&b.nir);
  return ok;
}

static size_t ny_native_nir_local_count(const ny_nir_func_t *f);
static bool ny_native_ensure_parent_dir_for_path(const char *path);

static bool ny_native_nir_dump_rt_main_binary(FILE *out, const program_t *prog,
                                              char *err, size_t err_len) {
  ny_native_nir_builder_t b = {.last_value = -1, .err = err, .err_len = err_len};
  for (size_t i = 0; prog && i < prog->body.len; ++i) {
    if (!ny_native_nir_lower_stmt(&b, prog->body.data[i])) {
      ny_nir_func_free(&b.nir);
      return false;
    }
    if (b.emitted_return)
      break;
  }
  if (!b.emitted_return) {
    if (b.last_value < 0) {
      ny_native_nir_fail(&b, "native NYIR binary dump unavailable: program has no raw expression result");
      ny_nir_func_free(&b.nir);
      return false;
    }
    if (!ny_native_nir_emit_ret(&b, b.last_value)) {
      ny_nir_func_free(&b.nir);
      return false;
    }
  }
  bool ok = ny_native_nir_opt_dump_binary(out, &b, "rt_main", err, err_len);
  ny_nir_func_free(&b.nir);
  return ok;
}


static bool ny_native_write_nir_metadata_report(const program_t *prog,
                                                const ny_options *opt,
                                                char *err, size_t err_len) {
  if (!opt || !opt->nyir_metadata_report)
    return true;
  FILE *out = stderr;
  if (opt->nyir_metadata_report_path && opt->nyir_metadata_report_path[0]) {
    ny_native_ensure_parent_dir_for_path(opt->nyir_metadata_report_path);
    out = fopen(opt->nyir_metadata_report_path, "wb");
    if (!out) {
      ny_native_set_err(err, err_len,
                        "native NYIR metadata: failed to open %s: %s",
                        opt->nyir_metadata_report_path, strerror(errno));
      return false;
    }
  }

  if (opt->nyir_metadata_bin_path && opt->nyir_metadata_bin_path[0]) {
    FILE *in = fopen(opt->nyir_metadata_bin_path, "rb");
    if (!in) {
      if (out != stderr)
        fclose(out);
      ny_native_set_err(err, err_len,
                        "native NYIR metadata: failed to open %s: %s",
                        opt->nyir_metadata_bin_path, strerror(errno));
      return false;
    }
    ny_nir_func_t f = {0};
    char name[128] = {0};
    char local_err[512] = {0};
    bool ok = ny_nir_load_binary(in, &f, name, sizeof(name), local_err,
                                 sizeof(local_err));
    fclose(in);
    if (ok) {
      ny_nir_metadata_summary_t summary = {0};
      ok = ny_nir_metadata_summary(&f, &summary, local_err,
                                   sizeof(local_err));
      if (ok) {
        fprintf(out, "nyir metadata report functions=1 source=binary path=%s\n",
                opt->nyir_metadata_bin_path);
        ny_nir_metadata_summary_dump(out, name[0] ? name : "rt_main",
                                     &summary);
      }
    }
    ny_nir_func_free(&f);
    if (out != stderr)
      fclose(out);
    if (!ok) {
      ny_native_set_err(err, err_len, "%s",
                        local_err[0] ? local_err
                                     : "native NYIR binary metadata failed");
      return false;
    }
    if (err && err_len > 0)
      err[0] = '\0';
    return true;
  }

  ny_nir_func_t rt_main = {0};
  ny_nir_func_t funcs[64] = {{0}};
  const char *names[64] = {0};
  size_t wanted_names = 0;
  for (size_t i = 0; prog && i < prog->body.len && wanted_names < 64; ++i) {
    const stmt_t *s = prog->body.data[i];
    if (s && s->kind == NY_S_FUNC)
      names[wanted_names++] = s->as.fn.name ? s->as.fn.name : "<fn>";
  }

  size_t func_count = 0;
  char local_err[512] = {0};
  bool ok = ny_native_build_nir(prog, opt, &rt_main, funcs, &func_count, 64,
                                local_err, sizeof(local_err));
  if (!ok) {
    if (out != stderr)
      fclose(out);
    ny_native_set_err(err, err_len, "%s",
                      local_err[0] ? local_err : "native NYIR build failed");
    return false;
  }

  fprintf(out, "nyir metadata report functions=%zu\n", func_count + 1);
  for (size_t i = 0; i < func_count; ++i) {
    ny_nir_metadata_summary_t summary = {0};
    if (!ny_nir_metadata_summary(&funcs[i], &summary, local_err,
                                 sizeof(local_err))) {
      ok = false;
      break;
    }
    ny_nir_metadata_summary_dump(out, i < wanted_names ? names[i] : "<fn>",
                                 &summary);
  }
  if (ok) {
    ny_nir_metadata_summary_t summary = {0};
    if (ny_nir_metadata_summary(&rt_main, &summary, local_err,
                                sizeof(local_err)))
      ny_nir_metadata_summary_dump(out, "rt_main", &summary);
    else
      ok = false;
  }

  ny_nir_func_free(&rt_main);
  for (size_t i = 0; i < func_count; ++i)
    ny_nir_func_free(&funcs[i]);
  if (out != stderr)
    fclose(out);
  if (!ok) {
    ny_native_set_err(err, err_len, "%s",
                      local_err[0] ? local_err : "native NYIR metadata failed");
    return false;
  }
  if (err && err_len > 0)
    err[0] = '\0';
  return true;
}

static bool ny_native_write_eval_profile(const ny_options *opt,
                                         const ny_nir_eval_result_t *result,
                                         const char *name, char *err,
                                         size_t err_len) {
  if (!opt || !opt->nyir_run_profile)
    return true;
  FILE *out = stderr;
  if (opt->nyir_run_profile_path && opt->nyir_run_profile_path[0]) {
    ny_native_ensure_parent_dir_for_path(opt->nyir_run_profile_path);
    out = fopen(opt->nyir_run_profile_path, "wb");
    if (!out) {
      ny_native_set_err(err, err_len,
                        "native NYIR VM profile: failed to open %s: %s",
                        opt->nyir_run_profile_path, strerror(errno));
      return false;
    }
  }
  ny_nir_eval_result_dump(out, name, result);
  if (out != stderr)
    fclose(out);
  return true;
}

static bool ny_native_write_eval_result(const ny_options *opt,
                                        const ny_nir_eval_result_t *result,
                                        const char *name, char *err,
                                        size_t err_len) {
  FILE *out = stderr;
  if (opt && opt->nyir_run_path && opt->nyir_run_path[0]) {
    ny_native_ensure_parent_dir_for_path(opt->nyir_run_path);
    out = fopen(opt->nyir_run_path, "wb");
    if (!out) {
      ny_native_set_err(err, err_len, "native NYIR VM: failed to open %s: %s",
                        opt->nyir_run_path, strerror(errno));
      return false;
    }
  }
  fprintf(out, "nyir vm function=%s returned=%s result=%" PRId64 " steps=%zu\n",
          name && name[0] ? name : "rt_main",
          result && result->returned ? "yes" : "no",
          result ? result->result : 0, result ? result->steps : 0);
  if (out != stderr)
    fclose(out);
  if (!ny_native_write_eval_profile(opt, result, name, err, err_len))
    return false;
  if (err && err_len > 0)
    err[0] = '\0';
  return true;
}


static size_t ny_native_vm_max_steps(const ny_options *opt) {
  if (opt && opt->nyir_run_max_steps >= 0)
    return (size_t)opt->nyir_run_max_steps;
  return 1000000;
}

static size_t ny_native_vm_recursion_limit(const ny_options *opt) {
  if (opt && opt->nyir_run_recursion_limit >= 0)
    return (size_t)opt->nyir_run_recursion_limit;
  return 256;
}

static bool ny_native_eval_ir_func(ny_nir_func_t *rt_main,
                                   const ny_options *opt, const char *name,
                                   char *err, size_t err_len) {
  size_t local_count = ny_native_nir_local_count(rt_main);
  int64_t *locals = local_count ? (int64_t *)calloc(local_count, sizeof(*locals))
                                : NULL;
  if (local_count && !locals) {
    ny_native_set_err(err, err_len, "native NYIR VM: out of memory");
    return false;
  }
  ny_nir_eval_result_t result = {0};
  bool ok = ny_nir_eval(rt_main, locals, local_count,
                        ny_native_vm_max_steps(opt), &result, err, err_len);
  free(locals);
  if (!ok)
    return false;
  return ny_native_write_eval_result(opt, &result, name, err, err_len);
}

static bool ny_native_emit_nir_func(ny_native_writer_t *w,
                                    const ny_native_target_info_t *target,
                                    const ny_nir_func_t *nir,
                                    const char *label, bool tag_return,
                                    char *err, size_t err_len);

typedef struct {
  ny_nir_func_t *funcs;
  const char **names;
  size_t count;
  size_t depth;
  size_t recursion_limit;
  size_t max_steps;
  ny_nir_eval_result_t *profile;
} ny_native_vm_call_ctx_t;

static void ny_native_vm_profile_merge(ny_nir_eval_result_t *dst,
                                       const ny_nir_eval_result_t *src) {
  if (!dst || !src)
    return;
  dst->steps += src->steps;
  dst->branch_taken += src->branch_taken;
  dst->branch_not_taken += src->branch_not_taken;
  dst->call_count += src->call_count;
  if (src->max_value_index > dst->max_value_index)
    dst->max_value_index = src->max_value_index;
  if (src->max_local_index > dst->max_local_index)
    dst->max_local_index = src->max_local_index;
  if (src->max_pc > dst->max_pc)
    dst->max_pc = src->max_pc;
  for (size_t i = 0; i < (size_t)NYIR_OP_COUNT; ++i)
    dst->op_counts[i] += src->op_counts[i];
}

static bool ny_native_vm_symbol_matches(const char *symbol, const char *name) {
  if (!symbol || !name)
    return false;
  if (strcmp(symbol, name) == 0)
    return true;
  return strncmp(symbol, "ny_fn_", 6) == 0 && strcmp(symbol + 6, name) == 0;
}

static bool ny_native_vm_call_resolve(void *opaque, const char *symbol,
                                      const int64_t *args, size_t arg_count,
                                      int64_t *out, char *err,
                                      size_t err_len) {
  ny_native_vm_call_ctx_t *ctx = (ny_native_vm_call_ctx_t *)opaque;
  if (!ctx || !symbol)
    return ny_native_set_err(err, err_len, "native NYIR VM: missing call target"), false;
  if (ctx->depth >= ctx->recursion_limit)
    return ny_native_set_err(err, err_len,
                             "native NYIR VM: recursive call limit exceeded at depth %zu",
                             ctx->depth),
           false;
  if ((strcmp(symbol, "malloc") == 0 || strcmp(symbol, "__malloc") == 0) &&
      arg_count == 1) {
    void *p = malloc((size_t)(args ? args[0] : 0));
    if (!p)
      return ny_native_set_err(err, err_len, "native NYIR VM: malloc failed"),
             false;
    if (out)
      *out = (int64_t)(uintptr_t)p;
    return true;
  }
  if ((strcmp(symbol, "free") == 0 || strcmp(symbol, "__free") == 0) &&
      arg_count == 1) {
    free((void *)(uintptr_t)(args ? args[0] : 0));
    if (out)
      *out = 0;
    return true;
  }
  for (size_t i = 0; i < ctx->count; ++i) {
    if (!ny_native_vm_symbol_matches(symbol, ctx->names[i]))
      continue;
    ny_nir_func_t *callee = &ctx->funcs[i];
    size_t local_count = ny_native_nir_local_count(callee);
    if (local_count < arg_count)
      local_count = arg_count;
    int64_t *locals = local_count ? (int64_t *)calloc(local_count, sizeof(*locals))
                                  : NULL;
    if (local_count && !locals)
      return ny_native_set_err(err, err_len, "native NYIR VM: out of memory"), false;
    for (size_t a = 0; a < arg_count; ++a)
      locals[a] = args ? args[a] : 0;
    ny_nir_eval_result_t r = {0};
    ctx->depth++;
    bool ok = ny_nir_eval_with_calls(callee, locals, local_count,
                                     ctx->max_steps, &r,
                                     ny_native_vm_call_resolve, ctx, err,
                                     err_len);
    ctx->depth--;
    free(locals);
    if (!ok)
      return false;
    ny_native_vm_profile_merge(ctx->profile, &r);
    if (!r.returned)
      return ny_native_set_err(err, err_len,
                               "native NYIR VM: callee '%s' did not return",
                               ctx->names[i] ? ctx->names[i] : symbol),
             false;
    if (out)
      *out = r.result;
    return true;
  }
  return ny_native_set_err(err, err_len,
                           "native NYIR VM: unresolved call target '%s'",
                           symbol),
         false;
}

static bool ny_native_eval_ir_func_with_calls(ny_nir_func_t *rt_main,
                                              ny_nir_func_t *funcs,
                                              const char **names, size_t count,
                                              const ny_options *opt,
                                              const char *name, char *err,
                                              size_t err_len) {
  size_t local_count = ny_native_nir_local_count(rt_main);
  int64_t *locals = local_count ? (int64_t *)calloc(local_count, sizeof(*locals))
                                : NULL;
  if (local_count && !locals) {
    ny_native_set_err(err, err_len, "native NYIR VM: out of memory");
    return false;
  }
  ny_native_vm_call_ctx_t ctx = {.funcs = funcs,
                                  .names = names,
                                  .count = count,
                                  .recursion_limit =
                                      ny_native_vm_recursion_limit(opt),
                                  .max_steps = ny_native_vm_max_steps(opt)};
  ny_nir_eval_result_t result = {0};
  ny_nir_eval_result_t nested_profile = {0};
  ctx.profile = &nested_profile;
  bool ok = ny_nir_eval_with_calls(rt_main, locals, local_count,
                                   ny_native_vm_max_steps(opt), &result,
                                   ny_native_vm_call_resolve, &ctx, err,
                                   err_len);
  free(locals);
  if (!ok)
    return false;
  ny_native_vm_profile_merge(&nested_profile, &result);
  nested_profile.returned = result.returned;
  nested_profile.result = result.result;
  return ny_native_write_eval_result(opt, &nested_profile, name, err, err_len);
}

static bool ny_native_eval_ir_value(ny_nir_func_t *rt_main,
                                    ny_nir_func_t *funcs,
                                    const char **names, size_t count,
                                    const ny_options *opt,
                                    ny_nir_eval_result_t *out, char *err,
                                    size_t err_len) {
  if (!rt_main || !out) {
    ny_native_set_err(err, err_len, "native oracle: missing NYIR entry");
    return false;
  }
  size_t local_count = ny_native_nir_local_count(rt_main);
  int64_t *locals = local_count ? (int64_t *)calloc(local_count, sizeof(*locals))
                                : NULL;
  if (local_count && !locals) {
    ny_native_set_err(err, err_len, "native oracle: out of memory");
    return false;
  }
  ny_native_vm_call_ctx_t ctx = {.funcs = funcs,
                                  .names = names,
                                  .count = count,
                                  .recursion_limit =
                                      ny_native_vm_recursion_limit(opt),
                                  .max_steps = ny_native_vm_max_steps(opt)};
  ny_nir_eval_result_t top = {0};
  ny_nir_eval_result_t nested = {0};
  ctx.profile = &nested;
  bool ok = ny_nir_eval_with_calls(rt_main, locals, local_count,
                                   ny_native_vm_max_steps(opt), &top,
                                   ny_native_vm_call_resolve, &ctx, err,
                                   err_len);
  free(locals);
  if (!ok)
    return false;
  ny_native_vm_profile_merge(&nested, &top);
  nested.returned = top.returned;
  nested.result = top.result;
  *out = nested;
  return true;
}

static bool ny_native_collect_vm_profile(ny_nir_func_t *rt_main,
                                         ny_nir_func_t *funcs,
                                         const char **names, size_t count,
                                         const ny_options *opt,
                                         ny_nir_eval_result_t *profile,
                                         char *err, size_t err_len) {
  if (!rt_main || !profile)
    return false;
  memset(profile, 0, sizeof(*profile));
  size_t local_count = ny_native_nir_local_count(rt_main);
  int64_t *locals = local_count ? (int64_t *)calloc(local_count, sizeof(*locals))
                                : NULL;
  if (local_count && !locals) {
    ny_native_set_err(err, err_len,
                      "native tier report VM profile: out of memory");
    return false;
  }
  ny_native_vm_call_ctx_t ctx = {.funcs = funcs,
                                  .names = names,
                                  .count = count,
                                  .recursion_limit =
                                      ny_native_vm_recursion_limit(opt),
                                  .max_steps = ny_native_vm_max_steps(opt),
                                  .profile = profile};
  ny_nir_eval_result_t top = {0};
  bool ok = ny_nir_eval_with_calls(rt_main, locals, local_count,
                                   ny_native_vm_max_steps(opt), &top,
                                   ny_native_vm_call_resolve, &ctx, err,
                                   err_len);
  free(locals);
  if (!ok)
    return false;
  ny_native_vm_profile_merge(profile, &top);
  profile->returned = top.returned;
  profile->result = top.result;
  return true;
}

static bool ny_native_result_oracle_emit_asm(
    const ny_native_target_info_t *target, const ny_nir_func_t *rt_main,
    const ny_nir_func_t *funcs, const char **names, size_t count,
    const char *path, char *err, size_t err_len) {
  ny_native_writer_t w = {0};
  bool ok = false;
  if (!target || !rt_main || !path || !*path) {
    ny_native_set_err(err, err_len, "native oracle: missing assembly target");
    return false;
  }
  if (target->target != NY_NATIVE_TARGET_X86_64) {
    ny_native_set_err(err, err_len,
                      "native oracle: only x86-64 raw-int native is supported");
    return false;
  }
  if (!ny_native_printf(&w, "# Nytrix native result oracle (NYIR only)\n") ||
      !ny_native_put(&w, "\t.text\n"))
    goto done;
  for (size_t i = 0; i < count; ++i) {
    char label[256];
    snprintf(label, sizeof(label), "ny_fn_%s",
             names && names[i] && names[i][0] ? names[i] : "unknown_fn");
    if (!ny_native_emit_nir_func(&w, target, &funcs[i], label, false, err,
                                 err_len))
      goto done;
  }
  if (!ny_native_emit_nir_func(&w, target, rt_main, "rt_main", false, err,
                               err_len))
    goto done;
  ok = ny_write_file(path, w.data ? w.data : "", w.len) == 0;
  if (!ok)
    ny_native_set_err(err, err_len, "native oracle: failed to write %s: %s",
                      path, strerror(errno));
done:
  free(w.data);
  return ok;
}

static bool ny_native_parse_i64(const char *s, int64_t *out) {
  if (!s || !*s || !out)
    return false;
  errno = 0;
  char *end = NULL;
  long long v = strtoll(s, &end, 10);
  if (errno != 0 || end == s)
    return false;
  while (end && *end && isspace((unsigned char)*end))
    end++;
  if (end && *end)
    return false;
  *out = (int64_t)v;
  return true;
}

static bool ny_native_nir_returns_f64(const ny_nir_func_t *f) {
  if (!f || f->next_value <= 0)
    return false;
  bool f64[4096] = {0};
  int limit = f->next_value < 4096 ? f->next_value : 4096;
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; i < f->len; ++i) {
      const ny_nir_inst_t *in = &f->data[i];
      if (in->dst >= 0 && in->dst < limit &&
          (in->op == NYIR_CONST_F64 || in->op == NYIR_ADD_F64 ||
           in->op == NYIR_SUB_F64 || in->op == NYIR_MUL_F64 ||
           in->op == NYIR_DIV_F64 || in->op == NYIR_I64_TO_F64 ||
           in->op == NYIR_F32_TO_F64 ||
           (in->op == NY_NIR_CALL && (in->flags & NY_NIR_INST_F_RET_F64))) &&
          !f64[in->dst]) {
        f64[in->dst] = true;
        changed = true;
      }
      if (in->op == NY_NIR_COPY && in->a >= 0 && in->a < limit &&
          in->dst >= 0 && in->dst < limit && f64[in->a] && !f64[in->dst]) {
        f64[in->dst] = true;
        changed = true;
      }
    }
  }
  for (size_t i = f->len; i > 0; --i) {
    const ny_nir_inst_t *in = &f->data[i - 1];
    if (in->op == NY_NIR_RET && in->a >= 0 && in->a < limit)
      return f64[in->a];
  }
  return false;
}

static bool ny_native_nir_returns_f32(const ny_nir_func_t *f) {
  if (!f || f->next_value <= 0)
    return false;
  bool f32v[4096] = {0};
  int limit = f->next_value < 4096 ? f->next_value : 4096;
  for (size_t i = 0; i < f->len; ++i) {
    const ny_nir_inst_t *in = &f->data[i];
    if (in->dst >= 0 && in->dst < limit &&
        (in->op == NYIR_CONST_F32 || in->op == NYIR_ADD_F32 ||
         in->op == NYIR_SUB_F32 || in->op == NYIR_MUL_F32 ||
         in->op == NYIR_DIV_F32 || in->op == NYIR_I64_TO_F32 ||
         in->op == NYIR_F64_TO_F32 ||
         (in->op == NY_NIR_CALL && (in->flags & NY_NIR_INST_F_RET_F32))) &&
        !f32v[in->dst])
      f32v[in->dst] = true;
  }
  for (size_t i = f->len; i > 0; --i) {
    const ny_nir_inst_t *in = &f->data[i - 1];
    if (in->op == NY_NIR_RET && in->a >= 0 && in->a < limit)
      return f32v[in->a];
  }
  return false;
}

static int ny_native_run_capture_i64(const char *exe, int64_t *out,
                                     char *err, size_t err_len) {
#ifdef _WIN32
  (void)exe;
  (void)out;
  ny_native_set_err(err, err_len,
                    "native oracle: result capture is not implemented on Windows");
  return -1;
#else
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    ny_native_set_err(err, err_len, "native oracle: pipe failed: %s",
                      strerror(errno));
    return -1;
  }
  pid_t pid = fork();
  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);
    execl(exe, exe, (char *)NULL);
    _exit(127);
  }
  close(pipefd[1]);
  if (pid < 0) {
    close(pipefd[0]);
    ny_native_set_err(err, err_len, "native oracle: fork failed: %s",
                      strerror(errno));
    return -1;
  }
  char buf[256];
  size_t len = 0;
  for (;;) {
    ssize_t n = read(pipefd[0], buf + len, sizeof(buf) - 1 - len);
    if (n > 0) {
      len += (size_t)n;
      if (len >= sizeof(buf) - 1)
        break;
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    break;
  }
  close(pipefd[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  buf[len] = '\0';
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    ny_native_set_err(err, err_len,
                      "native oracle: harness failed (status=%d output=%.*s)",
                      status, 180, buf);
    return -1;
  }
  char *line = strstr(buf, "native result function=rt_main returned=yes result=");
  if (!line) {
    ny_native_set_err(err, err_len,
                      "native oracle: missing result line (output=%.*s)", 180,
                      buf);
    return -1;
  }
  line += strlen("native result function=rt_main returned=yes result=");
  char *nl = strchr(line, '\n');
  if (nl)
    *nl = '\0';
  if (!ny_native_parse_i64(line, out)) {
    ny_native_set_err(err, err_len,
                      "native oracle: invalid result value '%s'", line);
    return -1;
  }
  return 0;
#endif
}

bool ny_native_result_oracle_for_program(const program_t *prog,
                                         const ny_options *opt, char *err,
                                         size_t err_len) {
  if (!prog || !opt || !opt->native_result_oracle)
    return true;
  ny_native_target_info_t target = {0};
  if (!ny_native_target_info_init(&target, opt) ||
      target.target != NY_NATIVE_TARGET_X86_64) {
    ny_native_set_err(err, err_len,
                      "native oracle: x86-64 native backend is required");
    return false;
  }

  ny_nir_func_t rt_main = {0};
  ny_nir_func_t funcs[128];
  const char *names[128];
  memset(funcs, 0, sizeof(funcs));
  memset(names, 0, sizeof(names));
  size_t count = 0;
  char local_err[512] = {0};
  if (!ny_native_build_nir(prog, opt, &rt_main, funcs, &count, 128, local_err,
                           sizeof(local_err))) {
    ny_native_set_err(err, err_len, "native oracle: %s",
                      local_err[0] ? local_err : "failed to build NYIR");
    return false;
  }
  size_t name_index = 0;
  for (size_t i = 0; prog && i < prog->body.len && name_index < count; ++i) {
    const stmt_t *stmt = prog->body.data[i];
    if (stmt && stmt->kind == NY_S_FUNC)
      names[name_index++] = stmt->as.fn.name ? stmt->as.fn.name : "<fn>";
  }

  bool ok = false;
  ny_nir_eval_result_t vm = {0};
  int64_t native_result = 0;
  char asm_path[4096], obj_path[4096], c_path[4096], exe_path[4096];
  unsigned long long stamp = (unsigned long long)ny_ticks_now();
  snprintf(asm_path, sizeof(asm_path), "%s/ny_oracle_%ld_%llu.s",
           ny_get_temp_dir(), (long)getpid(), stamp);
  snprintf(obj_path, sizeof(obj_path), "%s/ny_oracle_%ld_%llu.o",
           ny_get_temp_dir(), (long)getpid(), stamp);
  snprintf(c_path, sizeof(c_path), "%s/ny_oracle_%ld_%llu.c",
           ny_get_temp_dir(), (long)getpid(), stamp);
  snprintf(exe_path, sizeof(exe_path), "%s/ny_oracle_%ld_%llu",
           ny_get_temp_dir(), (long)getpid(), stamp);

  if (!ny_native_eval_ir_value(&rt_main, funcs, names, count, opt, &vm, err,
                               err_len))
    goto done;
  if (!vm.returned) {
    ny_native_set_err(err, err_len, "native oracle: VM did not return");
    goto done;
  }
  bool returns_f64 = ny_native_nir_returns_f64(&rt_main);
  bool returns_f32 = ny_native_nir_returns_f32(&rt_main);
  if (!ny_native_result_oracle_emit_asm(&target, &rt_main, funcs, names, count,
                                        asm_path, err, err_len))
    goto done;

  const char *cc = ny_builder_choose_cc();
  const char *as_argv[] = {cc, "-c", asm_path, "-o", obj_path, NULL};
  if (ny_exec_spawn(as_argv) != 0) {
    ny_native_set_err(err, err_len,
                      "native oracle: assembler failed for NYIR output");
    goto done;
  }
  const char *harness_i64 =
      "#include <stdio.h>\n"
      "extern long long rt_main(void);\n"
      "int main(void) {\n"
      "  long long r = rt_main();\n"
      "  printf(\"native result function=rt_main returned=yes result=%lld\\n\", r);\n"
      "  return 0;\n"
      "}\n";
  const char *harness_f64 =
      "#include <stdint.h>\n"
      "#include <stdio.h>\n"
      "#include <string.h>\n"
      "extern double rt_main(void);\n"
      "int main(void) {\n"
      "  double r = rt_main();\n"
      "  int64_t bits = 0;\n"
      "  memcpy(&bits, &r, sizeof(bits));\n"
      "  printf(\"native result function=rt_main returned=yes result=%lld\\n\", (long long)bits);\n"
      "  return 0;\n"
      "}\n";
  const char *harness_f32 =
      "#include <stdint.h>\n"
      "#include <stdio.h>\n"
      "#include <string.h>\n"
      "extern float rt_main(void);\n"
      "int main(void) {\n"
      "  float r = rt_main();\n"
      "  int32_t bits = 0;\n"
      "  memcpy(&bits, &r, sizeof(bits));\n"
      "  printf(\"native result function=rt_main returned=yes result=%lld\\n\", (long long)(int64_t)(uint32_t)bits);\n"
      "  return 0;\n"
      "}\n";
  const char *harness = returns_f32 ? harness_f32 :
                        returns_f64 ? harness_f64 : harness_i64;
  if (ny_write_file(c_path, harness, strlen(harness)) != 0) {
    ny_native_set_err(err, err_len, "native oracle: failed to write harness");
    goto done;
  }
  const char *link_argv[] = {cc, c_path, obj_path, "-no-pie", "-o", exe_path,
                             NULL};
  if (ny_exec_spawn(link_argv) != 0) {
    ny_native_set_err(err, err_len, "native oracle: harness link failed");
    goto done;
  }
  if (ny_native_run_capture_i64(exe_path, &native_result, err, err_len) != 0)
    goto done;
  if (native_result != vm.result) {
    ny_native_set_err(err, err_len,
                      "native oracle: VM/native mismatch vm=%" PRId64
                      " native=%" PRId64,
                      vm.result, native_result);
    goto done;
  }
  if (opt->native_result_oracle_expected &&
      opt->native_result_oracle_expected[0]) {
    int64_t expected = 0;
    if (!ny_native_parse_i64(opt->native_result_oracle_expected, &expected)) {
      ny_native_set_err(err, err_len,
                        "native oracle: invalid expected result '%s'",
                        opt->native_result_oracle_expected);
      goto done;
    }
    if (vm.result != expected) {
      ny_native_set_err(err, err_len,
                        "native oracle: expected=%" PRId64 " vm=%" PRId64
                        " native=%" PRId64,
                        expected, vm.result, native_result);
      goto done;
    }
  }
  fprintf(stderr,
          "native oracle function=rt_main vm=%" PRId64 " native=%" PRId64
          " ok=yes\n",
          vm.result, native_result);
  ok = true;

done:
  unlink(asm_path);
  unlink(obj_path);
  unlink(c_path);
  unlink(exe_path);
  ny_nir_func_free(&rt_main);
  for (size_t i = 0; i < count; ++i)
    ny_nir_func_free(&funcs[i]);
  return ok;
}

bool ny_native_eval_ir_binary_file(const char *path, const ny_options *opt,
                                   char *err, size_t err_len) {
  if (!path || !*path) {
    ny_native_set_err(err, err_len,
                      "native NYIR VM: missing binary input path");
    return false;
  }
  FILE *in = fopen(path, "rb");
  if (!in) {
    ny_native_set_err(err, err_len, "native NYIR VM: failed to open %s: %s",
                      path, strerror(errno));
    return false;
  }
  ny_nir_func_t f = {0};
  char name[128] = {0};
  bool ok = ny_nir_load_binary(in, &f, name, sizeof(name), err, err_len);
  fclose(in);
  if (!ok) {
    ny_nir_func_free(&f);
    return false;
  }
  ok = ny_native_eval_ir_func(&f, opt, name[0] ? name : "rt_main", err,
                              err_len);
  ny_nir_func_free(&f);
  return ok;
}

bool ny_native_eval_ir_for_program(const program_t *prog,
                                   const ny_options *opt, char *err,
                                   size_t err_len) {
  if (opt && opt->nyir_run_bin_path && opt->nyir_run_bin_path[0])
    return ny_native_eval_ir_binary_file(opt->nyir_run_bin_path, opt, err,
                                         err_len);
  ny_extern_table_t externs;
  ny_extern_table_init(&externs);
  if (!ny_native_nir_build_extern_table(prog, &externs, err, err_len))
    return false;
  ny_nir_func_t rt_main = {0};
  ny_nir_func_t funcs[128];
  const char *names[128];
  memset(funcs, 0, sizeof(funcs));
  memset(names, 0, sizeof(names));
  size_t count = 0;
  for (size_t i = 0; prog && i < prog->body.len && count < 128; ++i) {
    const stmt_t *s = prog->body.data[i];
    if (!s || s->kind != NY_S_FUNC)
      continue;
    char local_err[512] = {0};
    if (ny_native_nir_build_function(prog, s, &funcs[count], local_err,
                                     sizeof(local_err))) {
      names[count] = s->as.fn.name;
      count++;
    }
  }
  bool ok;
  if (!ny_native_nir_build_rt_main(prog, &rt_main, &externs, err, err_len)) {
    for (size_t i = 0; i < count; ++i)
      ny_nir_func_free(&funcs[i]);
    ok = false;
    goto eval_done_free;
  }
  ok = ny_native_eval_ir_func_with_calls(&rt_main, funcs, names, count, opt,
                                         "rt_main", err, err_len);
  ny_nir_func_free(&rt_main);
  for (size_t i = 0; i < count; ++i)
    ny_nir_func_free(&funcs[i]);
eval_done_free:
  for (size_t i = 0; i < externs.count; ++i) {
    if (externs.entries[i].owned) {
      free((void *)externs.entries[i].ny_name);
      free((void *)externs.entries[i].c_symbol);
    }
  }
  return ok;
}

bool ny_native_dump_ir_for_program(const program_t *prog,
                                   const ny_options *opt, char *err,
                                   size_t err_len) {
  if (!opt || !opt->native_dump_ir)
    return true;
  bool defer_metadata_bin_report =
      opt->nyir_metadata_report && opt->nyir_metadata_bin_path &&
      opt->nyir_metadata_bin_path[0] && opt->nyir_dump_bin &&
      opt->nyir_dump_bin_path && opt->nyir_dump_bin_path[0] &&
      strcmp(opt->nyir_metadata_bin_path, opt->nyir_dump_bin_path) == 0;
  if (!defer_metadata_bin_report &&
      !ny_native_write_nir_metadata_report(prog, opt, err, err_len))
    return false;
  bool run_binary_after_dump =
      opt->nyir_run && opt->nyir_run_bin_path && opt->nyir_run_bin_path[0] &&
      opt->nyir_dump_bin;
  if (opt->nyir_run && !run_binary_after_dump) {
    if (!ny_native_eval_ir_for_program(prog, opt, err, err_len))
      return false;
    if (!opt->nyir_dump_text && !opt->nyir_dump_bin)
      return true;
  }
  if (!opt->nyir_dump_text && !opt->nyir_dump_bin)
    return true;
  if (opt->nyir_dump_bin) {
    FILE *bout = stderr;
    if (opt->nyir_dump_bin_path && opt->nyir_dump_bin_path[0]) {
      bout = fopen(opt->nyir_dump_bin_path, "wb");
      if (!bout) {
        ny_native_set_err(err, err_len,
                          "native NYIR binary dump: failed to open %s: %s",
                          opt->nyir_dump_bin_path, strerror(errno));
        return false;
      }
    }
    char berr[512] = {0};
    bool bok = ny_native_nir_dump_rt_main_binary(bout, prog, berr, sizeof(berr));
    if (bout != stderr)
      fclose(bout);
    if (!bok) {
      ny_native_set_err(err, err_len, "%s",
                        berr[0] ? berr : "native NYIR binary dump failed");
      return false;
    }
    if (run_binary_after_dump) {
      if (!opt->nyir_dump_bin_path || !opt->nyir_dump_bin_path[0]) {
        ny_native_set_err(err, err_len,
                          "native NYIR VM: --nyir-run-bin with same-process dump requires --nyir-dump-bin=PATH");
        return false;
      }
      if (!ny_native_eval_ir_binary_file(opt->nyir_run_bin_path, opt, err,
                                         err_len))
        return false;
    }
    if (defer_metadata_bin_report &&
        !ny_native_write_nir_metadata_report(prog, opt, err, err_len))
      return false;
    if (!opt->nyir_dump_text)
      return true;
  }
  FILE *out = stderr;
  if (opt->native_dump_ir_path && opt->native_dump_ir_path[0]) {
    out = fopen(opt->native_dump_ir_path, "wb");
    if (!out) {
      ny_native_set_err(err, err_len, "native NYIR dump: failed to open %s: %s",
                        opt->native_dump_ir_path, strerror(errno));
      return false;
    }
  }

  bool attempted_any = false;
  for (size_t i = 0; prog && i < prog->body.len; ++i) {
    const stmt_t *s = prog->body.data[i];
    if (!s || s->kind != NY_S_FUNC)
      continue;
    attempted_any = true;
    char local_err[512] = {0};
    if (!ny_native_nir_dump_function(out, s, local_err, sizeof(local_err),
                                     opt)) {
      fprintf(out, "native NYIR dump unavailable for function %s: %s\n",
              s->as.fn.name ? s->as.fn.name : "<anon>",
              local_err[0] ? local_err : "unsupported shape");
    }
  }

  attempted_any = true;
  char local_err[512] = {0};
  if (!ny_native_nir_dump_rt_main(out, prog, local_err, sizeof(local_err),
                                  opt)) {
    fprintf(out, "%s\n", local_err[0] ? local_err :
            "native NYIR dump unavailable: unsupported program shape");
  }

  if (!attempted_any)
    fputs("native NYIR dump unavailable: program has no dumpable body\n", out);
  if (out != stderr)
    fclose(out);
  if (err && err_len > 0)
    err[0] = '\0';
  return true;
}

bool ny_native_tier_plan_init(ny_native_tier_plan_t *plan,
                              const ny_native_target_info_t *target,
                              const ny_options *opt) {
  if (!plan)
    return false;
  memset(plan, 0, sizeof(*plan));
  plan->backend_name = target && target->target_name ? target->target_name : "unknown";
  ny_opt_profile_kind_t profile =
      ny_opt_profile_kind_from_name(opt && opt->opt_profile ? opt->opt_profile
                                                            : NULL);
  switch (profile) {
  case NY_OPT_PROFILE_PEAK:
    plan->compile_budget = 1000000;
    plan->hot_threshold = 64;
    plan->cold_threshold = 2;
    plan->cache_score = 100;
    break;
  case NY_OPT_PROFILE_SPEED:
    plan->compile_budget = 500000;
    plan->hot_threshold = 32;
    plan->cold_threshold = 2;
    plan->cache_score = 80;
    break;
  case NY_OPT_PROFILE_COMPILE:
  case NY_OPT_PROFILE_NONE:
    plan->compile_budget = 25000;
    plan->hot_threshold = 8;
    plan->cold_threshold = 1;
    plan->cache_score = 20;
    plan->prefer_nir_vm = true;
    break;
  case NY_OPT_PROFILE_SIZE:
    plan->compile_budget = 75000;
    plan->hot_threshold = 16;
    plan->cold_threshold = 1;
    plan->cache_score = 50;
    break;
  case NY_OPT_PROFILE_BALANCED:
  case NY_OPT_PROFILE_CUSTOM:
  case NY_OPT_PROFILE_DEFAULT:
  default:
    plan->compile_budget = 150000;
    plan->hot_threshold = 16;
    plan->cold_threshold = 1;
    plan->cache_score = 60;
    break;
  }
  if (opt) {
    if (opt->native_tier_budget >= 0)
      plan->compile_budget = (size_t)opt->native_tier_budget;
    if (opt->native_hot_threshold >= 0)
      plan->hot_threshold = (size_t)opt->native_hot_threshold;
    if (opt->native_cold_threshold >= 0)
      plan->cold_threshold = (size_t)opt->native_cold_threshold;
    if (opt->native_cache_score >= 0)
      plan->cache_score = (unsigned)opt->native_cache_score;
    if (opt->native_prefer_vm)
      plan->prefer_nir_vm = true;
    if (opt->native_prefer_asm)
      plan->prefer_nir_vm = false;
  }
  plan->prefer_ast_fallback =
      target && (target->caps & (unsigned)NY_NATIVE_CAP_AST_FALLBACK) != 0 &&
      !plan->prefer_nir_vm;
  return true;
}

bool ny_native_handoff_summary(const ny_nir_func_t *nir,
                               ny_native_handoff_summary_t *summary) {
  if (!nir || !summary)
    return false;
  memset(summary, 0, sizeof(*summary));
  if (nir->len == 0)
    return true;
  summary->entry_points = 1;
  summary->deopt_safe_points = 1;
  for (size_t i = 0; i < nir->len; ++i) {
    const ny_nir_inst_t *in = &nir->data[i];
    switch (in->op) {
    case NY_NIR_RET:
      summary->return_points++;
      summary->deopt_safe_points++;
      break;
    case NY_NIR_CALL:
      summary->call_points++;
      summary->deopt_safe_points++;
      break;
    case NY_NIR_BR:
    case NY_NIR_BR_IF:
      summary->branch_points++;
      summary->deopt_safe_points++;
      break;
    case NY_NIR_LABEL:
      summary->label_points++;
      summary->deopt_safe_points++;
      break;
    default:
      break;
    }
  }
  return true;
}


static size_t ny_native_tier_inst_cost(const ny_nir_inst_t *in) {
  if (!in)
    return 0;
  switch (in->op) {
  case NY_NIR_NOP:
  case NY_NIR_LABEL:
    return 0;
  case NY_NIR_DIV_I64:
  case NY_NIR_MOD_I64:
    return 8;
  case NY_NIR_CALL:
    return 12;
  case NY_NIR_BR:
  case NY_NIR_BR_IF:
  case NY_NIR_RET:
    return 3;
  case NY_NIR_LOAD_LOCAL:
  case NY_NIR_STORE_LOCAL:
    return 2;
  default:
    return 1;
  }
}

typedef struct {
  size_t insts;
  int values;
  size_t cost;
  size_t calls;
  size_t branches;
  size_t memory_ops;
  size_t divmod_ops;
  size_t control_ops;
  size_t effect_ops;
} ny_native_tier_facts_t;

static void ny_native_tier_facts_add(ny_native_tier_facts_t *facts,
                                     const ny_nir_func_t *f) {
  if (!facts || !f)
    return;
  facts->insts += f->len;
  if (f->next_value > 0)
    facts->values += f->next_value;
  for (size_t i = 0; i < f->len; ++i) {
    const ny_nir_inst_t *in = &f->data[i];
    facts->cost += ny_native_tier_inst_cost(in);
    if (in->op == NY_NIR_CALL)
      facts->calls++;
    else if (in->op == NY_NIR_BR || in->op == NY_NIR_BR_IF)
      facts->branches++;
    else if (in->op == NY_NIR_LOAD_LOCAL || in->op == NY_NIR_STORE_LOCAL)
      facts->memory_ops++;
    if (in->op == NY_NIR_DIV_I64 || in->op == NY_NIR_MOD_I64)
      facts->divmod_ops++;
    if ((in->effects & (unsigned)NY_NIR_EFFECT_CONTROL) != 0)
      facts->control_ops++;
    if (in->effects != 0)
      facts->effect_ops++;
  }
}

static const char *ny_native_tier_recommendation(
    const ny_native_tier_plan_t *plan, const ny_native_target_info_t *target,
    const ny_native_tier_facts_t *facts) {
  if (!plan || !target || !facts)
    return "unavailable";
  bool has_vm = (target->caps & (unsigned)NY_NATIVE_CAP_NIR_VM) != 0;
  bool has_asm = (target->caps & (unsigned)NY_NATIVE_CAP_NIR_ASM) != 0;
  bool has_obj = (target->caps & ((unsigned)NY_NATIVE_CAP_ELF_OBJECT |
                                  (unsigned)NY_NATIVE_CAP_COFF_OBJECT |
                                  (unsigned)NY_NATIVE_CAP_MACHO_OBJECT)) != 0;
  if (facts->cost <= plan->cold_threshold && has_vm)
    return "nyir-vm-cold";
  if (plan->prefer_nir_vm && has_vm && facts->cost <= plan->compile_budget)
    return "nyir-vm-preferred";
  if (has_obj && plan->cache_score >= 50 && facts->cost >= plan->hot_threshold)
    return "native-object-cache";
  if (has_asm)
    return "native-asm";
  if (plan->prefer_ast_fallback)
    return "ast-fallback";
  return has_vm ? "nyir-vm" : "unsupported";
}

static const char *ny_native_tier_recommendation_with_profile(
    const ny_native_tier_plan_t *plan, const ny_native_target_info_t *target,
    const ny_native_tier_facts_t *facts,
    const ny_nir_eval_result_t *profile) {
  if (!profile || profile->steps == 0)
    return ny_native_tier_recommendation(plan, target, facts);
  bool has_obj =
      target && (target->caps & ((unsigned)NY_NATIVE_CAP_ELF_OBJECT |
                                 (unsigned)NY_NATIVE_CAP_COFF_OBJECT |
                                 (unsigned)NY_NATIVE_CAP_MACHO_OBJECT)) != 0;
  bool has_asm = target && (target->caps & (unsigned)NY_NATIVE_CAP_NIR_ASM) != 0;
  bool has_vm = target && (target->caps & (unsigned)NY_NATIVE_CAP_NIR_VM) != 0;
  if (plan && has_obj && plan->cache_score >= 50 &&
      profile->steps >= plan->hot_threshold)
    return "native-object-cache-profile";
  if (plan && plan->prefer_nir_vm && has_vm &&
      profile->steps <= plan->cold_threshold)
    return "nyir-vm-profile-cold";
  if (has_asm)
    return "native-asm-profile";
  return has_vm ? "nyir-vm-profile" : "unsupported";
}

static void ny_native_print_caps(FILE *out, unsigned caps) {
  bool first = true;
#define NY_CAP(name, bit)                                                        \
  do {                                                                           \
    if ((caps & (unsigned)(bit)) != 0) {                                         \
      fprintf(out, "%s%s", first ? "" : ",", name);                           \
      first = false;                                                             \
    }                                                                            \
  } while (0)
  NY_CAP("nir-asm", NY_NATIVE_CAP_NIR_ASM);
  NY_CAP("ast-fallback", NY_NATIVE_CAP_AST_FALLBACK);
  NY_CAP("asm-object", NY_NATIVE_CAP_ASM_OBJECT);
  NY_CAP("nir-vm", NY_NATIVE_CAP_NIR_VM);
  NY_CAP("elf-object", NY_NATIVE_CAP_ELF_OBJECT);
  NY_CAP("coff-object", NY_NATIVE_CAP_COFF_OBJECT);
  NY_CAP("macho-object", NY_NATIVE_CAP_MACHO_OBJECT);
#undef NY_CAP
  if (first)
    fputs("none", out);
}

bool ny_native_write_tier_report_for_program(const program_t *prog,
                                             const ny_options *opt, char *err,
                                             size_t err_len) {
  if (!opt || !opt->native_tier_report)
    return true;
  ny_native_target_info_t target = {0};
  if (!ny_native_target_info_init(&target, opt)) {
    ny_native_set_err(err, err_len,
                      "native tier report unavailable for selected backend");
    return false;
  }
  ny_native_tier_plan_t plan = {0};
  if (!ny_native_tier_plan_init(&plan, &target, opt)) {
    ny_native_set_err(err, err_len, "native tier report: failed to build plan");
    return false;
  }

  ny_nir_func_t rt_main = {0};
  ny_nir_func_t funcs[128];
  const char *func_names[128];
  memset(funcs, 0, sizeof(funcs));
  memset(func_names, 0, sizeof(func_names));
  size_t func_count = 0;
  char local_err[512] = {0};
  bool built = ny_native_build_nir(prog, opt, &rt_main, funcs, &func_count,
                                   128, local_err, sizeof(local_err));
  if (!built) {
    ny_native_set_err(err, err_len, "native tier report: %s",
                      local_err[0] ? local_err : "failed to build NYIR");
    return false;
  }
  size_t name_index = 0;
  for (size_t i = 0; prog && i < prog->body.len && name_index < func_count; ++i) {
    const stmt_t *stmt = prog->body.data[i];
    if (!stmt || stmt->kind != NY_S_FUNC)
      continue;
    func_names[name_index++] = stmt->as.fn.name ? stmt->as.fn.name : "<fn>";
  }

  ny_nir_eval_result_t vm_profile = {0};
  bool vm_profile_used = false;
  if (opt->nyir_run_profile && rt_main.len) {
    char profile_err[512] = {0};
    if (ny_native_collect_vm_profile(&rt_main, funcs, func_names, func_count,
                                     opt, &vm_profile, profile_err,
                                     sizeof(profile_err))) {
      vm_profile_used = true;
    } else if (verbose_enabled) {
      fprintf(stderr, "native tier report: VM profile unavailable: %s\n",
              profile_err[0] ? profile_err : "unknown error");
    }
  }

  ny_native_tier_facts_t facts = {0};
  ny_native_handoff_summary_t handoffs = {0};
  ny_native_tier_facts_add(&facts, &rt_main);
  ny_native_handoff_summary_t local_handoff = {0};
  if (ny_native_handoff_summary(&rt_main, &local_handoff)) {
    handoffs.entry_points += local_handoff.entry_points;
    handoffs.return_points += local_handoff.return_points;
    handoffs.call_points += local_handoff.call_points;
    handoffs.branch_points += local_handoff.branch_points;
    handoffs.label_points += local_handoff.label_points;
    handoffs.deopt_safe_points += local_handoff.deopt_safe_points;
  }
  for (size_t i = 0; i < func_count; ++i) {
    ny_native_tier_facts_add(&facts, &funcs[i]);
    memset(&local_handoff, 0, sizeof(local_handoff));
    if (ny_native_handoff_summary(&funcs[i], &local_handoff)) {
      handoffs.entry_points += local_handoff.entry_points;
      handoffs.return_points += local_handoff.return_points;
      handoffs.call_points += local_handoff.call_points;
      handoffs.branch_points += local_handoff.branch_points;
      handoffs.label_points += local_handoff.label_points;
      handoffs.deopt_safe_points += local_handoff.deopt_safe_points;
    }
  }

  FILE *out = stderr;
  if (opt->native_tier_report_path && opt->native_tier_report_path[0]) {
    ny_native_ensure_parent_dir_for_path(opt->native_tier_report_path);
    out = fopen(opt->native_tier_report_path, "wb");
    if (!out) {
      ny_native_set_err(err, err_len,
                        "native tier report: failed to open %s: %s",
                        opt->native_tier_report_path, strerror(errno));
      for (size_t i = 0; i < func_count; ++i)
        ny_nir_func_free(&funcs[i]);
      ny_nir_func_free(&rt_main);
      return false;
    }
  }

  fprintf(out, "native tier report target=%s abi=%s object=%s ptr=%zub\n",
          target.target_name ? target.target_name : "unknown",
          target.abi_name ? target.abi_name : "unknown",
          target.object_format ? target.object_format : "unknown",
          target.pointer_bits);
  fprintf(out, "caps=");
  ny_native_print_caps(out, target.caps);
  fputc('\n', out);
  fprintf(out,
          "plan budget=%zu hot=%zu cold=%zu cache=%u prefer_vm=%s ast_fallback=%s\n",
          plan.compile_budget, plan.hot_threshold, plan.cold_threshold,
          plan.cache_score, plan.prefer_nir_vm ? "yes" : "no",
          plan.prefer_ast_fallback ? "yes" : "no");
  fprintf(out,
          "facts functions=%zu insts=%zu values=%d cost=%zu calls=%zu "
          "branches=%zu locals=%zu divmod=%zu control=%zu effects=%zu\n",
          func_count + (rt_main.len ? 1u : 0u), facts.insts, facts.values,
          facts.cost, facts.calls, facts.branches, facts.memory_ops,
          facts.divmod_ops, facts.control_ops, facts.effect_ops);
  fprintf(out,
          "handoffs entries=%zu returns=%zu calls=%zu branches=%zu labels=%zu "
          "deopt_safe=%zu\n",
          handoffs.entry_points, handoffs.return_points, handoffs.call_points,
          handoffs.branch_points, handoffs.label_points,
          handoffs.deopt_safe_points);
  fprintf(out,
          "vm_profile used=%s returned=%s result=%" PRId64
          " steps=%zu calls=%zu branches_taken=%zu branches_not_taken=%zu "
          "max_pc=%zu max_value=%zu max_local=%zu\n",
          vm_profile_used ? "yes" : "no",
          vm_profile.returned ? "yes" : "no", vm_profile.result,
          vm_profile.steps, vm_profile.call_count, vm_profile.branch_taken,
          vm_profile.branch_not_taken, vm_profile.max_pc,
          vm_profile.max_value_index, vm_profile.max_local_index);
  fprintf(out, "recommend=%s\n",
          ny_native_tier_recommendation_with_profile(
              &plan, &target, &facts, vm_profile_used ? &vm_profile : NULL));

  ny_native_tier_facts_t rt_facts = {0};
  if (rt_main.len) {
    ny_native_tier_facts_add(&rt_facts, &rt_main);
    ny_native_handoff_summary_t rt_handoffs = {0};
    ny_native_handoff_summary(&rt_main, &rt_handoffs);
    fprintf(out,
            "function name=rt_main insts=%zu values=%d cost=%zu calls=%zu "
            "branches=%zu locals=%zu divmod=%zu control=%zu effects=%zu "
            "handoffs=%zu deopt_safe=%zu recommend=%s\n",
            rt_facts.insts, rt_facts.values, rt_facts.cost, rt_facts.calls,
            rt_facts.branches, rt_facts.memory_ops, rt_facts.divmod_ops,
            rt_facts.control_ops, rt_facts.effect_ops,
            rt_handoffs.entry_points + rt_handoffs.return_points +
                rt_handoffs.call_points + rt_handoffs.branch_points +
                rt_handoffs.label_points,
            rt_handoffs.deopt_safe_points,
            ny_native_tier_recommendation_with_profile(
                &plan, &target, &rt_facts,
                vm_profile_used ? &vm_profile : NULL));
  }

  size_t func_index = 0;
  for (size_t i = 0; prog && i < prog->body.len && func_index < func_count; ++i) {
    const stmt_t *stmt = prog->body.data[i];
    if (!stmt || stmt->kind != NY_S_FUNC)
      continue;
    ny_native_tier_facts_t fn_facts = {0};
    ny_native_tier_facts_add(&fn_facts, &funcs[func_index]);
    ny_native_handoff_summary_t fn_handoffs = {0};
    ny_native_handoff_summary(&funcs[func_index], &fn_handoffs);
    fprintf(out,
            "function name=%s insts=%zu values=%d cost=%zu calls=%zu "
            "branches=%zu locals=%zu divmod=%zu control=%zu effects=%zu "
            "handoffs=%zu deopt_safe=%zu recommend=%s\n",
            stmt->as.fn.name ? stmt->as.fn.name : "<fn>", fn_facts.insts,
            fn_facts.values, fn_facts.cost, fn_facts.calls,
            fn_facts.branches, fn_facts.memory_ops, fn_facts.divmod_ops,
            fn_facts.control_ops, fn_facts.effect_ops,
            fn_handoffs.entry_points + fn_handoffs.return_points +
                fn_handoffs.call_points + fn_handoffs.branch_points +
                fn_handoffs.label_points,
            fn_handoffs.deopt_safe_points,
            ny_native_tier_recommendation(&plan, &target, &fn_facts));
    func_index++;
  }

  if (out != stderr)
    fclose(out);
  for (size_t i = 0; i < func_count; ++i)
    ny_nir_func_free(&funcs[i]);
  ny_nir_func_free(&rt_main);
  if (err && err_len > 0)
    err[0] = '\0';
  return true;
}

bool ny_native_target_info_init(ny_native_target_info_t *info,
                                const ny_options *opt) {
  if (!info || !opt)
    return false;
  memset(info, 0, sizeof(*info));
  const char *triple = opt->host_triple;
  ny_native_backend_t backend = opt->native_backend;
  if (backend == NY_NATIVE_BACKEND_LLVM)
    return false;
  const char *target_name = "unknown";
  if (!ny_native_backend_target(backend, &info->target, &target_name)) {
    info->target = NY_NATIVE_TARGET_UNKNOWN;
    target_name = "unknown";
  }
  info->target_name = target_name;

  info->abi = opt->native_abi;
  if (info->abi == NY_NATIVE_ABI_AUTO) {
    if (info->target == NY_NATIVE_TARGET_ARM)
      info->abi = NY_NATIVE_ABI_AAPCS;
    else
      info->abi = ny_native_triple_is_windows(triple) ? NY_NATIVE_ABI_WIN64
                                                      : NY_NATIVE_ABI_SYSV;
  } else if (info->abi == NY_NATIVE_ABI_AAPCS &&
             info->target != NY_NATIVE_TARGET_ARM) {
    info->abi = ny_native_triple_is_windows(triple) ? NY_NATIVE_ABI_WIN64
                                                    : NY_NATIVE_ABI_SYSV;
  }
  info->abi_name = ny_native_abi_label(info->abi);
  info->object_format = ny_native_triple_is_macho(triple) ? "macho"
                        : ny_native_triple_is_windows(triple) ? "coff"
                                                              : "elf";
  info->symbol_prefix = strcmp(info->object_format, "macho") == 0 ? "_" : "";
  info->stack_align = 16;
  info->pointer_bits = 64;
  info->float_abi_name = "";
  if (info->target == NY_NATIVE_TARGET_X86_64 && info->abi == NY_NATIVE_ABI_WIN64) {
    static const char *win64_regs[] = {"%rcx", "%rdx", "%r8", "%r9"};
    for (size_t i = 0; i < 4; i++)
      info->gp_arg_regs[i] = win64_regs[i];
    info->gp_arg_reg_count = 4;
    info->shadow_space_bytes = 32;
    info->red_zone = false;
    info->caps = NY_NATIVE_CAP_NIR_ASM | NY_NATIVE_CAP_AST_FALLBACK |
                 NY_NATIVE_CAP_ASM_OBJECT | NY_NATIVE_CAP_NIR_VM;
    if (strcmp(info->object_format, "elf") == 0)
      info->caps |= NY_NATIVE_CAP_ELF_OBJECT;
    else if (strcmp(info->object_format, "coff") == 0)
      info->caps |= NY_NATIVE_CAP_COFF_OBJECT;
    else if (strcmp(info->object_format, "macho") == 0)
      info->caps |= NY_NATIVE_CAP_MACHO_OBJECT;
  } else if (info->target == NY_NATIVE_TARGET_X86_64) {
    static const char *sysv_regs[] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
    for (size_t i = 0; i < 6; i++)
      info->gp_arg_regs[i] = sysv_regs[i];
    info->gp_arg_reg_count = 6;
    info->shadow_space_bytes = 0;
    info->red_zone = true;
    info->caps = NY_NATIVE_CAP_NIR_ASM | NY_NATIVE_CAP_AST_FALLBACK |
                 NY_NATIVE_CAP_ASM_OBJECT | NY_NATIVE_CAP_NIR_VM;
    if (strcmp(info->object_format, "elf") == 0)
      info->caps |= NY_NATIVE_CAP_ELF_OBJECT;
    else if (strcmp(info->object_format, "coff") == 0)
      info->caps |= NY_NATIVE_CAP_COFF_OBJECT;
    else if (strcmp(info->object_format, "macho") == 0)
      info->caps |= NY_NATIVE_CAP_MACHO_OBJECT;
  } else if (info->target == NY_NATIVE_TARGET_AARCH64) {
    static const char *aarch64_regs[] = {"x0", "x1", "x2", "x3", "x4", "x5"};
    for (size_t i = 0; i < 6; i++)
      info->gp_arg_regs[i] = aarch64_regs[i];
    info->gp_arg_reg_count = 6;
    info->shadow_space_bytes = 0;
    info->red_zone = false;
    info->pointer_bits = 64;
    info->caps = NY_NATIVE_CAP_NIR_ASM | NY_NATIVE_CAP_NIR_VM;
  } else if (info->target == NY_NATIVE_TARGET_X86) {
    info->gp_arg_reg_count = 0;
    info->shadow_space_bytes = 0;
    info->red_zone = false;
    info->pointer_bits = 32;
    info->caps = NY_NATIVE_CAP_NIR_ASM | NY_NATIVE_CAP_NIR_VM |
                 NY_NATIVE_CAP_ASM_OBJECT;
    if (strcmp(info->object_format, "elf") == 0)
      info->caps |= NY_NATIVE_CAP_ELF_OBJECT;
  } else if (info->target == NY_NATIVE_TARGET_ARM) {
    static const char *aapcs_regs[] = {"r0", "r1", "r2", "r3"};
    info->abi = NY_NATIVE_ABI_AAPCS;
    info->abi_name = ny_native_abi_label(info->abi);
    for (size_t i = 0; i < 4; i++)
      info->gp_arg_regs[i] = aapcs_regs[i];
    info->gp_arg_reg_count = 4;
    info->shadow_space_bytes = 0;
    info->stack_align = 8;
    info->red_zone = false;
    info->pointer_bits = 32;
    info->float_abi_name = ny_native_arm_float_abi_name();
    info->caps = NY_NATIVE_CAP_NIR_ASM | NY_NATIVE_CAP_NIR_VM;
  } else if (info->target == NY_NATIVE_TARGET_BPF) {
    static const char *bpf_regs[] = {"r1", "r2", "r3", "r4", "r5"};
    for (size_t i = 0; i < 5; i++)
      info->gp_arg_regs[i] = bpf_regs[i];
    info->gp_arg_reg_count = 5;
    info->shadow_space_bytes = 0;
    info->red_zone = false;
    info->pointer_bits = 64;
    info->stack_align = 8;
    info->caps = NY_NATIVE_CAP_NIR_ASM | NY_NATIVE_CAP_NIR_VM;
  } else if (info->target == NY_NATIVE_TARGET_MIPS) {
    static const char *mips_regs[] = {"$a0", "$a1", "$a2", "$a3"};
    for (size_t i = 0; i < 4; i++)
      info->gp_arg_regs[i] = mips_regs[i];
    info->gp_arg_reg_count = 4;
    info->shadow_space_bytes = 0;
    info->red_zone = false;
    info->pointer_bits = 64;
    info->caps = NY_NATIVE_CAP_NIR_ASM | NY_NATIVE_CAP_NIR_VM;
  } else if (info->target == NY_NATIVE_TARGET_POWERPC) {
    static const char *ppc_regs[] = {"r3", "r4", "r5", "r6", "r7", "r8"};
    for (size_t i = 0; i < 6; i++)
      info->gp_arg_regs[i] = ppc_regs[i];
    info->gp_arg_reg_count = 6;
    info->shadow_space_bytes = 0;
    info->red_zone = false;
    info->pointer_bits = 64;
    info->caps = NY_NATIVE_CAP_NIR_ASM | NY_NATIVE_CAP_NIR_VM;
  } else if (info->target == NY_NATIVE_TARGET_AVR) {
    static const char *avr_regs[] = {"r24:r31", "r16:r23"};
    for (size_t i = 0; i < 2; i++)
      info->gp_arg_regs[i] = avr_regs[i];
    info->gp_arg_reg_count = 2;
    info->shadow_space_bytes = 0;
    info->red_zone = false;
    info->pointer_bits = 16;
    info->stack_align = 1;
    info->caps = NY_NATIVE_CAP_NIR_ASM | NY_NATIVE_CAP_NIR_VM;
  } else if (info->target == NY_NATIVE_TARGET_WASM) {
    static const char *wasm_regs[] = {"$a0", "$a1", "$a2", "$a3"};
    for (size_t i = 0; i < 4; i++)
      info->gp_arg_regs[i] = wasm_regs[i];
    info->gp_arg_reg_count = 4;
    info->shadow_space_bytes = 0;
    info->red_zone = false;
    info->pointer_bits = 32;
    info->caps = NY_NATIVE_CAP_NIR_ASM | NY_NATIVE_CAP_NIR_VM;
  } else if (info->target == NY_NATIVE_TARGET_RISCV) {
    static const char *riscv_regs[] = {"a0", "a1", "a2", "a3", "a4", "a5"};
    for (size_t i = 0; i < 6; i++)
      info->gp_arg_regs[i] = riscv_regs[i];
    info->gp_arg_reg_count = 6;
    info->shadow_space_bytes = 0;
    info->red_zone = false;
    info->pointer_bits = 64;
    info->caps = NY_NATIVE_CAP_NIR_ASM | NY_NATIVE_CAP_NIR_VM;
  }
  return info->target != NY_NATIVE_TARGET_UNKNOWN;
}

static bool ny_native_target_has(const ny_native_target_info_t *target,
                                 ny_native_target_cap_t cap) {
  return target && (target->caps & (unsigned)cap) != 0;
}

static size_t ny_native_nir_local_count(const ny_nir_func_t *f) {
  int64_t max_slot = -1;
  for (size_t i = 0; f && i < f->len; ++i) {
    const ny_nir_inst_t *in = &f->data[i];
    if ((in->op == NY_NIR_LOAD_LOCAL || in->op == NY_NIR_STORE_LOCAL ||
         in->op == NYIR_ADDR_LOCAL) &&
        in->imm > max_slot)
      max_slot = in->imm;
  }
  return max_slot >= 0 ? (size_t)max_slot + 1 : 0;
}

static bool ny_native_ensure_parent_dir_for_path(const char *path) {
  if (!path || !*path)
    return true;
  char tmp[4096];
  snprintf(tmp, sizeof(tmp), "%s", path);
  char *slash = strrchr(tmp, '/');
#ifdef _WIN32
  char *bslash = strrchr(tmp, '\\');
  if (!slash || (bslash && bslash > slash))
    slash = bslash;
#endif
  if (!slash || slash == tmp)
    return true;
  *slash = '\0';
  ny_ensure_dir_recursive(tmp);
  return true;
}

static bool ny_native_emit_nir_func(ny_native_writer_t *w,
                                    const ny_native_target_info_t *target,
                                    const ny_nir_func_t *nir,
                                    const char *label, bool tag_return,
                                    char *err, size_t err_len) {
  if (!target)
    return false;
  switch (target->target) {
  case NY_NATIVE_TARGET_X86_64:
    return ny_native_x86_64_emit_nir(w, target, nir, label, tag_return, err,
                                     err_len);
  case NY_NATIVE_TARGET_AARCH64:
    return ny_native_aarch64_emit_nir(w, target, nir, label, tag_return, err,
                                      err_len);
  case NY_NATIVE_TARGET_X86:
    return ny_native_i386_emit_nir(w, target, nir, label, tag_return, err,
                                   err_len);
  case NY_NATIVE_TARGET_ARM:
    return ny_native_arm_emit_nir(w, target, nir, label, tag_return, err,
                                  err_len);
  case NY_NATIVE_TARGET_RISCV:
    return ny_native_riscv_emit_nir(w, target, nir, label, tag_return, err,
                                    err_len);
  case NY_NATIVE_TARGET_BPF:
    return ny_native_bpf_emit_nir(w, target, nir, label, tag_return, err,
                                  err_len);
  case NY_NATIVE_TARGET_MIPS:
    return ny_native_mips_emit_nir(w, target, nir, label, tag_return, err,
                                   err_len);
  case NY_NATIVE_TARGET_POWERPC:
    return ny_native_powerpc_emit_nir(w, target, nir, label, tag_return, err,
                                      err_len);
  case NY_NATIVE_TARGET_AVR:
    return ny_native_avr_emit_nir(w, target, nir, label, tag_return, err,
                                  err_len);
  case NY_NATIVE_TARGET_WASM:
    return ny_native_wasm_emit_nir(w, target, nir, label, tag_return, err,
                                   err_len);
  default:
    ny_native_set_err(err, err_len,
                      "native backend target '%s' has no NYIR emitter",
                      target->target_name ? target->target_name : "unknown");
    return false;
  }
}

bool ny_native_emit_asm_entry(const program_t *prog, const ny_options *opt,
                              const char *path, const char *entry_name,
                              bool tag_return, char *err, size_t err_len) {
  ny_native_target_info_t target;
  if (!ny_native_target_info_init(&target, opt)) {
    ny_native_set_err(err, err_len, "native backend is not enabled");
    return false;
  }
  bool target_has_ast_fallback =
      ny_native_target_has(&target, NY_NATIVE_CAP_AST_FALLBACK);
  if (!ny_native_target_has(&target, NY_NATIVE_CAP_NIR_ASM)) {
    ny_native_set_err(err, err_len,
                      "native backend target '%s' is registered (abi=%s object=%s ptr=%zub) but no emitter is implemented yet",
                      target.target_name, target.abi_name,
                      target.object_format, target.pointer_bits);
    return false;
  }
  if (!opt->native_dump_ir_path && !opt->nyir_dump_bin_path &&
      !ny_native_dump_ir_for_program(prog, opt, err, err_len))
    return false;

  /* Try the NYIR-first codegen path: build, optimize, verify, emit from IR. */
  ny_nir_func_t rt_main_nir = {0};
  ny_nir_func_t func_nirs[64];
  size_t func_count = 0;
  char nir_err[512] = {0};
  bool nir_ok = ny_native_build_nir(prog, opt, &rt_main_nir, func_nirs,
                                     &func_count, 64, nir_err, sizeof(nir_err));

  ny_native_writer_t w = {0};
  bool ok = false;

  if (nir_ok && rt_main_nir.len > 0) {
    /* Emit header comment. */
    if (!ny_native_printf(&w, "# Nytrix native %s backend output (NYIR path)\n",
                          target.target_name))
      nir_ok = false;
    if (nir_ok &&
        !ny_native_printf(&w,
                          "# target=%s abi=%s object=%s ptr=%zub red_zone=%s shadow_space=%zu\n",
                          target.target_name, target.abi_name,
                          target.object_format, target.pointer_bits,
                          target.red_zone ? "yes" : "no",
                          target.shadow_space_bytes))
      nir_ok = false;
    ny_native_tier_plan_t tier = {0};
    if (nir_ok && ny_native_tier_plan_init(&tier, &target, opt) &&
        !ny_native_printf(&w,
                          "# tier budget=%zu hot=%zu cold=%zu cache=%u vm=%s ast_fallback=%s\n",
                          tier.compile_budget, tier.hot_threshold,
                          tier.cold_threshold, tier.cache_score,
                          tier.prefer_nir_vm ? "yes" : "no",
                          tier.prefer_ast_fallback ? "yes" : "no"))
      nir_ok = false;

    /* Emit user functions (raw return, no tagging). */
    if (nir_ok) {
      for (size_t i = 0; i < func_count; ++i) {
        const char *fn_name = NULL;
        /* Find the function name from the program AST. */
        for (size_t j = 0; j < prog->body.len; ++j) {
          const stmt_t *s = prog->body.data[j];
          if (s && s->kind == NY_S_FUNC && s->as.fn.name) {
            /* Match by order — the NYIR builder processes them in the same
             * order as the program body. */
            if (fn_name == NULL) {
              /* Count functions before position j that are FUNC. */
              size_t func_idx = 0;
              for (size_t k = 0; k < j; ++k) {
                if (prog->body.data[k] && prog->body.data[k]->kind == NY_S_FUNC)
                  func_idx++;
              }
              if (func_idx == i) {
                fn_name = s->as.fn.name;
                break;
              }
            }
          }
        }
        if (!fn_name)
          fn_name = "unknown_fn";
        /* Build the native label: ny_fn_<name>. */
        char label[256];
        snprintf(label, sizeof(label), "ny_fn_%s", fn_name);
        bool emitted = ny_native_emit_nir_func(&w, &target, &func_nirs[i],
                                               label, false, err, err_len);
        if (!emitted) {
          nir_ok = false;
          break;
        }
      }
    }

    /* Emit the top-level entry. */
    if (nir_ok) {
      const char *top_name = entry_name && entry_name[0] ? entry_name : "rt_main";
      bool emitted = ny_native_emit_nir_func(&w, &target, &rt_main_nir,
                                             top_name, tag_return, err,
                                             err_len);
      if (!emitted)
        nir_ok = false;
    }

    if (nir_ok)
      ok = ny_write_file(path, w.data ? w.data : "", w.len) == 0;

    if (verbose_enabled >= 1 && nir_ok) {
      fprintf(stderr, "native asm: %zu functions + %s (%zu NYIR insts total)"
                      " -> %s (%zu bytes)\n",
              func_count, entry_name && entry_name[0] ? entry_name : "rt_main",
              rt_main_nir.len, path ? path : "(stdout)", w.len);
    }

    /* Clean up NYIR. */
    ny_nir_func_free(&rt_main_nir);
    for (size_t i = 0; i < func_count; ++i)
      ny_nir_func_free(&func_nirs[i]);
  }

  if (!nir_ok || !ok) {
    /* NYIR path failed or produced no output; only x86-64 has AST fallback. */
    ny_nir_func_free(&rt_main_nir);
    for (size_t i = 0; i < func_count; ++i)
      ny_nir_func_free(&func_nirs[i]);
    free(w.data);
    w = (ny_native_writer_t){0};
    if (!target_has_ast_fallback) {
      if (err && err_len > 0 && err[0] == '\0')
        ny_native_set_err(err, err_len,
                          "native %s backend requires NYIR-supported input",
                          target.target_name);
      return false;
    }
    if (entry_name && entry_name[0] && strcmp(entry_name, "rt_main") != 0) {
      if (err && err_len > 0 && err[0] == '\0')
        ny_native_set_err(err, err_len,
                          "native NYIR path failed before executable entry emission");
      return false;
    }
    if (verbose_enabled >= 1)
      fprintf(stderr, "native asm: NYIR path failed, falling back to AST\n");
    char fallback_err[512] = {0};
    ok = ny_native_x86_64_emit_rt_main(&w, &target, prog, fallback_err,
                                       sizeof(fallback_err));
    if (ok)
      ok = ny_write_file(path, w.data ? w.data : "", w.len) == 0;
    if (!ok && err && err_len > 0 && err[0] == '\0')
      ny_native_set_err(err, err_len, "%s",
                        fallback_err[0] ? fallback_err
                                        : "native AST fallback failed");
    free(w.data);
    return ok;
  }

  if (!ok && err && err_len > 0 && err[0] == '\0')
    ny_native_set_err(err, err_len, "failed to write native assembly to %s: %s",
                      path ? path : "(null)", strerror(errno));
  free(w.data);
  return ok;
}

bool ny_native_emit_asm(const program_t *prog, const ny_options *opt,
                        const char *path, char *err, size_t err_len) {
  return ny_native_emit_asm_entry(prog, opt, path, "rt_main", true, err,
                                  err_len);
}

bool ny_native_emit_object(const program_t *prog, const ny_options *opt,
                           const char *path, const char *entry_name,
                           bool tag_return, char *err, size_t err_len) {
  if (!prog || !opt || !path || !*path) {
    ny_native_set_err(err, err_len,
                      "native object emission: missing input or output path");
    return false;
  }
  ny_native_target_info_t target;
  if (!ny_native_target_info_init(&target, opt)) {
    ny_native_set_err(err, err_len, "native object emission: backend disabled");
    return false;
  }
  if (!ny_native_target_has(&target, NY_NATIVE_CAP_ASM_OBJECT)) {
    ny_native_set_err(err, err_len,
                      "native object emission for target '%s' is not enabled yet; use --emit-asm for assembly output",
                      target.target_name ? target.target_name : "unknown");
    return false;
  }
  if (ny_native_target_has(&target, NY_NATIVE_CAP_ELF_OBJECT) ||
      ny_native_target_has(&target, NY_NATIVE_CAP_COFF_OBJECT) ||
      ny_native_target_has(&target, NY_NATIVE_CAP_MACHO_OBJECT)) {
    ny_nir_func_t rt_main_nir = {0};
    ny_nir_func_t func_nirs[64] = {{0}};
    size_t func_count = 0;
    char nir_err[512] = {0};
    if (ny_native_build_nir(prog, opt, &rt_main_nir, func_nirs, &func_count,
                            64, nir_err, sizeof(nir_err)) &&
        rt_main_nir.len > 0) {
      const char *obj_symbol =
          entry_name && entry_name[0] ? entry_name : "rt_main";
      const char *func_names[64] = {0};
      size_t name_count = 0;
      for (size_t i = 0; prog && i < prog->body.len && name_count < func_count; ++i) {
        const stmt_t *s = prog->body.data[i];
        if (s && s->kind == NY_S_FUNC)
          func_names[name_count++] = s->as.fn.name ? s->as.fn.name : "unknown_fn";
      }
      char obj_err[512] = {0};
      bool obj_ok = false;
      if (ny_native_target_has(&target, NY_NATIVE_CAP_ELF_OBJECT)) {
        if (target.target == NY_NATIVE_TARGET_X86) {
          obj_ok = ny_native_emit_elf32_i386_object_from_nirs(
              &rt_main_nir, func_nirs, func_names, func_count, &target, path,
              obj_symbol, tag_return, obj_err, sizeof(obj_err));
        } else {
          obj_ok = ny_native_emit_elf64_object_from_nirs(
              &rt_main_nir, func_nirs, func_names, func_count, &target, path,
              obj_symbol, tag_return, obj_err, sizeof(obj_err));
        }
      } else if (ny_native_target_has(&target, NY_NATIVE_CAP_COFF_OBJECT)) {
        obj_ok = ny_native_emit_coff_x64_object_from_nirs(
            &rt_main_nir, func_nirs, func_names, func_count, &target, path,
            obj_symbol, tag_return, obj_err, sizeof(obj_err));
      } else if (ny_native_target_has(&target, NY_NATIVE_CAP_MACHO_OBJECT)) {
        obj_ok = ny_native_emit_macho_x64_object_from_nirs(
            &rt_main_nir, func_nirs, func_names, func_count, &target, path,
            obj_symbol, tag_return, obj_err, sizeof(obj_err));
      }
      if (obj_ok) {
        for (size_t i = 0; i < func_count; ++i)
          ny_nir_func_free(&func_nirs[i]);
        ny_nir_func_free(&rt_main_nir);
        if (err && err_len > 0)
          err[0] = '\0';
        return true;
      }
      if (target.target == NY_NATIVE_TARGET_X86 &&
          ny_native_target_has(&target, NY_NATIVE_CAP_ELF_OBJECT)) {
        ny_native_set_err(err, err_len, "%s",
                          obj_err[0] ? obj_err
                                     : "i386 ELF object writer failed");
        for (size_t i = 0; i < func_count; ++i)
          ny_nir_func_free(&func_nirs[i]);
        ny_nir_func_free(&rt_main_nir);
        return false;
      }
    }
    for (size_t i = 0; i < func_count; ++i)
      ny_nir_func_free(&func_nirs[i]);
    ny_nir_func_free(&rt_main_nir);
    if (err && err_len > 0)
      err[0] = '\0';
  }
  char asm_path[4096];
  char asm_name[96];
  snprintf(asm_name, sizeof(asm_name), "ny_native_%ld_%llu.s", (long)getpid(),
           (unsigned long long)ny_ticks_now());
  ny_join_path(asm_path, sizeof(asm_path), ny_get_temp_dir(), asm_name);
  if (!ny_native_emit_asm_entry(prog, opt, asm_path, entry_name, tag_return,
                                err, err_len))
    return false;
  ny_native_ensure_parent_dir_for_path(path);
  const char *cc = ny_builder_choose_cc();
  const char *argv[] = {cc, "-c", asm_path, "-o", path, NULL};
  int rc = ny_exec_spawn(argv);
  unlink(asm_path);
  if (rc != 0) {
    ny_native_set_err(err, err_len,
                      "native object emission: assembler failed with rc=%d", rc);
    return false;
  }
  struct stat st;
  if (stat(path, &st) != 0 || st.st_size <= 0) {
    ny_native_set_err(err, err_len,
                      "native object emission: assembler produced no object");
    return false;
  }
  if (err && err_len > 0)
    err[0] = '\0';
  return true;
}
