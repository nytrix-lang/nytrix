#ifndef NY_CODE_TYPES_H
#define NY_CODE_TYPES_H

#include "base/common.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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
typedef struct fun_sig fun_sig;

typedef enum ny_ct_fast_kind_t {
  NY_CT_FAST_NONE,
  NY_CT_FAST_INT,
  NY_CT_FAST_BOOL,
  NY_CT_FAST_STR,
  NY_CT_FAST_BIGINT,
  NY_CT_FAST_LIST,
  NY_CT_FAST_TUPLE,
  NY_CT_FAST_RANGE
} ny_ct_fast_kind_t;

typedef struct ny_ct_fast_val_t {
  ny_ct_fast_kind_t kind;
  int64_t i;
  bool b;
  const char *s;
  size_t len;
  struct ny_ct_fast_val_t *items;
  int64_t range_start;
  int64_t range_stop;
  int64_t range_step;
} ny_ct_fast_val_t;

static inline ny_ct_fast_val_t ny_ct_fast_none(void) {
  return (ny_ct_fast_val_t){.kind = NY_CT_FAST_NONE,
                            .i = 0,
                            .b = false,
                            .s = NULL,
                            .len = 0,
                            .items = NULL,
                            .range_start = 0,
                            .range_stop = 0,
                            .range_step = 1};
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
  } else if (v->kind == NY_CT_FAST_BIGINT) {
    *out = (v->s && strcmp(v->s, "0") != 0 && strcmp(v->s, "-0") != 0);
  } else if (v->kind == NY_CT_FAST_LIST || v->kind == NY_CT_FAST_TUPLE ||
             v->kind == NY_CT_FAST_RANGE) {
    *out = v->len > 0;
  } else {
    *out = true;
  }
  return true;
}

typedef VEC(const char *) assigned_name_list;
typedef VEC(uint64_t) assigned_hash_list;

typedef struct ny_name_set_slot {
  const char *name;
  uint64_t hash;
  size_t len;
} ny_name_set_slot;

typedef struct ny_name_set {
  ny_name_set_slot *slots;
  size_t cap;
  size_t len;
} ny_name_set;

typedef struct codegen_t codegen_t;

typedef enum ny_owner_state_t {
  NY_OWNER_BORROWED = 0,
  NY_OWNER_OWNED,
  /* Ownership was consumed without cleanup, e.g. forget() or transfer. */
  NY_OWNER_MOVED,
  /* Ownership was consumed by release() and cleanup was emitted. */
  NY_OWNER_RELEASED,
} ny_owner_state_t;

typedef struct binding {
  const char *name;
  LLVMValueRef value;
  LLVMValueRef raw_int_value;
  stmt_t *stmt_t;
  bool is_slot;
  bool is_mut;
  bool is_used;
  bool owned;
  bool is_stable;
  bool is_f64_slot;
  bool is_f32_slot;
  bool is_int_slot;
  bool is_f64_direct;
  bool is_f32_direct;
  bool is_int_direct;
  bool is_int_raw_direct;
  bool escapes;
  bool is_list_storage;
  bool is_int_list_storage;
  bool is_f64_list_storage;
  bool is_dict_storage;
  bool is_int_dict_storage;
  bool has_int_range;
  int64_t int_min_raw;
  int64_t int_max_raw;
  bool has_list_int_range;
  int64_t list_int_min_raw;
  int64_t list_int_max_raw;
  bool has_list_len_min;
  int64_t list_len_min_raw;
  bool static_indexable_invalid;
  bool static_indexable_object_elided;
  bool static_int_list_elide_candidate;
  bool static_int_list_elide_lowered;
  const char *static_int_list_elide_bail_reason;
  LLVMValueRef static_int_list_global;
  size_t static_int_list_len;
  bool static_int_list_untagged;
  LLVMValueRef raw_int_list_ptr;
  size_t raw_int_list_len;
  bool raw_int_list_mutation;
  bool raw_int_list_untagged;
  const char *raw_int_list_bail_reason;
  fun_sig *direct_callable_sig;
  bool has_dict_int_range;
  int64_t dict_int_min_raw;
  int64_t dict_int_max_raw;

  const char *type_name;
  const char *decl_type_name;
  uint64_t name_hash;
  uint32_t name_len;
  uint64_t tail_hash;
  uint32_t tail_len;
  bool tail_cached;
  bool ownership_tracked;
  bool ownership_raw_ptr;
  bool ownership_alloc_size_known;
  int64_t ownership_alloc_size_raw;
  bool ownership_forgotten;
  bool ownership_defer_registered;
  ny_owner_state_t owner_state;
  const char *ownership_borrow_source;
  uint64_t ownership_borrow_source_hash;
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

typedef enum ny_type_kind_t {
  NY_TYPE_CONCRETE,
  NY_TYPE_VAR,
  NY_TYPE_ARROW,
  NY_TYPE_APPLY,
} ny_type_kind_t;

typedef struct ny_type_t ny_type_t;

typedef struct ny_type_arena_t {
  arena_t arena;
  size_t nodes_allocated;
  int next_var_id;
} ny_type_arena_t;

struct ny_type_t {
  ny_type_kind_t kind;
  int id;
  union {
    const char *name;
    struct {
      ny_type_t *parent;
      ny_type_t *bound;
    } var;
    struct {
      ny_type_t *param;
      ny_type_t *ret;
    } arrow;
    struct {
      const char *name;
      ny_type_t *arg0;
      ny_type_t *arg1;
      int arity;
    } apply;
  } as;
};

void ny_type_arena_init(ny_type_arena_t *arena);
void ny_type_arena_reset(ny_type_arena_t *arena);
ny_type_t *ny_type_concrete(ny_type_arena_t *arena, const char *name);
ny_type_t *ny_type_var(ny_type_arena_t *arena);
ny_type_t *ny_type_arrow(ny_type_arena_t *arena, ny_type_t *param, ny_type_t *ret);
ny_type_t *ny_type_apply(ny_type_arena_t *arena, const char *name, ny_type_t *arg0,
                         ny_type_t *arg1, int arity);
ny_type_t *ny_type_find(ny_type_t *type);
bool ny_type_occurs(ny_type_t *needle, ny_type_t *haystack);
bool ny_type_unify(ny_type_t *a, ny_type_t *b);
const char *ny_type_kind_name(ny_type_kind_t kind);
char *ny_type_to_string(ny_type_t *type);

struct fun_sig {
  const char *name;
  const char *module_name;
  LLVMTypeRef type;
  LLVMValueRef value;
  stmt_t *stmt_t;
  const char *source_file;
  int arity;
  int min_arity;
  bool min_arity_known;
  bool is_variadic;
  bool is_extern;
  bool is_native_abi;
  bool is_pure;
  bool is_memo_safe;
  bool is_stable;
  uint32_t effects;
  bool args_escape;
  bool args_mutated;
  bool returns_alias;
  bool effects_known;
  bool is_recursive;
  bool tailcall;
  bool is_attached_method;
  const char *link_name;
  const char *return_type;
  const char *abi_return_type;
  const char *inferred_return_type;
  ny_str_list param_types;
  bool native_sret_return;
  bool returns_owned;
  const char *returns_borrow;
  ny_str_list borrows;
  ny_str_list consumes;
  ny_str_list mutates;
  ny_str_list releases;
  ny_str_list forgets;
  bool owned;
  uint64_t name_hash;
  uint32_t name_len;
  uint64_t tail_hash;
  uint32_t tail_len;
  bool tail_cached;
};

#define NY_MONO_MAX_ARITY 16

typedef enum ny_mono_type_kind_t {
  NY_MONO_TYPE_NONE = 0,
  NY_MONO_TYPE_INT = 1,
  NY_MONO_TYPE_F64 = 2,
  NY_MONO_TYPE_LIST = 3,
  NY_MONO_TYPE_F64_LIST = 4,
} ny_mono_type_kind_t;

typedef struct ny_mono_specialization_t {
  const char *base_name;
  const char *specialized_name;
  const char *accept_reason;
  const char *return_policy;
  stmt_t *base_stmt;
  stmt_t *specialized_stmt;
  uint64_t key_hash;
  size_t body_cost;
  int arity;
  uint8_t return_kind;
  bool raw_return_proven;
  bool raw_return_active;
  bool inline_body_eligible;
  bool arg_range_known[NY_MONO_MAX_ARITY];
  int64_t arg_min_raw[NY_MONO_MAX_ARITY];
  int64_t arg_max_raw[NY_MONO_MAX_ARITY];
  bool arg_list_len_min_known[NY_MONO_MAX_ARITY];
  int64_t arg_list_len_min_raw[NY_MONO_MAX_ARITY];
  uint8_t types[NY_MONO_MAX_ARITY];
} ny_mono_specialization_t;

typedef VEC(ny_mono_specialization_t) ny_mono_specialization_list;

typedef struct ny_operator_def_t {
  const char *op;
  const char *left_type;
  const char *right_type;
  const char *return_type;
  const char *target_name;
  const char *module_name;
  stmt_t *stmt;
} ny_operator_def_t;

typedef VEC(ny_operator_def_t) ny_operator_def_list;

typedef struct enum_field_def_t {
  const char *name;
  const char *type_name;
} enum_field_def_t;

typedef VEC(enum_field_def_t) ny_enum_field_def_list;

typedef struct enum_member_def_t {
  const char *name;
  int64_t value;
  int64_t runtime_tag;
  bool has_payload;
  ny_enum_field_def_list fields;
} enum_member_def_t;

typedef VEC(enum_member_def_t) ny_enum_member_list;
typedef VEC(char *) ny_enum_type_param_list;

typedef struct enum_def_t {
  const char *name;
  ny_enum_type_param_list type_params;
  ny_enum_member_list members;
  bool has_payload;
  int64_t adt_tag_base;
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
  bool heap_allocated;
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
  bool mono_param_range_known[NY_MONO_MAX_ARITY];
  int64_t mono_param_min_raw[NY_MONO_MAX_ARITY];
  int64_t mono_param_max_raw[NY_MONO_MAX_ARITY];
  bool mono_param_list_len_min_known[NY_MONO_MAX_ARITY];
  int64_t mono_param_list_len_min_raw[NY_MONO_MAX_ARITY];
  uint8_t mono_param_kinds[NY_MONO_MAX_ARITY];
} sema_func_t;

typedef struct sema_var_t {
  ny_type_list resolved_types;
  VEC(bool) is_int_proven;
  VEC(bool) is_f64_proven;
  VEC(bool) escapes;
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
  int64_t loop_trip_hint;
} scope;

#endif
