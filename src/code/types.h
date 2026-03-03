#ifndef NY_CODE_TYPES_H
#define NY_CODE_TYPES_H

#include "base/common.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef NYTRIX_RUNTIME_ONLY
typedef struct LLVMOpaqueType *LLVMTypeRef;
typedef struct LLVMOpaqueValue *LLVMValueRef;
typedef struct LLVMOpaqueBasicBlock *LLVMBasicBlockRef;
#else
#include <llvm-c/Core.h>
#endif

typedef struct stmt_t stmt_t;
typedef struct token_t token_t;
typedef struct expr_t expr_t;
typedef struct program_t program_t;

typedef enum ny_ct_fast_kind_t {
  NY_CT_FAST_NONE,
  NY_CT_FAST_INT,
  NY_CT_FAST_BOOL,
  NY_CT_FAST_STR
} ny_ct_fast_kind_t;

typedef struct ny_ct_fast_val_t {
  ny_ct_fast_kind_t kind;
  int64_t i;
  bool b;
  const char *s;
} ny_ct_fast_val_t;

static inline ny_ct_fast_val_t ny_ct_fast_none(void) {
  return (ny_ct_fast_val_t){.kind = NY_CT_FAST_NONE, .i = 0, .b = false, .s = NULL};
}

static inline bool ny_ct_fast_truthy(const ny_ct_fast_val_t *v, bool *out) {
  if (!v || !out)
    return false;
  if (v->kind == NY_CT_FAST_NONE) {
    *out = false;
  } else if (v->kind == NY_CT_FAST_BOOL) {
    *out = v->b;
  } else if (v->kind == NY_CT_FAST_INT) {
    *out = (v->i != 0);
  } else if (v->kind == NY_CT_FAST_STR) {
    *out = (v->s && *v->s);
  } else {
    *out = true;
  }
  return true;
}

typedef VEC(const char *) assigned_name_list;
typedef VEC(uint64_t) assigned_hash_list;

typedef struct codegen_t codegen_t;

typedef struct binding {
  const char *name;
  LLVMValueRef value;
  stmt_t *stmt_t;
  bool is_slot;
  bool is_mut;
  bool is_used;
  bool owned;
  bool is_stable;
  const char *type_name;
  const char *decl_type_name;
  uint64_t name_hash;
  uint32_t name_len;
  uint64_t tail_hash;
  uint32_t tail_len;
  bool tail_cached;
} binding;

typedef VEC(binding) binding_list;

typedef VEC(char *) ny_str_list;
typedef VEC(char *) str_list;

enum ny_effect_bits_t {
  NY_FX_NONE = 0u,
  NY_FX_IO = 1u << 0,
  NY_FX_ALLOC = 1u << 1,
  NY_FX_FFI = 1u << 2,
  NY_FX_THREAD = 1u << 3,
  NY_FX_ALL = NY_FX_IO | NY_FX_ALLOC | NY_FX_FFI | NY_FX_THREAD
};

typedef struct fun_sig {
  const char *name;
  LLVMTypeRef type;
  LLVMValueRef value;
  stmt_t *stmt_t;
  int arity;
  bool is_variadic;
  bool is_extern;
  bool is_pure;
  bool is_memo_safe;
  bool is_stable;
  uint32_t effects;
  bool args_escape;
  bool args_mutated;
  bool returns_alias;
  bool effects_known;
  bool is_recursive;
  const char *link_name;
  const char *return_type;
  bool owned;
  uint64_t name_hash;
  uint32_t name_len;
  uint64_t tail_hash;
  uint32_t tail_len;
  bool tail_cached;
} fun_sig;

typedef struct enum_member_def_t {
  const char *name;
  int64_t value;
} enum_member_def_t;

typedef VEC(enum_member_def_t) ny_enum_member_list;

typedef struct enum_def_t {
  const char *name;
  ny_enum_member_list members;
  stmt_t *stmt;
} enum_def_t;

typedef VEC(LLVMTypeRef) ny_type_list;

typedef struct type_layout_t {
  size_t size;
  size_t align;
  LLVMTypeRef llvm_type;
  bool is_valid;
} type_layout_t;

typedef struct layout_field_info_t {
  const char *name;
  const char *type_name;
  size_t offset;
  size_t size;
  size_t align;
} layout_field_info_t;

typedef VEC(layout_field_info_t) ny_layout_info_list;

typedef struct layout_def_t {
  const char *name;
  ny_layout_info_list fields;
  size_t size;
  size_t align;
  size_t align_override;
  size_t pack;
  stmt_t *stmt;
  bool is_layout;
  LLVMTypeRef llvm_type;
} layout_def_t;

typedef struct sema_func_t {
  LLVMTypeRef resolved_return_type;
  ny_type_list resolved_param_types;
  bool is_pure;
  bool purity_known;
  bool is_memo_safe;
  bool memo_known;
  uint32_t effects;
  bool effects_known;
  bool args_escape;
  bool args_mutated;
  bool returns_alias;
  bool escape_known;
  bool is_recursive;
} sema_func_t;

typedef struct sema_var_t {
  ny_type_list resolved_types;
} sema_var_t;

typedef struct scope {
  VEC(binding) vars;
  VEC(stmt_t *) defers;
  LLVMBasicBlockRef break_bb;
  LLVMBasicBlockRef continue_bb;
  uint64_t name_bloom[4];
  const char *lookup_name;
  size_t lookup_name_len;
  binding *lookup_hit;
  size_t lookup_vars_len;
  binding *lookup_vars_data;
  const char *lookup_miss_name;
  size_t lookup_miss_name_len;
  uint64_t lookup_miss_hash;
  size_t lookup_miss_vars_len;
  binding *lookup_miss_vars_data;
  const char *lookup_name_no_mark;
  size_t lookup_name_len_no_mark;
  binding *lookup_hit_no_mark;
  size_t lookup_vars_len_no_mark;
  binding *lookup_vars_data_no_mark;
  const char *lookup_miss_name_no_mark;
  size_t lookup_miss_name_len_no_mark;
  uint64_t lookup_miss_hash_no_mark;
  size_t lookup_miss_vars_len_no_mark;
  binding *lookup_miss_vars_data_no_mark;
} scope;

#endif
