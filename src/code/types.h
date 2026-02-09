#ifndef NY_CODE_TYPES_H
#define NY_CODE_TYPES_H

#include "base/common.h" // For VEC macro
#include <llvm-c/Core.h> // For LLVMTypeRef
#include <stdbool.h>     // For bool
#include <stddef.h>      // For size_t

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
  bool is_mut;
  bool is_used;
  bool owned;
  const char *type_name; // Optional static type annotation
} binding;

typedef VEC(binding) binding_list; // Now defined here

typedef VEC(char *) ny_str_list; // Existing in priv.h, moved here
typedef VEC(char *) str_list;    // Existing in priv.h, moved here

typedef struct fun_sig {
  const char *name;
  LLVMTypeRef type;
  LLVMValueRef value;
  stmt_t *stmt_t; // Use stmt_t from AST
  int arity;
  bool is_variadic;
  bool is_extern;
  const char *link_name;
  const char *return_type;
  bool owned;
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
} layout_def_t;

typedef struct sema_func_t {
  LLVMTypeRef resolved_return_type;
  ny_type_list resolved_param_types;
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
} scope;

#endif // NY_CODE_TYPES_H
