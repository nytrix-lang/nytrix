#ifndef NY_CODE_TYPES_H
#define NY_CODE_TYPES_H

#include "base/common.h" // For VEC macro
#include <stdbool.h>     // For bool
#include <stddef.h>      // For size_t
#include <stdint.h>      // For uint64_t

/*
 * Runtime-only builds (used by AOT/native artifact compilation in tests)
 * should not depend on LLVM headers being present on the host.
 */
#ifdef NYTRIX_RUNTIME_ONLY
typedef struct LLVMOpaqueType *LLVMTypeRef;
typedef struct LLVMOpaqueValue *LLVMValueRef;
typedef struct LLVMOpaqueBasicBlock *LLVMBasicBlockRef;
#else
#include <llvm-c/Core.h> // For LLVMTypeRef / LLVMValueRef / LLVMBasicBlockRef
#endif

// Forward declarations of AST node types used in semantic structs
typedef struct stmt_t stmt_t;
typedef struct token_t token_t;
typedef struct expr_t expr_t;
typedef struct program_t program_t;
// Removed: typedef struct ny_param_list ny_param_list; // Defined in ast/ast.h

// Forward declaration of codegen_t (defined in code.h)
typedef struct codegen_t codegen_t;

// Full definitions of common structs used by various codegen components
typedef struct binding {
  const char *name;
  LLVMValueRef value;
  stmt_t *stmt_t; // Use stmt_t from AST
  bool is_slot;   // true when `value` is an address that must be loaded/stored
  bool is_mut;
  bool is_used;
  bool owned;
  const char *type_name;      // Active type view (may be flow-narrowed)
  const char *decl_type_name; // Original declared/static type
  uint64_t name_hash;    // Lazy-computed hash for fast symbol lookup
  uint32_t name_len;     // Lazy-computed length for fast symbol lookup
} binding;

typedef VEC(binding) binding_list; // Now defined here

typedef VEC(char *) ny_str_list; // Existing in priv.h, moved here
typedef VEC(char *) str_list;    // Existing in priv.h, moved here

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
  stmt_t *stmt_t; // Use stmt_t from AST
  int arity;
  bool is_variadic;
  bool is_extern;
  bool is_pure;
  bool is_memo_safe;
  uint32_t effects;
  bool args_escape;
  bool args_mutated;
  bool returns_alias;
  bool effects_known;
  bool is_recursive;
  const char *link_name;
  const char *return_type;
  bool owned;
  uint64_t name_hash; // Lazy-computed hash for fast overload lookup
  uint32_t name_len;  // Lazy-computed length for fast overload lookup
} fun_sig;

// Semantic analysis structs for enum and type resolution
typedef struct enum_member_def_t {
  const char *name;
  int64_t value; // Resolved integer value of the enum member
} enum_member_def_t;

typedef VEC(enum_member_def_t) ny_enum_member_list;

typedef struct enum_def_t {
  const char *name;
  ny_enum_member_list members;
  stmt_t *stmt; // Reference to the original enum statement in the AST
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

// Definition of scope (needs binding, stmt_t)
typedef struct scope {
  VEC(binding) vars;
  VEC(stmt_t *) defers;
  LLVMBasicBlockRef break_bb;
  LLVMBasicBlockRef continue_bb;
  // Tiny bloom prefilter for local bindings to skip impossible scope scans.
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

#endif // NY_CODE_TYPES_H
