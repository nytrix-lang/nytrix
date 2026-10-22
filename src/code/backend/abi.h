#ifndef NY_BACKEND_ABI_H
#define NY_BACKEND_ABI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <nyr/ir.h>
typedef enum {
  NY_NATIVE_RUNTIME_ABI_NO_DYNAMIC_BOXING = 1u << 0,
  NY_NATIVE_RUNTIME_ABI_RESULT_F64 = 1u << 1,
  /* Build and call the stdlib implementation instead of a compatibility stub.
   */
  NY_NATIVE_RUNTIME_ABI_SOURCE_PREFERRED = 1u << 2,
  /* Conversion work reported separately from ordinary runtime calls. */
  NY_NATIVE_RUNTIME_ABI_VALUE_CONVERSION = 1u << 3,
  /* Arguments/results cross this entry in the tagged dynamic-value ABI. */
  NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES = 1u << 4,
  /* This entry accepts tagged BigInt handles without scalar re-boxing. */
  NY_NATIVE_RUNTIME_ABI_BIGINT_VALUES = 1u << 5,
  /* Result representation is encoded by NY_NATIVE_RUNTIME_ABI_RESULT_MASK. */
  NY_NATIVE_RUNTIME_ABI_RESULT_CSTR = 1u << 6,
  NY_NATIVE_RUNTIME_ABI_RESULT_RAW_WORD = 2u << 6,
  NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_INT = 3u << 6,
  NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE = 4u << 6,
  /* Result is an opaque heap-backed BigInt handle. */
  NY_NATIVE_RUNTIME_ABI_RESULT_BIGINT = 5u << 6,
  /* Result is an unboxed integer or boolean word. */
  NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT = 6u << 6,
  /* Result is a raw address rather than a language-level value. */
  NY_NATIVE_RUNTIME_ABI_RESULT_POINTER = 7u << 6,
  /* Result is a language-level typed-buffer handle carried in a word. */
  NY_NATIVE_RUNTIME_ABI_RESULT_TYPED_BUFFER = 8u << 6,
} ny_native_runtime_abi_flag_t;

#define NY_NATIVE_RUNTIME_ABI_RESULT_MASK (15u << 6)

/* Per-argument tagged-value representation bits live above the entry flags
 * and the 4-bit result-kind field. Entries with richer raw/tagged/object
 * unions use the argument ABI table. */
#define NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(index)                                \
  (1u << (10u + (unsigned)(index)))

/* Typed call shapes used by the native NYIR debug VM. NYIR carries f64 values
 * as bits in i64 slots, while the C ABI may pass them in floating registers. */
typedef enum {
  NY_NATIVE_VM_CALL_NONE,
  NY_NATIVE_VM_I64_NULLARY_TO_I64,
  NY_NATIVE_VM_I64_BINARY_TO_I64,
  NY_NATIVE_VM_I64_TERNARY_TO_I64,
  NY_NATIVE_VM_I64_QUATERNARY_TO_I64,
  NY_NATIVE_VM_I64_QUINARY_TO_I64,
  NY_NATIVE_VM_I64_SENARY_TO_I64,
  NY_NATIVE_VM_I64_SEPTENARY_TO_I64,
  NY_NATIVE_VM_I64_OCTARY_TO_I64,
  NY_NATIVE_VM_I64_NONARY_TO_I64,
  NY_NATIVE_VM_I64_DECENARY_TO_I64,
  NY_NATIVE_VM_I64_UNDECENARY_TO_I64,
  NY_NATIVE_VM_F64_UNARY_TO_I64,
  NY_NATIVE_VM_F64_UNARY_TO_F64,
  NY_NATIVE_VM_F64_BINARY_TO_F64,
  NY_NATIVE_VM_I64_UNARY_TO_I64,
  NY_NATIVE_VM_I64_UNARY_TO_F64,
  NY_NATIVE_VM_I64_BINARY_TO_F64,
  NY_NATIVE_VM_I64_I64_F64_TO_I64,
  NY_NATIVE_VM_I64_F64_TO_I64,
  NY_NATIVE_VM_F64_I64_TO_I64,
} ny_native_vm_call_abi_t;

typedef union {
  int64_t (*i64_nullary_i64)(void);
  int64_t (*i64_binary_i64)(int64_t, int64_t);
  int64_t (*i64_ternary_i64)(int64_t, int64_t, int64_t);
  int64_t (*i64_quaternary_i64)(int64_t, int64_t, int64_t, int64_t);
  int64_t (*i64_quinary_i64)(int64_t, int64_t, int64_t, int64_t, int64_t);
  int64_t (*i64_senary_i64)(int64_t, int64_t, int64_t, int64_t, int64_t,
                            int64_t);
  int64_t (*i64_septenary_i64)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t);
  int64_t (*i64_octary_i64)(int64_t, int64_t, int64_t, int64_t, int64_t,
                            int64_t, int64_t, int64_t);
  int64_t (*i64_nonary_i64)(int64_t, int64_t, int64_t, int64_t, int64_t,
                            int64_t, int64_t, int64_t, int64_t);
  int64_t (*i64_decenary_i64)(int64_t, int64_t, int64_t, int64_t, int64_t,
                              int64_t, int64_t, int64_t, int64_t, int64_t);
  int64_t (*i64_undecenary_i64)(int64_t, int64_t, int64_t, int64_t, int64_t,
                                int64_t, int64_t, int64_t, int64_t, int64_t,
                                int64_t);
  int64_t (*f64_unary_i64)(double);
  double (*f64_unary_f64)(double);
  double (*f64_binary_f64)(double, double);
  int64_t (*i64_unary_i64)(int64_t);
  double (*i64_unary_f64)(int64_t);
  double (*i64_binary_f64)(int64_t, int64_t);
  int64_t (*i64_i64_f64_i64)(int64_t, int64_t, double);
  int64_t (*i64_f64_i64)(int64_t, double);
  int64_t (*f64_i64_i64)(double, int64_t);
} ny_native_vm_call_fn_t;

typedef struct {
  const char *symbol;
  unsigned flags;
  ny_native_vm_call_abi_t vm_call_abi;
  ny_native_vm_call_fn_t vm_call;
} ny_native_runtime_abi_decl_t;

/* Per-argument policies that are specific to an intrinsic boundary and
 * cannot be expressed by a whole-call tagged/raw flag. Masks use source
 * argument indices (the callable slot is not included). */
typedef struct {
  const char *symbol;
  uint16_t cstr_early_mask;
  uint16_t single_slot_mask;
  uint16_t preserve_tagged_mask;
  uint16_t pointer_arg_mask;
  uint16_t object_arg_mask;
  uint16_t typed_buffer_arg_mask;
  /* Two-bit NYIR formal ownership contracts for up to 16 source arguments.
   * Keep these independent of representation and actual-value ownership. */
  uint32_t formal_contracts;
} ny_native_runtime_arg_abi_decl_t;

static inline uint8_t ny_native_runtime_arg_formal_contract(
    const ny_native_runtime_arg_abi_decl_t *decl, size_t index) {
  if (!decl || index >= 16)
    return NYIR_ARG_CONTRACT_UNKNOWN;
  return (uint8_t)((decl->formal_contracts >> (index * 2u)) & 0x3u);
}

static inline int ny_native_runtime_arg_abi_is_pointer(
    const ny_native_runtime_arg_abi_decl_t *decl, size_t index) {
  return decl && index < 16 &&
         (decl->pointer_arg_mask & (uint16_t)(1u << index)) != 0;
}

/* Source-level runtime names whose concrete bridge is selected from the
 * argument representation. Empty replacements mean that representation uses
 * the declared symbol unchanged. */
typedef struct {
  const char *symbol;
  const char *f64_symbol;
  const char *tagged_value_symbol;
  const char *raw_i64_symbol;
} ny_native_runtime_overload_abi_decl_t;

/* Some low-level entries deliberately accept more than one source
 * representation in a word. Keep those unions and their ownership contract
 * explicit instead of treating an i64 C signature as a complete ABI. */
typedef enum {
  NY_NATIVE_ABI_VALUE_RAW_I64 = 1u << 0,
  NY_NATIVE_ABI_VALUE_TAGGED = 1u << 1,
  NY_NATIVE_ABI_VALUE_OBJECT_HANDLE = 1u << 2,
  NY_NATIVE_ABI_VALUE_RAW_CSTR = 1u << 3,
  NY_NATIVE_ABI_VALUE_ALIAS_ARG0 = 1u << 4,
  /* Representation is selected by the callable's encoded callback ABI. */
  NY_NATIVE_ABI_VALUE_CALLBACK_MARKED = 1u << 5,
} ny_native_runtime_value_repr_t;

typedef enum {
  NY_NATIVE_ABI_OWNERSHIP_UNSPECIFIED,
  NY_NATIVE_ABI_BORROWED,
  NY_NATIVE_ABI_BORROWED_ALIAS,
  /* Ownership follows the dynamic callback result marker and normalization. */
  NY_NATIVE_ABI_RESULT_ABI,
} ny_native_runtime_value_ownership_t;

typedef enum {
  NY_NATIVE_ABI_DICT_RECEIVER_NATIVE,
  NY_NATIVE_ABI_DICT_RECEIVER_MANAGED,
  NY_NATIVE_ABI_DICT_RECEIVER_COUNT,
} ny_native_runtime_dict_receiver_t;

typedef struct {
  const char *symbol;
  uint8_t arity;
  /* Representation unions may depend on the receiver's storage family. */
  uint8_t arg_repr[NY_NATIVE_ABI_DICT_RECEIVER_COUNT][3];
  uint8_t arg_ownership[3];
  uint8_t result_repr;
  uint8_t result_ownership;
} ny_native_runtime_value_abi_decl_t;

/* Convert the intrinsic's explicit ownership ABI into the corresponding
 * formal call contract carried by NYIR. Actual argument ownership is separate
 * metadata. A result alias is a returns-borrow contract only when the aliased
 * formal itself is declared borrowed. */
static inline uint8_t ny_native_runtime_value_formal_contract(
    const ny_native_runtime_value_abi_decl_t *decl, size_t index) {
  if (!decl || index >= decl->arity)
    return NYIR_ARG_CONTRACT_UNKNOWN;
  if (index == 0 && decl->result_repr == NY_NATIVE_ABI_VALUE_ALIAS_ARG0 &&
      decl->result_ownership == NY_NATIVE_ABI_BORROWED_ALIAS &&
      decl->arg_ownership[index] == NY_NATIVE_ABI_BORROWED)
    return NYIR_ARG_CONTRACT_RETURNS_BORROW;
  if (decl->arg_ownership[index] == NY_NATIVE_ABI_BORROWED)
    return NYIR_ARG_CONTRACT_BORROWS;
  return NYIR_ARG_CONTRACT_UNKNOWN;
}

typedef union {
  int64_t (*call0)(int64_t);
  int64_t (*call1)(int64_t, int64_t);
  int64_t (*call2)(int64_t, int64_t, int64_t);
  int64_t (*call3)(int64_t, int64_t, int64_t, int64_t);
  int64_t (*call4)(int64_t, int64_t, int64_t, int64_t, int64_t);
  int64_t (*call5)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
  int64_t (*call6)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                   int64_t);
  int64_t (*call7)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                   int64_t, int64_t);
  int64_t (*call8)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                   int64_t, int64_t, int64_t);
  int64_t (*call9)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                   int64_t, int64_t, int64_t, int64_t);
  int64_t (*call10)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                    int64_t, int64_t, int64_t, int64_t, int64_t);
  int64_t (*call11)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                    int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
  int64_t (*call12)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                    int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                    int64_t);
  int64_t (*call13)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                    int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                    int64_t, int64_t);
  int64_t (*call14)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                    int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                    int64_t, int64_t, int64_t);
  int64_t (*call15)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                    int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                    int64_t, int64_t, int64_t, int64_t);
} ny_native_runtime_callback_fn_t;

/* Dynamic callback adapters use the callable's marker bits to choose the
 * callback parameter and result representations. Their C slots therefore
 * accept unions of raw values, tagged values, and object handles instead of
 * one statically selected scalar ABI. The adapter borrows the callable and
 * input arguments for the duration of the call; result ownership is
 * follows the callback result marker: normalization preserves managed
 * handles, boxes raw scalars when required, and promotes raw C strings to
 * managed strings. The adapter does not retain or release returned handles. */
typedef struct {
  const char *symbol;
  uint8_t source_arity;
  uint8_t callable_repr;
  uint8_t arg_repr;
  uint8_t arg_ownership;
  uint8_t result_repr;
  uint8_t callable_ownership;
  uint8_t environment_ownership;
  uint8_t result_ownership;
  bool vm_resolve_unmarked_callable;
  ny_native_runtime_callback_fn_t call;
} ny_native_runtime_callback_abi_decl_t;

/* True when a typed VM call's C parameter at index uses the f64 ABI. */
static inline int
ny_native_runtime_abi_arg_is_f64(const ny_native_runtime_abi_decl_t *decl,
                                 size_t index) {
  if (!decl)
    return 0;
  switch (decl->vm_call_abi) {
  case NY_NATIVE_VM_I64_NULLARY_TO_I64:
  case NY_NATIVE_VM_I64_BINARY_TO_I64:
  case NY_NATIVE_VM_I64_TERNARY_TO_I64:
  case NY_NATIVE_VM_I64_QUATERNARY_TO_I64:
  case NY_NATIVE_VM_I64_QUINARY_TO_I64:
  case NY_NATIVE_VM_I64_SENARY_TO_I64:
  case NY_NATIVE_VM_I64_SEPTENARY_TO_I64:
  case NY_NATIVE_VM_I64_OCTARY_TO_I64:
  case NY_NATIVE_VM_I64_NONARY_TO_I64:
  case NY_NATIVE_VM_I64_DECENARY_TO_I64:
  case NY_NATIVE_VM_I64_UNDECENARY_TO_I64:
    return 0;
  case NY_NATIVE_VM_F64_UNARY_TO_I64:
  case NY_NATIVE_VM_F64_UNARY_TO_F64:
    return index == 0;
  case NY_NATIVE_VM_F64_BINARY_TO_F64:
    return index < 2;
  case NY_NATIVE_VM_F64_I64_TO_I64:
    return index == 0;
  case NY_NATIVE_VM_I64_I64_F64_TO_I64:
    return index == 2;
  case NY_NATIVE_VM_I64_F64_TO_I64:
    return index == 1;
  case NY_NATIVE_VM_CALL_NONE:
  case NY_NATIVE_VM_I64_UNARY_TO_I64:
  case NY_NATIVE_VM_I64_UNARY_TO_F64:
  case NY_NATIVE_VM_I64_BINARY_TO_F64:
    return 0;
  }
  return 0;
}

static inline int
ny_native_runtime_abi_arg_is_tagged(const ny_native_runtime_abi_decl_t *decl,
                                    size_t index) {
  if (!decl)
    return 0;
  if (decl->flags & NY_NATIVE_RUNTIME_ABI_TAGGED_VALUES)
    return 1;
  return index < 22 &&
         (decl->flags & NY_NATIVE_RUNTIME_ABI_TAGGED_ARG(index)) != 0;
}

static inline int ny_native_runtime_abi_accepts_bigint_values(
    const ny_native_runtime_abi_decl_t *decl) {
  return decl && (decl->flags & NY_NATIVE_RUNTIME_ABI_BIGINT_VALUES) != 0;
}

static inline int
ny_native_runtime_abi_result_is_cstr(const ny_native_runtime_abi_decl_t *decl) {
  return decl && (decl->flags & NY_NATIVE_RUNTIME_ABI_RESULT_MASK) ==
                     NY_NATIVE_RUNTIME_ABI_RESULT_CSTR;
}

static inline int ny_native_runtime_abi_result_is_bigint(
    const ny_native_runtime_abi_decl_t *decl) {
  return decl && (decl->flags & NY_NATIVE_RUNTIME_ABI_RESULT_MASK) ==
                     NY_NATIVE_RUNTIME_ABI_RESULT_BIGINT;
}

static inline int ny_native_runtime_abi_result_is_raw_word(
    const ny_native_runtime_abi_decl_t *decl) {
  return decl && (decl->flags & NY_NATIVE_RUNTIME_ABI_RESULT_MASK) ==
                     NY_NATIVE_RUNTIME_ABI_RESULT_RAW_WORD;
}

static inline int ny_native_runtime_abi_result_is_tagged_int(
    const ny_native_runtime_abi_decl_t *decl) {
  return decl && (decl->flags & NY_NATIVE_RUNTIME_ABI_RESULT_MASK) ==
                     NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_INT;
}

static inline int ny_native_runtime_abi_result_is_tagged_value(
    const ny_native_runtime_abi_decl_t *decl) {
  return decl && (decl->flags & NY_NATIVE_RUNTIME_ABI_RESULT_MASK) ==
                     NY_NATIVE_RUNTIME_ABI_RESULT_TAGGED_VALUE;
}

static inline int ny_native_runtime_abi_result_is_raw_int(
    const ny_native_runtime_abi_decl_t *decl) {
  return decl && (decl->flags & NY_NATIVE_RUNTIME_ABI_RESULT_MASK) ==
                     NY_NATIVE_RUNTIME_ABI_RESULT_RAW_INT;
}

static inline int ny_native_runtime_abi_result_is_pointer(
    const ny_native_runtime_abi_decl_t *decl) {
  return decl && (decl->flags & NY_NATIVE_RUNTIME_ABI_RESULT_MASK) ==
                     NY_NATIVE_RUNTIME_ABI_RESULT_POINTER;
}

static inline int ny_native_runtime_abi_result_is_typed_buffer(
    const ny_native_runtime_abi_decl_t *decl) {
  return decl && (decl->flags & NY_NATIVE_RUNTIME_ABI_RESULT_MASK) ==
                     NY_NATIVE_RUNTIME_ABI_RESULT_TYPED_BUFFER;
}

extern const ny_native_runtime_abi_decl_t ny_native_runtime_abi_decls[];
extern const size_t ny_native_runtime_abi_decl_count;

const ny_native_runtime_abi_decl_t *
ny_native_runtime_abi_decl(const char *symbol);

extern const ny_native_runtime_arg_abi_decl_t ny_native_runtime_arg_abi_decls[];
extern const size_t ny_native_runtime_arg_abi_decl_count;
const ny_native_runtime_arg_abi_decl_t *
ny_native_runtime_arg_abi_decl(const char *symbol);
extern const ny_native_runtime_overload_abi_decl_t
    ny_native_runtime_overload_abi_decls[];
extern const size_t ny_native_runtime_overload_abi_decl_count;
const ny_native_runtime_overload_abi_decl_t *
ny_native_runtime_overload_abi_decl(const char *symbol);
static inline bool ny_native_runtime_arg_abi_cstr_early(
    const ny_native_runtime_arg_abi_decl_t *decl, size_t index) {
  return decl && index < 16 &&
         (decl->cstr_early_mask & (uint16_t)(1u << index)) != 0;
}
static inline bool ny_native_runtime_arg_abi_single_slot(
    const ny_native_runtime_arg_abi_decl_t *decl, size_t index) {
  return decl && index < 16 &&
         (decl->single_slot_mask & (uint16_t)(1u << index)) != 0;
}
static inline bool ny_native_runtime_arg_abi_preserve_tagged(
    const ny_native_runtime_arg_abi_decl_t *decl, size_t index) {
  return decl && index < 16 &&
         (decl->preserve_tagged_mask & (uint16_t)(1u << index)) != 0;
}

extern const ny_native_runtime_value_abi_decl_t
    ny_native_runtime_value_abi_decls[];
extern const size_t ny_native_runtime_value_abi_decl_count;

const ny_native_runtime_value_abi_decl_t *
ny_native_runtime_value_abi_decl(const char *symbol);

extern const ny_native_runtime_callback_abi_decl_t
    ny_native_runtime_callback_abi_decls[];
extern const size_t ny_native_runtime_callback_abi_decl_count;

const ny_native_runtime_callback_abi_decl_t *
ny_native_runtime_callback_abi_decl(const char *symbol);
const ny_native_runtime_callback_abi_decl_t *
ny_native_runtime_callback_abi_decl_for_arity(size_t source_arity);

#endif
