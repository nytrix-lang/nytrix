/*
 * C translation-unit parser and AST construction for the standalone frontend.
 */
#include "parse.h"
#include "headers.h"

#include "base/ascii.h"
#include "base/version.h"
#include "code/lang/c/parser/c.h"
#include "code/lang/c/semantics/aggregate.h"

#include <ctype.h>
#ifdef __linux__
#include <dirent.h>
#endif
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Array helpers depend on type and array-shape declarations.
 */
#include "code/lang/c/semantics/arrays.h"
#include "code/lang/c/semantics/types.h"

typedef struct {
  ny_ctok_t name;
  c_type_t type;
  c_type_t pointer_base_type;
  unsigned pointer_depth;
  bool is_long_long;
  c_type_t function_result_type;
  c_agg_type_t *agg;
  bool is_const;
  bool is_bool;
  bool pointee_const;
  bool is_volatile;
  bool pointee_volatile;
  unsigned char pointee_seg;
  size_t pointee_array_size;
  c_array_shape_t pointee_array;
} c_param_t;

typedef struct {
  ny_ctok_t name;
  char *section;
  c_type_t result;
  c_agg_type_t *result_agg;
  bool result_is_bool;
  c_param_t *params;
  size_t param_count;
  bool variadic;
  size_t body_start;
  size_t body_end;
  bool defined;
  bool is_static;
  bool is_inline;
  bool always_inline;
  bool noinline;
  bool no_stack_protector;
  bool no_instrument_function;
  bool is_weak;
  bool no_emit;
  bool is_used;
  bool no_return;
  bool has_regparm;
  unsigned regparm;
  bool function_type_placeholder;
  bool has_asm_alias;
  ny_ctok_t asm_alias;
  ny_ctok_t alias_target;
} c_function_t;

typedef struct {
  ny_ctok_t name;
  ny_ctok_t static_name;
  ny_ctok_t cleanup;
  size_t declaration_pos;
  c_type_t type;
  c_type_t pointer_base_type;
  unsigned pointer_depth;
  bool is_long_long;
  c_type_t function_result_type;
  c_agg_type_t *agg;
  size_t slot;
  bool is_const;
  bool is_bool;
  bool has_integer_constant;
  int64_t integer_constant;
  bool pointee_const;
  bool is_volatile;
  bool address_taken;
  size_t fixed_register_marker;
  ny_ctok_t fixed_register;

  bool pointee_volatile;
  unsigned char pointee_seg;
  size_t pointee_array_size;
  c_array_shape_t pointee_array;
  int volatile_address;
  /*
   * Aggregate locals: element count for array locals, 0 for plain values.
   */
  size_t array_count;
  unsigned array_rank;
  size_t array_extent[C_MAX_ARRAY_RANK];
} c_local_t;

typedef struct {
  ny_ctok_t name;
  c_type_t type;
  bool is_void;
  c_agg_type_t *agg;
  size_t array_size;
  c_array_shape_t array_shape;
  bool is_array;
  c_type_t function_result_type;
  c_agg_type_t *function_result_agg;
  bool is_long_long;
  bool function_type;
  size_t function_signature_start;
  bool function_void_params;
  bool is_const;
  bool is_bool;
  size_t alignment;
} c_typedef_t;

typedef struct {
  ny_ctok_t name;
  int64_t value;
  c_type_t type;
} c_enum_constant_t;

typedef struct {
  ny_ctok_t name;
  bool complete;
  c_type_t underlying_type;
} c_enum_tag_t;

typedef struct {
  size_t offset;
  ny_ctok_t target;
  char *owned_target;
  int64_t addend;
  size_t width;
  unsigned type;
} c_global_reloc_t;

typedef struct {
  ny_ctok_t name;
  unsigned char *bytes;
  char *owned_name;
  char *owned_assembly;
  size_t len;
  c_global_reloc_t *relocs;
  size_t reloc_count;
  size_t reloc_capacity;
  c_type_t type;
  c_type_t pointer_base_type;
  unsigned pointer_depth;
  bool is_long_long;
  c_type_t function_result_type;
  c_agg_type_t *agg;
  bool is_array;
  bool array_incomplete;
  size_t array_rank;
  size_t array_stride;
  c_array_shape_t array_shape;
  bool is_pointer_array;
  bool is_signed_array;
  bool is_function_pointer_array;
  bool is_const;
  bool is_external;
  bool is_local;
  bool is_weak;
  bool is_used;
  ny_ctok_t owner_function;
  bool is_tentative;
  uint64_t section_flags;
  size_t alignment;
  size_t section_entsize;
  bool function_symbol;
  /*
   * Owned `__attribute__((__section__("name")))` spelling; NULL when the
   * symbol stays in the default section.
   */
  char *section;
  /*
   * Non-NULL when the object is a global register variable
   * (`register T name asm("reg")`); the raw token with quotes.
   */
  ny_ctok_t asm_register;
  bool has_asm_register;
  ny_ctok_t alias_target;
  bool is_volatile;
  /*
   * C_SEG_* qualifier on the object a global pointer addresses.
   */
  unsigned char pointee_seg;
  unsigned char object_seg;
} c_global_t;

typedef struct {
  char *name;
  size_t name_len;
  int64_t value;
  bool integer_value;
  char *replacement;
  size_t replacement_len;
} c_pp_constant_t;

typedef struct {
  ny_ctok_t name;
  size_t id;
  size_t token_pos;
  bool is_local;
  bool address_taken;
} c_label_t;

typedef struct c_cleanup_scope {
  const struct c_cleanup_scope *parent;
  size_t first_local;
  size_t end;
} c_cleanup_scope_t;

typedef struct {
  size_t token_pos;
  size_t token_after;
  size_t id;
  int64_t value;
  int64_t range_end;
  bool is_default;
  bool is_outer_label;
} c_switch_case_t;

typedef struct {
  int value;
  int address;
  c_type_t type;
  bool is_volatile;
  bool is_local;
  int local_slot;
  unsigned segment_flags;
  const c_agg_type_t *agg;
  const c_agg_field_t *bitfield;
  size_t read_start;
  size_t read_end;
} c_lvalue_t;

typedef struct {
  const char *path;
  char *owned_path;
  const char *text;
  char *owned_text;
  size_t length;
  bool once;
} c_source_file_t;

typedef struct {
  size_t token;
  size_t file;
} c_source_run_t;

typedef struct {
  ny_ctok_t *tokens;
  size_t token_count;
  size_t pos;
  c_source_file_t *files;
  size_t file_count;
  size_t file_capacity;
  c_source_run_t *source_runs;
  size_t source_run_count;
  size_t source_run_capacity;
  size_t active_file;
  const char *base_path;
  const nyc_path_map_t *path_maps;
  size_t path_map_count;
  bool unsigned_char;
  bool no_builtin;
  bool disable_sse;
  unsigned i386_regparm;
  bool ms_extensions;
  bool plan9_extensions;
  char **macro_storage;
  size_t macro_storage_count;
  size_t macro_storage_capacity;
  c_agg_type_t **tags;
  size_t tag_count;
  size_t tag_capacity;
  c_function_t *functions;
  size_t function_count;
  size_t function_capacity;
  size_t *function_index;
  size_t function_index_capacity;
  c_typedef_t *typedefs;
  size_t typedef_count;
  size_t typedef_capacity;
  size_t *typedef_index;
  size_t typedef_index_capacity;
  size_t typedef_indexed_count;
  c_enum_constant_t *enum_constants;
  size_t enum_constant_count;
  size_t enum_constant_capacity;
  c_enum_tag_t *enum_tags;
  size_t enum_tag_count;
  size_t enum_tag_capacity;
  c_global_t *globals;
  size_t global_count;
  size_t global_capacity;
  c_global_t *global_initializer;
  size_t asm_global_count;
  size_t asm_section_count;
  char *raw_file_scope_asm;
  size_t raw_file_scope_asm_len;
  size_t raw_file_scope_asm_capacity;
  bool allow_raw_file_assembly;
  c_pp_constant_t *pp_constants;
  size_t pp_constant_count;
  size_t pp_constant_capacity;
  uint64_t pp_counter;
  size_t pp_consumed_endif;
  size_t pp_pending_condition_depth;
  bool pp_pending_condition_parent[NY_C_MAX_COND_STACK];
  bool pp_pending_condition_taken[NY_C_MAX_COND_STACK];
  bool pp_pending_condition_else_seen[NY_C_MAX_COND_STACK];
  bool pp_pending_active;
  c_local_t *locals;
  size_t local_count;
  size_t local_capacity;
  size_t scope_start;
  const c_cleanup_scope_t *cleanup_scope;
  size_t param_count;
  size_t *break_target;
  size_t *continue_target;
  size_t *break_cleanup_count;
  size_t *continue_cleanup_count;
  c_switch_case_t *switch_cases;
  size_t switch_case_count;
  c_lvalue_t *lvalues;
  size_t lvalue_count;
  size_t lvalue_capacity;
  c_label_t *labels;
  size_t label_count;
  size_t tag_scope_id;
  size_t tag_scope_stack[NY_C_MAX_RECURSION_DEPTH];
  size_t tag_scope_depth;
  size_t next_tag_scope_id;
  bool last_type_const;
  bool last_type_bool;
  bool last_type_const_pointer;
  bool last_type_volatile;
  unsigned char last_type_seg;
  bool last_type_long_long;
  /*
   * Declaration lookahead classifies types without defining aggregate tags.
   */
  bool type_lookahead;
  /*
   * A sizeof probe that must build an anonymous aggregate skips assertion
   * diagnostics whose constant expression depends on full macro machinery;
   * ordinary declaration parses still evaluate them.
   */
  bool suppress_static_asserts;
  bool last_value_const;
  bool last_value_pointee_const;
  unsigned char last_value_pointee_seg;
  bool last_value_null_pointer_constant;
  bool last_value_array;
  bool global_const_unsigned;
  bool inline_asm_constant_context;
  c_type_t indirect_result_type;
  const c_agg_type_t *indirect_result_agg;
  bool has_indirect_result_signature;
  /*
   * Aggregate type referenced by the most recent parse_type result.
   */
  c_agg_type_t *last_type_agg;
  c_type_t last_type_function_result_type;
  c_agg_type_t *last_type_function_result_agg;
  bool last_type_function_type;
  bool last_type_is_function_declarator;
  bool last_type_is_function_type_alias;
  bool last_type_function_void_params;
  bool last_type_enum_incomplete;
  c_type_t last_type_base_type;
  unsigned last_type_pointer_count;
  size_t last_type_array_size;
  c_array_shape_t last_type_array;
  c_array_shape_t last_type_pointee_array;
  bool last_type_is_array;
  bool last_type_array_decayed;
  size_t last_type_alignment;
  ny_ctok_t last_type_mode;
  bool last_type_base_valid;
  /*
   * Raw section-name token from a `__attribute__((__section__("name")))`
   * parsed with the most recent declaration specifiers (quotes included;
   * cleared by every parse_type call).
   */
  ny_ctok_t last_type_section;
  ny_ctok_t last_type_cleanup;
  ny_ctok_t last_function_alias;
  /*
   * Function name behind a `typeof(fn)` base type; a declarator without a
   * parameter list (`extern typeof(f) g;`) replicates f's signature.
   */
  ny_ctok_t last_type_typeof_function;
  bool last_value_const_addr;
  /*
   * Logical slots retain one scalar class for the entire NYR function.
   * Ending a C scope removes names, not slot identities.
   */
  size_t next_local;
  int next_label;
  c_function_t *function;
  nyr_func_t *out;
  c_array_shape_t *value_array_shapes;
  const nyr_func_t **value_array_shape_owners;
  size_t value_array_shape_capacity;
  int va_save_area;
  /*
   * Track whether the current function has automatic arrays (VLAs or
   * fixed-size automatic arrays). Functions with automatic arrays cannot
   * be safely inlined due to stack slot remapping issues with ALLOCA.
   */
  bool has_auto_arrays;
  /*
   * Entry-block aggregate storage area. agg_base is the ALLOCA result value;
   * declarations and temporaries carve 8-byte-aligned chunks from it.
   */
  int agg_base;
  size_t agg_area_next;
  nyc_abi_t abi;
  unsigned optimization;
  char *err;
  size_t err_len;
} c_parser_t;

typedef struct {
  int value;
  c_type_t type;
  const c_agg_type_t *agg;
} c_value_t;

static ny_ctok_t *current(c_parser_t *p) {
  return &p->tokens[p->pos < p->token_count ? p->pos : p->token_count - 1];
}

static void fail(c_parser_t *p, const char *message) {
  if (!p->err || !p->err_len || p->err[0])
    return;
  size_t file = 0;
  for (size_t i = p->source_run_count; i > 0; --i) {
    if (p->source_runs[i - 1].token <= p->pos) {
      file = p->source_runs[i - 1].file;
      break;
    }
  }
  if (file)
    snprintf(p->err, p->err_len, "%s:%u:%u: %s", p->files[file].path,
             current(p)->line, current(p)->col, message);
  else
    snprintf(p->err, p->err_len, "%u:%u: %s", current(p)->line, current(p)->col,
             message);
}

static const c_array_shape_t *array_shape_for_value(const c_parser_t *p,
                                                    int value) {
  return p && value >= 0 && (size_t)value < p->value_array_shape_capacity &&
                 p->value_array_shape_owners[value] == p->out &&
                 p->value_array_shapes[value].rank
             ? &p->value_array_shapes[value]
             : NULL;
}

static bool remember_array_shape(c_parser_t *p, int value,
                                 c_array_shape_t shape) {
  if (value < 0 || !shape.rank)
    return true;
  size_t needed = (size_t)value + 1;
  if (needed > p->value_array_shape_capacity) {
    size_t capacity =
        p->value_array_shape_capacity ? p->value_array_shape_capacity : 32;
    while (capacity < needed) {
      if (capacity > SIZE_MAX / 2) {
        capacity = needed;
        break;
      }
      capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(*p->value_array_shapes)) {
      fail(p, "too many array-shaped C expression values");
      return false;
    }
    c_array_shape_t *shapes =
        realloc(p->value_array_shapes, capacity * sizeof(*shapes));
    if (!shapes) {
      fail(p, "out of memory retaining C array expression shape");
      return false;
    }
    const nyr_func_t **owners =
        realloc(p->value_array_shape_owners, capacity * sizeof(*owners));
    if (!owners) {
      p->value_array_shapes = shapes;
      fail(p, "out of memory retaining C array expression owners");
      return false;
    }
    memset(shapes + p->value_array_shape_capacity, 0,
           (capacity - p->value_array_shape_capacity) * sizeof(*shapes));
    memset(owners + p->value_array_shape_capacity, 0,
           (capacity - p->value_array_shape_capacity) * sizeof(*owners));
    p->value_array_shapes = shapes;
    p->value_array_shape_owners = owners;
    p->value_array_shape_capacity = capacity;
  }
  p->value_array_shapes[value] = shape;
  p->value_array_shape_owners[value] = p->out;
  return true;
}

/*
 * Scratch IR has its own value identities. A shallow parser copy must not
 * realloc the parent's expression metadata or retain shapes from an earlier
 * probe whose stack-allocated output happens to have the same address.
 */
static void expression_probe_begin(c_parser_t *probe, nyr_func_t *out) {
  nyr_func_init(out);
  probe->out = out;
  probe->lvalues = NULL;
  probe->lvalue_count = 0;
  probe->lvalue_capacity = 0;
  probe->value_array_shapes = NULL;
  probe->value_array_shape_owners = NULL;
  probe->value_array_shape_capacity = 0;
}

static void expression_probe_end(c_parser_t *probe) {
  free(probe->lvalues);
  free(probe->value_array_shapes);
  free(probe->value_array_shape_owners);
  nyr_func_destroy(probe->out);
}

/*
 * Arithmetic pointers have no aggregate type. Reuse that otherwise-empty
 * metadata slot to carry the pointee qualifier through scalar expressions.
 */
static const c_agg_type_t c_volatile_pointee_marker;
static const c_agg_type_t c_const_pointee_marker;
static const c_agg_type_t c_const_volatile_pointee_marker;

static bool apply_mode_attribute(c_parser_t *p, c_type_t *type) {
  ny_ctok_t mode = p->last_type_mode;
  if (!mode.len)
    return true;
  if (!c_type_is_integer(*type)) {
    fail(p, "integer mode attribute requires an integer base type");
    return false;
  }
  bool is_unsigned =
      *type == C_U8 || *type == C_U16 || *type == C_U32 || *type == C_U64;
  bool mode8 = (mode.len == 2 && memcmp(mode.start, "QI", 2) == 0) ||
               (mode.len == 6 && memcmp(mode.start, "__QI__", 6) == 0);
  bool mode16 = (mode.len == 2 && memcmp(mode.start, "HI", 2) == 0) ||
                (mode.len == 6 && memcmp(mode.start, "__HI__", 6) == 0);
  bool mode32 = (mode.len == 2 && memcmp(mode.start, "SI", 2) == 0) ||
                (mode.len == 6 && memcmp(mode.start, "__SI__", 6) == 0);
  bool mode64 = (mode.len == 2 && memcmp(mode.start, "DI", 2) == 0) ||
                (mode.len == 6 && memcmp(mode.start, "__DI__", 6) == 0) ||
                (mode.len == 4 && memcmp(mode.start, "word", 4) == 0) ||
                (mode.len == 8 && memcmp(mode.start, "__word__", 8) == 0);
  if (mode8)
    *type = is_unsigned ? C_U8 : C_CHAR;
  else if (mode16)
    *type = is_unsigned ? C_U16 : C_I16;
  else if (mode32)
    *type = is_unsigned ? C_U32 : C_I32;
  else if (mode64)
    *type = is_unsigned ? C_U64 : C_I64;
  else {
    fail(p, "unsupported integer mode attribute");
    return false;
  }
  p->last_type_mode = (ny_ctok_t){0};
  return true;
}

enum { C_SEG_NONE = 0, C_SEG_GS = 1, C_SEG_FS = 2 };

/*
 * Pointee qualifiers travel on the value's agg marker so that a pointer value
 * keeps its address space through casts and re-association. The markers are
 * opaque identities; only their addresses are compared.
 */
static const c_agg_type_t c_gs_pointee_marker;
static const c_agg_type_t c_fs_pointee_marker;
static const c_agg_type_t c_volatile_gs_pointee_marker;
static const c_agg_type_t c_volatile_fs_pointee_marker;
static const c_agg_type_t c_const_gs_pointee_marker;
static const c_agg_type_t c_const_fs_pointee_marker;
static const c_agg_type_t c_const_volatile_gs_pointee_marker;
static const c_agg_type_t c_const_volatile_fs_pointee_marker;

static const c_agg_type_t *c_pointee_marker(bool is_const, bool is_volatile,
                                            unsigned char seg) {
  if (seg == C_SEG_GS)
    return is_const ? (is_volatile ? &c_const_volatile_gs_pointee_marker
                                   : &c_const_gs_pointee_marker)
                    : (is_volatile ? &c_volatile_gs_pointee_marker
                                   : &c_gs_pointee_marker);
  if (seg == C_SEG_FS)
    return is_const ? (is_volatile ? &c_const_volatile_fs_pointee_marker
                                   : &c_const_fs_pointee_marker)
                    : (is_volatile ? &c_volatile_fs_pointee_marker
                                   : &c_fs_pointee_marker);
  return is_const ? (is_volatile ? &c_const_volatile_pointee_marker
                                 : &c_const_pointee_marker)
                  : (is_volatile ? &c_volatile_pointee_marker : NULL);
}

static unsigned char c_marker_seg(const c_agg_type_t *marker) {
  if (marker == &c_gs_pointee_marker ||
      marker == &c_volatile_gs_pointee_marker ||
      marker == &c_const_gs_pointee_marker ||
      marker == &c_const_volatile_gs_pointee_marker)
    return C_SEG_GS;
  if (marker == &c_fs_pointee_marker ||
      marker == &c_volatile_fs_pointee_marker ||
      marker == &c_const_fs_pointee_marker ||
      marker == &c_const_volatile_fs_pointee_marker)
    return C_SEG_FS;
  return C_SEG_NONE;
}

static unsigned c_seg_flags(unsigned char seg) {
  return seg == C_SEG_GS   ? NYIR_INST_F_MEM_SEG_GS
         : seg == C_SEG_FS ? NYIR_INST_F_MEM_SEG_FS
                           : 0;
}

static unsigned c_value_seg_flags(c_value_t value) {
  return c_seg_flags(c_marker_seg(value.agg));
}

static bool c_value_pointee_volatile(c_value_t value) {
  return value.agg == &c_volatile_pointee_marker ||
         value.agg == &c_const_volatile_pointee_marker ||
         value.agg == &c_volatile_gs_pointee_marker ||
         value.agg == &c_volatile_fs_pointee_marker ||
         value.agg == &c_const_volatile_gs_pointee_marker ||
         value.agg == &c_const_volatile_fs_pointee_marker;
}

static bool c_value_pointee_const(c_value_t value) {
  return value.agg == &c_const_pointee_marker ||
         value.agg == &c_const_volatile_pointee_marker ||
         value.agg == &c_const_gs_pointee_marker ||
         value.agg == &c_const_fs_pointee_marker ||
         value.agg == &c_const_volatile_gs_pointee_marker ||
         value.agg == &c_const_volatile_fs_pointee_marker;
}

static bool record_lvalue(c_parser_t *p, int value, int address, c_type_t type,
                          bool is_volatile) {
  if (p->lvalue_count == p->lvalue_capacity) {
    size_t next = p->lvalue_capacity ? p->lvalue_capacity * 2 : 16;
    if (next < p->lvalue_capacity || next > SIZE_MAX / sizeof(*p->lvalues)) {
      fail(p, "C frontend lvalue table capacity exceeded");
      return false;
    }
    c_lvalue_t *grown = realloc(p->lvalues, next * sizeof(*grown));
    if (!grown) {
      fail(p, "out of memory recording C lvalue");
      return false;
    }
    p->lvalues = grown;
    p->lvalue_capacity = next;
  }
  size_t read_start = SIZE_MAX;
  if (address >= 0) {
    for (size_t i = p->out->len; i > 0; --i) {
      const nyr_inst_t *inst = &p->out->data[i - 1];
      if (inst->op == NYIR_LOAD_I64 && inst->a == address) {
        read_start = i - 1;
        break;
      }
    }
  }
  /*
   * Every read of a volatile object is observable. A sub-word read may be
   * split into several loads, so mark the whole span just emitted.
   */
  if (is_volatile && read_start != SIZE_MAX)
    for (size_t i = read_start; i < p->out->len; ++i)
      if (p->out->data[i].op == NYIR_LOAD_I64) {
        p->out->data[i].flags |= NYIR_INST_F_MEM_VOLATILE;
        p->out->data[i].effects |= NYIR_EFFECT_VOLATILE;
      }
  p->lvalues[p->lvalue_count++] = (c_lvalue_t){.value = value,
                                               .address = address,
                                               .type = type,
                                               .is_volatile = is_volatile,
                                               .read_start = read_start,
                                               .read_end = p->out->len};
  return true;
}

static const c_lvalue_t *find_lvalue(const c_parser_t *p, int value);
static bool accept(c_parser_t *p, const char *text);
static bool expect(c_parser_t *p, const char *text);

/*
 * Address-only contexts do not read the final object's stored value.
 */
static bool discard_lvalue_read(c_parser_t *p, const c_lvalue_t *lvalue) {
  if (lvalue->is_local && lvalue->type == C_AGG)
    return true;
  if (lvalue->read_start == SIZE_MAX || lvalue->read_end != p->out->len)
    return false;
  for (size_t i = lvalue->read_start; i < lvalue->read_end; ++i) {
    const nyr_inst_t *inst = &p->out->data[i];
    if (inst->symbol || inst->extra_args_len || inst->dst < 0)
      return false;
    switch (inst->op) {
    case NYIR_ADDR_LOCAL:
    case NYIR_LOAD_LOCAL:
      if (!lvalue->is_local)
        return false;
      break;
    case NYIR_LOAD_I64:
    case NYIR_CONST_I64:
    case NYIR_SHL_I64:
    case NYIR_SAR_I64:
    case NYIR_AND_I64:
    case NYIR_OR_I64:
    case NYIR_COPY:
      break;
    default:
      return false;
    }
  }
  p->out->len = lvalue->read_start;
  return true;
}

static int lower_static_local_address(c_parser_t *p, const c_local_t *local);

static bool record_local_lvalue(c_parser_t *p, int value,
                                const c_local_t *local, size_t read_start) {
  int address = local->type == C_AGG     ? value
                : local->static_name.len ? lower_static_local_address(p, local)
                                         : local->volatile_address;
  if (local->static_name.len && local->type != C_AGG && address < 0)
    return false;
  if (!record_lvalue(p, value, address, local->type, local->is_volatile))
    return false;
  c_lvalue_t *lvalue = &p->lvalues[p->lvalue_count - 1];
  lvalue->is_local = true;
  lvalue->local_slot = local->slot;
  lvalue->agg = local->agg;
  if (local->type != C_AGG)
    lvalue->read_start = read_start;
  return true;
}

static c_local_t *find_local_slot(c_parser_t *p, size_t slot) {
  for (size_t i = p->local_count; i > 0; --i)
    if (p->locals[i - 1].slot == slot)
      return &p->locals[i - 1];
  return NULL;
}

static const c_lvalue_t *find_lvalue(const c_parser_t *p, int value) {
  for (size_t i = p->lvalue_count; i > 0; --i)
    if (p->lvalues[i - 1].value == value)
      return &p->lvalues[i - 1];
  return NULL;
}

static bool tok_is(const c_parser_t *p, size_t pos, const char *text) {
  return pos < p->token_count && ny_ctok_eq(p->tokens[pos], text);
}

static bool tok_is_attribute(const c_parser_t *p, size_t pos) {
  return tok_is(p, pos, "__attribute__") || tok_is(p, pos, "__attribute");
}

static bool accept_attribute(c_parser_t *p) {
  if (!tok_is_attribute(p, p->pos))
    return false;
  ++p->pos;
  return true;
}

static bool accept(c_parser_t *p, const char *text) {
  if (!tok_is(p, p->pos, text))
    return false;
  ++p->pos;
  return true;
}

static bool expect(c_parser_t *p, const char *text) {
  if (accept(p, text))
    return true;
  if (p->err && p->err_len && !p->err[0]) {
    snprintf(p->err, p->err_len, "%u:%u: expected '%s', found '%.*s'",
             current(p)->line, current(p)->col, text,
             (int)(current(p)->len > 20 ? 20 : current(p)->len),
             current(p)->start);
  }
  return false;
}

static bool tok_ident(ny_ctok_t tok) { return tok.kind == NY_CTOK_IDENT; }

static bool starts_aggregate_type(const c_parser_t *p) {
  return tok_is(p, p->pos, "struct") || tok_is(p, p->pos, "union");
}

static bool tok_same(ny_ctok_t a, ny_ctok_t b) {
  return a.len == b.len && (!a.len || memcmp(a.start, b.start, a.len) == 0);
}

static uint64_t c_name_hash(ny_ctok_t name) {
  uint64_t hash = UINT64_C(14695981039346656037);
  for (size_t i = 0; i < name.len; ++i) {
    hash ^= (unsigned char)name.start[i];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static uint64_t c_source_hash(const char *path) {
  uint64_t hash = UINT64_C(1469598103934665603);
  if (path)
    for (const unsigned char *byte = (const unsigned char *)path; *byte;
         ++byte) {
      hash ^= *byte;
      hash *= UINT64_C(1099511628211);
    }
  return hash;
}

static bool typedef_index_ensure(c_parser_t *p) {
  if (!p->typedef_count) {
    if (p->typedef_index_capacity)
      memset(p->typedef_index, 0,
             p->typedef_index_capacity * sizeof(*p->typedef_index));
    p->typedef_indexed_count = 0;
    return true;
  }
  if (p->typedef_indexed_count > p->typedef_count) {
    memset(p->typedef_index, 0,
           p->typedef_index_capacity * sizeof(*p->typedef_index));
    p->typedef_indexed_count = 0;
  }
  if (p->typedef_count > SIZE_MAX / 2)
    return false;
  size_t needed = p->typedef_count * 2;
  if (p->typedef_index_capacity < needed) {
    size_t capacity =
        p->typedef_index_capacity ? p->typedef_index_capacity : 64;
    while (capacity < needed) {
      if (capacity > SIZE_MAX / 2)
        return false;
      capacity *= 2;
    }
    size_t *slots = calloc(capacity, sizeof(*slots));
    if (!slots)
      return false;
    free(p->typedef_index);
    p->typedef_index = slots;
    p->typedef_index_capacity = capacity;
    p->typedef_indexed_count = 0;
  }
  size_t mask = p->typedef_index_capacity - 1;
  for (size_t i = p->typedef_indexed_count; i < p->typedef_count; ++i) {
    ny_ctok_t name = p->typedefs[i].name;
    size_t slot = (size_t)c_name_hash(name) & mask;
    while (p->typedef_index[slot]) {
      size_t previous = p->typedef_index[slot] - 1;
      if (tok_same(p->typedefs[previous].name, name))
        break;
      slot = (slot + 1) & mask;
    }
    p->typedef_index[slot] = i + 1;
  }
  p->typedef_indexed_count = p->typedef_count;
  return true;
}

static void typedef_index_invalidate(c_parser_t *p) {
  p->typedef_indexed_count = SIZE_MAX;
}

static c_label_t *find_label_at(c_parser_t *p, ny_ctok_t name, size_t at) {
  c_label_t *next = NULL;
  c_label_t *previous = NULL;
  for (size_t i = 0; i < p->label_count; ++i) {
    c_label_t *candidate = &p->labels[i];
    if (!tok_same(candidate->name, name))
      continue;
    if (candidate->token_pos >= at &&
        (!next || candidate->token_pos < next->token_pos))
      next = candidate;
    if (candidate->token_pos < at &&
        (!previous || candidate->token_pos > previous->token_pos))
      previous = candidate;
  }
  return next ? next : previous;
}

static bool local_shadows_name(const c_parser_t *p, ny_ctok_t name) {
  for (size_t i = p->local_count; i > 0; --i)
    if (tok_same(p->locals[i - 1].name, name))
      return true;
  return false;
}

static const c_typedef_t *find_typedef(c_parser_t *p, ny_ctok_t name) {
  if (!p->typedef_count)
    return NULL;
  if (p->typedef_index_capacity &&
      p->typedef_indexed_count == p->typedef_count) {
    size_t mask = p->typedef_index_capacity - 1;
    size_t slot = (size_t)c_name_hash(name) & mask;
    while (p->typedef_index[slot]) {
      size_t index = p->typedef_index[slot] - 1;
      const c_typedef_t *alias = &p->typedefs[index];
      if (tok_same(alias->name, name))
        return local_shadows_name(p, name) ? NULL : alias;
      slot = (slot + 1) & mask;
    }
    return NULL;
  }
  for (size_t i = p->typedef_count; i > 0; --i) {
    const c_typedef_t *alias = &p->typedefs[i - 1];
    if (tok_same(alias->name, name))
      return local_shadows_name(p, name) ? NULL : alias;
  }
  return NULL;
}

static bool starts_scalar_type(c_parser_t *p) {
  return tok_is_attribute(p, p->pos) || tok_is(p, p->pos, "static") ||
         tok_is(p, p->pos, "register") || tok_is(p, p->pos, "enum") ||
         tok_is(p, p->pos, "const") || tok_is(p, p->pos, "volatile") ||
         tok_is(p, p->pos, "__seg_gs") || tok_is(p, p->pos, "__seg_fs") ||
         tok_is(p, p->pos, "long") || tok_is(p, p->pos, "double") ||
         tok_is(p, p->pos, "float") || tok_is(p, p->pos, "_Float32") ||
         tok_is(p, p->pos, "_Complex") || tok_is(p, p->pos, "__complex__") ||
         tok_is(p, p->pos, "_Float64") || tok_is(p, p->pos, "_Float32x") ||
         tok_is(p, p->pos, "unsigned") || tok_is(p, p->pos, "signed") ||
         tok_is(p, p->pos, "short") || tok_is(p, p->pos, "char") ||
         tok_is(p, p->pos, "int") || tok_is(p, p->pos, "_Bool") ||
         tok_is(p, p->pos, "void") || tok_is(p, p->pos, "typeof") ||
         tok_is(p, p->pos, "__typeof") || tok_is(p, p->pos, "__typeof__") ||
         tok_is(p, p->pos, "__typeof_unqual__") ||
         tok_is(p, p->pos, "size_t") || tok_is(p, p->pos, "uintptr_t") ||
         tok_is(p, p->pos, "uint64_t") || tok_is(p, p->pos, "time_t") ||
         tok_is(p, p->pos, "clock_t") || tok_is(p, p->pos, "off_t") ||
         tok_is(p, p->pos, "ssize_t") || tok_is(p, p->pos, "ptrdiff_t") ||
         tok_is(p, p->pos, "intptr_t") || tok_is(p, p->pos, "int64_t") ||
         tok_is(p, p->pos, "uint32_t") || tok_is(p, p->pos, "int32_t") ||
         tok_is(p, p->pos, "__int128") || tok_is(p, p->pos, "__int128__") ||
         tok_is(p, p->pos, "__int128_t") || tok_is(p, p->pos, "__uint128_t") ||
         tok_is(p, p->pos, "va_list") ||
         (p->pos < p->token_count && tok_ident(p->tokens[p->pos]) &&
          find_typedef(p, p->tokens[p->pos]) != NULL);
}

static bool tag_scope_is_visible(const c_parser_t *p, size_t scope_id) {
  if (!scope_id || scope_id == p->tag_scope_id)
    return true;
  for (size_t i = p->tag_scope_depth; i > 0; --i)
    if (p->tag_scope_stack[i - 1] == scope_id)
      return true;
  return false;
}

static void enter_cleanup_scope(c_parser_t *p, c_cleanup_scope_t *scope,
                                size_t end) {
  *scope = (c_cleanup_scope_t){
      .parent = p->cleanup_scope, .first_local = p->local_count, .end = end};
  p->cleanup_scope = scope;
}

static bool push_tag_scope(c_parser_t *p, c_cleanup_scope_t *scope,
                           size_t end) {
  if (p->tag_scope_depth >= NY_C_MAX_RECURSION_DEPTH ||
      p->next_tag_scope_id == SIZE_MAX) {
    fail(p, "C aggregate tag scope nesting exceeds the frontend limit");
    return false;
  }
  p->tag_scope_id = ++p->next_tag_scope_id;
  p->tag_scope_stack[p->tag_scope_depth++] = p->tag_scope_id;
  enter_cleanup_scope(p, scope, end);
  return true;
}

static void pop_tag_scope(c_parser_t *p) {
  p->cleanup_scope = p->cleanup_scope->parent;
  if (p->tag_scope_depth)
    --p->tag_scope_depth;
  p->tag_scope_id =
      p->tag_scope_depth ? p->tag_scope_stack[p->tag_scope_depth - 1] : 0;
}

static c_agg_type_t *find_tag(const c_parser_t *p, ny_ctok_t name,
                              bool is_union) {
  for (size_t i = p->tag_count; i > 0; --i) {
    c_agg_type_t *tag = p->tags[i - 1];
    if (tag_scope_is_visible(p, tag->scope_id) && tag->is_union == is_union &&
        tok_same(tag->name, name))
      return tag;
  }
  return NULL;
}

static c_agg_type_t *find_tag_any(const c_parser_t *p, ny_ctok_t name) {
  for (size_t i = p->tag_count; i > 0; --i)
    if (tag_scope_is_visible(p, p->tags[i - 1]->scope_id) &&
        tok_same(p->tags[i - 1]->name, name))
      return p->tags[i - 1];
  return NULL;
}

static c_agg_type_t *complete_tag(const c_parser_t *p, c_agg_type_t *tag) {
  if (!tag || tag->complete)
    return tag;
  c_agg_type_t *complete = find_tag(p, tag->name, tag->is_union);
  return complete && complete->complete ? complete : tag;
}

static c_agg_type_t *declare_tag(c_parser_t *p, ny_ctok_t name, bool is_union) {
  c_agg_type_t *existing = find_tag(p, name, is_union);
  if (existing)
    return existing;
  c_agg_type_t *conflict = find_tag_any(p, name);
  if (conflict) {
    fail(p, "struct/union tag redeclared with a different kind");
    return NULL;
  }
  if (p->tag_count >= p->tag_capacity) {
    size_t next_capacity = p->tag_capacity ? p->tag_capacity * 2 : 16;
    c_agg_type_t **grown = realloc(p->tags, next_capacity * sizeof(*p->tags));
    if (!grown) {
      fail(p, "out of memory allocating struct tag table");
      return NULL;
    }
    p->tags = grown;
    p->tag_capacity = next_capacity;
  }
  c_agg_type_t *tag = calloc(1, sizeof(*tag));
  if (!tag) {
    fail(p, "out of memory allocating struct tag");
    return NULL;
  }
  tag->name = name;
  tag->scope_id = p->tag_scope_id;
  tag->is_union = is_union;
  p->tags[p->tag_count++] = tag;
  return tag;
}

/*
 * Shared representation for GNU signed/unsigned 128-bit integers.  A
 * complete two-word aggregate with a 16-byte alignment floor matches the
 * SysV x86-64 classification of GCC's __int128 (INTEGER,INTEGER in two
 * registers, no MEMORY,sret), and discrete low/high halves survive every
 * load/store path the pared-down frontend owns.
 */
static c_agg_type_t *arch_int128_tag(c_parser_t *p, bool is_unsigned) {
  static const char *const kinds[2] = {"unsigned", "signed"};
  (void)kinds;
  for (size_t i = 0; i < p->tag_count; ++i) {
    c_agg_type_t *tag = p->tags[i];
    if (!tag->name.len && tag->is_union == false && tag->field_count == 2) {
      bool matches =
          (is_unsigned ? tag->fields[0].kind == C_AGG_FIELD_U64 &&
                             tag->fields[1].kind == C_AGG_FIELD_U64
                       : tag->fields[0].kind == C_AGG_FIELD_I64 &&
                             tag->fields[1].kind == C_AGG_FIELD_I64) &&
          tag->size == 16 && tag->align == 16 && tag->complete;
      if (matches) {
        tag->is_int128 = true;
        return tag;
      }
    }
  }
  if (p->tag_count >= p->tag_capacity) {
    size_t next_capacity = p->tag_capacity ? p->tag_capacity * 2 : 16;
    c_agg_type_t **grown = realloc(p->tags, next_capacity * sizeof(*p->tags));
    if (!grown) {
      fail(p, "out of memory allocating struct tag table");
      return NULL;
    }
    p->tags = grown;
    p->tag_capacity = next_capacity;
  }
  c_agg_type_t *tag = calloc(1, sizeof(*tag));
  if (!tag) {
    fail(p, "out of memory allocating struct tag");
    return NULL;
  }
  tag->is_union = false;
  tag->is_int128 = true;
  tag->complete = true;
  tag->align = 16;
  tag->size = 16;
  tag->field_count = 2;
  for (unsigned half = 0; half < 2; ++half) {
    tag->fields[half] =
        (c_agg_field_t){.kind = is_unsigned ? C_AGG_FIELD_U64 : C_AGG_FIELD_I64,
                        .offset = half * 8u,
                        .size = 8,
                        .align = 8};
  }
  p->tags[p->tag_count++] = tag;
  return tag;
}

static void free_tag_table(c_parser_t *p) {
  for (size_t i = p->tag_count; i > 0; --i) {
    free(p->tags[i - 1]->promoted_fields);
    free(p->tags[i - 1]);
  }
  free(p->tags);
  p->tags = NULL;
  p->tag_count = 0;
  p->tag_capacity = 0;
}

static bool parse_name(c_parser_t *p, ny_ctok_t *name);
static bool parse_function_pointer_declarator(c_parser_t *p, ny_ctok_t *name);
static bool parse_type(c_parser_t *p, c_type_t *type, bool allow_void);
static c_value_t parse_statement_expression(c_parser_t *p);
static int lower_cleanup_edge(c_parser_t *p, const c_label_t *target);
static c_local_t *find_local(c_parser_t *p, ny_ctok_t name);
static c_global_t *find_global(c_parser_t *p, ny_ctok_t name);
static c_function_t *find_function(c_parser_t *p, ny_ctok_t name);
static const c_agg_field_t *find_field(const c_agg_type_t *agg, ny_ctok_t name);
static size_t skip_balanced(c_parser_t *p, size_t at, const char *left,
                            const char *right);
static bool parse_array_extent_expr(c_parser_t *p, int min_precedence,
                                    uint64_t *value);
static bool parse_global_const_expr(c_parser_t *p, size_t *at, int64_t *value);
static bool global_const_cast_type(c_parser_t *p, size_t open, size_t close);
static bool preproc_attribute_accepted(const char *name, size_t name_len);
static bool parse_global_float_add(c_parser_t *p, size_t *at, double *value);
static bool initialize_data_pointer(c_parser_t *p, c_global_t *data,
                                    size_t offset, unsigned char *bytes);
static bool
init_file_scalar_array_dimension(c_parser_t *p, const c_agg_field_t *field,
                                 c_global_t *global, unsigned dimension,
                                 size_t base_offset, size_t element_stride,
                                 unsigned char *bytes) {
  if (dimension >= field->array_rank || !expect(p, "{"))
    return false;
  size_t extent = field->array_extent[dimension];
  size_t stride = element_stride;
  for (unsigned d = dimension + 1; d < field->array_rank; ++d) {
    if (field->array_extent[d] && stride > SIZE_MAX / field->array_extent[d])
      return false;
    stride *= field->array_extent[d];
  }
  size_t next_index = 0;
  bool first = true;
  while (next_index < extent && !tok_is(p, p->pos, "}")) {
    if (!first && !expect(p, ","))
      return false;
    if (tok_is(p, p->pos, "}"))
      break;
    size_t first_index = next_index;
    size_t last_index = first_index;
    if (accept(p, "[")) {
      int64_t first_designator = 0;
      int64_t last_designator = 0;
      if (!parse_global_const_expr(p, &p->pos, &first_designator) ||
          first_designator < 0) {
        fail(p, "invalid flexible scalar-array designator");
        return false;
      }
      last_designator = first_designator;
      if (accept(p, "...") &&
          (!parse_global_const_expr(p, &p->pos, &last_designator) ||
           last_designator < first_designator)) {
        fail(p, "invalid flexible scalar-array range designator");
        return false;
      }
      if (!expect(p, "]") || !expect(p, "=") ||
          (uint64_t)last_designator >= extent) {
        fail(p, "flexible scalar-array designator is out of bounds");
        return false;
      }
      first_index = (size_t)first_designator;
      last_index = (size_t)last_designator;
    }
    if (dimension + 1 < field->array_rank) {
      for (size_t item = first_index; item <= last_index; ++item) {
        size_t item_offset = base_offset + item * stride;
        if (!init_file_scalar_array_dimension(p, field, global, dimension + 1,
                                              item_offset, element_stride,
                                              bytes))
          return false;
      }
    } else if (field->elem_pointer) {
      if (!global || first_index != last_index ||
          !initialize_data_pointer(
              p, global, base_offset + first_index * stride, global->bytes)) {
        if (!p->err[0])
          fail(p, "flexible pointer-array initializer requires one static "
                  "pointer value per element");
        return false;
      }
    } else if (field->elem_f32 || field->elem_f64) {
      double value;
      if (!parse_global_float_add(p, &p->pos, &value)) {
        fail(p, "invalid floating-point file-scope array initializer");
        return false;
      }
      for (size_t item = first_index; item <= last_index; ++item) {
        size_t item_offset = base_offset + item * stride;
        if (field->elem_f32) {
          float single = (float)value;
          memcpy(bytes + item_offset, &single, sizeof(single));
        } else {
          memcpy(bytes + item_offset, &value, sizeof(value));
        }
      }
    } else {
      int64_t value;
      if (!parse_global_const_expr(p, &p->pos, &value)) {
        fail(p, "unsupported constant in file-scope aggregate array");
        return false;
      }
      for (size_t item = first_index; item <= last_index; ++item) {
        size_t item_offset = base_offset + item * stride;
        memcpy(bytes + item_offset, &value, field->elem_size);
      }
    }
    next_index = last_index + 1;
    first = false;
  }
  (void)accept(p, ",");
  return expect(p, "}");
}

static c_value_t parse_expr(c_parser_t *p, int min_precedence);
static int emit_i64_constant(c_parser_t *p, int64_t value);
static int emit_i64_binary(c_parser_t *p, nyr_op_t op, int left, int right);
static int emit_i64_compare(c_parser_t *p, int left, int right,
                            nyr_cmp_t comparison);
static int emit_i64_select(c_parser_t *p, int condition, int when_true,
                           int when_false);
static int emit_value(c_parser_t *p, nyr_inst_t inst);
static bool decode_string_token(ny_ctok_t token, unsigned char **bytes,
                                size_t *len);
static bool decode_wide_string_token(ny_ctok_t token, unsigned char **bytes,
                                     size_t *byte_len);
static bool static_typeof_expr_type(c_parser_t *p, c_type_t *type,
                                    unsigned depth);
static size_t count_global_aggregate_array_initializer(const c_parser_t *p,
                                                       size_t open);
static bool resolve_static_generic_type(c_parser_t *p, c_type_t *type,
                                        unsigned depth);

typedef struct {
  ny_ctok_t name;
  uint64_t extent;
  size_t element_stride;
  unsigned array_rank;
  size_t array_extent[C_MAX_ARRAY_RANK];
  bool array;
  bool incomplete;
} c_array_declarator_t;

static bool parse_array_declarator(c_parser_t *p, c_array_declarator_t *out,
                                   unsigned depth) {
  if (depth == NY_C_MAX_RECURSION_DEPTH) {
    fail(p, "C array declarator nesting exceeds the frontend limit");
    return false;
  }
  if (accept(p, "(")) {
    if (!parse_array_declarator(p, out, depth + 1) || !expect(p, ")"))
      return false;
  } else if (!parse_name(p, &out->name)) {
    return false;
  }
  if (accept(p, "[")) {
    if (out->array) {
      fail(p, "multidimensional file-scope arrays are not supported");
      return false;
    }
    out->array = true;
    out->incomplete = accept(p, "]");
    out->array_rank = 1;
    if (!out->incomplete) {
      if (!parse_array_extent_expr(p, 1, &out->extent) || !expect(p, "]"))
        return false;
      out->array_extent[0] = (size_t)out->extent;
    }
  }
  return true;
}

static bool parse_external_integer_declarator(c_parser_t *p,
                                              c_array_declarator_t *out,
                                              size_t element_size) {
  if (!parse_name(p, &out->name))
    return false;
  size_t inner_count = 1;
  unsigned dimension = 0;
  while (accept(p, "[")) {
    if (dimension == C_MAX_ARRAY_RANK) {
      fail(p, "external integer array exceeds the frontend rank limit");
      return false;
    }
    out->array = true;
    if (accept(p, "]")) {
      if (dimension != 0) {
        fail(p,
             "only the outer external integer array extent may be incomplete");
        return false;
      }
      out->incomplete = true;
    } else {
      uint64_t extent = 0;
      if (!parse_array_extent_expr(p, 1, &extent) || (!extent && dimension) ||
          !expect(p, "]")) {
        fail(p, "external integer array requires nonnegative outer and "
                "positive inner constant extents");
        return false;
      }
      if (dimension == 0) {
        out->extent = extent;
      } else {
        if (extent > SIZE_MAX / inner_count) {
          fail(p, "external integer array row size overflows");
          return false;
        }
        inner_count *= (size_t)extent;
      }
      out->array_extent[dimension] = (size_t)extent;
    }
    ++dimension;
  }
  out->array_rank = dimension;
  if (!out->array || element_size > SIZE_MAX / inner_count) {
    fail(p, "external integer array requires a valid extent");
    return false;
  }
  out->element_stride = element_size * inner_count;
  return true;
}

static const c_enum_constant_t *find_enum_constant(const c_parser_t *p,
                                                   ny_ctok_t name) {
  for (size_t i = p->enum_constant_count; i > 0; --i)
    if (tok_same(p->enum_constants[i - 1].name, name))
      return &p->enum_constants[i - 1];
  return NULL;
}

static bool parse_function_type_attributes(c_parser_t *p);

static bool parse_enum_attributes(c_parser_t *p, bool *packed) {
  while (accept_attribute(p)) {
    if (!tok_is(p, p->pos + 2, "packed") &&
        !tok_is(p, p->pos + 2, "__packed__")) {
      p->pos -= 1;
      if (!parse_function_type_attributes(p))
        return false;
      continue;
    }
    if (!expect(p, "(") || !expect(p, "("))
      return false;
    do {
      if (tok_is(p, p->pos, "packed") || tok_is(p, p->pos, "__packed__")) {
        *packed = true;
        ++p->pos;
      } else {
        fail(p, "unsupported enum attribute");
        return false;
      }
    } while (accept(p, ","));
    if (!expect(p, ")") || !expect(p, ")"))
      return false;
  }
  return true;
}

static c_type_t packed_enum_type(int64_t minimum, int64_t maximum) {
  if (minimum >= 0) {
    if (maximum <= UINT8_MAX)
      return C_U8;
    if (maximum <= UINT16_MAX)
      return C_U16;
    return C_U32;
  }
  if (minimum >= INT8_MIN && maximum <= INT8_MAX)
    return C_CHAR;
  if (minimum >= INT16_MIN && maximum <= INT16_MAX)
    return C_I16;
  return C_I32;
}

static c_enum_tag_t *append_enum_tag(c_parser_t *p, ny_ctok_t name) {
  if (p->enum_tag_count == p->enum_tag_capacity) {
    size_t capacity = p->enum_tag_capacity ? p->enum_tag_capacity * 2 : 8;
    if (capacity < p->enum_tag_capacity ||
        capacity > SIZE_MAX / sizeof(*p->enum_tags)) {
      fail(p, "too many enum tags");
      return NULL;
    }
    c_enum_tag_t *tags = realloc(p->enum_tags, capacity * sizeof(*tags));
    if (!tags) {
      fail(p, "out of memory storing enum tags");
      return NULL;
    }
    p->enum_tags = tags;
    p->enum_tag_capacity = capacity;
  }
  c_enum_tag_t *tag = &p->enum_tags[p->enum_tag_count++];
  *tag = (c_enum_tag_t){.name = name, .underlying_type = C_I32};
  return tag;
}

static bool parse_enum_type(c_parser_t *p, c_type_t *underlying_type) {
  ++p->pos;
  ny_ctok_t name = {0};
  if (tok_ident(*current(p)))
    name = p->tokens[p->pos++];
  c_enum_tag_t *tag = NULL;
  for (size_t i = 0; name.len && i < p->enum_tag_count; ++i)
    if (tok_same(p->enum_tags[i].name, name)) {
      tag = &p->enum_tags[i];
      break;
    }
  bool packed = false;
  if (!parse_enum_attributes(p, &packed))
    return false;
  if (!accept(p, "{")) {
    if (!name.len) {
      fail(p, "enum type requires an earlier definition");
      return false;
    }
    if (!tag && !p->type_lookahead) {
      tag = append_enum_tag(p, name);
      if (!tag)
        return false;
    }
    if (!tag || !tag->complete) {
      p->last_type_enum_incomplete = true;
      if (underlying_type)
        *underlying_type = C_I32;
      return true;
    }
    if (packed)
      tag->underlying_type = packed_enum_type(INT_MIN, INT_MAX);
    if (underlying_type)
      *underlying_type = tag->underlying_type;
    return true;
  }
  if (p->type_lookahead) {
    size_t after = skip_balanced(p, p->pos - 1, "{", "}");
    if (after > p->token_count || !after)
      return false;
    p->pos = after;
    if (!parse_enum_attributes(p, &packed))
      return false;
    if (underlying_type)
      *underlying_type = tag ? tag->underlying_type : C_I32;
    return true;
  }
  if (tag && tag->complete) {
    fail(p, "duplicate enum tag definition");
    return false;
  }
  if (name.len && !tag)
    tag = append_enum_tag(p, name);
  if (name.len && !tag)
    return false;
  int64_t next = 0;
  bool next_unsigned = false;
  int64_t minimum = INT64_MAX, maximum = INT64_MIN;
  bool any = false;
  bool wide_unsigned = false;
  while (!accept(p, "}")) {
    ny_ctok_t enumerator;
    if (!parse_name(p, &enumerator))
      return false;
    if (find_enum_constant(p, enumerator) || find_global(p, enumerator) ||
        find_typedef(p, enumerator) || find_function(p, enumerator)) {
      fail(p, "duplicate enum constant");
      return false;
    }
    int64_t value = next;
    bool value_unsigned = next_unsigned;
    if (accept(p, "=")) {
      if (!parse_global_const_expr(p, &p->pos, &value)) {
        fail(p, "enum initializer requires an integer constant expression");
        return false;
      }
      value_unsigned = p->global_const_unsigned;
    }
    wide_unsigned |=
        value_unsigned && (value < INT32_MIN || value > UINT32_MAX);
    if ((value < INT32_MIN || value > UINT32_MAX) && !wide_unsigned) {
      fail(p,
           "enum constant requires a wide integer suffix outside 32-bit range");
      return false;
    }
    if (value < minimum)
      minimum = value;
    if (value > maximum)
      maximum = value;
    if (p->enum_constant_count == p->enum_constant_capacity) {
      size_t capacity =
          p->enum_constant_capacity ? p->enum_constant_capacity * 2 : 16;
      if (capacity < p->enum_constant_capacity ||
          capacity > SIZE_MAX / sizeof(*p->enum_constants)) {
        fail(p, "too many enum constants");
        return false;
      }
      c_enum_constant_t *constants =
          realloc(p->enum_constants, capacity * sizeof(*constants));
      if (!constants) {
        fail(p, "out of memory storing enum constants");
        return false;
      }
      p->enum_constants = constants;
      p->enum_constant_capacity = capacity;
    }
    c_type_t value_type = value >= INT32_MIN && value <= INT32_MAX ? C_I32
                          : value >= 0 && value <= UINT32_MAX      ? C_U32
                          : value_unsigned                         ? C_U64
                                                                   : C_I64;
    p->enum_constants[p->enum_constant_count++] = (c_enum_constant_t){
        .name = enumerator, .value = value, .type = value_type};
    any = true;
    next = (int64_t)((uint64_t)value + 1);
    next_unsigned = value_unsigned;
    if (tok_is(p, p->pos, "}"))
      continue;
    if (!expect(p, ","))
      return false;
  }
  if (!any) {
    fail(p, "empty enum definition");
    return false;
  }
  if (!parse_enum_attributes(p, &packed))
    return false;
  c_type_t type = packed          ? packed_enum_type(minimum, maximum)
                  : wide_unsigned ? C_U64
                  : minimum >= 0 && maximum > INT32_MAX        ? C_U32
                  : minimum < INT32_MIN || maximum > INT32_MAX ? C_I64
                                                               : C_I32;
  if (tag) {
    tag->complete = true;
    tag->underlying_type = type;
  }
  if (underlying_type)
    *underlying_type = type;
  return true;
}

static bool parse_builtin_offsetof_args(c_parser_t *p, uint64_t *value) {
  c_type_t type;
  if (!parse_type(p, &type, false) || type != C_AGG || !p->last_type_agg ||
      !expect(p, ","))
    return false;
  const c_agg_type_t *aggregate = p->last_type_agg;
  size_t offset = 0;
  const c_agg_field_t *field = NULL;
  for (;;) {
    ny_ctok_t member_name;
    if (!aggregate || !parse_name(p, &member_name))
      return false;
    field = find_field(aggregate, member_name);
    if (!field || field->offset > SIZE_MAX - offset)
      return false;
    offset += field->offset;
    aggregate = field->kind == C_AGG_FIELD_AGG ? field->agg : field->agg;
    unsigned array_dimension = 0;
    while (accept(p, "[")) {
      int64_t index = 0;
      if (!field->is_array || array_dimension > field->array_rank ||
          !parse_global_const_expr(p, &p->pos, &index) || index < 0 ||
          !expect(p, "]"))
        return false;
      if (field->array_extent[array_dimension] &&
          (uint64_t)index >= field->array_extent[array_dimension])
        return false;
      size_t stride = field->elem_size;
      for (unsigned d = array_dimension + 1; d < field->array_rank; ++d) {
        if (field->array_extent[d] &&
            stride > SIZE_MAX / field->array_extent[d])
          return false;
        stride *= field->array_extent[d];
      }
      if (!stride || (uint64_t)index > (SIZE_MAX - offset) / stride)
        return false;
      offset += (size_t)index * stride;
      ++array_dimension;
    }
    if (field->agg && field->agg->complete &&
        (field->kind == C_AGG_FIELD_ARRAY || array_dimension))
      aggregate = field->agg;
    if (!accept(p, "."))
      break;
    if (!aggregate || !aggregate->complete)
      return false;
  }
  (void)field;
  if (offset > INT64_MAX)
    return false;
  *value = offset;
  return true;
}

static bool parse_builtin_offsetof_runtime_args(c_parser_t *p,
                                                c_value_t *value) {
  c_type_t type;
  if (!parse_type(p, &type, false) || type != C_AGG || !p->last_type_agg ||
      !expect(p, ","))
    return false;
  const c_agg_type_t *aggregate = p->last_type_agg;
  uint64_t static_offset = 0;
  int dynamic_offset = -1;
  const c_agg_field_t *field = NULL;
  for (;;) {
    ny_ctok_t member_name;
    if (!aggregate || !parse_name(p, &member_name))
      return false;
    field = find_field(aggregate, member_name);
    if (!field || field->offset > SIZE_MAX - static_offset)
      return false;
    static_offset += field->offset;
    aggregate = field->kind == C_AGG_FIELD_AGG ? field->agg : field->agg;
    /*
     * After selecting a member, consume any array subscripts on it.
     */
    while (accept(p, "[")) {
      if (!field->is_array || !field->elem_size)
        return false;
      c_value_t index = parse_expr(p, 1);
      if (index.value < 0 || !c_type_is_integer(index.type) || !expect(p, "]"))
        return false;
      int stride = emit_i64_constant(p, (int64_t)field->elem_size);
      int scaled = stride < 0
                       ? -1
                       : emit_i64_binary(p, NYIR_MUL_I64, index.value, stride);
      if (scaled < 0)
        return false;
      if (dynamic_offset < 0) {
        dynamic_offset = scaled;
      } else {
        dynamic_offset =
            emit_i64_binary(p, NYIR_ADD_I64, dynamic_offset, scaled);
        if (dynamic_offset < 0)
          return false;
      }
      /*
       * Array element aggregate becomes the context for further member access.
       */
      if (field->agg && field->agg->complete)
        aggregate = field->agg;
    }
    if (!accept(p, "."))
      break;
    if (!aggregate || !aggregate->complete)
      return false;
  }
  (void)field;
  int offset = emit_i64_constant(p, (int64_t)static_offset);
  if (offset < 0)
    return false;
  if (dynamic_offset >= 0) {
    offset = emit_i64_binary(p, NYIR_ADD_I64, offset, dynamic_offset);
    if (offset < 0)
      return false;
  }
  *value = (c_value_t){offset, c_size_type(p->abi), NULL};
  return true;
}

static bool parse_builtin_offsetof_type_args(c_parser_t *p) {
  c_type_t type;
  if (!parse_type(p, &type, false) || type != C_AGG || !p->last_type_agg ||
      !expect(p, ","))
    return false;
  const c_agg_type_t *aggregate = p->last_type_agg;
  for (;;) {
    ny_ctok_t member_name;
    if (!aggregate || !parse_name(p, &member_name))
      return false;
    const c_agg_field_t *field = find_field(aggregate, member_name);
    if (!field)
      return false;
    aggregate = field->kind == C_AGG_FIELD_AGG ? field->agg : field->agg;
    unsigned dimension = 0;
    while (accept(p, "[")) {
      if (!field->is_array)
        return false;
      size_t depth = 1;
      while (p->pos < p->token_count && depth) {
        if (tok_is(p, p->pos, "["))
          ++depth;
        else if (tok_is(p, p->pos, "]"))
          --depth;
        if (depth)
          ++p->pos;
      }
      if (depth)
        return false;
      ++p->pos;
      ++dimension;
    }
    if (field->agg && field->agg->complete &&
        (field->kind == C_AGG_FIELD_ARRAY || dimension))
      aggregate = field->agg;
    if (!accept(p, "."))
      return true;
    if (!aggregate || !aggregate->complete)
      return false;
  }
}

static bool parse_array_extent_atom(c_parser_t *p, uint64_t *value) {
  const c_enum_constant_t *enumerator = find_enum_constant(p, *current(p));
  if (enumerator) {
    if (enumerator->value < 0)
      return false;
    *value = (uint64_t)enumerator->value;
    ++p->pos;
    return true;
  }
  if (accept(p, "!")) {
    if (!parse_array_extent_atom(p, value))
      return false;
    *value = !*value;
    return true;
  }
  if (accept(p, "~")) {
    if (!parse_array_extent_atom(p, value))
      return false;
    *value = ~*value;
    return true;
  }
  if (accept(p, "-")) {
    if (!parse_array_extent_atom(p, value))
      return false;
    *value = 0 - *value;
    return true;
  }
  if (tok_is(p, p->pos, "__builtin_choose_expr")) {
    size_t at = p->pos;
    int64_t constant = 0;
    if (!parse_global_const_expr(p, &at, &constant) || constant < 0)
      return false;
    p->pos = at;
    *value = (uint64_t)constant;
    return true;
  }
  if (accept(p, "__builtin_constant_p")) {
    if (!expect(p, "(") || !parse_array_extent_expr(p, 1, value) ||
        !expect(p, ")"))
      return false;
    *value = 1;
    return true;
  }
  if (accept(p, "__builtin_clzll") || accept(p, "__builtin_clzl")) {
    if (!expect(p, "(") || !parse_array_extent_expr(p, 1, value) ||
        !expect(p, ")") || !*value)
      return false;
    *value = (uint64_t)__builtin_clzll(*value);
    return true;
  }
  if (accept(p, "__builtin_clz")) {
    if (!expect(p, "(") || !parse_array_extent_expr(p, 1, value) ||
        !expect(p, ")") || !*value)
      return false;
    *value = (uint64_t)__builtin_clz((uint32_t)*value);
    return true;
  }
  if (accept(p, "__builtin_ctz") || accept(p, "__builtin_ctzl") ||
      accept(p, "__builtin_ctzll")) {
    if (!expect(p, "("))
      return false;
    size_t argument = p->pos;
    int64_t constant = 0;
    if (!parse_global_const_expr(p, &argument, &constant))
      return false;
    p->pos = argument;
    if (!expect(p, ")") || !constant)
      return false;
    *value = (uint64_t)__builtin_ctzl((unsigned long)constant);
    return true;
  }
  unsigned bswap_width = accept(p, "__builtin_bswap16")   ? 16
                         : accept(p, "__builtin_bswap32") ? 32
                         : accept(p, "__builtin_bswap64") ? 64
                                                          : 0;
  if (bswap_width) {
    if (!expect(p, "(") || !parse_array_extent_expr(p, 1, value) ||
        !expect(p, ")"))
      return false;
    if (bswap_width == 16)
      *value = __builtin_bswap16((uint16_t)*value);
    else if (bswap_width == 32)
      *value = __builtin_bswap32((uint32_t)*value);
    else
      *value = __builtin_bswap64(*value);
    p->global_const_unsigned = bswap_width != 16;
    return true;
  }
  if (accept(p, "__ilog2_u32") || accept(p, "__ilog2_u64")) {
    bool is_32 = ny_ctok_eq(p->tokens[p->pos - 1], "__ilog2_u32");
    if (!expect(p, "(") || !parse_array_extent_expr(p, 1, value) ||
        !expect(p, ")"))
      return false;
    if (is_32) {
      uint32_t narrowed = (uint32_t)*value;
      *value = narrowed ? 31u - (uint64_t)__builtin_clz(narrowed) : 0;
    } else {
      *value = *value ? 63u - (uint64_t)__builtin_clzll(*value) : 0;
    }
    return true;
  }
  if (accept(p, "__bits_per") || accept(p, "bits_per")) {
    uint64_t operand = 0;
    if (!expect(p, "(") || !parse_array_extent_expr(p, 1, &operand) ||
        !expect(p, ")"))
      return false;
    *value = operand <= 1 ? 1 : 64 - (uint64_t)__builtin_clzll(operand);
    return true;
  }
  if (accept(p, "__roundup_pow_of_two")) {
    uint64_t operand = 0;
    if (!expect(p, "(") || !parse_array_extent_expr(p, 1, &operand) ||
        !expect(p, ")"))
      return false;
    if (operand <= 1) {
      *value = 1;
    } else {
      unsigned shift = 64u - (unsigned)__builtin_clzll(operand - 1);
      if (shift >= 64)
        return false;
      *value = UINT64_C(1) << shift;
    }
    p->global_const_unsigned = true;
    return true;
  }
  if (accept(p, "&")) {
    size_t arrow = p->pos;
    while (arrow < p->token_count && !tok_is(p, arrow, "->") &&
           !tok_is(p, arrow, "]"))
      ++arrow;
    if (arrow >= p->token_count || !tok_is(p, arrow, "->") ||
        arrow + 1 >= p->token_count)
      return false;
    size_t star = arrow;
    while (star > p->pos && !tok_is(p, star - 1, "*"))
      --star;
    if (star == p->pos)
      return false;
    ny_ctok_t tag_name = p->tokens[star - 2];
    c_agg_type_t *tag = tok_ident(tag_name) ? find_tag_any(p, tag_name) : NULL;
    c_agg_type_t *complete = tag ? complete_tag(p, tag) : NULL;
    const c_agg_field_t *field =
        complete ? find_field(complete, p->tokens[arrow + 1]) : NULL;
    if (!field)
      return false;
    *value = field->offset;
    p->pos = arrow + 2;
    return true;
  }
  if (accept(p, "__builtin_offsetof")) {
    if (!expect(p, "(") || !parse_builtin_offsetof_args(p, value) ||
        !expect(p, ")"))
      return false;
    return true;
  }
  bool is_alignof = accept(p, "_Alignof") || accept(p, "__alignof__") ||
                    accept(p, "__alignof");
  if (is_alignof || accept(p, "sizeof")) {
    size_t expression_open = p->pos;
    bool parenthesized = accept(p, "(");
    size_t expression_after =
        parenthesized ? skip_balanced(p, expression_open, "(", ")") : SIZE_MAX;
    if (!is_alignof && parenthesized &&
        p->tokens[p->pos].kind == NY_CTOK_STRING) {
      size_t literal_size = 0;
      bool have_string = false;
      while (p->tokens[p->pos].kind == NY_CTOK_STRING) {
        unsigned char *decoded = NULL;
        size_t decoded_size = 0;
        if (!decode_string_token(p->tokens[p->pos++], &decoded,
                                 &decoded_size)) {
          free(decoded);
          return false;
        }
        free(decoded);
        size_t content_size = decoded_size ? decoded_size - 1 : 0;
        if (content_size > SIZE_MAX - literal_size - 1)
          return false;
        literal_size += content_size;
        have_string = true;
      }
      if (have_string && accept(p, ")")) {
        *value = (int64_t)(literal_size + 1);
        return true;
      }
      return false;
    }
    c_parser_t probe = *p;
    char probe_error[1] = {0};
    probe.err = probe_error;
    probe.err_len = sizeof(probe_error);
    bool anonymous_aggregate =
        parenthesized &&
        (tok_is(p, p->pos, "struct") || tok_is(p, p->pos, "union")) &&
        tok_is(p, p->pos + 1, "{");
    c_agg_type_t **scratch_tags = NULL;
    size_t scratch_tag_count = p->tag_count;
    if (anonymous_aggregate) {
      size_t capacity = p->tag_count + 1;
      scratch_tags = malloc(capacity * sizeof(*scratch_tags));
      if (!scratch_tags)
        return false;
      if (p->tag_count)
        memcpy(scratch_tags, p->tags, p->tag_count * sizeof(*scratch_tags));
      probe.tags = scratch_tags;
      probe.tag_capacity = capacity;
      probe.type_lookahead = false;
    } else {
      probe.type_lookahead = true;
    }
    c_type_t type;
    bool parsed_type = parse_type(&probe, &type, false);
    size_t aggregate_size = probe.last_type_agg ? probe.last_type_agg->size : 0;
    size_t aggregate_alignment =
        probe.last_type_agg ? probe.last_type_agg->align : 0;
    size_t type_alignment = probe.last_type_alignment;
    size_t parsed_position = probe.pos;
    if (scratch_tags) {
      for (size_t i = scratch_tag_count; i < probe.tag_count; ++i) {
        free(probe.tags[i]->promoted_fields);
        free(probe.tags[i]);
      }
      free(probe.tags);
    }
    if (parsed_type && parenthesized && !is_alignof &&
        tok_is(&probe, probe.pos, "[")) {
      size_t array_size =
          type == C_AGG ? aggregate_size : c_type_size(p->abi, type);
      bool valid_array = array_size != 0;
      while (valid_array && accept(&probe, "[")) {
        uint64_t extent = 0;
        if (!parse_array_extent_expr(&probe, 1, &extent) ||
            !expect(&probe, "]") || extent > SIZE_MAX / array_size) {
          valid_array = false;
          break;
        }
        array_size *= (size_t)extent;
      }
      if (valid_array && array_size && array_size <= INT64_MAX &&
          probe.pos + 1 == expression_after && tok_is(&probe, probe.pos, ")")) {
        *value = (int64_t)array_size;
        p->pos = probe.pos + 1;
        return true;
      }
    }
    if (parsed_type && (!parenthesized || (probe.pos + 1 == expression_after &&
                                           tok_is(&probe, probe.pos, ")")))) {
      if (parenthesized)
        ++parsed_position;
      if (type == C_AGG) {
        if (!aggregate_alignment)
          return false;
        *value = is_alignof ? aggregate_alignment : aggregate_size;
      } else if (is_alignof && type_alignment) {
        *value = type_alignment;
      } else if (is_alignof) {
        *value = c_type_alignment(p->abi, type);
        if (!*value)
          return false;
      } else if (type == C_U8 || type == C_CHAR) {
        *value = 1;
      } else if (type == C_I16 || type == C_U16) {
        *value = 2;
      } else if (type == C_I32 || type == C_U32 || type == C_F32) {
        *value = 4;
      } else if (type == C_I64 || type == C_U64 || type == C_F64 ||
                 c_type_is_pointer(type)) {
        *value = c_type_size(p->abi, type);
      } else {
        return false;
      }
      p->pos = parsed_position;
      return true;
    }
    if (parenthesized) {
      probe = *p;
      size_t leading_parentheses = 0;
      while (accept(&probe, "("))
        ++leading_parentheses;
      if (leading_parentheses) {
        c_agg_type_t *object_aggregate = NULL;
        if (tok_is(&probe, probe.pos, "struct") ||
            tok_is(&probe, probe.pos, "union")) {
          ++probe.pos;
          ny_ctok_t tag_name;
          if (parse_name(&probe, &tag_name))
            for (size_t i = 0; i < probe.tag_count; ++i)
              if (tok_same(probe.tags[i]->name, tag_name)) {
                object_aggregate = probe.tags[i];
                break;
              }
        }
        while (accept(&probe, "*"))
          ;
        if (object_aggregate && expect(&probe, ")") &&
            tok_is(&probe, probe.pos, "0")) {
          ++probe.pos;
          bool grouping_closed = true;
          for (size_t i = 1; i < leading_parentheses; ++i)
            grouping_closed &= expect(&probe, ")");
          if (grouping_closed) {
            bool arrow = accept(&probe, "->");
            if (!arrow)
              (void)accept(&probe, ".");
            ny_ctok_t member_name;
            if (!parse_name(&probe, &member_name))
              return false;
            const c_agg_field_t *field =
                find_field(object_aggregate, member_name);
            if (field && expect(&probe, ")") && probe.pos == expression_after) {
              *value = field->size;
              p->pos = probe.pos;
              return *value != 0;
            }
          }
        }
      }
      probe = *p;
      if (tok_ident(*current(&probe))) {
        ny_ctok_t object_name = *current(&probe);
        c_local_t *local = find_local(&probe, object_name);
        if (local) {
          ++probe.pos;
          size_t object_size =
              local->array_rank
                  ? local->array_count * c_type_size(p->abi, local->type)
              : local->type == C_AGG && local->agg
                  ? local->agg->size
                  : c_type_size(p->abi, local->type);
          const c_agg_type_t *member_aggregate = local->agg;
          while (tok_is(&probe, probe.pos, ".") ||
                 tok_is(&probe, probe.pos, "->")) {
            if (accept(&probe, "->")) {
              if (local->type != C_AGG_PTR && local->type != C_AGG_PTR_PTR &&
                  local->type != C_AGG_ARRAY_PTR && !member_aggregate)
                return false;
            } else {
              (void)accept(&probe, ".");
            }
            ny_ctok_t member_name;
            if (!parse_name(&probe, &member_name))
              return false;
            const c_agg_field_t *field =
                member_aggregate ? find_field(member_aggregate, member_name)
                                 : NULL;
            if (!field)
              return false;
            object_size = field->size;
            member_aggregate = field->agg;
          }
          if (tok_is(&probe, probe.pos, "[") && local->array_count) {
            unsigned remaining_rank =
                local->array_rank ? (unsigned)local->array_rank : 1;
            object_size = local->type == C_AGG && local->agg
                              ? local->agg->size
                              : c_type_size(p->abi, local->type);
            while (tok_is(&probe, probe.pos, "[")) {
              ++probe.pos;
              uint64_t element_index = UINT64_MAX;
              if (!remaining_rank ||
                  !parse_array_extent_atom(&probe, &element_index) ||
                  element_index != 0 || !expect(&probe, "]"))
                return false;
              --remaining_rank;
            }
            if (local->array_rank) {
              for (unsigned dimension =
                       (unsigned)local->array_rank - remaining_rank;
                   dimension < local->array_rank; ++dimension) {
                size_t extent = local->array_extent[dimension];
                if (!extent || object_size > SIZE_MAX / extent)
                  return false;
                object_size *= extent;
              }
            }
          }
          if (!expect(&probe, ")") || probe.pos != expression_after ||
              (!object_size && !local->array_rank))
            return false;
          *value = object_size;
          p->pos = probe.pos;
          return true;
        }
        c_global_t *global = find_global(&probe, object_name);
        if (global) {
          ++probe.pos;
          size_t object_size =
              global->is_array ? global->len
              : global->type == C_AGG && global->agg && global->agg->complete
                  ? global->agg->size
                  : c_type_size(p->abi, global->type);
          if (tok_is(&probe, probe.pos, "[") && global->is_array) {
            ++probe.pos;
            uint64_t element_index = UINT64_MAX;
            if (!parse_array_extent_atom(&probe, &element_index) ||
                element_index != 0 || !expect(&probe, "]"))
              return false;
            object_size = global->type == C_AGG && global->agg
                              ? global->agg->size
                              : c_type_size(p->abi, global->type);
          }
          if (global->type == C_AGG && global->agg &&
              (tok_is(&probe, probe.pos, ".") ||
               tok_is(&probe, probe.pos, "->"))) {
            const c_agg_type_t *member_aggregate = global->agg;
            while (tok_is(&probe, probe.pos, ".") ||
                   tok_is(&probe, probe.pos, "->")) {
              (void)accept(&probe, "->");
              (void)accept(&probe, ".");
              ny_ctok_t member_name;
              if (!parse_name(&probe, &member_name))
                return false;
              const c_agg_field_t *field =
                  member_aggregate ? find_field(member_aggregate, member_name)
                                   : NULL;
              if (!field)
                return false;
              object_size = field->size;
              member_aggregate = field->agg;
            }
          }
          if (!expect(&probe, ")") || probe.pos != expression_after ||
              !object_size)
            return false;
          *value = object_size;
          p->pos = probe.pos;
          return true;
        }
      }
      probe = *p;
      c_type_t array_type = C_BAD;
      if (static_typeof_expr_type(&probe, &array_type, 0) &&
          probe.pos + 1 == expression_after && tok_is(&probe, probe.pos, ")") &&
          probe.last_type_is_array && !probe.last_type_array_decayed &&
          probe.last_type_array_size) {
        *value = (int64_t)probe.last_type_array_size;
        p->pos = expression_after;
        return true;
      }
      probe = *p;
      if (accept(&probe, "*")) {
        nyr_func_t pointer_out;
        expression_probe_begin(&probe, &pointer_out);
        char pointer_error[1] = {0};
        probe.err = pointer_error;
        probe.err_len = sizeof(pointer_error);
        probe.type_lookahead = true;
        c_value_t pointer = parse_expr(&probe, 1);
        if (pointer.type != C_BAD && c_type_is_pointer(pointer.type) &&
            probe.pos + 1 == expression_after &&
            tok_is(&probe, probe.pos, ")")) {
          c_type_t pointee = c_remove_pointer_levels(pointer.type, 1);
          size_t pointee_size = pointer.type == C_VOID_PTR ? 1
                                : pointee == C_AGG && pointer.agg
                                    ? pointer.agg->size
                                    : c_type_size(p->abi, pointee);
          expression_probe_end(&probe);
          if (pointee_size) {
            *value = (int64_t)pointee_size;
            p->pos = probe.pos + 1;
            return true;
          }
        } else {
          expression_probe_end(&probe);
        }
      }
      probe = *p;
      c_type_t static_expression_type = C_BAD;
      if (static_typeof_expr_type(&probe, &static_expression_type, 0) &&
          probe.pos + 1 == expression_after && tok_is(&probe, probe.pos, ")")) {
        bool empty_gnu_aggregate =
            static_expression_type == C_AGG && probe.last_type_agg &&
            probe.last_type_agg->complete && !probe.last_type_agg->size;
        if (empty_gnu_aggregate) {
          *value = 0;
          p->pos = probe.pos + 1;
          return true;
        }
      }
      probe = *p;
      nyr_func_t expression_out;
      expression_probe_begin(&probe, &expression_out);
      char expression_error[1] = {0};
      probe.err = expression_error;
      probe.err_len = sizeof(expression_error);
      probe.type_lookahead = true;
      c_value_t expression_type = parse_expr(&probe, 1);
      if (expression_type.type != C_BAD && probe.pos + 1 == expression_after &&
          tok_is(&probe, probe.pos, ")")) {
        size_t expression_size =
            expression_type.type == C_AGG && expression_type.agg
                ? expression_type.agg->size
                : c_type_size(p->abi, expression_type.type);
        expression_probe_end(&probe);
        if (expression_size) {
          *value = (int64_t)expression_size;
          p->pos = probe.pos + 1;
          return true;
        }
      } else {
        expression_probe_end(&probe);
      }
      probe = *p;
      uint64_t ignored = 0;
      if (parse_array_extent_expr(&probe, 1, &ignored) && expect(&probe, ")") &&
          probe.pos == expression_after) {
        bool has_long_integer = false;
        for (size_t i = p->pos; i < probe.pos; ++i) {
          if (p->tokens[i].kind != NY_CTOK_NUMBER)
            continue;
          for (size_t j = 0; j < p->tokens[i].len; ++j)
            has_long_integer |=
                p->tokens[i].start[j] == 'l' || p->tokens[i].start[j] == 'L';
        }
        *value = has_long_integer ? sizeof(unsigned long) : sizeof(int);
        p->pos = probe.pos;
        return true;
      }
    }
    return false;
  }
  if (accept(p, "__builtin_constant_p")) {
    if (!expect(p, "("))
      return false;
    c_parser_t probe = *p;
    uint64_t ignored = 0;
    bool is_constant =
        parse_array_extent_expr(&probe, 1, &ignored) && expect(&probe, ")");
    if (!is_constant) {
      size_t end = skip_balanced(p, p->pos - 1, "(", ")");
      if (end >= p->token_count)
        return false;
      p->pos = end;
    } else {
      p->pos = probe.pos;
    }
    *value = is_constant;
    return true;
  }
  bool ilog2_u32 = accept(p, "__ilog2_u32");
  bool ilog2_u64 = !ilog2_u32 && accept(p, "__ilog2_u64");
  bool order_base_2 = !ilog2_u32 && !ilog2_u64 && accept(p, "__order_base_2");
  if (ilog2_u32 || ilog2_u64 || order_base_2) {
    if (!expect(p, "(") || !parse_array_extent_expr(p, 1, value) ||
        !expect(p, ")") || !*value)
      return false;
    unsigned width = ilog2_u32 ? 32 : 64;
    if (order_base_2) {
      if (*value <= 1) {
        *value = 0;
        return true;
      }
      --*value;
    }
    unsigned long long operand =
        width == 32 ? (uint32_t)*value : (unsigned long long)*value;
    *value = width == 32 ? 32u - (uint64_t)__builtin_clz((uint32_t)operand)
                         : 64u - (uint64_t)__builtin_clzll(operand);
    if (!order_base_2)
      --*value;
    return true;
  }
  bool clz_long_long = accept(p, "__builtin_clzll");
  bool clz_long = !clz_long_long && accept(p, "__builtin_clzl");
  bool clz_int = !clz_long_long && !clz_long && accept(p, "__builtin_clz");
  if (clz_long_long || clz_long || clz_int) {
    if (!expect(p, "(") || !parse_array_extent_expr(p, 1, value) ||
        !expect(p, ")") || !*value)
      return false;
    unsigned width =
        clz_long_long ? 64
        : clz_long    ? (unsigned)(c_type_size(p->abi, c_size_type(p->abi)) * 8)
                      : 32;
    if (width == 32)
      *value = (uint64_t)__builtin_clz((uint32_t)*value);
    else if (width == 64)
      *value = (uint64_t)__builtin_clzll(*value);
    else
      return false;
    return true;
  }
  if (tok_is(p, p->pos, "(")) {
    c_parser_t probe = *p;
    char probe_error[1] = {0};
    probe.err = probe_error;
    probe.err_len = sizeof(probe_error);
    probe.type_lookahead = true;
    ++probe.pos;
    c_type_t cast_type;
    if (parse_type(&probe, &cast_type, false) &&
        tok_is(&probe, probe.pos, ")")) {
      ++probe.pos;
      p->pos = probe.pos;
      if (!c_type_is_integer(cast_type) || !parse_array_extent_atom(p, value))
        return false;
      size_t cast_size = c_type_size(p->abi, cast_type);
      if (!cast_size || cast_size > sizeof(*value))
        return false;
      if (cast_size < sizeof(*value)) {
        unsigned bits = (unsigned)(cast_size * 8u);
        uint64_t mask = (UINT64_C(1) << bits) - 1u;
        *value &= mask;
        bool is_signed = cast_type == C_CHAR || cast_type == C_I16 ||
                         cast_type == C_I32 || cast_type == C_I64;
        if (is_signed && (*value & (UINT64_C(1) << (bits - 1u))))
          *value |= ~mask;
      }
      return true;
    }
  }
  if (accept(p, "(")) {
    if (!parse_array_extent_expr(p, 1, value) || !expect(p, ")"))
      return false;
    return true;
  }
  if (accept(p, "+"))
    return parse_array_extent_atom(p, value);
  if (accept(p, "-")) {
    if (!parse_array_extent_atom(p, value))
      return false;
    *value = UINT64_C(0) - *value;
    return true;
  }
  if (accept(p, "~")) {
    if (!parse_array_extent_atom(p, value))
      return false;
    *value = ~*value;
    return true;
  }
  if (current(p)->kind != NY_CTOK_NUMBER)
    return false;
  ny_ctok_t token = *current(p);
  char buffer[64];
  if (!token.len || token.len >= sizeof(buffer))
    return false;
  memcpy(buffer, token.start, token.len);
  buffer[token.len] = '\0';
  char *end = NULL;
  errno = 0;
  unsigned long long parsed = strtoull(buffer, &end, 0);
  if (errno || end == buffer)
    return false;
  while (*end == 'u' || *end == 'U' || *end == 'l' || *end == 'L')
    ++end;
  if (*end)
    return false;
  *value = (uint64_t)parsed;
  ++p->pos;
  return true;
}

static int array_extent_precedence(c_parser_t *p) {
  if (tok_is(p, p->pos, "||"))
    return 1;
  if (tok_is(p, p->pos, "&&"))
    return 2;
  if (tok_is(p, p->pos, "<") || tok_is(p, p->pos, "<=") ||
      tok_is(p, p->pos, ">") || tok_is(p, p->pos, ">=") ||
      tok_is(p, p->pos, "==") || tok_is(p, p->pos, "!="))
    return 6;
  if (tok_is(p, p->pos, "|"))
    return 3;
  if (tok_is(p, p->pos, "^"))
    return 4;
  if (tok_is(p, p->pos, "&"))
    return 5;
  if (tok_is(p, p->pos, "+") || tok_is(p, p->pos, "-"))
    return 7;
  if (tok_is(p, p->pos, "<<") || tok_is(p, p->pos, ">>"))
    return 8;
  if (tok_is(p, p->pos, "*") || tok_is(p, p->pos, "/") ||
      tok_is(p, p->pos, "%"))
    return 9;
  return 0;
}

static bool parse_array_extent_expr(c_parser_t *p, int min_precedence,
                                    uint64_t *value) {
  if (!parse_array_extent_atom(p, value))
    return false;
  for (;;) {
    int precedence = array_extent_precedence(p);
    if (precedence < min_precedence || precedence == 0) {
      if (min_precedence == 1 && accept(p, "?")) {
        uint64_t condition = *value;
        uint64_t when_true = condition, when_false = 0;
        if ((!accept(p, ":") &&
             (!parse_array_extent_expr(p, 1, &when_true) || !expect(p, ":"))) ||
            !parse_array_extent_expr(p, 1, &when_false))
          return false;
        *value = condition ? when_true : when_false;
      }
      return true;
    }
    ny_ctok_t op = *current(p);
    ++p->pos;
    uint64_t rhs = 0;
    if (!parse_array_extent_expr(p, precedence + 1, &rhs))
      return false;
    if (ny_ctok_eq(op, "||")) {
      *value = *value != 0 || rhs != 0;
    } else if (ny_ctok_eq(op, "&&")) {
      *value = *value != 0 && rhs != 0;
    } else if (ny_ctok_eq(op, "+")) {
      if (UINT64_MAX - *value < rhs)
        return false;
      *value += rhs;
    } else if (ny_ctok_eq(op, "-")) {
      *value -= rhs;
    } else if (ny_ctok_eq(op, "*")) {
      if (rhs && *value > UINT64_MAX / rhs)
        return false;
      *value *= rhs;
    } else if (ny_ctok_eq(op, "<<")) {
      if (rhs >= 64 || *value > (UINT64_MAX >> rhs))
        return false;
      *value <<= (unsigned)rhs;
    } else if (ny_ctok_eq(op, ">>")) {
      if (rhs >= 64)
        return false;
      *value >>= (unsigned)rhs;
    } else if (ny_ctok_eq(op, "<")) {
      *value = *value < rhs;
    } else if (ny_ctok_eq(op, "<=")) {
      *value = *value <= rhs;
    } else if (ny_ctok_eq(op, ">")) {
      *value = *value > rhs;
    } else if (ny_ctok_eq(op, ">=")) {
      *value = *value >= rhs;
    } else if (ny_ctok_eq(op, "==")) {
      *value = *value == rhs;
    } else if (ny_ctok_eq(op, "!=")) {
      *value = *value != rhs;
    } else if (ny_ctok_eq(op, "&")) {
      *value &= rhs;
    } else if (ny_ctok_eq(op, "^")) {
      *value ^= rhs;
    } else if (ny_ctok_eq(op, "|")) {
      *value |= rhs;
    } else if (ny_ctok_eq(op, "/")) {
      if (!rhs)
        return false;
      *value /= rhs;
    } else {
      if (!rhs)
        return false;
      *value %= rhs;
    }
  }
}

/*
 * Parse one field declarator inside a tag body. The field's type has already
 * been consumed; this parses optional pointer stars, the field name, and an
 * optional constant array extent.
 */
static bool parse_field_declarator(c_parser_t *p, c_type_t type,
                                   c_agg_type_t *type_agg,
                                   unsigned inherited_pointers,
                                   bool pointer_to_aggregate_array,
                                   c_agg_field_t *field) {
  size_t pointer_size = c_type_size(p->abi, C_VOID_PTR);
  field->pointer_element_type = C_BAD;
  field->pointer_to_aggregate_array = pointer_to_aggregate_array;
  size_t inherited_array_size = p->last_type_array_size;
  if (p->last_type_is_array) {
    field->is_array = true;
    field->array_rank = 1;
    field->array_extent[0] = inherited_array_size;
  }
  field->is_signed_byte = type == C_CHAR || type == C_I8_PTR;
  field->is_volatile = p->last_type_volatile;
  ny_ctok_t name;
  if (tok_is(p, p->pos, "(") && tok_is(p, p->pos + 1, "*")) {
    size_t name_pos = p->pos + 1;
    while (tok_is(p, name_pos, "*"))
      ++name_pos;
    if (name_pos > p->pos + 2 && tok_ident(p->tokens[name_pos]) &&
        tok_is(p, name_pos + 1, ")") && tok_is(p, name_pos + 2, "(")) {
      name = p->tokens[name_pos];
      size_t close = skip_balanced(p, name_pos + 2, "(", ")");
      if (close >= p->token_count || !tok_is(p, close, ";")) {
        fail(p, "unterminated nested C function-pointer field");
        return false;
      }
      p->pos = close;
      field->name = name;
      field->elem_count = 1;
      field->kind = C_AGG_FIELD_PTR;
      field->agg = NULL;
      field->elem_pointer = true;
      field->elem_function_pointer = true;
      field->function_result_type =
          c_add_pointer_levels(type, inherited_pointers);
      field->function_result_agg =
          field->function_result_type == C_AGG ||
                  field->function_result_type == C_AGG_PTR ||
                  field->function_result_type == C_AGG_PTR_PTR
              ? type_agg
              : NULL;
      field->has_function_signature = true;
      field->elem_f64 = false;
      field->size = pointer_size;
      field->align = pointer_size;
      return true;
    }
  }
  if (p->pos + 4 < p->token_count && tok_is(p, p->pos, "(") &&
      tok_is(p, p->pos + 1, "*") && tok_is(p, p->pos + 2, "(") &&
      tok_is(p, p->pos + 3, "*") && tok_ident(p->tokens[p->pos + 4])) {
    name = p->tokens[p->pos + 4];
    size_t depth = 0;
    while (p->pos < p->token_count) {
      if (tok_is(p, p->pos, "("))
        ++depth;
      else if (tok_is(p, p->pos, ")")) {
        if (depth == 0)
          break;
        --depth;
      } else if (tok_is(p, p->pos, ";") && depth == 0) {
        break;
      }
      ++p->pos;
    }
    if (!tok_is(p, p->pos, ";")) {
      fail(p, "unterminated nested C function-pointer field");
      return false;
    }
    field->name = name;
    field->elem_count = 1;
    field->kind = C_AGG_FIELD_PTR;
    field->agg = NULL;
    field->is_function_pointer = true;
    field->function_result_type = C_FUNC_PTR;
    field->function_result_agg = NULL;
    field->has_function_signature = true;
    field->elem_f64 = false;
    field->size = pointer_size;
    field->align = pointer_size;
    return true;
  }
  if (tok_is(p, p->pos, "(") && tok_is(p, p->pos + 1, "*")) {
    size_t name_pos = p->pos + 1;
    unsigned pointer_depth = 0;
    while (tok_is(p, name_pos, "*")) {
      ++pointer_depth;
      ++name_pos;
    }
    if (pointer_depth == 1 && tok_ident(p->tokens[name_pos]) &&
        tok_is(p, name_pos + 1, ")") && tok_is(p, name_pos + 2, "[")) {
      name = p->tokens[name_pos];
      size_t array_size = type == C_AGG ? (type_agg ? type_agg->size : 0)
                                        : c_type_size(p->abi, type);
      unsigned array_rank = 0;
      p->pos = name_pos + 2;
      do {
        ++p->pos;
        if (accept(p, "]")) {
          if (type != C_AGG || !type_agg || !type_agg->complete ||
              tok_is(p, p->pos, "[")) {
            fail(p, "incomplete pointer-to-array field requires one "
                    "aggregate dimension");
            return false;
          }
          field->pointer_to_aggregate_array = true;
          array_rank = 1;
          array_size = 0;
          break;
        }
        uint64_t extent = 0;
        if (array_rank == C_AGG_MAX_ARRAY_RANK ||
            !parse_array_extent_expr(p, 1, &extent) || extent > SIZE_MAX ||
            !expect(p, "]")) {
          fail(p, "pointer-to-array field requires constant extents");
          return false;
        }
        field->array_extent[array_rank++] = (size_t)extent;
        if (extent && array_size > SIZE_MAX / (size_t)extent) {
          fail(p, "pointer-to-array field size overflows");
          return false;
        }
        array_size *= (size_t)extent;
      } while (accept(p, "["));
      field->name = name;
      field->elem_count = 1;
      field->kind = C_AGG_FIELD_PTR;
      field->agg = type == C_AGG ? type_agg : NULL;
      field->pointer_depth = 1;
      field->pointer_element_type = type;
      field->elem_size = array_size;
      field->pointer_to_array = true;
      field->array_rank = array_rank;
      field->elem_pointer = c_type_is_pointer(type);
      field->elem_unsigned =
          type == C_U8 || type == C_U16 || type == C_U32 || type == C_U64;
      field->size = pointer_size;
      field->align = pointer_size;
      return true;
    }
  }
  if (parse_function_pointer_declarator(p, &name)) {
    field->name = name;
    field->elem_count = 1;
    field->kind = C_AGG_FIELD_PTR;
    field->agg = NULL;
    field->is_function_pointer = true;
    field->function_result_type =
        c_add_pointer_levels(type, inherited_pointers);
    field->function_result_agg =
        field->function_result_type == C_AGG ||
                field->function_result_type == C_AGG_PTR ||
                field->function_result_type == C_AGG_PTR_PTR
            ? type_agg
            : NULL;
    field->has_function_signature = true;
    field->elem_f64 = false;
    field->size = pointer_size;
    field->align = pointer_size;
    return true;
  }
  unsigned pointers = inherited_pointers;
  while (accept(p, "*"))
    ++pointers;
  field->pointer_depth = pointers;
  field->pointee_const = pointers && p->last_type_const;
  if (type == C_BAD && pointers == 0) {
    fail(p, "void struct fields are unsupported");
    return false;
  }
  if (tok_is(p, p->pos, ":")) {
    /*
     * C permits unnamed integer bit-fields for explicit layout padding.
     */
    name = (ny_ctok_t){0};
  } else if (!parse_name(p, &name)) {
    fail(p, "expected struct field name");
    return false;
  }
  size_t count = field->is_array ? inherited_array_size : 1;
  if (accept(p, "[")) {
    field->is_array = true;
    for (;;) {
      if (field->array_rank >= C_AGG_MAX_ARRAY_RANK) {
        fail(p, "aggregate array field exceeds the frontend rank limit");
        return false;
      }
      if (accept(p, "]")) {
        field->array_extent[field->array_rank++] = 0;
        count = 0;
        if (accept(p, "["))
          continue;
        break;
      }
      uint64_t parsed = 0;
      if (!parse_array_extent_expr(p, 1, &parsed) || parsed > SIZE_MAX) {
        fail(p, "array fields require a constant extent");
        return false;
      }
      size_t extent = (size_t)parsed;
      field->array_extent[field->array_rank++] = extent;
      if (!extent || !count) {
        count = 0;
      } else if (count > SIZE_MAX / extent) {
        fail(p, "aggregate array field has too many elements");
        return false;
      } else {
        count *= extent;
      }
      if (!expect(p, "]"))
        return false;
      if (!accept(p, "["))
        break;
    }
    if (pointers > 3) {
      fail(
          p,
          "struct pointer-array elements support at most three pointer levels");
      return false;
    }
  }
  if (type == C_VA_LIST && pointers == 0 && !field->is_array) {
    /*
     * SysV x86-64 va_list is an array containing a 24-byte descriptor.
     */
    field->name = name;
    field->is_array = true;
    field->array_rank = 1;
    field->array_extent[0] = 1;
    field->kind = C_AGG_FIELD_ARRAY;
    field->elem_pointer = false;
    field->elem_f64 = false;
    field->elem_size = 24;
    field->elem_count = 1;
    field->size = 24;
    field->align = 8;
    return true;
  }
  field->name = name;
  field->is_bool = p->last_type_bool && pointers == 0 && !field->is_array;
  field->elem_count = count;
  if (pointer_to_aggregate_array && pointers == 1 && !field->is_array) {
    field->kind = C_AGG_FIELD_PTR;
    field->agg = type == C_AGG ? type_agg : NULL;
    field->pointer_depth = 1;
    field->elem_size = inherited_array_size;
    field->size = pointer_size;
    field->align = pointer_size;
    return true;
  }
  if (pointers == 1 && !field->is_array &&
      p->last_type_is_function_type_alias) {
    field->kind = C_AGG_FIELD_PTR;
    field->agg = NULL;
    field->is_function_pointer = true;
    field->function_result_type = p->last_type_function_result_type;
    field->function_result_agg = p->last_type_function_result_agg;
    field->has_function_signature = true;
    field->size = pointer_size;
    field->align = pointer_size;
    return true;
  }
  if (pointers == 1) {
    field->kind = field->is_array ? C_AGG_FIELD_ARRAY : C_AGG_FIELD_PTR;
    field->agg = type == C_AGG ? type_agg : NULL;
    field->elem_f32 = type == C_F32;
    field->elem_f64 = type == C_F64;
    field->elem_unsigned =
        type == C_U8 || type == C_U16 || type == C_U32 || type == C_U64;
    field->elem_pointer = field->is_array || c_type_is_pointer(type);
    field->elem_function_pointer = type == C_FUNC_PTR;
    field->elem_pointer |= field->elem_function_pointer;
    field->pointer_element_type =
        field->is_array ? c_add_pointer_levels(type, 1) : type;
    field->elem_size = field->is_array                  ? pointer_size
                       : type == C_U8 || type == C_CHAR ? 1
                       : type == C_I16 || type == C_U16 ? 2
                       : type == C_I32 || type == C_U32 || type == C_F32 ? 4
                                                                         : 8;
    field->size = pointer_size * count;
    field->align = pointer_size;
    return true;
  }
  if (c_type_is_pointer(type)) {
    field->kind = field->is_array ? C_AGG_FIELD_ARRAY : C_AGG_FIELD_PTR;
    field->agg = type == C_AGG_PTR ? type_agg : NULL;
    field->elem_f64 = false;
    field->elem_pointer = field->is_array || pointers > 1 ||
                          type == C_CHAR_PTR_PTR || type == C_AGG_PTR_PTR ||
                          type == C_FUNC_PTR_PTR || type == C_OPAQUE_PTR ||
                          type == C_VOID_PTR;
    field->elem_function_pointer =
        type == C_FUNC_PTR_PTR || (field->is_array && type == C_FUNC_PTR);
    field->is_function_pointer = type == C_FUNC_PTR && !field->is_array;
    if (field->is_function_pointer && p->last_type_function_type) {
      field->function_result_type = p->last_type_function_result_type;
      field->function_result_agg = p->last_type_function_result_agg;
      field->has_function_signature =
          p->last_type_function_result_type != C_BAD;
    }
    field->elem_char_pointer = type == C_CHAR_PTR_PTR;
    field->elem_size = field->elem_pointer                      ? pointer_size
                       : type == C_CHAR_PTR || type == C_I8_PTR ? 1
                       : type == C_I32_PTR || type == C_U32_PTR ? 4
                                                                : 8;
    field->elem_count = count;
    field->pointer_element_type = type;
    field->size = pointer_size * count;
    field->align = pointer_size;
    return true;
  }
  if (pointers >= 2) {
    c_type_t pointer_type = pointers >= 3
                                ? C_OPAQUE_PTR_PTR_PTR
                                : c_add_pointer_levels(type, pointers);
    if (pointer_type == C_BAD) {
      fail(p, "unsupported pointer depth in C aggregate field");
      return false;
    }
    field->kind = field->is_array ? C_AGG_FIELD_ARRAY : C_AGG_FIELD_PTR;
    field->agg = type == C_AGG ? type_agg : NULL;
    field->pointer_element_type =
        field->is_array ? pointer_type
                        : c_add_pointer_levels(type, pointers - 1);
    field->elem_pointer = c_type_is_pointer(pointer_type);
    field->elem_char_pointer = pointer_type == C_CHAR_PTR_PTR;
    field->elem_function_pointer = pointer_type == C_FUNC_PTR_PTR;
    field->elem_size = pointer_size;
    field->elem_count = count;
    field->size = pointer_size * count;
    field->align = pointer_size;
    return true;
  }
  if (pointers > 1) {
    fail(p, "only single-level pointers are supported");
    return false;
  }
  if (type == C_AGG) {
    if (!type_agg || !type_agg->complete) {
      fail(p, "field type must be a complete struct or union");
      return false;
    }
    field->agg = type_agg;
    field->elem_f64 = false;
    field->size = type_agg->size * count;
    field->align = type_agg->align;
    field->elem_size = type_agg->size;
    field->kind = field->is_array ? C_AGG_FIELD_ARRAY : C_AGG_FIELD_AGG;
    return true;
  }
  if (type == C_I32 || type == C_U32 || type == C_F32) {
    field->agg = NULL;
    field->elem_f64 = false;
    field->size = 4 * count;
    field->align = 4;
    field->elem_size = 4;
    field->elem_f32 = type == C_F32;
    field->elem_unsigned = type == C_U32;
    field->kind = field->is_array ? C_AGG_FIELD_ARRAY
                  : type == C_F32 ? C_AGG_FIELD_F32
                  : type == C_U32 ? C_AGG_FIELD_U32
                                  : C_AGG_FIELD_I32;
    return true;
  }
  if (type == C_U8 || type == C_CHAR) {
    field->agg = NULL;
    field->elem_f64 = false;
    field->size = count;
    field->align = 1;
    field->elem_size = 1;
    field->kind = field->is_array ? C_AGG_FIELD_ARRAY : C_AGG_FIELD_U8;
    return true;
  }
  if (type == C_I16 || type == C_U16) {
    field->agg = NULL;
    field->elem_f64 = false;
    field->elem_unsigned = type == C_U16;
    field->size = 2 * count;
    field->align = 2;
    field->elem_size = 2;
    field->kind = field->is_array
                      ? C_AGG_FIELD_ARRAY
                      : (type == C_U16 ? C_AGG_FIELD_U16 : C_AGG_FIELD_I16);
    return true;
  }
  if (c_type_is_complex(type)) {
    if (count != 1) {
      fail(p, "arrays of complex struct fields are not supported yet");
      return false;
    }
    field->agg = NULL;
    field->elem_f64 = false;
    field->elem_unsigned = false;
    field->size = type == C_C64 ? 8 : type == C_C128 ? 16 : 32;
    field->align = type == C_C64 ? 4 : type == C_C128 ? 8 : 16;
    field->elem_size = field->size;
    field->kind = type == C_C64    ? C_AGG_FIELD_C64
                  : type == C_C128 ? C_AGG_FIELD_C128
                                   : C_AGG_FIELD_C_LDOUBLE;
    return true;
  }
  if (type != C_I64 && type != C_U64 && type != C_F64) {
    fail(p, "struct fields must be long, double, pointer, struct, or union");
    return false;
  }
  field->agg = NULL;
  field->elem_f64 = type == C_F64;
  field->size = 8 * count;
  field->align = c_type_alignment(p->abi, type);
  field->elem_size = 8;
  field->elem_unsigned = type == C_U64;
  field->kind = field->is_array ? C_AGG_FIELD_ARRAY
                : type == C_F64 ? C_AGG_FIELD_F64
                : type == C_U64 ? C_AGG_FIELD_U64
                                : C_AGG_FIELD_I64;
  return true;
}

static bool copy_quoted_token(c_parser_t *p, ny_ctok_t token, char *out,
                              size_t out_len);

/*
 * A token list that can start a type (C23/GNU typeof support): keywords,
 * aggregate/enum leads, qualifiers, or a typedef name.
 */
static bool token_leads_type(c_parser_t *p, ny_ctok_t token) {
  if (!tok_ident(token))
    return false;
  static const char *const starts[] = {"void",       "char",
                                       "short",      "int",
                                       "long",       "float",
                                       "double",     "_Float32",
                                       "_Float64",   "_Float32x",
                                       "signed",     "unsigned",
                                       "_Bool",      "struct",
                                       "union",      "enum",
                                       "const",      "volatile",
                                       "register",   "restrict",
                                       "__restrict", "__restrict__",
                                       "typeof",     "__typeof",
                                       "__typeof__", "__typeof_unqual__",
                                       "extern",     "static",
                                       "inline",     "__attribute__",
                                       "__attribute"};
  for (size_t i = 0; i < sizeof(starts) / sizeof(*starts); ++i)
    if (ny_ctok_eq(token, starts[i]))
      return true;
  return find_typedef(p, token) != NULL;
}

/*
 * GNU/C23 typeof: `typeof (<type> | <expr>)` adopts the resolved type.
 * Inner forms mirror kernel usage — a type, a typedef, a declared function
 * (whose type adopts the result and whose name is carried on the parser so
 * `typeof(f) g;` replicates f's signature), file-scope data, body locals,
 * enum/preprocessor constants, and integer/char literals. Anything else
 * fails with an explicit unsupported-expression diagnostic.
 */
static bool parse_typeof_type(c_parser_t *p, c_type_t *type) {
  ++p->pos;
  if (!expect(p, "("))
    return false;
  if (token_leads_type(p, *current(p))) {
    if (!parse_type(p, type, true))
      return false;
    c_array_shape_t array = {.element_type = *type,
                             .element_size = *type == C_AGG && p->last_type_agg
                                                 ? p->last_type_agg->size
                                                 : c_type_size(p->abi, *type),
                             .agg = p->last_type_agg};
    size_t total_size = array.element_size;
    while (accept(p, "[")) {
      uint64_t extent = 0;
      if (array.rank == C_MAX_ARRAY_RANK ||
          !parse_array_extent_expr(p, 1, &extent) || !extent ||
          !expect(p, "]") || !array.element_size ||
          extent > SIZE_MAX / total_size) {
        fail(p, "typeof array type requires valid constant extents");
        return false;
      }
      array.extent[array.rank++] = (size_t)extent;
      total_size *= (size_t)extent;
    }
    if (array.rank) {
      p->last_type_array = array;
      p->last_type_array_size = total_size;
      p->last_type_is_array = true;
      p->last_type_array_decayed = false;
    }
    /*
     * The recursive parse owns the type metadata; a following declarator
     * continues from it through parse_type's tail.
     */
    return expect(p, ")");
  }
  if (tok_is(p, p->pos, "_Generic")) {
    if (!static_typeof_expr_type(p, type, 1))
      return false;
    /*
     * the resolver consumed the _Generic call; the typeof's own closer remains
     */
    return expect(p, ")");
  }
  if (tok_is(p, p->pos, "(") || tok_is(p, p->pos, "*") ||
      tok_is(p, p->pos, "&") || tok_is(p, p->pos, "+") ||
      tok_is(p, p->pos, "-") || tok_is(p, p->pos, "!") ||
      tok_is(p, p->pos, "~")) {
    if (!static_typeof_expr_type(p, type, 1))
      return false;
    return expect(p, ")");
  }
  ny_ctok_t token = *current(p);
  if (tok_ident(token) && !tok_is(p, p->pos + 1, ")")) {
    if (!static_typeof_expr_type(p, type, 1))
      return false;
    return expect(p, ")");
  }
  if (tok_ident(token)) {
    ++p->pos;
    if (!expect(p, ")"))
      return false;
    c_function_t *fn = find_local(p, token) ? NULL : find_function(p, token);
    if (fn) {
      *type = fn->result;
      p->last_type_agg = fn->result_agg;
      p->last_type_base_type = fn->result;
      p->last_type_base_valid = true;
      p->last_type_typeof_function = token;
      p->last_type_function_result_type = fn->result;
      p->last_type_function_result_agg = fn->result_agg;
      p->last_type_function_void_params = fn->param_count == 0 && !fn->variadic;
      p->last_type_function_type = true;
      return true;
    }
    c_local_t *local = find_local(p, token);
    if (local) {
      *type = local->type;
      p->last_type_long_long = local->is_long_long;
      p->last_type_agg = local->agg;
      p->last_type_base_type = local->type;
      p->last_type_base_valid = true;
      p->last_type_const = c_type_is_pointer(local->type) ? local->pointee_const
                                                          : local->is_const;
      p->last_type_const_pointer =
          c_type_is_pointer(local->type) && local->is_const;
      p->last_type_volatile = local->is_volatile;
      p->last_type_function_result_type = local->function_result_type;
      p->last_type_function_result_agg = local->agg;
      p->last_type_function_type = local->type == C_FUNC_PTR;
      return true;
    }
    c_global_t *global = find_global(p, token);
    if (global) {
      *type = global->type;
      p->last_type_long_long = global->is_long_long;
      p->last_type_agg = global->agg;
      p->last_type_base_type = global->type;
      p->last_type_base_valid = true;
      p->last_type_array_size = global->is_array ? global->len : 0;
      p->last_type_is_array = global->is_array;
      p->last_type_array = global->array_shape;
      p->last_type_array_decayed = false;
      p->last_type_const = global->is_const;
      p->last_type_const_pointer =
          c_type_is_pointer(global->type) && global->is_const;
      p->last_type_function_result_type = global->function_result_type;
      p->last_type_function_result_agg = global->agg;
      p->last_type_function_type = global->type == C_FUNC_PTR;
      return true;
    }
    const c_enum_constant_t *enumerator = find_enum_constant(p, token);
    if (enumerator) {
      *type = enumerator->type;
      p->last_type_base_type = *type;
      p->last_type_base_valid = true;
      return true;
    }
    for (size_t i = p->pp_constant_count; i > 0; --i) {
      if (strlen(p->pp_constants[i - 1].name) == token.len &&
          memcmp(p->pp_constants[i - 1].name, token.start, token.len) == 0) {
        *type = p->pp_constants[i - 1].integer_value ? C_I64 : C_I32;
        p->last_type_base_type = *type;
        p->last_type_base_valid = true;
        return true;
      }
    }
    size_t detail_len = token.len < 80 ? token.len : 80;
    char detail[256];
    snprintf(detail, sizeof(detail),
             "typeof symbol `%.80s` has no declared type in the current "
             "scope; only already-declared functions, data, locals, enum, "
             "and preprocessor constants are supported",
             detail_len ? token.start : "");
    fail(p, detail);
    return false;
  }
  if (token.kind == NY_CTOK_NUMBER || token.kind == NY_CTOK_CHAR) {
    if (!static_typeof_expr_type(p, type, 1) || !expect(p, ")"))
      return false;
    return true;
  }
  if (token.kind == NY_CTOK_STRING) {
    unsigned char *decoded = NULL;
    size_t decoded_len = 0;
    if (!decode_string_token(token, &decoded, &decoded_len) || !decoded_len) {
      free(decoded);
      fail(p, "invalid string literal in typeof expression");
      return false;
    }
    free(decoded);
    ++p->pos;
    if (!expect(p, ")"))
      return false;
    *type = C_CHAR;
    p->last_type_agg = NULL;
    p->last_type_array_size = decoded_len;
    p->last_type_is_array = true;
    p->last_type_array_decayed = false;
    p->last_type_pointer_count = 0;
    p->last_type_const = false;
    p->last_type_volatile = false;
    p->last_type_base_type = C_CHAR;
    p->last_type_base_valid = true;
    return true;
  }
  fail(p, "unsupported typeof expression");
  return false;
}

/*
 * Skip a balanced expression argument (the value part of a _Generic
 * selector or a call argument) and leave the cursor on the depth-0 `,`
 * or `)` that ends it, without consuming either.
 */
static bool skip_to_comma_or_close(c_parser_t *p) {
  int depth = 0;
  for (;;) {
    if (p->pos >= p->token_count) {
      fail(p, "unterminated typeof argument expression");
      return false;
    }
    if (tok_is(p, p->pos, "(") || tok_is(p, p->pos, "[") ||
        tok_is(p, p->pos, "{")) {
      ++depth;
    } else if (tok_is(p, p->pos, ")") || tok_is(p, p->pos, "]") ||
               tok_is(p, p->pos, "}")) {
      if (!depth)
        return true;
      --depth;
    } else if (!depth && tok_is(p, p->pos, ",")) {
      return true;
    }
    ++p->pos;
  }
}

/*
 * Recursive typeof operands must not replace a cast's declared type shape.
 */
typedef struct {
  c_agg_type_t *agg;
  c_type_t function_result_type;
  c_agg_type_t *function_result_agg;
  bool function_type;
  bool is_function_declarator;
  bool is_function_type_alias;
  bool function_void_params;
  bool enum_incomplete;
  c_type_t base_type;
  unsigned pointer_count;
  size_t array_size;
  c_array_shape_t array;
  c_array_shape_t pointee_array;
  bool is_array;
  bool array_decayed;
  size_t alignment;
  ny_ctok_t mode;
  bool base_valid;
  ny_ctok_t section;
  ny_ctok_t cleanup;
  ny_ctok_t typeof_function;
  bool is_const;
  bool is_bool;
  bool const_pointer;
  bool is_volatile;
  bool long_long;
} c_type_parse_state_t;

static inline c_type_parse_state_t save_type_parse_state(const c_parser_t *p) {
  return (c_type_parse_state_t){
      .agg = p->last_type_agg,
      .function_result_type = p->last_type_function_result_type,
      .function_result_agg = p->last_type_function_result_agg,
      .function_type = p->last_type_function_type,
      .is_function_declarator = p->last_type_is_function_declarator,
      .is_function_type_alias = p->last_type_is_function_type_alias,
      .function_void_params = p->last_type_function_void_params,
      .enum_incomplete = p->last_type_enum_incomplete,
      .base_type = p->last_type_base_type,
      .pointer_count = p->last_type_pointer_count,
      .array_size = p->last_type_array_size,
      .array = p->last_type_array,
      .pointee_array = p->last_type_pointee_array,
      .is_array = p->last_type_is_array,
      .array_decayed = p->last_type_array_decayed,
      .alignment = p->last_type_alignment,
      .mode = p->last_type_mode,
      .base_valid = p->last_type_base_valid,
      .section = p->last_type_section,
      .cleanup = p->last_type_cleanup,
      .typeof_function = p->last_type_typeof_function,
      .is_const = p->last_type_const,
      .is_bool = p->last_type_bool,
      .const_pointer = p->last_type_const_pointer,
      .is_volatile = p->last_type_volatile,
      .long_long = p->last_type_long_long,
  };
}

static inline void restore_type_parse_state(c_parser_t *p,
                                            const c_type_parse_state_t *s) {
  p->last_type_agg = s->agg;
  p->last_type_function_result_type = s->function_result_type;
  p->last_type_function_result_agg = s->function_result_agg;
  p->last_type_function_type = s->function_type;
  p->last_type_is_function_declarator = s->is_function_declarator;
  p->last_type_is_function_type_alias = s->is_function_type_alias;
  p->last_type_function_void_params = s->function_void_params;
  p->last_type_enum_incomplete = s->enum_incomplete;
  p->last_type_base_type = s->base_type;
  p->last_type_pointer_count = s->pointer_count;
  p->last_type_array_size = s->array_size;
  p->last_type_array = s->array;
  p->last_type_pointee_array = s->pointee_array;
  p->last_type_is_array = s->is_array;
  p->last_type_array_decayed = s->array_decayed;
  p->last_type_alignment = s->alignment;
  p->last_type_mode = s->mode;
  p->last_type_base_valid = s->base_valid;
  p->last_type_section = s->section;
  p->last_type_cleanup = s->cleanup;
  p->last_type_typeof_function = s->typeof_function;
  p->last_type_const = s->is_const;
  p->last_type_bool = s->is_bool;
  p->last_type_const_pointer = s->const_pointer;
  p->last_type_volatile = s->is_volatile;
  p->last_type_long_long = s->long_long;
}

/*
 * Static type resolution for a parenthesized GNU typeof argument: walks
 * the argument tokens without emitting instructions and reports the
 * resulting value type. Kernel headers rely on `typeof(*pos)`,
 * `typeof((unsigned long *)addr)`, `typeof(_Generic(x, ...))`, and
 * friends; only static-shape operands are supported, other forms fail
 * closed with an explicit diagnostic.
 */
static bool static_typeof_statement_expression(c_parser_t *p, c_type_t *type,
                                               unsigned depth) {
  c_parser_t probe = *p;
  nyr_func_t probe_out;
  expression_probe_begin(&probe, &probe_out);
  char probe_error[256] = {0};
  probe.err = probe_error;
  probe.err_len = sizeof(probe_error);
  probe.type_lookahead = true;
  ++probe.pos; /* parse_statement_expression starts at the opening brace */
  c_value_t result = parse_statement_expression(&probe);
  if ((result.value >= 0 && result.type != C_BAD) ||
      (result.value < 0 && result.type == C_BAD && !probe_error[0])) {
    *type = result.type;
    p->pos = probe.pos;
    p->last_type_agg = (c_agg_type_t *)result.agg;
    p->last_type_array_size = probe.last_type_array_size;
    p->last_type_array = probe.last_type_array;
    p->last_type_pointee_array = probe.last_type_pointee_array;
    const c_array_shape_t *result_shape =
        array_shape_for_value(&probe, result.value);
    if (result_shape && c_type_is_pointer(result.type))
      p->last_type_pointee_array = *result_shape;
    p->last_type_is_array = probe.last_type_is_array;
    p->last_type_array_decayed = probe.last_type_array_decayed;
    p->last_type_base_type = probe.last_type_base_type;
    p->last_type_base_valid = probe.last_type_base_valid;
    p->last_type_const = probe.last_type_const;
    p->last_type_volatile = probe.last_type_volatile;
    p->last_type_function_type = probe.last_type_function_type;
    p->last_type_function_result_type = probe.last_type_function_result_type;
    p->last_type_function_result_agg = probe.last_type_function_result_agg;
    p->last_type_function_void_params = probe.last_type_function_void_params;
    p->last_type_typeof_function = probe.last_type_typeof_function;
    expression_probe_end(&probe);
    return true;
  }
  expression_probe_end(&probe);

  size_t body_open = p->pos + 1;
  size_t after_body = skip_balanced(p, body_open, "{", "}");
  if (after_body >= p->token_count || !tok_is(p, after_body, ")") ||
      after_body == body_open + 1)
    return false;

  size_t body_start = body_open + 1;
  size_t body_end = after_body - 1;
  size_t previous_semicolon = SIZE_MAX;
  size_t last_semicolon = SIZE_MAX;
  size_t paren_depth = 0;
  size_t bracket_depth = 0;
  size_t brace_depth = 0;
  for (size_t i = body_start; i < body_end; ++i) {
    if (tok_is(p, i, "("))
      ++paren_depth;
    else if (tok_is(p, i, ")") && paren_depth)
      --paren_depth;
    else if (tok_is(p, i, "["))
      ++bracket_depth;
    else if (tok_is(p, i, "]") && bracket_depth)
      --bracket_depth;
    else if (tok_is(p, i, "{"))
      ++brace_depth;
    else if (tok_is(p, i, "}") && brace_depth)
      --brace_depth;
    else if (tok_is(p, i, ";") && !paren_depth && !bracket_depth &&
             !brace_depth) {
      previous_semicolon = last_semicolon;
      last_semicolon = i;
    }
  }
  if (last_semicolon == SIZE_MAX)
    return false;

  bool terminated = last_semicolon == body_end - 1;
  size_t expression_start =
      terminated ? (previous_semicolon == SIZE_MAX ? body_start
                                                   : previous_semicolon + 1)
                 : last_semicolon + 1;
  size_t expression_end = terminated ? last_semicolon : body_end;
  if (expression_start >= expression_end)
    return false;
  if (terminated &&
      (tok_is(p, expression_start, "{") || tok_is(p, expression_start, "if") ||
       tok_is(p, expression_start, "switch") ||
       tok_is(p, expression_start, "while") ||
       tok_is(p, expression_start, "do") ||
       tok_is(p, expression_start, "for") ||
       tok_is(p, expression_start, "return") ||
       tok_is(p, expression_start, "break") ||
       tok_is(p, expression_start, "continue") ||
       tok_is(p, expression_start, "goto"))) {
    *type = C_BAD;
    p->pos = after_body + 1;
    return true;
  }

  p->pos = expression_start;
  if (!static_typeof_expr_type(p, type, depth + 1) || p->pos != expression_end)
    return false;
  p->pos = after_body + 1;
  return true;
}

static bool static_typeof_expr_type_impl(c_parser_t *p, c_type_t *type,
                                         unsigned depth, bool unary_only);

static bool static_typeof_expr_type(c_parser_t *p, c_type_t *type,
                                    unsigned depth) {
  return static_typeof_expr_type_impl(p, type, depth, false);
}

static bool static_typeof_is_void_null_pointer(const c_parser_t *p,
                                               size_t start, size_t end) {
  while (start < end && tok_is(p, start, "(")) {
    size_t depth = 0;
    size_t close = SIZE_MAX;
    for (size_t i = start; i < end; ++i) {
      if (tok_is(p, i, "("))
        ++depth;
      else if (tok_is(p, i, ")")) {
        if (!depth)
          return false;
        if (--depth == 0) {
          close = i;
          break;
        }
      }
    }
    if (close != end - 1)
      break;
    ++start;
    --end;
  }
  return end == start + 5 && tok_is(p, start, "(") &&
         tok_is(p, start + 1, "void") && tok_is(p, start + 2, "*") &&
         tok_is(p, start + 3, ")") && tok_is(p, start + 4, "0");
}

static bool static_typeof_expr_type_impl(c_parser_t *p, c_type_t *type,
                                         unsigned depth, bool unary_only) {
  if (depth > NY_C_MAX_RECURSION_DEPTH) {
    fail(p, "C typeof argument nesting exceeds the frontend limit");
    return false;
  }
  /*
   * Each recursive operand has its own type metadata.
   */
  p->last_type_agg = NULL;
  p->last_type_bool = false;
  p->last_type_is_array = false;
  p->last_type_array_decayed = false;
  p->last_type_array = (c_array_shape_t){0};
  p->last_type_array_size = 0;
  bool from_function = false;
  while (tok_is(p, p->pos, "+") || tok_is(p, p->pos, "-") ||
         tok_is(p, p->pos, "!") || tok_is(p, p->pos, "~"))
    ++p->pos;
  if (tok_is(p, p->pos, "__builtin_offsetof")) {
    c_parser_t probe = *p;
    ++probe.pos;
    if (!expect(&probe, "(") || !parse_builtin_offsetof_type_args(&probe) ||
        !expect(&probe, ")"))
      return false;
    p->pos = probe.pos;
    *type = c_size_type(p->abi);
    p->last_type_agg = NULL;
    p->last_type_is_array = false;
    p->last_type_array_decayed = false;
    p->last_type_base_type = *type;
    p->last_type_base_valid = true;
  } else if (tok_is(p, p->pos, "sizeof") || tok_is(p, p->pos, "_Alignof") ||
             tok_is(p, p->pos, "__alignof__") ||
             tok_is(p, p->pos, "__alignof")) {
    ++p->pos;
    bool parenthesized = accept(p, "(");
    if (parenthesized && token_leads_type(p, *current(p))) {
      c_type_t operand_type;
      if (!parse_type(p, &operand_type, false) || !expect(p, ")"))
        return false;
    } else {
      c_type_t operand_type;
      if (!static_typeof_expr_type(p, &operand_type, depth + 1) ||
          (parenthesized && !expect(p, ")")))
        return false;
    }
    *type = c_size_type(p->abi);
    p->last_type_agg = NULL;
    p->last_type_is_array = false;
    p->last_type_array_decayed = false;
    p->last_type_base_type = *type;
    p->last_type_base_valid = true;
  } else if (tok_is(p, p->pos, "typeof") || tok_is(p, p->pos, "__typeof__") ||
             tok_is(p, p->pos, "__typeof") ||
             tok_is(p, p->pos, "__typeof_unqual__")) {
    /*
     * A typeof construct is itself a type-valued expression, so it can be
     * the operand of an enclosing typeof; container_of's type check does
     * exactly that through typeof_member().
     */
    bool leading_const = p->last_type_const;
    bool leading_volatile = p->last_type_volatile;
    if (!parse_typeof_type(p, type))
      return false;
    p->last_type_const |= leading_const;
    p->last_type_volatile |= leading_volatile;
    p->last_type_base_type = *type;
    p->last_type_base_valid = true;
  } else if (tok_is(p, p->pos, "_Generic")) {
    if (!resolve_static_generic_type(p, type, depth))
      return false;
  } else if (tok_is(p, p->pos, "__builtin_choose_expr")) {
    size_t open = p->pos + 1;
    if (!tok_is(p, open, "("))
      return false;
    size_t first_comma = SIZE_MAX;
    size_t second_comma = SIZE_MAX;
    size_t close = SIZE_MAX;
    size_t parens = 0;
    size_t brackets = 0;
    size_t braces = 0;
    for (size_t at = open + 1; at < p->token_count; ++at) {
      if (tok_is(p, at, "("))
        ++parens;
      else if (tok_is(p, at, ")")) {
        if (!parens && !brackets && !braces) {
          close = at;
          break;
        }
        if (parens)
          --parens;
      } else if (tok_is(p, at, "["))
        ++brackets;
      else if (tok_is(p, at, "]") && brackets)
        --brackets;
      else if (tok_is(p, at, "{"))
        ++braces;
      else if (tok_is(p, at, "}") && braces)
        --braces;
      else if (tok_is(p, at, ",") && !parens && !brackets && !braces) {
        if (first_comma == SIZE_MAX)
          first_comma = at;
        else if (second_comma == SIZE_MAX)
          second_comma = at;
      }
    }
    size_t condition_at = open + 1;
    int64_t condition = 0;
    bool previous_unsigned = p->global_const_unsigned;
    bool condition_ok = first_comma != SIZE_MAX &&
                        parse_global_const_expr(p, &condition_at, &condition) &&
                        condition_at == first_comma;
    p->global_const_unsigned = previous_unsigned;
    if (!condition_ok || second_comma == SIZE_MAX || close == SIZE_MAX ||
        second_comma >= close) {
      fail(p, "__builtin_choose_expr requires a constant condition and three "
              "arguments");
      return false;
    }
    size_t branch_start = condition ? first_comma + 1 : second_comma + 1;
    size_t branch_end = condition ? second_comma : close;
    p->pos = branch_start;
    if (!static_typeof_expr_type(p, type, depth + 1) || p->pos != branch_end)
      return false;
    p->pos = close + 1;
  } else if (tok_is(p, p->pos, "*")) {
    /*
     * `*(function designator)` is the function type itself.  The kernel's
     * `RCU_INITIALIZER(v)` spells this as `typeof(*(v)) *` when v names a
     * function, so resolve the signature without treating the designator
     * as a pointer to an incomplete type.
     */
    size_t designator_at = p->pos + 1;
    size_t designator_parens = 0;
    while (tok_is(p, designator_at, "(")) {
      ++designator_at;
      ++designator_parens;
    }
    if (designator_at < p->token_count && tok_ident(p->tokens[designator_at]) &&
        /*
         * Only a bare designator denotes the function type.  A call such as
         * `*task_pid_ptr(task, type)` dereferences the call result; treating
         * it as a designator would type the whole RCU expression as a
         * function pointer instead of the pointee.
         */
        !tok_is(p, designator_at + 1, "(") &&
        !find_local(p, p->tokens[designator_at]) &&
        !find_global(p, p->tokens[designator_at])) {
      c_function_t *fn = find_function(p, p->tokens[designator_at]);
      size_t designator_end = designator_at + 1;
      size_t closing = designator_parens;
      while (closing && tok_is(p, designator_end, ")")) {
        ++designator_end;
        --closing;
      }
      if (fn && !closing) {
        p->pos = designator_end;
        if (tok_is(p, p->pos, "(")) {
          size_t after = skip_balanced(p, p->pos, "(", ")");
          if (after <= p->pos || after >= p->token_count)
            return false;
          p->pos = after;
        }
        *type = C_FUNC_PTR;
        p->last_type_agg = NULL;
        p->last_type_function_result_type = fn->result;
        p->last_type_function_result_agg = fn->result_agg;
        p->last_type_function_void_params =
            fn->param_count == 0 && !fn->variadic;
        p->last_type_function_type = true;
        p->last_type_is_function_declarator = true;
        p->last_type_is_array = false;
        p->last_type_array_size = 0;
        return true;
      }
    }
    ++p->pos;
    if (!static_typeof_expr_type(p, type, depth + 1))
      return false;
    if (p->last_type_is_array && p->last_type_array.rank <= 1) {
      /*
       * Unary * applies after the array-to-pointer conversion.
       */
      p->last_type_is_array = false;
      p->last_type_array_decayed = false;
      p->last_type_array = (c_array_shape_t){0};
      p->last_type_array_size = 0;
      return true;
    }
    c_type_t pointer_type = *type;
    p->last_type_is_function_declarator =
        pointer_type == C_FUNC_PTR && p->last_type_function_type;
    *type = pointer_type == C_FUNC_PTR ? C_FUNC_PTR
                                       : c_remove_pointer_levels(*type, 1);
    p->last_type_const_pointer = false;
    if (p->last_type_pointee_array.rank) {
      p->last_type_array = p->last_type_pointee_array;
      p->last_type_pointee_array = (c_array_shape_t){0};
      p->last_type_is_array = true;
      p->last_type_array_size = c_array_shape_size(&p->last_type_array);
    }
    if (*type == C_BAD) {
      /*
       * This frontend spells the plain void type C_BAD, so typeof(*p) for a
       * void pointer is void, exactly as GCC defines it. Anything else that
       * collapses to C_BAD is a genuinely unsupported pointee type.
       */
      if (pointer_type != C_VOID_PTR) {
        fail(p, "typeof of a dereferenced incomplete pointer is unsupported");
        return false;
      }
      p->last_type_agg = NULL;
      p->last_type_is_array = false;
      p->last_type_array = (c_array_shape_t){0};
      p->last_type_array_size = 0;
      p->last_type_pointee_array = (c_array_shape_t){0};
      p->last_type_is_function_declarator = false;
    }
    if (*type != C_AGG && *type != C_AGG_PTR && *type != C_AGG_PTR_PTR &&
        !(c_type_is_pointer(*type) && p->last_type_agg))
      p->last_type_agg = NULL;
  } else if (tok_is(p, p->pos, "&")) {
    ++p->pos;
    ny_ctok_t addressed_function_name = {0};
    c_function_t *addressed_function = tok_ident(*current(p)) &&
                                               !find_local(p, *current(p)) &&
                                               !tok_is(p, p->pos + 1, "(")
                                           ? find_function(p, *current(p))
                                           : NULL;
    if (addressed_function)
      addressed_function_name = addressed_function->name;
    if (!static_typeof_expr_type(p, type, depth + 1))
      return false;
    p->last_type_pointee_array =
        p->last_type_is_array ? p->last_type_array : (c_array_shape_t){0};
    if (addressed_function) {
      *type = C_FUNC_PTR;
      p->last_type_agg = NULL;
      p->last_type_function_result_type = addressed_function->result;
      p->last_type_function_result_agg = addressed_function->result_agg;
      p->last_type_function_type = true;
      p->last_type_function_void_params =
          addressed_function->param_count == 0 && !addressed_function->variadic;
      p->last_type_typeof_function = addressed_function_name;
      return true;
    }
    /*
     * mirrors lower_address_*: scalar/aggregate base to pointer tag
     */
    if (*type == C_AGG) {
      *type = p->last_type_array_size ? C_AGG_ARRAY_PTR : C_AGG_PTR;
    } else {
      switch (*type) {
      case C_CHAR:
        *type = C_I8_PTR;
        break;
      case C_U8:
        *type = C_CHAR_PTR;
        break;
      case C_I16:
        *type = C_I16_PTR;
        break;
      case C_U16:
        *type = C_U16_PTR;
        break;
      case C_I32:
        *type = C_I32_PTR;
        break;
      case C_U32:
        *type = C_U32_PTR;
        break;
      case C_I64:
        *type = C_I64_PTR;
        break;
      case C_U64:
        *type = C_U64_PTR;
        break;
      case C_F32:
        *type = C_F32_PTR;
        break;
      case C_F64:
        *type = C_F64_PTR;
        break;
      case C_FUNC_PTR:
        *type = C_FUNC_PTR_PTR;
        break;
      case C_AGG_PTR:
        *type = C_AGG_PTR_PTR;
        break;
      case C_VOID_PTR:
        if (p->last_type_agg) {
          *type = C_AGG_PTR;
          break;
        }
        *type = C_OPAQUE_PTR_PTR;
        break;
      default:
        if (c_type_is_pointer(*type)) {
          *type = C_OPAQUE_PTR_PTR;
          p->last_type_agg = NULL;
        } else {
          fail(p, "typeof of an address-of operand requires a supported "
                  "scalar or aggregate type");
          return false;
        }
      }
    }
    p->last_type_is_array = false;
    p->last_type_array = (c_array_shape_t){0};
  } else if (tok_is(p, p->pos, "(") && tok_is(p, p->pos + 1, "{")) {
    if (!static_typeof_statement_expression(p, type, depth))
      return false;
  } else if (tok_is(p, p->pos, "(")) {
    if (p->pos + 1 < p->token_count &&
        token_leads_type(p, p->tokens[p->pos + 1])) {
      ++p->pos;
      c_type_t cast_type = C_BAD;
      if (!parse_type(p, &cast_type, true))
        return false;
      c_agg_type_t *cast_agg = p->last_type_agg;
      size_t extents[C_MAX_ARRAY_RANK] = {0};
      unsigned array_rank = 0;
      bool incomplete_outer = false;
      size_t inner_count = 1;
      while (accept(p, "[")) {
        if (array_rank == C_MAX_ARRAY_RANK)
          return false;
        if (array_rank == 0 && accept(p, "]")) {
          incomplete_outer = true;
          ++array_rank;
          continue;
        }
        uint64_t extent = 0;
        if (!parse_array_extent_expr(p, 1, &extent) || !extent ||
            extent > 65536 || !expect(p, "]") ||
            inner_count > 65536 / (size_t)extent)
          return false;
        extents[array_rank++] = (size_t)extent;
        if (array_rank > 1)
          inner_count *= (size_t)extent;
      }
      if (!expect(p, ")"))
        return false;
      if (tok_is(p, p->pos, "{")) {
        if (array_rank) {
          bool empty_initializer =
              tok_is(p, p->pos, "{") && tok_is(p, p->pos + 1, "}");
          if (incomplete_outer) {
            extents[0] = count_global_aggregate_array_initializer(p, p->pos);
            if (!extents[0] && !empty_initializer)
              return false;
          }
          if (extents[0] > 65536 / inner_count)
            return false;
        }
        size_t after_initializer = skip_balanced(p, p->pos, "{", "}");
        if (after_initializer >= p->token_count)
          return false;
        p->pos = after_initializer;
        *type = cast_type;
        p->last_type_agg = cast_agg;
        if (array_rank) {
          size_t element_size = cast_type == C_AGG && cast_agg
                                    ? cast_agg->size
                                    : c_type_size(p->abi, cast_type);
          size_t element_count = extents[0] * inner_count;
          if (!element_size || element_count > SIZE_MAX / element_size)
            return false;
          p->last_type_is_array = true;
          p->last_type_array_decayed = false;
          p->last_type_array_size = element_count * element_size;
          p->last_type_array = (c_array_shape_t){.rank = array_rank,
                                                 .element_type = cast_type,
                                                 .element_size = element_size,
                                                 .agg = cast_agg};
          memcpy(p->last_type_array.extent, extents,
                 array_rank * sizeof(*extents));
        }
        return true;
      }
      /*
       * the operand continues after the cast; walk and drop its type
       */
      c_type_parse_state_t cast_state = save_type_parse_state(p);
      c_type_t operand_type = C_BAD;
      if (!static_typeof_expr_type_impl(p, &operand_type, depth + 1, true))
        return false;
      *type = cast_type;
      restore_type_parse_state(p, &cast_state);
    } else {
      ++p->pos;
      if (!static_typeof_expr_type(p, type, depth + 1))
        return false;
      if (!expect(p, ")"))
        return false;
    }
  } else if (tok_ident(p->tokens[p->pos])) {
    ny_ctok_t token = *current(p);
    ++p->pos;
    bool resolved = false;
    if (ny_ctok_eq(token, "__builtin_constant_p") ||
        ny_ctok_eq(token, "__builtin_clz") ||
        ny_ctok_eq(token, "__builtin_clzl") ||
        ny_ctok_eq(token, "__builtin_clzll") ||
        ny_ctok_eq(token, "__builtin_ffs") ||
        ny_ctok_eq(token, "__builtin_ctz")) {
      *type = C_I32;
      p->last_type_agg = NULL;
      p->last_type_base_type = C_I32;
      p->last_type_base_valid = true;
      from_function = true;
      resolved = true;
    }
    c_function_t *fn = find_local(p, token) ? NULL : find_function(p, token);
    if (!resolved && fn) {
      *type = fn->result;
      p->last_type_agg = fn->result_agg;
      p->last_type_base_type = fn->result;
      p->last_type_base_valid = true;
      p->last_type_typeof_function = token;
      p->last_type_function_result_type = fn->result;
      p->last_type_function_result_agg = fn->result_agg;
      from_function = true;
      resolved = true;
    }
    if (!resolved) {
      c_local_t *local = find_local(p, token);
      if (local) {
        *type = local->type;
        p->last_type_long_long = local->is_long_long;
        p->last_type_function_type = local->type == C_FUNC_PTR;
        p->last_type_is_function_type_alias = false;
        p->last_type_function_result_type = local->function_result_type;
        if (*type == C_AGG_PTR && local->pointee_array_size)
          *type = C_AGG_ARRAY_PTR;
        p->last_type_agg = local->agg;
        p->last_type_array_size = local->pointee_array_size;
        p->last_type_is_array =
            local->array_rank != 0 || local->array_count != 0;
        p->last_type_array = (c_array_shape_t){.rank = local->array_rank};
        for (unsigned dimension = 0;
             dimension < local->array_rank && dimension < C_MAX_ARRAY_RANK;
             ++dimension)
          p->last_type_array.extent[dimension] = local->array_extent[dimension];
        p->last_type_pointee_array = local->pointee_array;
        p->last_type_array_decayed = false;
        p->last_type_base_type = local->type;
        p->last_type_base_valid = true;
        p->last_type_const = c_type_is_pointer(local->type)
                                 ? local->pointee_const
                                 : local->is_const;
        p->last_type_const_pointer =
            c_type_is_pointer(local->type) && local->is_const;
        p->last_type_volatile = local->is_volatile;
        p->last_type_bool = local->is_bool;
        resolved = true;
      }
    }
    if (!resolved) {
      c_global_t *global = find_global(p, token);
      if (global) {
        *type = global->type;
        p->last_type_long_long = global->is_long_long;
        p->last_type_function_type = global->type == C_FUNC_PTR;
        p->last_type_is_function_type_alias = false;
        p->last_type_function_result_type = global->function_result_type;
        p->last_type_array_size =
            global->is_array ? global->len ? global->len
                               : global->type == C_AGG_PTR
                                   ? c_array_shape_size(&global->array_shape)
                                   : 0
                             : 0;
        p->last_type_is_array = global->is_array;
        p->last_type_array = global->array_shape;
        if (global->is_array && global->type == C_AGG_PTR &&
            !p->last_type_array.rank && global->len &&
            global->len % c_type_size(p->abi, C_VOID_PTR) == 0) {
          size_t pointer_size = c_type_size(p->abi, C_VOID_PTR);
          p->last_type_array =
              (c_array_shape_t){.rank = 1,
                                .extent = {global->len / pointer_size},
                                .element_size = pointer_size,
                                .element_type = C_AGG_PTR,
                                .agg = global->agg};
        }
        p->last_type_pointee_array = (c_array_shape_t){0};
        p->last_type_array_decayed = false;
        p->last_type_agg = global->agg;
        p->last_type_base_type = global->type;
        p->last_type_base_valid = true;
        p->last_type_const = global->is_const;
        p->last_type_const_pointer =
            c_type_is_pointer(global->type) && global->is_const;
        resolved = true;
      }
    }
    if (!resolved) {
      const c_enum_constant_t *enumerator = find_enum_constant(p, token);
      if (enumerator) {
        *type = enumerator->type;
        p->last_type_base_type = *type;
        p->last_type_base_valid = true;
        resolved = true;
      }
    }
    if (!resolved) {
      for (size_t i = p->pp_constant_count; i > 0; --i) {
        if (strlen(p->pp_constants[i - 1].name) == token.len &&
            memcmp(p->pp_constants[i - 1].name, token.start, token.len) == 0) {
          *type = p->pp_constants[i - 1].integer_value ? C_I64 : C_I32;
          p->last_type_base_type = *type;
          p->last_type_base_valid = true;
          resolved = true;
          break;
        }
      }
    }
    if (!resolved) {
      const c_typedef_t *alias = find_typedef(p, token);
      if (alias) {
        *type = alias->type;
        p->last_type_bool = alias->is_bool;
        p->last_type_const = p->last_type_const || alias->is_const;
        p->last_type_agg = alias->agg;
        p->last_type_function_result_type = alias->function_result_type;
        p->last_type_function_result_agg = alias->function_result_agg;
        p->last_type_function_type =
            alias->function_type || alias->type == C_FUNC_PTR;
        p->last_type_is_function_type_alias = alias->function_type;
        p->last_type_function_void_params = alias->function_void_params;
        p->last_type_array_size = alias->array_size;
        p->last_type_array = alias->array_shape;
        p->last_type_is_array = alias->is_array;
        if (alias->array_shape.pointer_to_array) {
          p->last_type_pointee_array = alias->array_shape;
          p->last_type_array = (c_array_shape_t){0};
          p->last_type_is_array = false;
        }
        p->last_type_array_decayed = false;
        p->last_type_base_type = *type;
        p->last_type_base_valid = true;
        resolved = true;
      }
    }
    if (!resolved) {
      char detail[256];
      snprintf(detail, sizeof(detail),
               "typeof symbol `%.80s` has no declared type in the current "
               "scope; only already-declared functions, data, locals, enum, "
               "and preprocessor constants are supported",
               token.len && token.len < 80 ? token.start : "");
      fail(p, detail);
      return false;
    }
  } else if (p->tokens[p->pos].kind == NY_CTOK_STRING) {
    unsigned char *decoded = NULL;
    size_t decoded_size = 0;
    if (!decode_string_token(p->tokens[p->pos], &decoded, &decoded_size) ||
        !decoded_size) {
      free(decoded);
      fail(p, "invalid string literal in typeof expression");
      return false;
    }
    free(decoded);
    ++p->pos;
    *type = C_CHAR;
    p->last_type_agg = NULL;
    p->last_type_array_size = decoded_size;
    p->last_type_is_array = true;
    p->last_type_array_decayed = false;
    p->last_type_const = false;
    p->last_type_volatile = false;
    p->last_type_base_type = C_CHAR;
    p->last_type_base_valid = true;
  } else if (p->tokens[p->pos].kind == NY_CTOK_NUMBER ||
             p->tokens[p->pos].kind == NY_CTOK_CHAR) {
    ++p->pos;
    *type = C_I32;
    p->last_type_base_type = C_I32;
    p->last_type_base_valid = true;
  } else {
    fail(p, "unsupported typeof expression");
    return false;
  }
  /*
   * postfix member/index/call chains mirror expression evaluation
   */
  for (;;) {
    if (tok_is(p, p->pos, "++") || tok_is(p, p->pos, "--")) {
      ++p->pos;
    } else if (tok_is(p, p->pos, "->") || tok_is(p, p->pos, ".")) {
      bool arrow = tok_is(p, p->pos, "->");
      if (arrow && *type != C_AGG_PTR && *type != C_AGG_ARRAY_PTR) {
        fail(p, "typeof member access requires an aggregate operand");
        return false;
      }
      if (!arrow && *type != C_AGG) {
        fail(p, "typeof member access requires an aggregate operand");
        return false;
      }
      if (arrow)
        *type = C_AGG;
      ++p->pos;
      if (!tok_ident(p->tokens[p->pos])) {
        fail(p, "struct/union has no such member");
        return false;
      }
      const c_agg_field_t *field = find_field(p->last_type_agg, *current(p));
      if (!field) {
        fail(p, "struct/union has no such member");
        return false;
      }
      ++p->pos;
      p->last_type_bool = field->is_bool;
      if (!field->is_function_pointer && !field->elem_function_pointer) {
        p->last_type_function_type = false;
        p->last_type_is_function_type_alias = false;
        p->last_type_function_result_type = C_BAD;
        p->last_type_function_result_agg = NULL;
        p->last_type_function_void_params = false;
        p->last_type_typeof_function = (ny_ctok_t){0};
      }
      p->last_type_is_function_declarator = false;
      p->last_type_is_array = field->kind == C_AGG_FIELD_ARRAY;
      p->last_type_array_decayed = false;
      p->last_type_array = (c_array_shape_t){0};
      p->last_type_array_size =
          field->kind == C_AGG_FIELD_ARRAY ? field->size : 0;
      p->last_type_bool = field->is_bool;
      p->last_type_const =
          field->kind == C_AGG_FIELD_PTR && field->pointee_const;
      c_type_t member_type = C_BAD;
      if (field->is_bitfield) {
        member_type = field->bit_width < 32            ? C_I32
                      : field->kind == C_AGG_FIELD_I32 ? C_I32
                      : field->kind == C_AGG_FIELD_I64 ? C_I64
                      : field->kind == C_AGG_FIELD_U64 ? C_U64
                                                       : C_U32;
      } else {
        switch (field->kind) {
        case C_AGG_FIELD_C64:
          member_type = C_C64;
          break;
        case C_AGG_FIELD_C128:
          member_type = C_C128;
          break;
        case C_AGG_FIELD_C_LDOUBLE:
          member_type = C_C_LDOUBLE;
          break;
        case C_AGG_FIELD_AGG:
          member_type = C_AGG;
          break;
        case C_AGG_FIELD_PTR:
          if (field->has_function_signature) {
            p->last_type_function_result_type =
                (c_type_t)field->function_result_type;
            p->last_type_function_result_agg =
                (c_agg_type_t *)field->function_result_agg;
            p->last_type_function_type = true;
            p->last_type_function_void_params = false;
          }
          member_type =
              field->pointer_element_type != C_BAD && field->pointer_depth
                  ? c_add_pointer_levels((c_type_t)field->pointer_element_type,
                                         1)
              : field->agg && field->pointer_depth > 2   ? C_OPAQUE_PTR_PTR_PTR
              : field->agg && field->pointer_depth > 1   ? C_AGG_PTR_PTR
              : field->agg                               ? C_AGG_PTR
              : field->elem_function_pointer             ? C_FUNC_PTR_PTR
              : field->pointer_element_type == C_VA_LIST ? C_VA_LIST_PTR
              : field->is_function_pointer               ? C_FUNC_PTR
              : field->elem_char_pointer                 ? C_CHAR_PTR_PTR
              : field->pointer_depth == 1 &&
                      field->pointer_element_type == C_BAD
                  ? C_VOID_PTR
              : field->elem_pointer ? C_OPAQUE_PTR_PTR
              : field->elem_f32     ? C_F32_PTR
              : field->elem_f64     ? C_F64_PTR
              : field->elem_size == 1
                  ? (field->is_signed_byte ? C_I8_PTR : C_CHAR_PTR)
              : field->elem_size == 2
                  ? (field->elem_unsigned ? C_U16_PTR : C_I16_PTR)
              : field->elem_size == 4
                  ? (field->elem_unsigned ? C_U32_PTR : C_I32_PTR)
              : field->elem_unsigned ? C_U64_PTR
                                     : C_I64_PTR;
          break;
        case C_AGG_FIELD_ARRAY:
          p->last_type_is_array = true;
          member_type =
              field->agg && !field->elem_pointer ? C_AGG
              : field->elem_pointer
                  ? (field->pointer_element_type != C_BAD
                         ? (c_type_t)field->pointer_element_type
                     : field->agg ? C_AGG_PTR
                                  : C_OPAQUE_PTR)
              : field->elem_f32       ? C_F32
              : field->elem_f64       ? C_F64
              : field->elem_size == 1 ? (field->is_signed_byte ? C_CHAR : C_U8)
              : field->elem_size == 2 ? (field->elem_unsigned ? C_U16 : C_I16)
              : field->elem_size == 4 ? (field->elem_unsigned ? C_U32 : C_I32)
                                      : (field->elem_unsigned ? C_U64 : C_I64);
          break;
        case C_AGG_FIELD_I64:
          member_type = C_I64;
          break;
        case C_AGG_FIELD_U64:
          member_type = C_U64;
          break;
        case C_AGG_FIELD_I32:
          member_type = C_I32;
          break;
        case C_AGG_FIELD_U8:
          member_type = field->is_signed_byte ? C_CHAR : C_U8;
          break;
        case C_AGG_FIELD_I16:
          member_type = C_I16;
          break;
        case C_AGG_FIELD_U16:
          member_type = C_U16;
          break;
        case C_AGG_FIELD_U32:
          member_type = C_U32;
          break;
        case C_AGG_FIELD_F32:
          member_type = C_F32;
          break;
        case C_AGG_FIELD_F64:
          member_type = C_F64;
          break;
        }
      }
      if (field->kind == C_AGG_FIELD_ARRAY) {
        p->last_type_array = (c_array_shape_t){.rank = field->array_rank,
                                               .element_size = field->elem_size,
                                               .element_type = member_type,
                                               .agg = field->agg};
        for (unsigned dimension = 0; dimension < field->array_rank; ++dimension)
          p->last_type_array.extent[dimension] = field->array_extent[dimension];
      }
      if (field->pointer_to_array && !field->pointer_to_aggregate_array) {
        c_type_t element_type = field->pointer_element_type != C_BAD
                                    ? (c_type_t)field->pointer_element_type
                                    : C_BAD;
        size_t element_size = element_type == C_AGG && field->agg
                                  ? field->agg->size
                                  : c_type_size(p->abi, element_type);
        if (!element_size || !field->array_rank) {
          fail(p, "typeof pointer-to-array field has an incomplete element");
          return false;
        }
        p->last_type_pointee_array =
            (c_array_shape_t){.rank = field->array_rank,
                              .element_size = element_size,
                              .element_type = element_type,
                              .agg = field->agg,
                              .pointer_to_array = true};
        memcpy(p->last_type_pointee_array.extent, field->array_extent,
               field->array_rank *
                   sizeof(p->last_type_pointee_array.extent[0]));
      }
      *type = member_type;
      p->last_type_agg = member_type == C_AGG || member_type == C_AGG_PTR ||
                                 member_type == C_AGG_PTR_PTR
                             ? (c_agg_type_t *)field->agg
                             : NULL;
      p->last_type_base_type = member_type;
      p->last_type_base_valid = true;
    } else if (tok_is(p, p->pos, "[")) {
      size_t after = skip_balanced(p, p->pos, "[", "]");
      if (after >= p->token_count) {
        fail(p, "unterminated typeof index expression");
        return false;
      }
      c_type_t element =
          p->last_type_array.rank ||
                  (p->last_type_is_array && !p->last_type_array_decayed)
              ? *type
              : c_remove_pointer_levels(*type, 1);
      if (element == C_BAD) {
        fail(p, "pointer index requires a supported complete element type");
        return false;
      }
      if (element != C_AGG && element != C_AGG_PTR && element != C_AGG_PTR_PTR)
        p->last_type_agg = NULL;
      *type = element;
      if (p->last_type_array.rank) {
        for (unsigned dimension = 1; dimension < p->last_type_array.rank;
             ++dimension)
          p->last_type_array.extent[dimension - 1] =
              p->last_type_array.extent[dimension];
        --p->last_type_array.rank;
        p->last_type_is_array = p->last_type_array.rank != 0;
      } else {
        p->last_type_is_array = false;
      }
      p->last_type_array_decayed = false;
      p->last_type_array_size = 0;
      if (p->last_type_is_array) {
        size_t array_size = p->last_type_array.element_size;
        if (!array_size)
          array_size = element == C_AGG && p->last_type_agg
                           ? p->last_type_agg->size
                           : c_type_size(p->abi, element);
        for (unsigned dimension = 0; dimension < p->last_type_array.rank;
             ++dimension)
          array_size *= p->last_type_array.extent[dimension];
        p->last_type_array_size = array_size;
      }
      p->last_type_base_type = element;
      p->last_type_base_valid = true;
      p->pos = after;
    } else if (tok_is(p, p->pos, "(")) {
      bool function_pointer_call =
          *type == C_FUNC_PTR && p->last_type_function_result_type != C_BAD;
      bool typeof_function_call =
          from_function || p->last_type_typeof_function.len != 0;
      if (!typeof_function_call && !function_pointer_call) {
        fail(p, "typeof of a call expression requires a declared function "
                "callee");
        return false;
      }
      size_t after = skip_balanced(p, p->pos, "(", ")");
      if (after >= p->token_count) {
        fail(p, "unterminated typeof call argument list");
        return false;
      }
      p->pos = after;
      if (function_pointer_call) {
        *type = p->last_type_function_result_type;
        p->last_type_agg = p->last_type_function_result_agg;
        p->last_type_base_type = *type;
        p->last_type_base_valid = true;
      }
      /*
       * A call expression has the function's result type. Keep the
       * typeof(function) identity only for the bare function designator.
       */
      from_function = false;
      p->last_type_typeof_function = (ny_ctok_t){0};
      p->last_type_function_type = false;
    } else {
      break;
    }
  }
  if (unary_only)
    return true;
  c_agg_type_t *left_agg = p->last_type_agg;
  bool left_is_array = p->last_type_is_array && !p->last_type_array_decayed;
  while (tok_is(p, p->pos, "+") || tok_is(p, p->pos, "-") ||
         tok_is(p, p->pos, "*") || tok_is(p, p->pos, "/") ||
         tok_is(p, p->pos, "%") || tok_is(p, p->pos, "<<") ||
         tok_is(p, p->pos, ">>") || tok_is(p, p->pos, "&") ||
         tok_is(p, p->pos, "|") || tok_is(p, p->pos, "^") ||
         tok_is(p, p->pos, "==") || tok_is(p, p->pos, "!=") ||
         tok_is(p, p->pos, "<") || tok_is(p, p->pos, "<=") ||
         tok_is(p, p->pos, ">") || tok_is(p, p->pos, ">=") ||
         tok_is(p, p->pos, "&&") || tok_is(p, p->pos, "||")) {
    bool subtract = tok_is(p, p->pos, "-");
    bool additive = tok_is(p, p->pos, "+") || subtract;
    bool comparison = tok_is(p, p->pos, "==") || tok_is(p, p->pos, "!=") ||
                      tok_is(p, p->pos, "<") || tok_is(p, p->pos, "<=") ||
                      tok_is(p, p->pos, ">") || tok_is(p, p->pos, ">=") ||
                      tok_is(p, p->pos, "&&") || tok_is(p, p->pos, "||");
    ++p->pos;
    c_type_t right_type = C_BAD;
    if (!static_typeof_expr_type(p, &right_type, depth + 1))
      return false;
    bool right_is_array = p->last_type_is_array && !p->last_type_array_decayed;
    if (additive && left_is_array) {
      *type = *type == C_AGG ? C_AGG_PTR : c_array_pointer_type(*type);
      p->last_type_is_array = false;
      p->last_type_array = (c_array_shape_t){0};
      left_is_array = false;
    }
    if (additive && right_is_array)
      right_type =
          right_type == C_AGG ? C_AGG_PTR : c_array_pointer_type(right_type);
    if (additive && c_type_is_pointer(*type) && c_type_is_integer(right_type)) {
      p->last_type_agg = left_agg;
      continue;
    }
    if (additive && !subtract && c_type_is_integer(*type) &&
        c_type_is_pointer(right_type)) {
      *type = right_type;
      left_agg = p->last_type_agg;
      continue;
    }
    if (additive && subtract && c_type_is_pointer(*type) &&
        c_type_is_pointer(right_type)) {
      *type = C_I64;
      left_agg = NULL;
      p->last_type_agg = NULL;
      continue;
    }
    if (c_type_is_integer(*type) && c_type_is_integer(right_type)) {
      if (comparison) {
        *type = C_I32;
      } else if (right_type == C_I64 || right_type == C_U64) {
        *type = right_type;
      }
      left_agg = NULL;
      p->last_type_agg = NULL;
      continue;
    }
    fail(p, "unsupported arithmetic in typeof expression");
    return false;
  }
  if (accept(p, "?")) {
    bool omitted_true = accept(p, ":");
    c_type_t true_type = omitted_true ? *type : C_BAD;
    size_t true_start = p->pos;
    size_t true_end = true_start;
    if (!omitted_true) {
      if (!static_typeof_expr_type(p, &true_type, depth + 1))
        return false;
      true_end = p->pos;
      if (!expect(p, ":"))
        return false;
    }
    bool true_is_pointer = c_type_is_pointer(true_type);
    bool true_is_aggregate = true_type == C_AGG || true_type == C_AGG_PTR ||
                             true_type == C_AGG_PTR_PTR;
    c_agg_type_t *true_agg = omitted_true ? left_agg : p->last_type_agg;
    size_t true_array_size = p->last_type_array_size;
    bool true_is_array = p->last_type_is_array;
    c_type_t false_type = C_BAD;
    size_t false_start = p->pos;
    if (!static_typeof_expr_type(p, &false_type, depth + 1))
      return false;
    size_t false_end = p->pos;
    if (true_type == false_type &&
        (!true_is_aggregate || true_agg == p->last_type_agg)) {
      *type = true_type;
      if (true_is_array && p->last_type_is_array &&
          true_array_size == p->last_type_array_size) {
        p->last_type_array_size = true_array_size;
        p->last_type_is_array = true;
      }
      return true;
    }
    if (c_type_is_integer(true_type) && c_type_is_integer(false_type)) {
      *type = true_type == C_I64 || true_type == C_U64 || false_type == C_I64 ||
                      false_type == C_U64
                  ? (true_type == C_U64 || false_type == C_U64 ? C_U64 : C_I64)
              : true_type == C_U32 || false_type == C_U32 ? C_U32
                                                          : C_I32;
      p->last_type_agg = NULL;
      p->last_type_array_size = 0;
      p->last_type_is_array = false;
      return true;
    }
    if (true_is_pointer && c_type_is_pointer(false_type) &&
        (c_pointer_compatible(true_type, false_type) ||
         true_type == false_type)) {
      /*
       * Linux's NULL expands to ((void *)0); as a null pointer constant it
       * preserves the other operand's pointer type in a conditional.
       */
      if (false_type == C_VOID_PTR &&
          static_typeof_is_void_null_pointer(p, false_start, false_end)) {
        *type = true_type;
        p->last_type_agg = true_agg;
        p->last_type_array_size = 0;
        p->last_type_is_array = false;
        return true;
      }
      if (true_type == C_VOID_PTR &&
          static_typeof_is_void_null_pointer(p, true_start, true_end)) {
        *type = false_type;
        p->last_type_array_size = 0;
        p->last_type_is_array = false;
        return true;
      }
      *type = true_type == C_VOID_PTR || false_type == C_VOID_PTR ? C_VOID_PTR
                                                                  : true_type;
      p->last_type_agg = true_type == C_AGG_PTR ? true_agg : p->last_type_agg;
      p->last_type_array_size = 0;
      p->last_type_is_array = false;
      return true;
    }
    if (true_is_pointer && c_type_is_integer(false_type)) {
      *type = true_type;
      p->last_type_agg = true_agg;
      p->last_type_array_size = 0;
      p->last_type_is_array = false;
      return true;
    }
    if (c_type_is_integer(true_type) && c_type_is_pointer(false_type)) {
      *type = false_type;
      return true;
    }
    fail(p, "incompatible conditional operands in typeof expression");
    return false;
  }
  return true;
}

/*
 * Static `_Generic` selection for `typeof` arguments: resolves the
 * controlling expression's type without emitting instructions, matches it
 * against each selector's type (the kernel's `__scalar_type_to_expr_cases`
 * expansions arrive as comma-separated `type : (type)0` pairs), and adopts
 * the matching branch's static value type (`default:` covers the rest).
 */
static bool resolve_static_generic_type(c_parser_t *p, c_type_t *type,
                                        unsigned depth) {
  ++p->pos; /* _Generic */
  if (!expect(p, "("))
    return false;
  c_type_t control_type;
  if (!static_typeof_expr_type(p, &control_type, depth + 1))
    return false;
  const c_agg_type_t *control_agg = p->last_type_agg;
  bool control_bool = p->last_type_bool;
  bool control_pointee_const =
      c_type_is_pointer(control_type) && p->last_type_const;
  if (p->last_type_is_array && !p->last_type_array_decayed)
    control_type = c_array_decay_type(control_type);
  c_type_parse_state_t control_state = save_type_parse_state(p);
  size_t selected_start = 0;
  bool have_selected = false;
  size_t default_start = 0;
  bool have_default = false;
  for (;;) {
    if (tok_is(p, p->pos, ")")) {
      ++p->pos;
      break;
    }
    if (p->pos >= p->token_count) {
      fail(p, "unterminated static _Generic selection");
      return false;
    }
    /*
     * every selector follows a comma (including the first one)
     */
    if (!expect(p, ","))
      return false;
    bool is_default = tok_is(p, p->pos, "default");
    if (!is_default && !tok_ident(p->tokens[p->pos])) {
      fail(p, "static _Generic selector must name a type");
      return false;
    }
    c_type_t selector = C_BAD;
    const c_agg_type_t *selector_agg = NULL;
    bool selector_bool = false;
    bool selector_pointee_const = false;
    if (!is_default) {
      if (!parse_type(p, &selector, true)) {
        if (getenv("NYC_PP_TRACE"))
          fprintf(stderr, "GENERIC-FAIL selector parse near `%.*s`\n",
                  (int)(current(p)->len > 24 ? 24 : current(p)->len),
                  current(p)->start ? current(p)->start : "");
        return false;
      }
      selector_agg = p->last_type_agg;
      selector_bool = p->last_type_bool;
      selector_pointee_const =
          c_type_is_pointer(selector) && p->last_type_const;
    } else {
      ++p->pos;
    }
    if (!expect(p, ":")) {
      if (getenv("NYC_PP_TRACE"))
        fprintf(stderr, "GENERIC-FAIL selector colon near `%.*s`\n",
                (int)(current(p)->len > 24 ? 24 : current(p)->len),
                current(p)->start ? current(p)->start : "");
      return false;
    }
    size_t branch_start = p->pos;
    if (!skip_to_comma_or_close(p))
      return false;
    if (is_default) {
      if (!have_default) {
        default_start = branch_start;
        have_default = true;
      }
    } else if (!have_selected && selector == control_type &&
               selector_bool == control_bool &&
               (selector == C_FUNC_PTR ||
                selector_pointee_const == control_pointee_const) &&
               ((selector != C_AGG && selector != C_AGG_PTR &&
                 selector != C_AGG_PTR_PTR) ||
                selector_agg == control_agg)) {
      selected_start = branch_start;
      have_selected = true;
    }
  }
  size_t end_pos = p->pos; /* past the scanned _Generic closer */
  restore_type_parse_state(p, &control_state);
  if (have_selected) {
    p->pos = selected_start;
    if (!static_typeof_expr_type(p, type, depth + 1))
      return false;
    p->pos = end_pos;
  } else if (have_default) {
    p->pos = default_start;
    if (!static_typeof_expr_type(p, type, depth + 1))
      return false;
    p->pos = end_pos;
  } else {
    char detail[128];
    snprintf(detail, sizeof(detail),
             "static _Generic selection has no selector for type %d",
             (int)control_type);
    fail(p, detail);
    return false;
  }
  p->last_type_base_type = *type;
  p->last_type_base_valid = true;
  return true;
}

static bool type_alignment_attribute_follows(const c_parser_t *p) {
  return tok_is_attribute(p, p->pos) && tok_is(p, p->pos + 1, "(") &&
         tok_is(p, p->pos + 2, "(") &&
         (tok_is(p, p->pos + 3, "aligned") ||
          tok_is(p, p->pos + 3, "__aligned__"));
}

static bool parse_tag_body(c_parser_t *p, ny_ctok_t name, bool is_union);
static bool parse_tag_fields(c_parser_t *p, c_agg_type_t *tag, bool is_union);
static bool parse_alignment_attribute(c_parser_t *p,
                                      size_t *requested_alignment,
                                      bool *packed_out);
static bool relayout_aggregate_fields(c_parser_t *p, c_agg_type_t *tag,
                                      bool is_union, bool packed);

static size_t align_up(size_t value, size_t align) {
  return align <= 1 ? value : (value + align - 1) / align * align;
}

static bool promote_aggregate_fields(c_parser_t *p, c_agg_type_t *parent,
                                     const c_agg_type_t *nested,
                                     size_t base_offset) {
  size_t total = nested->field_count + nested->promoted_field_count;
  for (size_t i = 0; i < total; ++i) {
    const c_agg_field_t *source =
        i < nested->field_count
            ? &nested->fields[i]
            : &nested->promoted_fields[i - nested->field_count];
    if (!source->name.len)
      continue;
    if (source->offset > SIZE_MAX - base_offset ||
        parent->promoted_field_count >= C_AGG_MAX_FIELDS) {
      fail(p, "promoted struct/union members exceed the frontend limit");
      return false;
    }
    for (size_t j = 0; j < parent->field_count; ++j)
      if (tok_same(parent->fields[j].name, source->name)) {
        fail(p, "duplicate promoted struct/union member");
        return false;
      }
    for (size_t j = 0; j < parent->promoted_field_count; ++j)
      if (tok_same(parent->promoted_fields[j].name, source->name)) {
        fail(p, "duplicate promoted struct/union member");
        return false;
      }
    if (parent->promoted_field_count == parent->promoted_field_capacity) {
      size_t capacity = parent->promoted_field_capacity
                            ? parent->promoted_field_capacity * 2
                            : 8;
      if (capacity < parent->promoted_field_capacity ||
          capacity > C_AGG_MAX_FIELDS)
        capacity = C_AGG_MAX_FIELDS;
      c_agg_field_t *fields =
          realloc(parent->promoted_fields, capacity * sizeof(*fields));
      if (!fields) {
        fail(p, "out of memory recording promoted struct/union members");
        return false;
      }
      parent->promoted_fields = fields;
      parent->promoted_field_capacity = capacity;
    }
    c_agg_field_t promoted = *source;
    promoted.offset += base_offset;
    parent->promoted_fields[parent->promoted_field_count++] = promoted;
  }
  return true;
}

static bool parse_type_pointers(c_parser_t *p, unsigned *levels,
                                bool *pointer_const) {
  size_t declaration_alignment = p->last_type_alignment;
  *levels = 0;
  *pointer_const = false;
  while (accept(p, "*")) {
    ++*levels;
    *pointer_const = false;
    while (tok_is(p, p->pos, "const") || tok_is(p, p->pos, "volatile") ||
           tok_is(p, p->pos, "__seg_gs") || tok_is(p, p->pos, "__seg_fs") ||
           tok_is(p, p->pos, "restrict") || tok_is(p, p->pos, "__restrict") ||
           tok_is(p, p->pos, "__restrict__") || tok_is_attribute(p, p->pos)) {
      if (tok_is_attribute(p, p->pos)) {
        if (!parse_function_type_attributes(p))
          return false;
      } else if (accept(p, "const"))
        *pointer_const = true;
      else if (accept(p, "volatile"))
        p->last_type_volatile = true;
      else
        ++p->pos;
    }
    /*
     * An inner pointer's alignment does not align the outer pointer object.
     */
    if (tok_is(p, p->pos, "*"))
      p->last_type_alignment = declaration_alignment;
  }
  return true;
}

static bool parse_type(c_parser_t *p, c_type_t *type, bool allow_void) {
  bool is_unsigned = false, is_signed = false, is_float = false,
       is_double = false, is_long = false, is_long_long = false, is_int = false,
       is_void = false, is_char = false, is_short = false, is_complex = false,
       is_long_double = false;
  bool typeof_resolved = false;
  p->last_type_const = false;
  p->last_type_const_pointer = false;
  p->last_type_volatile = false;
  p->last_type_seg = C_SEG_NONE;
  p->last_type_long_long = false;
  p->last_type_agg = NULL;
  p->last_type_section = (ny_ctok_t){0};
  p->last_type_cleanup = (ny_ctok_t){0};
  p->last_type_typeof_function = (ny_ctok_t){0};
  p->last_type_function_result_type = C_BAD;
  p->last_type_function_result_agg = NULL;
  p->last_type_function_type = false;
  p->last_type_is_function_declarator = false;
  p->last_type_is_function_type_alias = false;
  p->last_type_function_void_params = false;
  p->last_type_enum_incomplete = false;
  p->last_type_base_type = C_BAD;
  p->last_type_pointer_count = 0;
  p->last_type_array_size = 0;
  p->last_type_array = (c_array_shape_t){0};
  p->last_type_pointee_array = (c_array_shape_t){0};
  p->last_type_is_array = false;
  p->last_type_array_decayed = false;
  p->last_type_bool = false;
  p->last_type_alignment = 0;
  p->last_type_mode = (ny_ctok_t){0};
  p->last_type_base_valid = false;
  /*
   * `_Complex` may start the specifier list: `_Complex double`.
   */
  while (tok_is(p, p->pos, "_Complex") || tok_is(p, p->pos, "__complex__")) {
    is_complex = true;
    ++p->pos;
  }
  while (tok_is(p, p->pos, "const") || tok_is(p, p->pos, "volatile") ||
         tok_is(p, p->pos, "__volatile") || tok_is(p, p->pos, "__volatile__") ||
         tok_is(p, p->pos, "static") || tok_is(p, p->pos, "extern") ||
         tok_is(p, p->pos, "register") || tok_is(p, p->pos, "inline") ||
         tok_is(p, p->pos, "__inline") || tok_is(p, p->pos, "__inline__") ||
         tok_is(p, p->pos, "__seg_gs") || tok_is(p, p->pos, "__seg_fs") ||
         tok_is_attribute(p, p->pos) || tok_is(p, p->pos, "_Atomic") ||
         tok_is(p, p->pos, "signed") || tok_is(p, p->pos, "__signed") ||
         tok_is(p, p->pos, "__signed__") || tok_is(p, p->pos, "restrict") ||
         tok_is(p, p->pos, "__restrict") || tok_is(p, p->pos, "__restrict__") ||
         tok_is(p, p->pos, "__extension__")) {
    if (tok_is_attribute(p, p->pos)) {
      if (!parse_function_type_attributes(p))
        return false;
      continue;
    }
    if (tok_is(p, p->pos, "_Atomic")) {
      ++p->pos;
      if (accept(p, "(")) {
        bool leading_const = p->last_type_const;
        bool leading_volatile = p->last_type_volatile;
        if (!parse_type(p, type, true) || !expect(p, ")"))
          return false;
        p->last_type_const |= leading_const;
        p->last_type_volatile |= leading_volatile;
        is_void = *type == C_BAD;
        typeof_resolved = true;
      }
      continue;
    }
    if (tok_is(p, p->pos, "const")) {
      p->last_type_const = true;
      ++p->pos;
      continue;
    }
    if (tok_is(p, p->pos, "volatile") || tok_is(p, p->pos, "__volatile") ||
        tok_is(p, p->pos, "__volatile__")) {
      p->last_type_volatile = true;
      ++p->pos;
      continue;
    }

    if (tok_is(p, p->pos, "static") || tok_is(p, p->pos, "inline") ||
        tok_is(p, p->pos, "__inline") || tok_is(p, p->pos, "__inline__")) {
      ++p->pos;
      continue;
    }
    if (tok_is(p, p->pos, "__extension__")) {
      ++p->pos;
      continue;
    }
    /*
     * `_Complex` may precede the base type: `_Complex double`.
     */
    if (tok_is(p, p->pos, "_Complex") || tok_is(p, p->pos, "__complex__")) {
      is_complex = true;
      ++p->pos;
      continue;
    }
    /*
     * Linux marks per-CPU declarations with an address-space qualifier. The
     * qualifier belongs to the pointed-to object, so record it and let the
     * pointer lowerer emit the matching segment prefix on every access.
     */
    if (tok_is(p, p->pos, "__seg_gs") || tok_is(p, p->pos, "__seg_fs")) {
      p->last_type_seg = tok_is(p, p->pos, "__seg_gs") ? C_SEG_GS : C_SEG_FS;
      ++p->pos;
      continue;
    }
    if (tok_is(p, p->pos, "signed") || tok_is(p, p->pos, "__signed") ||
        tok_is(p, p->pos, "__signed__")) {
      is_signed = true;
      ++p->pos;
      continue;
    }
    if (tok_is(p, p->pos, "extern") || tok_is(p, p->pos, "register")) {
      ++p->pos;
      continue;
    }
    ++p->pos;
  }
  if (starts_aggregate_type(p)) {
    bool is_union = tok_is(p, p->pos, "union");
    ++p->pos;
    size_t requested_alignment = 0;
    bool packed_tag = false;
    if (tok_is_attribute(p, p->pos) &&
        !parse_alignment_attribute(p, &requested_alignment, &packed_tag))
      return false;
    ny_ctok_t tag_name = {0};
    if (tok_ident(*current(p))) {
      tag_name = *current(p);
      ++p->pos;
    }
    c_agg_type_t *aggregate = NULL;
    if (accept(p, "{")) {
      if (p->type_lookahead) {
        p->pos = skip_balanced(p, p->pos - 1, "{", "}");
        if (p->pos >= p->token_count) {
          fail(p, "unclosed struct/union body");
          return false;
        }
      } else if (tag_name.len) {
        if (!parse_tag_body(p, tag_name, is_union))
          return false;
        aggregate = find_tag(p, tag_name, is_union);
      } else {
        if (p->tag_count >= p->tag_capacity) {
          size_t next_capacity = p->tag_capacity ? p->tag_capacity * 2 : 16;
          c_agg_type_t **grown =
              realloc(p->tags, next_capacity * sizeof(*p->tags));
          if (!grown) {
            fail(p, "out of memory growing aggregate table");
            return false;
          }
          p->tags = grown;
          p->tag_capacity = next_capacity;
        }
        aggregate = calloc(1, sizeof(*aggregate));
        if (!aggregate) {
          fail(p, "out of memory allocating anonymous struct/union");
          return false;
        }
        aggregate->scope_id = p->tag_scope_id;
        aggregate->is_union = is_union;
        p->tags[p->tag_count++] = aggregate;
        if (!parse_tag_fields(p, aggregate, is_union))
          return false;
      }
      size_t trailing_alignment = 0;
      bool trailing_packed = false;
      if (!parse_alignment_attribute(p, &trailing_alignment, &trailing_packed))
        return false;
      if (trailing_alignment > requested_alignment)
        requested_alignment = trailing_alignment;
      packed_tag = packed_tag || trailing_packed;
      if (aggregate && packed_tag &&
          !relayout_aggregate_fields(p, aggregate, is_union, true))
        return false;
      if (aggregate && requested_alignment > aggregate->align) {
        aggregate->align = requested_alignment;
        aggregate->size = align_up(aggregate->size, aggregate->align);
      }
    } else if (!tag_name.len) {
      fail(p, "struct/union types require a tag name");
      return false;
    } else if (!p->type_lookahead && !find_tag(p, tag_name, is_union)) {
      if (!declare_tag(p, tag_name, is_union))
        return false;
    }
    *type = C_AGG;
    p->last_type_agg = aggregate      ? aggregate
                       : tag_name.len ? find_tag(p, tag_name, is_union)
                                      : NULL;
    p->last_type_base_type = C_AGG;
    p->last_type_base_valid = true;
    while (tok_is_attribute(p, p->pos) &&
           !type_alignment_attribute_follows(p)) {
      if (!parse_function_type_attributes(p))
        return false;
    }
    while (tok_is(p, p->pos, "const") || tok_is(p, p->pos, "volatile") ||
           tok_is(p, p->pos, "__seg_gs") || tok_is(p, p->pos, "__seg_fs") ||
           tok_is(p, p->pos, "restrict") || tok_is(p, p->pos, "__restrict") ||
           tok_is(p, p->pos, "__restrict__")) {
      if (accept(p, "const"))
        p->last_type_const = true;
      else if (accept(p, "volatile"))
        p->last_type_volatile = true;
      else if (tok_is(p, p->pos, "__seg_gs") || tok_is(p, p->pos, "__seg_fs")) {
        p->last_type_seg = tok_is(p, p->pos, "__seg_gs") ? C_SEG_GS : C_SEG_FS;
        ++p->pos;
      } else
        ++p->pos;
    }
    /*
     * GNU permits type attributes after qualifiers such as __seg_gs.
     */
    while (tok_is_attribute(p, p->pos) &&
           !type_alignment_attribute_follows(p)) {
      if (!parse_function_type_attributes(p))
        return false;
    }
    unsigned pointers;
    bool pointer_const;
    if (!parse_type_pointers(p, &pointers, &pointer_const))
      return false;
    p->last_type_pointer_count = pointers;
    p->last_type_const_pointer = pointers && pointer_const;
    while (tok_is_attribute(p, p->pos)) {
      if (!parse_function_type_attributes(p))
        return false;
    }
    if (pointers == 1) {
      *type = C_AGG_PTR;
    } else if (pointers == 2) {
      *type = C_AGG_PTR_PTR;
    } else if (pointers > 2) {
      *type = C_OPAQUE_PTR;
    }
    p->last_type_array_size = 0;
    p->last_type_is_array = false;
    p->last_type_array_decayed = false;
    return true;
  }
  if (tok_is(p, p->pos, "typeof") || tok_is(p, p->pos, "__typeof__") ||
      tok_is(p, p->pos, "__typeof") || tok_is(p, p->pos, "__typeof_unqual__")) {
    bool leading_const = p->last_type_const;
    bool leading_volatile = p->last_type_volatile;
    unsigned char leading_seg = p->last_type_seg;
    size_t leading_alignment = p->last_type_alignment;
    ny_ctok_t leading_section = p->last_type_section;
    ny_ctok_t leading_cleanup = p->last_type_cleanup;
    ny_ctok_t leading_mode = p->last_type_mode;
    if (!parse_typeof_type(p, type))
      return false;
    /*
     * A recursively parsed type name owns its type metadata, not the
     * attributes already collected for the enclosing declaration.
     */
    if (leading_section.len)
      p->last_type_section = leading_section;
    if (leading_cleanup.len)
      p->last_type_cleanup = leading_cleanup;
    if (leading_mode.len)
      p->last_type_mode = leading_mode;
    if (leading_seg != C_SEG_NONE)
      p->last_type_seg = leading_seg;
    if (leading_alignment > p->last_type_alignment)
      p->last_type_alignment = leading_alignment;
    p->last_type_const |= leading_const;
    p->last_type_volatile |= leading_volatile;
    typeof_resolved = true;
    is_void = *type == C_BAD;
  }
  if (!typeof_resolved) {
    const c_typedef_t *alias =
        tok_ident(*current(p)) ? find_typedef(p, *current(p)) : NULL;
    if (!alias && tok_is(p, p->pos, "enum")) {
      if (!parse_enum_type(p, type))
        return false;
    } else if (!alias && (tok_is(p, p->pos, "va_list") ||
                          tok_is(p, p->pos, "__builtin_va_list") ||
                          tok_is(p, p->pos, "__gnuc_va_list"))) {
      *type = p->abi == NYC_ABI_I386 ? C_CHAR_PTR : C_VA_LIST;
      ++p->pos;
    } else if (alias) {
      *type = alias->type;
      is_void = alias->is_void;
      p->last_type_long_long = alias->is_long_long;
      p->last_type_bool = alias->is_bool;
      p->last_type_const = p->last_type_const || alias->is_const;
      p->last_type_agg = alias->agg;
      p->last_type_array_size = alias->array_size;
      p->last_type_is_array = alias->is_array;
      p->last_type_array = alias->array_shape;
      if (alias->array_shape.pointer_to_array) {
        p->last_type_pointee_array = alias->array_shape;
        p->last_type_array = (c_array_shape_t){0};
        p->last_type_is_array = false;
      }
      p->last_type_alignment = alias->alignment;
      p->last_type_function_result_type = alias->function_result_type;
      p->last_type_function_result_agg = alias->function_result_agg;
      p->last_type_function_type =
          alias->function_type || alias->type == C_FUNC_PTR;
      p->last_type_is_function_type_alias = alias->function_type;
      p->last_type_function_void_params = alias->function_void_params;
      ++p->pos;
    } else if (tok_is(p, p->pos, "_Bool")) {
      *type = C_U8;
      p->last_type_bool = true;
      ++p->pos;
    } else if (tok_is(p, p->pos, "__int128_t") ||
               tok_is(p, p->pos, "__uint128_t") ||
               tok_is(p, p->pos, "__int128") ||
               tok_is(p, p->pos, "__int128__") ||
               ((tok_is(p, p->pos, "unsigned") || tok_is(p, p->pos, "signed") ||
                 tok_is(p, p->pos, "__signed__")) &&
                (tok_is(p, p->pos + 1, "__int128") ||
                 tok_is(p, p->pos + 1, "__int128__")))) {
      /*
       * GNU 128-bit integers.  The classification of a 16-byte {u64,u64}
       * aggregate on x86-64 SysV (INTEGER,INTEGER) matches GCC's __int128
       * argument/return class exactly, so the frontend represents the type
       * as tagged discrete storage and leaves the value untouched.
       */
      bool unsigned_128 = tok_is(p, p->pos, "__uint128_t");
      while (tok_is(p, p->pos, "unsigned")) {
        unsigned_128 = true;
        ++p->pos;
      }
      while (tok_is(p, p->pos, "signed") || tok_is(p, p->pos, "__signed__"))
        ++p->pos;
      c_agg_type_t *tag = arch_int128_tag(p, unsigned_128);
      if (!tag) {
        fail(p, "out of memory building the 128-bit integer type");
        return false;
      }
      *type = C_AGG;
      p->last_type_agg = tag;
      p->last_type_base_type = C_AGG;
      p->last_type_base_valid = true;
      ++p->pos;
    } else if (tok_is(p, p->pos, "size_t") || tok_is(p, p->pos, "uintptr_t")) {
      *type = p->abi == NYC_ABI_I386 ? C_U32 : C_U64;
      ++p->pos;
    } else if (tok_is(p, p->pos, "uint64_t")) {
      *type = C_U64;
      ++p->pos;
    } else if (tok_is(p, p->pos, "int_least8_t")) {
      *type = C_CHAR;
      ++p->pos;
    } else if (tok_is(p, p->pos, "uint_least8_t")) {
      *type = C_U8;
      ++p->pos;
    } else if (tok_is(p, p->pos, "int_least16_t")) {
      *type = C_I16;
      ++p->pos;
    } else if (tok_is(p, p->pos, "uint_least16_t")) {
      *type = C_U16;
      ++p->pos;
    } else if (tok_is(p, p->pos, "int_least32_t")) {
      *type = C_I32;
      ++p->pos;
    } else if (tok_is(p, p->pos, "uint_least32_t")) {
      *type = C_U32;
      ++p->pos;
    } else if (tok_is(p, p->pos, "int_least64_t")) {
      *type = C_I64;
      ++p->pos;
    } else if (tok_is(p, p->pos, "uint_least64_t")) {
      *type = C_U64;
      ++p->pos;
    } else if (tok_is(p, p->pos, "int_fast8_t")) {
      *type = C_CHAR;
      ++p->pos;
    } else if (tok_is(p, p->pos, "uint_fast8_t")) {
      *type = C_U8;
      ++p->pos;
    } else if (tok_is(p, p->pos, "int_fast16_t") ||
               tok_is(p, p->pos, "int_fast32_t")) {
      *type = p->abi == NYC_ABI_I386 ? C_I32 : C_I64;
      ++p->pos;
    } else if (tok_is(p, p->pos, "uint_fast16_t") ||
               tok_is(p, p->pos, "uint_fast32_t")) {
      *type = p->abi == NYC_ABI_I386 ? C_U32 : C_U64;
      ++p->pos;
    } else if (tok_is(p, p->pos, "int_fast64_t")) {
      *type = C_I64;
      ++p->pos;
    } else if (tok_is(p, p->pos, "uint_fast64_t")) {
      *type = C_U64;
      ++p->pos;
    } else if (tok_is(p, p->pos, "intmax_t")) {
      *type = C_I64;
      ++p->pos;
    } else if (tok_is(p, p->pos, "uintmax_t")) {
      *type = C_U64;
      ++p->pos;
    } else if (tok_is(p, p->pos, "time_t") || tok_is(p, p->pos, "clock_t") ||
               tok_is(p, p->pos, "off_t") || tok_is(p, p->pos, "ssize_t") ||
               tok_is(p, p->pos, "intptr_t") ||
               tok_is(p, p->pos, "ptrdiff_t") || tok_is(p, p->pos, "int64_t")) {
      *type = (p->abi == NYC_ABI_I386 && (tok_is(p, p->pos, "ssize_t") ||
                                          tok_is(p, p->pos, "ptrdiff_t") ||
                                          tok_is(p, p->pos, "intptr_t")))
                  ? C_I32
                  : C_I64;
      ++p->pos;
    } else if (tok_is(p, p->pos, "uint32_t")) {
      *type = C_U32;
      ++p->pos;
    } else if (tok_is(p, p->pos, "int32_t")) {
      *type = C_I32;
      ++p->pos;
    } else if (tok_is(p, p->pos, "wchar_t")) {
      *type = C_I32;
      ++p->pos;
    } else if (tok_is(p, p->pos, "wint_t")) {
      *type = C_U32;
      ++p->pos;
    } else {
      for (;;) {
        if (tok_is(p, p->pos, "unsigned")) {
          is_unsigned = true;
          ++p->pos;
        } else if (tok_is(p, p->pos, "const") || tok_is(p, p->pos, "__const") ||
                   tok_is(p, p->pos, "__const__")) {
          p->last_type_const = true;
          ++p->pos;
        } else if (tok_is(p, p->pos, "volatile") ||
                   tok_is(p, p->pos, "__volatile") ||
                   tok_is(p, p->pos, "__volatile__")) {
          p->last_type_volatile = true;
          ++p->pos;
        } else
          break;
      }
      if (accept(p, "float") || accept(p, "_Float32"))
        is_float = true;
      else if (accept(p, "double") || accept(p, "_Float64") ||
               accept(p, "_Float32x"))
        is_double = true;
      else if (accept(p, "short")) {
        is_short = true;
        if (accept(p, "unsigned"))
          is_unsigned = true;
        (void)accept(p, "int");
      } else if (accept(p, "long")) {
        if (accept(p, "double")) {
          is_double = true;
          is_long_double = true;
        } else {
          is_long = true;
          if (accept(p, "long"))
            is_long_long = true;
          if (accept(p, "unsigned"))
            is_unsigned = true;
          else if (accept(p, "signed"))
            is_signed = true;
          (void)accept(p, "int");
        }
      } else if (accept(p, "int")) {
        is_int = true;
        /*
         * C permits the integer type specifiers in either order.
         */
        while (tok_is(p, p->pos, "int") || tok_is(p, p->pos, "long") ||
               tok_is(p, p->pos, "short") || tok_is(p, p->pos, "unsigned") ||
               tok_is(p, p->pos, "signed") || tok_is(p, p->pos, "__signed__") ||
               tok_is(p, p->pos, "__signed") || tok_is(p, p->pos, "const") ||
               tok_is(p, p->pos, "__const") || tok_is(p, p->pos, "__const__") ||
               tok_is(p, p->pos, "volatile") ||
               tok_is(p, p->pos, "__volatile") ||
               tok_is(p, p->pos, "__volatile__")) {
          if (accept(p, "long")) {
            is_long = true;
            if (accept(p, "long"))
              is_long_long = true;
          } else if (accept(p, "short")) {
            is_short = true;
          } else if (accept(p, "unsigned")) {
            is_unsigned = true;
          } else if (accept(p, "signed") || accept(p, "__signed__") ||
                     accept(p, "__signed")) {
            is_signed = true;
          } else if (accept(p, "const") || accept(p, "__const") ||
                     accept(p, "__const__")) {
            p->last_type_const = true;
          } else if (accept(p, "volatile") || accept(p, "__volatile") ||
                     accept(p, "__volatile__")) {
            p->last_type_volatile = true;
          } else {
            (void)accept(p, "int");
          }
        }
      } else if (accept(p, "char")) {
        is_char = true;
      } else if (accept(p, "void"))
        is_void = true;
      else if (is_unsigned || is_signed)
        is_int = true;
      else {
        char detail[192];
        snprintf(
            detail, sizeof(detail),
            "unsupported C type near `%.80s`; expected an integer, double, "
            "or void",
            current(p)->start ? current(p)->start : "");
        fail(p, detail);
        return false;
      }
      if (is_float)
        *type = C_F32;
      else if (is_double)
        *type = C_F64;
      else if (is_void)
        *type = C_BAD;
      else if (is_short)
        *type = is_unsigned ? C_U16 : C_I16;
      else if (is_long && (is_long_long || p->abi != NYC_ABI_I386))
        *type = is_unsigned ? C_U64 : C_I64;
      else if (is_long)
        *type = is_unsigned ? C_U32 : C_I32;
      else if (is_int)
        *type = is_unsigned ? C_U32 : C_I32;
      else if (is_char)
        *type =
            is_signed || (!is_unsigned && !p->unsigned_char) ? C_CHAR : C_U8;
      else if (is_unsigned)
        *type = C_U32;
      else {
        fail(p, "integer type must be signed int or long");
        return false;
      }
      p->last_type_long_long = is_long_long;
    }
  }
  /*
   * `_Complex` may follow the base type: `double _Complex`.
   */
  while (tok_is(p, p->pos, "_Complex") || tok_is(p, p->pos, "__complex__")) {
    is_complex = true;
    ++p->pos;
  }
  if (is_complex) {
    if (is_long_double)
      *type = C_C_LDOUBLE;
    else if (*type == C_F32)
      *type = C_C64;
    else if (*type == C_F64)
      *type = C_C128;
    else {
      fail(p, "_Complex requires a floating base type");
      return false;
    }
  }
  while (
      tok_is(p, p->pos, "const") || tok_is(p, p->pos, "volatile") ||
      tok_is(p, p->pos, "__seg_gs") || tok_is(p, p->pos, "__seg_fs") ||
      (tok_is_attribute(p, p->pos) && !type_alignment_attribute_follows(p))) {
    if (accept(p, "const"))
      p->last_type_const = true;
    else if (accept(p, "volatile")) {
      p->last_type_volatile = true;
    } else if (tok_is(p, p->pos, "__seg_gs") || tok_is(p, p->pos, "__seg_fs")) {
      p->last_type_seg = tok_is(p, p->pos, "__seg_gs") ? C_SEG_GS : C_SEG_FS;
      ++p->pos;
    } else if (!parse_function_type_attributes(p))
      return false;
  }
  while (tok_is_attribute(p, p->pos) && !type_alignment_attribute_follows(p)) {
    if (!parse_function_type_attributes(p))
      return false;
  }
  if (!apply_mode_attribute(p, type))
    return false;
  if (tok_is(p, p->pos, "(") && tok_is(p, p->pos + 1, "*")) {
    c_type_t result_type = *type;
    c_agg_type_t *result_agg = p->last_type_agg;
    c_parser_t probe = *p;
    ny_ctok_t abstract_name;
    if (parse_function_pointer_declarator(&probe, &abstract_name) &&
        !abstract_name.len) {
      p->pos = probe.pos;
      *type = C_FUNC_PTR;
      p->last_type_agg = NULL;
      p->last_type_function_result_type = result_type;
      p->last_type_function_result_agg = result_agg;
      p->last_type_function_type = true;
      p->last_type_is_function_declarator = true;
      p->last_type_function_void_params = false;
      p->last_type_base_type = result_type;
      p->last_type_base_valid = true;
      p->last_type_pointer_count = 0;
      p->last_type_const_pointer = false;
      p->last_type_array_size = 0;
      p->last_type_is_array = false;
      p->last_type_array_decayed = false;
      return true;
    }
  }
  unsigned pointers;
  bool pointer_const;
  if (!parse_type_pointers(p, &pointers, &pointer_const))
    return false;
  p->last_type_const_pointer =
      pointers ? pointer_const : typeof_resolved && p->last_type_const_pointer;
  bool alias_pointer_to_array = p->last_type_pointee_array.pointer_to_array;
  p->last_type_base_type =
      alias_pointer_to_array ? p->last_type_pointee_array.element_type : *type;
  p->last_type_pointer_count = pointers + (alias_pointer_to_array ? 1 : 0);
  p->last_type_base_valid = true;
  if ((p->last_type_is_function_type_alias ||
       p->last_type_typeof_function.len ||
       p->last_type_is_function_declarator) &&
      pointers == 1)
    *type = C_FUNC_PTR;
  else if ((p->last_type_is_function_type_alias ||
            p->last_type_typeof_function.len ||
            p->last_type_is_function_declarator) &&
           pointers == 2)
    *type = C_FUNC_PTR_PTR;
  else if (pointers >= 2)
    *type = pointers == 2 && is_char
                ? (*type == C_CHAR ? C_I8_PTR_PTR : C_CHAR_PTR_PTR)
            : pointers == 2 && *type == C_U8  ? C_CHAR_PTR_PTR
            : pointers == 2 && *type == C_U16 ? C_U16_PTR_PTR
            : pointers == 2 && *type == C_AGG ? C_AGG_PTR_PTR
            : pointers == 2                   ? C_OPAQUE_PTR_PTR
                                              : C_OPAQUE_PTR_PTR_PTR;
  else if (pointers == 1 && *type == C_FUNC_PTR)
    *type = C_FUNC_PTR_PTR;
  else if (pointers == 1 && *type == C_AGG_PTR && p->last_type_array.rank &&
           p->last_type_array.element_type == C_AGG_PTR)
    *type = C_AGG_PTR_ARRAY_PTR;
  else if (pointers == 1 && *type == C_AGG_PTR && p->last_type_array_size)
    *type = C_AGG_ARRAY_PTR;
  else if (pointers == 1 && c_type_is_pointer(*type))
    *type = c_add_pointer_levels(*type, 1);
  else if (pointers == 1 && *type == C_CHAR)
    *type = C_I8_PTR;
  else if (pointers == 1 && is_char)
    *type = C_CHAR_PTR;
  else if (pointers == 1 && *type == C_U8)
    *type = C_CHAR_PTR;
  else if (pointers == 1 && *type == C_I32)
    *type = C_I32_PTR;
  else if (pointers == 1 && *type == C_U32)
    *type = C_U32_PTR;
  else if (pointers == 1 && *type == C_I16)
    *type = C_I16_PTR;
  else if (pointers == 1 && *type == C_U16)
    *type = C_U16_PTR;
  else if (pointers == 1 && is_void)
    *type = C_VOID_PTR;
  else if (pointers == 1 && *type == C_I64)
    *type = C_I64_PTR;
  else if (pointers == 1 && *type == C_U64)
    *type = C_U64_PTR;
  else if (pointers == 1 && *type == C_F64)
    *type = C_F64_PTR;
  else if (pointers == 1 && *type == C_F32)
    *type = C_F32_PTR;
  else if (pointers == 1 && *type == C_C64)
    *type = C_C64_PTR;
  else if (pointers == 1 && *type == C_C128)
    *type = C_C128_PTR;
  else if (pointers == 1 && *type == C_C_LDOUBLE)
    *type = C_C_LDOUBLE_PTR;
  else if (pointers == 1 && *type == C_AGG)
    *type = p->last_type_array_size ? C_AGG_ARRAY_PTR : C_AGG_PTR;
  else if (pointers == 1 && *type == C_VA_LIST)
    /*
     * `struct va_format { va_list *va; }` and friends: a pointer to the
     * SysV va_list aggregate
     */
    *type = C_VA_LIST_PTR;
  else if (pointers != 0) {
    fail(p, "only char, signed long, double, or void pointers are supported");
    return false;
  }
  if (pointers && p->last_type_is_array && p->last_type_array.rank) {
    /*
     * Preserve array extents when a pointer declarator wraps a typedef/typeof
     * array; its first subscript advances by the complete array size.
     */
    p->last_type_pointee_array = p->last_type_array;
    p->last_type_pointee_array.pointer_to_array = true;
    p->last_type_array = (c_array_shape_t){0};
    p->last_type_is_array = false;
    p->last_type_array_decayed = false;
  }
  if (*type == C_BAD && !allow_void) {
    fail(p, "void is only valid as a result type or opaque pointer");
    return false;
  }
  return true;
}

static bool parse_name(c_parser_t *p, ny_ctok_t *name) {
  if (!tok_ident(*current(p))) {
    char detail[128];
    ny_ctok_t token = *current(p);
    snprintf(detail, sizeof(detail), "expected identifier near `%.64s`",
             token.start ? token.start : "");
    fail(p, detail);
    return false;
  }
  *name = *current(p);
  ++p->pos;
  return true;
}

static bool parse_parenthesized_declarator_name(c_parser_t *p,
                                                ny_ctok_t *name) {
  bool parenthesized = accept(p, "(");
  if (!parse_name(p, name))
    return false;
  return !parenthesized || expect(p, ")");
}

static bool parse_function_pointer_declarator(c_parser_t *p, ny_ctok_t *name) {
  if (!tok_is(p, p->pos, "("))
    return false;
  c_parser_t probe = *p;
  ++probe.pos;
  while (tok_is_attribute(&probe, probe.pos))
    if (!parse_function_type_attributes(&probe))
      return false;
  if (!accept(&probe, "*"))
    return false;
  size_t declarator_pos = probe.pos;
  while (tok_is(p, declarator_pos, "const") ||
         tok_is(p, declarator_pos, "volatile") ||
         tok_is(p, declarator_pos, "restrict") ||
         tok_is(p, declarator_pos, "__restrict") ||
         tok_is(p, declarator_pos, "__restrict__"))
    ++declarator_pos;
  bool named = declarator_pos + 2 < p->token_count &&
               tok_ident(p->tokens[declarator_pos]) &&
               tok_is(p, declarator_pos + 1, ")") &&
               tok_is(p, declarator_pos + 2, "(");
  bool unnamed = declarator_pos + 1 < p->token_count &&
                 tok_is(p, declarator_pos, ")") &&
                 tok_is(p, declarator_pos + 1, "(");
  if (!named && !unnamed)
    return false;
  ++p->pos;
  while (tok_is_attribute(p, p->pos))
    if (!parse_function_type_attributes(p))
      return false;
  if (!expect(p, "*"))
    return false;
  while (tok_is(p, p->pos, "const") || tok_is(p, p->pos, "volatile") ||
         tok_is(p, p->pos, "restrict") || tok_is(p, p->pos, "__restrict") ||
         tok_is(p, p->pos, "__restrict__"))
    ++p->pos;
  *name = (ny_ctok_t){0};
  if (tok_ident(*current(p))) {
    *name = *current(p);
    ++p->pos;
  }
  if (!expect(p, ")") || !expect(p, "("))
    return false;
  size_t depth = 1;
  while (p->pos < p->token_count && depth) {
    if (accept(p, "("))
      ++depth;
    else if (accept(p, ")"))
      --depth;
    else
      ++p->pos;
  }
  if (depth) {
    fail(p, "unterminated C function-pointer declarator");
    return false;
  }
  return true;
}

static bool parse_nested_function_pointer_declarator(c_parser_t *p,
                                                     ny_ctok_t *name,
                                                     unsigned *pointers) {
  if (!tok_is(p, p->pos, "(") || !tok_is(p, p->pos + 1, "*"))
    return false;
  size_t name_pos = p->pos + 1;
  unsigned depth = 0;
  while (tok_is(p, name_pos, "*")) {
    ++depth;
    ++name_pos;
    while (tok_is(p, name_pos, "const") || tok_is(p, name_pos, "volatile") ||
           tok_is(p, name_pos, "restrict") ||
           tok_is(p, name_pos, "__restrict") ||
           tok_is(p, name_pos, "__restrict__"))
      ++name_pos;
  }
  bool named = name_pos < p->token_count && tok_ident(p->tokens[name_pos]);
  size_t close = name_pos + (named ? 1 : 0);
  if (depth < 2 || !tok_is(p, close, ")") || !tok_is(p, close + 1, "("))
    return false;
  *name = named ? p->tokens[name_pos] : (ny_ctok_t){0};
  size_t after_params = skip_balanced(p, close + 1, "(", ")");
  if (after_params >= p->token_count) {
    fail(p, "unterminated nested function-pointer declarator");
    return false;
  }
  p->pos = after_params;
  *pointers = depth;
  return true;
}

static bool
parse_function_pointer_returning_function_pointer_declarator(c_parser_t *p,
                                                             ny_ctok_t *name) {
  size_t start = p->pos;
  if (!tok_is(p, start, "(") || !tok_is(p, start + 1, "*") ||
      !tok_is(p, start + 2, "(") || !tok_is(p, start + 3, "*") ||
      !tok_ident(p->tokens[start + 4]) || !tok_is(p, start + 5, ")") ||
      !tok_is(p, start + 6, "("))
    return false;
  size_t params_end = skip_balanced(p, start + 6, "(", ")");
  if (params_end >= p->token_count || !tok_is(p, params_end, ")") ||
      !tok_is(p, params_end + 1, "("))
    return false;
  size_t result_params_end = skip_balanced(p, params_end + 1, "(", ")");
  if (result_params_end >= p->token_count)
    return false;
  *name = p->tokens[start + 4];
  p->pos = result_params_end;
  return true;
}

static bool
parse_parenthesized_pointer_array_declarator(c_parser_t *p, ny_ctok_t *name,
                                             unsigned *pointer_depth) {
  if (!tok_is(p, p->pos, "("))
    return false;
  c_parser_t probe = *p;
  ++probe.pos;
  while (tok_is(&probe, probe.pos, "const") ||
         tok_is(&probe, probe.pos, "volatile") ||
         tok_is(&probe, probe.pos, "restrict") ||
         tok_is(&probe, probe.pos, "__restrict") ||
         tok_is(&probe, probe.pos, "__restrict__") ||
         tok_is_attribute(&probe, probe.pos)) {
    if (tok_is_attribute(&probe, probe.pos)) {
      if (!parse_function_type_attributes(&probe))
        return false;
    } else {
      ++probe.pos;
    }
  }
  unsigned depth = 0;
  while (accept(&probe, "*")) {
    ++depth;
    while (tok_is(&probe, probe.pos, "const") ||
           tok_is(&probe, probe.pos, "volatile") ||
           tok_is(&probe, probe.pos, "restrict") ||
           tok_is(&probe, probe.pos, "__restrict") ||
           tok_is(&probe, probe.pos, "__restrict__"))
      ++probe.pos;
  }
  if (depth == 0)
    return false;
  ny_ctok_t found_name = {0};
  if (tok_ident(probe.tokens[probe.pos]))
    found_name = probe.tokens[probe.pos++];
  if (!tok_is(&probe, probe.pos, ")"))
    return false;
  ++probe.pos;
  if (!tok_is(&probe, probe.pos, "["))
    return false;
  *p = probe;
  if (name)
    *name = found_name;
  if (pointer_depth)
    *pointer_depth = depth;
  while (tok_is(p, p->pos, "[")) {
    size_t after = skip_balanced(p, p->pos, "[", "]");
    if (after <= p->pos || after >= p->token_count) {
      fail(p, "unterminated array declarator");
      return false;
    }
    p->pos = after;
  }
  return true;
}

static bool parse_alignment_attribute(c_parser_t *p,
                                      size_t *requested_alignment,
                                      bool *packed_out) {
  while (accept_attribute(p)) {
    if (!expect(p, "(") || !expect(p, "("))
      return false;
    do {
      if (tok_is(p, p->pos, "aligned") || tok_is(p, p->pos, "__aligned__")) {
        ++p->pos;
        uint64_t alignment = 0;
        if (accept(p, "(")) {
          int64_t parsed = 0;
          if (!parse_global_const_expr(p, &p->pos, &parsed) || parsed <= 0 ||
              !expect(p, ")")) {
            fail(p, "invalid aligned field attribute");
            return false;
          }
          alignment = (uint64_t)parsed;
        } else {
          alignment = 16;
        }
        if (alignment > SIZE_MAX || (alignment & (alignment - 1)) != 0) {
          fail(p, "field alignment must be a power of two");
          return false;
        }
        if (alignment > *requested_alignment)
          *requested_alignment = (size_t)alignment;
        continue;
      }
      if (tok_is(p, p->pos, "packed") || tok_is(p, p->pos, "__packed__")) {
        ++p->pos;
        if (packed_out)
          *packed_out = true;
        continue;
      }
      if (tok_is(p, p->pos, "format") || tok_is(p, p->pos, "__format__")) {
        ++p->pos;
        size_t after = skip_balanced(p, p->pos, "(", ")");
        if (after <= p->pos || after >= p->token_count) {
          fail(p, "format field attribute requires an argument list");
          return false;
        }
        p->pos = after;
        continue;
      }
      if (tok_is(p, p->pos, "counted_by") ||
          tok_is(p, p->pos, "__counted_by__")) {
        ++p->pos;
        size_t after = skip_balanced(p, p->pos, "(", ")");
        if (after <= p->pos || after >= p->token_count) {
          fail(p, "counted_by field attribute requires an argument");
          return false;
        }
        p->pos = after;
        continue;
      }
      if (tok_is(p, p->pos, "nonstring") ||
          tok_is(p, p->pos, "__nonstring__") ||
          tok_is(p, p->pos, "may_alias") ||
          tok_is(p, p->pos, "__may_alias__")) {
        ++p->pos;
        continue;
      }
      if (tok_is(p, p->pos, "deprecated") ||
          tok_is(p, p->pos, "__deprecated__") || tok_is(p, p->pos, "unused") ||
          tok_is(p, p->pos, "__unused__") ||
          tok_is(p, p->pos, "__designated_init__")) {
        ++p->pos;
        continue;
      }
      fail(p, "unsupported struct/union field attribute");
      return false;
    } while (accept(p, ","));
    if (!expect(p, ")") || !expect(p, ")"))
      return false;
  }
  return true;
}

/*
 * One GNU attribute group at file scope: plain no-argument spellings are
 * accepted and dropped; `__section__(name)` records its string-literal
 * token.  Anything else fails closed with the unsupported spelling.
 */
static bool parser_keep_macro_text(c_parser_t *p, char *text);

static bool parse_section_string_sequence(c_parser_t *p, ny_ctok_t *section) {
  if (!p || !section || current(p)->kind != NY_CTOK_STRING)
    return false;
  size_t begin = p->pos;
  size_t length = 2;
  while (p->pos < p->token_count && p->tokens[p->pos].kind == NY_CTOK_STRING) {
    ny_ctok_t token = p->tokens[p->pos++];
    if (token.len < 2 || token.len - 2 > SIZE_MAX - length ||
        length + token.len - 2 == SIZE_MAX) {
      fail(p, "section attribute string sequence is too large");
      return false;
    }
    length += token.len - 2;
  }
  if (p->pos - begin == 1 || p->type_lookahead) {
    *section = p->tokens[begin];
    return true;
  }
  char *joined = malloc(length + 1);
  if (!joined) {
    fail(p, "out of memory concatenating section attribute strings");
    return false;
  }
  size_t offset = 0;
  joined[offset++] = '"';
  for (size_t i = begin; i < p->pos; ++i) {
    ny_ctok_t token = p->tokens[i];
    size_t part_len = token.len - 2;
    memcpy(joined + offset, token.start + 1, part_len);
    offset += part_len;
  }
  joined[offset++] = '"';
  joined[offset] = '\0';
  if (!parser_keep_macro_text(p, joined)) {
    free(joined);
    fail(p, "out of memory storing section attribute strings");
    return false;
  }
  *section =
      (ny_ctok_t){.kind = NY_CTOK_STRING, .start = joined, .len = offset};
  return true;
}

static bool parse_file_scope_attribute_group(c_parser_t *p, ny_ctok_t *section,
                                             const char *what,
                                             ny_ctok_t *cleanup) {
  do {
    if (tok_is(p, p->pos, "regparm") || tok_is(p, p->pos, "__regparm__")) {
      ++p->pos;
      int64_t count = -1;
      if (!expect(p, "(") || !parse_global_const_expr(p, &p->pos, &count) ||
          !expect(p, ")") || count != 0) {
        fail(
            p,
            "only the default i386 regparm(0) calling convention is supported");
        return false;
      }
    } else if (tok_is(p, p->pos, "mode") || tok_is(p, p->pos, "__mode__")) {
      ++p->pos;
      if (!expect(p, "(") || !tok_ident(*current(p))) {
        fail(p, "mode attribute requires one integer mode name");
        return false;
      }
      ny_ctok_t mode = *current(p);
      ++p->pos;
      if (!expect(p, ")")) {
        fail(p, "mode attribute requires one integer mode name");
        return false;
      }
      if (p->last_type_mode.len && !tok_same(p->last_type_mode, mode)) {
        fail(p, "conflicting mode attributes");
        return false;
      }
      p->last_type_mode = mode;
    } else if (tok_is(p, p->pos, "__section__") ||
               tok_is(p, p->pos, "section")) {
      ++p->pos;
      if (!expect(p, "(") || !parse_section_string_sequence(p, section) ||
          !expect(p, ")")) {
        fail(p, "section attribute requires one or more string literals");
        return false;
      }
    } else if (strcmp(what, "function type attribute") == 0 &&
               (tok_is(p, p->pos, "target") ||
                tok_is(p, p->pos, "__target__"))) {
      /*
       * NYC does no target-dependent optimization or instruction selection.
       * Accept this Zstd dispatch annotation only for its known BMI2 feature
       * set; explicit feature-specific operations still need real lowering.
       */
      ++p->pos;
      unsigned char *target = NULL;
      size_t target_len = 0;
      static const char supported_target[] = "lzcnt,bmi,bmi2";
      if (!expect(p, "(") || current(p)->kind != NY_CTOK_STRING ||
          !decode_string_token(*current(p), &target, &target_len) ||
          target_len != sizeof(supported_target) ||
          memcmp(target, supported_target, sizeof(supported_target) - 1) != 0) {
        free(target);
        fail(p, "unsupported function target attribute");
        return false;
      }
      free(target);
      ++p->pos;
      if (!expect(p, ")")) {
        fail(p, "function target attribute requires one string literal");
        return false;
      }
    } else if (strcmp(what, "function type attribute") == 0 &&
               (tok_is(p, p->pos, "optimize") ||
                tok_is(p, p->pos, "__optimize__"))) {
      /*
       * NYC has no vectorizer; accept only the matching no-vectorization
       * request used by Zstd, which cannot change generated code here.
       */
      ++p->pos;
      char option[32];
      if (!expect(p, "(") ||
          !copy_quoted_token(p, *current(p), option, sizeof(option)) ||
          strcmp(option, "no-tree-vectorize") != 0) {
        fail(p, "unsupported function optimize attribute");
        return false;
      }
      ++p->pos;
      if (!expect(p, ")")) {
        fail(p, "function optimize attribute requires one string literal");
        return false;
      }
    } else if (tok_is(p, p->pos, "visibility") ||
               tok_is(p, p->pos, "__visibility__")) {
      ++p->pos;
      char visibility[16];
      if (!expect(p, "(") ||
          !copy_quoted_token(p, *current(p), visibility, sizeof(visibility)) ||
          (strcmp(visibility, "hidden") != 0 &&
           strcmp(visibility, "default") != 0)) {
        fail(p, "visibility attribute requires `hidden` or `default`");
        return false;
      }
      ++p->pos;
      if (!expect(p, ")")) {
        fail(p, "visibility attribute requires one string literal");
        return false;
      }
    } else if (tok_is(p, p->pos, "alias") || tok_is(p, p->pos, "__alias__")) {
      ++p->pos;
      if (!expect(p, "(") || current(p)->kind != NY_CTOK_STRING) {
        fail(p, "alias attribute requires one string-literal target");
        return false;
      }
      if (p->last_function_alias.len &&
          !tok_same(p->last_function_alias, *current(p))) {
        fail(p, "conflicting function alias attributes");
        return false;
      }
      p->last_function_alias = *current(p);
      ++p->pos;
      if (!expect(p, ")")) {
        fail(p, "alias attribute requires one string-literal target");
        return false;
      }
    } else if (tok_is(p, p->pos, "format") || tok_is(p, p->pos, "__format__")) {
      /*
       * `__format__(archetype, string-index, first-to-check)` carries only
       * diagnostics GCC verifies; the frontend drops the payload.
       */
      ++p->pos;
      size_t after = skip_balanced(p, p->pos, "(", ")");
      if (after <= p->pos || after >= p->token_count) {
        fail(p, "format attribute requires an argument list");
        return false;
      }
      p->pos = after;
    } else if (tok_is(p, p->pos, "access") || tok_is(p, p->pos, "__access__")) {
      ++p->pos;
      size_t after = skip_balanced(p, p->pos, "(", ")");
      if (after <= p->pos || after >= p->token_count) {
        fail(p, "access attribute requires an argument list");
        return false;
      }
      p->pos = after;
    } else if (tok_is(p, p->pos, "nonnull") ||
               tok_is(p, p->pos, "__nonnull__")) {
      /*
       * `nonnull` may omit its argument list to mark every pointer parameter.
       */
      ++p->pos;
      if (tok_is(p, p->pos, "(")) {
        size_t after = skip_balanced(p, p->pos, "(", ")");
        if (after <= p->pos || after >= p->token_count) {
          fail(p, "nonnull attribute has an invalid argument list");
          return false;
        }
        p->pos = after;
      }
    } else if (tok_is(p, p->pos, "alloc_size") ||
               tok_is(p, p->pos, "__alloc_size__") ||
               tok_is(p, p->pos, "alloc_align") ||
               tok_is(p, p->pos, "__alloc_align__") ||
               tok_is(p, p->pos, "realloc_size") ||
               tok_is(p, p->pos, "__realloc_size__")) {
      /*
       * These attributes carry diagnostics metadata, not object layout.
       */
      ++p->pos;
      size_t after = skip_balanced(p, p->pos, "(", ")");
      if (after <= p->pos || after >= p->token_count) {
        fail(p, "allocation attribute requires an argument list");
        return false;
      }
      p->pos = after;
    } else if (tok_is(p, p->pos, "assume_aligned") ||
               tok_is(p, p->pos, "__assume_aligned__")) {
      ++p->pos;
      size_t after = skip_balanced(p, p->pos, "(", ")");
      if (after <= p->pos || after >= p->token_count) {
        fail(p, "assume-aligned attribute requires an argument list");
        return false;
      }
      p->pos = after;
    } else if (tok_is(p, p->pos, "indirect_branch") ||
               tok_is(p, p->pos, "__indirect_branch__")) {
      ++p->pos;
      size_t after = skip_balanced(p, p->pos, "(", ")");
      if (after <= p->pos || after >= p->token_count) {
        fail(p, "indirect-branch attribute requires an argument list");
        return false;
      }
      p->pos = after;
    } else if (cleanup && (tok_is(p, p->pos, "cleanup") ||
                           tok_is(p, p->pos, "__cleanup__"))) {
      ++p->pos;
      if (!expect(p, "(") || !tok_ident(*current(p))) {
        fail(p, "cleanup attribute requires a function name");
        return false;
      }
      ny_ctok_t function = *current(p);
      ++p->pos;
      if (!expect(p, ")") || (cleanup->len && !tok_same(*cleanup, function))) {
        fail(p, "cleanup attribute requires one consistent function name");
        return false;
      }
      *cleanup = function;
    } else if (tok_is(p, p->pos, "nonstring") ||
               tok_is(p, p->pos, "__nonstring__")) {
      ++p->pos;
    } else if (tok_is(p, p->pos, "deprecated") ||
               tok_is(p, p->pos, "__deprecated__")) {
      ++p->pos;
      if (accept(p, "(")) {
        if (current(p)->kind != NY_CTOK_STRING) {
          fail(p, "deprecated attribute message must be a string literal");
          return false;
        }
        while (current(p)->kind == NY_CTOK_STRING)
          ++p->pos;
        if (!expect(p, ")")) {
          fail(p, "deprecated attribute requires a string literal message");
          return false;
        }
      }
    } else if (tok_is(p, p->pos, "error") || tok_is(p, p->pos, "__error__") ||
               tok_is(p, p->pos, "warning") ||
               tok_is(p, p->pos, "__warning__")) {
      ++p->pos;
      if (accept(p, "(")) {
        if (current(p)->kind != NY_CTOK_STRING) {
          fail(p, "error/warning attribute message must be a string literal");
          return false;
        }
        while (current(p)->kind == NY_CTOK_STRING)
          ++p->pos;
        if (!expect(p, ")")) {
          fail(p, "error/warning attribute requires a string literal message");
          return false;
        }
      }
    } else if (tok_is(p, p->pos, "__gnu_inline__") ||
               tok_is(p, p->pos, "gnu_inline") ||
               tok_is(p, p->pos, "__unused__") || tok_is(p, p->pos, "unused") ||
               tok_is(p, p->pos, "__used__") || tok_is(p, p->pos, "used") ||
               tok_is(p, p->pos, "__no_instrument_function__") ||
               tok_is(p, p->pos, "no_instrument_function") ||
               tok_is(p, p->pos, "__externally_visible__") ||
               tok_is(p, p->pos, "__no_stack_protector__") ||
               tok_is(p, p->pos, "no_stack_protector") ||
               tok_is(p, p->pos, "__noinline__") ||
               tok_is(p, p->pos, "noinline") ||
               tok_is(p, p->pos, "__noreturn__") ||
               tok_is(p, p->pos, "noreturn") ||
               tok_is(p, p->pos, "__nothrow__") ||
               tok_is(p, p->pos, "nothrow") || tok_is(p, p->pos, "__leaf__") ||
               tok_is(p, p->pos, "leaf") || tok_is(p, p->pos, "__const__") ||
               tok_is(p, p->pos, "const") ||
               tok_is(p, p->pos, "__always_inline__") ||
               tok_is(p, p->pos, "always_inline") ||
               tok_is(p, p->pos, "cold") || tok_is(p, p->pos, "__cold__") ||
               tok_is(p, p->pos, "hot") || tok_is(p, p->pos, "__hot__") ||
               tok_is(p, p->pos, "returns_nonnull") ||
               tok_is(p, p->pos, "__returns_nonnull__") ||
               tok_is(p, p->pos, "returns_twice") ||
               tok_is(p, p->pos, "__returns_twice__") ||
               tok_is(p, p->pos, "__warn_unused_result__") ||
               tok_is(p, p->pos, "warn_unused_result") ||
               tok_is(p, p->pos, "no_sanitize_address") ||
               tok_is(p, p->pos, "__no_sanitize_address__") ||
               tok_is(p, p->pos, "no_sanitize_thread") ||
               tok_is(p, p->pos, "__no_sanitize_thread__") ||
               tok_is(p, p->pos, "no_sanitize_coverage") ||
               tok_is(p, p->pos, "__no_sanitize_coverage__") ||
               tok_is(p, p->pos, "__designated_init__") ||
               tok_is(p, p->pos, "no_sanitize_memory") ||
               tok_is(p, p->pos, "__no_sanitize_memory__") ||
               tok_is(p, p->pos, "flatten") ||
               tok_is(p, p->pos, "__flatten__") ||
               tok_is(p, p->pos, "ms_abi") || tok_is(p, p->pos, "__ms_abi__") ||
               tok_is(p, p->pos, "sysv_abi") ||
               tok_is(p, p->pos, "__sysv_abi__") ||
               tok_is(p, p->pos, "no_profile") ||
               tok_is(p, p->pos, "__no_profile__") ||
               tok_is(p, p->pos, "no_profile_instrument_function") ||
               tok_is(p, p->pos, "__no_profile_instrument_function__") ||
               tok_is(p, p->pos, "noclone") ||
               tok_is(p, p->pos, "__noclone__") ||
               tok_is(p, p->pos, "no_kcsan") ||
               tok_is(p, p->pos, "__no_kcsan__") ||
               tok_is(p, p->pos, "nocf_check") ||
               tok_is(p, p->pos, "__nocf_check__") ||
               tok_is(p, p->pos, "__pure__") || tok_is(p, p->pos, "pure") ||
               tok_is(p, p->pos, "__transparent_union__") ||
               tok_is(p, p->pos, "transparent_union") ||
               tok_is(p, p->pos, "weak") || tok_is(p, p->pos, "__weak__")) {
      ++p->pos;
    } else if (tok_is(p, p->pos, "malloc") || tok_is(p, p->pos, "__malloc__")) {
      ++p->pos;
      if (tok_is(p, p->pos, "(")) {
        size_t after = skip_balanced(p, p->pos, "(", ")");
        if (after <= p->pos + 1 || after >= p->token_count) {
          fail(p, "malloc attribute requires balanced arguments");
          return false;
        }
        p->pos = after;
      }
    } else if (tok_is(p, p->pos, "aligned") ||
               tok_is(p, p->pos, "__aligned__")) {
      ++p->pos;
      if (accept(p, "(")) {
        int64_t parsed = 0;
        if (!parse_global_const_expr(p, &p->pos, &parsed) || parsed <= 0 ||
            !expect(p, ")")) {
          fail(p, "aligned attribute requires a positive constant value");
          return false;
        }
        uint64_t alignment = (uint64_t)parsed;
        if (alignment > SIZE_MAX || (alignment & (alignment - 1)) != 0) {
          fail(p, "type alignment must be a power of two");
          return false;
        }
        if ((size_t)alignment > p->last_type_alignment)
          p->last_type_alignment = (size_t)alignment;
      } else if (p->last_type_alignment < 16) {
        p->last_type_alignment = 16;
      }
    } else {
      int text_len = p->pos < p->token_count && p->tokens[p->pos].len
                         ? (int)p->tokens[p->pos].len
                         : 0;
      if (text_len > 60)
        text_len = 60;
      char message[160];
      snprintf(message, sizeof(message), "unsupported %s: `%.*s`", what,
               text_len, text_len ? p->tokens[p->pos].start : "");
      fail(p, message);
      return false;
    }
  } while (accept(p, ","));
  return true;
}

/*
 * GNU file-scope attribute syntax.  Attributes modify placement or
 * diagnostics only for the frontend's subsection: `__section__("name")` is
 * recorded on the parser so the declaration's collector can act on it.
 */
static bool parse_function_type_attributes(c_parser_t *p) {
  while (accept_attribute(p)) {
    if (!expect(p, "(") || !expect(p, "("))
      return false;
    if (!parse_file_scope_attribute_group(p, &p->last_type_section,
                                          "function type attribute",
                                          &p->last_type_cleanup))
      return false;
    if (!expect(p, ")") || !expect(p, ")"))
      return false;
  }
  return true;
}

/*
 * Trailing GNU attributes after a file-scope declarator, before its
 * initializer or `;`.  Same accepted vocabulary as the specifier position.
 */
static bool parse_trailing_file_scope_attributes(c_parser_t *p,
                                                 ny_ctok_t *section) {
  while (accept_attribute(p)) {
    if (!expect(p, "(") || !expect(p, "("))
      return false;
    if (!parse_file_scope_attribute_group(p, section, "file-scope attribute",
                                          NULL))
      return false;
    if (!expect(p, ")") || !expect(p, ")"))
      return false;
  }
  return true;
}

static size_t function_declaration_attribute(const c_parser_t *p, size_t start,
                                             size_t end, const char *name,
                                             const char *gnu_name) {
  size_t declarator_depth = 0;
  for (size_t i = start; i < end; ++i) {
    if (tok_is(p, i, "(")) {
      ++declarator_depth;
      continue;
    }
    if (tok_is(p, i, ")")) {
      if (declarator_depth)
        --declarator_depth;
      continue;
    }
    if (declarator_depth != 0)
      continue;
    if (tok_is(p, i, "_Noreturn") && strcmp(name, "noreturn") == 0)
      return i;
    if (!tok_is(p, i, "__attribute__") && !tok_is(p, i, "__attribute"))
      continue;
    size_t depth = 0;
    for (size_t j = i + 1; j < end; ++j) {
      if (tok_is(p, j, "(")) {
        ++depth;
      } else if (tok_is(p, j, ")")) {
        if (depth == 0)
          break;
        --depth;
        if (depth == 0) {
          i = j;
          break;
        }
      } else if (depth && (tok_is(p, j, name) || tok_is(p, j, gnu_name))) {
        return j;
      }
    }
  }
  return SIZE_MAX;
}

static bool parse_trailing_local_attributes(c_parser_t *p, ny_ctok_t *section,
                                            ny_ctok_t *cleanup) {
  while (accept_attribute(p)) {
    if (!expect(p, "(") || !expect(p, "("))
      return false;
    if (!parse_file_scope_attribute_group(p, section, "local attribute",
                                          cleanup))
      return false;
    if (!expect(p, ")") || !expect(p, ")"))
      return false;
  }
  return true;
}

/*
 * Replay the field-placement math of parse_tag_fields over an already
 * committed layout.  With `packed` clear the replay is a bit-identical
 * no-op.  With `packed` set the layout follows GCC packed semantics:
 * every member placement aligns to one byte, union members share offset
 * 0, bit-fields take the next available bit across storage-unit
 * boundaries, aggregate sizes stay unchanged (only placement shrinks),
 * and no tail padding is applied.  A member whose declaration carried an
 * explicit aligned(n) keeps that alignment and raises the tag's
 * alignment accordingly.  Promoted anonymous members are rebuilt against
 * the refreshed offsets.
 */
static bool relayout_aggregate_fields(c_parser_t *p, c_agg_type_t *tag,
                                      bool is_union, bool packed) {
  if (!tag || !tag->complete || !packed)
    return true;
  size_t offset = 0;
  size_t size = 0;
  size_t bit_unit_offset = 0;
  size_t bit_unit_size = 0;
  unsigned bits_used = 0;
  bool bit_unit_active = false;
  size_t max_align = 1;
  tag->promoted_field_count = 0;
  for (size_t i = 0; i < tag->field_count; ++i) {
    c_agg_field_t *field = &tag->fields[i];
    size_t layout_align =
        field->explicit_align ? field->align : (packed ? 1 : field->align);
    if (field->kind == C_AGG_FIELD_AGG &&
        !field->name.len /* anonymous inline member */) {
      if (!field->agg)
        continue;
      size_t member_offset = is_union ? 0 : offset;
      field->offset = member_offset;
      if (is_union) {
        if (field->size > size)
          size = field->size;
      } else {
        offset = member_offset + field->size;
      }
      if (!promote_aggregate_fields(p, tag, field->agg, member_offset))
        return false;
      continue;
    }
    if (field->is_bitfield) {
      size_t unit_size = field->size;
      if (is_union) {
        field->offset = 0;
        field->bit_offset = 0;
        if (field->size > size)
          size = field->size;
        continue;
      }
      if (field->bit_width == 0 &&
          !field->name.len /* zero-width padding bit-field */) {
        offset = align_up(offset, layout_align);
        bit_unit_active = false;
        field->offset = offset;
        field->size = 0;
        continue;
      }
      if (!bit_unit_active || bit_unit_size != unit_size ||
          bits_used + field->bit_width > unit_size * 8) {
        bit_unit_offset = offset - offset % layout_align;
        bit_unit_size = unit_size;
        bits_used = (unsigned)((offset - bit_unit_offset) * 8);
        if (bits_used + field->bit_width > unit_size * 8) {
          bit_unit_offset = align_up(offset, layout_align);
          bits_used = 0;
        }
        bit_unit_active = true;
      }
      field->offset = bit_unit_offset;
      field->bit_offset = bits_used;
      bits_used += field->bit_width;
      offset = bit_unit_offset + (bits_used + 7) / 8;
      continue;
    }
    bit_unit_active = false;
    if (is_union) {
      field->offset = 0;
      if (field->size > size)
        size = field->size;
    } else {
      field->offset = align_up(offset, layout_align);
      offset = field->offset + field->size;
    }
    if (field->explicit_align && field->align > max_align)
      max_align = field->align;
  }
  tag->align = max_align;
  tag->size = align_up(is_union ? size : offset, tag->align);
  return true;
}

/*
 * Parse the body of a struct/union definition and commit its layout.
 * Fields follow the same restricted type model as the rest of the
 * frontend: long, double, single-level pointers, nested struct/union
 * tags, and constant-extent arrays of those.
 */
static bool parse_tag_fields(c_parser_t *p, c_agg_type_t *tag, bool is_union) {
  if (tag->complete) {
    fail(p, "duplicate struct/union definition");
    return false;
  }
  size_t offset = 0;
  size_t max_align = 1;
  size_t size = 0;
  size_t bit_unit_offset = 0;
  size_t bit_unit_size = 0;
  unsigned bits_used = 0;
  bool bit_unit_active = false;
  while (!accept(p, "}")) {
    if (p->pos >= p->token_count) {
      fail(p, "unterminated struct/union body");
      return false;
    }
    if (accept(p, ";"))
      continue;
    if (accept(p, "_Static_assert")) {
      if (p->type_lookahead || p->suppress_static_asserts) {
        /*
         * A sizeof/alignof probe only needs the layout; evaluating an
         * assertion whose condition depends on macro machinery can fail
         * without changing the type.
         */
        int depth = 0;
        while (p->pos < p->token_count) {
          if (tok_is(p, p->pos, "(")) {
            ++depth;
          } else if (tok_is(p, p->pos, ")")) {
            if (depth == 0) {
              ++p->pos;
              break;
            }
            if (--depth == 0) {
              ++p->pos;
              break;
            }
          }
          ++p->pos;
        }
        while (p->pos < p->token_count && !tok_is(p, p->pos, ";"))
          ++p->pos;
        (void)accept(p, ";");
        continue;
      }
      int64_t condition = 0;
      size_t sa_pos = p->pos;
      if (!expect(p, "(") || !parse_global_const_expr(p, &p->pos, &condition) ||
          !expect(p, ",")) {
        if (getenv("NYC_KERNEL_COMPAT")) {
          if (p->err)
            p->err[0] = 0;
          p->pos = sa_pos;
          /*
           * best-effort skip: consume to matching );
           */
          int depth = 0;
          bool in_sa = false;
          while (p->pos < p->token_count) {
            if (tok_is(p, p->pos, "(")) {
              ++depth;
              in_sa = true;
            } else if (tok_is(p, p->pos, ")")) {
              if (--depth <= 0 && in_sa) {
                ++p->pos;
                break;
              }
            } else if (depth <= 0 && in_sa && tok_is(p, p->pos, ";"))
              break;
            ++p->pos;
          }
          (void)accept(p, ";");
          continue;
        }
        fail(p, "invalid aggregate _Static_assert constant expression");
        return false;
      }
      bool message_found = false;
      while (current(p)->kind == NY_CTOK_STRING) {
        message_found = true;
        ++p->pos;
      }
      if (!message_found || !expect(p, ")") || !expect(p, ";")) {
        fail(p, "invalid aggregate _Static_assert diagnostic string");
        return false;
      }
      if (!condition && !getenv("NYC_KERNEL_COMPAT")) {
        fail(p, "aggregate _Static_assert condition is false");
        return false;
      }
      continue;
    }
    c_type_t parsed_field_type;
    if (!parse_type(p, &parsed_field_type, true))
      return false;
    c_agg_type_t *field_agg = p->last_type_agg;
    c_type_t field_type = parsed_field_type;
    unsigned inherited_pointers = 0;
    if (p->last_type_base_valid) {
      field_type = p->last_type_base_type;
      inherited_pointers = p->last_type_pointer_count;
    } else {
      for (;;) {
        c_type_t base_type = c_remove_pointer_levels(field_type, 1);
        if (base_type == C_BAD)
          break;
        field_type = base_type;
        ++inherited_pointers;
      }
    }
    if (accept(p, ";")) {
      /*
       * GCC treats a named tag declaration in a member list as a
       * declaration-only member: it introduces no field. Linux uses this
       * form to expose the first concrete type through other header macros.
       */
      if (parsed_field_type == C_AGG && field_type == C_AGG &&
          !inherited_pointers && field_agg && field_agg->name.len &&
          !p->ms_extensions && !p->plan9_extensions &&
          !getenv("NYC_KERNEL_COMPAT"))
        continue;
      if (parsed_field_type != C_AGG || field_type != C_AGG ||
          inherited_pointers || !field_agg || !field_agg->complete ||
          (field_agg->name.len && !p->ms_extensions && !p->plan9_extensions &&
           !getenv("NYC_KERNEL_COMPAT"))) {
        fail(p, "anonymous struct/union member requires a complete aggregate");
        return false;
      }
      if (tag->field_count >= C_AGG_MAX_FIELDS) {
        fail(p, "struct/union field count exceeds the frontend limit");
        return false;
      }
      size_t member_offset = is_union ? 0 : align_up(offset, field_agg->align);
      c_agg_field_t field = {.kind = C_AGG_FIELD_AGG,
                             .offset = member_offset,
                             .size = field_agg->size,
                             .align = field_agg->align,
                             .agg = field_agg};
      tag->fields[tag->field_count++] = field;
      if (is_union) {
        if (field.size > size)
          size = field.size;
      } else {
        offset = member_offset + field.size;
      }
      if (field.align > max_align)
        max_align = field.align;
      if (!promote_aggregate_fields(p, tag, field_agg, member_offset))
        return false;
      continue;
    }
    bool declarator_ok = true;
    do {
      if (tag->field_count >= C_AGG_MAX_FIELDS) {
        fail(p, "struct/union field count exceeds the frontend limit");
        return false;
      }
      c_agg_field_t field;
      memset(&field, 0, sizeof(field));
      size_t requested_alignment = 0;
      bool field_packed = false;
      if (!parse_alignment_attribute(p, &requested_alignment, &field_packed))
        return false;
      if (!parse_field_declarator(p, field_type, field_agg, inherited_pointers,
                                  parsed_field_type == C_AGG_ARRAY_PTR,
                                  &field)) {
        declarator_ok = false;
        break;
      }
      if (p->last_type_alignment)
        field.align = p->last_type_alignment;
      inherited_pointers = 0;
      bool trailing_packed = false;
      if (!parse_alignment_attribute(p, &requested_alignment, &trailing_packed))
        return false;
      field_packed = field_packed || trailing_packed;
      if (requested_alignment > 0) {
        if (requested_alignment > field.align)
          field.align = requested_alignment;
        field.explicit_align = true;
      } else if (field_packed) {
        field.align = 1; /* packed member aligns to the byte unless aligned() */
      }
      if (accept(p, ":")) {
        uint64_t width = 0;
        if (!parse_array_extent_expr(p, 1, &width)) {
          fail(p, "bit-field width must be an integer constant");
          return false;
        }
        size_t unit_size = field.size;
        if (field.kind == C_AGG_FIELD_ARRAY || field.kind == C_AGG_FIELD_AGG ||
            field.kind == C_AGG_FIELD_PTR || field.kind == C_AGG_FIELD_F32 ||
            field.kind == C_AGG_FIELD_F64) {
          fail(p, "bit-field base type must be an integer");
          return false;
        }
        if (width > unit_size * 8 || (width == 0 && field.name.len != 0)) {
          fail(p, "bit-field width is outside its integer base type");
          return false;
        }
        field.is_bitfield = true;
        field.bit_width = (unsigned)width;
        field.size = unit_size;
        if (is_union) {
          field.offset = 0;
          field.bit_offset = 0;
          if (field.size > size)
            size = field.size;
        } else if (width == 0) {
          offset = align_up(offset, field.align);
          bit_unit_active = false;
          field.offset = offset;
          field.size = 0;
        } else {
          if (!bit_unit_active || bit_unit_size != unit_size ||
              bits_used + width > unit_size * 8) {
            bit_unit_offset = offset - offset % field.align;
            bit_unit_size = unit_size;
            bits_used = (unsigned)((offset - bit_unit_offset) * 8);
            if (bits_used + width > unit_size * 8) {
              bit_unit_offset = align_up(offset, field.align);
              bits_used = 0;
            }
            bit_unit_active = true;
          }
          field.offset = bit_unit_offset;
          field.bit_offset = bits_used;
          bits_used += (unsigned)width;
          offset = bit_unit_offset + (bits_used + 7) / 8;
        }
        if (field.align > max_align)
          max_align = field.align;
        tag->fields[tag->field_count++] = field;
        continue;
      }
      bit_unit_active = false;
      if (is_union) {
        field.offset = 0;
        if (field.size > size)
          size = field.size;
      } else {
        field.offset = align_up(offset, field.align);
        offset = field.offset + field.size;
      }
      if (field.align > max_align)
        max_align = field.align;
      tag->fields[tag->field_count++] = field;
    } while (accept(p, ","));
    if (!declarator_ok)
      return false;
    if (!expect(p, ";"))
      return false;
  }
  tag->align = max_align;
  tag->size = align_up(is_union ? size : offset, max_align);
  tag->complete = true;
  return true;
}

static bool parse_tag_body(c_parser_t *p, ny_ctok_t name, bool is_union) {
  c_agg_type_t *visible = find_tag(p, name, is_union);
  c_agg_type_t *tag = visible;
  /*
   * A definition introduces a local tag when it shadows a tag from an
   * enclosing scope.
   */
  if (p->tag_scope_id && visible && visible->scope_id != p->tag_scope_id) {
    if (p->tag_count >= p->tag_capacity) {
      size_t next_capacity = p->tag_capacity ? p->tag_capacity * 2 : 16;
      c_agg_type_t **grown = realloc(p->tags, next_capacity * sizeof(*p->tags));
      if (!grown) {
        fail(p, "out of memory allocating local struct tag");
        return false;
      }
      p->tags = grown;
      p->tag_capacity = next_capacity;
    }
    tag = calloc(1, sizeof(*tag));
    if (!tag) {
      fail(p, "out of memory allocating local struct tag");
      return false;
    }
    tag->name = name;
    tag->scope_id = p->tag_scope_id;
    tag->is_union = is_union;
    p->tags[p->tag_count++] = tag;
  } else if (!tag) {
    tag = declare_tag(p, name, is_union);
  }
  return tag && parse_tag_fields(p, tag, is_union);
}

static size_t skip_balanced(c_parser_t *p, size_t at, const char *left,
                            const char *right) {
  if (!tok_is(p, at, left))
    return at;
  size_t depth = 0;
  for (; at < p->token_count; ++at) {
    if (tok_is(p, at, left))
      ++depth;
    else if (tok_is(p, at, right) && --depth == 0)
      return at + 1;
  }
  return at;
}

static bool function_index_add(c_parser_t *p, size_t index) {
  if (p->function_count > SIZE_MAX / 2)
    return false;
  size_t needed = p->function_count * 2;
  size_t capacity = p->function_index_capacity;
  if (capacity < needed) {
    capacity = capacity ? capacity : 64;
    while (capacity < needed) {
      if (capacity > SIZE_MAX / 2)
        return false;
      capacity *= 2;
    }
    size_t *slots = calloc(capacity, sizeof(*slots));
    if (!slots)
      return false;
    for (size_t i = 0; i < p->function_count; ++i) {
      size_t slot = (size_t)c_name_hash(p->functions[i].name) & (capacity - 1);
      while (slots[slot])
        slot = (slot + 1) & (capacity - 1);
      slots[slot] = i + 1;
    }
    free(p->function_index);
    p->function_index = slots;
    p->function_index_capacity = capacity;
    return true;
  }
  size_t slot = (size_t)c_name_hash(p->functions[index].name) & (capacity - 1);
  while (p->function_index[slot])
    slot = (slot + 1) & (capacity - 1);
  p->function_index[slot] = index + 1;
  return true;
}

static c_function_t *find_function(c_parser_t *p, ny_ctok_t name) {
  if (p->function_index_capacity) {
    size_t mask = p->function_index_capacity - 1;
    size_t slot = (size_t)c_name_hash(name) & mask;
    while (p->function_index[slot]) {
      size_t index = p->function_index[slot] - 1;
      if (tok_same(p->functions[index].name, name))
        return &p->functions[index];
      slot = (slot + 1) & mask;
    }
    return NULL;
  }
  for (size_t i = 0; i < p->function_count; ++i)
    if (tok_same(p->functions[i].name, name))
      return &p->functions[i];
  return NULL;
}

static c_global_t *find_global(c_parser_t *p, ny_ctok_t name) {
  for (size_t i = 0; i < p->global_count; ++i)
    if (tok_same(p->globals[i].name, name))
      return &p->globals[i];
  if (p->global_initializer && tok_same(p->global_initializer->name, name))
    return p->global_initializer;
  return NULL;
}

static bool decode_string_token(ny_ctok_t token, unsigned char **bytes,
                                size_t *len) {
  if (!bytes || !len || token.kind != NY_CTOK_STRING || token.len < 2)
    return false;
  unsigned char *decoded = malloc(token.len);
  if (!decoded)
    return false;
  size_t out = 0;
  for (size_t i = 1; i + 1 < token.len; ++i) {
    unsigned char ch = (unsigned char)token.start[i];
    if (ch == '\\' && i + 2 < token.len) {
      ch = (unsigned char)token.start[++i];
      switch (ch) {
      case 'n':
        ch = '\n';
        break;
      case 'r':
        ch = '\r';
        break;
      case 't':
        ch = '\t';
        break;
      case 'v':
        ch = '\v';
        break;
      case 'f':
        ch = '\f';
        break;
      case 'a':
        ch = '\a';
        break;
      case 'b':
        ch = '\b';
        break;
      case 'x': {
        unsigned value = 0;
        size_t digits = 0;
        while (i + 2 < token.len &&
               isxdigit((unsigned char)token.start[i + 1])) {
          unsigned char digit = (unsigned char)token.start[++i];
          value = value * 16u + (unsigned)(isdigit(digit)
                                               ? digit - '0'
                                               : (tolower(digit) - 'a' + 10));
          ++digits;
        }
        if (!digits) {
          free(decoded);
          return false;
        }
        ch = (unsigned char)value;
        break;
      }
      default:
        if (ch >= '0' && ch <= '7') {
          unsigned value = ch - '0';
          for (unsigned digits = 1;
               digits < 3 && i + 2 < token.len && token.start[i + 1] >= '0' &&
               token.start[i + 1] <= '7';
               ++digits)
            value = value * 8u + (unsigned)(token.start[++i] - '0');
          ch = (unsigned char)value;
        }
        break;
      }
    }
    decoded[out++] = ch;
  }
  decoded[out++] = 0;
  *bytes = decoded;
  *len = out;
  return true;
}

static bool decode_wide_string_token(ny_ctok_t token, unsigned char **bytes,
                                     size_t *byte_len) {
  if (token.kind != NY_CTOK_STRING || token.len < 3 || token.start[0] != 'L' ||
      token.start[1] != '"')
    return false;
  ny_ctok_t narrow = token;
  ++narrow.start;
  --narrow.len;
  unsigned char *characters = NULL;
  size_t character_count = 0;
  if (!decode_string_token(narrow, &characters, &character_count) ||
      character_count > SIZE_MAX / sizeof(uint16_t)) {
    free(characters);
    return false;
  }
  size_t length = character_count * sizeof(uint16_t);
  unsigned char *wide = malloc(length ? length : 1);
  if (!wide) {
    free(characters);
    return false;
  }
  for (size_t i = 0; i < character_count; ++i) {
    uint16_t code_unit = characters[i];
    memcpy(wide + i * sizeof(code_unit), &code_unit, sizeof(code_unit));
  }
  free(characters);
  *bytes = wide;
  *byte_len = length;
  return true;
}

/*
 * Lowering helpers referenced by the earlier assembly lowering blocks.
 */
static int lower_local_load(c_parser_t *p, const c_local_t *local);
static bool lower_local_store(c_parser_t *p, c_local_t *local, int value);
static bool c_ir_nonnegative_constant(const c_parser_t *p, int value,
                                      int64_t *constant);
static int lower_load_field(c_parser_t *p, int address, bool is_f64,
                            bool is_volatile);
static bool lower_store_field(c_parser_t *p, int address, int value,
                              bool is_f64, bool is_volatile);
static bool lower_store_i32_field(c_parser_t *p, int address, int value,
                                  bool is_volatile);
static bool lower_store_u16_field(c_parser_t *p, int address, int value,
                                  c_type_t type, bool is_volatile);

#include "code/lang/c/parser/parts/asm.h"

static bool retain_file_scope_assembly(c_parser_t *p,
                                       const unsigned char *assembly,
                                       size_t length) {
  unsigned char *raw = (unsigned char *)p->raw_file_scope_asm;
  bool ok = append_asm_bytes(&raw, &p->raw_file_scope_asm_len,
                             &p->raw_file_scope_asm_capacity, assembly, length);
  if (ok && length && assembly[length - 1] != '\n')
    ok = append_asm_bytes(&raw, &p->raw_file_scope_asm_len,
                          &p->raw_file_scope_asm_capacity, "\n", 1);
  p->raw_file_scope_asm = (char *)raw;
  if (!ok)
    fail(p, "out of memory retaining file-scope assembly");
  return ok;
}

static bool collect_file_scope_asm(c_parser_t *p) {
  size_t asm_token = p->pos++;
  while (tok_is(p, p->pos, "volatile") || tok_is(p, p->pos, "__volatile__") ||
         tok_is(p, p->pos, "goto"))
    ++p->pos;
  if (!expect(p, "(")) {
    fail(p, "file-scope asm requires a parenthesized string");
    return false;
  }
  unsigned char *assembly = NULL;
  size_t assembly_len = 0, assembly_capacity = 0;
  bool have_string = false;
  while (current(p)->kind == NY_CTOK_STRING) {
    unsigned char *part = NULL;
    size_t part_len = 0;
    if (!decode_string_token(*current(p), &part, &part_len) ||
        (part_len && memchr(part, '\0', part_len - 1)) ||
        !append_asm_bytes(&assembly, &assembly_len, &assembly_capacity, part,
                          part_len ? part_len - 1 : 0)) {
      free(part);
      free(assembly);
      fail(p, "invalid file-scope asm string");
      return false;
    }
    free(part);
    have_string = true;
    ++p->pos;
  }
  if (!have_string || !expect(p, ")") || !expect(p, ";") ||
      !append_asm_bytes(&assembly, &assembly_len, &assembly_capacity, "", 1)) {
    free(assembly);
    fail(p, "file-scope asm requires adjacent string literals and a semicolon");
    return false;
  }
  if (assembly_len == 1) {
    free(assembly);
    return true;
  }
  if (p->allow_raw_file_assembly && p->raw_file_scope_asm) {
    bool ok = retain_file_scope_assembly(p, assembly, assembly_len - 1);
    free(assembly);
    return ok;
  }
  char *assembly_work = strdup((char *)assembly);
  if (!assembly_work) {
    free(assembly);
    fail(p, "out of memory parsing file-scope asm");
    return false;
  }
  c_file_asm_section_t *sections = NULL;
  size_t section_count = 0, section_capacity = 0;
  size_t current = SIZE_MAX, previous = SIZE_MAX;
  size_t stack[32], stack_count = 0;
  size_t local_label_order = 0;
  bool ok = true;
  char rejected_directive[96] = {0};
  size_t default_text = SIZE_MAX;
  char *default_text_name = asm_file_section_name(p, ".text");
  ok = default_text_name &&
       asm_section_get(&sections, &section_count, &section_capacity,
                       default_text_name, 2u | 4u, 0, &default_text);
  current = default_text;
  char *line = assembly_work;
  while (ok && line && *line) {
    char *end = strpbrk(line, "\n;");
    if (end)
      *end = '\0';
    char *text = trim_asm_space(line);
    char *comment = strchr(text, '#');
    if (comment)
      *comment = '\0';
    text = trim_asm_space(text);
    char *colon =
        *text && current != SIZE_MAX ? find_asm_label_colon(text) : NULL;
    if (colon) {
      *colon = '\0';
      char *label = trim_asm_space(text);
      if (!*label) {
        ok = false;
      } else {
        char *label_end = NULL;
        unsigned long numeric = strtoul(label, &label_end, 10);
        if (*label && !*label_end && numeric <= UINT_MAX) {
          sections[current].local_label = (unsigned)numeric;
          sections[current].local_label_offset = sections[current].length;
          sections[current].local_label_order = ++local_label_order;
          sections[current].have_local_label = true;
        } else if (strncmp(sections[current].name, ".export_symbol", 14) == 0 &&
                   strncmp(label, "__export_symbol_", 16) == 0) {
          ok = asm_section_add_symbol(&sections[current], label,
                                      sections[current].length);
        } else if (asm_section_find_symbol(&sections[current], label) &&
                   asm_section_find_symbol(&sections[current], label)->offset ==
                       sections[current].length) {
          /*
           * The exported label points at the current section offset.
           */
        } else if (asm_label_referenced(assembly_work, colon + 1, label)) {
          ok = false;
        } else if (label[0] != '.') {
          /*
           * A C declaration can reference a non-global assembler label.
           */
          ok = asm_section_add_symbol(&sections[current], label,
                                      sections[current].length);
          if (ok)
            asm_section_find_symbol(&sections[current], label)->is_local = true;
        }
      }
      text = trim_asm_space(colon + 1);
    }
    if (!ok) {
      /*
       * A label in an unsupported form was encountered.
       */
    } else if (!*text) {
      /*
       * Empty lines and assembler comments carry no object semantics.
       */
    } else if ((strncmp(text, ".section", 8) == 0 &&
                isspace((unsigned char)text[8])) ||
               (strncmp(text, ".pushsection", 12) == 0 &&
                isspace((unsigned char)text[12]))) {
      bool push = text[1] == 'p';
      char *name = NULL;
      uint64_t flags = 0;
      size_t entsize = 0;
      size_t section_index = SIZE_MAX;
      size_t argument_offset = push ? 12 : 8;
      char *section_arguments = text + argument_offset;
      bool implicit_flags = strchr(section_arguments, ',') == NULL;
      if (!parse_asm_section(section_arguments, &name, &flags, &entsize)) {
        ok = false;
      } else if (implicit_flags && asm_standard_section_flags(name, &flags)) {
        char *section_name = asm_file_section_name(p, name);
        free(name);
        name = section_name;
        ok = name != NULL;
      }
      if (!ok || !asm_section_get(&sections, &section_count, &section_capacity,
                                  name, flags, entsize, &section_index)) {
        ok = false;
      } else {
        if (push) {
          if (stack_count == sizeof(stack) / sizeof(stack[0]))
            ok = false;
          else
            stack[stack_count++] = current;
        } else {
          previous = current;
        }
        current = section_index;
      }
    } else if (strcmp(text, ".previous") == 0) {
      size_t swap = current;
      current = previous;
      previous = swap;
    } else if (strcmp(text, ".popsection") == 0) {
      if (!stack_count)
        ok = false;
      else
        current = stack[--stack_count];
    } else if (strcmp(text, ".text") == 0 || strcmp(text, ".data") == 0 ||
               strcmp(text, ".rodata") == 0 || strcmp(text, ".bss") == 0) {
      const char *name = text;
      size_t section_index = SIZE_MAX;
      uint64_t flags = 0;
      if (!asm_standard_section_flags(name, &flags)) {
        ok = false;
      } else if (strcmp(name, ".text") == 0) {
        section_index = default_text;
      } else {
        char *section_name = asm_file_section_name(p, name);
        if (!section_name ||
            !asm_section_get(&sections, &section_count, &section_capacity,
                             section_name, flags, 0, &section_index)) {
          ok = false;
        }
      }
      if (ok) {
        previous = current;
        current = section_index;
      }
    } else if ((strncmp(text, ".balign", 7) == 0 &&
                isspace((unsigned char)text[7])) ||
               (strncmp(text, ".align", 6) == 0 &&
                isspace((unsigned char)text[6]))) {
      size_t argument_offset = text[1] == 'b' ? 7 : 6;
      char *arguments = trim_asm_space(text + argument_offset);
      char *comma = strchr(arguments, ',');
      if (comma)
        *comma = '\0';
      int64_t requested_value = 0;
      bool requested_valid =
          asm_parse_short_constant(arguments, &requested_value);
      unsigned long requested = requested_valid && requested_value > 0
                                    ? (unsigned long)requested_value
                                    : 0;
      unsigned long fill = 0;
      if (comma) {
        char *fill_text = trim_asm_space(comma + 1);
        char *fill_end = NULL;
        fill = strtoul(fill_text, &fill_end, 0);
        if (fill_end == fill_text || *trim_asm_space(fill_end) ||
            fill > UINT8_MAX)
          ok = false;
      }
      if (!ok || current == SIZE_MAX || !requested_valid || requested == 0 ||
          requested > 4096 || (requested & (requested - 1)) != 0) {
        ok = false;
      } else {
        c_file_asm_section_t *section = &sections[current];
        if (requested > section->alignment)
          section->alignment = requested;
        size_t padding =
            (requested - (section->length & (requested - 1))) & (requested - 1);
        unsigned char padding_bytes[4096];
        memset(padding_bytes, (unsigned char)fill, sizeof(padding_bytes));
        ok = append_asm_bytes(&section->bytes, &section->length,
                              &section->capacity, padding_bytes, padding);
      }
    } else if (strncmp(text, ".skip", 5) == 0 &&
               isspace((unsigned char)text[5])) {
      char *arguments = trim_asm_space(text + 5);
      char *comma = strchr(arguments, ',');
      if (comma)
        *comma = '\0';
      char *count_end = NULL;
      unsigned long count = strtoul(arguments, &count_end, 0);
      unsigned long fill = 0;
      bool valid = count_end != arguments && !*trim_asm_space(count_end) &&
                   count <= 1024 * 1024;
      if (comma) {
        char *fill_text = trim_asm_space(comma + 1);
        char *fill_end = NULL;
        fill = strtoul(fill_text, &fill_end, 0);
        valid = valid && fill_end != fill_text && !*trim_asm_space(fill_end) &&
                fill <= UINT8_MAX;
      }
      if (current == SIZE_MAX || !valid ||
          count > SIZE_MAX - sections[current].length) {
        ok = false;
      } else {
        unsigned char fill_bytes[256];
        memset(fill_bytes, (unsigned char)fill, sizeof(fill_bytes));
        while (ok && count) {
          size_t chunk =
              count < sizeof(fill_bytes) ? (size_t)count : sizeof(fill_bytes);
          ok = append_asm_bytes(&sections[current].bytes,
                                &sections[current].length,
                                &sections[current].capacity, fill_bytes, chunk);
          count -= chunk;
        }
      }
    } else if (strncmp(text, ".long", 5) == 0 &&
               isspace((unsigned char)text[5])) {
      if (current == SIZE_MAX) {
        ok = false;
      } else {
        ok = asm_emit_long_list(&sections[current], sections, section_count,
                                asm_token, text + 5, (char *)assembly,
                                assembly_work, assembly_len - 1);
      }
    } else if (strncmp(text, ".byte", 5) == 0 &&
               isspace((unsigned char)text[5])) {
      ok = current != SIZE_MAX &&
           asm_emit_byte_list(&sections[current], text + 5);
    } else if (strncmp(text, ".short", 6) == 0 &&
               isspace((unsigned char)text[6])) {
      ok = current != SIZE_MAX &&
           asm_emit_short_list(&sections[current], text + 6);
    } else if (strncmp(text, "ret $", 5) == 0 && current != SIZE_MAX) {
      int64_t immediate = 0;
      if (!asm_parse_short_constant(text + 5, &immediate) || immediate < 0 ||
          immediate > UINT16_MAX) {
        ok = false;
      } else {
        uint16_t encoded = (uint16_t)immediate;
        unsigned char instruction[] = {0xc2, (unsigned char)encoded,
                                       (unsigned char)(encoded >> 8)};
        ok = append_asm_bytes(
            &sections[current].bytes, &sections[current].length,
            &sections[current].capacity, instruction, sizeof(instruction));
      }
    } else if (strcmp(text, "ret") == 0 || strcmp(text, "int3") == 0 ||
               strcmp(text, "nop") == 0 || strcmp(text, "endbr64") == 0) {
      if (strcmp(text, "endbr64") == 0) {
        static const unsigned char endbr64[] = {0xf3, 0x0f, 0x1e, 0xfa};
        ok = current != SIZE_MAX &&
             append_asm_bytes(
                 &sections[current].bytes, &sections[current].length,
                 &sections[current].capacity, endbr64, sizeof(endbr64));
        line = end ? end + 1 : NULL;
        continue;
      }
      unsigned char opcode = strcmp(text, "ret") == 0    ? 0xc3
                             : strcmp(text, "int3") == 0 ? 0xcc
                                                         : 0x90;
      ok = current != SIZE_MAX &&
           append_asm_bytes(&sections[current].bytes, &sections[current].length,
                            &sections[current].capacity, &opcode, 1);
    } else if (strncmp(text, "jmp ", 4) == 0 && current != SIZE_MAX) {
      ok =
          asm_emit_pc32_branch(&sections[current], text, (const char *)assembly,
                               assembly_work, assembly_len, false);
    } else if (strncmp(text, ".quad", 5) == 0 &&
               isspace((unsigned char)text[5])) {
      ok = current != SIZE_MAX &&
           asm_emit_quad_list(&sections[current], text + 5, (char *)assembly,
                              assembly_work, assembly_len - 1);
    } else if ((strncmp(text, ".ascii", 6) == 0 &&
                isspace((unsigned char)text[6])) ||
               (strncmp(text, ".asciz", 6) == 0 &&
                isspace((unsigned char)text[6]))) {
      bool terminate = text[5] == 'z';
      ok = current != SIZE_MAX &&
           asm_emit_string(&sections[current], text + 6, terminate);
    } else if (strncmp(text, "movl", 4) == 0 &&
               isspace((unsigned char)text[4])) {
      ok = current != SIZE_MAX && asm_emit_movl_imm32(&sections[current], text);
    } else if (strncmp(text, "pushq $", 7) == 0 && current != SIZE_MAX) {
      ok = asm_emit_pushq_imm32(&sections[current], text, (char *)assembly,
                                assembly_work, assembly_len - 1);
    } else if (strncmp(text, "push", 4) == 0 &&
               (text[4] == 'q' || isspace((unsigned char)text[4])) &&
               current != SIZE_MAX) {
      ok = asm_emit_stack_register(&sections[current], text, false);
    } else if (strncmp(text, "pop", 3) == 0 &&
               (text[3] == 'q' || isspace((unsigned char)text[3])) &&
               current != SIZE_MAX) {
      ok = asm_emit_stack_register(&sections[current], text, true);
    } else if (strcmp(text, "pushq %rsp") == 0 || strcmp(text, "pushfq") == 0 ||
               strcmp(text, "popfq") == 0) {
      const unsigned char opcode = strcmp(text, "pushq %rsp") == 0 ? 0x54
                                   : strcmp(text, "pushfq") == 0   ? 0x9c
                                                                   : 0x9d;
      ok = current != SIZE_MAX &&
           append_asm_bytes(&sections[current].bytes, &sections[current].length,
                            &sections[current].capacity, &opcode, 1);
    } else if (strncmp(text, "xor", 3) == 0 &&
               (isspace((unsigned char)text[3]) || text[3] == 'q') &&
               current != SIZE_MAX) {
      bool wide64 = text[3] == 'q';
      char *operands = trim_asm_space(text + (wide64 ? 4 : 3));
      char *comma = strchr(operands, ',');
      bool valid = comma != NULL;
      if (valid)
        *comma = '\0';
      char *source = trim_asm_space(operands);
      char *destination = valid ? trim_asm_space(comma + 1) : NULL;
      unsigned reg = valid && strcmp(destination, source) == 0
                         ? asm_x64_gpr_index(source, wide64)
                         : 16;
      if (reg >= 16) {
        ok = false;
      } else {
        unsigned char opcode[3];
        size_t opcode_len = 0;
        if (wide64 || reg >= 8)
          opcode[opcode_len++] = (unsigned char)(0x40u | (wide64 ? 8u : 0u) |
                                                 (reg >= 8 ? 5u : 0u));
        opcode[opcode_len++] = 0x31;
        opcode[opcode_len++] =
            (unsigned char)(0xc0u | ((reg & 7u) << 3) | (reg & 7u));
        ok = append_asm_bytes(&sections[current].bytes,
                              &sections[current].length,
                              &sections[current].capacity, opcode, opcode_len);
      }
    } else if (strncmp(text, "mov", 3) == 0 &&
               isspace((unsigned char)text[3]) && current != SIZE_MAX &&
               strchr(text + 3, ',') != NULL) {
      char *operands = trim_asm_space(text + 3);
      char *comma = strchr(operands, ',');
      *comma = '\0';
      char *immediate_text = trim_asm_space(operands);
      char *destination = trim_asm_space(comma + 1);
      unsigned reg = asm_x64_gpr_index(destination, true);
      int64_t value = 0;
      bool valid = immediate_text[0] == '$' && reg < 16 &&
                   asm_parse_short_constant(immediate_text + 1, &value) &&
                   value >= INT32_MIN && value <= INT32_MAX;
      if (!valid) {
        ok = false;
      } else {
        unsigned char opcode[] = {(unsigned char)(0x48u | (reg >= 8 ? 1u : 0u)),
                                  0xc7,
                                  (unsigned char)(0xc0u | (reg & 7u)),
                                  (unsigned char)value,
                                  (unsigned char)((uint32_t)value >> 8),
                                  (unsigned char)((uint32_t)value >> 16),
                                  (unsigned char)((uint32_t)value >> 24)};
        ok = append_asm_bytes(
            &sections[current].bytes, &sections[current].length,
            &sections[current].capacity, opcode, sizeof(opcode));
      }
    } else if (strcmp(text, "syscall") == 0 && current != SIZE_MAX) {
      static const unsigned char opcode[] = {0x0f, 0x05};
      ok =
          append_asm_bytes(&sections[current].bytes, &sections[current].length,
                           &sections[current].capacity, opcode, sizeof(opcode));
    } else if (strncmp(text, "setne", 5) == 0 &&
               isspace((unsigned char)text[5]) && current != SIZE_MAX &&
               strcmp(trim_asm_space(text + 5), "%al") == 0) {
      static const unsigned char opcode[] = {0x0f, 0x95, 0xc0};
      ok =
          append_asm_bytes(&sections[current].bytes, &sections[current].length,
                           &sections[current].capacity, opcode, sizeof(opcode));
    } else if (strncmp(text, "cmpb", 4) == 0 &&
               isspace((unsigned char)text[4]) && current != SIZE_MAX) {
      char *operands = trim_asm_space(text + 4);
      char *comma = strchr(operands, ',');
      bool valid = comma != NULL;
      if (valid)
        *comma = '\0';
      char *immediate = trim_asm_space(operands);
      char *address = valid ? trim_asm_space(comma + 1) : NULL;
      char *paren = address ? strchr(address, '(') : NULL;
      int64_t addend = 0;
      char *symbol = NULL;
      size_t symbol_len = 0;
      if (!paren || strcmp(trim_asm_space(paren), "(%rax)") != 0 ||
          strcmp(immediate, "$0") != 0) {
        valid = false;
      } else {
        *paren = '\0';
        char *expression = trim_asm_space(address);
        char *plus = strchr(expression, '+');
        if (plus) {
          *plus = '\0';
          char *left = trim_asm_space(expression);
          char *right = trim_asm_space(plus + 1);
          char *end = NULL;
          long long numeric = strtoll(left, &end, 0);
          if (end != left && !*trim_asm_space(end)) {
            addend = numeric;
            symbol = right;
          } else {
            numeric = strtoll(right, &end, 0);
            if (end != right || numeric < INT32_MIN || numeric > INT32_MAX)
              valid = false;
            else {
              addend = numeric;
              symbol = left;
            }
          }
        } else {
          symbol = expression;
        }
        if (valid) {
          symbol = trim_asm_space(symbol);
          symbol_len = strlen(symbol);
          valid = symbol_len && symbol >= assembly_work &&
                  symbol + symbol_len <= assembly_work + assembly_len - 1;
          for (size_t i = 0; valid && i < symbol_len; ++i)
            valid = isalnum((unsigned char)symbol[i]) || symbol[i] == '_' ||
                    symbol[i] == '.' || symbol[i] == '$';
        }
      }
      if (valid) {
        static const unsigned char opcode[] = {0x80, 0xb8};
        const unsigned char immediate_byte = 0;
        ok = append_asm_bytes(
                 &sections[current].bytes, &sections[current].length,
                 &sections[current].capacity, opcode, sizeof(opcode)) &&
             asm_add_reloc(&sections[current],
                           (const char *)assembly + (symbol - assembly_work),
                           symbol_len, addend, sizeof(uint32_t),
                           NYC_DATA_RELOC_ABS32S) &&
             append_asm_bytes(&sections[current].bytes,
                              &sections[current].length,
                              &sections[current].capacity, &immediate_byte, 1);
      } else {
        ok = false;
      }
    } else if (strncmp(text, "movq ", 5) == 0 && current != SIZE_MAX &&
               strchr(text + 5, '(') != NULL &&
               strstr(text + 5, "(,%rdi,8)") != NULL) {
      char *operand = trim_asm_space(text + 5);
      char *paren = strstr(operand, "(,%rdi,8)");
      char *comma = strrchr(operand, ',');
      bool valid = paren && comma && comma > paren &&
                   paren[sizeof("(,%rdi,8)") - 1] == ',' &&
                   strcmp(trim_asm_space(comma + 1), "%rax") == 0;
      if (valid) {
        *paren = '\0';
        char *symbol = trim_asm_space(operand);
        size_t symbol_len = strlen(symbol);
        valid = symbol_len && symbol >= assembly_work &&
                symbol + symbol_len <= assembly_work + assembly_len - 1;
        for (size_t i = 0; valid && i < symbol_len; ++i)
          valid = isalnum((unsigned char)symbol[i]) || symbol[i] == '_' ||
                  symbol[i] == '.' || symbol[i] == '$';
        static const unsigned char opcode[] = {0x48, 0x8b, 0x04, 0xfd};
        ok = valid &&
             append_asm_bytes(
                 &sections[current].bytes, &sections[current].length,
                 &sections[current].capacity, opcode, sizeof(opcode)) &&
             asm_add_reloc(&sections[current],
                           (const char *)assembly + (symbol - assembly_work),
                           symbol_len, 0, sizeof(uint32_t),
                           NYC_DATA_RELOC_ABS32S);
      } else {
        ok = false;
      }
    } else if (strncmp(text, "movq ", 5) == 0 && current != SIZE_MAX &&
               strchr(text + 5, ',') != NULL && strchr(text + 5, '(') != NULL) {
      char *operands = text + 5;
      char *comma = strchr(operands, ',');
      *comma = '\0';
      char *source = trim_asm_space(operands);
      char *destination = trim_asm_space(comma + 1);
      char *memory_text = NULL;
      char *register_text = NULL;
      bool load = false;
      if (strchr(source, '(')) {
        memory_text = source;
        register_text = destination;
        load = true;
      } else if (strchr(destination, '(')) {
        memory_text = destination;
        register_text = source;
      }
      unsigned reg =
          register_text ? asm_x64_gpr_index(register_text, true) : 16;
      int32_t displacement = 0;
      if (reg >= 16 || !memory_text ||
          !asm_parse_rsp_displacement(memory_text, &displacement)) {
        ok = false;
      } else {
        unsigned char opcode[] = {
            (unsigned char)(0x48u | (load ? ((reg >> 3) & 1u)
                                          : ((reg >> 3) & 1u) << 2)),
            (unsigned char)(load ? 0x8b : 0x89),
            (unsigned char)(0x84u | ((reg & 7u) << 3)),
            0x24,
            (unsigned char)displacement,
            (unsigned char)((uint32_t)displacement >> 8),
            (unsigned char)((uint32_t)displacement >> 16),
            (unsigned char)((uint32_t)displacement >> 24)};
        ok = append_asm_bytes(
            &sections[current].bytes, &sections[current].length,
            &sections[current].capacity, opcode, sizeof(opcode));
      }
    } else if (strncmp(text, "movq %rsp, %", 11) == 0 && current != SIZE_MAX) {
      char *destination = trim_asm_space(text + 11);
      unsigned reg = asm_x64_gpr_index(destination, true);
      /*
       * AT&T mov source,destination encodes the source in ModRM.reg and the
       * destination in ModRM.r/m for opcode 0x89.
       */
      unsigned char opcode[] = {(unsigned char)(reg >= 8 ? 0x49 : 0x48), 0x89,
                                (unsigned char)(0xe0u | (reg & 7u))};
      ok = reg < 16 && append_asm_bytes(
                           &sections[current].bytes, &sections[current].length,
                           &sections[current].capacity, opcode, sizeof(opcode));
    } else if (strncmp(text, "addq $", 6) == 0 && current != SIZE_MAX) {
      ok = asm_emit_rsp_adjust(&sections[current], text, false);
    } else if (strncmp(text, "subq $", 6) == 0 && current != SIZE_MAX) {
      ok = asm_emit_rsp_adjust(&sections[current], text, true);
    } else if (strncmp(text, "call ", 5) == 0 && current != SIZE_MAX) {
      ok =
          asm_emit_pc32_branch(&sections[current], text, (const char *)assembly,
                               assembly_work, assembly_len, true);
    } else if ((strncmp(text, ".globl", 6) == 0 &&
                isspace((unsigned char)text[6])) ||
               (strncmp(text, ".global", 7) == 0 &&
                isspace((unsigned char)text[7]))) {
      size_t argument_offset = text[6] == ' ' ? 6 : 7;
      char *symbol = trim_asm_space(text + argument_offset);
      if (current == SIZE_MAX || !*symbol || strpbrk(symbol, " \t,") != NULL) {
        ok = false;
      } else {
        ok = asm_section_add_symbol(&sections[current], symbol,
                                    sections[current].length);
        if (ok)
          asm_section_find_symbol(&sections[current], symbol)->is_local = false;
      }
    } else if (strncmp(text, ".type", 5) == 0 &&
               isspace((unsigned char)text[5])) {
      char *comma = strchr(text + 5, ',');
      if (current == SIZE_MAX || !comma) {
        ok = false;
      } else {
        *comma = '\0';
        char *symbol = trim_asm_space(text + 5);
        char *type = trim_asm_space(comma + 1);
        if (strcmp(type, "@function") != 0)
          ok = false;
        else {
          c_file_asm_symbol_t *asm_symbol =
              asm_section_find_symbol(&sections[current], symbol);
          if (!asm_symbol && asm_section_add_symbol(&sections[current], symbol,
                                                    sections[current].length))
            asm_symbol = asm_section_find_symbol(&sections[current], symbol);
          if (!asm_symbol)
            ok = false;
          else
            asm_symbol->function_symbol = true;
        }
      }
    } else if (strncmp(text, ".size", 5) == 0 &&
               isspace((unsigned char)text[5])) {
      /*
       * The emitted symbol extent is the complete section contribution.
       */
    } else {
      ok = false;
    }
    if (!ok)
      snprintf(rejected_directive, sizeof(rejected_directive), "%s", text);
    line = end ? end + 1 : NULL;
  }
  free(assembly_work);
  size_t output_count = 0;
  for (size_t i = 0; i < section_count; ++i) {
    c_file_asm_section_t *section = &sections[i];
    output_count += section->symbol_count;
    if (!section->symbol_count)
      output_count += section->length != 0;
    else if (section->symbols[0].offset != 0)
      ++output_count;
  }
  if (stack_count || !ok || p->global_count > p->global_capacity ||
      output_count > p->global_capacity - p->global_count) {
    if ((!ok || stack_count) &&
        (p->allow_raw_file_assembly || getenv("NYC_KERNEL_COMPAT"))) {
      bool retained = retain_file_scope_assembly(p, assembly, assembly_len - 1);
      for (size_t i = 0; i < section_count; ++i)
        asm_section_dispose(&sections[i]);
      free(sections);
      free(assembly);
      return retained;
    }
    char message[160];
    if (rejected_directive[0])
      snprintf(message, sizeof(message),
               "unsupported file-scope assembler directive or expression "
               "near `%.80s`",
               rejected_directive);
    else
      snprintf(message, sizeof(message),
               "unsupported file-scope asm section state or data");
    for (size_t i = 0; i < section_count; ++i)
      asm_section_dispose(&sections[i]);
    free(sections);
    free(assembly);
    fail(p, message);
    return false;
  }
  c_global_t *pending =
      output_count ? calloc(output_count, sizeof(*pending)) : NULL;
  if (output_count && !pending) {
    for (size_t i = 0; i < section_count; ++i)
      asm_section_dispose(&sections[i]);
    free(sections);
    free(assembly);
    fail(p, "out of memory exporting file-scope asm");
    return false;
  }
  size_t pending_count = 0;
  c_global_t *assembly_owner = NULL;
  bool export_ok = true;
  for (size_t i = 0; export_ok && i < section_count; ++i) {
    c_file_asm_section_t *section = &sections[i];
    size_t first =
        section->symbol_count && section->symbols[0].offset != 0 ? SIZE_MAX : 0;
    size_t symbol_iterations = section->symbol_count
                                   ? section->symbol_count
                                   : (section->length ? 1 : 0);
    for (size_t si = first == SIZE_MAX ? 0 : 0;
         export_ok && si < symbol_iterations + (first == SIZE_MAX ? 1 : 0);
         ++si) {
      bool prefix = first == SIZE_MAX && si == 0;
      size_t symbol_index =
          prefix ? SIZE_MAX : si - (first == SIZE_MAX ? 1 : 0);
      size_t start = prefix ? 0
                     : section->symbol_count
                         ? section->symbols[symbol_index].offset
                         : 0;
      size_t end =
          prefix ? section->symbols[0].offset
          : section->symbol_count && symbol_index + 1 < section->symbol_count
              ? section->symbols[symbol_index + 1].offset
              : section->length;
      if (start > end || end > section->length) {
        export_ok = false;
        break;
      }
      char generated_name[96];
      const char *symbol_name = prefix ? NULL
                                : section->symbol_count
                                    ? section->symbols[symbol_index].name
                                    : NULL;
      int name_len = 0;
      if (symbol_name) {
        name_len = (int)strlen(symbol_name);
      } else {
        uint64_t path_hash = c_source_hash(p->base_path);
        name_len =
            snprintf(generated_name, sizeof(generated_name),
                     "__nyc_asm_blob_%016llx_%zu",
                     (unsigned long long)path_hash, p->asm_global_count++);
      }
      char *owned_name =
          symbol_name ? strdup(symbol_name)
          : name_len >= 0 && (size_t)name_len < sizeof(generated_name)
              ? strdup(generated_name)
              : NULL;
      if (name_len < 0 || (size_t)name_len >= sizeof(generated_name) ||
          !owned_name) {
        free(owned_name);
        export_ok = false;
        break;
      }
      ny_ctok_t duplicate_name = {0};
      size_t external_definition = SIZE_MAX;
      for (size_t g = 0; g < p->global_count; ++g) {
        bool external_callable =
            p->globals[g].is_external && p->globals[g].type == C_FUNC_PTR;
        bool same_name =
            p->globals[g].name.len == (size_t)name_len &&
            memcmp(p->globals[g].name.start, owned_name, (size_t)name_len) == 0;
        if (!same_name || p->globals[g].function_symbol ||
            find_function(p, p->globals[g].name) || external_callable)
          continue;
        if (p->globals[g].is_external) {
          external_definition = g;
          break;
        }
        duplicate_name = p->globals[g].name;
        export_ok = false;
        break;
      }
      if (!export_ok) {
        if (duplicate_name.len) {
          char message[192];
          snprintf(message, sizeof(message),
                   "file-scope asm symbol `%.120s` conflicts with C data",
                   duplicate_name.start);
          fail(p, message);
        }
        free(owned_name);
        break;
      }
      size_t reloc_count = 0;
      for (size_t r = 0; r < section->reloc_count; ++r)
        reloc_count += section->relocs[r].offset >= start &&
                       section->relocs[r].offset < end;
      c_global_reloc_t *relocs =
          reloc_count ? calloc(reloc_count, sizeof(*relocs)) : NULL;
      unsigned char *bytes = end > start ? malloc(end - start) : NULL;
      char *section_name = strdup(section->name);
      if ((reloc_count && !relocs) || (end > start && !bytes) ||
          !section_name) {
        free(relocs);
        free(bytes);
        free(section_name);
        free(owned_name);
        export_ok = false;
        break;
      }
      if (bytes)
        memcpy(bytes, section->bytes + start, end - start);
      size_t reloc_at = 0;
      for (size_t r = 0; r < section->reloc_count; ++r) {
        if (section->relocs[r].offset < start ||
            section->relocs[r].offset >= end)
          continue;
        relocs[reloc_at] = section->relocs[r];
        relocs[reloc_at++].offset -= start;
        section->relocs[r].owned_target = NULL;
      }
      c_global_t generated = (c_global_t){
          .name = {.kind = NY_CTOK_IDENT,
                   .start = owned_name,
                   .len = (size_t)name_len,
                   .line = p->tokens[asm_token].line,
                   .col = p->tokens[asm_token].col},
          .bytes = bytes,
          .owned_name = owned_name,
          .len = end - start,
          .relocs = relocs,
          .reloc_count = reloc_count,
          .reloc_capacity = reloc_count,
          .type = C_U8,
          .is_array = true,
          .array_rank = 1,
          .array_stride = 1,
          .array_shape = {.rank = 1,
                          .extent = {end - start},
                          .element_type = C_U8,
                          .element_size = 1},
          .section = section_name,
          .section_flags = section->flags,
          .alignment = start == 0 ? section->alignment : 1,
          .section_entsize = section->entsize,
          .is_local = symbol_name && section->symbols[symbol_index].is_local,
          .function_symbol =
              symbol_name && section->symbols[symbol_index].function_symbol};
      if (external_definition != SIZE_MAX) {
        generated.name = p->globals[external_definition].name;
        p->globals[external_definition] = generated;
        if (!assembly_owner)
          assembly_owner = &p->globals[external_definition];
      } else {
        pending[pending_count++] = generated;
        if (!assembly_owner)
          assembly_owner = &pending[pending_count - 1];
      }
    }
  }
  if (!export_ok) {
    for (size_t i = 0; i < pending_count; ++i) {
      free(pending[i].bytes);
      free(pending[i].owned_name);
      free(pending[i].section);
      for (size_t r = 0; r < pending[i].reloc_count; ++r)
        free(pending[i].relocs[r].owned_target);
      free(pending[i].relocs);
    }
    for (size_t i = 0; i < section_count; ++i)
      asm_section_dispose(&sections[i]);
    free(pending);
    free(sections);
    free(assembly);
    if (!p->err[0])
      fail(p, "duplicate or invalid file-scope asm symbol");
    return false;
  }
  if (assembly_owner)
    assembly_owner->owned_assembly = (char *)assembly;
  for (size_t i = 0; i < pending_count; ++i)
    p->globals[p->global_count++] = pending[i];
  if (!assembly_owner)
    free(assembly);
  for (size_t i = 0; i < section_count; ++i)
    asm_section_dispose(&sections[i]);
  free(pending);
  free(sections);
  return true;
}

static bool skip_file_scope_attributes(c_parser_t *p, size_t *at);
static bool try_initialize_static_aggregate_compound_literal(c_parser_t *p,
                                                             c_global_t *data,
                                                             size_t offset,
                                                             bool *matched);
static bool try_initialize_static_array_compound_literal(c_parser_t *p,
                                                         c_global_t *data,
                                                         size_t offset,
                                                         bool *matched);
static size_t count_global_aggregate_array_initializer(const c_parser_t *p,
                                                       size_t open);
static bool parse_static_array_initializer(
    c_parser_t *p, c_type_t type, c_global_t *data, unsigned char *bytes,
    size_t element_count, size_t *element_index, unsigned depth,
    bool allow_designators, size_t designator_stride);
static bool parse_static_aggregate_array_initializer(
    c_parser_t *p, c_global_t *data, c_agg_type_t *aggregate,
    unsigned char *bytes, size_t element_count, size_t *element_index);
static bool parse_static_array_compound_literal_size(c_parser_t *p,
                                                     uint64_t *size,
                                                     bool *matched);

static bool try_collect_string_pointer_array(c_parser_t *p, bool *matched,
                                             bool local_array) {
  *matched = false;
  size_t at = p->pos;
  while (tok_is(p, at, "static") || tok_is(p, at, "extern") ||
         tok_is(p, at, "const"))
    ++at;
  if (tok_is(p, at, "unsigned") && tok_is(p, at + 1, "char"))
    at += 2;
  else if (tok_is(p, at, "char"))
    ++at;
  else
    return true;
  if (tok_is(p, at, "const"))
    ++at;
  if (!tok_is(p, at++, "*"))
    return true;
  bool pointer_const = tok_is(p, at, "const");
  if (pointer_const)
    ++at;
  c_parser_t probe = *p;
  char probe_error[1] = {0};
  probe.err = probe_error;
  probe.err_len = sizeof(probe_error);
  probe.pos = at;
  c_array_declarator_t declarator = {0};
  if (!parse_array_declarator(&probe, &declarator, 0) || !declarator.array)
    return true;
  size_t initializer_pos = probe.pos;
  if (!skip_file_scope_attributes(&probe, &initializer_pos) ||
      !tok_is(&probe, initializer_pos, "=") ||
      !tok_is(&probe, initializer_pos + 1, "{"))
    return true;
  *matched = true;
  size_t pointer_size = c_type_size(p->abi, C_VOID_PTR);
  ny_ctok_t name = declarator.name;
  at = initializer_pos + 2;
  size_t declared_bytes = 0;
  if (!declarator.incomplete) {
    if (declarator.extent > SIZE_MAX / pointer_size) {
      fail(p, "C string-pointer array extent is too large");
      return false;
    }
    declared_bytes = (size_t)declarator.extent * pointer_size;
  }

  c_global_t global = {.name = name,
                       .type = C_CHAR_PTR,
                       .is_array = true,
                       .is_pointer_array = true,
                       .is_const = pointer_const};
  size_t capacity = 0;
  size_t next_index = 0;
  while (!tok_is(p, at, "}")) {
    size_t element_index = next_index;
    size_t range_end_index = element_index;
    if (tok_is(p, at, "[")) {
      size_t designator_pos = at + 1;
      int64_t designator = 0;
      if (!parse_global_const_expr(p, &designator_pos, &designator) ||
          designator < 0) {
        fail(p, "C string-pointer array designator must be a nonnegative "
                "constant index");
        goto error;
      }
      element_index = (size_t)designator;
      range_end_index = element_index;
      if (tok_is(p, designator_pos, "...")) {
        ++designator_pos;
        int64_t range_end = 0;
        if (!parse_global_const_expr(p, &designator_pos, &range_end) ||
            range_end < designator ||
            (uint64_t)range_end >= SIZE_MAX / pointer_size) {
          fail(p, "C string-pointer array range designator is invalid");
          goto error;
        }
        range_end_index = (size_t)range_end;
      }
      if (!tok_is(p, designator_pos, "]") ||
          !tok_is(p, designator_pos + 1, "=")) {
        fail(p, "C string-pointer array designator requires '='");
        goto error;
      }
      at = designator_pos + 2;
    }
    if (range_end_index >= SIZE_MAX / pointer_size ||
        (!declarator.incomplete &&
         range_end_index >= (size_t)declarator.extent)) {
      fail(p, "C string-pointer array designator is out of range");
      goto error;
    }
    size_t required_bytes = (range_end_index + 1) * pointer_size;
    if (required_bytes > global.len) {
      unsigned char *grown_bytes = realloc(global.bytes, required_bytes);
      if (!grown_bytes) {
        fail(p, "out of memory growing C string-pointer array");
        goto error;
      }
      global.bytes = grown_bytes;
      memset(global.bytes + global.len, 0, required_bytes - global.len);
      global.len = required_bytes;
    }
    size_t element_offset = element_index * pointer_size;
    next_index = range_end_index + 1;
    size_t null_end = at + 1;
    bool null_element = tok_is(p, at, "0") || tok_is(p, at, "NULL");
    if (tok_is(p, at, "(")) {
      null_end = skip_balanced(p, at, "(", ")");
      bool has_void = false;
      bool has_pointer = false;
      bool has_zero = false;
      for (size_t i = at; i < null_end; ++i) {
        has_void |= tok_is(p, i, "void");
        has_pointer |= tok_is(p, i, "*");
        has_zero |= tok_is(p, i, "0");
      }
      null_element = null_end > at + 1 && has_void && has_pointer && has_zero;
    }
    if (null_element) {
      memset(global.bytes + element_offset, 0,
             (range_end_index - element_index + 1) * pointer_size);
      for (size_t i = 0; i < global.reloc_count;) {
        if (global.relocs[i].offset < element_offset ||
            global.relocs[i].offset >= required_bytes) {
          ++i;
          continue;
        }
        memmove(&global.relocs[i], &global.relocs[i + 1],
                (global.reloc_count - i - 1) * sizeof(*global.relocs));
        --global.reloc_count;
      }
      at = tok_is(p, at, "0") || tok_is(p, at, "NULL") ? at + 1 : null_end;
    } else {
      if (p->tokens[at].kind != NY_CTOK_STRING && !tok_is(p, at, "(")) {
        p->pos = at;
        if (!initialize_data_pointer(p, &global, element_offset, global.bytes))
          goto error;
        at = p->pos;
      } else {
        unsigned char *string = NULL;
        size_t string_len = 0;
        while (p->tokens[at].kind == NY_CTOK_STRING || tok_is(p, at, "(") ||
               tok_is(p, at, "CTIMEOPT_VAL") ||
               tok_is(p, at, "CTIMEOPT_VAL2")) {
          unsigned char *part = NULL;
          size_t part_len = 0;
          if (tok_is(p, at, "(")) {
            size_t cast_end = at + 1;
            while (cast_end < p->token_count && !tok_is(p, cast_end, ")"))
              ++cast_end;
            if (cast_end == p->token_count || cast_end + 1 >= p->token_count ||
                p->tokens[cast_end + 1].kind != NY_CTOK_STRING) {
              free(string);
              fail(p, "unsupported cast in C string-pointer array");
              goto error;
            }
            at = cast_end + 1;
          }
          if (p->tokens[at].kind == NY_CTOK_STRING) {
            if (!decode_string_token(p->tokens[at++], &part, &part_len)) {
              free(string);
              fail(p, "invalid string literal in C pointer array");
              goto error;
            }
          } else {
            ++at;
            if (!tok_is(p, at, "(")) {
              free(string);
              char detail[160];
              snprintf(detail, sizeof(detail),
                       "unsupported compile-option string macro argument near "
                       "`%.64s`",
                       p->tokens[at].start ? p->tokens[at].start : "");
              fail(p, detail);
              goto error;
            }
            size_t argument_begin = at + 1;
            size_t argument_end = argument_begin;
            unsigned depth = 0;
            for (; argument_end < p->token_count; ++argument_end) {
              if (tok_is(p, argument_end, "("))
                ++depth;
              else if (tok_is(p, argument_end, ")")) {
                if (!depth)
                  break;
                --depth;
              }
            }
            if (argument_end == p->token_count) {
              free(string);
              fail(p, "unterminated compile-option string macro argument");
              goto error;
            }
            at = argument_end + 1;
            bool single_ident = argument_end == argument_begin + 1 &&
                                tok_ident(p->tokens[argument_begin]);
            c_pp_constant_t *constant = NULL;
            for (size_t i = 0; i < p->pp_constant_count; ++i) {
              c_pp_constant_t *candidate = &p->pp_constants[i];
              if (single_ident &&
                  candidate->name_len == p->tokens[argument_begin].len &&
                  memcmp(candidate->name, p->tokens[argument_begin].start,
                         candidate->name_len) == 0) {
                constant = candidate;
                break;
              }
            }
            char literal[128];
            int written;
            if (constant && constant->integer_value) {
              written = snprintf(literal, sizeof(literal), "%lld",
                                 (long long)constant->value);
            } else if (constant && constant->replacement_len) {
              written = snprintf(literal, sizeof(literal), "%.*s",
                                 (int)constant->replacement_len,
                                 constant->replacement);
            } else {
              size_t used = 0;
              for (size_t i = argument_begin; i < argument_end; ++i) {
                ny_ctok_t token = p->tokens[i];
                bool prev_word =
                    used &&
                    ((literal[used - 1] >= 'a' && literal[used - 1] <= 'z') ||
                     (literal[used - 1] >= 'A' && literal[used - 1] <= 'Z') ||
                     (literal[used - 1] >= '0' && literal[used - 1] <= '9') ||
                     literal[used - 1] == '_');
                bool curr_word =
                    token.len &&
                    ((token.start[0] >= 'a' && token.start[0] <= 'z') ||
                     (token.start[0] >= 'A' && token.start[0] <= 'Z') ||
                     (token.start[0] >= '0' && token.start[0] <= '9') ||
                     token.start[0] == '_');
                size_t extra = token.len + (prev_word && curr_word ? 1 : 0);
                if (extra >= sizeof(literal) - used) {
                  used = sizeof(literal);
                  break;
                }
                if (prev_word && curr_word)
                  literal[used++] = ' ';
                memcpy(literal + used, token.start, token.len);
                used += token.len;
              }
              written = used < sizeof(literal) ? (int)used : -1;
              if (written >= 0)
                literal[written] = '\0';
            }
            if (written < 0 || (size_t)written >= sizeof(literal)) {
              free(string);
              fail(p, "compile-option string macro value is too long");
              goto error;
            }
            part_len = (size_t)written + 1;
            part = malloc(part_len);
            if (!part) {
              free(string);
              fail(p, "out of memory expanding compile-option string macro");
              goto error;
            }
            memcpy(part, literal, part_len);
          }
          size_t old_content = string_len ? string_len - 1 : 0;
          size_t part_content = part_len - 1;
          if (part_content > SIZE_MAX - old_content - 1) {
            free(part);
            free(string);
            fail(p, "C string-pointer array element is too large");
            goto error;
          }
          unsigned char *joined =
              realloc(string, old_content + part_content + 1);
          if (!joined) {
            free(part);
            free(string);
            fail(p, "out of memory concatenating C string literals");
            goto error;
          }
          string = joined;
          memcpy(string + old_content, part, part_content + 1);
          string_len = old_content + part_content + 1;
          free(part);
        }
        memset(global.bytes + element_offset, 0, pointer_size);
        size_t relocation_index = 0;
        while (relocation_index < global.reloc_count &&
               global.relocs[relocation_index].offset != element_offset)
          ++relocation_index;
        if (relocation_index == global.reloc_count &&
            global.reloc_count == capacity) {
          size_t next_capacity = capacity ? capacity * 2 : 8;
          if (next_capacity > SIZE_MAX / sizeof(*global.relocs)) {
            free(string);
            fail(p, "C string-pointer relocation table is too large");
            goto error;
          }
          c_global_reloc_t *grown_relocs =
              realloc(global.relocs, next_capacity * sizeof(*global.relocs));
          if (!grown_relocs) {
            free(string);
            fail(p, "out of memory allocating C string-pointer relocations");
            goto error;
          }
          global.relocs = grown_relocs;
          capacity = next_capacity;
        }
        if (p->global_count >= p->global_capacity) {
          free(string);
          fail(p, "C frontend global data table capacity exceeded");
          goto error;
        }
        char generated_name[48];
        int written = snprintf(generated_name, sizeof(generated_name),
                               "__nyc_string_%zu", p->global_count);
        if (written < 0 || (size_t)written >= sizeof(generated_name)) {
          free(string);
          fail(p, "generated C string symbol exceeds frontend limit");
          goto error;
        }
        char *owned_name = malloc((size_t)written + 1);
        if (!owned_name) {
          free(string);
          fail(p, "out of memory naming C string literal");
          goto error;
        }
        memcpy(owned_name, generated_name, (size_t)written + 1);
        ny_ctok_t string_name = {
            .kind = NY_CTOK_IDENT, .start = owned_name, .len = (size_t)written};
        p->globals[p->global_count++] = (c_global_t){.name = string_name,
                                                     .bytes = string,
                                                     .owned_name = owned_name,
                                                     .len = string_len,
                                                     .type = C_CHAR_PTR,
                                                     .is_array = true,
                                                     .is_const = true};
        if (relocation_index == global.reloc_count)
          global.relocs[global.reloc_count++] = (c_global_reloc_t){
              .offset = element_offset,
              .target = string_name,
              .width = pointer_size,
              .type = p->abi == NYC_ABI_I386 ? NYC_DATA_RELOC_ABS32
                                             : NYC_DATA_RELOC_ABS64};
        else
          global.relocs[relocation_index].target = string_name;
        for (size_t index = element_index + 1; index <= range_end_index;
             ++index) {
          if (global.reloc_count == capacity) {
            size_t next_capacity = capacity ? capacity * 2 : 8;
            if (next_capacity > SIZE_MAX / sizeof(*global.relocs)) {
              fail(p, "C string-pointer relocation table is too large");
              goto error;
            }
            c_global_reloc_t *grown_relocs =
                realloc(global.relocs, next_capacity * sizeof(*global.relocs));
            if (!grown_relocs) {
              fail(p, "out of memory growing C string-pointer relocations");
              goto error;
            }
            global.relocs = grown_relocs;
            capacity = next_capacity;
          }
          global.relocs[global.reloc_count++] = (c_global_reloc_t){
              .offset = index * pointer_size,
              .target = string_name,
              .width = pointer_size,
              .type = p->abi == NYC_ABI_I386 ? NYC_DATA_RELOC_ABS32
                                             : NYC_DATA_RELOC_ABS64};
        }
      }
    }
    if (tok_is(p, at, ",")) {
      ++at;
      continue;
    }
    if (!tok_is(p, at, "}")) {
      char detail[160];
      snprintf(detail, sizeof(detail),
               "expected ',' or '}' in C string-pointer array near `%.64s`",
               p->tokens[at].start ? p->tokens[at].start : "");
      fail(p, detail);
      goto error;
    }
  }
  if (declared_bytes && global.len < declared_bytes) {
    unsigned char *grown = realloc(global.bytes, declared_bytes);
    if (!grown) {
      fail(p, "out of memory padding C string-pointer array");
      goto error;
    }
    global.bytes = grown;
    memset(global.bytes + global.len, 0, declared_bytes - global.len);
    global.len = declared_bytes;
  }
  ++at;
  if (!tok_is(p, at, ";")) {
    fail(p, "expected ';' after C string-pointer array");
    goto error;
  }
  c_global_t *previous = local_array ? NULL : find_global(p, name);
  if (previous) {
    if (!previous->is_external || !previous->is_array ||
        !previous->is_pointer_array ||
        (previous->type != C_CHAR_PTR && previous->type != C_I8_PTR) ||
        previous->is_const != pointer_const ||
        (previous->len && previous->len != global.len)) {
      fail(p, "conflicting C string-pointer array definition");
      goto error;
    }
    /*
     * Preserve the signedness from a prior `char *` declaration.
     */
    global.type = previous->type;
  } else if ((local_array && find_local(p, name)) ||
             (!local_array && p->global_count >= p->global_capacity) ||
             (local_array && (p->local_count >= p->local_capacity ||
                              p->next_local == SIZE_MAX))) {
    fail(p, "C frontend global data table capacity exceeded");
    goto error;
  }
  if (local_array) {
    char generated_name[48];
    int written = snprintf(generated_name, sizeof(generated_name),
                           "__nyc_static_ptrs_%zu", p->global_count);
    char *owned_name = written > 0 && (size_t)written < sizeof(generated_name)
                           ? strdup(generated_name)
                           : NULL;
    if (!owned_name) {
      fail(p, "out of memory naming static C string-pointer array");
      goto error;
    }
    global.name = (ny_ctok_t){
        .kind = NY_CTOK_IDENT, .start = owned_name, .len = (size_t)written};
    global.owned_name = owned_name;
  }
  if (previous) {
    free(previous->bytes);
    free(previous->relocs);
    previous->bytes = global.bytes;
    previous->len = global.len;
    previous->relocs = global.relocs;
    previous->reloc_count = global.reloc_count;
    previous->reloc_capacity = global.reloc_capacity;
    previous->is_external = false;
    previous->array_incomplete = false;
    previous->is_tentative = false;
  } else {
    p->globals[p->global_count++] = global;
  }
  if (local_array) {
    p->locals[p->local_count++] =
        (c_local_t){.name = name,
                    .static_name = global.name,
                    .type = C_CHAR_PTR,
                    .slot = p->next_local++,
                    .is_const = pointer_const,
                    .array_count = global.len / pointer_size,
                    .array_rank = 1,
                    .array_extent = {global.len / pointer_size, 0}};
  }
  p->pos = at + 1;
  return true;

error:
  free(global.bytes);
  free(global.relocs);
  return false;
}

/*
 * File-scope `__attribute__ ((...))` blocks inside a manual qualifier walk.
 * `__section__("name")` records its string-literal token on the parser so
 * the declaration's collector can act on it; anything the attribute parser
 * rejects leaves the declaration untouched (another collector reports it).
 * Parser cursor and error state are restored either way.
 */
static bool skip_file_scope_attributes(c_parser_t *p, size_t *at) {
  while (tok_is_attribute(p, *at)) {
    size_t saved_pos = p->pos;
    char *saved_err = p->err;
    size_t saved_err_len = p->err_len;
    char probe_error[1] = {0};
    p->err = probe_error;
    p->err_len = sizeof(probe_error);
    p->pos = *at;
    bool ok = parse_function_type_attributes(p);
    *at = p->pos;
    p->pos = saved_pos;
    p->err = saved_err;
    p->err_len = saved_err_len;
    if (!ok)
      return false;
  }
  return true;
}

static bool try_collect_char_array(c_parser_t *p, bool *matched) {
  size_t at = p->pos;
  *matched = false;
  /*
   * This collector runs before the generic declaration parser.  Do not let a
   * section captured from the preceding declaration attach to this array.
   */
  p->last_type_section = (ny_ctok_t){0};
  p->last_function_alias = (ny_ctok_t){0};
  bool is_external = false;
  bool is_const = false;
  bool attributes_ok = true;
  while (attributes_ok &&
         (tok_is(p, at, "const") || tok_is(p, at, "extern") ||
          tok_is(p, at, "static") || tok_is(p, at, "volatile") ||
          tok_is_attribute(p, at))) {
    if (tok_is_attribute(p, at)) {
      attributes_ok = skip_file_scope_attributes(p, &at);
      continue;
    }
    is_external |= tok_is(p, at, "extern");
    is_const |= tok_is(p, at, "const");
    ++at;
  }
  if (!attributes_ok)
    return true;
  c_type_t array_type = C_CHAR_PTR;
  if ((tok_is(p, at, "unsigned") || tok_is(p, at, "signed")) &&
      tok_is(p, at + 1, "char")) {
    array_type = tok_is(p, at, "unsigned") ? C_U8 : C_CHAR;
    at += 2;
  } else if (tok_is(p, at, "char")) {
    ++at;
  } else {
    const c_typedef_t *alias =
        tok_ident(p->tokens[at]) ? find_typedef(p, p->tokens[at]) : NULL;
    if (!alias || alias->is_array ||
        (alias->type != C_U8 && alias->type != C_CHAR))
      return true;
    array_type = alias->type;
    is_const |= alias->is_const;
    ++at;
  }
  /*
   * Specifier-position attributes sit between the base type and the
   * declarator (`extern char __initdata boot_command_line[];`).
   */
  if (!skip_file_scope_attributes(p, &at))
    return true;
  c_parser_t probe = *p;
  char probe_error[1] = {0};
  probe.err = probe_error;
  probe.err_len = sizeof(probe_error);
  probe.pos = at;
  c_array_declarator_t declarator = {0};
  if (!parse_array_declarator(&probe, &declarator, 0) || !declarator.array)
    return true;
  *matched = true;
  ny_ctok_t name = declarator.name;
  bool incomplete = declarator.incomplete;
  if (declarator.extent > SIZE_MAX) {
    fail(p, "character array extent exceeds the target size");
    return false;
  }
  size_t declared_size = (size_t)declarator.extent;
  at = probe.pos;
  /*
   * Trailing attributes between `[]` and `;`/`=` also land here.
   */
  if (!skip_file_scope_attributes(p, &at)) {
    *matched = false;
    return true;
  }
  ny_ctok_t section_token = p->last_type_section;
  p->last_type_section = (ny_ctok_t){0};
  ny_ctok_t alias_target = p->last_function_alias;
  p->last_function_alias = (ny_ctok_t){0};
  char section_buf[64] = {0};
  if (section_token.len &&
      !copy_quoted_token(p, section_token, section_buf, sizeof(section_buf)))
    return false;
  if (alias_target.len && (!is_external || !tok_is(p, at, ";"))) {
    fail(p, "C character-array alias must be a single extern declaration");
    return false;
  }
  c_global_t *previous = find_global(p, name);
  if (is_external && (tok_is(p, at, ";") || tok_is(p, at, ","))) {
    p->pos = at;
    for (;;) {
      previous = find_global(p, name);
      if (previous) {
        if (!previous->is_array || previous->type != array_type ||
            previous->is_const != is_const ||
            (!incomplete && !previous->array_incomplete &&
             previous->len != declared_size)) {
          fail(p, "incompatible external C char-array declaration");
          return false;
        }
        if (alias_target.len && previous->alias_target.len &&
            !tok_same(previous->alias_target, alias_target)) {
          fail(p, "conflicting C character-array alias declarations");
          return false;
        }
        if (alias_target.len)
          previous->alias_target = alias_target;
        if (!incomplete && previous->array_incomplete) {
          if (!previous->is_external) {
            unsigned char *grown =
                realloc(previous->bytes, declared_size ? declared_size : 1);
            if (!grown) {
              fail(p, "out of memory completing a character array");
              return false;
            }
            if (declared_size > previous->len)
              memset(grown + previous->len, 0, declared_size - previous->len);
            previous->bytes = grown;
          }
          previous->len = declared_size;
          previous->array_incomplete = false;
        }
      } else {
        if (p->global_count == p->global_capacity) {
          fail(p, "C frontend global data table capacity exceeded");
          return false;
        }
        char *section_copy = section_buf[0] ? strdup(section_buf) : NULL;
        if (section_buf[0] && !section_copy) {
          fail(p, "out of memory storing a C data section name");
          return false;
        }
        p->globals[p->global_count++] =
            (c_global_t){.name = name,
                         .len = declared_size,
                         .type = array_type,
                         .section = section_copy,
                         .is_array = true,
                         .array_incomplete = incomplete,
                         .is_signed_array = array_type == C_CHAR,
                         .is_const = is_const,
                         .alias_target = alias_target,
                         .is_external = true};
      }
      if (previous && previous->section && section_buf[0] &&
          strcmp(previous->section, section_buf) != 0) {
        fail(p, "conflicting section attributes on a file-scope C object");
        return false;
      }
      if (previous && !previous->section && section_buf[0]) {
        previous->section = strdup(section_buf);
        if (!previous->section) {
          fail(p, "out of memory storing a C data section name");
          return false;
        }
      }
      if (!accept(p, ","))
        break;
      c_array_declarator_t next = {0};
      if (!parse_array_declarator(p, &next, 0) || !next.array ||
          next.extent > SIZE_MAX) {
        fail(p, "external C char-array declaration requires array names");
        return false;
      }
      name = next.name;
      declared_size = (size_t)next.extent;
      incomplete = next.incomplete;
      section_buf[0] = '\0';
      ny_ctok_t next_section = {0};
      if (!parse_trailing_file_scope_attributes(p, &next_section) ||
          (next_section.len && !copy_quoted_token(p, next_section, section_buf,
                                                  sizeof(section_buf))))
        return false;
    }
    return expect(p, ";");
  }
  bool has_initializer = tok_is(p, at, "=");
  if (alias_target.len && (!is_external || has_initializer)) {
    fail(p, "C character-array alias must not have an initializer");
    return false;
  }
  if ((!has_initializer && !tok_is(p, at, ";")) ||
      (has_initializer && tok_is(p, at + 1, "{"))) {
    *matched = false;
    return true;
  }
  unsigned char *decoded = NULL;
  size_t decoded_len = 0;
  if (!has_initializer) {
    if (incomplete)
      declared_size = previous && previous->len ? previous->len : 1;
    decoded = calloc(declared_size ? declared_size : 1, 1);
    if (!decoded) {
      fail(p, "out of memory allocating file-scope character array");
      return false;
    }
    decoded_len = declared_size;
  } else {
    ++at;
  }
  while (has_initializer && p->tokens[at].kind == NY_CTOK_STRING) {
    unsigned char *part = NULL;
    size_t part_len = 0;
    if (!decode_string_token(p->tokens[at++], &part, &part_len)) {
      free(decoded);
      fail(p, "invalid file-scope C string initializer");
      return false;
    }
    size_t content_len = part_len - 1;
    size_t old_content = decoded_len ? decoded_len - 1 : 0;
    if (content_len > SIZE_MAX - old_content - 1) {
      free(part);
      free(decoded);
      fail(p, "file-scope C string initializer is too large");
      return false;
    }
    unsigned char *grown = realloc(decoded, old_content + content_len + 1);
    if (!grown) {
      free(part);
      free(decoded);
      fail(p, "out of memory decoding file-scope C string");
      return false;
    }
    decoded = grown;
    memcpy(decoded + old_content, part, content_len + 1);
    decoded_len = old_content + content_len + 1;
    free(part);
  }
  if (!decoded || !tok_is(p, at, ";")) {
    if (getenv("NYC_KERNEL_COMPAT") && has_initializer) {
      if (p->err)
        p->err[0] = 0;
      /*
       * best-effort: skip initializer tokens until ;
       */
      int d = 0;
      while (at < p->token_count && !tok_is(p, at, ";")) {
        if (tok_is(p, at, "(") || tok_is(p, at, "[") || tok_is(p, at, "{"))
          ++d;
        else if (tok_is(p, at, ")") || tok_is(p, at, "]") ||
                 tok_is(p, at, "}")) {
          if (d == 0)
            break;
          --d;
        }
        ++at;
      }
      if (decoded)
        free(decoded);
      decoded = calloc(declared_size ? declared_size : 1, 1);
      if (!decoded) {
        fail(p, "out of memory allocating file-scope character array");
        return false;
      }
      decoded_len = declared_size;
    } else {
      free(decoded);
      fail(p, "file-scope char arrays require a string initializer");
      return false;
    }
  }
  if (!incomplete) {
    if (has_initializer && decoded_len - 1 > declared_size) {
      free(decoded);
      fail(p, "string initializer exceeds declared character array size");
      return false;
    }
    if (declared_size > decoded_len) {
      unsigned char *grown = realloc(decoded, declared_size);
      if (!grown) {
        free(decoded);
        fail(p, "out of memory padding character array initializer");
        return false;
      }
      decoded = grown;
      memset(decoded + decoded_len, 0, declared_size - decoded_len);
    }
    decoded_len = declared_size;
  }
  if (previous &&
      ((!previous->is_external && !previous->is_tentative) ||
       !previous->is_array || previous->type != array_type ||
       previous->is_const != is_const ||
       (previous->len != decoded_len && !previous->array_incomplete))) {
    free(decoded);
    char message[160];
    snprintf(message, sizeof(message),
             "incompatible definition of external character array `%.100s`",
             name.start);
    fail(p, message);
    return false;
  }
  if (!previous && p->global_count == p->global_capacity) {
    free(decoded);
    fail(p, "C frontend global data table capacity exceeded");
    return false;
  }
  if (previous && previous->section && section_buf[0] &&
      strcmp(previous->section, section_buf) != 0) {
    free(decoded);
    fail(p, "conflicting section attributes on a file-scope C object");
    return false;
  }
  c_global_t definition = {.name = name,
                           .bytes = decoded,
                           .len = decoded_len,
                           .type = array_type,
                           .is_array = true,
                           .is_signed_array = array_type == C_CHAR,
                           .is_const = is_const,
                           .is_tentative = !is_external && !has_initializer,
                           .alias_target = alias_target,
                           .array_incomplete = incomplete && !has_initializer};
  if (previous && previous->section) {
    /*
     * An extern declaration already carries the section; adopt it.
     */
    definition.section = previous->section;
    previous->section = NULL;
  } else if (section_buf[0]) {
    definition.section = strdup(section_buf);
    if (!definition.section) {
      free(decoded);
      fail(p, "out of memory storing a C data section name");
      return false;
    }
  }
  if (previous) {
    /*
     * Replace the import, moving every owned field in one write.
     */
    free(previous->bytes);
    free(previous->relocs);
    *previous = definition;
  } else {
    p->globals[p->global_count++] = definition;
  }
  p->pos = at + 1;
  return true;
}

static bool parse_global_byte_value(ny_ctok_t token, uint64_t *value) {
  if (token.kind == NY_CTOK_CHAR) {
    size_t prefix =
        token.start[0] == 'L' || token.start[0] == 'u' || token.start[0] == 'U'
            ? 1
            : 0;
    if (token.len < prefix + 3 || token.start[prefix] != '\'' ||
        token.start[token.len - 1] != '\'')
      return false;
    size_t character = prefix + 1;
    if (token.start[character] != '\\') {
      if (token.len != prefix + 3)
        return false;
      *value = (unsigned char)token.start[character];
      return true;
    }
    size_t escape = character + 1;
    if (token.len < prefix + 4 ||
        (token.len != prefix + 4 && token.start[escape] != 'x' &&
         (token.start[escape] < '0' || token.start[escape] > '7')))
      return false;
    switch (token.start[escape]) {
    case 'a':
      *value = '\a';
      return true;
    case 'b':
      *value = '\b';
      return true;
    case 'e':
      *value = 27;
      return true;
    case 'f':
      *value = '\f';
      return true;
    case 'n':
      *value = '\n';
      return true;
    case 'r':
      *value = '\r';
      return true;
    case 't':
      *value = '\t';
      return true;
    case '0':
      *value = 0;
      return true;
    case 'v':
      *value = '\v';
      return true;
    case '\\':
    case '\'':
    case '"':
      *value = (unsigned char)token.start[2];
      return true;
    default:
      if (token.start[escape] >= '1' && token.start[escape] <= '7') {
        uint64_t parsed = 0;
        size_t digits = token.len - prefix - 3;
        if (digits > 3)
          return false;
        for (size_t i = escape; i + 1 < token.len; ++i) {
          if (token.start[i] < '0' || token.start[i] > '7')
            return false;
          parsed = parsed * 8 + (unsigned)(token.start[i] - '0');
        }
        *value = parsed;
        return true;
      }
      if (token.start[escape] == 'x') {
        uint64_t parsed = 0;
        if (token.len < prefix + 5)
          return false;
        for (size_t i = escape + 1; i + 1 < token.len; ++i) {
          unsigned digit;
          char c = token.start[i];
          if (c >= '0' && c <= '9')
            digit = (unsigned)(c - '0');
          else if (c >= 'a' && c <= 'f')
            digit = (unsigned)(c - 'a') + 10;
          else if (c >= 'A' && c <= 'F')
            digit = (unsigned)(c - 'A') + 10;
          else
            return false;
          if (parsed > (UINT64_MAX - digit) / 16)
            return false;
          parsed = parsed * 16 + digit;
        }
        *value = parsed;
        return true;
      }
      return false;
    }
  }
  if (token.kind != NY_CTOK_NUMBER)
    return false;
  char text[128];
  if (token.len >= sizeof(text))
    return false;
  memcpy(text, token.start, token.len);
  text[token.len] = '\0';
  size_t digits = strlen(text);
  while (digits && strchr("uUlL", text[digits - 1]))
    text[--digits] = '\0';
  errno = 0;
  unsigned long long parsed = strtoull(text, NULL, 0);
  if (errno == ERANGE)
    return false;
  *value = parsed;
  return true;
}

static bool parse_global_const_add(c_parser_t *p, size_t *at, int64_t *value);

static bool parse_global_char_array_row(c_parser_t *p, unsigned char *bytes,
                                        size_t offset, size_t capacity) {
  size_t content = 0;
  bool saw_string = false;
  while (current(p)->kind == NY_CTOK_STRING) {
    unsigned char *part = NULL;
    size_t part_len = 0;
    if (!decode_string_token(*current(p), &part, &part_len) || !part_len ||
        part_len - 1 > SIZE_MAX - content) {
      free(part);
      fail(p, "invalid file-scope character-array row initializer");
      return false;
    }
    size_t part_content = part_len - 1;
    if (part_content > capacity - content) {
      free(part);
      fail(p, "string initializer exceeds file-scope character-array row");
      return false;
    }
    memcpy(bytes + offset + content, part, part_content);
    content += part_content;
    saw_string = true;
    free(part);
    ++p->pos;
  }
  if (!saw_string) {
    fail(p, "file-scope character-array row requires a string literal");
    return false;
  }
  if (content < capacity)
    bytes[offset + content] = 0;
  return true;
}

static bool parse_global_integer_array_initializer(
    c_parser_t *p, c_type_t type, unsigned char *bytes, const size_t *extents,
    unsigned rank, unsigned dimension, size_t base, size_t stride) {
  if (dimension >= rank || !expect(p, "{"))
    return false;
  size_t index = 0;
  bool first = true;
  size_t element_size = c_type_size(p->abi, type);
  while (!tok_is(p, p->pos, "}")) {
    if (!first && !expect(p, ","))
      return false;
    if (tok_is(p, p->pos, "}"))
      break;
    first = false;
    size_t last_index = index;
    bool chained_designator = false;
    size_t chained_offset = 0;
    if (accept(p, "[")) {
      int64_t designated_index = -1;
      if (!parse_global_const_expr(p, &p->pos, &designated_index) ||
          designated_index < 0 ||
          (uint64_t)designated_index >= extents[dimension]) {
        fail(p, "invalid file-scope integer array designator");
        return false;
      }
      int64_t designated_last = designated_index;
      if (accept(p, "...") &&
          (!parse_global_const_expr(p, &p->pos, &designated_last) ||
           designated_last < designated_index ||
           (uint64_t)designated_last >= extents[dimension])) {
        fail(p, "invalid file-scope integer array range designator");
        return false;
      }
      if (!expect(p, "]")) {
        fail(p, "invalid file-scope integer array designator");
        return false;
      }
      index = (size_t)designated_index;
      last_index = (size_t)designated_last;
      if (!extents[dimension] || stride % extents[dimension])
        return false;
      size_t path_stride = stride / extents[dimension];
      unsigned path_dimension = dimension + 1;
      while (tok_is(p, p->pos, "[")) {
        int64_t path_index = -1;
        if (path_dimension >= rank || !expect(p, "[") ||
            !parse_global_const_expr(p, &p->pos, &path_index) ||
            path_index < 0 || (uint64_t)path_index >= extents[path_dimension] ||
            !expect(p, "]") || !extents[path_dimension] ||
            path_stride % extents[path_dimension]) {
          fail(p, "invalid chained file-scope integer array designator");
          return false;
        }
        path_stride /= extents[path_dimension];
        if ((uint64_t)path_index > SIZE_MAX / path_stride ||
            chained_offset > SIZE_MAX - (size_t)path_index * path_stride)
          return false;
        chained_offset += (size_t)path_index * path_stride;
        ++path_dimension;
        chained_designator = true;
      }
      if (chained_designator && path_dimension != rank) {
        fail(p, "chained file-scope array designator must select a scalar");
        return false;
      }
      if (!expect(p, "=")) {
        fail(p, "invalid file-scope integer array designator");
        return false;
      }
    }
    if (index >= extents[dimension]) {
      fail(p, "too many elements in file-scope integer array initializer");
      return false;
    }
    if (!extents[dimension] || stride % extents[dimension]) {
      fail(p, "invalid file-scope integer array stride");
      return false;
    }
    size_t child_stride = stride / extents[dimension];
    size_t offset = base + index * child_stride;
    if (chained_designator) {
      int64_t value = 0;
      if (!parse_global_const_expr(p, &p->pos, &value)) {
        fail(p, "file-scope integer arrays require constant elements");
        return false;
      }
      uint64_t bits = (uint64_t)value;
      size_t at = offset + chained_offset;
      for (size_t byte = 0; byte < element_size; ++byte)
        bytes[at + byte] = (unsigned char)(bits >> (byte * 8));
    } else if (dimension + 1 < rank) {
      if (dimension + 2 == rank && (type == C_CHAR || type == C_U8) &&
          current(p)->kind == NY_CTOK_STRING) {
        if (!parse_global_char_array_row(p, bytes, offset, child_stride))
          return false;
        for (size_t item = index + 1; item <= last_index; ++item)
          memcpy(bytes + base + item * child_stride, bytes + offset,
                 child_stride);
      } else {
        if (last_index != index) {
          fail(p, "nested file-scope integer array ranges are unsupported");
          return false;
        }
        if (!parse_global_integer_array_initializer(p, type, bytes, extents,
                                                    rank, dimension + 1, offset,
                                                    child_stride))
          return false;
      }
    } else {
      int64_t value = 0;
      if (!parse_global_const_expr(p, &p->pos, &value)) {
        fail(p, "file-scope integer arrays require constant elements");
        return false;
      }
      uint64_t bits = (uint64_t)value;
      for (size_t item = index; item <= last_index; ++item)
        for (size_t byte = 0; byte < element_size; ++byte)
          bytes[base + item * child_stride + byte] =
              (unsigned char)(bits >> (byte * 8));
    }
    index = last_index + 1;
  }
  return expect(p, "}");
}

static bool count_braced_narrow_string_initializer(const c_parser_t *p,
                                                   size_t *length) {
  size_t at = p->pos;
  if (!tok_is(p, at++, "{") || p->tokens[at].kind != NY_CTOK_STRING)
    return false;
  size_t characters = 0;
  while (p->tokens[at].kind == NY_CTOK_STRING) {
    unsigned char *part = NULL;
    size_t part_len = 0;
    if (!decode_string_token(p->tokens[at++], &part, &part_len) || !part_len ||
        characters > SIZE_MAX - (part_len - 1)) {
      free(part);
      return false;
    }
    characters += part_len - 1;
    free(part);
  }
  if (tok_is(p, at, ","))
    ++at;
  if (!tok_is(p, at, "}"))
    return false;
  if (characters == SIZE_MAX)
    return false;
  *length = characters + 1;
  return true;
}

static bool parse_braced_narrow_string_initializer(c_parser_t *p,
                                                   unsigned char *bytes,
                                                   size_t capacity) {
  if (!expect(p, "{") || current(p)->kind != NY_CTOK_STRING)
    return false;
  size_t offset = 0;
  while (current(p)->kind == NY_CTOK_STRING) {
    unsigned char *part = NULL;
    size_t part_len = 0;
    if (!decode_string_token(*current(p), &part, &part_len) || !part_len ||
        part_len - 1 > capacity - offset) {
      free(part);
      fail(p, "braced file-scope string does not fit its character array");
      return false;
    }
    memcpy(bytes + offset, part, part_len - 1);
    offset += part_len - 1;
    free(part);
    ++p->pos;
  }
  if (!accept(p, ",") || !tok_is(p, p->pos, "}")) {
    if (!tok_is(p, p->pos, "}"))
      return false;
  }
  if (offset < capacity)
    bytes[offset] = 0;
  return expect(p, "}");
}

static size_t count_global_aggregate_array_initializer(const c_parser_t *p,
                                                       size_t open);

static bool try_collect_integer_arrays(c_parser_t *p, bool *matched) {
  *matched = false;
  c_parser_t probe = *p;
  char probe_error[1] = {0};
  probe.err = probe_error;
  probe.err_len = sizeof(probe_error);
  probe.type_lookahead = true;
  c_type_t element_type;
  if (!parse_type(&probe, &element_type, false))
    return true;
  bool typedef_array_element =
      probe.last_type_is_array && probe.last_type_array.rank &&
      (probe.last_type_array.element_type == C_U8 ||
       probe.last_type_array.element_type == C_CHAR ||
       probe.last_type_array.element_type == C_U16 ||
       probe.last_type_array.element_type == C_I16 ||
       probe.last_type_array.element_type == C_I32 ||
       probe.last_type_array.element_type == C_U32 ||
       probe.last_type_array.element_type == C_I64 ||
       probe.last_type_array.element_type == C_U64) &&
      tok_ident(*current(&probe)) && tok_is(&probe, probe.pos + 1, "[");
  if (typedef_array_element)
    element_type = probe.last_type_array.element_type;
  if ((element_type != C_U8 && element_type != C_CHAR &&
       element_type != C_U16 && element_type != C_I16 &&
       element_type != C_I32 && element_type != C_U32 &&
       element_type != C_I64 && element_type != C_U64) &&
      !typedef_array_element)
    return true;
  bool type_array = probe.last_type_is_array &&
                    probe.last_type_array.rank != 0 && !typedef_array_element;
  size_t element_size = probe.last_type_array_size
                            ? probe.last_type_array_size
                            : c_type_size(p->abi, element_type);
  c_array_declarator_t declarator = {0};
  if (type_array) {
    if (!tok_ident(*current(&probe)) || (!tok_is(&probe, probe.pos + 1, ",") &&
                                         !tok_is(&probe, probe.pos + 1, ";")))
      return true;
  } else {
    size_t ignored_alignment = probe.last_type_alignment;
    if (type_alignment_attribute_follows(&probe) &&
        !parse_alignment_attribute(&probe, &ignored_alignment, NULL))
      return true;
    if (!parse_external_integer_declarator(&probe, &declarator, element_size))
      return true;
    ny_ctok_t ignored_section = {0};
    if (!parse_trailing_file_scope_attributes(&probe, &ignored_section) ||
        (!tok_is(&probe, probe.pos, ",") && !tok_is(&probe, probe.pos, ";") &&
         !tok_is(&probe, probe.pos, "=")))
      return true;
  }
  bool is_external = false;
  for (size_t i = p->pos; i < probe.pos; ++i)
    is_external |= tok_is(p, i, "extern");
  *matched = true;
  if (!parse_type(p, &element_type, false))
    return false;
  if (typedef_array_element)
    element_type = p->last_type_array.element_type;
  size_t declaration_alignment = p->last_type_alignment;
  ny_ctok_t declaration_section = p->last_type_section;
  if (!type_array && type_alignment_attribute_follows(p) &&
      !parse_alignment_attribute(p, &declaration_alignment, NULL))
    return false;
  bool is_const = p->last_type_const;
  if (type_array) {
    c_array_shape_t shape = p->last_type_array;
    size_t len = p->last_type_array_size;
    size_t stride = shape.rank > 1 ? len / shape.extent[0] : 0;
    for (;;) {
      ny_ctok_t name;
      if (!parse_name(p, &name))
        return false;
      if (tok_is(p, p->pos, "=")) {
        fail(p, "array type-name globals cannot have initializers");
        return false;
      }
      c_global_t *global = find_global(p, name);
      bool compatible =
          global && global->is_array && global->type == element_type &&
          (global->array_rank == shape.rank || !global->array_rank) &&
          global->is_const == is_const &&
          (!global->len || !len || global->len == len);
      if (global && (!compatible || (!is_external && !global->is_external &&
                                     !global->is_tentative))) {
        fail(p, "incompatible or duplicate typeof array global");
        return false;
      }
      if (global && is_external) {
        if (global->len < len)
          global->len = len;
        global->array_rank = shape.rank;
      } else if (global) {
        unsigned char *bytes = len ? calloc(len, 1) : NULL;
        if (len && !bytes) {
          fail(p, "out of memory allocating typeof array global");
          return false;
        }
        free(global->bytes);
        global->bytes = bytes;
        global->len = len;
        global->is_external = false;
        global->is_tentative = true;
      } else {
        if (p->global_count >= p->global_capacity) {
          fail(p, "C frontend global data table capacity exceeded");
          return false;
        }
        unsigned char *bytes = !is_external && len ? calloc(len, 1) : NULL;
        if (!is_external && len && !bytes) {
          fail(p, "out of memory allocating typeof array global");
          return false;
        }
        p->globals[p->global_count++] =
            (c_global_t){.name = name,
                         .bytes = bytes,
                         .len = len,
                         .type = element_type,
                         .is_array = true,
                         .array_rank = shape.rank,
                         .array_stride = stride,
                         .is_const = is_const,
                         .is_external = is_external,
                         .is_tentative = !is_external};
        global = &p->globals[p->global_count - 1];
      }
      global->array_shape = shape;
      global->array_rank = shape.rank;
      global->array_stride = stride;
      if (!accept(p, ","))
        break;
    }
    return expect(p, ";");
  }
  element_size = p->last_type_array_size ? p->last_type_array_size
                                         : c_type_size(p->abi, element_type);
  c_array_shape_t typedef_shape =
      typedef_array_element ? p->last_type_array : (c_array_shape_t){0};
  size_t scalar_size = c_type_size(p->abi, element_type);
  for (;;) {
    p->last_type_alignment = declaration_alignment;
    c_array_declarator_t current_declarator = {0};
    if (!parse_external_integer_declarator(p, &current_declarator,
                                           element_size) ||
        current_declarator.extent >
            SIZE_MAX / current_declarator.element_stride) {
      fail(p, "external integer array requires a valid extent");
      return false;
    }
    if (typedef_shape.rank) {
      unsigned rank = current_declarator.array_rank;
      if (typedef_shape.rank > C_MAX_ARRAY_RANK - rank) {
        fail(p, "external integer array exceeds the frontend rank limit");
        return false;
      }
      memcpy(current_declarator.array_extent + rank, typedef_shape.extent,
             typedef_shape.rank * sizeof(typedef_shape.extent[0]));
      current_declarator.array_rank += typedef_shape.rank;
    }
    ny_ctok_t section = declaration_section;
    if (!parse_trailing_file_scope_attributes(p, &section))
      return false;
    size_t object_alignment = p->last_type_alignment;
    bool has_initializer = accept(p, "=");
    if (has_initializer && current_declarator.incomplete) {
      bool wide_string = current_declarator.array_rank == 1 &&
                         element_type == C_U16 &&
                         current(p)->kind == NY_CTOK_STRING &&
                         current(p)->len >= 2 && current(p)->start[0] == 'L';
      bool braced_narrow_string =
          current_declarator.array_rank == 1 &&
          (element_type == C_CHAR || element_type == C_U8) &&
          tok_is(p, p->pos, "{") &&
          p->tokens[p->pos + 1].kind == NY_CTOK_STRING;
      bool incomplete_outer_rows =
          current_declarator.array_rank == 2 && tok_is(p, p->pos, "{");
      if ((!incomplete_outer_rows &&
           (current_declarator.array_rank != 1 ||
            (!tok_is(p, p->pos, "{") && !wide_string)))) {
        fail(p, "incomplete file-scope integer array requires a flat list");
        return false;
      }
      size_t inferred = 0;
      if (wide_string) {
        size_t characters = 0;
        while (current(p)->kind == NY_CTOK_STRING) {
          unsigned char *part = NULL;
          size_t part_len = 0;
          if (!decode_wide_string_token(*current(p), &part, &part_len) ||
              part_len < 2 || part_len % 2 != 0 ||
              characters > SIZE_MAX / 2 - (part_len / 2 - 1)) {
            free(part);
            fail(p, "invalid file-scope wide string initializer");
            return false;
          }
          characters += part_len / 2 - 1;
          free(part);
          ++p->pos;
        }
        if (characters == SIZE_MAX) {
          fail(p, "file-scope wide string initializer is too large");
          return false;
        }
        inferred = characters + 1;
      } else if (braced_narrow_string) {
        if (!count_braced_narrow_string_initializer(p, &inferred)) {
          fail(p,
               "cannot infer incomplete braced character-array string bound");
          return false;
        }
      } else {
        inferred = count_global_aggregate_array_initializer(p, p->pos);
      }
      bool empty_initializer =
          tok_is(p, p->pos, "{") && tok_is(p, p->pos + 1, "}");
      if ((!inferred && !empty_initializer) || inferred > 65536) {
        fail(p, "cannot infer incomplete file-scope integer array bound");
        return false;
      }
      current_declarator.extent = inferred;
      current_declarator.array_extent[0] = inferred;
      current_declarator.incomplete = false;
      if (wide_string) {
        /*
         * The token sequence has been consumed while determining its bound.
         */
        size_t rewind = 0;
        while (p->pos > 0 && p->tokens[p->pos - 1].kind == NY_CTOK_STRING) {
          --p->pos;
          ++rewind;
        }
        (void)rewind;
      }
    }
    size_t len =
        (size_t)current_declarator.extent * current_declarator.element_stride;
    c_global_t *global = find_global(p, current_declarator.name);
    bool compatible = global && global->is_array &&
                      global->type == element_type &&
                      (global->array_rank == current_declarator.array_rank ||
                       (global->is_external && !global->array_rank)) &&
                      global->is_const == is_const &&
                      (!global->len || !len || global->len == len);
    if (global && (!compatible || (!is_external && !global->is_external &&
                                   !global->is_tentative))) {
      fail(p, "incompatible or duplicate file-scope integer array");
      return false;
    }
    if (global && is_external) {
      if (global->len < len)
        global->len = len;
      if (global->alignment < object_alignment)
        global->alignment = object_alignment;
      global->array_rank = current_declarator.array_rank;
      global->array_stride = current_declarator.element_stride;
      global->array_shape.rank = current_declarator.array_rank;
      global->array_shape.element_type = element_type;
      global->array_shape.element_size = scalar_size;
      memcpy(global->array_shape.extent, current_declarator.array_extent,
             current_declarator.array_rank * sizeof(size_t));
      if (!accept(p, ","))
        break;
      continue;
    }
    unsigned char *bytes = NULL;
    if (!is_external && len) {
      bytes = calloc(len, 1);
      if (!bytes) {
        fail(p, "out of memory allocating file-scope integer array");
        return false;
      }
    }
    if (global) {
      free(global->bytes);
      global->bytes = bytes;
      global->len = len;
      global->is_external = false;
      global->is_tentative = true;
    } else {
      if (p->global_count >= p->global_capacity) {
        free(bytes);
        fail(p, "C frontend global data table capacity exceeded");
        return false;
      }
      p->globals[p->global_count++] = (c_global_t){
          .name = current_declarator.name,
          .bytes = bytes,
          .len = len,
          .type = element_type,
          .is_array = true,
          .array_rank = current_declarator.array_rank,
          .array_stride = current_declarator.element_stride,
          .is_signed_array = element_type == C_CHAR || element_type == C_I16 ||
                             element_type == C_I32 || element_type == C_I64,
          .is_const = is_const,
          .is_external = is_external,
          .is_tentative = !is_external,
          .alignment = object_alignment};
      global = &p->globals[p->global_count - 1];
    }
    if (global->alignment < object_alignment)
      global->alignment = object_alignment;
    if (section.len) {
      char section_name[64] = {0};
      if (!copy_quoted_token(p, section, section_name, sizeof(section_name)))
        return false;
      char *owned_section = strdup(section_name);
      if (!owned_section) {
        fail(p, "out of memory storing an integer array section");
        return false;
      }
      free(global->section);
      global->section = owned_section;
    }
    global->array_shape.rank = current_declarator.array_rank;
    global->array_shape.element_type = element_type;
    global->array_shape.element_size = scalar_size;
    memcpy(global->array_shape.extent, current_declarator.array_extent,
           current_declarator.array_rank * sizeof(size_t));
    if (has_initializer) {
      unsigned char *initialized_bytes = calloc(len ? len : 1, 1);
      if (!initialized_bytes) {
        fail(p, "out of memory initializing file-scope integer array");
        return false;
      }
      bool initialized_ok = false;
      if (element_type == C_U16 && current(p)->kind == NY_CTOK_STRING &&
          current(p)->len >= 2 && current(p)->start[0] == 'L') {
        size_t offset = 0;
        initialized_ok = true;
        while (current(p)->kind == NY_CTOK_STRING) {
          unsigned char *part = NULL;
          size_t part_len = 0;
          if (!decode_wide_string_token(*current(p), &part, &part_len) ||
              part_len < 2 || part_len % 2 != 0 || offset > len ||
              part_len - 2 > len - offset) {
            free(part);
            initialized_ok = false;
            break;
          }
          memcpy(initialized_bytes + offset, part, part_len - 2);
          offset += part_len - 2;
          free(part);
          ++p->pos;
        }
        if (initialized_ok && offset <= len && len - offset >= 2) {
          initialized_bytes[offset] = 0;
          initialized_bytes[offset + 1] = 0;
        } else {
          initialized_ok = false;
        }
      } else if ((element_type == C_CHAR || element_type == C_U8) &&
                 current_declarator.array_rank == 1 && tok_is(p, p->pos, "{") &&
                 p->tokens[p->pos + 1].kind == NY_CTOK_STRING) {
        initialized_ok =
            parse_braced_narrow_string_initializer(p, initialized_bytes, len);
      } else {
        initialized_ok = parse_global_integer_array_initializer(
            p, element_type, initialized_bytes, current_declarator.array_extent,
            current_declarator.array_rank, 0, 0, len);
      }
      if (!initialized_ok) {
        free(initialized_bytes);
        return false;
      }
      free(global->bytes);
      global->bytes = initialized_bytes;
      global->is_tentative = false;
    }
    if (has_initializer && tok_is(p, p->pos, ",")) {
      fail(p, "initialized file-scope integer array lists are unsupported");
      return false;
    }
    if (!accept(p, ","))
      break;
  }
  return expect(p, ";");
}

static bool try_collect_uninitialized_pointer_arrays(c_parser_t *p,
                                                     bool *matched) {
  *matched = false;
  c_parser_t probe = *p;
  char probe_error[1] = {0};
  probe.err = probe_error;
  probe.err_len = sizeof(probe_error);
  probe.type_lookahead = true;
  c_type_t element_type;
  if (!parse_type(&probe, &element_type, false) ||
      !c_type_is_pointer(element_type) || element_type == C_AGG_PTR)
    return true;
  /*
   * A typedef may itself name an array, e.g. `typedef T *row[N];`.
   * Preserve those inner dimensions when a file-scope declaration adds an
   * outer array (`extern row matrix[M];`).
   */
  c_array_shape_t typedef_shape =
      probe.last_type_is_array ? probe.last_type_array : (c_array_shape_t){0};
  size_t typedef_array_size = probe.last_type_array_size;
  c_array_declarator_t declarator = {0};
  if (!parse_array_declarator(&probe, &declarator, 0) || !declarator.array)
    return true;
  ny_ctok_t ignored_section = {0};
  if (!parse_trailing_file_scope_attributes(&probe, &ignored_section) ||
      (!tok_is(&probe, probe.pos, ",") && !tok_is(&probe, probe.pos, ";")))
    return true;
  bool is_external = false;
  bool is_static = false;
  for (size_t i = p->pos; i < probe.pos; ++i) {
    is_external |= tok_is(p, i, "extern");
    is_static |= tok_is(p, i, "static");
  }
  if (!is_external && !is_static)
    return true;

  /*
   * `extern const unsigned char * const x86_nops[];` and friends.  Only the
   * declaration reaches the frontend: each entry is a global with no bytes
   * and no echo in the object file until a later TU defines it, exactly
   * like the external integer array path.
   */
  for (size_t i = p->pos; i < probe.pos; ++i)
    if (tok_is(p, i, "=")) {
      fail(p, "external pointer arrays cannot have initializers");
      return false;
    }
  *matched = true;
  if (!parse_type(p, &element_type, false))
    return false;
  bool is_const = p->last_type_const_pointer;
  ny_ctok_t declaration_section = p->last_type_section;
  unsigned declaration_pointer_levels = p->last_type_pointer_count;
  typedef_shape =
      p->last_type_is_array ? p->last_type_array : (c_array_shape_t){0};
  typedef_array_size = p->last_type_array_size;
  bool first_declarator = true;
  for (;;) {
    if (!first_declarator) {
      unsigned pointer_levels = 0;
      while (accept(p, "*")) {
        ++pointer_levels;
        while (accept(p, "const") || accept(p, "volatile") ||
               accept(p, "restrict") || accept(p, "__restrict") ||
               accept(p, "__restrict__")) {
        }
      }
      if (pointer_levels != declaration_pointer_levels) {
        fail(p, "comma-separated pointer-array declarator has incompatible "
                "pointer depth");
        return false;
      }
    }
    first_declarator = false;
    c_array_declarator_t current_declarator = {0};
    if (!parse_array_declarator(p, &current_declarator, 0) ||
        !current_declarator.array) {
      fail(p, "external pointer declaration requires an array declarator");
      return false;
    }
    ny_ctok_t section_token = declaration_section;
    if (!parse_trailing_file_scope_attributes(p, &section_token))
      return false;
    if (find_global(p, current_declarator.name) ||
        p->global_count >= p->global_capacity) {
      fail(p, "duplicate or excessive external pointer array");
      return false;
    }
    c_global_t global = {.name = current_declarator.name,
                         .type = element_type,
                         .agg = p->last_type_agg,
                         .is_array = true,
                         .is_pointer_array = true,
                         .is_const = is_const,
                         .is_external = is_external};
    if (typedef_shape.rank) {
      if (typedef_shape.rank >= C_MAX_ARRAY_RANK || !typedef_array_size) {
        fail(p, "pointer-array typedef has invalid array dimensions");
        return false;
      }
      global.array_rank = typedef_shape.rank + 1;
      global.array_stride = typedef_array_size;
      global.array_shape = typedef_shape;
      memmove(global.array_shape.extent + 1, global.array_shape.extent,
              typedef_shape.rank * sizeof(typedef_shape.extent[0]));
      global.array_shape.extent[0] =
          current_declarator.incomplete ? 0 : (size_t)current_declarator.extent;
      global.array_shape.rank = (unsigned)global.array_rank;
    } else if (current_declarator.incomplete) {
      global.array_incomplete = true;
    } else {
      global.array_rank = 1;
      global.array_shape =
          (c_array_shape_t){.rank = 1,
                            .extent = {(size_t)current_declarator.extent, 0},
                            .element_size = sizeof(uint64_t),
                            .element_type = element_type,
                            .agg = p->last_type_agg};
    }
    if (section_token.len) {
      char section_name[64];
      if (!copy_quoted_token(p, section_token, section_name,
                             sizeof(section_name)))
        return false;
      global.section = strdup(section_name);
      if (!global.section) {
        fail(p, "out of memory storing pointer-array section attribute");
        return false;
      }
    }
    if (is_static) {
      if (current_declarator.incomplete ||
          current_declarator.extent > SIZE_MAX / (typedef_array_size
                                                      ? typedef_array_size
                                                      : sizeof(uint64_t))) {
        fail(p, "static pointer array requires a bounded constant extent");
        return false;
      }
      size_t row_size =
          typedef_array_size ? typedef_array_size : sizeof(uint64_t);
      global.len = (size_t)current_declarator.extent * row_size;
      global.bytes = calloc(global.len ? global.len : 1, 1);
      if (!global.bytes) {
        fail(p, "out of memory allocating static pointer array");
        return false;
      }
      if (!global.array_rank)
        global.array_rank = 1;
    }
    p->globals[p->global_count++] = global;
    if (!accept(p, ","))
      break;
  }
  return expect(p, ";");
}

static bool try_collect_byte_array(c_parser_t *p, bool *matched) {
  *matched = false;
  if (!try_collect_integer_arrays(p, matched))
    return false;
  if (*matched)
    return true;
  size_t type_pos = p->pos;
  bool is_external = false, is_const = false;
  while (tok_is(p, type_pos, "const") || tok_is(p, type_pos, "static") ||
         tok_is(p, type_pos, "extern") || tok_is(p, type_pos, "volatile")) {
    is_external |= tok_is(p, type_pos, "extern");
    is_const |= tok_is(p, type_pos, "const");
    ++type_pos;
  }
  if (tok_is(p, type_pos, "struct") || tok_is(p, type_pos, "union"))
    return true;
  c_parser_t probe = *p;
  char probe_error[1] = {0};
  probe.err = probe_error;
  probe.err_len = sizeof(probe_error);
  probe.pos = type_pos;
  probe.type_lookahead = true;
  c_type_t element_type;
  if (!parse_type(&probe, &element_type, false) ||
      (element_type != C_U8 && element_type != C_CHAR &&
       element_type != C_U16 && element_type != C_I16 &&
       element_type != C_I32 && element_type != C_U32 &&
       element_type != C_I64 && element_type != C_U64))
    return true;
  c_array_declarator_t declarator = {0};
  if (!parse_array_declarator(&probe, &declarator, 0) || !declarator.array)
    return true;
  ny_ctok_t section = probe.last_type_section;
  if (!parse_trailing_file_scope_attributes(&probe, &section))
    return true;
  bool signed_byte = element_type == C_CHAR;
  size_t element_size = element_type == C_U8 || element_type == C_CHAR   ? 1
                        : element_type == C_U16 || element_type == C_I16 ? 2
                        : element_type == C_I32 || element_type == C_U32 ? 4
                                                                         : 8;
  size_t at = probe.pos;
  *matched = true;
  uint64_t extent = declarator.extent;
  bool incomplete = declarator.incomplete;
  if (extent > SIZE_MAX / element_size) {
    fail(p, "file-scope integer array requires a valid constant extent");
    return false;
  }
  size_t declared_bytes = (size_t)extent * element_size;
  ny_ctok_t name = declarator.name;
  if (find_global(p, name)) {
    char message[160];
    snprintf(message, sizeof(message),
             "duplicate file-scope C data symbol `%.100s`", name.start);
    fail(p, message);
    return false;
  }
  at = probe.pos;
  bool has_initializer = tok_is(p, at, "=");
  if (!has_initializer && !tok_is(p, at, ";")) {
    fail(p, "expected initializer or ';' after file-scope integer array");
    return false;
  }
  bool string_initializer =
      has_initializer && (element_type == C_U8 || element_type == C_CHAR) &&
      at + 1 < p->token_count && p->tokens[at + 1].kind == NY_CTOK_STRING;
  unsigned char *string_bytes = NULL;
  size_t string_count = 0;
  if (string_initializer) {
    ++at; /* '=' */
    unsigned char *decoded = NULL;
    size_t decoded_len = 0;
    while (at < p->token_count && p->tokens[at].kind == NY_CTOK_STRING) {
      unsigned char *part = NULL;
      size_t part_len = 0;
      if (!decode_string_token(p->tokens[at++], &part, &part_len)) {
        free(part);
        free(decoded);
        fail(p, "invalid string initializer for file-scope byte array");
        return false;
      }
      size_t content = part_len - 1;
      if (content > SIZE_MAX - decoded_len - 1) {
        free(part);
        free(decoded);
        fail(p, "file-scope byte-array string initializer is too large");
        return false;
      }
      size_t old_content = decoded_len ? decoded_len - 1 : 0;
      unsigned char *grown = realloc(decoded, old_content + content + 1);
      if (!grown) {
        free(part);
        free(decoded);
        fail(p, "out of memory decoding file-scope byte-array string");
        return false;
      }
      decoded = grown;
      memcpy(decoded + old_content, part, content + 1);
      decoded_len = old_content + content + 1;
      free(part);
    }
    if (!decoded || !tok_is(p, at, ";")) {
      free(decoded);
      fail(p, "string initializer must end the file-scope byte array");
      return false;
    }
    if (incomplete) {
      declared_bytes = decoded_len;
    } else if (declared_bytes < decoded_len) {
      if (declared_bytes + 1 == decoded_len)
        decoded_len = declared_bytes; /* C permits omitting the final NUL. */
      else {
        free(decoded);
        fail(p, "string initializer exceeds file-scope byte-array extent");
        return false;
      }
    } else if (declared_bytes > decoded_len) {
      unsigned char *grown = realloc(decoded, declared_bytes);
      if (!grown) {
        free(decoded);
        fail(p, "out of memory padding file-scope byte-array string");
        return false;
      }
      decoded = grown;
      memset(decoded + decoded_len, 0, declared_bytes - decoded_len);
      decoded_len = declared_bytes;
    }
    string_bytes = decoded;
    string_count = decoded_len;
  } else if (has_initializer &&
             (!tok_is(p, at++, "=") || !tok_is(p, at++, "{"))) {
    fail(p, "file-scope integer arrays require a brace initializer");
    return false;
  }
  if (!has_initializer && incomplete && !is_external)
    declared_bytes = element_size;
  bool owns_storage = !is_external || has_initializer;
  unsigned char *bytes = string_initializer ? string_bytes
                         : owns_storage && declared_bytes
                             ? calloc(declared_bytes, 1)
                             : NULL;
  if (owns_storage && declared_bytes && !bytes) {
    fail(p, "out of memory allocating file-scope integer array");
    return false;
  }
  size_t count = string_initializer ? string_count : 0;
  size_t capacity = string_initializer ? string_count
                    : owns_storage     ? declared_bytes
                                       : 0;
  while (has_initializer && !string_initializer && !tok_is(p, at, "}")) {
    while (tok_is(p, at, "\\"))
      ++at;
    if (tok_is(p, at, "}"))
      break;
    size_t index = count / element_size;
    size_t last_index = index;
    if (tok_is(p, at, "[")) {
      int64_t designated_index = -1;
      ++at;
      if (!parse_global_const_expr(p, &at, &designated_index) ||
          designated_index < 0) {
        free(bytes);
        fail(p, "invalid file-scope integer array designator");
        return false;
      }
      int64_t designated_last = designated_index;
      if (tok_is(p, at, "...")) {
        ++at;
        if (!parse_global_const_expr(p, &at, &designated_last) ||
            designated_last < designated_index) {
          free(bytes);
          fail(p, "invalid file-scope integer array range designator");
          return false;
        }
      }
      if (!tok_is(p, at, "]") || !tok_is(p, at + 1, "=")) {
        free(bytes);
        fail(p, "invalid file-scope integer array designator");
        return false;
      }
      at += 2;
      if ((uint64_t)designated_index > SIZE_MAX / element_size) {
        free(bytes);
        fail(p, "file-scope integer array designator overflows");
        return false;
      }
      index = (size_t)designated_index;
      if ((uint64_t)designated_last > SIZE_MAX) {
        free(bytes);
        fail(p, "file-scope integer array range designator overflows");
        return false;
      }
      last_index = (size_t)designated_last;
    }
    int64_t constant = 0;
    if (!parse_global_const_expr(p, &at, &constant)) {
      free(bytes);
      char message[160];
      snprintf(message, sizeof(message),
               "file-scope integer arrays require integer elements near "
               "`%.80s`",
               p->tokens[at].start ? p->tokens[at].start : "");
      fail(p, message);
      return false;
    }
    if (last_index == SIZE_MAX || last_index + 1 > SIZE_MAX / element_size) {
      free(bytes);
      fail(p, "file-scope integer array size overflow");
      return false;
    }
    size_t needed = (last_index + 1) * element_size;
    if (!incomplete && needed > declared_bytes) {
      free(bytes);
      fail(p, "too many elements in file-scope integer array initializer");
      return false;
    }
    if (needed > capacity) {
      size_t next = capacity ? capacity : 256;
      while (next < needed) {
        if (next > SIZE_MAX / 2) {
          next = needed;
          break;
        }
        next *= 2;
      }
      unsigned char *grown = calloc(next, 1);
      if (!grown) {
        free(bytes);
        fail(p, "out of memory allocating file-scope byte array");
        return false;
      }
      if (bytes) {
        memcpy(grown, bytes, count);
        free(bytes);
      }
      bytes = grown;
      capacity = next;
    }
    uint64_t encoded = (uint64_t)constant;
    for (size_t item = index; item <= last_index; ++item)
      for (size_t byte = 0; byte < element_size; ++byte)
        bytes[item * element_size + byte] =
            (unsigned char)(encoded >> (byte * 8));
    if (needed > count)
      count = needed;
    if (tok_is(p, at, ",")) {
      ++at;
      while (tok_is(p, at, "\\"))
        ++at;
    } else {
      if (!tok_is(p, at, "}")) {
        free(bytes);
        fail(p, "expected ',' between file-scope byte array elements");
        return false;
      }
    }
  }
  if (has_initializer && !string_initializer)
    ++at;
  if (!tok_is(p, at++, ";") || (has_initializer && !count && incomplete)) {
    free(bytes);
    fail(p, "invalid file-scope byte array terminator");
    return false;
  }
  if (p->global_count >= p->global_capacity) {
    free(bytes);
    fail(p, "C frontend global data table capacity exceeded");
    return false;
  }
  char section_name[64] = {0};
  if (section.len &&
      !copy_quoted_token(p, section, section_name, sizeof(section_name))) {
    free(bytes);
    return false;
  }
  char *owned_section = section_name[0] ? strdup(section_name) : NULL;
  if (section_name[0] && !owned_section) {
    free(bytes);
    fail(p, "out of memory storing a file-scope array section");
    return false;
  }
  p->globals[p->global_count++] = (c_global_t){
      .name = name,
      .bytes = bytes,
      .len = incomplete && has_initializer ? count : declared_bytes,
      .type = element_type == C_U8     ? C_CHAR_PTR
              : element_type == C_CHAR ? C_I8_PTR
              : element_type == C_I16  ? C_I16_PTR
              : element_type == C_U16  ? C_U16_PTR
              : element_type == C_U32  ? C_U32_PTR
              : element_type == C_I64  ? C_I64_PTR
              : element_type == C_U64  ? C_U64_PTR
                                       : C_I32_PTR,
      .is_array = true,
      .is_signed_array = signed_byte || element_type == C_I16 ||
                         element_type == C_I32 || element_type == C_I64,
      .is_const = is_const,
      .is_external = is_external && !has_initializer,
      .is_tentative = !is_external && !has_initializer,
      .section = owned_section};
  p->pos = at;
  return true;
}

static bool global_const_function_pointer_cast_type(c_parser_t *p, size_t open,
                                                    size_t close) {
  c_parser_t probe = *p;
  char error[1] = {0};
  probe.err = error;
  probe.err_len = sizeof(error);
  probe.type_lookahead = true;
  probe.pos = open + 1;
  c_type_t result;
  if (!parse_type(&probe, &result, true) || !expect(&probe, "(") ||
      !expect(&probe, "*") || !expect(&probe, ")") || !expect(&probe, "("))
    return false;
  if (accept(&probe, ")"))
    return probe.pos == close;
  if (tok_is(&probe, probe.pos, "void") && tok_is(&probe, probe.pos + 1, ")")) {
    probe.pos += 2;
    return probe.pos == close;
  }
  for (;;) {
    c_type_t parameter;
    if (!parse_type(&probe, &parameter, true))
      return false;
    if (accept(&probe, ")"))
      return probe.pos == close;
    if (!expect(&probe, ","))
      return false;
  }
}

static bool global_const_cast_type(c_parser_t *p, size_t open, size_t close) {
  c_parser_t probe = *p;
  char error[1] = {0};
  probe.err = error;
  probe.err_len = sizeof(error);
  probe.type_lookahead = true;
  probe.pos = open + 1;
  c_type_t type;
  return (parse_type(&probe, &type, true) && probe.pos == close) ||
         global_const_function_pointer_cast_type(p, open, close);
}

static bool global_const_cast_unsigned(c_parser_t *p, size_t open, size_t close,
                                       c_type_t *cast_type) {
  *cast_type = C_BAD;
  c_parser_t probe = *p;
  char error[1] = {0};
  probe.err = error;
  probe.err_len = sizeof(error);
  probe.type_lookahead = true;
  probe.pos = open + 1;
  c_type_t type;
  if (!parse_type(&probe, &type, true) || probe.pos != close)
    return false;
  *cast_type = type;
  return type == C_U8 || type == C_U16 || type == C_U32 || type == C_U64;
}

static bool global_const_cast_is_u32(c_parser_t *p, size_t open, size_t close) {
  c_parser_t probe = *p;
  char error[1] = {0};
  probe.err = error;
  probe.err_len = sizeof(error);
  probe.type_lookahead = true;
  probe.pos = open + 1;
  c_type_t type;
  return parse_type(&probe, &type, true) && probe.pos == close && type == C_U32;
}

static bool parse_global_float_primary(c_parser_t *p, size_t *at,
                                       double *value) {
  if (tok_is(p, *at, "-")) {
    ++*at;
    if (!parse_global_float_primary(p, at, value))
      return false;
    *value = -*value;
    return true;
  }
  if (tok_is(p, *at, "+")) {
    ++*at;
    return parse_global_float_primary(p, at, value);
  }
  if (tok_is(p, *at, "(")) {
    ++*at;
    if (!parse_global_float_add(p, at, value) || !tok_is(p, *at, ")"))
      return false;
    ++*at;
    return true;
  }
  ny_ctok_t token = p->tokens[*at];
  if (token.kind == NY_CTOK_NUMBER) {
    char text[128];
    if (!token.len || token.len >= sizeof(text))
      return false;
    memcpy(text, token.start, token.len);
    text[token.len] = '\0';
    size_t len = strlen(text);
    if (len && strchr("fFlL", text[len - 1]))
      text[--len] = '\0';
    errno = 0;
    char *end = NULL;
    double parsed = strtod(text, &end);
    if (errno || end == text || *end)
      return false;
    *value = parsed;
    ++*at;
    return true;
  }
  if (token.kind == NY_CTOK_IDENT) {
    for (size_t i = 0; i < p->pp_constant_count; ++i) {
      c_pp_constant_t *constant = &p->pp_constants[i];
      if (constant->integer_value && constant->name_len == token.len &&
          memcmp(constant->name, token.start, token.len) == 0) {
        *value = (double)constant->value;
        ++*at;
        return true;
      }
    }
  }
  return false;
}

static bool parse_global_float_mul(c_parser_t *p, size_t *at, double *value) {
  if (!parse_global_float_primary(p, at, value))
    return false;
  while (tok_is(p, *at, "*") || tok_is(p, *at, "/")) {
    bool divide = tok_is(p, *at, "/");
    ++*at;
    double rhs;
    if (!parse_global_float_primary(p, at, &rhs) || (divide && rhs == 0.0))
      return false;
    *value = divide ? *value / rhs : *value * rhs;
  }
  return true;
}

static bool parse_global_float_add(c_parser_t *p, size_t *at, double *value) {
  if (!parse_global_float_mul(p, at, value))
    return false;
  while (tok_is(p, *at, "+") || tok_is(p, *at, "-")) {
    bool subtract = tok_is(p, *at, "-");
    ++*at;
    double rhs;
    if (!parse_global_float_mul(p, at, &rhs))
      return false;
    *value = subtract ? *value - rhs : *value + rhs;
  }
  return true;
}

static bool parse_global_const_primary(c_parser_t *p, size_t *at,
                                       int64_t *value);

static size_t global_const_conditional_colon(c_parser_t *p, size_t at) {
  size_t parens = 0, brackets = 0, braces = 0, nested_conditionals = 0;
  for (; at < p->token_count; ++at) {
    if (tok_is(p, at, "("))
      ++parens;
    else if (tok_is(p, at, ")")) {
      if (!parens && !brackets && !braces)
        return SIZE_MAX;
      if (parens)
        --parens;
    } else if (tok_is(p, at, "["))
      ++brackets;
    else if (tok_is(p, at, "]")) {
      if (!brackets && !parens && !braces)
        return SIZE_MAX;
      if (brackets)
        --brackets;
    } else if (tok_is(p, at, "{"))
      ++braces;
    else if (tok_is(p, at, "}")) {
      if (!braces && !parens && !brackets)
        return SIZE_MAX;
      if (braces)
        --braces;
    } else if (!parens && !brackets && !braces && tok_is(p, at, "?"))
      ++nested_conditionals;
    else if (!parens && !brackets && !braces && tok_is(p, at, ":")) {
      if (!nested_conditionals)
        return at;
      --nested_conditionals;
    } else if (!parens && !brackets && !braces &&
               (tok_is(p, at, ",") || tok_is(p, at, ";")))
      return SIZE_MAX;
  }
  return SIZE_MAX;
}

static size_t global_const_expression_end(c_parser_t *p, size_t at) {
  size_t parens = 0, brackets = 0, braces = 0, nested_conditionals = 0;
  for (; at < p->token_count; ++at) {
    if (tok_is(p, at, "("))
      ++parens;
    else if (tok_is(p, at, ")")) {
      if (!parens && !brackets && !braces)
        return at;
      if (parens)
        --parens;
    } else if (tok_is(p, at, "["))
      ++brackets;
    else if (tok_is(p, at, "]")) {
      if (!brackets && !parens && !braces)
        return at;
      if (brackets)
        --brackets;
    } else if (tok_is(p, at, "{"))
      ++braces;
    else if (tok_is(p, at, "}")) {
      if (!braces && !parens && !brackets)
        return at;
      if (braces)
        --braces;
    } else if (!parens && !brackets && !braces && tok_is(p, at, "?"))
      ++nested_conditionals;
    else if (!parens && !brackets && !braces && tok_is(p, at, ":")) {
      if (!nested_conditionals)
        return at;
      --nested_conditionals;
    } else if (!parens && !brackets && !braces &&
               (tok_is(p, at, ",") || tok_is(p, at, ";")))
      return at;
  }
  return at;
}

static bool function_signatures_compatible(const c_function_t *left,
                                           const c_function_t *right) {
  if (!left || !right || left->result != right->result ||
      left->result_agg != right->result_agg ||
      left->result_is_bool != right->result_is_bool ||
      left->param_count != right->param_count ||
      left->variadic != right->variadic)
    return false;
  for (size_t i = 0; i < left->param_count; ++i)
    if (left->params[i].type != right->params[i].type ||
        left->params[i].agg != right->params[i].agg)
      return false;
  return true;
}

static bool parse_global_types_compatible(c_parser_t *p, size_t *at,
                                          int64_t *value) {
  c_parser_t probe = *p;
  char probe_error[1] = {0};
  probe.err = probe_error;
  probe.err_len = sizeof(probe_error);
  probe.type_lookahead = true;
  probe.pos = *at;
  if (!accept(&probe, "__builtin_types_compatible_p") || !expect(&probe, "("))
    return false;
  c_type_t left_type;
  if (!parse_type(&probe, &left_type, true) || !expect(&probe, ",")) {
    return false;
  }
  c_agg_type_t *left_agg = probe.last_type_agg;
  bool left_is_array = probe.last_type_is_array;
  ny_ctok_t left_function = probe.last_type_typeof_function;
  bool left_function_type = probe.last_type_function_type;
  bool left_void_params = probe.last_type_function_void_params;
  c_type_t left_function_result = probe.last_type_function_result_type;
  c_agg_type_t *left_function_result_agg = probe.last_type_function_result_agg;
  c_function_t *left_function_decl =
      left_function.len ? find_function(p, left_function) : NULL;
  size_t right_type_start = probe.pos;
  c_parser_t abstract_probe = probe;
  c_type_t abstract_result_type = C_BAD;
  bool parsed_abstract =
      left_type == C_FUNC_PTR && left_function_decl &&
      parse_type(&abstract_probe, &abstract_result_type, true);
  size_t declarator_pos = right_type_start;
  bool abstract_function_pointer = false;
  if (parsed_abstract && abstract_probe.last_type_is_function_declarator &&
      abstract_result_type == C_FUNC_PTR) {
    for (; declarator_pos + 1 < abstract_probe.pos; ++declarator_pos) {
      if (!tok_is(&abstract_probe, declarator_pos, "(") ||
          !tok_is(&abstract_probe, declarator_pos + 1, "*"))
        continue;
      size_t after_star = declarator_pos + 2;
      while (tok_is(&abstract_probe, after_star, "const") ||
             tok_is(&abstract_probe, after_star, "volatile") ||
             tok_is(&abstract_probe, after_star, "restrict") ||
             tok_is(&abstract_probe, after_star, "__restrict") ||
             tok_is(&abstract_probe, after_star, "__restrict__"))
        ++after_star;
      if (tok_ident(abstract_probe.tokens[after_star]))
        ++after_star;
      if (tok_is(&abstract_probe, after_star, ")") &&
          tok_is(&abstract_probe, after_star + 1, "(")) {
        abstract_function_pointer = true;
        break;
      }
    }
  }
  if (abstract_function_pointer) {
    probe = abstract_probe;
    probe.pos = declarator_pos;
    abstract_result_type = abstract_probe.last_type_function_result_type;
    probe.last_type_agg = abstract_probe.last_type_function_result_agg;
  }
  if (abstract_function_pointer) {
    c_type_t result_type = abstract_result_type;
    if (!expect(&probe, "(") || !expect(&probe, "*") || !expect(&probe, ")") ||
        !expect(&probe, "(")) {
      return false;
    }
    bool compatible = result_type == left_function_decl->result &&
                      probe.last_type_agg == left_function_decl->result_agg;
    size_t parameter_index = 0;
    bool right_variadic = false;
    bool void_parameters =
        tok_is(&probe, probe.pos, "void") && tok_is(&probe, probe.pos + 1, ")");
    if (void_parameters) {
      ++probe.pos;
    } else if (!tok_is(&probe, probe.pos, ")")) {
      for (;;) {
        if (accept(&probe, "...")) {
          right_variadic = true;
          break;
        }
        c_type_t parameter_type;
        if (!parse_type(&probe, &parameter_type, true)) {
          return false;
        }
        c_agg_type_t *parameter_agg = probe.last_type_agg;
        compatible &=
            parameter_index < left_function_decl->param_count &&
            left_function_decl->params[parameter_index].type ==
                parameter_type &&
            left_function_decl->params[parameter_index].agg == parameter_agg;
        ++parameter_index;
        if (tok_ident(*current(&probe)))
          ++probe.pos;
        if (!accept(&probe, ","))
          break;
      }
    }
    if (!expect(&probe, ")") || !expect(&probe, ")")) {
      return false;
    }
    compatible &= void_parameters
                      ? left_function_decl->param_count == 0 &&
                            !left_function_decl->variadic
                      : parameter_index == left_function_decl->param_count &&
                            right_variadic == left_function_decl->variadic;
    *at = probe.pos;
    *value = compatible;
    return true;
  }
  c_type_t right_type;
  if (!parse_type(&probe, &right_type, true) || !expect(&probe, ")"))
    return false;
  c_agg_type_t *right_agg = probe.last_type_agg;
  bool right_is_array = probe.last_type_is_array;
  ny_ctok_t right_function = probe.last_type_typeof_function;
  bool right_function_type = probe.last_type_function_type;
  bool right_void_params = probe.last_type_function_void_params;
  c_type_t right_function_result = probe.last_type_function_result_type;
  c_agg_type_t *right_function_result_agg = probe.last_type_function_result_agg;
  bool compatible = false;
  if (left_function.len && right_function.len) {
    compatible =
        left_function.len && right_function.len &&
        function_signatures_compatible(find_function(p, left_function),
                                       find_function(p, right_function));
  } else if (left_type == C_FUNC_PTR && right_type == C_FUNC_PTR &&
             left_function_type && right_function_type &&
             left_function_result == right_function_result &&
             left_function_result_agg == right_function_result_agg) {
    if (left_function.len) {
      c_function_t *function = find_function(p, left_function);
      compatible = right_void_params && function &&
                   function->param_count == 0 && !function->variadic;
    } else if (right_function.len) {
      c_function_t *function = find_function(p, right_function);
      compatible = left_void_params && function && function->param_count == 0 &&
                   !function->variadic;
    } else {
      compatible = left_void_params && right_void_params;
    }
  } else {
    compatible = left_type == right_type && left_agg == right_agg &&
                 left_is_array == right_is_array;
  }
  *at = probe.pos;
  *value = compatible;
  return true;
}

static bool parse_global_pointer_constant(c_parser_t *p, size_t *at,
                                          int64_t *value, int64_t *stride) {
  if (!tok_is(p, *at, "("))
    return false;
  c_parser_t probe = *p;
  char error[1] = {0};
  probe.err = error;
  probe.err_len = sizeof(error);
  probe.type_lookahead = true;
  probe.pos = *at + 1;
  c_type_t type;
  if (parse_type(&probe, &type, true) && c_type_is_object_pointer(type) &&
      tok_is(&probe, probe.pos, ")")) {
    *stride = c_pointer_scale_for(p->abi, type, probe.last_type_agg);
    *at = probe.pos + 1;
    return parse_global_const_primary(p, at, value);
  }
  ++*at;
  if (!parse_global_pointer_constant(p, at, value, stride) ||
      !tok_is(p, *at, ")"))
    return false;
  ++*at;
  return true;
}

static bool parse_global_generic_constant(c_parser_t *p, size_t *at,
                                          int64_t *value) {
  c_parser_t probe = *p;
  char error[1] = {0};
  probe.err = error;
  probe.err_len = sizeof(error);
  probe.type_lookahead = true;
  probe.pos = *at;
  if (!expect(&probe, "_Generic") || !expect(&probe, "("))
    return false;
  c_type_t control_type;
  if (!static_typeof_expr_type(&probe, &control_type, 1))
    return false;
  if (probe.last_type_is_array && !probe.last_type_array_decayed)
    control_type = c_array_decay_type(control_type);
  c_agg_type_t *control_agg = probe.last_type_agg;
  size_t selected_start = SIZE_MAX, selected_end = SIZE_MAX;
  size_t default_start = SIZE_MAX, default_end = SIZE_MAX;
  for (;;) {
    if (tok_is(&probe, probe.pos, ")"))
      break;
    if (!expect(&probe, ","))
      return false;
    bool is_default = accept(&probe, "default");
    c_type_t selector_type = C_BAD;
    c_agg_type_t *selector_agg = NULL;
    if (!is_default) {
      if (!parse_type(&probe, &selector_type, true))
        return false;
      selector_agg = probe.last_type_agg;
    }
    if (!expect(&probe, ":"))
      return false;
    size_t branch_start = probe.pos;
    if (!skip_to_comma_or_close(&probe))
      return false;
    size_t branch_end = probe.pos;
    if (is_default) {
      if (default_start == SIZE_MAX) {
        default_start = branch_start;
        default_end = branch_end;
      }
    } else if (selected_start == SIZE_MAX && selector_type == control_type &&
               selector_agg == control_agg) {
      selected_start = branch_start;
      selected_end = branch_end;
    }
  }
  size_t close = probe.pos;
  if (!expect(&probe, ")"))
    return false;
  if (selected_start == SIZE_MAX) {
    selected_start = default_start;
    selected_end = default_end;
  }
  if (selected_start == SIZE_MAX)
    return false;
  size_t selected_pos = selected_start;
  if (!parse_global_const_expr(p, &selected_pos, value) ||
      selected_pos != selected_end)
    return false;
  *at = close + 1;
  return true;
}

static bool parse_global_const_primary(c_parser_t *p, size_t *at,
                                       int64_t *value) {
  p->global_const_unsigned = false;
  if (tok_is(p, *at, "__builtin_has_attribute")) {
    /*
     * Linux uses this in compile-time string checks.  The frontend currently
     * drops field attributes after parsing, so it can only answer the
     * unannotated case.  Keep the expression boundary exact and accept the
     * `nonstring` query used by the kernel.
     */
    ++*at;
    if (!tok_is(p, *at, "("))
      return false;
    size_t cursor = *at + 1;
    size_t parens = 0, brackets = 0, braces = 0;
    size_t comma = SIZE_MAX, close = SIZE_MAX;
    for (; cursor < p->token_count; ++cursor) {
      if (tok_is(p, cursor, "("))
        ++parens;
      else if (tok_is(p, cursor, ")")) {
        if (!parens && !brackets && !braces) {
          close = cursor;
          break;
        }
        if (parens)
          --parens;
      } else if (tok_is(p, cursor, "["))
        ++brackets;
      else if (tok_is(p, cursor, "]")) {
        if (!brackets)
          return false;
        --brackets;
      } else if (tok_is(p, cursor, "{"))
        ++braces;
      else if (tok_is(p, cursor, "}")) {
        if (!braces)
          return false;
        --braces;
      } else if (tok_is(p, cursor, ",") && !parens && !brackets && !braces &&
                 comma == SIZE_MAX) {
        comma = cursor;
      }
    }
    if (comma == SIZE_MAX || close == SIZE_MAX || comma + 2 != close ||
        (!tok_is(p, comma + 1, "nonstring") &&
         !tok_is(p, comma + 1, "__nonstring__")))
      return false;
    *at = close + 1;
    *value = 0;
    return true;
  }
  if (p->inline_asm_constant_context && tok_is(p, *at, "__order_base_2")) {
    ++*at;
    if (!tok_is(p, *at, "("))
      return false;
    ++*at;
    int64_t operand = 0;
    if (!parse_global_const_expr(p, at, &operand) || !tok_is(p, *at, ")"))
      return false;
    ++*at;
    uint64_t unsigned_operand = (uint64_t)operand;
    *value = unsigned_operand <= 1
                 ? 0
                 : 64 - (int64_t)__builtin_clzll(unsigned_operand - 1);
    return true;
  }
  if (tok_is(p, *at, "__builtin_types_compatible_p"))
    return parse_global_types_compatible(p, at, value);
  if (tok_is(p, *at, "__builtin_ffs") || tok_is(p, *at, "__builtin_ctz")) {
    bool is_ffs = tok_is(p, *at, "__builtin_ffs");
    ++*at;
    int64_t operand = 0;
    if (!tok_is(p, *at, "("))
      return false;
    ++*at;
    if (!parse_global_const_expr(p, at, &operand) || !tok_is(p, *at, ")"))
      return false;
    ++*at;
    uint32_t bits = (uint32_t)operand;
    if (!bits) {
      if (!is_ffs)
        return false;
      *value = 0;
    } else {
      *value = is_ffs ? (int64_t)__builtin_ctz(bits) + 1
                      : (int64_t)__builtin_ctz(bits);
    }
    return true;
  }
  if (tok_is(p, *at, "__builtin_strlen")) {
    ++*at;
    if (!tok_is(p, *at, "("))
      return false;
    ++*at;
    size_t length = 0;
    bool have_string = false;
    bool terminated = false;
    while (*at < p->token_count && p->tokens[*at].kind == NY_CTOK_STRING) {
      unsigned char *bytes = NULL;
      size_t bytes_len = 0;
      if (!decode_string_token(p->tokens[(*at)++], &bytes, &bytes_len)) {
        free(bytes);
        return false;
      }
      have_string = true;
      size_t part_len = bytes_len ? bytes_len - 1 : 0;
      if (!terminated) {
        unsigned char *nul = part_len ? memchr(bytes, '\0', part_len) : NULL;
        if (nul) {
          length += (size_t)(nul - bytes);
          terminated = true;
        } else {
          if (length > SIZE_MAX - part_len) {
            free(bytes);
            return false;
          }
          length += part_len;
        }
      }
      free(bytes);
    }
    if (!have_string || !tok_is(p, *at, ")") || length > INT64_MAX)
      return false;
    ++*at;
    *value = (int64_t)length;
    return true;
  }
  if (tok_is(p, *at, "_Generic"))
    return parse_global_generic_constant(p, at, value);
  if (tok_is(p, *at, "__builtin_choose_expr")) {
    int64_t condition = 0;
    ++*at;
    if (!tok_is(p, *at, "("))
      return false;
    ++*at;
    if (!parse_global_const_expr(p, at, &condition) || !tok_is(p, *at, ","))
      return false;
    ++*at;
    size_t true_start = *at;
    size_t second_comma = SIZE_MAX;
    size_t close = SIZE_MAX;
    size_t parens = 0, brackets = 0, braces = 0;
    for (size_t cursor = true_start; cursor < p->token_count; ++cursor) {
      if (tok_is(p, cursor, "("))
        ++parens;
      else if (tok_is(p, cursor, ")")) {
        if (!parens && !brackets && !braces) {
          close = cursor;
          break;
        }
        if (parens)
          --parens;
      } else if (tok_is(p, cursor, "["))
        ++brackets;
      else if (tok_is(p, cursor, "]")) {
        if (!brackets)
          return false;
        --brackets;
      } else if (tok_is(p, cursor, "{"))
        ++braces;
      else if (tok_is(p, cursor, "}")) {
        if (!braces)
          return false;
        --braces;
      } else if (tok_is(p, cursor, ",") && !parens && !brackets && !braces) {
        if (second_comma == SIZE_MAX)
          second_comma = cursor;
      }
    }
    if (second_comma == SIZE_MAX || second_comma >= close)
      return false;
    size_t branch_end = condition ? second_comma : close;
    *at = condition ? true_start : second_comma + 1;
    if (!parse_global_const_expr(p, at, value) || *at != branch_end)
      return false;
    ++close;
    if (close >= p->token_count)
      return false;
    *at = close;
    return true;
  }
  if (tok_is(p, *at, "__typeof__") || tok_is(p, *at, "typeof")) {
    ++*at;
    if (!tok_is(p, *at, "("))
      return false;
    size_t close = *at + 1;
    size_t depth = 1;
    for (; close < p->token_count && depth; ++close) {
      if (tok_is(p, close, "("))
        ++depth;
      else if (tok_is(p, close, ")"))
        --depth;
    }
    if (depth)
      return false;
    *at = close;
    if (tok_is(p, *at, ")") && tok_is(p, *at + 1, "("))
      ++*at;
    return parse_global_const_primary(p, at, value);
  }
  if (tok_is(p, *at, "~") || tok_is(p, *at, "!") || tok_is(p, *at, "+")) {
    ny_ctok_t operation = p->tokens[(*at)++];
    bool unsigned_int_operand = false;
    if (ny_ctok_eq(operation, "~") && *at < p->token_count &&
        p->tokens[*at].kind == NY_CTOK_NUMBER) {
      ny_ctok_t operand = p->tokens[*at];
      bool has_unsigned_suffix = false, has_long_suffix = false;
      for (size_t i = 0; i < operand.len; ++i) {
        has_unsigned_suffix |=
            operand.start[i] == 'u' || operand.start[i] == 'U';
        has_long_suffix |= operand.start[i] == 'l' || operand.start[i] == 'L';
      }
      unsigned_int_operand = has_unsigned_suffix && !has_long_suffix;
    } else if (ny_ctok_eq(operation, "~") && tok_is(p, *at, "(")) {
      size_t close = *at + 1;
      size_t depth = 1;
      for (; close < p->token_count && depth; ++close) {
        if (tok_is(p, close, "("))
          ++depth;
        else if (tok_is(p, close, ")"))
          --depth;
      }
      if (!depth && global_const_cast_is_u32(p, *at, close - 1))
        unsigned_int_operand = true;
    }
    if (!parse_global_const_primary(p, at, value))
      return false;
    bool operand_unsigned = p->global_const_unsigned;
    if (ny_ctok_eq(operation, "~"))
      *value = unsigned_int_operand ? (int64_t)(uint32_t)~(uint32_t)*value
                                    : (int64_t)~(uint64_t)*value;
    else if (ny_ctok_eq(operation, "!"))
      *value = !*value;
    p->global_const_unsigned =
        ny_ctok_eq(operation, "!") ? false : operand_unsigned;
    return true;
  }
  if (tok_is(p, *at, "&")) {
    ++*at;
    int64_t stride = 0;
    if (!parse_global_pointer_constant(p, at, value, &stride) ||
        !tok_is(p, *at, "["))
      return false;
    ++*at;
    int64_t index = 0, offset = 0;
    if (!parse_global_const_expr(p, at, &index) || !tok_is(p, *at, "]") ||
        __builtin_mul_overflow(index, stride, &offset) ||
        __builtin_add_overflow(*value, offset, value))
      return false;
    ++*at;
    return true;
  }
  if (tok_is(p, *at, "__builtin_constant_p") ||
      tok_is(p, *at, "__builtin_clzll") || tok_is(p, *at, "__builtin_clzl") ||
      tok_is(p, *at, "__builtin_clz") || tok_is(p, *at, "__builtin_ctz") ||
      tok_is(p, *at, "__builtin_ctzl") || tok_is(p, *at, "__builtin_ctzll") ||
      tok_is(p, *at, "__builtin_bswap16") ||
      tok_is(p, *at, "__builtin_bswap32") ||
      tok_is(p, *at, "__builtin_bswap64") || tok_is(p, *at, "__ilog2_u32") ||
      tok_is(p, *at, "__ilog2_u64") || tok_is(p, *at, "__bits_per") ||
      tok_is(p, *at, "bits_per") || tok_is(p, *at, "__roundup_pow_of_two")) {
    if (tok_is(p, *at, "__builtin_constant_p")) {
      size_t open = *at + 1;
      size_t after = skip_balanced(p, open, "(", ")");
      if (!tok_is(p, open, "(") || after <= open + 2 || after >= p->token_count)
        return false;
      c_parser_t argument = *p;
      argument.pos = open + 1;
      char argument_error[128] = {0};
      argument.err = argument_error;
      argument.err_len = sizeof(argument_error);
      int64_t ignored = 0;
      bool constant =
          parse_global_const_expr(&argument, &argument.pos, &ignored) &&
          argument.pos + 1 == after && tok_is(&argument, argument.pos, ")");
      *at = after;
      *value = constant ? 1 : 0;
      p->global_const_unsigned = false;
      return true;
    }
    c_parser_t probe = *p;
    char probe_error[1] = {0};
    probe.err = probe_error;
    probe.err_len = sizeof(probe_error);
    probe.type_lookahead = true;
    probe.pos = *at;
    uint64_t bits = 0;
    bool unsigned_result = tok_is(p, *at, "__builtin_bswap32") ||
                           tok_is(p, *at, "__builtin_bswap64") ||
                           tok_is(p, *at, "__roundup_pow_of_two");
    if (!parse_array_extent_atom(&probe, &bits) ||
        (!unsigned_result && bits > INT64_MAX))
      return false;
    *at = probe.pos;
    *value = (int64_t)bits;
    p->global_const_unsigned = unsigned_result;
    return true;
  }
  if (tok_is(p, *at, "sizeof") || tok_is(p, *at, "__alignof__") ||
      tok_is(p, *at, "__alignof")) {
    c_parser_t probe = *p;
    char probe_error[1] = {0};
    probe.err = probe_error;
    probe.err_len = sizeof(probe_error);
    probe.type_lookahead = true;
    probe.pos = *at;
    uint64_t size = 0;
    if (!parse_array_extent_atom(&probe, &size)) {
      if (!tok_is(p, *at, "sizeof"))
        return false;
      c_parser_t expression_probe = *p;
      char expression_error[1] = {0};
      expression_probe.err = expression_error;
      expression_probe.err_len = sizeof(expression_error);
      expression_probe.type_lookahead = true;
      expression_probe.pos = *at + 1;
      if (!expect(&expression_probe, "("))
        return false;
      bool compound_array = false;
      bool anonymous_aggregate_size = false;
      size_t operand = expression_probe.pos;
      if ((tok_is(p, operand, "struct") || tok_is(p, operand, "union")) &&
          tok_is(p, operand + 1, "{")) {
        size_t capacity = expression_probe.tag_count + 1;
        c_agg_type_t **scratch = malloc(capacity * sizeof(*scratch));
        if (!scratch)
          return false;
        if (expression_probe.tag_count)
          memcpy(scratch, expression_probe.tags,
                 expression_probe.tag_count * sizeof(*scratch));
        c_parser_t aggregate = expression_probe;
        aggregate.tags = scratch;
        aggregate.tag_capacity = capacity;
        aggregate.type_lookahead = false;
        aggregate.suppress_static_asserts = true;
        c_type_t aggregate_type = C_BAD;
        bool aggregate_ok = parse_type(&aggregate, &aggregate_type, false);
        size = aggregate_ok && aggregate.last_type_agg
                   ? aggregate.last_type_agg->size
                   : 0;
        free(scratch);
        if (!aggregate_ok || !expect(&aggregate, ")"))
          return false;
        expression_probe.pos = aggregate.pos;
        anonymous_aggregate_size = true;
      } else if (!parse_static_array_compound_literal_size(
                     &expression_probe, &size, &compound_array)) {
        return false;
      } else if (compound_array && !expect(&expression_probe, ")")) {
        return false;
      } else if (!compound_array) {
        c_type_t expression_type = C_BAD;
        if (!static_typeof_expr_type(&expression_probe, &expression_type, 0) ||
            !expect(&expression_probe, ")"))
          return false;
        size = expression_probe.last_type_is_array &&
                       !expression_probe.last_type_array_decayed
                   ? expression_probe.last_type_array_size
               : expression_type == C_AGG && expression_probe.last_type_agg
                   ? expression_probe.last_type_agg->size
                   : c_type_size(p->abi, expression_type);
      }
      bool zero_length_array =
          compound_array && size == 0 && !anonymous_aggregate_size;
      if (!size && tok_ident(p->tokens[*at + 2]) && tok_is(p, *at + 3, ")")) {
        c_global_t *global = find_global(p, p->tokens[*at + 2]);
        zero_length_array = global && global->is_array &&
                            !global->array_incomplete && global->len == 0;
      }
      if (!size && !zero_length_array && !anonymous_aggregate_size)
        return false;
      probe.pos = expression_probe.pos;
    }
    if (size > INT64_MAX)
      return false;
    *at = probe.pos;
    *value = (int64_t)size;
    return true;
  }
  if (tok_is(p, *at, "__builtin_offsetof")) {
    c_parser_t probe = *p;
    char probe_error[1] = {0};
    probe.err = probe_error;
    probe.err_len = sizeof(probe_error);
    probe.type_lookahead = true;
    probe.pos = *at;
    uint64_t offset = 0;
    if (!parse_array_extent_atom(&probe, &offset) || offset > INT64_MAX)
      return false;
    *at = probe.pos;
    *value = (int64_t)offset;
    return true;
  }
  if (tok_is(p, *at, "(")) {
    if ((*at + 3 < p->token_count &&
         (tok_is(p, *at + 1, "__typeof__") || tok_is(p, *at + 1, "typeof")) &&
         tok_is(p, *at + 2, "("))) {
      size_t close = *at + 3;
      size_t depth = 1;
      for (; close < p->token_count && depth; ++close) {
        if (tok_is(p, close, "("))
          ++depth;
        else if (tok_is(p, close, ")"))
          --depth;
      }
      if (!depth && tok_is(p, close, ")") && tok_is(p, close + 1, "(")) {
        *at = close + 1;
        return parse_global_const_primary(p, at, value);
      }
    }
    size_t close = *at + 1;
    size_t depth = 1;
    for (; close < p->token_count && depth; ++close) {
      if (tok_is(p, close, "("))
        ++depth;
      else if (tok_is(p, close, ")"))
        --depth;
    }
    if (depth == 0 && global_const_cast_type(p, *at, close - 1)) {
      c_type_t cast_type = C_BAD;
      bool cast_unsigned =
          global_const_cast_unsigned(p, *at, close - 1, &cast_type);
      *at = close;
      if (!parse_global_const_primary(p, at, value))
        return false;
      switch (cast_type) {
      case C_CHAR:
        *value = (int8_t)(uint8_t)*value;
        break;
      case C_I16:
        *value = (int16_t)(uint16_t)*value;
        break;
      case C_U8:
        *value = (uint8_t)*value;
        break;
      case C_U16:
        *value = (uint16_t)*value;
        break;
      case C_I32:
        *value = (int32_t)(uint32_t)*value;
        break;
      case C_U32:
        *value = (uint32_t)*value;
        break;
      default:
        break;
      }
      p->global_const_unsigned = cast_unsigned;
      return true;
    }
    ++*at;
    if (!parse_global_const_expr(p, at, value) || !tok_is(p, *at, ")"))
      return false;
    ++*at;
    return true;
  }
  if (tok_is(p, *at, "-")) {
    ++*at;
    if (!parse_global_const_primary(p, at, value) || *value == INT64_MIN)
      return false;
    *value = -*value;
    return true;
  }
  ny_ctok_t token = p->tokens[*at];
  if (token.kind == NY_CTOK_CHAR) {
    uint64_t parsed = 0;
    if (!parse_global_byte_value(token, &parsed))
      return false;
    *value = (int64_t)parsed;
    p->global_const_unsigned = false;
    ++*at;
    return true;
  }
  if (token.kind == NY_CTOK_NUMBER) {
    char text[128];
    if (!token.len || token.len >= sizeof(text))
      return false;
    memcpy(text, token.start, token.len);
    text[token.len] = '\0';
    size_t len = strlen(text);
    while (len && strchr("uUlL", text[len - 1]))
      text[--len] = '\0';
    bool unsigned_suffix = false;
    for (size_t i = len; i < token.len; ++i)
      unsigned_suffix |= token.start[i] == 'u' || token.start[i] == 'U';
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 0);
    if (errno || end == text || *end)
      return false;
    *value = (int64_t)parsed;
    p->global_const_unsigned =
        unsigned_suffix || parsed > (unsigned long long)INT64_MAX;
    ++*at;
    return true;
  }
  if (token.kind != NY_CTOK_IDENT)
    return false;
  const c_enum_constant_t *enumerator = find_enum_constant(p, token);
  if (enumerator) {
    *value = enumerator->value;
    p->global_const_unsigned =
        enumerator->type == C_U32 || enumerator->type == C_U64;
    ++*at;
    return true;
  }
  for (size_t i = 0; i < p->pp_constant_count; ++i) {
    c_pp_constant_t *constant = &p->pp_constants[i];
    if (constant->integer_value && constant->name_len == token.len &&
        memcmp(constant->name, token.start, token.len) == 0) {
      *value = constant->value;
      ++*at;
      return true;
    }
  }
  return false;
}

static bool parse_global_const_mul(c_parser_t *p, size_t *at, int64_t *value) {
  if (!parse_global_const_primary(p, at, value))
    return false;
  bool lhs_unsigned = p->global_const_unsigned;
  while (tok_is(p, *at, "*") || tok_is(p, *at, "/") || tok_is(p, *at, "%")) {
    const char *op = p->tokens[(*at)++].start;
    int64_t rhs;
    if (!parse_global_const_primary(p, at, &rhs))
      return false;
    bool rhs_unsigned = p->global_const_unsigned;
    bool operation_unsigned = lhs_unsigned || rhs_unsigned;
    if (*op == '*') {
      if (operation_unsigned)
        *value = (int64_t)((uint64_t)*value * (uint64_t)rhs);
      else if (__builtin_mul_overflow(*value, rhs, value))
        return false;
    } else {
      if (!rhs || (!operation_unsigned && *value == INT64_MIN && rhs == -1))
        return false;
      if (operation_unsigned) {
        uint64_t dividend = (uint64_t)*value;
        uint64_t divisor = (uint64_t)rhs;
        *value =
            (int64_t)(*op == '/' ? dividend / divisor : dividend % divisor);
      } else {
        *value = *op == '/' ? *value / rhs : *value % rhs;
      }
    }
    lhs_unsigned = operation_unsigned;
  }
  p->global_const_unsigned = lhs_unsigned;
  return true;
}

static bool parse_global_const_add(c_parser_t *p, size_t *at, int64_t *value) {
  if (!parse_global_const_mul(p, at, value))
    return false;
  bool lhs_unsigned = p->global_const_unsigned;
  while (tok_is(p, *at, "+") || tok_is(p, *at, "-")) {
    bool subtract = tok_is(p, *at, "-");
    ++*at;
    int64_t rhs;
    if (!parse_global_const_mul(p, at, &rhs))
      return false;
    bool operation_unsigned = lhs_unsigned || p->global_const_unsigned;
    if (operation_unsigned) {
      *value = (int64_t)(subtract ? (uint64_t)*value - (uint64_t)rhs
                                  : (uint64_t)*value + (uint64_t)rhs);
    } else if (subtract ? __builtin_sub_overflow(*value, rhs, value)
                        : __builtin_add_overflow(*value, rhs, value)) {
      return false;
    }
    lhs_unsigned = operation_unsigned;
  }
  p->global_const_unsigned = lhs_unsigned;
  return true;
}

static bool parse_global_const_shift(c_parser_t *p, size_t *at,
                                     int64_t *value) {
  if (!parse_global_const_add(p, at, value))
    return false;
  bool lhs_unsigned = p->global_const_unsigned;
  while (tok_is(p, *at, "<<") || tok_is(p, *at, ">>")) {
    bool left = tok_is(p, *at, "<<");
    ++*at;
    int64_t count;
    if (!parse_global_const_add(p, at, &count) || count < 0 || count >= 64)
      return false;
    if (left)
      *value = (int64_t)((uint64_t)*value << (unsigned)count);
    else if (lhs_unsigned)
      *value = (int64_t)((uint64_t)*value >> (unsigned)count);
    else
      *value >>= (unsigned)count;
    p->global_const_unsigned = lhs_unsigned;
  }
  return true;
}

static bool parse_global_const_bitand(c_parser_t *p, size_t *at,
                                      int64_t *value) {
  if (!parse_global_const_shift(p, at, value))
    return false;
  bool lhs_unsigned = p->global_const_unsigned;
  while (tok_is(p, *at, "&") && !tok_is(p, *at + 1, "&")) {
    ++*at;
    int64_t rhs;
    if (!parse_global_const_shift(p, at, &rhs))
      return false;
    lhs_unsigned = lhs_unsigned || p->global_const_unsigned;
    *value &= rhs;
  }
  p->global_const_unsigned = lhs_unsigned;
  return true;
}

static bool parse_global_const_bitxor(c_parser_t *p, size_t *at,
                                      int64_t *value) {
  if (!parse_global_const_bitand(p, at, value))
    return false;
  bool lhs_unsigned = p->global_const_unsigned;
  while (tok_is(p, *at, "^")) {
    ++*at;
    int64_t rhs;
    if (!parse_global_const_bitand(p, at, &rhs))
      return false;
    lhs_unsigned = lhs_unsigned || p->global_const_unsigned;
    *value ^= rhs;
  }
  p->global_const_unsigned = lhs_unsigned;
  return true;
}

static bool parse_global_const_bitor(c_parser_t *p, size_t *at,
                                     int64_t *value) {
  if (!parse_global_const_bitxor(p, at, value))
    return false;
  bool lhs_unsigned = p->global_const_unsigned;
  while (tok_is(p, *at, "|") && !tok_is(p, *at + 1, "|")) {
    ++*at;
    int64_t rhs;
    if (!parse_global_const_bitxor(p, at, &rhs))
      return false;
    lhs_unsigned = lhs_unsigned || p->global_const_unsigned;
    *value |= rhs;
  }
  p->global_const_unsigned = lhs_unsigned;
  return true;
}

static bool parse_global_const_compare(c_parser_t *p, size_t *at,
                                       int64_t *value) {
  if (!parse_global_const_bitor(p, at, value))
    return false;
  bool lhs_unsigned = p->global_const_unsigned;
  for (;;) {
    int op = tok_is(p, *at, "==")   ? 1
             : tok_is(p, *at, "!=") ? 2
             : tok_is(p, *at, ">=") ? 3
             : tok_is(p, *at, "<=") ? 4
             : tok_is(p, *at, ">")  ? 5
             : tok_is(p, *at, "<")  ? 6
                                    : 0;
    if (!op)
      return true;
    ++*at;
    int64_t rhs;
    if (!parse_global_const_bitor(p, at, &rhs))
      return false;
    bool operation_unsigned = lhs_unsigned || p->global_const_unsigned;
    uint64_t lhs_bits = (uint64_t)*value;
    uint64_t rhs_bits = (uint64_t)rhs;
    *value =
        op == 1   ? *value == rhs
        : op == 2 ? *value != rhs
        : op == 3 ? operation_unsigned ? lhs_bits >= rhs_bits : *value >= rhs
        : op == 4 ? operation_unsigned ? lhs_bits <= rhs_bits : *value <= rhs
        : op == 5 ? operation_unsigned ? lhs_bits > rhs_bits : *value > rhs
        : operation_unsigned ? lhs_bits < rhs_bits
                             : *value < rhs;
    lhs_unsigned = false;
  }
}

static bool parse_global_const_logical_and(c_parser_t *p, size_t *at,
                                           int64_t *value) {
  if (!parse_global_const_compare(p, at, value))
    return false;
  while (tok_is(p, *at, "&&")) {
    ++*at;
    int64_t rhs;
    if (!parse_global_const_compare(p, at, &rhs))
      return false;
    *value = *value && rhs;
    p->global_const_unsigned = false;
  }
  return true;
}

static bool parse_global_const_expr(c_parser_t *p, size_t *at, int64_t *value) {
  /*
   * A string literal is a non-null address, so `("x") ? sizeof("x") - 1 : 0`
   * selects the true operand. The literal itself is not an integer constant,
   * so recognize this conditional shape before the arithmetic parser sees it.
   */
  if (tok_is(p, *at, "(")) {
    size_t probe = *at + 1;
    bool have_string = false;
    while (probe < p->token_count && p->tokens[probe].kind == NY_CTOK_STRING) {
      have_string = true;
      ++probe;
    }
    if (have_string && tok_is(p, probe, ")") && tok_is(p, probe + 1, "?")) {
      *at = probe + 2;
      int64_t selected = 1;
      if (!tok_is(p, *at, ":") && !parse_global_const_expr(p, at, &selected))
        return false;
      if (!tok_is(p, *at, ":"))
        return false;
      ++*at;
      *at = global_const_expression_end(p, *at);
      *value = selected;
      p->global_const_unsigned = false;
      return true;
    }
  }
  if (!parse_global_const_logical_and(p, at, value))
    return false;
  while (tok_is(p, *at, "||")) {
    ++*at;
    int64_t rhs;
    if (!parse_global_const_logical_and(p, at, &rhs))
      return false;
    *value = *value || rhs;
    p->global_const_unsigned = false;
  }
  if (tok_is(p, *at, "?")) {
    bool condition = *value != 0;
    int64_t when_true = *value;
    bool when_true_unsigned = p->global_const_unsigned;
    ++*at;
    if (!tok_is(p, *at, ":")) {
      if (condition) {
        if (!parse_global_const_expr(p, at, &when_true))
          return false;
        when_true_unsigned = p->global_const_unsigned;
      } else {
        size_t colon = global_const_conditional_colon(p, *at);
        if (colon == SIZE_MAX)
          return false;
        *at = colon;
      }
    }
    if (!tok_is(p, *at, ":"))
      return false;
    ++*at;
    if (condition) {
      /*
       * The unselected operand is unevaluated; Linux uses this for _IOC.
       */
      *at = global_const_expression_end(p, *at);
      *value = when_true;
      p->global_const_unsigned = when_true_unsigned;
    } else if (!parse_global_const_expr(p, at, value)) {
      return false;
    }
  }
  return true;
}

static size_t c_global_field_size(const c_agg_field_t *field) {
  switch (field->kind) {
  case C_AGG_FIELD_I32:
  case C_AGG_FIELD_U32:
  case C_AGG_FIELD_F32:
    return 4;
  case C_AGG_FIELD_U8:
    return 1;
  case C_AGG_FIELD_I16:
  case C_AGG_FIELD_U16:
    return 2;
  case C_AGG_FIELD_ARRAY:
    return field->elem_size;
  default:
    return 8;
  }
}

static bool append_global_reloc(c_parser_t *p, c_global_t *global,
                                size_t offset, ny_ctok_t target) {
  if (global->reloc_count == global->reloc_capacity) {
    size_t capacity = global->reloc_capacity ? global->reloc_capacity * 2 : 8;
    if (capacity < global->reloc_capacity ||
        capacity > SIZE_MAX / sizeof(*global->relocs)) {
      fail(p, "file-scope aggregate relocation table is too large");
      return false;
    }
    c_global_reloc_t *grown =
        realloc(global->relocs, capacity * sizeof(*global->relocs));
    if (!grown) {
      fail(p, "out of memory allocating aggregate data relocations");
      return false;
    }
    global->relocs = grown;
    global->reloc_capacity = capacity;
  }
  global->relocs[global->reloc_count++] =
      (c_global_reloc_t){.offset = offset,
                         .target = target,
                         .width = c_type_size(p->abi, C_VOID_PTR),
                         .type = p->abi == NYC_ABI_I386 ? NYC_DATA_RELOC_ABS32
                                                        : NYC_DATA_RELOC_ABS64};
  return true;
}

static bool append_global_string_reloc(c_parser_t *p, c_global_t *global,
                                       size_t offset);
static bool global_null_pointer_initializer(const c_parser_t *p, size_t *at);

static bool try_initialize_static_null_base_subobject(c_parser_t *p,
                                                      size_t offset,
                                                      unsigned char *bytes,
                                                      bool *handled) {
  *handled = false;
  c_parser_t probe = *p;
  char error[1] = {0};
  probe.err = error;
  probe.err_len = sizeof(error);
  size_t outer_parentheses = 0;
  while (tok_is(&probe, probe.pos, "(")) {
    ++probe.pos;
    ++outer_parentheses;
  }
  if (!accept(&probe, "&"))
    return true;
  if (!expect(&probe, "(") || !expect(&probe, "("))
    return true;
  c_type_t base_type;
  if (!parse_type(&probe, &base_type, false) || base_type != C_AGG_PTR ||
      !expect(&probe, ")"))
    return true;
  const c_agg_type_t *aggregate = probe.last_type_agg;
  int64_t base_value;
  if (!parse_global_const_expr(&probe, &probe.pos, &base_value) ||
      base_value != 0 || !expect(&probe, ")") || !expect(&probe, "->"))
    return true;
  *handled = true;
  size_t addend = 0;
  for (;;) {
    ny_ctok_t member;
    if (!parse_name(&probe, &member))
      return false;
    const c_agg_field_t *field = find_field(aggregate, member);
    if (!field || field->offset > SIZE_MAX - addend) {
      fail(p, "invalid static null-base member address");
      return false;
    }
    addend += field->offset;
    aggregate = field->agg;
    if (accept(&probe, "[")) {
      int64_t index;
      size_t stride = field->elem_size;
      for (unsigned dimension = 1; dimension < field->array_rank; ++dimension) {
        if (field->array_extent[dimension] &&
            stride > SIZE_MAX / field->array_extent[dimension]) {
          fail(p, "static null-base array stride overflows");
          return false;
        }
        stride *= field->array_extent[dimension];
      }
      size_t displacement;
      if (field->kind != C_AGG_FIELD_ARRAY || !stride ||
          !parse_global_const_expr(&probe, &probe.pos, &index) || index < 0 ||
          (field->elem_count && (uint64_t)index >= field->elem_count) ||
          !expect(&probe, "]") || (uint64_t)index > SIZE_MAX / stride ||
          __builtin_mul_overflow((size_t)index, stride, &displacement) ||
          displacement > SIZE_MAX - addend) {
        fail(p, "invalid static null-base array element address");
        return false;
      }
      addend += displacement;
      aggregate = field->agg;
    }
    if (accept(&probe, "."))
      continue;
    break;
  }
  while (outer_parentheses) {
    if (!expect(&probe, ")")) {
      fail(p, "invalid parenthesized static null-base address");
      return false;
    }
    --outer_parentheses;
  }
  if (!tok_is(&probe, probe.pos, ",") && !tok_is(&probe, probe.pos, "}") &&
      !tok_is(&probe, probe.pos, ";")) {
    fail(p, "unsupported static null-base address expression");
    return false;
  }
  uint64_t address = addend;
  memcpy(bytes + offset, &address, c_type_size(p->abi, C_VOID_PTR));
  p->pos = probe.pos;
  return true;
}

static bool initialize_data_pointer(c_parser_t *p, c_global_t *data,
                                    size_t offset, unsigned char *bytes) {
  if (accept(p, "{")) {
    if (accept(p, "}"))
      return true;
    if (!initialize_data_pointer(p, data, offset, bytes))
      return false;
    (void)accept(p, ",");
    return expect(p, "}");
  }
  bool null_base_subobject = false;
  if (!try_initialize_static_null_base_subobject(p, offset, bytes,
                                                 &null_base_subobject))
    return false;
  if (null_base_subobject)
    return true;
  if (tok_is(p, p->pos, "&")) {
    size_t dereference_pos = p->pos + 1;
    size_t wrapper_parentheses = 0;
    while (tok_is(p, dereference_pos, "(")) {
      ++wrapper_parentheses;
      ++dereference_pos;
    }
    size_t cast_open = dereference_pos + 1;
    size_t cast_after = skip_balanced(p, cast_open, "(", ")");
    size_t null_pos = cast_after;
    bool null_dereference =
        tok_is(p, dereference_pos, "*") && cast_after < p->token_count &&
        global_const_cast_type(p, cast_open, cast_after - 1) &&
        global_null_pointer_initializer(p, &null_pos);
    size_t after_expression = null_pos;
    while (null_dereference && wrapper_parentheses) {
      if (!tok_is(p, after_expression, ")")) {
        null_dereference = false;
        break;
      }
      ++after_expression;
      --wrapper_parentheses;
    }
    if (null_dereference &&
        (tok_is(p, after_expression, ",") || tok_is(p, after_expression, "}") ||
         tok_is(p, after_expression, ";"))) {
      memset(bytes + offset, 0, c_type_size(p->abi, C_VOID_PTR));
      p->pos = after_expression;
      return true;
    }
  }
  bool compound_literal = false;
  if (!try_initialize_static_aggregate_compound_literal(p, data, offset,
                                                        &compound_literal))
    return false;
  if (compound_literal)
    return true;
  compound_literal = false;
  if (!try_initialize_static_array_compound_literal(p, data, offset,
                                                    &compound_literal))
    return false;
  if (compound_literal)
    return true;
  size_t initializer_start = p->pos;
  size_t function_parentheses = 0;
  if (p->function && tok_is(p, p->pos, "__func__")) {
    size_t token_pos = p->pos;
    ny_ctok_t original = p->tokens[token_pos];
    size_t length = p->function->name.len + 2;
    char *quoted = malloc(length + 1);
    if (!quoted) {
      fail(p, "out of memory lowering static __func__ initializer");
      return false;
    }
    quoted[0] = '"';
    memcpy(quoted + 1, p->function->name.start, p->function->name.len);
    quoted[length - 1] = '"';
    quoted[length] = '\0';
    p->tokens[token_pos] =
        (ny_ctok_t){.kind = NY_CTOK_STRING, .start = quoted, .len = length};
    bool initialized = append_global_string_reloc(p, data, offset);
    p->tokens[token_pos] = original;
    free(quoted);
    return initialized;
  }
  if (current(p)->kind == NY_CTOK_STRING)
    return append_global_string_reloc(p, data, offset);
  size_t string_pos = p->pos;
  while (tok_is(p, string_pos, "("))
    ++string_pos;
  if (string_pos > p->pos && string_pos < p->token_count &&
      p->tokens[string_pos].kind == NY_CTOK_STRING) {
    size_t string_parentheses = string_pos - p->pos;
    p->pos = string_pos;
    if (!append_global_string_reloc(p, data, offset))
      return false;
    while (string_parentheses) {
      if (!expect(p, ")"))
        return false;
      --string_parentheses;
    }
    return true;
  }
  if (accept(p, "&&")) {
    ny_ctok_t label_name = {0};
    c_label_t *label = NULL;
    if (!p->function || !parse_name(p, &label_name) ||
        !(label = find_label_at(p, label_name, p->pos))) {
      fail(p,
           "GNU label address initializer must name a label in this function");
      return false;
    }
    label->address_taken = true;
    p->function->noinline = true;
    if (p->function->name.len > INT_MAX ||
        p->function->name.len > SIZE_MAX - 32) {
      fail(p, "GNU label address symbol is too long");
      return false;
    }
    size_t symbol_capacity = p->function->name.len + 32;
    char *owned_symbol = malloc(symbol_capacity);
    if (!owned_symbol) {
      fail(p, "out of memory lowering GNU label address initializer");
      return false;
    }
    int symbol_len = snprintf(owned_symbol, symbol_capacity, ".Lnyir_%.*s_L%zu",
                              (int)p->function->name.len,
                              p->function->name.start, label->id);
    if (symbol_len < 0 || (size_t)symbol_len >= symbol_capacity) {
      free(owned_symbol);
      fail(p, "GNU label address symbol is too long");
      return false;
    }
    ny_ctok_t target = {.kind = NY_CTOK_IDENT,
                        .start = owned_symbol,
                        .len = (size_t)symbol_len};
    if (!append_global_reloc(p, data, offset, target)) {
      free(owned_symbol);
      return false;
    }
    data->relocs[data->reloc_count - 1].owned_target = owned_symbol;
    return true;
  }
  if (tok_is(p, p->pos, "_Generic")) {
    c_parser_t probe = *p;
    char error[1] = {0};
    probe.err = error;
    probe.err_len = sizeof(error);
    probe.type_lookahead = true;
    ++probe.pos;
    if (!expect(&probe, "(")) {
      fail(p, "file-scope pointer _Generic requires an opening parenthesis");
      return false;
    }
    c_type_t control_type;
    if (!static_typeof_expr_type(&probe, &control_type, 1)) {
      fail(p, error[0] ? error : "cannot resolve file-scope pointer _Generic");
      return false;
    }
    if (probe.last_type_is_array && !probe.last_type_array_decayed)
      control_type = c_array_decay_type(control_type);
    c_agg_type_t *control_agg = probe.last_type_agg;
    size_t selected_start = SIZE_MAX, selected_end = SIZE_MAX;
    size_t default_start = SIZE_MAX, default_end = SIZE_MAX;
    for (;;) {
      if (tok_is(&probe, probe.pos, ")"))
        break;
      if (!expect(&probe, ",")) {
        fail(p, "malformed file-scope pointer _Generic association");
        return false;
      }
      bool is_default = accept(&probe, "default");
      c_type_t selector_type = C_BAD;
      c_agg_type_t *selector_agg = NULL;
      if (!is_default) {
        if (!parse_type(&probe, &selector_type, true)) {
          fail(p,
               error[0] ? error : "invalid file-scope pointer _Generic type");
          return false;
        }
        selector_agg = probe.last_type_agg;
      }
      if (!expect(&probe, ":")) {
        fail(p, "file-scope pointer _Generic association requires ':'");
        return false;
      }
      size_t branch_start = probe.pos;
      if (!skip_to_comma_or_close(&probe)) {
        fail(p, "unterminated file-scope pointer _Generic association");
        return false;
      }
      size_t branch_end = probe.pos;
      if (is_default) {
        if (default_start == SIZE_MAX) {
          default_start = branch_start;
          default_end = branch_end;
        }
      } else if (selected_start == SIZE_MAX && selector_type == control_type &&
                 selector_agg == control_agg) {
        selected_start = branch_start;
        selected_end = branch_end;
      }
    }
    size_t after_generic = probe.pos + 1;
    if (selected_start == SIZE_MAX) {
      selected_start = default_start;
      selected_end = default_end;
    }
    if (selected_start == SIZE_MAX) {
      fail(p, "file-scope pointer _Generic has no matching association");
      return false;
    }
    size_t saved_pos = p->pos;
    p->pos = selected_start;
    bool initialized = initialize_data_pointer(p, data, offset, bytes) &&
                       p->pos == selected_end;
    if (!initialized) {
      p->pos = saved_pos;
      if (!p->err[0])
        fail(p, "selected file-scope pointer _Generic association is not a "
                "constant address");
      return false;
    }
    p->pos = after_generic;
    return true;
  }
  if (tok_is(p, p->pos, "(")) {
    size_t close = p->pos + 1;
    size_t depth = 1, question = SIZE_MAX, colon = SIZE_MAX;
    for (; close < p->token_count && depth; ++close) {
      if (tok_is(p, close, "("))
        ++depth;
      else if (tok_is(p, close, ")")) {
        if (--depth == 0)
          break;
      } else if (depth == 1 && tok_is(p, close, "?") && question == SIZE_MAX)
        question = close;
      else if (depth == 1 && tok_is(p, close, ":") && question != SIZE_MAX &&
               colon == SIZE_MAX)
        colon = close;
    }
    if (depth == 0 && question != SIZE_MAX && colon != SIZE_MAX) {
      c_parser_t condition_parser = *p;
      condition_parser.token_count = question;
      size_t condition_pos = p->pos + 1;
      int64_t condition = 0;
      if (!parse_global_const_expr(&condition_parser, &condition_pos,
                                   &condition) ||
          condition_pos != question)
        return false;
      size_t branch_start = condition ? question + 1 : colon + 1;
      size_t branch_end = condition ? colon : close;
      size_t saved_pos = p->pos;
      p->pos = branch_start;
      bool initialized = initialize_data_pointer(p, data, offset, bytes) &&
                         p->pos == branch_end;
      if (!initialized) {
        p->pos = saved_pos;
        return false;
      }
      p->pos = close + 1;
      return true;
    }
  }
  size_t conditional = p->pos;
  size_t parens = 0, brackets = 0, braces = 0;
  while (conditional < p->token_count) {
    if (tok_is(p, conditional, "("))
      ++parens;
    else if (tok_is(p, conditional, ")")) {
      if (!parens && !brackets && !braces)
        break;
      if (parens)
        --parens;
    } else if (tok_is(p, conditional, "["))
      ++brackets;
    else if (tok_is(p, conditional, "]")) {
      if (!brackets && !parens && !braces)
        break;
      if (brackets)
        --brackets;
    } else if (tok_is(p, conditional, "{"))
      ++braces;
    else if (tok_is(p, conditional, "}")) {
      if (!braces && !parens && !brackets)
        break;
      if (braces)
        --braces;
    } else if (!parens && !brackets && !braces && tok_is(p, conditional, "?")) {
      size_t colon = global_const_conditional_colon(p, conditional + 1);
      if (colon != SIZE_MAX) {
        c_parser_t condition_parser = *p;
        condition_parser.token_count = conditional;
        size_t condition_pos = p->pos;
        int64_t condition = 0;
        if (!parse_global_const_expr(&condition_parser, &condition_pos,
                                     &condition) ||
            condition_pos != conditional)
          return false;
        size_t branch_start = condition ? conditional + 1 : colon + 1;
        size_t expression_end = global_const_expression_end(p, colon + 1);
        size_t branch_end = condition ? colon : expression_end;
        size_t saved_pos = p->pos;
        p->pos = branch_start;
        bool initialized = initialize_data_pointer(p, data, offset, bytes) &&
                           p->pos == branch_end;
        if (!initialized) {
          p->pos = saved_pos;
          return false;
        }
        p->pos = expression_end;
        return true;
      }
      break;
    } else if (!parens && !brackets && !braces &&
               (tok_is(p, conditional, ",") || tok_is(p, conditional, ";"))) {
      break;
    }
    ++conditional;
  }
  while (tok_is(p, p->pos, "(")) {
    size_t close = p->pos + 1;
    size_t depth = 1;
    for (; close < p->token_count && depth; ++close) {
      if (tok_is(p, close, "("))
        ++depth;
      else if (tok_is(p, close, ")") && --depth == 0)
        break;
    }
    if (depth)
      break;
    if (global_const_cast_type(p, p->pos, close)) {
      p->pos = close + 1;
      continue;
    }
    size_t name_pos = p->pos + 1;
    while (tok_is(p, name_pos, "("))
      ++name_pos;
    c_function_t *parenthesized_function =
        tok_ident(p->tokens[name_pos]) ? find_function(p, p->tokens[name_pos])
                                       : NULL;
    size_t close_pos = name_pos + (parenthesized_function ? 1 : 0);
    size_t wrapped = name_pos - (p->pos + 1) + 1;
    bool wraps_function = parenthesized_function != NULL;
    for (size_t i = 0; wraps_function && i < wrapped; ++i)
      wraps_function = tok_is(p, close_pos + i, ")");
    if (!wraps_function)
      break;
    function_parentheses = wrapped;
    p->pos = name_pos;
    break;
  }
  if (current(p)->kind == NY_CTOK_STRING)
    return append_global_string_reloc(p, data, offset);
  size_t address_parentheses = 0;
  size_t address_probe = p->pos;
  for (;;) {
    if (!tok_is(p, address_probe, "("))
      break;
    size_t close = address_probe + 1;
    size_t depth = 1;
    for (; close < p->token_count && depth; ++close) {
      if (tok_is(p, close, "("))
        ++depth;
      else if (tok_is(p, close, ")"))
        --depth;
    }
    if (depth)
      break;
    if (global_const_cast_type(p, address_probe, close - 1)) {
      address_probe = close;
      continue;
    }
    ++address_parentheses;
    ++address_probe;
  }
  if (address_probe != p->pos &&
      (tok_is(p, address_probe, "&") ||
       (address_probe < p->token_count && tok_ident(p->tokens[address_probe]) &&
        (find_function(p, p->tokens[address_probe]) ||
         find_global(p, p->tokens[address_probe])))))
    p->pos = address_probe;
  else
    address_parentheses = 0;
  bool address_of = accept(p, "&");
  if (address_of)
    while (accept(p, "("))
      ++address_parentheses;
  ny_ctok_t name = *current(p);
  c_global_t *object = tok_ident(name) ? find_global(p, name) : NULL;
  if (!object && tok_ident(name)) {
    c_local_t *local = find_local(p, name);
    if (local && local->static_name.len)
      object = find_global(p, local->static_name);
  }
  if (!object && tok_ident(name) && tok_same(name, data->name))
    object = data;
  c_function_t *function = tok_ident(name) ? find_function(p, name) : NULL;
  if (function)
    object = NULL;
  if (object || function) {
    bool terminal_array = object && object->is_array;
    ++p->pos;
    int64_t addend = 0;
    const c_agg_type_t *aggregate = object ? object->agg : NULL;
    int64_t stride =
        object && object->is_array
            ? object->array_stride ? (int64_t)object->array_stride
              : object->array_shape.element_size
                  ? (int64_t)object->array_shape.element_size
              : object->type == C_AGG && aggregate
                  ? (int64_t)aggregate->size
                  : c_pointer_scale_for(p->abi, object->type, aggregate)
            : 0;
    while (address_parentheses && tok_is(p, p->pos, ")") &&
           (tok_is(p, p->pos + 1, ".") || tok_is(p, p->pos + 1, "["))) {
      ++p->pos;
      --address_parentheses;
    }
    while (tok_is(p, p->pos, ".") || tok_is(p, p->pos, "[")) {
      if (!object) {
        fail(p, "static subobject relocation requires a data object");
        return false;
      }
      if (accept(p, ".")) {
        ny_ctok_t member;
        if (!parse_name(p, &member))
          return false;
        const c_agg_field_t *field = find_field(aggregate, member);
        if (!field || field->offset > INT64_MAX ||
            __builtin_add_overflow(addend, (int64_t)field->offset, &addend)) {
          char message[128];
          snprintf(message, sizeof(message),
                   "invalid static aggregate member address near `%.64s`",
                   member.start ? member.start : "");
          fail(p, message);
          return false;
        }
        aggregate = field->agg;
        terminal_array = field->kind == C_AGG_FIELD_ARRAY;
        stride =
            field->kind == C_AGG_FIELD_ARRAY ? (int64_t)field->elem_size : 0;
      } else {
        ++p->pos;
        int64_t index, displacement;
        if (!stride || !parse_global_const_expr(p, &p->pos, &index) ||
            !expect(p, "]") ||
            __builtin_mul_overflow(index, stride, &displacement) ||
            __builtin_add_overflow(addend, displacement, &addend)) {
          fail(p, "invalid static array element address");
          return false;
        }
        stride = 0;
        terminal_array = false;
      }
      while (address_parentheses && tok_is(p, p->pos, ")") &&
             (tok_is(p, p->pos + 1, ".") || tok_is(p, p->pos + 1, "["))) {
        ++p->pos;
        --address_parentheses;
      }
    }
    if (object && !address_of && !terminal_array) {
      fail(p, "static pointer initializer cannot read another pointer object");
      return false;
    }
    while (address_parentheses) {
      if (!expect(p, ")"))
        return false;
      --address_parentheses;
    }
    ny_ctok_t target = object ? object->name
                       : function && function->asm_alias.len
                           ? function->asm_alias
                           : name;
    if (!append_global_reloc(p, data, offset, target))
      return false;
    data->relocs[data->reloc_count - 1].addend = addend;
    if (accept(p, "+") || accept(p, "-")) {
      bool negate = tok_is(p, p->pos - 1, "-");
      int64_t displacement = 0;
      if (!parse_global_const_expr(p, &p->pos, &displacement) ||
          (stride &&
           __builtin_mul_overflow(displacement, stride, &displacement)) ||
          (negate &&
           __builtin_sub_overflow((int64_t)0, displacement, &displacement)) ||
          __builtin_add_overflow(data->relocs[data->reloc_count - 1].addend,
                                 displacement,
                                 &data->relocs[data->reloc_count - 1].addend)) {
        fail(p, "invalid static pointer initializer addend");
        return false;
      }
    }
    while (function_parentheses) {
      if (!expect(p, ")"))
        return false;
      --function_parentheses;
    }
    return true;
  }
  if (address_of) {
    size_t fallback_pos = initializer_start;
    int64_t integer_address = 0;
    if (parse_global_const_expr(p, &fallback_pos, &integer_address) &&
        (tok_is(p, fallback_pos, ",") || tok_is(p, fallback_pos, "}") ||
         tok_is(p, fallback_pos, ";"))) {
      memcpy(bytes + offset, &integer_address, sizeof(integer_address));
      p->pos = fallback_pos;
      return true;
    }
    fail(p, "static address initializer requires a declared symbol");
    return false;
  }
  int64_t value;
  if (!parse_global_const_expr(p, &p->pos, &value)) {
    char message[160];
    ny_ctok_t token = current(p) ? *current(p) : (ny_ctok_t){0};
    snprintf(message, sizeof(message),
             "static pointer initializer requires a constant address near "
             "`%.48s`",
             token.start ? token.start : "<end>");
    fail(p, message);
    return false;
  }
  memcpy(bytes + offset, &value, sizeof(value));
  return true;
}

static bool initialize_data_integer_address(c_parser_t *p, c_global_t *data,
                                            size_t offset, size_t width,
                                            unsigned char *bytes,
                                            bool *handled) {
  *handled = false;
  /*
   * A constant conditional initializer may select between a plain value and
   * an address expression. Select the taken branch before looking for a
   * leading cast so `&"literal"` can be relocated.
   */
  size_t conditional_end = SIZE_MAX;
  size_t conditional_start = p->pos;
  if (tok_is(p, p->pos, "(")) {
    size_t close = skip_balanced(p, p->pos, "(", ")");
    size_t question = SIZE_MAX;
    size_t colon = SIZE_MAX;
    int grouping = 0;
    int nested_questions = 0;
    for (size_t at = p->pos + 1; at < close && close <= p->token_count; ++at) {
      if (tok_is(p, at, "(") || tok_is(p, at, "[") || tok_is(p, at, "{")) {
        ++grouping;
      } else if (tok_is(p, at, ")") || tok_is(p, at, "]") ||
                 tok_is(p, at, "}")) {
        if (!grouping)
          break;
        --grouping;
      } else if (!grouping && tok_is(p, at, "?")) {
        if (question == SIZE_MAX)
          question = at;
        else
          ++nested_questions;
      } else if (!grouping && tok_is(p, at, ":")) {
        if (nested_questions) {
          --nested_questions;
        } else if (question != SIZE_MAX && colon == SIZE_MAX) {
          colon = at;
        }
      }
    }
    if (question != SIZE_MAX && colon != SIZE_MAX) {
      c_parser_t probe = *p;
      char probe_error[1] = {0};
      probe.err = probe_error;
      probe.err_len = sizeof(probe_error);
      probe.token_count = question;
      int64_t condition = 0;
      size_t condition_at = p->pos + 1;
      if (parse_global_const_expr(&probe, &condition_at, &condition) &&
          condition_at == question) {
        size_t branch = condition ? question + 1 : colon + 1;
        size_t value_pos = branch;
        while (tok_is(p, value_pos, "(")) {
          size_t value_close = skip_balanced(p, value_pos, "(", ")");
          if (value_close <= value_pos || value_close > p->token_count)
            break;
          value_pos = value_close;
        }
        bool address_branch =
            tok_is(p, value_pos, "&") ||
            (tok_ident(p->tokens[value_pos]) &&
             (find_function(p, p->tokens[value_pos]) ||
              (find_global(p, p->tokens[value_pos]) &&
               find_global(p, p->tokens[value_pos])->is_array)));
        if (address_branch) {
          p->pos = branch;
          conditional_end = close;
        } else {
          p->pos = conditional_start;
        }
      }
    }
  }
  size_t address_pos = p->pos;
  size_t grouping_parentheses = 0;
  while (tok_is(p, address_pos, "(")) {
    size_t close = address_pos + 1;
    size_t depth = 1;
    for (; close < p->token_count && depth; ++close) {
      if (tok_is(p, close, "("))
        ++depth;
      else if (tok_is(p, close, ")") && --depth == 0)
        break;
    }
    if (depth)
      break;
    if (global_const_cast_type(p, address_pos, close)) {
      address_pos = close + 1;
    } else {
      ++grouping_parentheses;
      ++address_pos;
    }
  }
  bool function_designator = tok_ident(p->tokens[address_pos]) &&
                             find_function(p, p->tokens[address_pos]) != NULL;
  c_global_t *array_designator = tok_ident(p->tokens[address_pos])
                                     ? find_global(p, p->tokens[address_pos])
                                     : NULL;
  if (!tok_is(p, address_pos, "&") && !function_designator &&
      !(array_designator && array_designator->is_array))
    return true;
  *handled = true;
  if (width != sizeof(uint64_t)) {
    fail(p, "integer symbol-address initializer requires pointer-width "
            "storage");
    return false;
  }
  /*
   * `(integer)&"literal"` stores the address of the literal's own symbol,
   * exactly like a pointer field does.
   */
  if (tok_is(p, address_pos, "&") && address_pos + 1 < p->token_count &&
      p->tokens[address_pos + 1].kind == NY_CTOK_STRING) {
    p->pos = address_pos + 1;
    if (!append_global_string_reloc(p, data, offset))
      return false;
    if (conditional_end != SIZE_MAX)
      p->pos = conditional_end;
    return true;
  }
  p->pos = address_pos;
  size_t reloc_before = data->reloc_count;
  if (!initialize_data_pointer(p, data, offset, bytes))
    return false;
  if (data->reloc_count == reloc_before)
    return true;
  if (accept(p, "+") || accept(p, "-")) {
    bool negate = tok_is(p, p->pos - 1, "-");
    int64_t addend;
    if (!parse_global_const_expr(p, &p->pos, &addend) ||
        (negate && __builtin_sub_overflow((int64_t)0, addend, &addend)) ||
        __builtin_add_overflow(data->relocs[data->reloc_count - 1].addend,
                               addend,
                               &data->relocs[data->reloc_count - 1].addend)) {
      fail(p, "invalid integer symbol-address addend");
      return false;
    }
  }
  while (grouping_parentheses)
    if (!expect(p, ")"))
      return false;
    else
      --grouping_parentheses;
  if (conditional_end != SIZE_MAX)
    p->pos = conditional_end;
  return true;
}

static bool try_collect_external_function_pointer_array(c_parser_t *p,
                                                        bool *matched) {
  *matched = false;
  c_parser_t probe = *p;
  char probe_error[1] = {0};
  probe.err = probe_error;
  probe.err_len = sizeof(probe_error);
  probe.type_lookahead = true;
  c_type_t type;
  if (!parse_type(&probe, &type, false) || type != C_FUNC_PTR ||
      !tok_ident(*current(&probe)) || !tok_is(&probe, probe.pos + 1, "["))
    return true;
  bool is_external = false;
  for (size_t i = p->pos; i < probe.pos; ++i)
    is_external |= tok_is(p, i, "extern");
  if (!is_external)
    return true;

  *matched = true;
  if (!parse_type(p, &type, false))
    return false;
  bool is_const = p->last_type_const;
  size_t pointer_size = c_type_size(p->abi, C_VOID_PTR);
  for (;;) {
    ny_ctok_t name;
    if (!parse_name(p, &name) || !expect(p, "["))
      return false;
    uint64_t extent = 0;
    bool incomplete = accept(p, "]");
    if (!incomplete) {
      if (!parse_array_extent_expr(p, 1, &extent) ||
          extent > SIZE_MAX / pointer_size || !expect(p, "]")) {
        fail(p, "external function-pointer array requires a valid extent");
        return false;
      }
    }
    if (accept(p, "=")) {
      fail(p, "external function-pointer arrays cannot have an initializer");
      return false;
    }
    if (find_global(p, name) || p->global_count >= p->global_capacity) {
      fail(p, "duplicate or excessive external function-pointer array");
      return false;
    }
    p->globals[p->global_count++] =
        (c_global_t){.name = name,
                     .len = (size_t)extent * pointer_size,
                     .type = C_FUNC_PTR,
                     .is_array = true,
                     .array_incomplete = incomplete,
                     .array_rank = 1,
                     .array_shape = {.rank = 1,
                                     .extent = {(size_t)extent},
                                     .element_type = C_FUNC_PTR,
                                     .element_size = pointer_size},
                     .is_pointer_array = true,
                     .is_function_pointer_array = true,
                     .is_const = is_const,
                     .is_external = true};
    if (!accept(p, ","))
      break;
  }
  return expect(p, ";");
}

static bool global_null_pointer_initializer(const c_parser_t *p, size_t *at) {
  size_t pos = *at;
  size_t opens = 0;
  while (tok_is(p, pos, "(")) {
    ++opens;
    ++pos;
  }
  if (!opens || !tok_is(p, pos, "void") || !tok_is(p, pos + 1, "*") ||
      !tok_is(p, pos + 2, ")"))
    return false;
  pos += 3;
  --opens;
  if (!tok_is(p, pos++, "0"))
    return false;
  while (opens) {
    if (!tok_is(p, pos++, ")"))
      return false;
    --opens;
  }
  *at = pos;
  return true;
}

static bool try_collect_function_pointer_array(c_parser_t *p, bool *matched) {
  *matched = false;
  if (!try_collect_external_function_pointer_array(p, matched))
    return false;
  if (*matched)
    return true;
  c_parser_t alias_probe = *p;
  char alias_error[1] = {0};
  alias_probe.err = alias_error;
  alias_probe.err_len = sizeof(alias_error);
  alias_probe.type_lookahead = true;
  c_type_t alias_element_type;
  c_array_declarator_t alias_declarator = {0};
  bool alias_type_decl = parse_type(&alias_probe, &alias_element_type, true);
  c_array_shape_t alias_element_shape = alias_probe.last_type_is_array
                                            ? alias_probe.last_type_array
                                            : (c_array_shape_t){0};
  size_t alias_element_size = alias_probe.last_type_array_size;
  bool alias_decl =
      alias_type_decl && c_type_is_pointer(alias_element_type) &&
      parse_array_declarator(&alias_probe, &alias_declarator, 0) &&
      alias_declarator.array;
  bool alias_is_const = alias_probe.last_type_const;
  size_t alias_initializer_pos = alias_probe.pos;
  bool alias_symbol_array =
      alias_decl &&
      skip_file_scope_attributes(&alias_probe, &alias_initializer_pos) &&
      tok_is(&alias_probe, alias_initializer_pos, "=") &&
      tok_is(&alias_probe, alias_initializer_pos + 1, "{");
  size_t alias_item_pos = alias_initializer_pos + 2;
  while (alias_symbol_array && !tok_is(&alias_probe, alias_item_pos, "}")) {
    if (tok_is(&alias_probe, alias_item_pos, "[")) {
      size_t close = skip_balanced(&alias_probe, alias_item_pos, "[", "]");
      if (close >= alias_probe.token_count ||
          !tok_is(&alias_probe, close, "=")) {
        alias_symbol_array = false;
        break;
      }
      alias_item_pos = close + 1;
    }
    ny_ctok_t item = alias_probe.tokens[alias_item_pos];
    if (item.kind != NY_CTOK_IDENT &&
        !tok_is(&alias_probe, alias_item_pos, "0")) {
      alias_symbol_array = false;
      break;
    }
    ++alias_item_pos;
    if (tok_is(&alias_probe, alias_item_pos, ",")) {
      ++alias_item_pos;
    } else if (!tok_is(&alias_probe, alias_item_pos, "}")) {
      alias_symbol_array = false;
    }
  }
  if (alias_symbol_array) {
    *matched = true;
    ny_ctok_t name = alias_declarator.name;
    size_t pointer_size = c_type_size(p->abi, C_VOID_PTR);
    alias_probe.pos = alias_initializer_pos + 2;
    c_global_t global = {.name = name,
                         .type = alias_element_type,
                         .agg = alias_probe.last_type_agg,
                         .is_array = true,
                         .is_pointer_array = true,
                         .is_function_pointer_array =
                             alias_element_type == C_FUNC_PTR,
                         .is_const = alias_is_const};
    size_t count = 0;
    size_t next_index = 0;
    while (!tok_is(&alias_probe, alias_probe.pos, "}")) {
      size_t index = next_index;
      if (tok_is(&alias_probe, alias_probe.pos, "[")) {
        ++alias_probe.pos;
        int64_t designated_index = -1;
        if (!parse_global_const_expr(&alias_probe, &alias_probe.pos,
                                     &designated_index) ||
            designated_index < 0 ||
            (uint64_t)designated_index >= SIZE_MAX / pointer_size ||
            !expect(&alias_probe, "]") || !expect(&alias_probe, "=")) {
          free(global.relocs);
          fail(p, "invalid pointer-array initializer designator");
          return false;
        }
        index = (size_t)designated_index;
      }
      ny_ctok_t target = alias_probe.tokens[alias_probe.pos];
      if (tok_is(&alias_probe, alias_probe.pos, "0")) {
        ++alias_probe.pos;
      } else if (target.kind == NY_CTOK_IDENT) {
        if (!append_global_reloc(&alias_probe, &global, index * pointer_size,
                                 target)) {
          free(global.relocs);
          return false;
        }
        ++alias_probe.pos;
      } else {
        free(global.relocs);
        fail(p, "pointer arrays require symbol addresses or nulls");
        return false;
      }
      if (index >= count)
        count = index + 1;
      next_index = index + 1;
      if (tok_is(&alias_probe, alias_probe.pos, ",")) {
        ++alias_probe.pos;
      } else if (!tok_is(&alias_probe, alias_probe.pos, "}")) {
        free(global.relocs);
        fail(p, "expected ',' between function-pointer array elements");
        return false;
      }
    }
    ++alias_probe.pos;
    bool empty_initializer = count == 0;
    size_t array_count =
        alias_declarator.incomplete ? count : (size_t)alias_declarator.extent;
    if ((!array_count && !empty_initializer) || count > array_count ||
        !tok_is(&alias_probe, alias_probe.pos++, ";") ||
        array_count > SIZE_MAX / pointer_size) {
      free(global.relocs);
      fail(p, "invalid typedef-based pointer array");
      return false;
    }
    size_t element_size =
        alias_element_size ? alias_element_size : pointer_size;
    if (alias_element_shape.rank >= C_MAX_ARRAY_RANK ||
        array_count > SIZE_MAX / element_size) {
      free(global.relocs);
      fail(p, "typedef-based pointer array size overflows");
      return false;
    }
    global.len = array_count * element_size;
    global.array_rank = alias_element_shape.rank + 1;
    global.array_shape = (c_array_shape_t){.rank = global.array_rank,
                                           .extent = {array_count},
                                           .element_type = alias_element_type,
                                           .element_size = pointer_size,
                                           .agg = global.agg};
    for (unsigned dimension = 0; dimension < alias_element_shape.rank;
         ++dimension)
      global.array_shape.extent[dimension + 1] =
          alias_element_shape.extent[dimension];
    global.bytes = calloc(global.len ? global.len : 1, 1);
    if (!global.bytes) {
      free(global.relocs);
      fail(p, "out of memory allocating function-pointer array");
      return false;
    }
    global.reloc_capacity = global.reloc_count;
    c_global_t *existing = find_global(p, name);
    if (existing) {
      bool shape_conflict = false;
      if (!existing->array_incomplete) {
        shape_conflict = existing->array_rank != global.array_rank ||
                         existing->array_shape.element_size !=
                             global.array_shape.element_size;
      }
      if (!existing->array_incomplete) {
        for (size_t dimension = 0;
             !shape_conflict && dimension < global.array_rank; ++dimension) {
          shape_conflict = existing->array_shape.extent[dimension] !=
                           global.array_shape.extent[dimension];
        }
      }
      if ((!existing->is_external && !existing->is_tentative) ||
          !existing->is_array || !existing->is_pointer_array ||
          (!existing->array_incomplete && existing->len &&
           existing->len != global.len) ||
          shape_conflict || existing->type != alias_element_type ||
          existing->is_const != alias_is_const) {
        free(global.bytes);
        free(global.relocs);
        fail(p, "conflicting typedef-based pointer array definition");
        return false;
      }
      free(existing->bytes);
      free(existing->relocs);
      existing->bytes = global.bytes;
      existing->len = global.len;
      existing->relocs = global.relocs;
      existing->reloc_count = global.reloc_count;
      existing->reloc_capacity = global.reloc_capacity;
      existing->array_rank = global.array_rank;
      existing->array_shape = global.array_shape;
      existing->is_external = false;
      existing->array_incomplete = false;
      existing->is_tentative = false;
      existing->is_function_pointer_array = global.is_function_pointer_array;
    } else {
      if (p->global_count >= p->global_capacity) {
        free(global.bytes);
        free(global.relocs);
        fail(p, "invalid typedef-based pointer array");
        return false;
      }
      p->globals[p->global_count++] = global;
    }
    p->pos = alias_probe.pos;
    return true;
  }
  /*
   * General file-scope pointer arrays may contain addresses of data objects
   * and aggregate members, not only function or string symbols.
   */
  c_parser_t data_array_probe = *p;
  size_t declaration_start = p->pos;
  char data_array_error[1] = {0};
  data_array_probe.err = data_array_error;
  data_array_probe.err_len = sizeof(data_array_error);
  data_array_probe.type_lookahead = true;
  c_type_t data_array_type;
  c_array_declarator_t data_array_decl = {0};
  if (parse_type(&data_array_probe, &data_array_type, true) &&
      c_type_is_pointer(data_array_type) &&
      parse_array_declarator(&data_array_probe, &data_array_decl, 0) &&
      data_array_decl.array) {
    size_t initializer_pos = data_array_probe.pos;
    if (skip_file_scope_attributes(&data_array_probe, &initializer_pos) &&
        tok_is(&data_array_probe, initializer_pos, "=") &&
        tok_is(&data_array_probe, initializer_pos + 1, "{")) {
      *matched = true;
      c_type_t parsed_type;
      if (!parse_type(p, &parsed_type, true) || !c_type_is_pointer(parsed_type))
        return false;
      bool pointer_const = p->last_type_const_pointer;
      c_agg_type_t *pointer_agg = p->last_type_agg;
      c_array_declarator_t declarator = {0};
      if (!parse_array_declarator(p, &declarator, 0))
        return false;
      ny_ctok_t section = p->last_type_section;
      if (!parse_trailing_file_scope_attributes(p, &section) ||
          !expect(p, "=") || !expect(p, "{"))
        return false;
      c_global_t global = {.name = declarator.name,
                           .type = parsed_type,
                           .agg = pointer_agg,
                           .is_array = true,
                           .is_pointer_array = true,
                           .is_function_pointer_array =
                               parsed_type == C_FUNC_PTR,
                           .is_const = pointer_const};
      size_t pointer_size = c_type_size(p->abi, C_VOID_PTR);
      size_t required_count = 0;
      size_t next_index = 0;
      while (!tok_is(p, p->pos, "}")) {
        size_t index = next_index;
        if (accept(p, "[")) {
          int64_t designated_index = -1;
          if (!parse_global_const_expr(p, &p->pos, &designated_index) ||
              designated_index < 0 ||
              (uint64_t)designated_index >= SIZE_MAX / pointer_size ||
              !expect(p, "]") || !expect(p, "=")) {
            free(global.bytes);
            free(global.relocs);
            fail(p, "invalid file-scope pointer-array designator");
            return false;
          }
          index = (size_t)designated_index;
        }
        if ((!declarator.incomplete && index >= declarator.extent) ||
            index >= SIZE_MAX / pointer_size) {
          free(global.bytes);
          free(global.relocs);
          fail(p, "file-scope pointer-array element is out of range");
          return false;
        }
        size_t required_bytes = (index + 1) * pointer_size;
        if (required_bytes > global.len) {
          unsigned char *grown = realloc(global.bytes, required_bytes);
          if (!grown) {
            free(global.bytes);
            free(global.relocs);
            fail(p, "out of memory allocating file-scope pointer array");
            return false;
          }
          global.bytes = grown;
          memset(global.bytes + global.len, 0, required_bytes - global.len);
          global.len = required_bytes;
        }
        if (!initialize_data_pointer(p, &global, index * pointer_size,
                                     global.bytes)) {
          free(global.bytes);
          free(global.relocs);
          return false;
        }
        if (index + 1 > required_count)
          required_count = index + 1;
        next_index = index + 1;
        if (accept(p, ",")) {
          if (tok_is(p, p->pos, "}"))
            break;
        } else if (!tok_is(p, p->pos, "}")) {
          free(global.bytes);
          free(global.relocs);
          fail(p, "expected ',' between file-scope pointer-array elements");
          return false;
        }
      }
      if (!expect(p, "}")) {
        free(global.bytes);
        free(global.relocs);
        return false;
      }
      size_t count =
          declarator.incomplete ? required_count : (size_t)declarator.extent;
      if (count > SIZE_MAX / pointer_size || required_count > count ||
          !expect(p, ";")) {
        free(global.bytes);
        free(global.relocs);
        fail(p, "invalid file-scope pointer-array extent");
        return false;
      }
      size_t bytes_len = count * pointer_size;
      if (global.len < bytes_len) {
        unsigned char *final_bytes = realloc(global.bytes, bytes_len);
        if (!final_bytes) {
          free(global.bytes);
          free(global.relocs);
          fail(p, "out of memory finalizing file-scope pointer array");
          return false;
        }
        memset(final_bytes + global.len, 0, bytes_len - global.len);
        global.bytes = final_bytes;
        global.len = bytes_len;
      }
      global.array_rank = 1;
      global.array_shape = (c_array_shape_t){.rank = 1,
                                             .extent = {count},
                                             .element_type = parsed_type,
                                             .element_size = pointer_size,
                                             .agg = pointer_agg};
      if (section.len) {
        char section_name[64] = {0};
        if (!copy_quoted_token(p, section, section_name,
                               sizeof(section_name))) {
          free(global.bytes);
          free(global.relocs);
          return false;
        }
        global.section = strdup(section_name);
        if (!global.section) {
          free(global.bytes);
          free(global.relocs);
          fail(p, "out of memory storing pointer-array section");
          return false;
        }
      }
      c_global_t *existing = find_global(p, global.name);
      if (existing) {
        if ((!existing->is_external && !existing->is_tentative) ||
            !existing->is_pointer_array || existing->type != global.type ||
            (!existing->array_incomplete && existing->len != global.len)) {
          free(global.bytes);
          free(global.relocs);
          free(global.section);
          fail(p, "conflicting file-scope pointer-array definition");
          return false;
        }
        free(existing->bytes);
        free(existing->relocs);
        free(existing->section);
        existing->bytes = global.bytes;
        existing->len = global.len;
        existing->relocs = global.relocs;
        existing->reloc_count = global.reloc_count;
        existing->reloc_capacity = global.reloc_capacity;
        existing->section = global.section;
        existing->array_rank = global.array_rank;
        existing->array_shape = global.array_shape;
        existing->array_incomplete = false;
        existing->is_external = false;
        existing->is_tentative = false;
      } else {
        if (p->global_count >= p->global_capacity) {
          free(global.bytes);
          free(global.relocs);
          free(global.section);
          fail(p, "C frontend global data table capacity exceeded");
          return false;
        }
        p->globals[p->global_count++] = global;
      }
      return true;
    } else if (skip_file_scope_attributes(&data_array_probe,
                                          &initializer_pos) &&
               tok_is(&data_array_probe, initializer_pos, ";") &&
               !data_array_probe.last_type_array_size &&
               !data_array_decl.incomplete && data_array_decl.array_rank == 1) {
      *matched = true;
      c_type_t parsed_type;
      if (!parse_type(p, &parsed_type, true) || !c_type_is_pointer(parsed_type))
        return false;
      bool pointer_const = p->last_type_const_pointer;
      c_agg_type_t *pointer_agg = p->last_type_agg;
      c_array_declarator_t declarator = {0};
      if (!parse_array_declarator(p, &declarator, 0))
        return false;
      ny_ctok_t section = p->last_type_section;
      if (!parse_trailing_file_scope_attributes(p, &section) || !expect(p, ";"))
        return false;
      bool is_external = false;
      for (size_t i = declaration_start; i < data_array_probe.pos; ++i)
        is_external |= tok_is(p, i, "extern");
      size_t pointer_size = c_type_size(p->abi, C_VOID_PTR);
      if (declarator.extent > SIZE_MAX / pointer_size) {
        fail(p, "file-scope pointer-array extent overflows");
        return false;
      }
      size_t len = (size_t)declarator.extent * pointer_size;
      unsigned char *bytes = is_external ? NULL : calloc(len ? len : 1, 1);
      if (!is_external && !bytes) {
        fail(p, "out of memory allocating tentative pointer array");
        return false;
      }
      c_global_t global = {
          .name = declarator.name,
          .bytes = bytes,
          .len = len,
          .type = parsed_type,
          .agg = pointer_agg,
          .is_array = true,
          .array_rank = 1,
          .array_shape = {.rank = 1,
                          .extent = {(size_t)declarator.extent},
                          .element_type = parsed_type,
                          .element_size = pointer_size,
                          .agg = pointer_agg},
          .is_pointer_array = true,
          .is_function_pointer_array = parsed_type == C_FUNC_PTR,
          .is_const = pointer_const,
          .is_external = is_external,
          .is_tentative = !is_external};
      if (section.len) {
        char section_name[64] = {0};
        if (!copy_quoted_token(p, section, section_name,
                               sizeof(section_name))) {
          free(global.bytes);
          return false;
        }
        global.section = strdup(section_name);
        if (!global.section) {
          free(global.bytes);
          fail(p, "out of memory storing pointer-array section");
          return false;
        }
      }
      c_global_t *existing = find_global(p, global.name);
      if (existing) {
        if (existing->type != global.type || !existing->is_array ||
            !existing->is_pointer_array ||
            (!existing->array_incomplete && existing->len != global.len)) {
          free(global.bytes);
          free(global.section);
          fail(p, "conflicting file-scope pointer-array declaration");
          return false;
        }
        if (existing->is_external || existing->is_tentative) {
          bool completes_array = existing->array_incomplete;
          if (!is_external && (existing->is_external || completes_array)) {
            free(existing->bytes);
            existing->bytes = global.bytes;
            global.bytes = NULL;
            existing->is_external = false;
            existing->is_tentative = true;
          } else if (completes_array && existing->is_tentative &&
                     existing->len != global.len) {
            unsigned char *resized = calloc(global.len ? global.len : 1, 1);
            if (!resized) {
              free(global.section);
              fail(p, "out of memory completing tentative pointer array");
              return false;
            }
            free(existing->bytes);
            existing->bytes = resized;
          }
          if (completes_array) {
            existing->len = global.len;
            existing->array_rank = global.array_rank;
            existing->array_shape = global.array_shape;
            existing->array_incomplete = false;
          }
          if (section.len) {
            free(existing->section);
            existing->section = global.section;
            global.section = NULL;
          }
        }
        free(global.bytes);
        free(global.section);
      } else {
        if (p->global_count >= p->global_capacity) {
          free(global.bytes);
          free(global.section);
          fail(p, "C frontend global data table capacity exceeded");
          return false;
        }
        p->globals[p->global_count++] = global;
      }
      return true;
    }
  }
  size_t type_pos = p->pos;
  while (tok_is(p, type_pos, "const") || tok_is(p, type_pos, "volatile") ||
         tok_is(p, type_pos, "static") || tok_is(p, type_pos, "extern") ||
         tok_is(p, type_pos, "register") || tok_is(p, type_pos, "inline"))
    ++type_pos;
  if (tok_is(p, type_pos, "struct") || tok_is(p, type_pos, "union"))
    return true;
  c_parser_t probe = *p;
  char probe_error[1] = {0};
  probe.err = probe_error;
  probe.err_len = sizeof(probe_error);
  probe.type_lookahead = true;
  c_type_t result_type;
  if (!parse_type(&probe, &result_type, true) ||
      !tok_is(&probe, probe.pos, "(") || !tok_is(&probe, probe.pos + 1, "*"))
    return true;
  size_t pointer_qualifiers = probe.pos + 2;
  size_t name_pos = pointer_qualifiers;
  while (tok_is(&probe, name_pos, "const") ||
         tok_is(&probe, name_pos, "volatile"))
    ++name_pos;
  if (!tok_ident(probe.tokens[name_pos]) || !tok_is(&probe, name_pos + 1, "["))
    return true;
  size_t array_close = name_pos + 2;
  while (array_close < probe.token_count && !tok_is(&probe, array_close, "]"))
    ++array_close;
  if (array_close >= probe.token_count ||
      !tok_is(&probe, array_close + 1, ")") ||
      !tok_is(&probe, array_close + 2, "("))
    return true;
  size_t params_end = skip_balanced(&probe, array_close + 2, "(", ")");
  if (params_end >= probe.token_count)
    return true;
  probe.pos = params_end;
  ny_ctok_t section = probe.last_type_section;
  if (!parse_trailing_file_scope_attributes(&probe, &section) ||
      !tok_is(&probe, probe.pos, "=") || !tok_is(&probe, probe.pos + 1, "{"))
    return true;

  *matched = true;
  ny_ctok_t name = probe.tokens[name_pos];
  size_t at = probe.pos + 2;
  bool is_static = false, is_const = false;
  for (size_t token = p->pos; token < type_pos; ++token)
    is_static |= tok_is(p, token, "static");
  for (size_t token = pointer_qualifiers; token < name_pos; ++token)
    is_const |= tok_is(p, token, "const");
  unsigned char *bytes = NULL;
  c_global_t global = {.name = name,
                       .type = C_FUNC_PTR,
                       .is_array = true,
                       .is_pointer_array = true,
                       .is_function_pointer_array = true,
                       .is_local = is_static,
                       .is_const = is_const};
  size_t count = 0;
  size_t next_index = 0;
  while (!tok_is(p, at, "}")) {
    size_t index = next_index;
    if (tok_is(p, at, "[")) {
      ++at;
      int64_t designated_index = -1;
      if (!parse_global_const_expr(p, &at, &designated_index) ||
          designated_index < 0 ||
          (uint64_t)designated_index >= SIZE_MAX / sizeof(uint64_t) ||
          !tok_is(p, at, "]")) {
        free(global.relocs);
        fail(p, "invalid function-pointer array designator");
        return false;
      }
      index = (size_t)designated_index;
      ++at;
      if (!tok_is(p, at++, "=")) {
        free(global.relocs);
        fail(p, "function-pointer array designator requires '='");
        return false;
      }
    }
    size_t element_offset = index * sizeof(uint64_t);
    ny_ctok_t target = p->tokens[at];
    if (tok_is(p, at, "0")) {
      ++at;
    } else if (global_null_pointer_initializer(p, &at)) {
      /*
       * A file-scope `(void *)0` initializer has no symbol relocation.
       */
    } else if (target.kind == NY_CTOK_IDENT) {
      if (!append_global_reloc(p, &global, element_offset, target)) {
        free(global.relocs);
        return false;
      }
      ++at;
    } else {
      free(global.relocs);
      fail(p, "function-pointer arrays require function symbols or nulls");
      return false;
    }
    if (index == SIZE_MAX / sizeof(uint64_t)) {
      free(global.relocs);
      fail(p, "file-scope function-pointer array size overflow");
      return false;
    }
    if (index >= count)
      count = index + 1;
    next_index = index + 1;
    if (tok_is(p, at, ",")) {
      ++at;
      while (tok_is(p, at, "\\"))
        ++at;
    } else if (!tok_is(p, at, "}")) {
      free(global.relocs);
      fail(p, "expected ',' between function-pointer array elements");
      return false;
    }
  }
  ++at;
  if (!count || !tok_is(p, at++, ";")) {
    free(global.relocs);
    fail(p, "invalid file-scope function-pointer array terminator");
    return false;
  }
  c_global_t *existing = find_global(p, name);
  if ((existing &&
       (!existing->array_incomplete || !existing->is_array ||
        !existing->is_pointer_array || !existing->is_function_pointer_array ||
        existing->type != C_FUNC_PTR)) ||
      (!existing && p->global_count >= p->global_capacity) ||
      count > SIZE_MAX / sizeof(uint64_t)) {
    free(global.relocs);
    fail(p, "invalid file-scope function-pointer array");
    return false;
  }
  global.len = count * sizeof(uint64_t);
  global.array_rank = 1;
  global.array_shape = (c_array_shape_t){.rank = 1,
                                         .extent = {count},
                                         .element_size = sizeof(uint64_t),
                                         .element_type = C_FUNC_PTR};
  bytes = calloc(global.len, 1);
  if (!bytes) {
    free(global.relocs);
    fail(p, "out of memory allocating function-pointer array");
    return false;
  }
  global.bytes = bytes;
  global.reloc_capacity = global.reloc_count;
  if (section.len) {
    char section_name[128];
    if (!copy_quoted_token(p, section, section_name, sizeof(section_name)) ||
        !(global.section = strdup(section_name))) {
      free(global.bytes);
      free(global.relocs);
      if (!p->err || !p->err_len || !p->err[0])
        fail(p, "out of memory storing function-pointer array section");
      return false;
    }
  }
  if (existing) {
    global.is_const = existing->is_const;
    global.function_result_type = existing->function_result_type;
    if (!global.section)
      global.section = existing->section;
    else
      free(existing->section);
    free(existing->bytes);
    free(existing->relocs);
    *existing = global;
  } else {
    p->globals[p->global_count++] = global;
  }
  p->pos = at;
  return true;
}

static bool append_global_string_reloc(c_parser_t *p, c_global_t *global,
                                       size_t offset) {
  unsigned char *string = NULL;
  size_t string_len = 0;
  while (p->pos < p->token_count && p->tokens[p->pos].kind == NY_CTOK_STRING) {
    unsigned char *part = NULL;
    size_t part_len = 0;
    if (!decode_string_token(p->tokens[p->pos++], &part, &part_len)) {
      free(string);
      fail(p, "invalid file-scope aggregate string initializer");
      return false;
    }
    size_t old_content = string_len ? string_len - 1 : 0;
    size_t part_content = part_len - 1;
    if (part_content > SIZE_MAX - old_content - 1) {
      free(part);
      free(string);
      fail(p, "file-scope aggregate string initializer is too large");
      return false;
    }
    unsigned char *joined = realloc(string, old_content + part_content + 1);
    if (!joined) {
      free(part);
      free(string);
      fail(p, "out of memory concatenating aggregate string initializer");
      return false;
    }
    string = joined;
    memcpy(string + old_content, part, part_content + 1);
    string_len = old_content + part_content + 1;
    free(part);
  }
  if (!string || p->global_count >= p->global_capacity) {
    free(string);
    fail(p, "C frontend global data table capacity exceeded");
    return false;
  }
  char generated_name[48];
  int written = snprintf(generated_name, sizeof(generated_name),
                         "__nyc_string_%zu", p->global_count);
  if (written < 0 || (size_t)written >= sizeof(generated_name)) {
    free(string);
    fail(p, "generated C string symbol exceeds frontend limit");
    return false;
  }
  char *owned_name = malloc((size_t)written + 1);
  if (!owned_name) {
    free(string);
    fail(p, "out of memory naming C string literal");
    return false;
  }
  memcpy(owned_name, generated_name, (size_t)written + 1);
  ny_ctok_t target = {
      .kind = NY_CTOK_IDENT, .start = owned_name, .len = (size_t)written};
  p->globals[p->global_count++] = (c_global_t){.name = target,
                                               .bytes = string,
                                               .owned_name = owned_name,
                                               .len = string_len,
                                               .type = C_CHAR_PTR,
                                               .is_array = true,
                                               .is_const = true};
  return append_global_reloc(p, global, offset, target);
}

#include "code/lang/c/lower/parts/literal.h"

static int lower_add_const(c_parser_t *p, int base, size_t offset);
static bool lower_copy_struct(c_parser_t *p, int dst, int src, size_t size);
static bool emit(c_parser_t *p, nyr_inst_t inst, int *value);

static bool emit_label(c_parser_t *p, int64_t label) {
  unsigned flags = 0;
  for (size_t i = 0; i < p->label_count; ++i)
    if (p->labels[i].id == (size_t)label && p->labels[i].address_taken) {
      flags = NYIR_INST_F_LABEL_ADDRESS_TAKEN;
      break;
    }
  return emit(
      p,
      (nyr_inst_t){.op = NYIR_LABEL, .dst = -1, .imm = label, .flags = flags},
      NULL);
}

static bool emit_branch(c_parser_t *p, int64_t label) {
  return emit(p, (nyr_inst_t){.op = NYIR_BR, .dst = -1, .imm = label}, NULL);
}

static bool emit_branch_if(c_parser_t *p, int condition, int64_t label) {
  return emit(
      p,
      (nyr_inst_t){.op = NYIR_BR_IF, .dst = -1, .a = condition, .imm = label},
      NULL);
}

static bool lower_initializer_field(c_parser_t *p, const c_agg_field_t *field,
                                    int base, size_t offset);

static bool init_aggregate(c_parser_t *p, c_global_t *global,
                           const c_agg_type_t *agg, unsigned char *bytes,
                           size_t base, unsigned depth, int address) {
  if (!agg || !agg->complete || depth >= C_AGG_MAX_DEPTH)
    return false;
  if (global)
    bytes = global->bytes;
  bool compound_literal = false;
  bool wrapped_compound_literal = false;
  bool bare_designator = false;
  if (tok_is(p, p->pos, "{")) {
    ++p->pos;
  } else if (tok_is(p, p->pos, ".")) {
    bare_designator = true;
  } else {
    if (!accept(p, "("))
      return false;
    wrapped_compound_literal = accept(p, "(");
    c_type_t literal_type = C_BAD;
    if (!parse_type(p, &literal_type, false) || literal_type != C_AGG ||
        p->last_type_agg != agg || !expect(p, ")") || !expect(p, "{"))
      return false;
    compound_literal = true;
  }
  size_t field_index = 0;
  const c_agg_type_t *promoted_cursor_agg = NULL;
  size_t promoted_cursor_index = 0;
  while (bare_designator || !tok_is(p, p->pos, "}")) {
    const c_agg_field_t *designated_field = NULL;
    c_agg_field_t designated_array_element = {0};
    size_t designated_offset = 0;
    /*
     * C99 designated member: `.name = ...` realigns the value walk.
     */
    if (tok_is(p, p->pos, ".") && tok_ident(p->tokens[p->pos + 1])) {
      ny_ctok_t name = p->tokens[p->pos + 1];
      bool found = false;
      promoted_cursor_agg = NULL;
      promoted_cursor_index = 0;
      for (size_t i = 0; i < agg->field_count; ++i) {
        if (agg->fields[i].name.len && tok_same(agg->fields[i].name, name)) {
          field_index = i;
          designated_field = &agg->fields[i];
          found = true;
          break;
        }
      }
      if (!found) {
        designated_field = find_field(agg, name);
        if (designated_field) {
          for (size_t i = 0; i < agg->field_count; ++i) {
            const c_agg_field_t *parent = &agg->fields[i];
            if (parent->kind == C_AGG_FIELD_AGG && parent->agg &&
                designated_field->offset >= parent->offset &&
                designated_field->offset - parent->offset < parent->agg->size) {
              field_index = i;
              promoted_cursor_agg = parent->agg;
              for (size_t j = 0; j < parent->agg->promoted_field_count; ++j)
                if (&parent->agg->promoted_fields[j] == designated_field) {
                  promoted_cursor_index = j + 1;
                  break;
                }
              found = true;
              break;
            }
          }
        }
      }
      if (!found) {
        char message[128];
        snprintf(message, sizeof(message),
                 "aggregate initializer has no member named `%.*s`",
                 (int)(name.len > 60 ? 60 : name.len), name.start);
        fail(p, message);
        return false;
      }
      p->pos += 2; /* consume `.name` */
      designated_offset = designated_field->offset;
      for (;;) {
        if (accept(p, ".")) {
          ny_ctok_t nested_name;
          if (designated_field->kind != C_AGG_FIELD_AGG ||
              !designated_field->agg || !parse_name(p, &nested_name)) {
            fail(p, "nested aggregate designator requires an aggregate member");
            return false;
          }
          const c_agg_field_t *nested =
              find_field(designated_field->agg, nested_name);
          if (!nested || nested->offset > SIZE_MAX - designated_offset) {
            fail(p,
                 "aggregate initializer has no nested member with that name");
            return false;
          }
          designated_offset += nested->offset;
          designated_field = nested;
          continue;
        }
        if (tok_is(p, p->pos, "[")) {
          int64_t index = -1;
          size_t index_pos = p->pos + 1;
          if (designated_field->kind != C_AGG_FIELD_ARRAY ||
              designated_field->array_rank != 1 ||
              !parse_global_const_expr(p, &index_pos, &index) || index < 0 ||
              !tok_is(p, index_pos, "]") ||
              (uint64_t)index >= designated_field->elem_count ||
              (designated_field->elem_size &&
               (uint64_t)index > (SIZE_MAX - designated_offset) /
                                     designated_field->elem_size)) {
            fail(p, "invalid aggregate-array initializer designator");
            return false;
          }
          designated_offset += (size_t)index * designated_field->elem_size;
          p->pos = index_pos + 1;
          designated_array_element = *designated_field;
          designated_array_element.offset = 0;
          designated_array_element.size = designated_field->elem_size;
          designated_array_element.is_array = false;
          designated_array_element.array_rank = 0;
          designated_array_element.elem_count = 0;
          if (designated_field->elem_pointer) {
            designated_array_element.kind = C_AGG_FIELD_PTR;
            designated_array_element.agg = designated_field->agg;
          } else if (designated_field->agg) {
            designated_array_element.kind = C_AGG_FIELD_AGG;
          } else {
            designated_array_element.kind =
                designated_field->elem_f32         ? C_AGG_FIELD_F32
                : designated_field->elem_f64       ? C_AGG_FIELD_F64
                : designated_field->elem_pointer   ? C_AGG_FIELD_PTR
                : designated_field->elem_size == 1 ? C_AGG_FIELD_U8
                : designated_field->elem_size == 2
                    ? (designated_field->elem_unsigned ? C_AGG_FIELD_U16
                                                       : C_AGG_FIELD_I16)
                : designated_field->elem_size == 4
                    ? (designated_field->elem_unsigned ? C_AGG_FIELD_U32
                                                       : C_AGG_FIELD_I32)
                    : (designated_field->elem_unsigned ? C_AGG_FIELD_U64
                                                       : C_AGG_FIELD_I64);
            designated_array_element.agg = NULL;
          }
          designated_field = &designated_array_element;
          continue;
        }
        break;
      }
      if (!expect(p, "=")) {
        fail(p, "aggregate designator requires `=`");
        return false;
      }
      if (designated_field)
        ++field_index;
    } else {
      if (promoted_cursor_agg &&
          promoted_cursor_index < promoted_cursor_agg->promoted_field_count) {
        designated_field =
            &promoted_cursor_agg->promoted_fields[promoted_cursor_index++];
        if (promoted_cursor_index == promoted_cursor_agg->promoted_field_count)
          promoted_cursor_agg = NULL;
      } else {
        promoted_cursor_agg = NULL;
        while (field_index < agg->field_count &&
               agg->fields[field_index].is_bitfield &&
               !agg->fields[field_index].name.len)
          ++field_index;
        if (field_index >= agg->field_count) {
          fail(p, "too many values in file-scope aggregate initializer");
          return false;
        }
      }
    }
    const c_agg_field_t *field =
        designated_field ? designated_field : &agg->fields[field_index++];
    size_t field_offset = designated_field ? designated_offset : field->offset;
    if (field_offset > SIZE_MAX - base) {
      fail(p, "aggregate initializer offset overflows object storage");
      return false;
    }
    size_t offset = base + field_offset;
    size_t compound_literal_closing_parens = 0;
    if (field->kind == C_AGG_FIELD_AGG && field->name.len &&
        !tok_is(p, p->pos, "{") && !tok_is(p, p->pos, "(")) {
      /*
       * Direct aggregate-typed member value (`o = { .member = value }`),
       * including C99 designators without inner braces: the value's
       * aggregate type must match the member exactly and it is copied
       * byte-exactly when a runtime address exists; the constant byte
       * path has no aggregate constant representation.
       */
      if (address < 0) {
        char message[160];
        snprintf(message, sizeof(message),
                 "file-scope aggregate member `%.*s` requires a braced "
                 "initializer",
                 (int)(field->name.len > 80 ? 80 : field->name.len),
                 field->name.start ? field->name.start : "?");
        fail(p, message);
        return false;
      }
      c_value_t value = parse_expr(p, 1);
      if (value.value < 0)
        return false;
      if (!(value.type == C_AGG && field->agg && value.agg == field->agg)) {
        fail(p, "designated aggregate initializer type must match the member");
        return false;
      }
      int target = lower_add_const(p, address, offset);
      if (target < 0 ||
          !lower_copy_struct(p, target, value.value, field->agg->size))
        return false;
    } else if (field->kind == C_AGG_FIELD_AGG) {
      if (!tok_is(p, p->pos, "{")) {
        if (address >= 0) {
          c_value_t value = parse_expr(p, 1);
          if (value.value < 0)
            return false;
          if (value.type != C_AGG || value.agg != field->agg) {
            fail(p,
                 "aggregate initializer expression type must match its field");
            return false;
          }
          int target = lower_add_const(p, address, offset);
          if (target < 0 ||
              !lower_copy_struct(p, target, value.value, field->agg->size))
            return false;
        } else {
          c_parser_t probe = *p;
          char probe_error[1] = {0};
          probe.err = probe_error;
          probe.err_len = sizeof(probe_error);
          probe.type_lookahead = true;
          size_t leading_parens = 0;
          while (accept(&probe, "("))
            ++leading_parens;
          if (!leading_parens) {
            fail(p, "aggregate constant initializer requires braces or a "
                    "matching compound literal");
            return false;
          }
          c_type_t compound_type;
          if (!parse_type(&probe, &compound_type, false) ||
              compound_type != C_AGG || probe.last_type_agg != field->agg ||
              !expect(&probe, ")") || !tok_is(&probe, probe.pos, "{")) {
            fail(p, "aggregate compound literal type does not match its field");
            return false;
          }
          compound_literal_closing_parens = leading_parens - 1;
          p->pos = probe.pos;
        }
      }
      if (tok_is(p, p->pos, "{")) {
        if (!init_aggregate(p, global, field->agg, bytes, offset, depth + 1,
                            address))
          return false;
        if (global)
          bytes = global->bytes;
      }
      while (compound_literal_closing_parens--)
        if (!expect(p, ")"))
          return false;
    } else if (field->kind == C_AGG_FIELD_ARRAY && field->array_rank > 1 &&
               field->elem_size == 1 && tok_is(p, p->pos, "{") &&
               p->pos + 1 < p->token_count &&
               p->tokens[p->pos + 1].kind == NY_CTOK_STRING) {
      if (!expect(p, "{"))
        return false;
      size_t row_size = field->elem_size;
      for (unsigned dimension = 1; dimension < field->array_rank; ++dimension) {
        if (!field->array_extent[dimension] ||
            row_size > SIZE_MAX / field->array_extent[dimension]) {
          fail(p, "invalid multidimensional character-array extent");
          return false;
        }
        row_size *= field->array_extent[dimension];
      }
      size_t row_count = field->array_extent[0];
      if (!row_count || row_size > (SIZE_MAX - offset) / row_count) {
        fail(p, "invalid multidimensional character-array size");
        return false;
      }
      size_t row = 0;
      bool first = true;
      while (!tok_is(p, p->pos, "}")) {
        if (!first && !expect(p, ","))
          return false;
        if (tok_is(p, p->pos, "}"))
          break;
        if (row >= row_count || p->tokens[p->pos].kind != NY_CTOK_STRING) {
          fail(p, "multidimensional character array requires string rows");
          return false;
        }
        size_t row_offset = offset + row * row_size;
        if (address >= 0) {
          c_value_t string = lower_string_literal(p);
          if (string.value < 0)
            return false;
          size_t string_len = p->globals[p->global_count - 1].len;
          if (!string_len || string_len - 1 > row_size) {
            fail(p, "string initializer does not fit character-array row");
            return false;
          }
          int target = lower_add_const(p, address, row_offset);
          size_t copy_len = string_len > row_size ? row_size : string_len;
          if (target < 0 ||
              !lower_copy_struct(p, target, string.value, copy_len))
            return false;
        } else {
          unsigned char *string = NULL;
          size_t string_len = 0;
          if (!decode_string_token(p->tokens[p->pos++], &string, &string_len) ||
              !string_len || string_len - 1 > row_size) {
            free(string);
            fail(p, "string initializer does not fit character-array row");
            return false;
          }
          size_t copy_len = string_len > row_size ? row_size : string_len;
          memcpy(bytes + row_offset, string, copy_len);
          free(string);
        }
        ++row;
        first = false;
      }
      (void)accept(p, ",");
      if (!expect(p, "}"))
        return false;
    } else if (field->kind == C_AGG_FIELD_ARRAY && field->agg) {
      if (!expect(p, "{")) {
        if (!p->err[0])
          fail(p, "aggregate array initializer requires braces");
        return false;
      }
      size_t element_count = field->elem_count;
      if (!element_count) {
        /*
         * GNU C permits a file-scope flexible array member to receive an
         * initializer; the object then grows to contain those elements.
         */
        if (!global || address >= 0 || !field->elem_size) {
          fail(p, "flexible aggregate array initializer needs static storage");
          return false;
        }
        element_count = count_global_aggregate_array_initializer(p, p->pos - 1);
        if (!element_count || element_count > SIZE_MAX / field->elem_size ||
            offset > SIZE_MAX - element_count * field->elem_size) {
          fail(p, "invalid flexible aggregate array initializer extent");
          return false;
        }
        size_t required_size = offset + element_count * field->elem_size;
        if (required_size > global->len) {
          unsigned char *grown = realloc(global->bytes, required_size);
          if (!grown) {
            fail(p, "out of memory extending flexible aggregate object");
            return false;
          }
          memset(grown + global->len, 0, required_size - global->len);
          global->bytes = grown;
          global->len = required_size;
          bytes = grown;
        }
      }
      size_t next_index = 0;
      bool first = true;
      while (!tok_is(p, p->pos, "}")) {
        if (!first && !expect(p, ","))
          return false;
        if (tok_is(p, p->pos, "}"))
          break;
        size_t index = next_index;
        if (accept(p, "[")) {
          int64_t designated_index = 0;
          if (!parse_global_const_expr(p, &p->pos, &designated_index) ||
              designated_index < 0 ||
              (uint64_t)designated_index >= element_count || !expect(p, "]") ||
              !expect(p, "=")) {
            fail(p, "invalid aggregate-array initializer designator");
            return false;
          }
          index = (size_t)designated_index;
        }
        if (index >= element_count) {
          fail(p, "too many values in aggregate-array initializer");
          return false;
        }
        size_t element_offset = offset + index * field->elem_size;
        if (field->elem_pointer) {
          if (address >= 0) {
            c_value_t value = parse_expr(p, 1);
            if (value.value < 0)
              return false;
            if (!c_type_is_pointer(value.type) &&
                !p->last_value_null_pointer_constant) {
              fail(p, "aggregate pointer-array initializer requires a pointer");
              return false;
            }
            int target = lower_add_const(p, address, element_offset);
            if (target < 0 ||
                !emit(p,
                      (nyr_inst_t){.op = NYIR_STORE_I64,
                                   .dst = -1,
                                   .a = target,
                                   .c = value.value,
                                   .effects = NYIR_EFFECT_WRITE_MEMORY |
                                              NYIR_EFFECT_MAY_TRAP,
                                   .scalar_class = NYIR_SCALAR_I64_RAW},
                      NULL))
              return false;
          } else if (!initialize_data_pointer(p, global, element_offset,
                                              bytes)) {
            return false;
          }
        } else {
          if (!init_aggregate(p, global, field->agg, bytes, element_offset,
                              depth + 1, address)) {
            if (!p->err[0])
              fail(p, "unsupported nested file-scope aggregate array element");
            return false;
          }
          if (global)
            bytes = global->bytes;
        }
        next_index = index + 1;
        first = false;
      }
      (void)accept(p, ",");
      if (!expect(p, "}"))
        return false;
    } else if (field->kind == C_AGG_FIELD_ARRAY && field->elem_size == 1 &&
               p->pos < p->token_count &&
               p->tokens[p->pos].kind == NY_CTOK_STRING) {
      if (address >= 0) {
        c_value_t string = lower_string_literal(p);
        if (string.value < 0)
          return false;
        size_t string_len = p->globals[p->global_count - 1].len;
        if (!string_len || string_len - 1 > field->elem_count) {
          fail(p, "string initializer does not fit character array");
          return false;
        }
        int target = lower_add_const(p, address, offset);
        size_t copy_len =
            string_len > field->elem_count ? field->elem_count : string_len;
        if (target < 0 || !lower_copy_struct(p, target, string.value, copy_len))
          return false;
      } else {
        unsigned char *string = NULL;
        size_t string_len = 0;
        while (p->pos < p->token_count &&
               p->tokens[p->pos].kind == NY_CTOK_STRING) {
          unsigned char *part = NULL;
          size_t part_len = 0;
          if (!decode_string_token(p->tokens[p->pos++], &part, &part_len)) {
            free(string);
            fail(p, "invalid file-scope aggregate string initializer");
            return false;
          }
          size_t old_content = string_len ? string_len - 1 : 0;
          size_t part_content = part_len ? part_len - 1 : 0;
          if (part_content > SIZE_MAX - old_content - 1) {
            free(part);
            free(string);
            fail(p, "file-scope aggregate string initializer is too large");
            return false;
          }
          unsigned char *joined =
              realloc(string, old_content + part_content + 1);
          if (!joined) {
            free(part);
            free(string);
            fail(p, "out of memory concatenating aggregate string initializer");
            return false;
          }
          string = joined;
          memcpy(string + old_content, part, part_content + 1);
          string_len = old_content + part_content + 1;
          free(part);
        }
        if (!string || !string_len || string_len - 1 > field->elem_count) {
          free(string);
          fail(p, "string initializer does not fit file-scope character array");
          return false;
        }
        size_t copy_len =
            string_len > field->elem_count ? field->elem_count : string_len;
        memcpy(bytes + offset, string, copy_len);
        free(string);
      }
    } else if (field->kind == C_AGG_FIELD_ARRAY) {
      if ((field->array_rank > 1 || !field->elem_count) && address < 0) {
        c_agg_field_t sized_field = *field;
        if (!sized_field.array_extent[0]) {
          size_t rows = count_global_aggregate_array_initializer(p, p->pos);
          size_t row_size = field->elem_size;
          for (unsigned dimension = 1; dimension < field->array_rank;
               ++dimension) {
            size_t extent = field->array_extent[dimension];
            if (!extent || row_size > SIZE_MAX / extent) {
              fail(p, "invalid flexible scalar-array inner extent");
              return false;
            }
            row_size *= extent;
          }
          if (!global || !rows || !row_size || rows > SIZE_MAX / row_size ||
              offset > SIZE_MAX - rows * row_size) {
            fail(p, "invalid flexible scalar-array initializer extent");
            return false;
          }
          size_t required_size = offset + rows * row_size;
          if (required_size > global->len) {
            unsigned char *grown = realloc(global->bytes, required_size);
            if (!grown) {
              fail(p, "out of memory extending flexible aggregate object");
              return false;
            }
            memset(grown + global->len, 0, required_size - global->len);
            global->bytes = grown;
            global->len = required_size;
            bytes = grown;
          }
          sized_field.array_extent[0] = rows;
        }
        if (!init_file_scalar_array_dimension(p, &sized_field, global, 0,
                                              offset, field->elem_size,
                                              global->bytes))
          return false;
      } else {
        if (!expect(p, "{"))
          return false;
        size_t item_size = field->elem_size;
        c_agg_field_t element = {
            .kind =
                field->elem_f32       ? C_AGG_FIELD_F32
                : field->elem_f64     ? C_AGG_FIELD_F64
                : field->elem_pointer ? C_AGG_FIELD_PTR
                : item_size == 1      ? C_AGG_FIELD_U8
                : item_size == 2
                    ? (field->elem_unsigned ? C_AGG_FIELD_U16 : C_AGG_FIELD_I16)
                : item_size == 4
                    ? (field->elem_unsigned ? C_AGG_FIELD_U32 : C_AGG_FIELD_I32)
                    : (field->elem_unsigned ? C_AGG_FIELD_U64
                                            : C_AGG_FIELD_I64),
            .size = item_size,
            .is_signed_byte = field->is_signed_byte,
        };
        size_t i = 0;
        while (i < field->elem_count && !tok_is(p, p->pos, "}")) {
          if (i && !expect(p, ","))
            return false;
          if (tok_is(p, p->pos, "}"))
            break;
          size_t first = i;
          size_t last = i;
          if (accept(p, "[")) {
            int64_t first_index = 0, last_index = 0;
            if (!parse_global_const_expr(p, &p->pos, &first_index) ||
                first_index < 0) {
              fail(p, "invalid scalar-array initializer designator");
              return false;
            }
            last_index = first_index;
            if (accept(p, "...") &&
                (!parse_global_const_expr(p, &p->pos, &last_index) ||
                 last_index < first_index)) {
              fail(p, "invalid scalar-array range designator");
              return false;
            }
            if (!expect(p, "]") || !expect(p, "=") ||
                (uint64_t)last_index >= field->elem_count) {
              fail(p, "scalar-array initializer designator is out of bounds");
              return false;
            }
            first = (size_t)first_index;
            last = (size_t)last_index;
            if (address >= 0 && first != last) {
              fail(p, "runtime scalar-array range designators are unsupported");
              return false;
            }
          }
          if (address >= 0) {
            if (!lower_initializer_field(p, &element, address,
                                         offset + first * item_size))
              return false;
          } else if (field->elem_pointer) {
            if (first != last) {
              fail(p, "file-scope pointer-array range initializers are "
                      "unsupported");
              return false;
            }
            if (!initialize_data_pointer(p, global, offset + first * item_size,
                                         bytes))
              return false;
          } else if (field->elem_f32 || field->elem_f64) {
            double value;
            if (!parse_global_float_add(p, &p->pos, &value)) {
              fail(p, "invalid floating-point file-scope array initializer");
              return false;
            }
            for (size_t index = first; index <= last; ++index) {
              if (field->elem_f32) {
                float single = (float)value;
                memcpy(bytes + offset + index * item_size, &single,
                       sizeof(single));
              } else {
                memcpy(bytes + offset + index * item_size, &value,
                       sizeof(value));
              }
            }
          } else {
            int64_t value;
            if (!parse_global_const_expr(p, &p->pos, &value)) {
              char message[128];
              snprintf(
                  message, sizeof(message),
                  "unsupported constant in file-scope aggregate array near "
                  "`%.48s`",
                  p->tokens[p->pos].start ? p->tokens[p->pos].start : "");
              fail(p, message);
              return false;
            }
            for (size_t index = first; index <= last; ++index)
              memcpy(bytes + offset + index * item_size, &value, item_size);
          }
          i = last + 1;
        }
        (void)accept(p, ",");
        if (!expect(p, "}"))
          return false;
      }
    } else if (address >= 0) {
      if (!lower_initializer_field(p, field, address, offset))
        return false;
    } else {
      if (tok_is(p, p->pos, "{") && tok_is(p, p->pos + 1, "(") &&
          tok_is(p, p->pos + 2, "0") && tok_is(p, p->pos + 3, ")") &&
          tok_is(p, p->pos + 4, "}")) {
        p->pos += 5;
        memset(bytes + offset, 0, c_global_field_size(field));
        goto aggregate_field_initialized;
      }
      size_t scalar_brace_depth = 0;
      while (accept(p, "{"))
        ++scalar_brace_depth;
      bool initialized_as_reloc = false;
      if (field->kind == C_AGG_FIELD_PTR) {
        if (!initialize_data_pointer(p, global, offset, bytes))
          return false;
        initialized_as_reloc = true;
      }
      if (field->kind == C_AGG_FIELD_F32 || field->kind == C_AGG_FIELD_F64) {
        double value;
        if (!parse_global_float_add(p, &p->pos, &value)) {
          fail(p, "file-scope floating-point fields require constant values");
          return false;
        }
        if (field->kind == C_AGG_FIELD_F32) {
          float single = (float)value;
          memcpy(bytes + offset, &single, sizeof(single));
        } else {
          memcpy(bytes + offset, &value, sizeof(value));
        }
      } else if (!initialized_as_reloc) {
        bool integer_address = false;
        if (field->kind != C_AGG_FIELD_F32 && field->kind != C_AGG_FIELD_F64 &&
            !initialize_data_integer_address(p, global, offset,
                                             c_global_field_size(field), bytes,
                                             &integer_address))
          return false;
        if (integer_address)
          initialized_as_reloc = true;
      }
      if (!initialized_as_reloc && field->kind != C_AGG_FIELD_F32 &&
          field->kind != C_AGG_FIELD_F64) {
        int64_t value = 0;
        size_t const_start = p->pos;
        if (!parse_global_const_expr(p, &p->pos, &value)) {
          if (getenv("NYC_KERNEL_COMPAT")) {
            if (p->err)
              p->err[0] = 0;
            p->pos = const_start;
            value = 0;
            /*
             * best-effort skip value expression
             */
            int depth = 0;
            while (p->pos < p->token_count) {
              if (tok_is(p, p->pos, "(") || tok_is(p, p->pos, "[") ||
                  tok_is(p, p->pos, "{"))
                ++depth;
              else if (tok_is(p, p->pos, ")") || tok_is(p, p->pos, "]") ||
                       tok_is(p, p->pos, "}")) {
                if (depth == 0)
                  break;
                --depth;
              } else if (depth == 0 &&
                         (tok_is(p, p->pos, ",") || tok_is(p, p->pos, ";")))
                break;
              ++p->pos;
            }
          } else {
            char message[256];
            snprintf(message, sizeof(message),
                     "file-scope aggregate field is not constant near `%.48s`",
                     p->tokens[p->pos].start ? p->tokens[p->pos].start : "");
            fail(p, message);
            return false;
          }
        }
        size_t size = c_global_field_size(field);
        if (field->is_bitfield) {
          uint64_t prior = 0;
          memcpy(&prior, bytes + offset, size);
          uint64_t mask = field->bit_width == 64
                              ? UINT64_MAX
                              : (UINT64_C(1) << field->bit_width) - 1;
          prior = (prior & ~(mask << field->bit_offset)) |
                  (((uint64_t)value & mask) << field->bit_offset);
          memcpy(bytes + offset, &prior, size);
        } else {
          memcpy(bytes + offset, &value, size);
        }
      }
      while (scalar_brace_depth) {
        (void)accept(p, ",");
        if (!expect(p, "}"))
          return false;
        --scalar_brace_depth;
      }
    }
  aggregate_field_initialized:
    if (bare_designator)
      break;
    if (tok_is(p, p->pos, "}"))
      break;
    if (!expect(p, ","))
      return false;
  }
  if (!bare_designator && !expect(p, "}"))
    return false;
  return !compound_literal || !wrapped_compound_literal || expect(p, ")");
}

static bool init_file_scope_aggregate(c_parser_t *p, c_global_t *global,
                                      const c_agg_type_t *agg,
                                      size_t base_offset) {
  c_global_t *previous = p->global_initializer;
  p->global_initializer = global;
  bool initialized =
      init_aggregate(p, global, agg, global->bytes, base_offset, 0, -1);
  p->global_initializer = previous;
  return initialized;
}

static size_t count_global_aggregate_array_initializer(const c_parser_t *p,
                                                       size_t open) {
  if (!tok_is(p, open, "{"))
    return 0;
  size_t depth = 1;
  size_t next_index = 0;
  size_t extent = 0;
  bool has_value = false;
  for (size_t i = open + 1; i < p->token_count; ++i) {
    if (depth == 1 && tok_is(p, i, ",")) {
      has_value = false;
      continue;
    }
    if (depth == 1 && !has_value && tok_is(p, i, "[")) {
      c_parser_t probe = *p;
      char probe_error[1] = {0};
      probe.err = probe_error;
      probe.err_len = sizeof(probe_error);
      size_t designator_pos = i + 1;
      int64_t designated_index = -1;
      int64_t last_index = -1;
      bool parsed_designator =
          parse_global_const_expr(&probe, &designator_pos, &designated_index);
      if (parsed_designator && tok_is(&probe, designator_pos, "...")) {
        ++designator_pos;
        parsed_designator =
            parse_global_const_expr(&probe, &designator_pos, &last_index);
      } else {
        last_index = designated_index;
      }
      size_t equals_pos = designator_pos + 1;
      while (tok_is(&probe, equals_pos, ".") ||
             tok_is(&probe, equals_pos, "[")) {
        if (tok_is(&probe, equals_pos, ".")) {
          if (!tok_ident(probe.tokens[equals_pos + 1]))
            break;
          equals_pos += 2;
        } else {
          size_t close = skip_balanced(&probe, equals_pos, "[", "]");
          if (close >= probe.token_count)
            break;
          equals_pos = close + 1;
        }
      }
      if (parsed_designator && designated_index >= 0 &&
          last_index >= designated_index &&
          tok_is(&probe, designator_pos, "]") &&
          tok_is(&probe, equals_pos, "=")) {
        if ((uint64_t)last_index >= SIZE_MAX)
          return 0;
        next_index = (size_t)last_index + 1;
        if (next_index > extent)
          extent = next_index;
        has_value = true;
        i = equals_pos;
        continue;
      }
    }
    if (tok_is(p, i, "{")) {
      ++depth;
      if (depth == 2 && !has_value) {
        if (next_index == SIZE_MAX)
          return 0;
        ++next_index;
        if (next_index > extent)
          extent = next_index;
        has_value = true;
      }
    } else if (tok_is(p, i, "}")) {
      if (--depth == 0)
        return extent;
    } else if (depth == 1 && !has_value) {
      if (next_index == SIZE_MAX)
        return 0;
      ++next_index;
      if (next_index > extent)
        extent = next_index;
      has_value = true;
    }
  }
  return 0;
}

static bool parse_file_scope_aggregate_array(
    c_parser_t *p, size_t typedef_array_count, size_t element_size,
    bool *is_array, size_t *array_count, unsigned *array_rank,
    size_t *array_stride, size_t array_extent[C_MAX_ARRAY_RANK],
    bool *incomplete) {
  size_t outer_count = 1;
  size_t inner_count = typedef_array_count ? typedef_array_count : 1;
  unsigned rank = typedef_array_count ? 1u : 0u;
  unsigned explicit_rank = 0;
  bool incomplete_outer = false;
  memset(array_extent, 0, C_MAX_ARRAY_RANK * sizeof(*array_extent));
  if (typedef_array_count)
    array_extent[0] = typedef_array_count;
  *is_array = typedef_array_count != 0;
  while (accept(p, "[")) {
    *is_array = true;
    if (rank == C_MAX_ARRAY_RANK) {
      fail(p, "file-scope aggregate array exceeds the rank limit");
      return false;
    }
    if (accept(p, "]")) {
      if (explicit_rank != 0) {
        fail(p, "only the outer aggregate array extent may be incomplete");
        return false;
      }
      incomplete_outer = true;
      array_extent[explicit_rank] = 0;
    } else {
      uint64_t extent = 0;
      if (!parse_array_extent_expr(p, 1, &extent) ||
          (!extent && explicit_rank != 0) || extent > 65536 ||
          !expect(p, "]")) {
        fail(p, "aggregate array requires a valid constant extent");
        return false;
      }
      if (explicit_rank == 0) {
        outer_count = (size_t)extent;
      } else {
        if (extent > SIZE_MAX / inner_count) {
          fail(p, "aggregate array inner extent overflows");
          return false;
        }
        inner_count *= (size_t)extent;
      }
      array_extent[explicit_rank] = (size_t)extent;
    }
    ++rank;
    ++explicit_rank;
  }
  if (typedef_array_count && explicit_rank)
    array_extent[explicit_rank] = typedef_array_count;
  if (rank && element_size && inner_count > SIZE_MAX / element_size) {
    fail(p, "aggregate array row size overflows");
    return false;
  }
  if (!incomplete_outer && outer_count > SIZE_MAX / inner_count) {
    fail(p, "aggregate array total extent overflows");
    return false;
  }
  *array_rank = rank;
  *array_count = incomplete_outer ? 0 : outer_count * inner_count;
  *array_stride = rank > 1 && element_size ? inner_count * element_size : 0;
  if (incomplete)
    *incomplete = incomplete_outer;
  return true;
}

static bool try_collect_aggregate_global(c_parser_t *p, bool *matched) {
  *matched = false;
  size_t declaration = p->pos;
  size_t declaration_start = p->pos;
  bool declaration_const = false;
  while (tok_is(p, declaration, "const") ||
         tok_is(p, declaration, "volatile") ||
         tok_is(p, declaration, "static") || tok_is(p, declaration, "extern") ||
         tok_is(p, declaration, "register")) {
    declaration_const |= tok_is(p, declaration, "const");
    ++declaration;
  }
  bool inline_definition = false;
  if ((tok_is(p, declaration, "struct") || tok_is(p, declaration, "union")) &&
      tok_ident(p->tokens[declaration + 1]) &&
      tok_is(p, declaration + 2, "{")) {
    size_t after_body = skip_balanced(p, declaration + 2, "{", "}");
    if (after_body < p->token_count && tok_ident(p->tokens[after_body])) {
      c_parser_t attribute_probe = *p;
      char attribute_error[1] = {0};
      attribute_probe.err = attribute_error;
      attribute_probe.err_len = sizeof(attribute_error);
      size_t after_declarator = after_body + 1;
      inline_definition =
          skip_file_scope_attributes(&attribute_probe, &after_declarator) &&
          (tok_is(p, after_declarator, "=") ||
           tok_is(p, after_declarator, ";") ||
           tok_is(p, after_declarator, "["));
    }
    if (!inline_definition)
      return true;
  }
  c_type_t type;
  c_agg_type_t *agg = NULL;
  if (!inline_definition) {
    c_parser_t probe = *p;
    char probe_error[1] = {0};
    probe.err = probe_error;
    probe.err_len = sizeof(probe_error);
    probe.type_lookahead = true;
    if (!parse_type(&probe, &type, false) || type != C_AGG)
      return true;
    while (accept(&probe, "const") || accept(&probe, "volatile")) {
      probe.last_type_const |= tok_is(&probe, probe.pos - 1, "const");
      probe.last_type_volatile |= tok_is(&probe, probe.pos - 1, "volatile");
    }
    ny_ctok_t type_section = {0};
    if (!parse_trailing_file_scope_attributes(&probe, &type_section))
      return true;
    if (!tok_ident(*current(&probe)))
      return true;
    ++probe.pos;
    bool probe_array = false;
    size_t probe_count = 0;
    size_t probe_stride = 0;
    size_t probe_extents[C_MAX_ARRAY_RANK] = {0};
    unsigned probe_rank = 0;
    if (!parse_file_scope_aggregate_array(
            &probe,
            probe.last_type_array_size && probe.last_type_agg &&
                    probe.last_type_agg->size
                ? probe.last_type_array_size / probe.last_type_agg->size
                : 0,
            probe.last_type_agg ? probe.last_type_agg->size : 0, &probe_array,
            &probe_count, &probe_rank, &probe_stride, probe_extents, NULL))
      return true;
    if (probe.last_type_is_array && !probe.last_type_array_size) {
      probe_array = true;
      probe_count = 0;
      probe_rank = probe.last_type_array.rank ? probe.last_type_array.rank : 1;
      probe_stride = probe.last_type_array.element_size;
    }
    ny_ctok_t probe_section = {0};
    if (!parse_trailing_file_scope_attributes(&probe, &probe_section))
      return true;
    if (!tok_is(&probe, probe.pos, "=") && !tok_is(&probe, probe.pos, ";") &&
        !tok_is(&probe, probe.pos, ","))
      return true;
  }
  *matched = true;
  p->last_function_alias = (ny_ctok_t){0};
  if (!parse_type(p, &type, false))
    return false;
  while (accept(p, "const") || accept(p, "volatile")) {
    p->last_type_const |= tok_is(p, p->pos - 1, "const");
    p->last_type_volatile |= tok_is(p, p->pos - 1, "volatile");
  }
  agg = p->last_type_agg;
  size_t type_alignment = p->last_type_alignment;
  ny_ctok_t type_section = p->last_type_section;
  if (!parse_trailing_file_scope_attributes(p, &type_section))
    return false;
  if (p->last_type_alignment > type_alignment)
    type_alignment = p->last_type_alignment;
  size_t typedef_array_size = p->last_type_array_size;
  bool typedef_is_array = p->last_type_is_array;
  bool is_const = declaration_const || p->last_type_const;
  size_t typedef_array_count = 0;
  if (typedef_array_size) {
    if (!agg || !agg->size || typedef_array_size % agg->size) {
      fail(p, "aggregate typedef array has an invalid element size");
      return false;
    }
    typedef_array_count = typedef_array_size / agg->size;
  }
  bool is_external = false;
  for (size_t i = declaration_start;
       i < p->token_count && !tok_is(p, i, ";") && !tok_is(p, i, "="); ++i)
    is_external |= tok_is(p, i, "extern");
  ny_ctok_t name;
  if (!parse_name(p, &name))
    return false;
  bool is_array = false;
  size_t array_count = 0;
  unsigned array_rank = 0;
  size_t array_stride = 0;
  size_t array_extents[C_MAX_ARRAY_RANK] = {0};
  bool incomplete_array = false;
  if (!parse_file_scope_aggregate_array(
          p, typedef_array_count, agg ? agg->size : 0, &is_array, &array_count,
          &array_rank, &array_stride, array_extents, &incomplete_array))
    return false;
  if (typedef_is_array && !typedef_array_size) {
    is_array = true;
    array_count = 0;
    array_rank = p->last_type_array.rank ? p->last_type_array.rank : 1;
    array_stride = p->last_type_array.element_size;
    incomplete_array = true;
  }
  ny_ctok_t trailing_section = {0};
  if (!parse_trailing_file_scope_attributes(p, &trailing_section))
    return false;
  bool incomplete_extern = is_external && agg && !agg->complete;
  bool has_initializer = accept(p, "=");
  ny_ctok_t alias_target = p->last_function_alias;
  p->last_function_alias = (ny_ctok_t){0};
  if (alias_target.len && (has_initializer || !tok_is(p, p->pos, ";"))) {
    fail(p, "C aggregate alias must be a single declaration without an "
            "initializer");
    return false;
  }
  if (alias_target.len)
    is_external = true; /* Alias metadata defines the symbol, not new bytes. */
  c_global_t *existing = NULL;
  bool completing_incomplete_array = false;
  bool replacing_incomplete_array = false;
  /*
   * `extern typeof(object) object;` declares the object again with its own
   * type by construction. Array shape carried through typeof can differ from
   * the declarator's recorded rank, so recognize the form instead of treating
   * it as a conflicting definition.
   */
  bool typeof_redeclaration = false;
  if (is_external && !has_initializer && p->pos >= 5 &&
      p->pos < p->token_count &&
      (tok_is(p, p->pos - 5, "typeof") ||
       tok_is(p, p->pos - 5, "__typeof__")) &&
      tok_is(p, p->pos - 4, "(") && tok_is(p, p->pos - 2, ")") &&
      p->tokens[p->pos - 3].len == name.len &&
      memcmp(p->tokens[p->pos - 3].start, name.start, name.len) == 0)
    typeof_redeclaration = true;
  for (;;) {
    existing = find_global(p, name);
    bool matching_extern = is_external && !has_initializer && existing &&
                           existing->type == C_AGG && existing->agg == agg &&
                           ((existing->is_array == is_array &&
                             existing->array_rank == array_rank) ||
                            (!is_array && existing->is_array)) &&
                           existing->is_const == is_const;
    if (getenv("NYC_KERNEL_COMPAT") && is_external && !has_initializer &&
        existing && !matching_extern)
      matching_extern = true;
    bool completing_extern =
        (!is_external || has_initializer) && existing &&
        (existing->is_external || existing->is_tentative) &&
        existing->type == C_AGG && existing->agg == agg &&
        existing->is_array == is_array && existing->array_rank == array_rank &&
        existing->is_const == is_const;
    completing_incomplete_array =
        !is_external && existing && existing->type == C_AGG &&
        existing->agg == agg && existing->is_array && is_array &&
        existing->array_incomplete && incomplete_array &&
        existing->array_rank == array_rank && existing->is_const == is_const;
    replacing_incomplete_array =
        !is_external && existing && existing->type == C_AGG &&
        existing->agg == agg && existing->is_array && is_array &&
        existing->array_incomplete && !incomplete_array &&
        existing->array_rank == array_rank && existing->is_const == is_const;
    bool matching_redeclaration =
        !has_initializer && existing && existing->type == C_AGG &&
        existing->agg == agg && existing->is_array == is_array &&
        existing->array_rank == array_rank && existing->is_const == is_const &&
        !(existing->is_external && !is_external);
    if (typeof_redeclaration && existing && existing->type == C_AGG &&
        existing->agg == agg)
      matching_redeclaration = true;
    if (getenv("NYC_KERNEL_COMPAT") && !has_initializer && existing &&
        !matching_redeclaration && !replacing_incomplete_array &&
        (is_external || !existing->is_external))
      matching_redeclaration = true;
    if (matching_redeclaration && !replacing_incomplete_array) {
      if (!expect(p, ";"))
        return false;
      if (p->last_type_alignment > existing->alignment)
        existing->alignment = p->last_type_alignment;
      if (alias_target.len)
        existing->alias_target = alias_target;
      return true;
    }
    if (!agg || (!agg->complete && !incomplete_extern) ||
        (existing && !matching_extern && !completing_extern &&
         !completing_incomplete_array && !replacing_incomplete_array) ||
        (!existing && p->global_count >= p->global_capacity)) {
      if (getenv("NYC_KERNEL_COMPAT") && is_external && !has_initializer &&
          existing) {
        if (!expect(p, ";"))
          return false;
        return true;
      }
      if (getenv("NYC_PP_TRACE")) {
        fprintf(stderr,
                "AGG-FAIL name=`%.*s` agg=%p complete=%d is_external=%d "
                "dup=%d cap=%d tokens:",
                (int)name.len, name.start, (void *)agg,
                agg ? (int)agg->complete : -1, (int)is_external,
                agg ? find_global(p, name) != NULL : -1,
                (int)(p->global_count >= p->global_capacity));
        for (size_t t = p->pos > 8 ? p->pos - 8 : 0;
             t < p->token_count && t < p->pos + 3; ++t)
          fprintf(stderr, " [%zu:%u:%u]%.*s", t, p->tokens[t].line,
                  p->tokens[t].col,
                  (int)(p->tokens[t].len > 24 ? 24 : p->tokens[t].len),
                  p->tokens[t].start);
        fputc('\n', stderr);
      }
      fail(p, "invalid file-scope aggregate definition");
      return false;
    }
    if (is_external && !has_initializer) {
      /*
       * Bare extern declarations (`extern struct F v[];`) need no
       * storage: arrays may keep an unspecified extent and the type may
       * be incomplete. Comma-separated declarators each record one.
       */
      if (!matching_extern) {
        c_global_t global = {.name = name, .bytes = NULL, .len = 0};
        global.type = C_AGG;
        global.agg = agg;
        global.is_array = is_array;
        global.array_rank = array_rank;
        global.array_stride = array_stride;
        global.array_shape =
            (c_array_shape_t){.rank = array_rank,
                              .element_type = C_AGG,
                              .element_size = agg ? agg->size : 0,
                              .agg = agg};
        memcpy(global.array_shape.extent, array_extents,
               array_rank * sizeof(*array_extents));
        if (array_count && agg->size <= SIZE_MAX / array_count)
          global.len = agg->size * array_count;
        global.is_const = is_const;
        global.is_external = true;
        global.alignment = p->last_type_alignment;
        global.alias_target = alias_target;
        p->globals[p->global_count++] = global;
      }
      if (!accept(p, ",")) {
        if (!expect(p, ";"))
          return false;
        return true;
      }
      if (tok_is(p, p->pos, "*")) {
        fail(p, "mixed pointer declarators in extern aggregate lists are not "
                "supported");
        return false;
      }
      if (!parse_name(p, &name))
        return false;
      if (!parse_file_scope_aggregate_array(
              p, typedef_array_count, agg ? agg->size : 0, &is_array,
              &array_count, &array_rank, &array_stride, array_extents,
              &incomplete_array))
        return false;
      trailing_section = (ny_ctok_t){0};
      p->last_type_alignment = type_alignment;
      if (!parse_trailing_file_scope_attributes(p, &trailing_section))
        return false;
      has_initializer = accept(p, "=");
      if (has_initializer) {
        fail(p, "extern C aggregate declarations cannot have initializers");
        return false;
      }
      continue;
    }
    break;
  }
  if (!array_count && incomplete_array && !has_initializer && !is_external &&
      is_array) {
    c_global_t global = {.name = name,
                         .type = C_AGG,
                         .agg = agg,
                         .is_array = true,
                         .array_incomplete = true,
                         .array_rank = array_rank,
                         .array_stride = array_stride,
                         .is_const = is_const,
                         .alignment = p->last_type_alignment,
                         .is_tentative = true};
    if (!expect(p, ";"))
      return false;
    if (existing) {
      if (replacing_incomplete_array) {
        free(existing->bytes);
        free(existing->relocs);
        if (existing->alignment > global.alignment)
          global.alignment = existing->alignment;
        *existing = global;
      }
    } else {
      p->globals[p->global_count++] = global;
    }
    return true;
  }
  if (!is_external && !has_initializer && tok_is(p, p->pos, ",")) {
    for (;;) {
      if (!agg || !agg->complete || !array_count ||
          agg->size > SIZE_MAX / array_count) {
        fail(p, "file-scope aggregate declarator has an invalid size");
        return false;
      }
      c_global_t *previous = find_global(p, name);
      size_t total_size = agg->size * array_count;
      if (previous) {
        if (previous->type != C_AGG || previous->agg != agg ||
            previous->is_array != is_array ||
            previous->array_rank != array_rank ||
            previous->is_const != is_const || previous->is_external) {
          fail(p, "incompatible repeated file-scope aggregate declaration");
          return false;
        }
        if (p->last_type_alignment > previous->alignment)
          previous->alignment = p->last_type_alignment;
      } else {
        if (p->global_count >= p->global_capacity) {
          fail(p, "C frontend global-variable storage capacity exceeded");
          return false;
        }
        unsigned char *storage = calloc(total_size ? total_size : 1, 1);
        if (!storage) {
          fail(p, "out of memory allocating file-scope aggregate");
          return false;
        }
        p->globals[p->global_count++] =
            (c_global_t){.name = name,
                         .bytes = storage,
                         .len = total_size,
                         .type = C_AGG,
                         .agg = agg,
                         .is_array = is_array,
                         .array_rank = array_rank,
                         .array_stride = array_stride,
                         .is_const = is_const,
                         .alignment = p->last_type_alignment,
                         .is_tentative = true};
      }
      if (!accept(p, ","))
        break;
      if (accept(p, "*")) {
        bool pointer_const = accept(p, "const");
        while (accept(p, "volatile")) {
        }
        ny_ctok_t pointer_name;
        ny_ctok_t pointer_section = {0};
        if (!parse_name(p, &pointer_name) ||
            !parse_trailing_file_scope_attributes(p, &pointer_section))
          return false;
        bool pointer_initializer = accept(p, "=");
        size_t pointer_size = c_type_size(p->abi, C_VOID_PTR);
        unsigned char *pointer_bytes = calloc(pointer_size, 1);
        if (!pointer_bytes) {
          fail(p, "out of memory allocating aggregate pointer global");
          return false;
        }
        c_global_t pointer_global = {.name = pointer_name,
                                     .bytes = pointer_bytes,
                                     .len = pointer_size,
                                     .type = C_AGG_PTR,
                                     .agg = agg,
                                     .is_const = pointer_const,
                                     .is_tentative = !pointer_initializer};
        if (pointer_section.len) {
          char section_name[64] = {0};
          if (!copy_quoted_token(p, pointer_section, section_name,
                                 sizeof(section_name))) {
            free(pointer_bytes);
            fail(p, "invalid aggregate pointer global section");
            return false;
          }
          pointer_global.section = strdup(section_name);
          if (!pointer_global.section) {
            free(pointer_bytes);
            fail(p, "out of memory storing an aggregate pointer section");
            return false;
          }
        }
        if (pointer_initializer &&
            !initialize_data_pointer(p, &pointer_global, 0, pointer_bytes)) {
          free(pointer_bytes);
          free(pointer_global.relocs);
          free(pointer_global.section);
          return false;
        }
        if (p->global_count >= p->global_capacity) {
          free(pointer_bytes);
          free(pointer_global.relocs);
          free(pointer_global.section);
          fail(p, "C frontend global-variable storage capacity exceeded");
          return false;
        }
        p->globals[p->global_count++] = pointer_global;
        return expect(p, ";");
      }
      if (!parse_name(p, &name) ||
          !parse_file_scope_aggregate_array(
              p, typedef_array_count, agg->size, &is_array, &array_count,
              &array_rank, &array_stride, array_extents, &incomplete_array))
        return false;
      trailing_section = (ny_ctok_t){0};
      p->last_type_alignment = type_alignment;
      if (!parse_trailing_file_scope_attributes(p, &trailing_section) ||
          trailing_section.len) {
        fail(p, "placement attributes on repeated aggregate declarators are "
                "unsupported");
        return false;
      }
      has_initializer = accept(p, "=");
      if (has_initializer) {
        fail(p,
             "initializers on repeated aggregate declarators are unsupported");
        return false;
      }
    }
    return expect(p, ";");
  }
  if (is_array && has_initializer && array_count == 0) {
    array_count = count_global_aggregate_array_initializer(p, p->pos);
    if (array_count && array_rank > 1) {
      size_t rows = array_count;
      size_t inner_count = array_stride / agg->size;
      if (!inner_count || rows > SIZE_MAX / inner_count) {
        fail(p, "incomplete aggregate array extent overflows");
        return false;
      }
      array_extents[0] = rows;
      array_count = rows * inner_count;
    } else if (array_count && array_rank == 1) {
      array_extents[0] = array_count;
    }
  }
  if (!array_count && !has_initializer && completing_incomplete_array) {
    if (!expect(p, ";"))
      return false;
    return true;
  }
  bool empty_array_initializer = is_array && has_initializer &&
                                 tok_is(p, p->pos, "{") &&
                                 tok_is(p, p->pos + 1, "}");
  if ((!array_count && !empty_array_initializer &&
       (!is_array || incomplete_array)) ||
      (array_count && agg->size > SIZE_MAX / array_count)) {
    fail(p, "file-scope aggregate array has an invalid size");
    return false;
  }
  size_t total_size = agg->size * array_count;
  unsigned char *bytes = calloc(total_size ? total_size : 1, 1);
  if (!bytes) {
    fail(p, "out of memory allocating file-scope aggregate");
    return false;
  }
  c_global_t global = {.name = name,
                       .bytes = bytes,
                       .len = total_size,
                       .type = C_AGG,
                       .agg = agg,
                       .is_array = is_array,
                       .array_rank = array_rank,
                       .array_stride = array_stride,
                       .is_const = is_const,
                       .alignment = p->last_type_alignment};
  if (type_section.len && trailing_section.len &&
      !tok_same(type_section, trailing_section)) {
    free(bytes);
    fail(p, "conflicting section attributes on a file-scope aggregate");
    return false;
  }
  ny_ctok_t object_section =
      trailing_section.len ? trailing_section : type_section;
  /*
   * The section name is resolved only once the initializer has parsed.  Doing
   * it here allocated `global.section` before dozens of mid-initializer error
   * exits that already release `global.bytes`/`global.relocs`, so every
   * malformed initializer leaked the section name.
   */
  size_t compound_literal_closing_parens = 0;
  if (has_initializer) {
    if (!is_array && tok_is(p, p->pos, "(")) {
      c_parser_t probe = *p;
      char probe_error[1] = {0};
      probe.err = probe_error;
      probe.err_len = sizeof(probe_error);
      probe.type_lookahead = true;
      size_t leading_parens = 0;
      while (accept(&probe, "("))
        ++leading_parens;
      c_type_t initializer_type;
      if (!leading_parens || !parse_type(&probe, &initializer_type, false) ||
          initializer_type != C_AGG || probe.last_type_agg != agg ||
          !expect(&probe, ")") || !tok_is(&probe, probe.pos, "{")) {
        free(bytes);
        fail(p, "aggregate initializer compound literal type mismatch");
        return false;
      }
      compound_literal_closing_parens = leading_parens - 1;
      p->pos = probe.pos;
    }
    bool initialized = true;
    if (is_array) {
      initialized = expect(p, "{");
      size_t i = 0;
      bool first_element = true;
      while (initialized && !tok_is(p, p->pos, "}")) {
        if (!first_element && !expect(p, ",")) {
          initialized = false;
          break;
        }
        if (tok_is(p, p->pos, "}"))
          break;
        first_element = false;
        size_t last_designated_index = SIZE_MAX;
        if (accept(p, "[")) {
          int64_t designated_index = -1;
          int64_t last_index = -1;
          uint64_t designated_limit = array_count;
          size_t designated_row_stride = 1;
          if (array_rank > 1 && array_extents[0] &&
              array_count % array_extents[0] == 0 && tok_is(p, p->pos, "[")) {
            /*
             * will check after parsing; handled below
             */
          }
          /*
           * For 2D aggregate arrays initialized by row, a designated index
           * addresses the outer dimension (row). Peek: if the value after
           * `]=` starts a row initializer, scale by row element count.
           */
          if (array_rank > 1 && array_extents[0] &&
              array_count % array_extents[0] == 0) {
            size_t row_count_peek = array_count / array_extents[0];
            /*
             * probe: parse index, then see if `] = {` follows as row
             */
            c_parser_t dprobe = *p;
            char dprobe_err[1] = {0};
            dprobe.err = dprobe_err;
            dprobe.err_len = sizeof(dprobe_err);
            int64_t di = -1, li = -1;
            if (parse_global_const_expr(&dprobe, &dprobe.pos, &di) && di >= 0) {
              li = di;
              if (tok_is(&dprobe, dprobe.pos, "...")) {
                ++dprobe.pos;
                if (!parse_global_const_expr(&dprobe, &dprobe.pos, &li))
                  li = di;
              }
              if (tok_is(&dprobe, dprobe.pos, "]") &&
                  tok_is(&dprobe, dprobe.pos + 1, "=") &&
                  tok_is(&dprobe, dprobe.pos + 2, "{") &&
                  (uint64_t)di < array_extents[0]) {
                designated_limit = array_extents[0];
                designated_row_stride = row_count_peek;
              }
            }
          }
          if (!parse_global_const_expr(p, &p->pos, &designated_index) ||
              designated_index < 0 ||
              (uint64_t)designated_index >= designated_limit) {
            fail(p, "invalid file-scope aggregate array designator");
            initialized = false;
            break;
          }
          if (accept(p, "...")) {
            if (!parse_global_const_expr(p, &p->pos, &last_index) ||
                last_index < designated_index ||
                (uint64_t)last_index >= designated_limit) {
              fail(p, "invalid file-scope aggregate array range designator");
              initialized = false;
              break;
            }
          } else {
            last_index = designated_index;
          }
          bool member_designator = tok_is(p, p->pos + 1, ".");
          if (!expect(p, "]") ||
              (member_designator && last_index != designated_index) ||
              (!member_designator && !expect(p, "="))) {
            fail(p, "invalid file-scope aggregate array designator");
            initialized = false;
            break;
          }
          i = (size_t)designated_index * designated_row_stride;
          last_designated_index = (size_t)last_index * designated_row_stride +
                                  (designated_row_stride - 1);
          if (designated_row_stride == 1)
            last_designated_index = (size_t)last_index;
        }
        if (i >= array_count) {
          fail(p,
               "too many elements in file-scope aggregate array initializer");
          initialized = false;
          break;
        }
        size_t relocation_start = global.reloc_count;
        size_t initialized_count = 1;
        if (array_rank > 1 && array_extents[0] &&
            array_count % array_extents[0] == 0 && tok_is(p, p->pos, "{")) {
          size_t row_count = array_count / array_extents[0];
          if (i % row_count != 0 || !expect(p, "{")) {
            initialized = false;
          } else {
            size_t row_index = 0;
            bool first_row_element = true;
            while (initialized && row_index < row_count &&
                   !tok_is(p, p->pos, "}")) {
              if (!first_row_element && !expect(p, ",")) {
                initialized = false;
                break;
              }
              if (tok_is(p, p->pos, "}"))
                break;
              first_row_element = false;
              initialized = init_file_scope_aggregate(
                  p, &global, agg, (i + row_index) * agg->size);
              bytes = global.bytes;
              ++row_index;
            }
            (void)accept(p, ",");
            if (initialized)
              initialized = expect(p, "}");
            initialized_count = row_count;
            if (initialized)
              last_designated_index = SIZE_MAX;
          }
        } else {
          initialized =
              init_file_scope_aggregate(p, &global, agg, i * agg->size);
          bytes = global.bytes;
        }
        if (initialized && last_designated_index != SIZE_MAX) {
          size_t relocation_end = global.reloc_count;
          for (size_t copy = i + 1;
               copy <= last_designated_index && initialized; ++copy) {
            size_t byte_delta = (copy - i) * agg->size;
            memcpy(global.bytes + copy * agg->size,
                   global.bytes + i * agg->size, agg->size);
            for (size_t relocation = relocation_start;
                 relocation < relocation_end; ++relocation) {
              size_t offset = global.relocs[relocation].offset + byte_delta;
              ny_ctok_t target = global.relocs[relocation].target;
              initialized = append_global_reloc(p, &global, offset, target);
              if (!initialized)
                break;
            }
          }
          i = last_designated_index + 1;
        } else if (initialized)
          i += initialized_count;
      }
      if (initialized)
        (void)accept(p, ",");
      if (initialized)
        initialized = expect(p, "}");
    } else {
      initialized = init_file_scope_aggregate(p, &global, agg, 0);
      bytes = global.bytes;
    }
    if (!initialized) {
      free(global.bytes);
      free(global.relocs);
      if (is_external)
        fail(p, "extern C aggregate declarations cannot have initializers");
      else if (!p->err[0]) {
        char message[160];
        snprintf(message, sizeof(message),
                 "unsupported file-scope aggregate initializer near `%.64s`",
                 p->pos < p->token_count && p->tokens[p->pos].start
                     ? p->tokens[p->pos].start
                     : "");
        fail(p, message);
      }
      return false;
    }
  }
  while (compound_literal_closing_parens--)
    if (!expect(p, ")")) {
      free(global.bytes);
      free(global.relocs);
      return false;
    }
  if (!expect(p, ";")) {
    free(global.bytes);
    free(global.relocs);
    return false;
  }
  if (object_section.len) {
    char section_name[64] = {0};
    if (!copy_quoted_token(p, object_section, section_name,
                           sizeof(section_name)) ||
        !(global.section = strdup(section_name))) {
      free(global.bytes);
      free(global.relocs);
      fail(p, "invalid or unavailable file-scope aggregate section name");
      return false;
    }
  }
  global.type = C_AGG;
  global.agg = agg;
  global.is_array = is_array;
  global.is_const = is_const;
  global.is_external = is_external && !has_initializer;
  global.is_tentative = !is_external && !has_initializer;
  if (is_array) {
    global.array_rank = array_rank;
    global.array_stride = array_stride;
    global.array_shape = (c_array_shape_t){.rank = array_rank,
                                           .element_type = C_AGG,
                                           .element_size = agg ? agg->size : 0,
                                           .agg = agg};
    memcpy(global.array_shape.extent, array_extents,
           array_rank * sizeof(*array_extents));
  }
  if (existing) {
    if (existing->alignment > global.alignment)
      global.alignment = existing->alignment;
    free(existing->bytes);
    free(existing->relocs);
    free(existing->section);
    *existing = global;
  } else {
    p->globals[p->global_count++] = global;
  }
  return true;
}

static bool typeof_names_function(c_parser_t *p, size_t type_pos);

static bool try_collect_external_aggregate_pointer_global(c_parser_t *p,
                                                          bool *matched) {
  *matched = false;
  c_parser_t probe = *p;
  char probe_error[1] = {0};
  probe.err = probe_error;
  probe.err_len = sizeof(probe_error);
  probe.type_lookahead = true;
  c_type_t type;
  if (!parse_type(&probe, &type, false) ||
      (type != C_AGG_PTR && type != C_AGG_PTR_PTR) ||
      !tok_ident(*current(&probe)) || typeof_names_function(p, p->pos))
    return true;
  ++probe.pos;
  if (tok_is(&probe, probe.pos, "["))
    return true;
  ny_ctok_t ignored_section = {0};
  if (!parse_trailing_file_scope_attributes(&probe, &ignored_section) ||
      (!tok_is(&probe, probe.pos, ";") && !tok_is(&probe, probe.pos, ",")))
    return true;
  bool is_external = false;
  for (size_t i = p->pos; i < probe.pos; ++i)
    is_external |= tok_is(p, i, "extern");
  if (!is_external)
    return true;

  *matched = true;
  if (!parse_type(p, &type, false))
    return false;
  c_agg_type_t *aggregate = p->last_type_agg;
  bool typedef_array = p->last_type_is_array;
  size_t typedef_array_size = p->last_type_array_size;
  unsigned typedef_array_rank = p->last_type_array.rank;
  if (typedef_array && !typedef_array_rank)
    typedef_array_rank = 1;
  bool is_const = p->last_type_const_pointer;
  bool first = true;
  for (;;) {
    if (!first) {
      unsigned pointers = 0;
      while (accept(p, "*"))
        ++pointers;
      unsigned required_pointers = type == C_AGG_PTR_PTR ? 2 : 1;
      if (pointers != required_pointers) {
        fail(p, "external aggregate-pointer declarators require one pointer "
                "level or a supported pointer-to-pointer declarator");
        return false;
      }
    }
    first = false;
    ny_ctok_t name;
    ny_ctok_t trailing_section = {0};
    if (!parse_name(p, &name) || tok_is(p, p->pos, "[") ||
        !parse_trailing_file_scope_attributes(p, &trailing_section)) {
      if (!p->err[0])
        fail(p, "external aggregate-pointer global requires a scalar "
                "declarator");
      return false;
    }
    c_global_t *existing = find_global(p, name);
    if (existing) {
      if (existing->type != type || existing->agg != aggregate ||
          existing->is_const != is_const ||
          existing->is_array != typedef_array ||
          (typedef_array && existing->array_rank != typedef_array_rank)) {
        fail(p, "conflicting external aggregate-pointer declaration");
        return false;
      }
    } else if (p->global_count >= p->global_capacity) {
      fail(p, "duplicate or excessive external aggregate-pointer global");
      return false;
    } else {
      p->globals[p->global_count++] =
          (c_global_t){.name = name,
                       .type = type,
                       .agg = aggregate,
                       .is_const = is_const,
                       .is_external = true,
                       .len = typedef_array_size,
                       .is_array = typedef_array,
                       .array_rank = typedef_array_rank,
                       .is_pointer_array = typedef_array};
    }
    if (!accept(p, ","))
      break;
  }
  return expect(p, ";");
}

static bool try_collect_aggregate_pointer_array(c_parser_t *p, bool *matched) {
  *matched = false;
  c_parser_t probe = *p;
  char probe_error[1] = {0};
  probe.err = probe_error;
  probe.err_len = sizeof(probe_error);
  probe.type_lookahead = true;
  c_type_t type;
  if (!parse_type(&probe, &type, false) ||
      (!c_type_is_pointer(type) &&
       !(type == C_AGG && probe.last_type_array_size)) ||
      !tok_ident(*current(&probe)) ||
      (!tok_is(&probe, probe.pos + 1, "[") &&
       !(type == C_AGG_PTR && probe.last_type_array_size &&
         (tok_is(&probe, probe.pos + 1, "=") ||
          tok_is(&probe, probe.pos + 1, ";")))))
    return true;

  *matched = true;
  bool is_external = false;
  for (size_t i = p->pos; i < probe.pos; ++i)
    is_external |= tok_is(p, i, "extern");
  c_type_t parsed_type;
  if (!parse_type(p, &parsed_type, false))
    return false;
  c_agg_type_t *aggregate = p->last_type_agg;
  ny_ctok_t declaration_section = p->last_type_section;
  bool is_const = p->last_type_const_pointer;
  bool aggregate_array_elements = parsed_type == C_AGG;
  size_t typedef_array_size = p->last_type_array_size;
  c_array_shape_t typedef_shape = p->last_type_array;
  bool typedef_pointer_array =
      c_type_is_pointer(parsed_type) && typedef_array_size;
  c_type_t array_element_type = aggregate_array_elements ? C_AGG : parsed_type;
  if (aggregate_array_elements && (!aggregate || !aggregate->complete)) {
    fail(p, "aggregate typedef array requires a complete element type");
    return false;
  }
  size_t element_size = c_type_size(p->abi, C_VOID_PTR);
  if (aggregate_array_elements) {
    if (!typedef_array_size) {
      fail(p, "aggregate typedef array has no storage size");
      return false;
    }
    element_size = typedef_array_size;
  }
  bool first_declarator = true;
  for (;;) {
    if (!first_declarator) {
      unsigned pointers = 0;
      while (accept(p, "*"))
        ++pointers;
      while (accept(p, "const") || accept(p, "volatile")) {
      }
      if (pointers != 1) {
        fail(p,
             "aggregate-pointer array declarators require one pointer level");
        return false;
      }
    }
    first_declarator = false;
    ny_ctok_t name;
    if (!parse_name(p, &name))
      return false;
    size_t array_rank = 0;
    size_t array_extents[C_MAX_ARRAY_RANK] = {0};
    uint64_t outer_extent = 0;
    uint64_t typedef_count =
        typedef_pointer_array ? typedef_array_size / element_size : 1;
    bool typedef_array_only = typedef_pointer_array && !tok_is(p, p->pos, "[");
    uint64_t inner_count =
        !aggregate_array_elements && typedef_array_size && !typedef_array_only
            ? typedef_count
            : 1;
    bool incomplete = false;
    if (typedef_array_only) {
      outer_extent =
          typedef_shape.rank ? typedef_shape.extent[0] : typedef_count;
      inner_count = outer_extent ? typedef_count / outer_extent : 1;
    }
    while (accept(p, "[")) {
      if (array_rank == C_MAX_ARRAY_RANK) {
        fail(p, "aggregate-pointer array exceeds the rank limit");
        return false;
      }
      uint64_t extent = 0;
      if (!array_rank && accept(p, "]")) {
        incomplete = true;
      } else if (!parse_array_extent_expr(p, 1, &extent) ||
                 (!extent && array_rank) || extent > 65536 || !expect(p, "]")) {
        fail(p, "file-scope aggregate-pointer array requires a valid constant "
                "extent");
        return false;
      } else if (array_rank == 0) {
        outer_extent = extent;
      } else {
        if (extent > SIZE_MAX / inner_count) {
          fail(p, "file-scope aggregate-pointer array size overflows");
          return false;
        }
        inner_count *= extent;
      }
      array_extents[array_rank] = (size_t)extent;
      ++array_rank;
    }

    ny_ctok_t trailing_section = declaration_section;
    if (!parse_trailing_file_scope_attributes(p, &trailing_section))
      return false;
    bool has_initializer = accept(p, "=");
    size_t initializer_count = 0;
    if (has_initializer) {
      if (aggregate_array_elements || (array_rank > 2 && !typedef_array_only) ||
          !tok_is(p, p->pos, "{")) {
        fail(p, "file-scope aggregate-pointer array initializer has an "
                "unsupported form");
        return false;
      }
      initializer_count =
          array_rank <= 1 || incomplete
              ? count_global_aggregate_array_initializer(p, p->pos)
              : outer_extent;
      bool empty_initializer = tok_is(p, p->pos + 1, "}");
      if ((!initializer_count && !empty_initializer) ||
          (!incomplete && initializer_count > outer_extent)) {
        fail(p, "file-scope aggregate-pointer array initializer has an "
                "invalid element count");
        return false;
      }
      if (incomplete)
        outer_extent = initializer_count;
    }

    if (!element_size || inner_count > SIZE_MAX / element_size ||
        (!incomplete &&
         outer_extent > SIZE_MAX / (inner_count * element_size))) {
      fail(p, "file-scope aggregate-pointer array size overflows");
      return false;
    }
    c_array_shape_t shape = {.rank = (unsigned)array_rank,
                             .element_type = array_element_type,
                             .element_size = aggregate_array_elements
                                                 ? aggregate->size
                                                 : element_size,
                             .agg = aggregate};
    memcpy(shape.extent, array_extents, array_rank * sizeof(size_t));
    if (typedef_shape.rank) {
      if (array_rank + typedef_shape.rank > C_MAX_ARRAY_RANK) {
        fail(p, "aggregate-pointer array exceeds the rank limit");
        return false;
      }
      memcpy(shape.extent + array_rank, typedef_shape.extent,
             typedef_shape.rank * sizeof(size_t));
      shape.rank += typedef_shape.rank;
    }
    if (shape.rank)
      shape.extent[0] = (size_t)outer_extent;
    size_t stored_array_rank = shape.rank;
    c_global_t *previous = find_global(p, name);
    size_t bytes_len =
        incomplete && !has_initializer
            ? is_external ? 0 : (size_t)inner_count * element_size
            : (size_t)(outer_extent * inner_count * element_size);
    bool compatible_redeclaration =
        previous &&
        (previous->is_external || previous->is_tentative || !has_initializer) &&
        previous->is_array &&
        previous->is_pointer_array == !aggregate_array_elements &&
        previous->type == array_element_type && previous->agg == aggregate &&
        previous->is_const == is_const &&
        previous->array_rank == stored_array_rank &&
        (previous->array_incomplete || incomplete ||
         previous->len == bytes_len);
    if ((previous && !compatible_redeclaration) ||
        (!previous && p->global_count >= p->global_capacity)) {
      fail(p, "invalid file-scope aggregate-pointer array");
      return false;
    }
    if (previous && incomplete && !has_initializer &&
        !previous->array_incomplete) {
      bytes_len = previous->len;
      shape = previous->array_shape;
      incomplete = false;
    }
    if (compatible_redeclaration) {
      if (!has_initializer && (is_external || (!previous->is_external &&
                                               !previous->is_tentative))) {
        if (bytes_len > previous->len)
          previous->len = bytes_len;
        if (!incomplete) {
          previous->array_stride =
              stored_array_rank > 1 ? (size_t)inner_count * element_size : 0;
          previous->array_shape = shape;
          previous->array_incomplete = false;
        }
        if (!accept(p, ","))
          break;
        continue;
      }
    }
    unsigned char *bytes = (is_external && !has_initializer)
                               ? NULL
                               : calloc(bytes_len ? bytes_len : 1, 1);
    if (!(is_external && !has_initializer) && !bytes) {
      fail(p, "out of memory allocating file-scope aggregate-pointer array");
      return false;
    }
    c_global_t global = {
        .name = name,
        .bytes = bytes,
        .len = bytes_len,
        .type = array_element_type,
        .agg = aggregate,
        .is_array = true,
        .array_incomplete = incomplete && !has_initializer,
        .array_rank = stored_array_rank,
        .array_shape = shape,
        .array_stride =
            stored_array_rank > 1 ? (size_t)inner_count * element_size : 0,
        .is_pointer_array = !aggregate_array_elements,
        .is_const = is_const,
        .is_external = is_external && !has_initializer,
        .is_tentative = !is_external && !has_initializer};
    if (trailing_section.len) {
      char section_name[64] = {0};
      if (!copy_quoted_token(p, trailing_section, section_name,
                             sizeof(section_name))) {
        free(global.bytes);
        fail(p, "invalid aggregate-pointer array section");
        return false;
      }
      global.section = strdup(section_name);
      if (!global.section) {
        free(global.bytes);
        fail(p, "out of memory storing an aggregate-pointer array section");
        return false;
      }
    }
    if (has_initializer) {
      ++p->pos; /* opening brace */
      size_t index = 0;
      size_t total_count = (size_t)outer_extent * (size_t)inner_count;
      while (!tok_is(p, p->pos, "}")) {
        size_t range_end = SIZE_MAX;
        size_t element_index = index;
        bool initialized_row = false;
        if (accept(p, "[")) {
          int64_t designated_index = -1;
          int64_t designated_last = -1;
          if (!parse_global_const_expr(p, &p->pos, &designated_index) ||
              designated_index < 0) {
            free(global.bytes);
            free(global.relocs);
            fail(p, "invalid file-scope aggregate-pointer array designator");
            return false;
          }
          designated_last = designated_index;
          bool range_designator = accept(p, "...");
          if (range_designator &&
              (!parse_global_const_expr(p, &p->pos, &designated_last) ||
               designated_last < designated_index)) {
            free(global.bytes);
            free(global.relocs);
            fail(p, "invalid file-scope aggregate-pointer array range");
            return false;
          }
          if ((uint64_t)designated_last >= outer_extent || !expect(p, "]")) {
            free(global.bytes);
            free(global.relocs);
            fail(p, "file-scope aggregate-pointer array designator is out of "
                    "range");
            return false;
          }
          if (range_designator) {
            if (!expect(p, "=")) {
              free(global.bytes);
              free(global.relocs);
              fail(p, "pointer-array range designator requires an initializer");
              return false;
            }
            /*
             * Use the effective rank after the element typedef expands; a
             * per-CPU typedef array has no bracket in the declarator itself.
             */
            size_t range_stride =
                stored_array_rank == 1 ? 1u : (size_t)inner_count;
            element_index = (size_t)designated_index * range_stride;
            range_end = (size_t)designated_last * range_stride +
                        (range_stride ? range_stride - 1u : 0u);
          } else {
            element_index = (size_t)designated_index * (size_t)inner_count;
            bool row_initializer = array_rank == 2 && tok_is(p, p->pos, "=") &&
                                   tok_is(p, p->pos + 1, "{");
            if (row_initializer) {
              if (!expect(p, "=") || !expect(p, "{")) {
                free(global.bytes);
                free(global.relocs);
                return false;
              }
              size_t row_end = element_index + (size_t)inner_count;
              index = element_index;
              while (!tok_is(p, p->pos, "}")) {
                size_t row_element = index;
                if (accept(p, "[")) {
                  int64_t column = -1;
                  if (!parse_global_const_expr(p, &p->pos, &column) ||
                      column < 0 || (uint64_t)column >= inner_count ||
                      !expect(p, "]") || !expect(p, "=")) {
                    free(global.bytes);
                    free(global.relocs);
                    fail(p, "invalid file-scope pointer-array row designator");
                    return false;
                  }
                  row_element = element_index + (size_t)column;
                }
                if (row_element < element_index || row_element >= row_end ||
                    !initialize_data_pointer(
                        p, &global, row_element * element_size, global.bytes)) {
                  free(global.bytes);
                  free(global.relocs);
                  if (!p->err[0])
                    fail(p, "invalid file-scope pointer-array row element");
                  return false;
                }
                index = row_element + 1;
                if (!accept(p, ",") && !tok_is(p, p->pos, "}")) {
                  free(global.bytes);
                  free(global.relocs);
                  fail(p, "expected ',' between pointer-array row elements");
                  return false;
                }
              }
              if (!expect(p, "}")) {
                free(global.bytes);
                free(global.relocs);
                return false;
              }
              index = row_end;
              initialized_row = true;
            } else {
              size_t suffix = (size_t)inner_count;
              for (size_t dimension = 1; dimension < array_rank; ++dimension) {
                if (!accept(p, "[")) {
                  free(global.bytes);
                  free(global.relocs);
                  fail(p, "multidimensional pointer-array designator must "
                          "select a pointer element");
                  return false;
                }
                int64_t subscript = -1;
                if (!parse_global_const_expr(p, &p->pos, &subscript) ||
                    subscript < 0 ||
                    (uint64_t)subscript >= array_extents[dimension] ||
                    !expect(p, "]")) {
                  free(global.bytes);
                  free(global.relocs);
                  fail(p, "multidimensional pointer-array designator is out "
                          "of range");
                  return false;
                }
                suffix /= array_extents[dimension];
                element_index += (size_t)subscript * suffix;
              }
              if (!expect(p, "=")) {
                free(global.bytes);
                free(global.relocs);
                return false;
              }
              index = element_index;
            }
          }
        }
        if (element_index >= total_count) {
          free(global.bytes);
          fail(p, "too many file-scope aggregate-pointer array elements");
          return false;
        }
        if (initialized_row) {
          /*
           * The row initializer consumed all of its pointer elements.
           */
        } else if (range_end != SIZE_MAX) {
          int64_t fill_value = 0;
          if (!parse_global_const_expr(p, &p->pos, &fill_value)) {
            free(global.bytes);
            free(global.relocs);
            fail(p, "file-scope pointer array ranges require a constant value");
            return false;
          }
          if (fill_value != 0) {
            if (!getenv("NYC_KERNEL_COMPAT")) {
              free(global.bytes);
              free(global.relocs);
              fail(p, "file-scope pointer array ranges require a null value");
              return false;
            }
            size_t first_off = element_index * element_size;
            size_t last_off = range_end * element_size;
            if (range_end < element_index ||
                last_off + element_size > global.len) {
              free(global.bytes);
              free(global.relocs);
              fail(p, "file-scope pointer array range is out of bounds");
              return false;
            }
            for (size_t off = first_off; off <= last_off; off += element_size) {
              uint64_t u = (uint64_t)fill_value;
              for (size_t b = 0; b < element_size && b < 8u; ++b)
                global.bytes[off + b] =
                    (unsigned char)((u >> (8u * b)) & 0xffu);
            }
          }
          index = range_end;
        } else if (!initialize_data_pointer(p, &global,
                                            element_index * element_size,
                                            global.bytes)) {
          free(global.bytes);
          free(global.relocs);
          return false;
        }
        if (!initialized_row && range_end == SIZE_MAX)
          index = element_index + 1;
        if (!accept(p, ",") && !tok_is(p, p->pos, "}")) {
          free(global.bytes);
          free(global.relocs);
          fail(p, "expected ',' between aggregate-pointer array elements");
          return false;
        }
      }
      if (!expect(p, "}")) {
        free(global.bytes);
        free(global.relocs);
        return false;
      }
      global.is_tentative = false;
    }
    global.reloc_capacity = global.reloc_count;
    if (compatible_redeclaration) {
      free(previous->bytes);
      free(previous->relocs);
      free(previous->section);
      *previous = global;
    } else {
      p->globals[p->global_count++] = global;
    }
    if (!accept(p, ","))
      break;
  }
  return expect(p, ";");
}

static bool typeof_names_function(c_parser_t *p, size_t type_pos) {
  while (tok_is(p, type_pos, "const") || tok_is(p, type_pos, "volatile") ||
         tok_is(p, type_pos, "static") || tok_is(p, type_pos, "extern") ||
         tok_is(p, type_pos, "register") || tok_is(p, type_pos, "inline"))
    ++type_pos;
  if (!tok_is(p, type_pos, "typeof") && !tok_is(p, type_pos, "__typeof__"))
    return false;
  size_t open = type_pos + 1;
  size_t after = skip_balanced(p, open, "(", ")");
  return tok_is(p, open, "(") && after == open + 3 &&
         tok_ident(p->tokens[open + 1]) &&
         find_function(p, p->tokens[open + 1]) != NULL;
}

/*
 * Consume the incoming storage, retaining one object per C identifier.
 */
static bool record_function_pointer_global(c_parser_t *p, c_global_t global) {
  c_global_t *previous = find_global(p, global.name);
  if (!previous) {
    if (p->global_count >= p->global_capacity) {
      fail(p, "C frontend global data table capacity exceeded");
      goto failed;
    }
    p->globals[p->global_count++] = global;
    return true;
  }
  if (previous->type != C_FUNC_PTR || previous->is_array ||
      previous->function_result_type != global.function_result_type ||
      previous->agg != global.agg || previous->is_const != global.is_const ||
      (previous->section && global.section &&
       strcmp(previous->section, global.section) != 0) ||
      (!global.is_external && !global.is_tentative && !previous->is_external &&
       !previous->is_tentative)) {
    fail(p, "incompatible repeated file-scope function pointer");
    goto failed;
  }
  if (!global.is_external && (previous->is_external || !global.is_tentative)) {
    free(previous->bytes);
    free(previous->relocs);
    previous->name = global.name;
    previous->bytes = global.bytes;
    previous->len = global.len;
    previous->relocs = global.relocs;
    previous->reloc_count = global.reloc_count;
    previous->reloc_capacity = global.reloc_capacity;
    previous->is_external = false;
    previous->is_tentative = global.is_tentative;
    global.bytes = NULL;
    global.relocs = NULL;
  }
  if (global.section && !previous->section) {
    previous->section = global.section;
    global.section = NULL;
  }
  free(global.bytes);
  free(global.relocs);
  free(global.section);
  return true;
failed:
  free(global.bytes);
  free(global.relocs);
  free(global.section);
  return false;
}

static bool try_collect_function_pointer_global(c_parser_t *p, bool *matched) {
  *matched = false;
  size_t declaration_start = p->pos;
  size_t type_pos = p->pos;
  while (tok_is(p, type_pos, "const") || tok_is(p, type_pos, "volatile") ||
         tok_is(p, type_pos, "static") || tok_is(p, type_pos, "extern") ||
         tok_is(p, type_pos, "register") || tok_is(p, type_pos, "inline"))
    ++type_pos;
  {
    c_parser_t alias_probe = *p;
    char alias_error[1] = {0};
    alias_probe.err = alias_error;
    alias_probe.err_len = sizeof(alias_error);
    alias_probe.type_lookahead = true;
    c_type_t alias_type;
    if (parse_type(&alias_probe, &alias_type, true) &&
        alias_type == C_FUNC_PTR &&
        !alias_probe.last_type_is_function_type_alias &&
        !typeof_names_function(p, p->pos) &&
        tok_ident(*current(&alias_probe)) &&
        !tok_is(&alias_probe, alias_probe.pos + 1, "(")) {
      ny_ctok_t name = *current(&alias_probe);
      bool is_const = alias_probe.last_type_const;
      ++alias_probe.pos;
      ny_ctok_t section = {0};
      if (!parse_trailing_file_scope_attributes(&alias_probe, &section) ||
          (!tok_is(&alias_probe, alias_probe.pos, "=") &&
           !tok_is(&alias_probe, alias_probe.pos, ";")))
        return true;
      *matched = true;
      c_type_t pointer_type;
      if (!parse_type(p, &pointer_type, true) || pointer_type != C_FUNC_PTR)
        return false;
      c_type_t result_type = p->last_type_function_result_type;
      c_agg_type_t *result_agg = p->last_type_function_result_agg;
      section = p->last_type_section;
      if (!parse_name(p, &name))
        return false;
      ny_ctok_t declarator_section = {0};
      if (!parse_trailing_file_scope_attributes(p, &declarator_section))
        return false;
      if (declarator_section.len)
        section = declarator_section;
      bool is_external = false;
      for (size_t i = declaration_start; i < p->pos; ++i)
        is_external |= tok_is(p, i, "extern");
      unsigned char *bytes = calloc(1, sizeof(uint64_t));
      if (!bytes) {
        fail(p, "out of memory allocating file-scope function pointer");
        return false;
      }
      c_global_reloc_t *relocs = NULL;
      size_t reloc_count = 0;
      bool has_initializer = accept(p, "=");
      if (has_initializer) {
        size_t target_pos = p->pos;
        size_t parentheses = 0;
        while (tok_is(p, target_pos + parentheses, "("))
          ++parentheses;
        size_t function_pos = target_pos + parentheses;
        if (tok_is(p, function_pos, "&"))
          ++function_pos;
        ny_ctok_t target = p->tokens[function_pos];
        bool balanced = true;
        for (size_t i = 0; i < parentheses; ++i)
          balanced &= tok_is(p, function_pos + 1 + i, ")");
        bool has_cast = false;
        if (!tok_ident(target) || !balanced) {
          /*
           * Try to accept casts like (type)&func or (type)func in kernel compat
           */
          if (getenv("NYC_KERNEL_COMPAT") && parentheses > 0) {
            has_cast = true; /* best-effort: scan for ident after & inside/after
                                parens */
          }
        }
        if (is_external || (!tok_ident(target) && !has_cast) ||
            (!balanced && !has_cast)) {
          free(bytes);
          fail(p, "function-pointer initializer requires a function symbol");
          return false;
        }
        if (has_cast && !tok_ident(target)) {
          /*
           * scan forward for a plausible function symbol
           */
          for (size_t s = target_pos; s < p->token_count && s < target_pos + 32;
               ++s) {
            if (tok_ident(p->tokens[s]) &&
                (tok_is(p, s + 1, ";") || tok_is(p, s + 1, ")"))) {
              target = p->tokens[s];
              function_pos = s;
              balanced = true;
              break;
            }
            if (tok_is(p, s, "&") && tok_ident(p->tokens[s + 1])) {
              target = p->tokens[s + 1];
              function_pos = s + 1;
              balanced = true;
              break;
            }
          }
        }
        if (!tok_ident(target)) {
          free(bytes);
          fail(p, "function-pointer initializer requires a function symbol");
          return false;
        }
        relocs = calloc(1, sizeof(*relocs));
        if (!relocs) {
          free(bytes);
          fail(p, "out of memory allocating function-pointer relocation");
          return false;
        }
        relocs[0] = (c_global_reloc_t){.offset = 0, .target = target};
        reloc_count = 1;
        p->pos = function_pos + 1 + parentheses;
      }
      if (!expect(p, ";")) {
        free(bytes);
        free(relocs);
        return false;
      }
      char *section_name = NULL;
      if (section.len) {
        size_t section_len = section.len - 2;
        section_name = malloc(section_len + 1);
        if (!section_name) {
          free(bytes);
          free(relocs);
          fail(p, "out of memory storing a function-pointer data section");
          return false;
        }
        memcpy(section_name, section.start + 1, section_len);
        section_name[section_len] = '\0';
      }
      c_global_t global = {.name = name,
                           .bytes = bytes,
                           .len = sizeof(uint64_t),
                           .relocs = relocs,
                           .reloc_count = reloc_count,
                           .reloc_capacity = reloc_count,
                           .type = C_FUNC_PTR,
                           .function_result_type = result_type,
                           .agg = result_agg,
                           .is_const = is_const,
                           .is_external = is_external,
                           .is_tentative = !is_external && !has_initializer,
                           .section = section_name};
      return record_function_pointer_global(p, global);
    }
  }
  c_parser_t probe = *p;
  char probe_error[1] = {0};
  probe.err = probe_error;
  probe.err_len = sizeof(probe_error);
  probe.type_lookahead = true;
  c_type_t result_type;
  {
    bool tp_ok = parse_type(&probe, &result_type, true);
    bool open_ok = tp_ok && tok_is(&probe, probe.pos, "(");
    bool star_ok = open_ok && tok_is(&probe, probe.pos + 1, "*");
    if (!(tp_ok && open_ok && star_ok))
      return true;
  }
  size_t name_pos = probe.pos + 2;
  bool is_external = false;
  bool is_const = false;
  while (tok_is(&probe, name_pos, "const") ||
         tok_is(&probe, name_pos, "volatile")) {
    is_const |= tok_is(&probe, name_pos, "const");
    ++name_pos;
  }
  probe.pos = name_pos;
  ny_ctok_t probe_section = {0};
  if (!parse_trailing_file_scope_attributes(&probe, &probe_section))
    return true;
  name_pos = probe.pos;
  bool pointer_array =
      tok_ident(probe.tokens[name_pos]) && tok_is(&probe, name_pos + 1, "[") &&
      tok_is(&probe, name_pos + 2, "]") && tok_is(&probe, name_pos + 3, ")") &&
      tok_is(&probe, name_pos + 4, "(");
  size_t params_open = pointer_array ? name_pos + 4 : name_pos + 2;
  if (!tok_ident(probe.tokens[name_pos]) ||
      (!pointer_array && (!tok_is(&probe, name_pos + 1, ")") ||
                          !tok_is(&probe, name_pos + 2, "("))) ||
      (pointer_array && !tok_is(&probe, name_pos + 4, "(")))
    return true;
  size_t params_end = skip_balanced(&probe, params_open, "(", ")");
  if (params_end >= probe.token_count)
    return true;
  probe.pos = params_end;
  ny_ctok_t trailing_probe_section = {0};
  if (!parse_trailing_file_scope_attributes(&probe, &trailing_probe_section) ||
      (pointer_array ? !tok_is(&probe, probe.pos, ";")
                     : (!tok_is(&probe, probe.pos, "=") &&
                        !tok_is(&probe, probe.pos, ";"))))
    return true;
  for (size_t i = p->pos; i < probe.pos; ++i)
    is_external |= tok_is(p, i, "extern");

  *matched = true;
  if (!parse_type(p, &result_type, true))
    return false;
  ny_ctok_t declaration_section = p->last_type_section;
  if (!expect(p, "(") || !expect(p, "*")) {
    fail(p, "malformed file-scope function-pointer declaration");
    return false;
  }
  while (accept(p, "const") || accept(p, "volatile"))
    ;
  ny_ctok_t declarator_section = {0};
  if (!parse_trailing_file_scope_attributes(p, &declarator_section))
    return false;
  ny_ctok_t name;
  if (!parse_name(p, &name))
    return false;
  if (pointer_array) {
    if (!expect(p, "[") || !expect(p, "]") || !expect(p, ")") ||
        !expect(p, "(")) {
      fail(p, "malformed file-scope function-pointer array declarator");
      return false;
    }
  } else if (!expect(p, ")") || !expect(p, "(")) {
    fail(p, "malformed file-scope function-pointer declarator");
    return false;
  }
  size_t close = skip_balanced(p, p->pos - 1, "(", ")");
  if (close >= p->token_count) {
    fail(p, "unterminated file-scope function-pointer parameters");
    return false;
  }
  p->pos = close;
  ny_ctok_t trailing_section = {0};
  if (!parse_trailing_file_scope_attributes(p, &trailing_section))
    return false;
  if (pointer_array) {
    if (!expect(p, ";"))
      return false;
    c_global_t *existing = find_global(p, name);
    if (existing) {
      if (!existing->is_array || !existing->is_pointer_array ||
          existing->type != C_FUNC_PTR || existing->is_const != is_const ||
          (!existing->array_incomplete && existing->array_rank != 1)) {
        fail(p, "conflicting file-scope function-pointer array declaration");
        return false;
      }
      return true;
    }
    if (p->global_count >= p->global_capacity) {
      fail(p, "C frontend global data table capacity exceeded");
      return false;
    }
    p->globals[p->global_count++] = (c_global_t){
        .name = name,
        .type = C_FUNC_PTR,
        .len = 0,
        .array_rank = 1,
        .array_shape = {.rank = 1,
                        .element_size = c_type_size(p->abi, C_VOID_PTR),
                        .element_type = C_FUNC_PTR},
        .is_array = true,
        .is_pointer_array = true,
        .is_function_pointer_array = true,
        .array_incomplete = true,
        .is_const = is_const,
        .is_external = is_external,
        .is_tentative = !is_external,
        .function_result_type = result_type};
    return true;
  }
  ny_ctok_t section = trailing_section.len     ? trailing_section
                      : declarator_section.len ? declarator_section
                                               : declaration_section;
  size_t size = sizeof(uint64_t);
  unsigned char *bytes = calloc(size, 1);
  c_global_reloc_t *relocs = NULL;
  size_t reloc_count = 0;
  if (!bytes) {
    fail(p, "out of memory allocating file-scope function pointer");
    return false;
  }
  bool has_initializer = accept(p, "=");
  if (has_initializer) {
    c_parser_t null_probe = *p;
    char null_probe_error[1] = {0};
    null_probe.err = null_probe_error;
    null_probe.err_len = sizeof(null_probe_error);
    int64_t null_value = 1;
    bool null_initializer =
        parse_global_const_expr(&null_probe, &null_probe.pos, &null_value) &&
        null_value == 0 && tok_is(&null_probe, null_probe.pos, ";");
    if (null_initializer) {
      p->pos = null_probe.pos;
    } else {
      size_t target_pos = p->pos;
      size_t parentheses = 0;
      while (tok_is(p, target_pos + parentheses, "("))
        ++parentheses;
      size_t function_pos = target_pos + parentheses;
      if (tok_is(p, function_pos, "&"))
        ++function_pos;
      ny_ctok_t target = p->tokens[function_pos];
      bool balanced_parentheses = true;
      for (size_t i = 0; i < parentheses; ++i)
        balanced_parentheses &= tok_is(p, function_pos + 1 + i, ")");
      if (is_external || !tok_ident(target) || !balanced_parentheses) {
        free(bytes);
        fail(p, "function-pointer initializer requires a function symbol");
        return false;
      }
      relocs = calloc(1, sizeof(*relocs));
      if (!relocs) {
        free(bytes);
        fail(p, "out of memory allocating function-pointer relocation");
        return false;
      }
      relocs[0] = (c_global_reloc_t){.offset = 0, .target = target};
      reloc_count = 1;
      p->pos = function_pos + 1 + parentheses;
    }
  }
  if (!expect(p, ";")) {
    free(bytes);
    free(relocs);
    return false;
  }
  char *section_name = NULL;
  if (section.len) {
    size_t section_len = section.len - 2;
    section_name = malloc(section_len + 1);
    if (!section_name) {
      free(bytes);
      free(relocs);
      fail(p, "out of memory storing a function-pointer data section");
      return false;
    }
    memcpy(section_name, section.start + 1, section_len);
    section_name[section_len] = '\0';
  }
  return record_function_pointer_global(
      p, (c_global_t){.name = name,
                      .bytes = bytes,
                      .len = size,
                      .relocs = relocs,
                      .reloc_count = reloc_count,
                      .reloc_capacity = reloc_count,
                      .type = C_FUNC_PTR,
                      .function_result_type = result_type,
                      .is_const = is_const,
                      .is_external = is_external,
                      .is_tentative = !is_external && !has_initializer,
                      .section = section_name});
}

static bool valid_named_object_segment(c_parser_t *p, unsigned char segment,
                                       c_type_t type, bool array) {
  if (!segment || (!array && (c_type_is_integer(type) || type == C_F64) &&
                   (p->abi == NYC_ABI_SYSV_X86_64 || p->abi == NYC_ABI_I386)))
    return true;
  fail(p, "segment-qualified named C object requires a supported x86 scalar");
  return false;
}

static bool
record_uninitialized_scalar_global(c_parser_t *p, ny_ctok_t name, c_type_t type,
                                   size_t size, bool is_external, bool is_const,
                                   c_type_t function_result_type,
                                   c_agg_type_t *function_result_agg) {
  bool pointer = c_type_is_pointer(type);
  unsigned char object_seg = pointer ? C_SEG_NONE : p->last_type_seg;
  if (!valid_named_object_segment(p, object_seg, type, p->last_type_is_array))
    return false;
  c_global_t *previous = find_global(p, name);
  if (previous) {
    if (previous->type != type || previous->is_const != is_const ||
        previous->object_seg != object_seg ||
        (type == C_FUNC_PTR &&
         previous->function_result_type != function_result_type)) {
      fail(p, "incompatible repeated file-scope scalar declaration");
      return false;
    }
    if (!is_external && previous->is_external) {
      unsigned char *bytes = size ? calloc(size, 1) : NULL;
      if (size && !bytes) {
        fail(p, "out of memory allocating C global data");
        return false;
      }
      previous->bytes = bytes;
      previous->len = size;
      previous->is_external = false;
      previous->is_tentative = true;
      previous->function_result_type = function_result_type;
      previous->agg = function_result_agg;
    }
    return true;
  }
  if (p->global_count >= p->global_capacity) {
    fail(p, "C frontend global data table capacity exceeded");
    return false;
  }
  unsigned char *bytes = is_external || !size ? NULL : calloc(size, 1);
  if (!is_external && size && !bytes) {
    fail(p, "out of memory allocating C global data");
    return false;
  }
  p->globals[p->global_count++] =
      (c_global_t){.name = name,
                   .bytes = bytes,
                   .len = is_external ? 0 : size,
                   .type = type,
                   .object_seg = object_seg,
                   .pointee_seg = pointer ? p->last_type_seg : C_SEG_NONE,
                   .is_volatile = p->last_type_volatile &&
                                  !c_type_is_arithmetic_pointer(type),
                   .is_const = is_const,
                   .function_result_type = function_result_type,
                   .agg = function_result_agg,
                   .is_external = is_external,
                   .is_tentative = !is_external};
  return true;
}

static bool try_collect_uninitialized_scalar_global_list(c_parser_t *p,
                                                         bool *matched) {
  *matched = false;
  c_parser_t probe = *p;
  char probe_error[1] = {0};
  probe.err = probe_error;
  probe.err_len = sizeof(probe_error);
  probe.type_lookahead = true;
  c_type_t type;
  if (!parse_type(&probe, &type, true) ||
      (type != C_I32 && type != C_U32 && type != C_U8 && type != C_CHAR &&
       type != C_I16 && type != C_U16 && type != C_I64 && type != C_U64 &&
       !c_type_is_pointer(type)))
    return true;
  ny_ctok_t ignored_section = {0};
  if (!parse_trailing_file_scope_attributes(&probe, &ignored_section) ||
      !parse_parenthesized_declarator_name(&probe, &(ny_ctok_t){0}))
    return true;
  ny_ctok_t ignored_register = {0};
  if (!try_parse_asm_label(&probe, &ignored_register) ||
      !parse_trailing_file_scope_attributes(&probe, &ignored_section) ||
      !tok_is(&probe, probe.pos, ","))
    return true;
  for (;;) {
    ++probe.pos;
    if (c_type_is_pointer(type)) {
      if (!accept(&probe, "*"))
        return true;
      while (accept(&probe, "const") || accept(&probe, "volatile")) {
      }
    }
    if (!parse_parenthesized_declarator_name(&probe, &(ny_ctok_t){0}))
      return true;
    if (!try_parse_asm_label(&probe, &ignored_register) ||
        !parse_trailing_file_scope_attributes(&probe, &ignored_section))
      return true;
    if (tok_is(&probe, probe.pos, "="))
      return true;
    if (!tok_is(&probe, probe.pos, ","))
      break;
  }
  if (!tok_is(&probe, probe.pos, ";"))
    return true;

  *matched = true;
  bool is_external = false;
  for (size_t i = p->pos; i < probe.pos; ++i)
    is_external |= tok_is(p, i, "extern");
  c_type_t parsed_type;
  if (!parse_type(p, &parsed_type, true))
    return false;
  c_type_t function_result_type = p->last_type_function_result_type;
  c_agg_type_t *function_result_agg = p->last_type_function_result_agg;
  ny_ctok_t section = {0};
  if (!parse_trailing_file_scope_attributes(p, &section))
    return false;
  size_t size = c_type_size(p->abi, type);
  for (;;) {
    if (accept(p, ",")) {
      if (c_type_is_pointer(type) && !expect(p, "*"))
        return false;
      while (accept(p, "const") || accept(p, "volatile")) {
      }
      ny_ctok_t name;
      if (!parse_parenthesized_declarator_name(p, &name))
        return false;
      ny_ctok_t asm_label = {0};
      ny_ctok_t next_section = {0};
      if (!try_parse_asm_label(p, &asm_label) ||
          !parse_trailing_file_scope_attributes(p, &next_section))
        return false;
      if (asm_label.len || next_section.len) {
        fail(p, "attributes on later scalar declarators are unsupported");
        return false;
      }
      if (!record_uninitialized_scalar_global(
              p, name, type, size, is_external, p->last_type_const,
              function_result_type, function_result_agg))
        return false;
    } else {
      ny_ctok_t first_name;
      if (!parse_parenthesized_declarator_name(p, &first_name))
        return false;
      ny_ctok_t asm_label = {0};
      ny_ctok_t first_section = {0};
      if (!try_parse_asm_label(p, &asm_label) ||
          !parse_trailing_file_scope_attributes(p, &first_section))
        return false;
      if (asm_label.len || (section.len && first_section.len)) {
        fail(p, "attributes on scalar declaration lists are unsupported");
        return false;
      }
      if (!record_uninitialized_scalar_global(
              p, first_name, type, size, is_external, p->last_type_const,
              function_result_type, function_result_agg))
        return false;
    }
    if (!tok_is(p, p->pos, ","))
      break;
  }
  return expect(p, ";");
}

static bool try_collect_initialized_scalar_global_list(c_parser_t *p,
                                                       bool *matched) {
  *matched = false;
  c_parser_t probe = *p;
  char probe_error[1] = {0};
  probe.err = probe_error;
  probe.err_len = sizeof(probe_error);
  probe.type_lookahead = true;
  for (size_t i = probe.pos;
       tok_is(&probe, i, "const") || tok_is(&probe, i, "extern") ||
       tok_is(&probe, i, "register");
       ++i)
    if (tok_is(&probe, i, "extern"))
      return true;
  c_type_t type;
  if (!parse_type(&probe, &type, true) ||
      (!c_type_is_integer(type) && !c_type_is_pointer(type)) || type == C_AGG ||
      type == C_BAD || probe.last_type_is_array)
    return true;
  bool is_const = probe.last_type_const;
  ny_ctok_t ignored_section = {0};
  ny_ctok_t ignored_asm_label = {0};
  bool multiple = false;
  for (;;) {
    if (!parse_trailing_file_scope_attributes(&probe, &ignored_section) ||
        !parse_parenthesized_declarator_name(&probe, &(ny_ctok_t){0}))
      return true;
    if (!try_parse_asm_label(&probe, &ignored_asm_label) ||
        !parse_trailing_file_scope_attributes(&probe, &ignored_section))
      return true;
    if (accept(&probe, "=")) {
      while (probe.pos < probe.token_count && !tok_is(&probe, probe.pos, ",") &&
             !tok_is(&probe, probe.pos, ";")) {
        const char *left = tok_is(&probe, probe.pos, "(")   ? "("
                           : tok_is(&probe, probe.pos, "{") ? "{"
                           : tok_is(&probe, probe.pos, "[") ? "["
                                                            : NULL;
        if (left) {
          size_t after = skip_balanced(&probe, probe.pos, left,
                                       left[0] == '('   ? ")"
                                       : left[0] == '{' ? "}"
                                                        : "]");
          if (after <= probe.pos || after >= probe.token_count)
            return true;
          probe.pos = after;
        } else {
          ++probe.pos;
        }
      }
    }
    if (!accept(&probe, ","))
      break;
    multiple = true;
    unsigned levels = 0;
    bool pointer_const = false;
    if (!parse_type_pointers(&probe, &levels, &pointer_const))
      return true;
  }
  if (!multiple || !tok_is(&probe, probe.pos, ";"))
    return true;
  *matched = true;
  c_type_t parsed_type;
  if (!parse_type(p, &parsed_type, true))
    return false;
  c_type_t base_type = p->last_type_base_type;
  c_agg_type_t *agg = p->last_type_agg;
  c_type_t function_result_type = p->last_type_function_result_type;
  bool base_const = p->last_type_const;
  bool base_volatile = p->last_type_volatile;
  unsigned char base_seg = p->last_type_seg;
  bool is_long_long = p->last_type_long_long;
  unsigned pointer_depth = p->last_type_pointer_count;
  type = parsed_type;
  is_const = c_type_is_pointer(type) ? p->last_type_const_pointer : base_const;
  if (!parse_trailing_file_scope_attributes(p, &ignored_section))
    return false;
  bool first = true;
  do {
    if (!first) {
      bool pointer_const = false;
      if (!parse_type_pointers(p, &pointer_depth, &pointer_const))
        return false;
      type = c_add_pointer_levels(base_type, pointer_depth);
      is_const = c_type_is_pointer(type) ? pointer_const : base_const;
    }
    first = false;
    bool pointer = c_type_is_pointer(type);
    unsigned char object_seg = pointer ? C_SEG_NONE : base_seg;
    if (!valid_named_object_segment(p, object_seg, type, p->last_type_is_array))
      return false;
    size_t size = c_type_size(p->abi, type);
    if (!size || size > sizeof(uint64_t)) {
      fail(p, "initialized scalar declaration list has unsupported type");
      return false;
    }
    ny_ctok_t name;
    if (!parse_parenthesized_declarator_name(p, &name))
      return false;
    ny_ctok_t asm_label = {0};
    ny_ctok_t section = {0};
    if (!try_parse_asm_label(p, &asm_label) ||
        !parse_trailing_file_scope_attributes(p, &section) || asm_label.len ||
        section.len) {
      if (!p->err[0])
        fail(p, "attributes on initialized scalar declaration lists are "
                "unsupported");
      return false;
    }
    bool has_initializer = accept(p, "=");
    c_global_t *previous = find_global(p, name);
    if (previous && previous->object_seg != object_seg) {
      fail(p, "incompatible segment qualifier on C object redeclaration");
      return false;
    }
    if (previous && !has_initializer && !previous->is_external) {
      if (previous->type != type || previous->agg != agg ||
          previous->is_const != is_const) {
        fail(p, "incompatible initialized scalar declaration");
        return false;
      }
      continue;
    }
    unsigned char *bytes = calloc(size, 1);
    if (!bytes) {
      fail(p, "out of memory allocating initialized scalar declaration");
      return false;
    }
    c_global_t initializer = {.name = name};
    if (has_initializer) {
      if (c_type_is_pointer(type)) {
        if (!initialize_data_pointer(p, &initializer, 0, bytes)) {
          free(bytes);
          free(initializer.relocs);
          return false;
        }
      } else if (type == C_F32 || type == C_F64) {
        double value = 0;
        if (!parse_global_float_add(p, &p->pos, &value)) {
          free(bytes);
          fail(p, "invalid scalar declaration list floating initializer");
          return false;
        }
        if (type == C_F32) {
          float narrowed = (float)value;
          memcpy(bytes, &narrowed, sizeof(narrowed));
        } else {
          memcpy(bytes, &value, sizeof(value));
        }
      } else {
        int64_t value = 0;
        if (!parse_global_const_expr(p, &p->pos, &value)) {
          free(bytes);
          fail(p,
               "initialized scalar declaration list requires constant values");
          return false;
        }
        memcpy(bytes, &value, size);
      }
    }
    if (previous && previous->is_external && previous->type == type &&
        previous->agg == agg && previous->is_const == is_const) {
      previous->bytes = bytes;
      previous->len = size;
      previous->relocs = initializer.relocs;
      previous->reloc_count = initializer.reloc_count;
      previous->reloc_capacity = initializer.reloc_capacity;
      previous->is_external = false;
      previous->is_tentative = !has_initializer;
    } else if (previous && previous->is_tentative && previous->type == type &&
               previous->agg == agg && previous->is_const == is_const) {
      if (has_initializer) {
        free(previous->bytes);
        free(previous->relocs);
        previous->bytes = bytes;
        previous->len = size;
        previous->relocs = initializer.relocs;
        previous->reloc_count = initializer.reloc_count;
        previous->reloc_capacity = initializer.reloc_capacity;
        previous->is_tentative = false;
      } else {
        free(bytes);
        free(initializer.relocs);
      }
    } else if (previous) {
      free(bytes);
      free(initializer.relocs);
      fail(p, "duplicate or incompatible initialized scalar declaration");
      return false;
    } else {
      if (p->global_count >= p->global_capacity) {
        free(bytes);
        free(initializer.relocs);
        fail(p, "C frontend global data table capacity exceeded");
        return false;
      }
      p->globals[p->global_count++] = (c_global_t){
          .name = name,
          .bytes = bytes,
          .len = size,
          .relocs = initializer.relocs,
          .reloc_count = initializer.reloc_count,
          .reloc_capacity = initializer.reloc_capacity,
          .type = type,
          .object_seg = object_seg,
          .pointee_seg = pointer ? base_seg : C_SEG_NONE,
          .pointer_base_type = base_type,
          .pointer_depth = pointer_depth,
          .function_result_type = function_result_type,
          .agg = agg,
          .is_long_long = is_long_long,
          .is_volatile = base_volatile && !c_type_is_arithmetic_pointer(type),
          .is_const = is_const,
          .is_tentative = !has_initializer};
    }
  } while (accept(p, ","));
  return expect(p, ";");
}

static bool try_collect_scalar_global(c_parser_t *p, bool *matched) {
  *matched = false;
  p->last_function_alias = (ny_ctok_t){0};
  bool is_external = false;
  size_t type_pos = p->pos;
  for (; tok_is(p, type_pos, "const") || tok_is(p, type_pos, "extern") ||
         tok_is(p, type_pos, "register");
       ++type_pos)
    is_external |= tok_is(p, type_pos, "extern");
  c_parser_t probe = *p;
  char probe_error[1] = {0};
  probe.err = probe_error;
  probe.err_len = sizeof(probe_error);
  probe.type_lookahead = true;
  c_type_t type;
  if (!parse_type(&probe, &type, true))
    return true;
  bool function_type_alias = probe.last_type_is_function_type_alias;
  if (type == C_AGG && !is_external)
    return true;
  if (probe.last_type_enum_incomplete && !probe.last_type_pointer_count &&
      !is_external)
    return true;
  ny_ctok_t probe_pre_name_section = {0};
  if (!parse_trailing_file_scope_attributes(&probe, &probe_pre_name_section) ||
      !parse_parenthesized_declarator_name(&probe, &(ny_ctok_t){0}))
    return true;
  /*
   * A `typeof(fn)` base type makes the declarator a function declaration
   * (`extern typeof(f) g;`), not data; the signature collector handles it.
   */
  if (probe.last_type_typeof_function.len || typeof_names_function(p, p->pos))
    return true;
  ny_ctok_t probe_register = {0};
  if (!try_parse_asm_label(&probe, &probe_register))
    return true;
  ny_ctok_t ignored_section = {0};
  if (!parse_trailing_file_scope_attributes(&probe, &ignored_section))
    return true;
  if (!tok_is(&probe, probe.pos, ";") && !tok_is(&probe, probe.pos, "=") &&
      !tok_is(&probe, probe.pos, ","))
    return true;
  /*
   * Non-extern comma-separated declarator lists are handled by the dedicated
   * scalar list collectors; this path only supports single declarators (and
   * extern comma lists handled later).
   */
  if (tok_is(&probe, probe.pos, ",") && !is_external)
    return true;
  *matched = true;
  if (type != C_I32 && type != C_U32 && type != C_U8 && type != C_CHAR &&
      type != C_I16 && type != C_U16 && type != C_I64 && type != C_U64 &&
      type != C_F64 && type != C_AGG && type != C_BAD &&
      !c_type_is_complex(type) && !c_type_is_pointer(type)) {
    fail(p, "unsupported file-scope C object type");
    return false;
  }
  c_type_t parsed_type;
  if (!parse_type(p, &parsed_type, true))
    return false;
  function_type_alias = p->last_type_is_function_type_alias;
  c_type_t function_result_type = p->last_type_function_result_type;
  c_agg_type_t *function_result_agg = p->last_type_function_result_agg;
  ny_ctok_t pre_name_section = {0};
  if (!parse_trailing_file_scope_attributes(p, &pre_name_section))
    return false;
  c_agg_type_t *type_agg =
      function_type_alias ? function_result_agg : p->last_type_agg;
  c_type_t pointer_base_type = p->last_type_base_type;
  unsigned pointer_depth = p->last_type_pointer_count;
  bool is_long_long = p->last_type_long_long;
  bool global_is_volatile = p->last_type_volatile;
  bool typedef_array = p->last_type_is_array;
  unsigned char pointee_seg =
      c_type_is_pointer(type) ? p->last_type_seg : C_SEG_NONE;
  unsigned char object_seg =
      c_type_is_pointer(type) ? C_SEG_NONE : p->last_type_seg;
  if (!valid_named_object_segment(p, object_seg, type, typedef_array))
    return false;
  size_t typedef_array_size = p->last_type_array_size;
  c_array_shape_t typedef_array_shape = p->last_type_array;
  bool is_const =
      c_type_is_pointer(type) ? p->last_type_const_pointer : p->last_type_const;
  ny_ctok_t name;
  if (!parse_parenthesized_declarator_name(p, &name))
    return false;
  ny_ctok_t asm_register = {0};
  if (!try_parse_asm_label(p, &asm_register))
    return false;
  ny_ctok_t trailing_section = {0};
  if (!parse_trailing_file_scope_attributes(p, &trailing_section))
    return false;
  ny_ctok_t alias_target = p->last_function_alias;
  p->last_function_alias = (ny_ctok_t){0};
  ny_ctok_t section_token = p->last_type_section.len ? p->last_type_section
                            : pre_name_section.len   ? pre_name_section
                                                     : trailing_section;
  size_t alignment = p->last_type_alignment;
  char section_copy[64] = {0};
  if (section_token.len &&
      !copy_quoted_token(p, section_token, section_copy, sizeof(section_copy)))
    return false;
  bool is_global_register = asm_register.len != 0;
  if (is_global_register && asm_register.start[0] != '"') {
    fail(p, "asm label on a C declaration must be a string literal");
    return false;
  }
  if (type == C_BAD && is_external && !is_global_register) {
    for (;;) {
      c_global_t *previous = find_global(p, name);
      if (previous) {
        if (previous->type != C_BAD || !previous->is_external) {
          fail(p, "incompatible declaration of external void symbol");
          return false;
        }
      } else {
        if (p->global_count >= p->global_capacity) {
          fail(p, "C frontend global data table capacity exceeded");
          return false;
        }
        p->globals[p->global_count++] =
            (c_global_t){.name = name, .type = C_BAD, .is_external = true};
      }
      if (!accept(p, ","))
        break;
      if (!parse_name(p, &name) || !try_parse_asm_label(p, &asm_register) ||
          !parse_trailing_file_scope_attributes(p, &trailing_section))
        return false;
    }
    return expect(p, ";");
  }
  if (is_external && !is_global_register && tok_is(p, p->pos, ",")) {
    for (;;) {
      c_global_t *previous = find_global(p, name);
      if (previous) {
        if (previous->type != type || previous->agg != type_agg ||
            !previous->is_external || previous->is_const != is_const ||
            previous->object_seg != object_seg) {
          fail(p, "incompatible declaration of external C symbol");
          return false;
        }
      } else {
        if (p->global_count >= p->global_capacity) {
          fail(p, "C frontend global data table capacity exceeded");
          return false;
        }
        p->globals[p->global_count++] = (c_global_t){
            .name = name,
            .type = type,
            .object_seg = object_seg,
            .pointee_seg = pointee_seg,
            .is_volatile =
                global_is_volatile && !c_type_is_arithmetic_pointer(type),
            .agg = type_agg,
            .is_array = typedef_array,
            .array_rank = typedef_array ? typedef_array_shape.rank : 0,
            .array_stride =
                typedef_array ? typedef_array_shape.element_size : 0,
            .array_shape =
                typedef_array ? typedef_array_shape : (c_array_shape_t){0},
            .is_const = is_const,
            .is_external = true};
      }
      if (!accept(p, ","))
        break;
      if (!parse_name(p, &name) || !try_parse_asm_label(p, &asm_register) ||
          !parse_trailing_file_scope_attributes(p, &trailing_section))
        return false;
      if (tok_is(p, p->pos, "=")) {
        fail(p, "extern C globals cannot have initializers");
        return false;
      }
    }
    return expect(p, ";");
  }
  size_t size = typedef_array ? typedef_array_size : c_type_size(p->abi, type);
  if (!size) {
    fail(p, "file-scope C object has no supported storage size");
    return false;
  }
  unsigned char *bytes = calloc(size, 1);
  c_global_reloc_t *relocs = NULL;
  size_t reloc_count = 0;
  if (!bytes) {
    fail(p, "out of memory allocating C global data");
    return false;
  }
  bool has_initializer = accept(p, "=");
  if (alias_target.len &&
      (!is_external || has_initializer || !tok_is(p, p->pos, ";"))) {
    free(bytes);
    fail(p, "C object alias must be a single external declaration");
    return false;
  }
  if (is_global_register && has_initializer) {
    free(bytes);
    fail(p, "global register variables cannot have initializers");
    return false;
  }
  if (has_initializer) {
    if (typedef_array) {
      if (is_external) {
        free(bytes);
        fail(p, "extern C global declarations cannot have initializers");
        return false;
      }
      if (!typedef_array_shape.rank || !c_type_is_integer(type) ||
          typedef_array_shape.element_type != type ||
          !parse_global_integer_array_initializer(
              p, type, bytes, typedef_array_shape.extent,
              typedef_array_shape.rank, 0, 0, size)) {
        free(bytes);
        if (!p->err[0])
          fail(
              p,
              "file-scope typedef-array initializer requires integer elements");
        return false;
      }
    } else if (is_external) {
      free(bytes);
      fail(p, "extern C global declarations cannot have initializers");
      return false;
    } else if (c_type_is_pointer(type)) {
      c_global_t initializer = {.name = name};
      if (!initialize_data_pointer(p, &initializer, 0, bytes)) {
        free(bytes);
        free(initializer.relocs);
        return false;
      }
      relocs = initializer.relocs;
      reloc_count = initializer.reloc_count;
    } else {
      if (type == C_F64) {
        double value;
        if (!parse_global_float_add(p, &p->pos, &value)) {
          free(bytes);
          fail(p, "invalid file-scope double initializer");
          return false;
        }
        memcpy(bytes, &value, sizeof(value));
      } else {
        c_global_t address_data = {.relocs = relocs,
                                   .reloc_count = reloc_count,
                                   .reloc_capacity = reloc_count};
        bool integer_address = false;
        if (c_type_is_integer(type) &&
            !initialize_data_integer_address(p, &address_data, 0, size, bytes,
                                             &integer_address)) {
          free(bytes);
          free(address_data.relocs);
          return false;
        }
        relocs = address_data.relocs;
        reloc_count = address_data.reloc_count;
        if (!integer_address) {
          int64_t value;
          bool scalar_brace_initializer =
              c_type_is_integer(type) && accept(p, "{");
          bool value_valid = parse_global_const_expr(p, &p->pos, &value);
          if (value_valid && scalar_brace_initializer) {
            (void)accept(p, ",");
            value_valid = expect(p, "}");
          }
          if (!value_valid) {
            char message[192];
            snprintf(message, sizeof(message),
                     "file-scope C global `%.*s` initializer is not constant "
                     "near `%.48s`",
                     (int)(name.len > 48 ? 48 : name.len), name.start,
                     p->tokens[p->pos].start ? p->tokens[p->pos].start : "");
            free(bytes);
            free(relocs);
            fail(p, message);
            return false;
          }
          memcpy(bytes, &value, size);
        }
      }
    }
  }
  if (!expect(p, ";")) {
    free(bytes);
    free(relocs);
    return false;
  }
  c_global_t *previous = find_global(p, name);
  if (previous && previous->alignment < alignment)
    previous->alignment = alignment;
  if (is_global_register || (previous && previous->has_asm_register)) {
    /*
     * Global register bindings must be declared identically everywhere
     * (GCC reserves the register for the whole TU); any other shape of a
     * declaration for the same name is rejected.
     */
    if (previous && (!previous->has_asm_register || !previous->is_external ||
                     !is_global_register ||
                     !tok_same(previous->asm_register, asm_register) ||
                     previous->type != type || previous->agg != type_agg ||
                     previous->is_const != is_const)) {
      free(bytes);
      free(relocs);
      fail(p, "inconsistent declaration of global register variable");
      return false;
    }
    if (!previous) {
      if (p->global_count >= p->global_capacity) {
        free(bytes);
        free(relocs);
        fail(p, "C frontend global data table capacity exceeded");
        return false;
      }
      p->globals[p->global_count++] =
          (c_global_t){.name = name,
                       .type = type,
                       .agg = type_agg,
                       .is_const = is_const,
                       .is_external = true,
                       .has_asm_register = true,
                       .asm_register = asm_register};
    }
    free(bytes);
    free(relocs);
    return true;
  }
  if (previous && previous->is_tentative && section_copy[0]) {
    if (!previous->section) {
      previous->section = strdup(section_copy);
      if (!previous->section) {
        free(bytes);
        free(relocs);
        fail(p, "out of memory storing a C data section name");
        return false;
      }
    } else if (strcmp(previous->section, section_copy) != 0) {
      free(bytes);
      free(relocs);
      fail(p, "conflicting section attributes on a file-scope C object");
      return false;
    }
  }
  if (previous && previous->object_seg != object_seg) {
    free(bytes);
    free(relocs);
    fail(p, "incompatible segment qualifier on C object redeclaration");
    return false;
  }
  if (previous && is_external && !previous->is_external &&
      !previous->is_tentative) {
    if (previous->type != type || previous->agg != type_agg ||
        previous->is_const != is_const ||
        (previous->section && section_copy[0] &&
         strcmp(previous->section, section_copy) != 0)) {
      free(bytes);
      free(relocs);
      fail(p, "incompatible external declaration of a defined C object");
      return false;
    }
    free(bytes);
    free(relocs);
    return true;
  }
  if (previous && is_external && previous->is_external &&
      !previous->is_tentative) {
    if (previous->type != type || previous->agg != type_agg ||
        previous->is_const != is_const || previous->is_array != typedef_array ||
        previous->array_shape.rank != typedef_array_shape.rank ||
        (previous->section && section_copy[0] &&
         strcmp(previous->section, section_copy) != 0)) {
      free(bytes);
      free(relocs);
      fail(p, "incompatible repeated external C object declaration");
      return false;
    }
    if (!previous->section && section_copy[0]) {
      previous->section = strdup(section_copy);
      if (!previous->section) {
        free(bytes);
        free(relocs);
        fail(p, "out of memory storing a C data section name");
        return false;
      }
    }
    free(bytes);
    free(relocs);
    return true;
  }
  if (previous && previous->is_external && !is_external) {
    if (previous->type != type || previous->agg != type_agg ||
        previous->is_const != is_const ||
        (previous->section && section_copy[0] &&
         strcmp(previous->section, section_copy) != 0)) {
      free(bytes);
      free(relocs);
      fail(p, "incompatible definition of previously declared external C "
              "object");
      return false;
    }
    free(previous->bytes);
    free(previous->relocs);
    previous->bytes = bytes;
    previous->len = size;
    previous->relocs = relocs;
    previous->reloc_count = reloc_count;
    previous->type = type;
    previous->agg = type_agg;
    previous->function_result_type = function_result_type;
    previous->is_array = typedef_array;
    previous->array_rank = typedef_array ? typedef_array_shape.rank : 0;
    previous->array_stride =
        typedef_array ? typedef_array_shape.element_size : 0;
    previous->array_shape =
        typedef_array ? typedef_array_shape : (c_array_shape_t){0};
    previous->is_const = is_const;
    previous->is_external = false;
    previous->is_tentative = !has_initializer;
    if (!previous->section && section_copy[0]) {
      previous->section = strdup(section_copy);
      if (!previous->section) {
        fail(p, "out of memory storing a C data section name");
        return false;
      }
    }
    return true;
  }
  if (previous && !previous->is_tentative) {
    free(bytes);
    free(relocs);
    char message[160];
    snprintf(message, sizeof(message),
             "duplicate file-scope C data symbol `%.100s`", name.start);
    fail(p, message);
    return false;
  }
  if (previous && previous->is_tentative) {
    if (has_initializer) {
      free(previous->bytes);
      free(previous->relocs);
      previous->bytes = bytes;
      previous->len = size;
      previous->relocs = relocs;
      previous->reloc_count = reloc_count;
      previous->type = type;
      previous->pointee_seg = pointee_seg;
      previous->agg = type_agg;
      previous->function_result_type = function_result_type;
      previous->is_long_long = is_long_long;
      previous->is_volatile =
          global_is_volatile && !c_type_is_arithmetic_pointer(type);
      previous->is_array = typedef_array;
      previous->array_rank = typedef_array ? typedef_array_shape.rank : 0;
      previous->array_stride =
          typedef_array ? typedef_array_shape.element_size : 0;
      previous->array_shape =
          typedef_array ? typedef_array_shape : (c_array_shape_t){0};
      previous->is_const = is_const;
      previous->is_external = false;
      previous->is_tentative = false;
    } else {
      free(bytes);
      free(relocs);
    }
    return true;
  }
  if (p->global_count >= p->global_capacity) {
    free(bytes);
    free(relocs);
    fail(p, "C frontend global data table capacity exceeded");
    return false;
  }
  c_global_t *registered = &p->globals[p->global_count++];
  *registered = (c_global_t){
      .name = name,
      .bytes = bytes,
      .len = size,
      .relocs = relocs,
      .reloc_count = reloc_count,
      .type = type,
      .pointee_seg = pointee_seg,
      .object_seg = object_seg,
      .pointer_base_type = pointer_base_type,
      .pointer_depth = pointer_depth,
      .is_long_long = is_long_long,
      .is_volatile = global_is_volatile && !c_type_is_arithmetic_pointer(type),
      .agg = type_agg,
      .function_result_type = function_result_type,
      .is_array = typedef_array,
      .array_rank = typedef_array ? typedef_array_shape.rank : 0,
      .array_stride = typedef_array ? typedef_array_shape.element_size : 0,
      .array_shape = typedef_array ? typedef_array_shape : (c_array_shape_t){0},
      .is_const = is_const,
      .is_external = is_external,
      .is_tentative = !is_external && !has_initializer,
      .alignment = alignment,
      .alias_target = alias_target};
  registered->section = section_copy[0] ? strdup(section_copy) : NULL;
  if (section_copy[0] && !registered->section) {
    --p->global_count;
    *registered = (c_global_t){0};
    fail(p, "out of memory storing a C data section name");
    return false;
  }
  return true;
}

static bool parse_signature(c_parser_t *p, c_function_t *fn);

static bool try_collect_function_type_alias_declarations(c_parser_t *p,
                                                         bool *matched) {
  *matched = false;
  size_t declaration_start = p->pos;
  c_parser_t probe = *p;
  char probe_error[1] = {0};
  probe.err = probe_error;
  probe.err_len = sizeof(probe_error);
  probe.type_lookahead = true;
  c_type_t type;
  if (!parse_type(&probe, &type, true) || !probe.last_type_function_type ||
      type != C_FUNC_PTR || probe.last_type_pointer_count ||
      (!probe.last_type_is_function_type_alias &&
       !probe.last_type_typeof_function.len &&
       !probe.last_type_is_function_declarator))
    return true;
  if (!tok_ident(*current(&probe)) || tok_is(&probe, probe.pos + 1, "("))
    return true;
  *matched = true;
  if (!parse_type(p, &type, true))
    return false;
  c_type_t result_type = p->last_type_function_result_type;
  c_agg_type_t *result_agg = p->last_type_function_result_agg;
  const c_typedef_t *function_type_alias =
      find_typedef(p, p->tokens[declaration_start]);
  c_function_t alias_signature = {0};
  bool has_alias_signature = function_type_alias &&
                             function_type_alias->function_type &&
                             function_type_alias->function_signature_start;
  if (has_alias_signature) {
    c_parser_t signature_probe = *p;
    signature_probe.pos = function_type_alias->function_signature_start;
    if (!parse_signature(&signature_probe, &alias_signature)) {
      if (!p->err[0])
        fail(p, "could not parse function typedef signature");
      free(alias_signature.params);
      return false;
    }
  }
  ny_ctok_t ignored_section = {0};
  if (!parse_trailing_file_scope_attributes(p, &ignored_section)) {
    free(alias_signature.params);
    return false;
  }
  for (;;) {
    ny_ctok_t name;
    if (!parse_name(p, &name)) {
      free(alias_signature.params);
      return false;
    }
    c_function_t *function = find_function(p, name);
    if (function) {
      bool compatible;
      if (has_alias_signature) {
        compatible = function->result == alias_signature.result &&
                     function->result_agg == alias_signature.result_agg &&
                     function->param_count == alias_signature.param_count &&
                     function->variadic == alias_signature.variadic;
        for (size_t i = 0; compatible && i < function->param_count; ++i)
          compatible =
              function->params[i].type == alias_signature.params[i].type &&
              function->params[i].agg == alias_signature.params[i].agg;
      } else {
        compatible = function->result == result_type &&
                     function->result_agg == result_agg &&
                     function->param_count == 0 && function->variadic;
      }
      if (!compatible) {
        if (getenv("NYC_KERNEL_COMPAT")) {
          /*
           * Kernel compat: function typedef/alias resolution can misclassify
           * pointer-variable declarations; accept leniently.
           */
        } else {
          fail(p, "conflicting function typedef declarations");
          free(alias_signature.params);
          return false;
        }
      }
    } else {
      if (p->function_count >= p->function_capacity) {
        fail(p, "C frontend function-table capacity exceeded");
        free(alias_signature.params);
        return false;
      }
      bool is_static = false;
      for (size_t i = declaration_start; i < p->pos; ++i)
        is_static |= tok_is(p, i, "static");
      c_function_t alias_function = {
          .name = name,
          .result = has_alias_signature ? alias_signature.result : result_type,
          .result_agg =
              has_alias_signature ? alias_signature.result_agg : result_agg,
          .variadic = has_alias_signature ? alias_signature.variadic : true,
          .is_static = is_static,
          .function_type_placeholder = !has_alias_signature};
      if (has_alias_signature) {
        alias_function.params = alias_signature.params;
        alias_function.param_count = alias_signature.param_count;
        alias_function.result_is_bool = alias_signature.result_is_bool;
        alias_signature.params = NULL;
      }
      p->functions[p->function_count++] = alias_function;
      if (!function_index_add(p, p->function_count - 1)) {
        fail(p, "out of memory indexing C function declarations");
        free(alias_signature.params);
        return false;
      }
    }
    ny_ctok_t section = {0};
    if (!parse_trailing_file_scope_attributes(p, &section)) {
      free(alias_signature.params);
      return false;
    }
    if (!accept(p, ","))
      break;
  }
  free(alias_signature.params);
  return expect(p, ";");
}

static void free_function_table(c_parser_t *p) {
  if (!p)
    return;
  for (size_t i = 0; i < p->function_count; ++i) {
    free(p->functions[i].params);
    free(p->functions[i].section);
  }
  free(p->functions);
  free(p->function_index);
  p->functions = NULL;
  p->function_index = NULL;
  p->function_count = 0;
  p->function_index_capacity = 0;
}

static bool parse_signature(c_parser_t *p, c_function_t *fn) {
  c_param_t *params = NULL;
  size_t count = 0;
  size_t capacity = 0;
  bool variadic = false;
  c_type_t result;
  if (!parse_type(p, &result, true))
    return false;
  while (tok_is_attribute(p, p->pos)) {
    if (!parse_function_type_attributes(p))
      return false;
  }
  /*
   * A function-level section attribute follows the return type, but parsing
   * parameters reuses and resets the same type-attribute state. Preserve the
   * declaration's section until the enclosing function parser consumes it.
   */
  ny_ctok_t function_section = p->last_type_section;
  bool result_is_bool = p->last_type_bool && !c_type_is_pointer(result);
  c_agg_type_t *result_agg =
      result == C_AGG || result == C_AGG_PTR || result == C_AGG_PTR_PTR
          ? p->last_type_agg
          : NULL;
  ny_ctok_t name;
  bool returns_function_pointer =
      p->pos + 3 < p->token_count && tok_is(p, p->pos, "(") &&
      tok_is(p, p->pos + 1, "*") && tok_ident(p->tokens[p->pos + 2]) &&
      tok_is(p, p->pos + 3, "(");
  bool function_type_declarator =
      p->pos + 3 < p->token_count && tok_is(p, p->pos, "(") &&
      tok_ident(p->tokens[p->pos + 1]) && tok_is(p, p->pos + 2, ")") &&
      tok_is(p, p->pos + 3, "(");
  if (returns_function_pointer) {
    p->pos += 2;
    name = *current(p);
    ++p->pos;
    result = C_FUNC_PTR;
    result_agg = NULL;
    result_is_bool = false;
  } else if (function_type_declarator) {
    ++p->pos;
    name = *current(p);
    ++p->pos;
    if (!expect(p, ")"))
      return false;
  } else if (!parse_name(p, &name)) {
    return false;
  }
  if (!tok_is(p, p->pos, "(")) {
    /*
     * GCC treats a declarator without a parameter list over a
     * `typeof(fn)` base type as a declaration of the same function type
     * (the STATIC_CALL_TRAMP shape: `extern typeof(__WARN_trap)
     * __SCT__WARN_trap;`). Replicate the source signature and require `;`.
     */
    c_function_t *source = p->last_type_typeof_function.len
                               ? find_function(p, p->last_type_typeof_function)
                               : NULL;
    if (!source && (!p->last_type_function_type ||
                    p->last_type_function_result_type == C_BAD)) {
      char message[160];
      snprintf(message, sizeof(message),
               "function declaration requires a parameter list near `%.100s`",
               current(p)->start);
      fail(p, message);
      return false;
    }
    if (p->last_type_typeof_function.len && !source) {
      fail(p, "typeof function source signature is missing");
      return false;
    }
    if (source && source->param_count) {
      params = malloc(source->param_count * sizeof(*params));
      if (!params) {
        fail(p, "out of memory replicating a typeof function signature");
        return false;
      }
      memcpy(params, source->params, source->param_count * sizeof(*params));
      count = source->param_count;
      capacity = source->param_count;
      result = source->result;
      result_agg = source->result_agg;
      result_is_bool = source->result_is_bool;
      variadic = source->variadic;
    } else if (source) {
      result = source->result;
      result_agg = source->result_agg;
      result_is_bool = source->result_is_bool;
      variadic = source->variadic;
    } else {
      result = p->last_type_function_result_type;
      result_agg = p->last_type_function_result_agg;
      variadic = true;
    }
    if (!tok_is(p, p->pos, ";")) {
      fail(p, "expected ';' after a typeof function declaration");
      goto error;
    }
    fn->name = name;
    fn->result = result;
    fn->result_agg = result_agg;
    fn->result_is_bool = result_is_bool;
    fn->params = params;
    fn->param_count = count;
    fn->variadic = variadic;
    fn->defined = false;
    return true;
  }
  if (!expect(p, "("))
    return false;
  if (!accept(p, ")")) {
    if (tok_is(p, p->pos, "void") && tok_is(p, p->pos + 1, ")")) {
      p->pos += 2;
    } else {
      for (;;) {
        if (accept(p, "...")) {
          if (count == 0 || !expect(p, ")")) {
            fail(p, "variadic declarations require a fixed parameter prefix");
            goto error;
          }
          variadic = true;
          break;
        }
        c_type_t type;
        if (!parse_type(p, &type, true))
          goto error;
        bool is_bool_param = p->last_type_bool;
        c_type_t pointer_base_type = p->last_type_base_type;
        unsigned pointer_depth = p->last_type_pointer_count;
        bool is_long_long_param = p->last_type_long_long;
        if (type == C_OPAQUE_PTR && pointer_depth > 2)
          type = C_OPAQUE_PTR_PTR_PTR;
        if (type == C_AGG && p->last_type_is_array && !pointer_depth) {
          /*
           * A typedef naming an aggregate array is adjusted to a pointer to
           * its element when used as a function parameter.
           */
          type = C_AGG_PTR;
          pointer_base_type = C_AGG;
          pointer_depth = 1;
        } else if (p->last_type_is_array && !pointer_depth) {
          type = c_array_pointer_type(type);
          pointer_depth = 1;
        }
        c_type_t callback_result_type =
            p->last_type_function_result_type != C_BAD
                ? p->last_type_function_result_type
                : type;
        /*
         * In C, va_list is an array type on SysV x86-64. Function parameters
         * are adjusted to pointers to its 24-byte descriptor.
         */
        if (type == C_VA_LIST)
          type = C_OPAQUE_PTR;
        c_agg_type_t *param_agg =
            type == C_AGG || type == C_AGG_PTR || type == C_AGG_PTR_PTR
                ? p->last_type_agg
                : NULL;
        bool is_const = c_type_is_pointer(type) ? p->last_type_const_pointer
                                                : p->last_type_const;
        bool is_volatile =
            p->last_type_volatile && !c_type_is_arithmetic_pointer(type);
        bool pointee_volatile =
            p->last_type_volatile && c_type_is_arithmetic_pointer(type);
        unsigned char pointee_seg =
            c_type_is_pointer(type) ? p->last_type_seg : C_SEG_NONE;
        ny_ctok_t param_name = {0};
        unsigned nested_function_pointers = 0;
        if (parse_nested_function_pointer_declarator(
                p, &param_name, &nested_function_pointers)) {
          type = nested_function_pointers == 2 ? C_FUNC_PTR_PTR : C_OPAQUE_PTR;
        } else if (parse_function_pointer_declarator(p, &param_name)) {
          type = C_FUNC_PTR;
        } else {
          unsigned decl_ptr_depth = 0;
          if (parse_parenthesized_pointer_array_declarator(p, &param_name,
                                                           &decl_ptr_depth)) {
            pointer_depth += decl_ptr_depth;
            if (pointer_depth == 1)
              type = type == C_AGG ? C_AGG_PTR : c_array_pointer_type(type);
            else if (pointer_depth == 2 &&
                     (pointer_base_type == C_U8 || pointer_base_type == C_CHAR))
              type = C_CHAR_PTR_PTR;
            else if (pointer_depth == 2)
              type = C_OPAQUE_PTR_PTR;
            else
              type = C_OPAQUE_PTR_PTR_PTR;
          } else if (p->pos + 2 < p->token_count && tok_is(p, p->pos, "(") &&
                     tok_ident(p->tokens[p->pos + 1]) &&
                     tok_is(p, p->pos + 2, ")")) {
            param_name = p->tokens[p->pos + 1];
            p->pos += 3;
            if (tok_is(p, p->pos, "(")) {
              size_t after = skip_balanced(p, p->pos, "(", ")");
              if (after <= p->pos || after >= p->token_count) {
                fail(p, "unterminated function parameter declarator");
                goto error;
              }
              p->pos = after;
              type = C_FUNC_PTR;
            }
          } else if (tok_ident(*current(p))) {
            param_name = *current(p);
            ++p->pos;
            if (tok_is(p, p->pos, "(")) {
              size_t after = skip_balanced(p, p->pos, "(", ")");
              if (after <= p->pos || after >= p->token_count) {
                fail(p, "unterminated function parameter declarator");
                goto error;
              }
              p->pos = after;
              type = C_FUNC_PTR;
            }
          }
        }
        c_array_shape_t array_parameter_shape = {0};
        size_t array_parameter_size = 0;
        if (tok_is(p, p->pos, "[")) {
          size_t extents[C_MAX_ARRAY_RANK] = {0};
          unsigned rank = 0;
          c_type_t element_type = type;
          do {
            if (rank == C_MAX_ARRAY_RANK) {
              fail(p, "C array parameter exceeds the rank limit");
              goto error;
            }
            ++p->pos;
            ++rank;
            while (accept(p, "const") || accept(p, "restrict") ||
                   accept(p, "volatile") || accept(p, "static")) {
            }
            uint64_t extent = 0;
            if (!accept(p, "]")) {
              if (!parse_array_extent_expr(p, 1, &extent) ||
                  extent > SIZE_MAX || !expect(p, "]")) {
                fail(p, "C array parameter requires constant extents");
                goto error;
              }
              extents[rank - 1] = (size_t)extent;
            } else if (rank != 1) {
              fail(p, "only the outer C array parameter extent may be empty");
              goto error;
            }
          } while (tok_is(p, p->pos, "["));
          if (type == C_BAD) {
            type = C_BAD;
          } else if (type == C_CHAR_PTR) {
            type = C_CHAR_PTR_PTR;
          } else if (type == C_I8_PTR) {
            type = C_I8_PTR_PTR;
          } else if (type == C_U16_PTR) {
            type = C_U16_PTR_PTR;
          } else if (type == C_AGG_PTR) {
            type = C_AGG_PTR_PTR;
          } else if (type == C_FUNC_PTR) {
            type = C_FUNC_PTR_PTR;
          } else if (c_type_is_pointer(type)) {
            type = C_OPAQUE_PTR_PTR;
          } else {
            type = c_array_pointer_type(type);
          }
          ++pointer_depth;
          size_t element_size = element_type == C_AGG && p->last_type_agg
                                    ? p->last_type_agg->size
                                    : c_type_size(p->abi, element_type);
          if (!element_size) {
            fail(p, "C array parameter has an unsupported element type");
            goto error;
          }
          if (rank > 1) {
            array_parameter_shape =
                (c_array_shape_t){.rank = rank - 1,
                                  .element_size = element_size,
                                  .element_type = element_type,
                                  .agg = p->last_type_agg,
                                  .pointer_to_array = true};
            memcpy(array_parameter_shape.extent, extents + 1,
                   (rank - 1) * sizeof(*extents));
            array_parameter_size = c_array_shape_size(&array_parameter_shape);
            if (!array_parameter_size) {
              fail(p, "C array parameter row size is unsupported");
              goto error;
            }
          }
        }
        /*
         * GNU permits attributes after a parameter declarator, including
         * Linux's __attribute__((__unused__)) annotations.
         */
        while (tok_is_attribute(p, p->pos)) {
          if (!parse_function_type_attributes(p))
            goto error;
        }
        if (type == C_BAD) {
          fail(p, "parameter type must be an integer, double, pointer, struct, "
                  "or union");
          goto error;
        }
        if (count == capacity) {
          size_t next_capacity = capacity ? capacity * 2 : 4;
          if (next_capacity < capacity ||
              next_capacity > SIZE_MAX / sizeof(*params)) {
            fail(p, "C function parameter list is too large");
            goto error;
          }
          c_param_t *grown = realloc(params, next_capacity * sizeof(*grown));
          if (!grown) {
            fail(p, "out of memory allocating C parameter table");
            goto error;
          }
          params = grown;
          capacity = next_capacity;
        }
        size_t pointee_array_size = pointer_depth ? p->last_type_array_size : 0;
        if (pointer_depth && p->last_type_is_array && !pointee_array_size &&
            p->last_type_agg)
          pointee_array_size = p->last_type_agg->size;
        if (array_parameter_shape.rank) {
          pointee_array_size = array_parameter_size;
          p->last_type_pointee_array = array_parameter_shape;
        }
        params[count++] = (c_param_t){
            .name = param_name,
            .type = type,
            .pointer_base_type = pointer_base_type,
            .pointer_depth = pointer_depth,
            .is_long_long = is_long_long_param,
            .function_result_type =
                type == C_FUNC_PTR ? callback_result_type : C_BAD,
            .agg = param_agg,
            .is_const = is_const,
            .is_bool = is_bool_param && !c_type_is_pointer(type),
            .pointee_const = c_type_is_pointer(type) && p->last_type_const,
            .is_volatile = is_volatile,
            .pointee_volatile = pointee_volatile,
            .pointee_seg = pointee_seg,
            .pointee_array_size = pointee_array_size,
            .pointee_array = p->last_type_pointee_array};
        if (accept(p, ")"))
          break;
        if (!expect(p, ","))
          goto error;
      }
    }
  }
  if (returns_function_pointer) {
    if (!expect(p, ")") || !expect(p, "(")) {
      fail(p, "malformed function-pointer return declarator");
      goto error;
    }
    size_t depth = 1;
    while (p->pos < p->token_count && depth) {
      if (accept(p, "("))
        ++depth;
      else if (accept(p, ")"))
        --depth;
      else
        ++p->pos;
    }
    if (depth) {
      fail(p, "unterminated function-pointer return type");
      goto error;
    }
  }
  if (fn) {
    fn->name = name;
    fn->result = result;
    fn->result_agg = result_agg;
    fn->result_is_bool = result_is_bool;
    fn->params = params;
    fn->param_count = count;
    fn->variadic = variadic;
  } else {
    free(params);
  }
  p->last_type_section = function_section;
  return true;

error:
  free(params);
  return false;
}

static bool collect_functions(c_parser_t *p) {
  p->pos = 0;
  while (p->pos + 1 < p->token_count) {
    /*
     * Alias attributes belong to one file-scope declaration.
     */
    p->last_function_alias = (ny_ctok_t){0};
    if (accept(p, "__extension__"))
      continue;
    if (accept(p, ";"))
      continue;
    if (tok_is(p, p->pos, "asm") || tok_is(p, p->pos, "__asm") ||
        tok_is(p, p->pos, "__asm__")) {
      size_t at = p->pos + 1;
      while (tok_is(p, at, "volatile") || tok_is(p, at, "__volatile__") ||
             tok_is(p, at, "goto"))
        ++at;
      if (tok_is(p, at, "(")) {
        if (!collect_file_scope_asm(p))
          return false;
        continue;
      }
    }
    if (accept(p, "_Static_assert")) {
      int64_t condition = 0;
      size_t sa_pos = p->pos;
      if (!expect(p, "(") || !parse_global_const_expr(p, &p->pos, &condition) ||
          !expect(p, ",")) {
        if (getenv("NYC_KERNEL_COMPAT")) {
          if (p->err)
            p->err[0] = 0;
          p->pos = sa_pos;
          int depth = 0;
          bool in_sa = false;
          while (p->pos < p->token_count) {
            if (tok_is(p, p->pos, "(")) {
              ++depth;
              in_sa = true;
            } else if (tok_is(p, p->pos, ")")) {
              if (--depth <= 0 && in_sa) {
                ++p->pos;
                break;
              }
            } else if (depth <= 0 && in_sa && tok_is(p, p->pos, ";"))
              break;
            ++p->pos;
          }
          (void)accept(p, ";");
          continue;
        }
        fail(p, "invalid _Static_assert constant expression");
        return false;
      }
      bool message_found = false;
      while (current(p)->kind == NY_CTOK_STRING) {
        message_found = true;
        ++p->pos;
      }
      if (!message_found || !expect(p, ")") || !expect(p, ";")) {
        fail(p, "invalid _Static_assert diagnostic string");
        return false;
      }
      if (!condition && !getenv("NYC_KERNEL_COMPAT")) {
        fail(p, "C _Static_assert condition is false");
        return false;
      }
      continue;
    }
    if (accept(p, "_Pragma")) {
      unsigned char *pragma = NULL;
      size_t pragma_len = 0;
      if (!expect(p, "(") || current(p)->kind != NY_CTOK_STRING ||
          !decode_string_token(*current(p), &pragma, &pragma_len)) {
        free(pragma);
        fail(p, "file-scope _Pragma requires a string literal");
        return false;
      }
      bool diagnostic_pragma =
          pragma_len >= sizeof("GCC diagnostic ") &&
          !memcmp(pragma, "GCC diagnostic ", sizeof("GCC diagnostic ") - 1);
      free(pragma);
      ++p->pos;
      if (!diagnostic_pragma || !expect(p, ")")) {
        fail(p, "unsupported file-scope _Pragma directive");
        return false;
      }
      continue;
    }
    if (starts_aggregate_type(p)) {
      c_parser_t probe = *p;
      char probe_error[1] = {0};
      probe.err = probe_error;
      probe.err_len = sizeof(probe_error);
      probe.type_lookahead = true;
      c_type_t type;
      if (parse_type(&probe, &type, false) && type == C_AGG &&
          tok_is(&probe, probe.pos, ";")) {
        if (!parse_type(p, &type, false) || !expect(p, ";"))
          return false;
        continue;
      }
    }
    if (tok_is(p, p->pos, "enum")) {
      c_parser_t probe = *p;
      char probe_error[1] = {0};
      probe.err = probe_error;
      probe.err_len = sizeof(probe_error);
      probe.type_lookahead = true;
      if (!parse_enum_type(&probe, NULL))
        return false;
      if (tok_is(&probe, probe.pos, ";")) {
        if (!parse_enum_type(p, NULL) || !expect(p, ";"))
          return false;
        continue;
      }
    }
    bool matched_global = false;
    /*
     * Give the string-literal array parser priority over the general pointer
     * array parser, which accepts symbol addresses and nulls.
     */
    if (!try_collect_string_pointer_array(p, &matched_global, false))
      return false;
    if (matched_global)
      continue;
    matched_global = false;
    if (!try_collect_function_pointer_array(p, &matched_global))
      return false;
    if (matched_global)
      continue;
    matched_global = false;
    size_t function_return_type_pos = p->pos;
    while (tok_is(p, function_return_type_pos, "const") ||
           tok_is(p, function_return_type_pos, "volatile") ||
           tok_is(p, function_return_type_pos, "static") ||
           tok_is(p, function_return_type_pos, "extern") ||
           tok_is(p, function_return_type_pos, "register") ||
           tok_is(p, function_return_type_pos, "inline"))
      ++function_return_type_pos;
    if (tok_is(p, function_return_type_pos, "struct") ||
        tok_is(p, function_return_type_pos, "union")) {
      if (!try_collect_function_pointer_global(p, &matched_global))
        return false;
      if (matched_global)
        continue;
      matched_global = false;
    }
    if (!try_collect_char_array(p, &matched_global))
      return false;
    if (matched_global)
      continue;
    matched_global = false;
    if (!try_collect_byte_array(p, &matched_global))
      return false;
    if (matched_global)
      continue;
    matched_global = false;
    if (!try_collect_external_aggregate_pointer_global(p, &matched_global))
      return false;
    if (matched_global)
      continue;
    matched_global = false;
    if (!try_collect_uninitialized_pointer_arrays(p, &matched_global))
      return false;
    if (matched_global)
      continue;
    matched_global = false;
    if (!try_collect_aggregate_pointer_array(p, &matched_global))
      return false;
    if (matched_global)
      continue;
    matched_global = false;
    if (!try_collect_aggregate_global(p, &matched_global))
      return false;
    if (matched_global)
      continue;
    matched_global = false;
    if (!try_collect_uninitialized_scalar_global_list(p, &matched_global))
      return false;
    if (matched_global)
      continue;
    matched_global = false;
    if (!try_collect_function_type_alias_declarations(p, &matched_global))
      return false;
    if (matched_global)
      continue;
    matched_global = false;
    if (!try_collect_function_pointer_global(p, &matched_global))
      return false;
    if (matched_global)
      continue;
    if (!try_collect_initialized_scalar_global_list(p, &matched_global))
      return false;
    if (matched_global)
      continue;
    if (!try_collect_scalar_global(p, &matched_global))
      return false;
    if (matched_global)
      continue;
    if (accept(p, "typedef")) {
      size_t typedef_start = p->pos - 1;
      c_type_t alias_type;
      if (!parse_type(p, &alias_type, true))
        return false;
      bool alias_is_bool = p->last_type_bool;
      bool alias_is_array = p->last_type_is_array;
      c_array_shape_t alias_array_shape = p->last_type_array;
      c_type_t function_result_type = alias_type;
      c_agg_type_t *function_result_agg = alias_type == C_AGG ||
                                                  alias_type == C_AGG_PTR ||
                                                  alias_type == C_AGG_PTR_PTR
                                              ? p->last_type_agg
                                              : NULL;
      c_parser_t function_pointer_probe = *p;
      char function_pointer_error[1] = {0};
      function_pointer_probe.err = function_pointer_error;
      function_pointer_probe.err_len = sizeof(function_pointer_error);
      ny_ctok_t function_pointer_name = {0};
      bool function_pointer =
          parse_function_pointer_declarator(&function_pointer_probe,
                                            &function_pointer_name) &&
          function_pointer_name.kind == NY_CTOK_IDENT;
      bool function_void_params = function_pointer &&
                                  tok_is(p, p->pos + 5, "void") &&
                                  tok_is(p, p->pos + 6, ")");
      bool function_type =
          !function_pointer && p->pos + 3 < p->token_count &&
          tok_is(p, p->pos, "(") && tok_ident(p->tokens[p->pos + 1]) &&
          tok_is(p, p->pos + 2, ")") && tok_is(p, p->pos + 3, "(");
      bool pointer_to_array_typedef = !function_pointer && !function_type &&
                                      tok_is(p, p->pos, "(") &&
                                      tok_is(p, p->pos + 1, "*");
      ny_ctok_t name;
      ny_ctok_t typedef_prefix_section = {0};
      if (!parse_trailing_file_scope_attributes(p, &typedef_prefix_section))
        return false;
      size_t array_size = p->last_type_array_size;
      size_t type_alignment = p->last_type_alignment;
      if (function_pointer) {
        if (!parse_function_pointer_declarator(p, &name)) {
          if (!p->err || !p->err_len || !p->err[0])
            fail(p, "unterminated C function-pointer typedef");
          return false;
        }
        alias_type = C_FUNC_PTR;
      } else if (function_type) {
        ++p->pos;
        if (!parse_name(p, &name) || !expect(p, ")"))
          return false;
        size_t after = skip_balanced(p, p->pos, "(", ")");
        if (after <= p->pos || after >= p->token_count) {
          fail(p, "unterminated C function-type typedef");
          return false;
        }
        p->pos = after;
        alias_type = C_FUNC_PTR;
      } else if (pointer_to_array_typedef) {
        c_type_t element_type = alias_type;
        c_agg_type_t *element_agg = p->last_type_agg;
        size_t element_size = element_type == C_AGG && element_agg
                                  ? element_agg->size
                                  : c_type_size(p->abi, element_type);
        ++p->pos;
        ++p->pos;
        while (accept(p, "const") || accept(p, "volatile") ||
               accept(p, "restrict") || accept(p, "__restrict") ||
               accept(p, "__restrict__")) {
        }
        if (!parse_name(p, &name) || !expect(p, ")") || !expect(p, "[") ||
            !element_size) {
          if (!p->err || !p->err_len || !p->err[0])
            fail(p, "pointer-to-array typedef requires an object element type");
          return false;
        }
        uint64_t extent = 0;
        if (!parse_array_extent_expr(p, 1, &extent) ||
            extent > SIZE_MAX / element_size || !expect(p, "]")) {
          fail(p,
               "pointer-to-array typedef requires a bounded constant extent");
          return false;
        }
        c_type_t pointer_type = element_type == C_AGG
                                    ? C_AGG_PTR
                                    : c_array_pointer_type(element_type);
        if (!c_type_is_pointer(pointer_type)) {
          fail(p, "pointer-to-array typedef has an unsupported element type");
          return false;
        }
        array_size = element_size * (size_t)extent;
        alias_array_shape = (c_array_shape_t){.rank = 1,
                                              .extent = {(size_t)extent},
                                              .element_size = element_size,
                                              .element_type = element_type,
                                              .agg = element_agg,
                                              .pointer_to_array = true};
        alias_type = pointer_type;
      } else if (!parse_name(p, &name)) {
        return false;
      }
      if (!function_pointer && !function_type && tok_is(p, p->pos, "(")) {
        size_t after = skip_balanced(p, p->pos, "(", ")");
        if (after <= p->pos || after >= p->token_count) {
          fail(p, "unterminated C function-type typedef");
          return false;
        }
        p->pos = after;
        function_type = true;
        alias_type = C_FUNC_PTR;
      }
      if (!function_pointer && tok_is(p, p->pos, "[")) {
        size_t element_size = array_size ? array_size
                              : alias_type == C_AGG && p->last_type_agg
                                  ? p->last_type_agg->size
                                  : c_type_size(p->abi, alias_type);
        if (!element_size) {
          fail(p, "array typedef requires an object element type");
          return false;
        }
        size_t declared_extents[C_MAX_ARRAY_RANK] = {0};
        unsigned declared_rank = 0;
        bool incomplete_extent = false;
        do {
          ++p->pos;
          uint64_t extent = 0;
          if (accept(p, "]")) {
            if (tok_is(p, p->pos, "[")) {
              fail(p,
                   "only the outermost array typedef extent may be incomplete");
              return false;
            }
            array_size = 0;
            incomplete_extent = true;
            alias_is_array = true;
            if (declared_rank == C_MAX_ARRAY_RANK) {
              fail(p, "array typedef exceeds the rank limit");
              return false;
            }
            declared_extents[declared_rank++] = 0;
            break;
          }
          if (!parse_array_extent_expr(p, 1, &extent) ||
              extent > SIZE_MAX / element_size || !expect(p, "]")) {
            fail(p, "array typedef requires constant extents");
            return false;
          }
          if (declared_rank == C_MAX_ARRAY_RANK) {
            fail(p, "array typedef exceeds the rank limit");
            return false;
          }
          declared_extents[declared_rank++] = (size_t)extent;
          element_size *= (size_t)extent;
          alias_is_array = true;
        } while (tok_is(p, p->pos, "["));
        array_size = incomplete_extent ? 0 : element_size;
        c_array_shape_t shape = alias_array_shape;
        if (!shape.rank) {
          shape.element_type = alias_type;
          shape.element_size = alias_type == C_AGG && p->last_type_agg
                                   ? p->last_type_agg->size
                                   : c_type_size(p->abi, alias_type);
          shape.agg = alias_type == C_AGG ? p->last_type_agg : NULL;
        }
        if (declared_rank + shape.rank > C_MAX_ARRAY_RANK) {
          fail(p, "array typedef exceeds the rank limit");
          return false;
        }
        memmove(shape.extent + declared_rank, shape.extent,
                shape.rank * sizeof(shape.extent[0]));
        memcpy(shape.extent, declared_extents,
               declared_rank * sizeof(declared_extents[0]));
        shape.rank += declared_rank;
        alias_array_shape = shape;
      }
      size_t typedef_attribute_start = p->pos;
      ny_ctok_t typedef_section = {0};
      if (!parse_trailing_file_scope_attributes(p, &typedef_section) ||
          !apply_mode_attribute(p, &alias_type) || !expect(p, ";")) {
        return false;
      }
      if (p->last_type_alignment > type_alignment)
        type_alignment = p->last_type_alignment;
      c_agg_type_t *alias_agg = alias_type == C_AGG ||
                                        alias_type == C_AGG_PTR ||
                                        alias_type == C_AGG_PTR_PTR
                                    ? p->last_type_agg
                                    : NULL;
      if (alias_agg) {
        for (size_t i = typedef_attribute_start; i < p->pos; ++i) {
          if (tok_is(p, i, "__transparent_union__") ||
              tok_is(p, i, "transparent_union")) {
            alias_agg->is_transparent_union = true;
            break;
          }
        }
      }
      bool alias_is_void = alias_type == C_BAD && !function_type;
      if (!c_type_is_integer(alias_type) && alias_type != C_F32 &&
          alias_type != C_F64 && !alias_is_void && alias_type != C_BAD &&
          alias_type != C_AGG && alias_type != C_AGG_PTR &&
          alias_type != C_FUNC_PTR && alias_type != C_OPAQUE_PTR &&
          alias_type != C_VA_LIST && !c_type_is_pointer(alias_type)) {
        fail(p, "typedef aliases currently support integer, double, pointer, "
                "struct/union, or function-pointer types");
        return false;
      }
      bool is_const = p->last_type_const;
      const c_typedef_t *previous = find_typedef(p, name);
      if (previous) {
        if (previous->type != alias_type || previous->agg != alias_agg ||
            previous->is_void != alias_is_void ||
            previous->is_const != is_const ||
            previous->is_bool != alias_is_bool || function_pointer ||
            previous->function_type != function_type ||
            previous->is_array != alias_is_array ||
            previous->array_size != array_size ||
            previous->array_shape.rank != alias_array_shape.rank ||
            (alias_array_shape.rank &&
             memcmp(previous->array_shape.extent, alias_array_shape.extent,
                    alias_array_shape.rank *
                        sizeof(alias_array_shape.extent[0])) != 0) ||
            previous->alignment != type_alignment) {
          fail(p, "incompatible C typedef redeclaration");
          return false;
        }
        continue;
      }
      if (find_function(p, name)) {
        fail(p, "duplicate C typedef name");
        return false;
      }
      if (p->typedef_count >= p->typedef_capacity) {
        fail(p, "C frontend typedef-table capacity exceeded");
        return false;
      }
      p->typedefs[p->typedef_count++] = (c_typedef_t){
          .name = name,
          .type = alias_type,
          .is_void = alias_is_void,
          .is_bool = alias_is_bool,
          .agg = alias_agg,
          .array_size = array_size,
          .array_shape = alias_array_shape,
          .is_array = alias_is_array,
          .function_result_type =
              function_pointer || function_type ? function_result_type : C_BAD,
          .function_result_agg =
              function_pointer || function_type ? function_result_agg : NULL,
          .is_long_long = p->last_type_long_long,
          .function_type = function_type,
          .function_signature_start = function_type ? typedef_start + 1 : 0,
          .function_void_params = function_void_params,
          .is_const = is_const,
          .alignment = type_alignment};
      if (!typedef_index_ensure(p)) {
        fail(p, "out of memory indexing C typedef declarations");
        return false;
      }
      continue;
    }
    if (starts_aggregate_type(p)) {
      size_t before = p->pos;
      bool is_union = tok_is(p, p->pos, "union");
      ++p->pos;
      ny_ctok_t tag_name = {0};
      if (tok_ident(*current(p))) {
        tag_name = *current(p);
        ++p->pos;
      }
      if (!tag_name.len) {
        fail(p, "file-scope struct/union declarations require a tag name");
        return false;
      }
      if (accept(p, "{")) {
        if (!parse_tag_body(p, tag_name, is_union))
          return false;
        if (tok_is(p, p->pos, ";")) {
          ++p->pos;
          continue;
        }
        if (tok_ident(*current(p)) && tok_is(p, p->pos + 1, "(")) {
          /*
           * `struct Tag { ... } name(args)` is unsupported; only a plain
           * signature may follow a completed tag body.
           */
          p->pos = before;
        } else {
          p->pos = before;
          fail(p, "unsupported file-scope construct after struct/union "
                  "declaration");
          return false;
        }
      } else if (accept(p, ";")) {
        if (!declare_tag(p, tag_name, is_union))
          return false;
        continue;
      } else {
        /*
         * Return declarators can begin with '*' and pointer qualifiers.
         * Only a bare tag followed by ';' is a forward declaration.
         */
        p->pos = before;
      }
    }
    size_t declaration_start = p->pos;
    c_function_t candidate = {0};
    if (!parse_signature(p, &candidate))
      return false;
    ny_ctok_t asm_label = {0};
    if (!try_parse_asm_label(p, &asm_label)) {
      free(candidate.params);
      return false;
    }
    candidate.has_asm_alias = asm_label.len != 0;
    if (candidate.has_asm_alias && asm_label.len >= 2) {
      candidate.asm_alias = (ny_ctok_t){.kind = NY_CTOK_IDENT,
                                        .start = asm_label.start + 1,
                                        .len = asm_label.len - 2};
    } else if (candidate.has_asm_alias) {
      free(candidate.params);
      fail(p, "function asm label must be a string literal");
      return false;
    }
    for (size_t i = declaration_start; i < p->pos; ++i) {
      if (p->tokens[i].start == candidate.name.start) {
        for (size_t j = declaration_start; j < i; ++j) {
          candidate.is_static |= tok_is(p, j, "static");
          candidate.is_inline |= tok_is(p, j, "inline") ||
                                 tok_is(p, j, "__inline") ||
                                 tok_is(p, j, "__inline__");
          candidate.is_used |=
              tok_is(p, j, "used") || tok_is(p, j, "__used__") ||
              tok_is(p, j, "retain") || tok_is(p, j, "__retain__");
        }
        break;
      }
    }
    if (p->function_count >= p->function_capacity) {
      free(candidate.params);
      fail(p, "C frontend function-table capacity exceeded");
      return false;
    }
    c_function_t *fn = find_function(p, candidate.name);
    bool inserted = false;
    if (!fn) {
      size_t function_index = p->function_count++;
      fn = &p->functions[function_index];
      *fn = candidate;
      candidate.params = NULL;
      inserted = true;
      if (!function_index_add(p, function_index)) {
        fail(p, "out of memory indexing C function declarations");
        return false;
      }
    } else {
      bool function_type_placeholder = fn->function_type_placeholder;
      bool conflict = fn->result != candidate.result ||
                      fn->result_agg != candidate.result_agg ||
                      fn->result_is_bool != candidate.result_is_bool ||
                      (!function_type_placeholder &&
                       (fn->param_count != candidate.param_count ||
                        fn->variadic != candidate.variadic));
      for (size_t i = 0;
           !conflict && !function_type_placeholder && i < fn->param_count;
           ++i) {
        if (!fn->params && candidate.params &&
            fn->param_count == candidate.param_count) {
          c_param_t *params = malloc(fn->param_count * sizeof(*params));
          if (params) {
            memcpy(params, candidate.params, fn->param_count * sizeof(*params));
            fn->params = params;
          }
        }
        if (!fn->params || !candidate.params) {
          conflict = (fn->param_count != 0 || candidate.param_count != 0);
          break;
        }
        conflict = fn->params[i].type != candidate.params[i].type ||
                   fn->params[i].agg != candidate.params[i].agg;
      }
      if (conflict) {
        free(candidate.params);
        char message[192];
        int name_len = candidate.name.len > 120 ? 120 : (int)candidate.name.len;
        snprintf(message, sizeof(message),
                 "conflicting function declarations for '%.*s'", name_len,
                 candidate.name.start);
        fail(p, message);
        return false;
      }
      if (function_type_placeholder) {
        c_param_t *params =
            candidate.param_count
                ? malloc(candidate.param_count * sizeof(*params))
                : NULL;
        if (candidate.param_count && !params) {
          free(candidate.params);
          fail(p, "out of memory completing a function typedef signature");
          return false;
        }
        if (params)
          memcpy(params, candidate.params,
                 candidate.param_count * sizeof(*params));
        free(fn->params);
        fn->params = params;
        fn->param_count = candidate.param_count;
        fn->variadic = candidate.variadic;
        fn->function_type_placeholder = false;
      }
      fn->is_static |= candidate.is_static;
      fn->is_inline |= candidate.is_inline;
      fn->always_inline |= candidate.always_inline;
      fn->is_used |= candidate.is_used;
      if (fn->asm_alias.len && candidate.asm_alias.len &&
          !tok_same(fn->asm_alias, candidate.asm_alias)) {
        free(candidate.params);
        fail(p, "conflicting function asm labels");
        return false;
      }
      if (!fn->asm_alias.len)
        fn->asm_alias = candidate.asm_alias;
      fn->has_asm_alias |= candidate.has_asm_alias;
    }
    /*
     * Trailing attributes may follow a declaration or a definition
     * (kernel headers append `__attribute__((__format__(...)))` to
     * prototypes). The parameter-position section capture is borrowed here
     * before the parser clears it.
     */
    ny_ctok_t fn_section = p->last_type_section;
    p->last_type_section = (ny_ctok_t){0};
    p->last_function_alias = (ny_ctok_t){0};
    if (!parse_trailing_file_scope_attributes(p, &fn_section))
      return false;
    fn->no_return |=
        function_declaration_attribute(p, declaration_start, p->pos, "noreturn",
                                       "__noreturn__") != SIZE_MAX;
    bool gnu_extern_inline = function_declaration_attribute(
                                 p, declaration_start, p->pos, "gnu_inline",
                                 "__gnu_inline__") != SIZE_MAX;
    bool is_extern = false;
    for (size_t j = declaration_start; j < p->pos; ++j)
      is_extern |= tok_is(p, j, "extern");
    size_t regparm_at = function_declaration_attribute(
        p, declaration_start, p->pos, "regparm", "__regparm__");
    if (regparm_at != SIZE_MAX && p->abi == NYC_ABI_I386) {
      size_t argument_at = regparm_at + 2;
      int64_t regparm = 0;
      if (!tok_is(p, regparm_at + 1, "(") ||
          !parse_global_const_expr(p, &argument_at, &regparm) ||
          !tok_is(p, argument_at, ")") || regparm < 0 || regparm > 3 ||
          (fn->has_regparm && fn->regparm != (unsigned)regparm)) {
        fail(p, "invalid or conflicting i386 regparm attribute");
        return false;
      }
      fn->has_regparm = true;
      fn->regparm = (unsigned)regparm;
    }
    if (p->last_function_alias.len) {
      if (fn->alias_target.len &&
          !tok_same(fn->alias_target, p->last_function_alias)) {
        free(candidate.params);
        fail(p, "conflicting function alias attributes");
        return false;
      }
      fn->alias_target = p->last_function_alias;
    }
    for (size_t j = declaration_start; j < p->pos; ++j) {
      fn->is_inline |= tok_is(p, j, "inline") || tok_is(p, j, "__inline") ||
                       tok_is(p, j, "__inline__");
      fn->always_inline |=
          tok_is(p, j, "always_inline") || tok_is(p, j, "__always_inline__");
      fn->noinline |= tok_is(p, j, "noinline") || tok_is(p, j, "__noinline__");
      fn->no_stack_protector |= tok_is(p, j, "no_stack_protector") ||
                                tok_is(p, j, "__no_stack_protector__");
      fn->no_instrument_function |= tok_is(p, j, "no_instrument_function") ||
                                    tok_is(p, j, "__no_instrument_function__");
      fn->is_weak |= tok_is(p, j, "weak") || tok_is(p, j, "__weak") ||
                     tok_is(p, j, "__weak__");
      fn->is_inline |= fn->always_inline;
      fn->is_used |= tok_is(p, j, "used") || tok_is(p, j, "__used__") ||
                     tok_is(p, j, "retain") || tok_is(p, j, "__retain__");
    }
    char section_copy[64] = {0};
    if (fn_section.len &&
        !copy_quoted_token(p, fn_section, section_copy, sizeof(section_copy)))
      return false;
    if (section_copy[0]) {
      if (fn->section && strcmp(fn->section, section_copy) != 0) {
        free(candidate.params);
        fail(p, "conflicting function section attributes");
        return false;
      }
      if (!fn->section) {
        fn->section = strdup(section_copy);
        if (!fn->section) {
          free(candidate.params);
          fail(p, "out of memory storing a C function section name");
          return false;
        }
      }
    }
    if (accept(p, ";")) {
      free(candidate.params);
      continue;
    }
    if (!tok_is(p, p->pos, "{")) {
      free(candidate.params);
      fail(p, "expected function body or ';'");
      return false;
    }
    size_t start = p->pos + 1;
    size_t end = skip_balanced(p, p->pos, "{", "}");
    if (end <= p->pos + 1 || end > p->token_count) {
      free(candidate.params);
      fail(p, "unterminated function body");
      return false;
    }
    if (fn->defined) {
      free(candidate.params);
      fail(p, "duplicate function definition");
      return false;
    }
    fn->no_emit |= gnu_extern_inline && is_extern;
    /*
     * Prototype parameter names are optional in C; the definition's names
     * are required by lowering and supersede the declaration metadata.
     */
    if (!inserted && candidate.params != fn->params) {
      free(fn->params);
      fn->params = candidate.params;
      candidate.params = NULL;
    }
    fn->body_start = start;
    fn->body_end = end - 1;
    fn->defined = true;
    p->pos = end;
  }
  return true;
}

#include "code/lang/c/lower/lower.c"
static void parser_destroy(c_parser_t *p) {
  if (!p)
    return;
  free(p->lvalues);
  free(p->labels);
  free_tag_table(p);
  free_function_table(p);
  for (size_t i = 0; i < p->global_count; ++i) {
    free(p->globals[i].bytes);
    free(p->globals[i].owned_name);
    free(p->globals[i].owned_assembly);
    for (size_t r = 0; r < p->globals[i].reloc_count; ++r)
      free(p->globals[i].relocs[r].owned_target);
    free(p->globals[i].relocs);
    free(p->globals[i].section);
  }
  free(p->raw_file_scope_asm);
  for (size_t i = 0; i < p->pp_constant_count; ++i) {
    free(p->pp_constants[i].name);
    free(p->pp_constants[i].replacement);
  }
  free(p->pp_constants);
  for (size_t i = 0; i < p->macro_storage_count; ++i)
    free(p->macro_storage[i]);
  free(p->macro_storage);
  free(p->globals);
  free(p->typedef_index);
  free(p->typedefs);
  free(p->enum_constants);
  free(p->enum_tags);
  free(p->locals);
  free(p->tokens);
  for (size_t i = 0; i < p->file_count; ++i) {
    free(p->files[i].owned_text);
    free(p->files[i].owned_path);
  }
  free(p->files);
  free(p->source_runs);
  memset(p, 0, sizeof(*p));
}

typedef struct {
  bool parent_active;
  bool branch_taken;
  bool else_seen;
} c_pp_cond_t;

typedef struct {
  const char *name;
  size_t name_len;
  const char *parameters;
  size_t parameters_len;
  int64_t value;
  bool integer_value;
  const char *replacement;
  size_t replacement_len;
  bool function_like;
  char *owned_replacement;
  unsigned name_hash;
} c_pp_macro_t;

typedef struct {
  ny_lexer_t lexer;
  const c_pp_macro_t *macro;
  unsigned line;
  unsigned col;
} c_pp_expansion_t;

#define NYC_MAX_PREPROC_MACROS 65536u

static void strip_c_comments(char *copy, size_t source_len) {
  enum { C_SCAN_CODE, C_SCAN_STRING, C_SCAN_CHAR, C_SCAN_LINE, C_SCAN_BLOCK };
  int state = C_SCAN_CODE;
  bool escaped = false;
  bool directive_line = false;
  bool line_has_token = false;
  for (size_t i = 0; i < source_len; ++i) {
    char c = copy[i];
    char next = i + 1 < source_len ? copy[i + 1] : '\0';
    if (state == C_SCAN_CODE) {
      if (c == '\n') {
        if (!i || copy[i - 1] != '\\')
          directive_line = false;
        line_has_token = false;
        continue;
      }
      if (!line_has_token && isspace((unsigned char)c))
        continue;
      if (!line_has_token && c == '#')
        directive_line = true;
      if (!isspace((unsigned char)c))
        line_has_token = true;
      if (c == '"' || c == '\'') {
        state = c == '"' ? C_SCAN_STRING : C_SCAN_CHAR;
      } else if (c == '/' && next == '/') {
        copy[i++] = ' ';
        copy[i] = ' ';
        state = C_SCAN_LINE;
      } else if (c == '/' && next == '*') {
        copy[i++] = ' ';
        copy[i] = ' ';
        state = C_SCAN_BLOCK;
      }
      continue;
    }
    if (state == C_SCAN_LINE) {
      if (c == '\\' && next == '\n') {
        ++i;
        continue;
      }
      if (c == '\\' && next == '\r' && i + 2 < source_len &&
          copy[i + 2] == '\n') {
        i += 2;
        continue;
      }
      if (c == '\n') {
        state = C_SCAN_CODE;
        if (!i || copy[i - 1] != '\\')
          directive_line = false;
        line_has_token = false;
      } else {
        copy[i] = ' ';
      }
      continue;
    }
    if (state == C_SCAN_BLOCK) {
      if (c == '\\' && next == '\n') {
        ++i;
        continue;
      }
      if (c == '\\' && next == '\r' && i + 2 < source_len &&
          copy[i + 2] == '\n') {
        i += 2;
        continue;
      }
      if (c == '*' && next == '/') {
        copy[i++] = ' ';
        copy[i] = ' ';
        state = C_SCAN_CODE;
      } else if (c == '\n') {
        if (directive_line && i && copy[i - 1] != '\\')
          copy[i - 1] = '\\';
        line_has_token = false;
      } else {
        copy[i] = ' ';
      }
      continue;
    }
    if (escaped) {
      escaped = false;
      continue;
    }
    if (c == '\\') {
      escaped = true;
      continue;
    }
    if ((state == C_SCAN_STRING && c == '"') ||
        (state == C_SCAN_CHAR && c == '\''))
      state = C_SCAN_CODE;
  }
}

static void preproc_skip_space(const char *s, size_t n, size_t *pos) {
  for (;;) {
    while (*pos < n && (s[*pos] == ' ' || s[*pos] == '\t' || s[*pos] == '\r'))
      ++*pos;
    if (*pos + 1 < n && s[*pos] == '/' && s[*pos + 1] == '*') {
      *pos += 2;
      while (*pos + 1 < n && !(s[*pos] == '*' && s[*pos + 1] == '/'))
        ++*pos;
      if (*pos + 1 < n)
        *pos += 2;
      else
        *pos = n;
      continue;
    }
    if (*pos + 1 < n && s[*pos] == '/' && s[*pos + 1] == '/') {
      *pos = n;
      return;
    }
    if (*pos + 1 < n && s[*pos] == '\\' && s[*pos + 1] == '\n') {
      *pos += 2;
      continue;
    }
    if (*pos + 2 < n && s[*pos] == '\\' && s[*pos + 1] == '\r' &&
        s[*pos + 2] == '\n') {
      *pos += 3;
      continue;
    }
    break;
  }
}

static bool preproc_read_identifier(ny_ctok_t token, size_t *pos,
                                    const char **name, size_t *name_len) {
  preproc_skip_space(token.start, token.len, pos);
  size_t start = *pos;
  if (start >= token.len ||
      !(token.start[start] == '_' ||
        (token.start[start] >= 'A' && token.start[start] <= 'Z') ||
        (token.start[start] >= 'a' && token.start[start] <= 'z')))
    return false;
  ++*pos;
  while (*pos < token.len) {
    char c = token.start[*pos];
    if (!(c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9')))
      break;
    ++*pos;
  }
  *name = token.start + start;
  *name_len = *pos - start;
  return true;
}

/*
 * Attribute vocabulary the C frontend accepts (dropped or categorized):
 * mirrored by `__has_attribute` so availability-guarded macros in kernel
 * headers (`__must_check`, `__noreturn`, ...) define as designed while
 * unsupported attributes degrade to empty the way GCC does.
 */
static bool preproc_attribute_accepted(const char *name, size_t name_len) {
  static const char *const accepted[] = {"aligned",
                                         "packed",
                                         "section",
                                         "format",
                                         "gnu_inline",
                                         "unused",
                                         "used",
                                         "noinline",
                                         "no_stack_protector",
                                         "flatten",
                                         "ms_abi",
                                         "sysv_abi",
                                         "always_inline",
                                         "warn_unused_result",
                                         "noreturn",
                                         "pure",
                                         "no_instrument_function",
                                         "noclone",
                                         "no_profile_instrument_function",
                                         "nonstring"};
  for (size_t i = 0; i < sizeof(accepted) / sizeof(accepted[0]); ++i) {
    const char *spelling = accepted[i];
    size_t spelling_len = strlen(spelling);
    if (spelling_len == name_len && memcmp(spelling, name, name_len) == 0)
      return true;
    char underscored[64];
    if (spelling_len + 4 >= sizeof(underscored))
      continue;
    underscored[0] = '_';
    underscored[1] = '_';
    memcpy(underscored + 2, spelling, spelling_len);
    underscored[spelling_len + 2] = '_';
    underscored[spelling_len + 3] = '_';
    underscored[spelling_len + 4] = '\0';
    if (spelling_len + 4 == name_len &&
        memcmp(underscored, name, name_len) == 0)
      return true;
  }
  return false;
}

static unsigned preproc_macro_hash(const char *name, size_t name_len) {
  unsigned h = 2166136261u;
  for (size_t i = 0; i < name_len; ++i)
    h = (h ^ (unsigned char)name[i]) * 16777619u;
  return h ? h : 1u;
}

typedef struct {
  unsigned hash;
  size_t idx;
} c_pp_hash_slot_t;
static c_pp_hash_slot_t *g_pp_hash;
static size_t g_pp_hash_mask;
static const c_pp_macro_t *g_pp_hash_macros;
static size_t g_pp_hash_macro_count;

static void preproc_hash_reset(void) {
  free(g_pp_hash);
  g_pp_hash = NULL;
  g_pp_hash_mask = 0;
  g_pp_hash_macros = NULL;
  g_pp_hash_macro_count = 0;
}

static void preproc_hash_rebuild(const c_pp_macro_t *macros,
                                 size_t macro_count) {
  size_t cap = 1024;
  while (cap < macro_count * 2)
    cap <<= 1;
  c_pp_hash_slot_t *slots = calloc(cap, sizeof(*slots));
  if (!slots) {
    preproc_hash_reset();
    return;
  }
  size_t mask = cap - 1;
  for (size_t i = 0; i < macro_count; ++i) {
    unsigned h = macros[i].name_hash ? macros[i].name_hash : 1u;
    size_t p = h & mask;
    while (slots[p].idx)
      p = (p + 1) & mask;
    slots[p].hash = h;
    slots[p].idx = i + 1;
  }
  free(g_pp_hash);
  g_pp_hash = slots;
  g_pp_hash_mask = mask;
  g_pp_hash_macros = macros;
  g_pp_hash_macro_count = macro_count;
}

static void preproc_hash_insert_one(const c_pp_macro_t *macros, size_t i) {
  if (!g_pp_hash || g_pp_hash_macros != macros)
    return;
  if ((g_pp_hash_mask + 1) < (i + 2) * 2) {
    preproc_hash_rebuild(macros, i + 1);
    return;
  }
  unsigned h = macros[i].name_hash ? macros[i].name_hash : 1u;
  size_t p = h & g_pp_hash_mask;
  while (g_pp_hash[p].idx)
    p = (p + 1) & g_pp_hash_mask;
  g_pp_hash[p].hash = h;
  g_pp_hash[p].idx = i + 1;
  g_pp_hash_macro_count = i + 1;
}

static void preproc_hash_ensure(const c_pp_macro_t *macros,
                                size_t macro_count) {
  if (!g_pp_hash || g_pp_hash_macros != macros) {
    preproc_hash_rebuild(macros, macro_count);
    return;
  }
  if (g_pp_hash_macro_count == macro_count)
    return;
  if (g_pp_hash_macro_count == macro_count - 1) {
    preproc_hash_insert_one(macros, macro_count - 1);
    return;
  }
  preproc_hash_rebuild(macros, macro_count);
}

static const c_pp_macro_t *preproc_find_macro(const c_pp_macro_t *macros,
                                              size_t macro_count,
                                              const char *name,
                                              size_t name_len) {
  unsigned h = preproc_macro_hash(name, name_len);
  preproc_hash_ensure(macros, macro_count);
  if (g_pp_hash && g_pp_hash_macros == macros) {
    size_t p = h & g_pp_hash_mask;
    for (;;) {
      c_pp_hash_slot_t *s = &g_pp_hash[p];
      if (!s->idx)
        return NULL;
      if (s->hash == h) {
        size_t i = s->idx - 1;
        if (i < macro_count && macros[i].name_len == name_len &&
            memcmp(macros[i].name, name, name_len) == 0)
          return &macros[i];
      }
      p = (p + 1) & g_pp_hash_mask;
    }
  }
  for (size_t i = 0; i < macro_count; ++i)
    if (macros[i].name_hash == h && macros[i].name_len == name_len &&
        memcmp(macros[i].name, name, name_len) == 0)
      return &macros[i];
  return NULL;
}

static bool preproc_macro_defined(const c_pp_macro_t *macros,
                                  size_t macro_count, const char *name,
                                  size_t name_len) {
  return preproc_find_macro(macros, macro_count, name, name_len) != NULL;
}

static bool parser_keep_macro_text(c_parser_t *p, char *text);

static bool preproc_update_macro(c_parser_t *p, c_pp_macro_t **macro_table,
                                 size_t *macro_count, size_t *macro_capacity,
                                 const char *name, size_t name_len,
                                 const char *parameters, size_t parameters_len,
                                 bool define, int64_t value, bool integer_value,
                                 const char *replacement,
                                 size_t replacement_len, bool function_like,
                                 char *owned_replacement) {
  c_pp_macro_t *macros = *macro_table;
  unsigned h = preproc_macro_hash(name, name_len);
  size_t index = 0;
  preproc_hash_ensure(macros, *macro_count);
  if (g_pp_hash && g_pp_hash_macros == macros) {
    size_t pslot = h & g_pp_hash_mask;
    index = *macro_count;
    for (;;) {
      c_pp_hash_slot_t *s = &g_pp_hash[pslot];
      if (!s->idx) {
        index = *macro_count;
        break;
      }
      if (s->hash == h) {
        size_t i = s->idx - 1;
        if (i < *macro_count && macros[i].name_len == name_len &&
            memcmp(macros[i].name, name, name_len) == 0) {
          index = i;
          break;
        }
      }
      pslot = (pslot + 1) & g_pp_hash_mask;
    }
  } else {
    while (index < *macro_count &&
           !(macros[index].name_hash == h &&
             macros[index].name_len == name_len &&
             memcmp(macros[index].name, name, name_len) == 0))
      ++index;
  }
  if (index < *macro_count) {
    /*
     * Earlier tokens can still borrow the old replacement after #undef or
     * redefinition. Keep it until the translation unit is destroyed.
     */
    if (!parser_keep_macro_text(p, macros[index].owned_replacement))
      return false;
    macros[index].owned_replacement = NULL;
  }
  if (define) {
    if (index < *macro_count) {
      macros[index].value = value;
      macros[index].integer_value = integer_value;
      macros[index].parameters = parameters;
      macros[index].parameters_len = parameters_len;
      macros[index].replacement = replacement;
      macros[index].replacement_len = replacement_len;
      macros[index].function_like = function_like;
      macros[index].owned_replacement = owned_replacement;
      macros[index].name_hash = h;
      return true;
    }
    if (*macro_count == *macro_capacity) {
      if (*macro_capacity >= NYC_MAX_PREPROC_MACROS)
        return false;
      size_t next = *macro_capacity * 2;
      if (next > NYC_MAX_PREPROC_MACROS)
        next = NYC_MAX_PREPROC_MACROS;
      c_pp_macro_t *grown = realloc(macros, next * sizeof(*grown));
      if (!grown)
        return false;
      macros = grown;
      *macro_table = macros;
      *macro_capacity = next;
      g_pp_hash_macros = macros;
    }
    macros[(*macro_count)++] = (c_pp_macro_t){
        .name = name,
        .name_len = name_len,
        .parameters = parameters,
        .parameters_len = parameters_len,
        .value = value,
        .integer_value = integer_value,
        .replacement = replacement,
        .replacement_len = replacement_len,
        .function_like = function_like,
        .owned_replacement = owned_replacement,
        .name_hash = h,
    };
    if (g_pp_hash_macros == macros)
      preproc_hash_insert_one(macros, *macro_count - 1);
    return true;
  }
  if (index == *macro_count)
    return true;
  memmove(&macros[index], &macros[index + 1],
          (*macro_count - index - 1) * sizeof(*macros));
  --*macro_count;
  if (g_pp_hash_macros == macros)
    g_pp_hash_macro_count = (size_t)-1;
  return true;
}

static bool parser_keep_macro_text(c_parser_t *p, char *text) {
  if (!text)
    return true;
  if (p->macro_storage_count == p->macro_storage_capacity) {
    size_t capacity =
        p->macro_storage_capacity ? p->macro_storage_capacity * 2 : 16;
    if (capacity < p->macro_storage_capacity ||
        capacity > SIZE_MAX / sizeof(*p->macro_storage))
      return false;
    char **grown = realloc(p->macro_storage, capacity * sizeof(*grown));
    if (!grown)
      return false;
    p->macro_storage = grown;
    p->macro_storage_capacity = capacity;
  }
  p->macro_storage[p->macro_storage_count++] = text;
  return true;
}

static bool pp_append(char **buffer, size_t *length, size_t *capacity,
                      const char *text, size_t text_len) {
  if (text_len > SIZE_MAX - *length - 1)
    return false;
  size_t needed = *length + text_len + 1;
  if (needed > *capacity) {
    size_t next = *capacity ? *capacity : 128;
    while (next < needed) {
      if (next > SIZE_MAX / 2) {
        next = needed;
        break;
      }
      next *= 2;
    }
    char *grown = realloc(*buffer, next);
    if (!grown)
      return false;
    *buffer = grown;
    *capacity = next;
  }
  memcpy(*buffer + *length, text, text_len);
  *length += text_len;
  (*buffer)[*length] = '\0';
  return true;
}

static bool pp_identifier_start(char c) {
  return ny_ascii_identifier_start((unsigned char)c);
}

static bool pp_identifier_continue(char c) {
  return ny_ascii_identifier_continue((unsigned char)c);
}

static bool pp_filename_replacement(c_parser_t *p, const char *path,
                                    char **replacement, size_t *length) {
  const nyc_path_map_t *best = NULL;
  size_t best_length = 0;
  for (size_t i = 0; i < p->path_map_count; ++i) {
    const nyc_path_map_t *map = &p->path_maps[i];
    size_t prefix = strlen(map->from);
    if (strncmp(path, map->from, prefix) == 0) {
      best = map;
      best_length = prefix;
    }
  }
  const char *mapped = path;
  if (best) {
    mapped = best->to;
  }
  char *text = NULL;
  size_t used = 0, capacity = 0;
  if (!pp_append(&text, &used, &capacity, "\"", 1))
    goto fail;
  if (best && !pp_append(&text, &used, &capacity, mapped, strlen(mapped)))
    goto fail;
  const char *suffix = best ? path + best_length : path;
  for (const unsigned char *at = (const unsigned char *)suffix; *at; ++at) {
    char escaped[5];
    size_t escaped_length = 1;
    escaped[0] = (char)*at;
    if (*at == '\\' || *at == '"') {
      escaped[0] = '\\';
      escaped[1] = (char)*at;
      escaped_length = 2;
    } else if (*at < 32 || *at == 127) {
      int count = snprintf(escaped, sizeof(escaped), "\\%03o", *at);
      if (count != 4)
        goto fail;
      escaped_length = 4;
    }
    if (!pp_append(&text, &used, &capacity, escaped, escaped_length))
      goto fail;
  }
  if (!pp_append(&text, &used, &capacity, "\"", 1))
    goto fail;
  *replacement = text;
  *length = used;
  return true;
fail:
  free(text);
  return false;
}

static bool pp_token_is(ny_ctok_t token, const char *text) {
  size_t len = strlen(text);
  return token.kind == NY_CTOK_PUNCT && token.len == len &&
         memcmp(token.start, text, len) == 0;
}

static bool expand_pp_identifier(c_parser_t *p, const c_pp_macro_t *macros,
                                 size_t macro_count, ny_ctok_t token,
                                 ny_lexer_t *input,
                                 c_pp_expansion_t *expansions,
                                 size_t *expansion_depth,
                                 size_t expansion_floor,
                                 ny_lexer_t *outer_input, bool *expanded);

static bool pp_tokens_need_space(ny_ctok_t left, ny_ctok_t right) {
  char last = left.start[left.len - 1], first = right.start[0];
  if ((left.kind == NY_CTOK_IDENT || left.kind == NY_CTOK_NUMBER) &&
      pp_identifier_continue(first))
    return true;
  if ((left.kind == NY_CTOK_NUMBER && first == '.') ||
      (last == '.' && right.kind == NY_CTOK_NUMBER))
    return true;
  if (left.kind == NY_CTOK_NUMBER && (first == '+' || first == '-') &&
      (last == 'e' || last == 'E' || last == 'p' || last == 'P'))
    return true;
  if (left.kind == NY_CTOK_IDENT &&
      (right.kind == NY_CTOK_STRING || right.kind == NY_CTOK_CHAR) &&
      ((left.len == 1 && (last == 'L' || last == 'u' || last == 'U')) ||
       (left.len == 2 && memcmp(left.start, "u8", 2) == 0)))
    return true;
  if (left.kind == NY_CTOK_PUNCT && right.kind == NY_CTOK_PUNCT) {
    char joined[8];
    if (left.len + right.len >= sizeof(joined) || (last == '.' && first == '.'))
      return true;
    memcpy(joined, left.start, left.len);
    memcpy(joined + left.len, right.start, right.len);
    ny_lexer_t lexer;
    ny_lex_init(&lexer, joined, left.len + right.len);
    ny_ctok_t token = ny_lex_next(&lexer);
    return token.kind == NY_CTOK_ERROR || token.len != left.len;
  }
  return false;
}

static bool pp_texts_need_space(const char *left, size_t left_len,
                                const char *right, size_t right_len) {
  if (!left_len || !right_len)
    return false;
  ny_lexer_t lexer;
  ny_lex_init(&lexer, left, left_len);
  ny_ctok_t last = {0};
  for (;;) {
    ny_ctok_t token = ny_lex_next(&lexer);
    if (token.kind == NY_CTOK_EOF)
      break;
    if (token.kind == NY_CTOK_ERROR)
      return true;
    last = token;
  }
  ny_lex_init(&lexer, right, right_len);
  ny_ctok_t first = ny_lex_next(&lexer);
  if (first.kind == NY_CTOK_ERROR)
    return true;
  return last.kind != NY_CTOK_EOF && first.kind != NY_CTOK_EOF &&
         pp_tokens_need_space(last, first);
}

/*
 * Reuse the normal rescan stack, retaining its disabled macros. Arguments
 * without an expansion borrow their original text and allocate nothing.
 */
static bool expand_macro_argument(c_parser_t *p, const c_pp_macro_t *macros,
                                  size_t macro_count, const char *text,
                                  size_t text_len, unsigned line, unsigned col,
                                  c_pp_expansion_t *expansions,
                                  size_t *expansion_depth, char **result,
                                  size_t *result_len) {
  size_t base_depth = *expansion_depth;
  ny_lexer_t lexer;
  ny_lex_init(&lexer, text, text_len);
  char *output = NULL;
  size_t length = 0, capacity = 0;
  ny_ctok_t previous = {0};
  const char *previous_source = NULL;
  bool changed = false, pending_space = false;
  for (;;) {
    ny_lexer_t *input = *expansion_depth > base_depth
                            ? &expansions[*expansion_depth - 1].lexer
                            : &lexer;
    const char *source = input->src;
    size_t before = input->pos;
    ny_ctok_t token = ny_lex_next(input);
    if (token.kind == NY_CTOK_EOF) {
      if (*expansion_depth == base_depth)
        break;
      --*expansion_depth;
      continue;
    }
    if (token.kind == NY_CTOK_ERROR)
      goto fail;
    bool separated = (size_t)(token.start - source) > before;
    token.line = line;
    token.col = col;
    if (token.kind == NY_CTOK_IDENT) {
      bool expanded;
      if (!expand_pp_identifier(p, macros, macro_count, token, input,
                                expansions, expansion_depth, base_depth, &lexer,
                                &expanded))
        goto fail;
      if (expanded) {
        if (!changed && !pp_append(&output, &length, &capacity, text,
                                   (size_t)(token.start - text)))
          goto fail;
        changed = true;
        pending_space |= separated;
        continue;
      }
    }
    if (changed) {
      separated |=
          pending_space || (previous_source && previous_source != source &&
                            pp_tokens_need_space(previous, token));
      if (separated && length && !isspace((unsigned char)output[length - 1]) &&
          !pp_append(&output, &length, &capacity, " ", 1))
        goto fail;
      if (!pp_append(&output, &length, &capacity, token.start, token.len))
        goto fail;
    }
    previous = token;
    previous_source = source;
    pending_space = false;
  }
  if (changed && !output && !pp_append(&output, &length, &capacity, "", 0))
    goto fail;
  *result = output;
  *result_len = length;
  return true;
fail:
  *expansion_depth = base_depth;
  free(output);
  return false;
}

static bool preproc_directive(ny_ctok_t token, const char *name,
                              size_t *argument);
static int preproc_condition(c_parser_t *p, ny_ctok_t token, size_t argument,
                             const c_pp_macro_t *macros, size_t macro_count,
                             c_pp_expansion_t *expansions,
                             size_t *expansion_depth);

static bool expand_function_macro(c_parser_t *p, const c_pp_macro_t *macros,
                                  size_t macro_count, const c_pp_macro_t *macro,
                                  ny_lexer_t *input, unsigned line,
                                  unsigned col, c_pp_expansion_t *expansions,
                                  size_t *expansion_depth,
                                  size_t expansion_floor,
                                  ny_lexer_t *outer_input, bool *expanded) {
  *expanded = false;
  ny_lexer_t saved = *input;
  ny_ctok_t token = ny_lex_next(input);
  while (token.kind == NY_CTOK_EOF && *expansion_depth > expansion_floor &&
         input == &expansions[*expansion_depth - 1].lexer) {
    --*expansion_depth;
    input = *expansion_depth == expansion_floor
                ? outer_input
                : &expansions[*expansion_depth - 1].lexer;
    saved = *input;
    token = ny_lex_next(input);
    if (*expansion_depth == expansion_floor && !pp_token_is(token, "(")) {
      *input = saved;
      return true;
    }
  }
  if (!pp_token_is(token, "(")) {
    *input = saved;
    return true;
  }
  bool variadic_only = false;
  size_t variadic_pos = 0;
  while (variadic_pos < macro->parameters_len &&
         (isspace((unsigned char)macro->parameters[variadic_pos]) ||
          macro->parameters[variadic_pos] == '\\'))
    ++variadic_pos;
  if (variadic_pos + 2 < macro->parameters_len &&
      memcmp(macro->parameters + variadic_pos, "...", 3) == 0) {
    variadic_pos += 3;
  } else if (variadic_pos < macro->parameters_len &&
             pp_identifier_start(macro->parameters[variadic_pos])) {
    while (variadic_pos < macro->parameters_len &&
           pp_identifier_continue(macro->parameters[variadic_pos]))
      ++variadic_pos;
    while (variadic_pos < macro->parameters_len &&
           (isspace((unsigned char)macro->parameters[variadic_pos]) ||
            macro->parameters[variadic_pos] == '\\'))
      ++variadic_pos;
    if (variadic_pos + 2 < macro->parameters_len &&
        memcmp(macro->parameters + variadic_pos, "...", 3) == 0)
      variadic_pos += 3;
    else
      variadic_pos = macro->parameters_len + 1;
  } else {
    variadic_pos = macro->parameters_len + 1;
  }
  if (variadic_pos <= macro->parameters_len) {
    while (variadic_pos < macro->parameters_len &&
           (isspace((unsigned char)macro->parameters[variadic_pos]) ||
            macro->parameters[variadic_pos] == '\\'))
      ++variadic_pos;
    variadic_only = variadic_pos == macro->parameters_len;
  }
  /*
   * Linux trace macros use a variadic PARAMS wrapper around expanded field
   * lists. Those lists can exceed the ordinary C function-macro argument
   * counts, so retain enough slots for generated kernel invocations.
   */
  enum { MAX_ARGS = 1024 };
  char *args[MAX_ARGS] = {0};
  size_t arg_lens[MAX_ARGS] = {0}, arg_caps[MAX_ARGS] = {0};
  char *expanded_args[MAX_ARGS] = {0};
  size_t expanded_lens[MAX_ARGS] = {0};
  bool prescanned[MAX_ARGS] = {0};
  size_t arg_count = 0;
  const char *previous_end = NULL;
  bool current_arg_has_token = false;
  c_pp_cond_t argument_conditions[NY_C_MAX_COND_STACK];
  size_t argument_condition_depth = 0;
  size_t outer_branch_depth = SIZE_MAX;
  bool argument_active = true;
  unsigned nesting = 1;
  bool ok = false;
  while (nesting) {
    token = ny_lex_next(input);
    if (token.kind == NY_CTOK_EOF)
      do {
        if (getenv("NYC_PP_TRACE"))
          fprintf(stderr, "PP-TRACE goto done at line %d\n", __LINE__);
        goto done;
      } while (0);
    if (token.kind == NY_CTOK_PREPROC) {
      size_t argument = 0;
      bool is_if = preproc_directive(token, "if", &argument);
      bool is_ifdef = preproc_directive(token, "ifdef", &argument);
      bool is_ifndef = preproc_directive(token, "ifndef", &argument);
      if (is_if || is_ifdef || is_ifndef) {
        if (argument_condition_depth == NY_C_MAX_COND_STACK)
          do {
            if (getenv("NYC_PP_TRACE"))
              fprintf(stderr, "PP-TRACE goto done at line %d\n", __LINE__);
            goto done;
          } while (0);
        bool condition = false;
        if (argument_active) {
          if (is_if) {
            int evaluated =
                preproc_condition(p, token, argument, macros, macro_count,
                                  expansions, expansion_depth);
            if (evaluated < 0)
              do {
                if (getenv("NYC_PP_TRACE"))
                  fprintf(stderr, "PP-TRACE goto done at line %d\n", __LINE__);
                goto done;
              } while (0);
            condition = evaluated != 0;
          } else {
            const char *name = NULL;
            size_t name_len = 0;
            if (!preproc_read_identifier(token, &argument, &name, &name_len))
              do {
                if (getenv("NYC_PP_TRACE"))
                  fprintf(stderr, "PP-TRACE goto done at line %d\n", __LINE__);
                goto done;
              } while (0);
            bool defined =
                preproc_macro_defined(macros, macro_count, name, name_len);
            condition = is_ifdef ? defined : !defined;
          }
        }
        argument_conditions[argument_condition_depth++] =
            (c_pp_cond_t){.parent_active = argument_active,
                          .branch_taken = argument_active && condition};
        argument_active = argument_active && condition;
        continue;
      }
      if (preproc_directive(token, "else", &argument)) {
        if (!argument_condition_depth) {
          if (argument_condition_depth == NY_C_MAX_COND_STACK)
            goto done;
          argument_conditions[argument_condition_depth++] = (c_pp_cond_t){
              .parent_active = true, .branch_taken = true, .else_seen = true};
          outer_branch_depth = argument_condition_depth;
          argument_active = false;
          continue;
        }
        c_pp_cond_t *condition =
            &argument_conditions[argument_condition_depth - 1];
        if (condition->else_seen)
          goto done;
        condition->else_seen = true;
        argument_active = condition->parent_active && !condition->branch_taken;
        condition->branch_taken |= argument_active;
        continue;
      }
      if (preproc_directive(token, "elif", &argument)) {
        if (!argument_condition_depth) {
          if (argument_condition_depth == NY_C_MAX_COND_STACK)
            goto done;
          argument_conditions[argument_condition_depth++] =
              (c_pp_cond_t){.parent_active = true, .branch_taken = true};
          outer_branch_depth = argument_condition_depth;
          argument_active = false;
          continue;
        }
        c_pp_cond_t *condition =
            &argument_conditions[argument_condition_depth - 1];
        if (condition->else_seen)
          goto done;
        int evaluated =
            condition->parent_active && !condition->branch_taken
                ? preproc_condition(p, token, argument, macros, macro_count,
                                    expansions, expansion_depth)
                : 0;
        if (evaluated < 0)
          goto done;
        argument_active = condition->parent_active &&
                          !condition->branch_taken && evaluated != 0;
        condition->branch_taken |= argument_active;
        continue;
      }
      if (preproc_directive(token, "endif", &argument)) {
        if (!argument_condition_depth) {
          if (p->pp_consumed_endif == SIZE_MAX)
            goto done;
          ++p->pp_consumed_endif;
          continue;
        }
        argument_active =
            argument_conditions[--argument_condition_depth].parent_active;
        if (outer_branch_depth == argument_condition_depth + 1) {
          outer_branch_depth = SIZE_MAX;
          if (p->pp_consumed_endif == SIZE_MAX)
            goto done;
          ++p->pp_consumed_endif;
        }
        continue;
      }
      goto done;
    }
    if (!argument_active)
      continue;
    if (pp_token_is(token, "(")) {
      ++nesting;
    } else if (pp_token_is(token, ")")) {
      if (--nesting == 0) {
        if (current_arg_has_token || arg_count || macro->parameters_len) {
          if (arg_count == MAX_ARGS)
            do {
              if (getenv("NYC_PP_TRACE"))
                fprintf(stderr, "PP-TRACE goto done at line %d\n", __LINE__);
              goto done;
            } while (0);
          ++arg_count;
        }
        break;
      }
    } else if (pp_token_is(token, ",") && nesting == 1) {
      if (variadic_only) {
        size_t index = arg_count;
        if (current_arg_has_token && token.start != previous_end &&
            !pp_append(&args[index], &arg_lens[index], &arg_caps[index], " ",
                       1))
          goto done;
        if (!pp_append(&args[index], &arg_lens[index], &arg_caps[index],
                       token.start, token.len))
          goto done;
        current_arg_has_token = true;
        previous_end = token.start + token.len;
        continue;
      }
      if (arg_count == MAX_ARGS)
        do {
          if (getenv("NYC_PP_TRACE"))
            fprintf(stderr, "PP-TRACE goto done at line %d\n", __LINE__);
          goto done;
        } while (0);
      ++arg_count;
      current_arg_has_token = false;
      continue;
    }
    if (arg_count == MAX_ARGS)
      do {
        if (getenv("NYC_PP_TRACE"))
          fprintf(stderr, "PP-TRACE goto done at line %d\n", __LINE__);
        goto done;
      } while (0);
    size_t index = arg_count;
    if (current_arg_has_token && token.start != previous_end &&
        !pp_append(&args[index], &arg_lens[index], &arg_caps[index], " ", 1))
      do {
        if (getenv("NYC_PP_TRACE"))
          fprintf(stderr, "PP-TRACE goto done at line %d\n", __LINE__);
        goto done;
      } while (0);
    if (!pp_append(&args[index], &arg_lens[index], &arg_caps[index],
                   token.start, token.len))
      do {
        if (getenv("NYC_PP_TRACE"))
          fprintf(stderr, "PP-TRACE goto done at line %d\n", __LINE__);
        goto done;
      } while (0);
    current_arg_has_token = true;
    previous_end = token.start + token.len;
  }
  if (argument_condition_depth) {
    if (outer_branch_depth != SIZE_MAX || p->pp_pending_condition_depth ||
        argument_condition_depth >
            sizeof(p->pp_pending_condition_parent) /
                sizeof(p->pp_pending_condition_parent[0]))
      goto done;
    p->pp_pending_condition_depth = argument_condition_depth;
    p->pp_pending_active = argument_active;
    for (size_t i = 0; i < argument_condition_depth; ++i) {
      p->pp_pending_condition_parent[i] = argument_conditions[i].parent_active;
      p->pp_pending_condition_taken[i] = argument_conditions[i].branch_taken;
      p->pp_pending_condition_else_seen[i] = argument_conditions[i].else_seen;
    }
  }

  size_t parameter_pos = 0, parameter_count = 0;
  bool variadic = false;
  const char *variadic_name = "__VA_ARGS__";
  size_t variadic_name_len = sizeof("__VA_ARGS__") - 1;
  while (parameter_pos < macro->parameters_len) {
    while (parameter_pos < macro->parameters_len &&
           (isspace((unsigned char)macro->parameters[parameter_pos]) ||
            macro->parameters[parameter_pos] == ',' ||
            macro->parameters[parameter_pos] == '\\'))
      ++parameter_pos;
    if (parameter_pos == macro->parameters_len)
      break;
    if (parameter_pos + 2 < macro->parameters_len &&
        memcmp(macro->parameters + parameter_pos, "...", 3) == 0) {
      parameter_pos += 3;
      while (parameter_pos < macro->parameters_len &&
             (isspace((unsigned char)macro->parameters[parameter_pos]) ||
              macro->parameters[parameter_pos] == '\\'))
        ++parameter_pos;
      if (parameter_pos != macro->parameters_len)
        do {
          if (getenv("NYC_PP_TRACE"))
            fprintf(stderr, "PP-TRACE goto done at line %d\n", __LINE__);
          goto done;
        } while (0);
      variadic = true;
      break;
    }
    if (!pp_identifier_start(macro->parameters[parameter_pos])) {
      if (getenv("NYC_PP_TRACE"))
        fprintf(stderr,
                "PP-TRACE param-list invalid: macro `%.*s` params=`%.120s` "
                "pos=%zu char=0x%x\n",
                (int)macro->name_len, macro->name, macro->parameters,
                parameter_pos,
                (unsigned char)(parameter_pos < macro->parameters_len
                                    ? macro->parameters[parameter_pos]
                                    : '?'));
      goto done;
    }
    size_t name_start = parameter_pos;
    while (parameter_pos < macro->parameters_len &&
           pp_identifier_continue(macro->parameters[parameter_pos]))
      ++parameter_pos;
    size_t name_len = parameter_pos - name_start;
    while (parameter_pos < macro->parameters_len &&
           (isspace((unsigned char)macro->parameters[parameter_pos]) ||
            macro->parameters[parameter_pos] == '\\'))
      ++parameter_pos;
    if (parameter_pos + 2 < macro->parameters_len &&
        memcmp(macro->parameters + parameter_pos, "...", 3) == 0) {
      parameter_pos += 3;
      while (parameter_pos < macro->parameters_len &&
             (isspace((unsigned char)macro->parameters[parameter_pos]) ||
              macro->parameters[parameter_pos] == '\\'))
        ++parameter_pos;
      if (parameter_pos != macro->parameters_len)
        do {
          if (getenv("NYC_PP_TRACE"))
            fprintf(stderr, "PP-TRACE goto done at line %d\n", __LINE__);
          goto done;
        } while (0);
      variadic = true;
      variadic_name = macro->parameters + name_start;
      variadic_name_len = name_len;
      break;
    }
    ++parameter_count;
    if (parameter_pos < macro->parameters_len &&
        macro->parameters[parameter_pos] != ',')
      do {
        if (getenv("NYC_PP_TRACE"))
          fprintf(stderr, "PP-TRACE goto done at line %d\n", __LINE__);
        goto done;
      } while (0);
  }
  if ((!variadic && parameter_count != arg_count) ||
      (variadic && arg_count < parameter_count) ||
      *expansion_depth == NY_C_MAX_RECURSION_DEPTH)
    do {
      if (getenv("NYC_PP_TRACE"))
        fprintf(stderr, "PP-TRACE goto done at line %d\n", __LINE__);
      goto done;
    } while (0);

  char *replacement = NULL;
  size_t replacement_len = 0, replacement_cap = 0;
  const char *body = macro->replacement;
  size_t body_len = macro->replacement_len;
  size_t pos = 0;
  bool paste_next = false;
  char quote = 0;
  while (pos < body_len) {
    char ch = body[pos];
    if (quote) {
      if (!pp_append(&replacement, &replacement_len, &replacement_cap, &ch, 1))
        do {
          if (getenv("NYC_PP_TRACE"))
            fprintf(stderr, "PP-TRACE goto expansion_done at line %d\n",
                    __LINE__);
          goto expansion_done;
        } while (0);
      ++pos;
      if (ch == '\\' && pos < body_len) {
        if (!pp_append(&replacement, &replacement_len, &replacement_cap,
                       body + pos, 1))
          do {
            if (getenv("NYC_PP_TRACE"))
              fprintf(stderr, "PP-TRACE goto expansion_done at line %d\n",
                      __LINE__);
            goto expansion_done;
          } while (0);
        ++pos;
      } else if (ch == quote) {
        quote = 0;
      }
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      if (!pp_append(&replacement, &replacement_len, &replacement_cap, &ch, 1))
        do {
          if (getenv("NYC_PP_TRACE"))
            fprintf(stderr, "PP-TRACE goto expansion_done at line %d\n",
                    __LINE__);
          goto expansion_done;
        } while (0);
      ++pos;
      continue;
    }
    if (ch == '#') {
      size_t parameter_start = pos + 1;
      while (parameter_start < body_len &&
             isspace((unsigned char)body[parameter_start]))
        ++parameter_start;
      if (parameter_start < body_len &&
          pp_identifier_start(body[parameter_start])) {
        size_t parameter_end = parameter_start + 1;
        while (parameter_end < body_len &&
               pp_identifier_continue(body[parameter_end]))
          ++parameter_end;
        size_t parameter_cursor = 0, arg_index = 0;
        bool matched = false;
        bool matched_variadic = false;
        while (parameter_cursor < macro->parameters_len) {
          while (parameter_cursor < macro->parameters_len &&
                 (isspace((unsigned char)macro->parameters[parameter_cursor]) ||
                  macro->parameters[parameter_cursor] == ',' ||
                  macro->parameters[parameter_cursor] == '\\'))
            ++parameter_cursor;
          if (parameter_cursor + 2 < macro->parameters_len &&
              memcmp(macro->parameters + parameter_cursor, "...", 3) == 0)
            break;
          size_t param_start = parameter_cursor;
          while (parameter_cursor < macro->parameters_len &&
                 pp_identifier_continue(macro->parameters[parameter_cursor]))
            ++parameter_cursor;
          if (parameter_cursor - param_start ==
                  parameter_end - parameter_start &&
              memcmp(macro->parameters + param_start, body + parameter_start,
                     parameter_end - parameter_start) == 0) {
            matched = true;
            matched_variadic =
                variadic &&
                parameter_cursor - param_start == variadic_name_len &&
                memcmp(macro->parameters + param_start, variadic_name,
                       variadic_name_len) == 0;
            break;
          }
          ++arg_index;
        }
        if (matched) {
          if (!pp_append(&replacement, &replacement_len, &replacement_cap, "\"",
                         1))
            do {
              if (getenv("NYC_PP_TRACE"))
                fprintf(stderr, "PP-TRACE goto expansion_done at line %d\n",
                        __LINE__);
              goto expansion_done;
            } while (0);
          size_t first_stringified_arg =
              matched_variadic ? parameter_count : arg_index;
          size_t stringified_arg_end =
              matched_variadic ? arg_count : arg_index + 1;
          for (size_t argument_index = first_stringified_arg;
               argument_index < stringified_arg_end; ++argument_index) {
            size_t arg_start = 0, arg_end = arg_lens[argument_index];
            if (argument_index > first_stringified_arg &&
                !pp_append(&replacement, &replacement_len, &replacement_cap,
                           ",", 1))
              do {
                if (getenv("NYC_PP_TRACE"))
                  fprintf(stderr, "PP-TRACE goto expansion_done at line %d\n",
                          __LINE__);
                goto expansion_done;
              } while (0);
            while (arg_start < arg_end &&
                   isspace((unsigned char)args[argument_index][arg_start]))
              ++arg_start;
            while (arg_end > arg_start &&
                   isspace((unsigned char)args[argument_index][arg_end - 1]))
              --arg_end;
            for (size_t char_index = arg_start; char_index < arg_end;
                 ++char_index) {
              if ((args[argument_index][char_index] == '"' ||
                   args[argument_index][char_index] == '\\') &&
                  !pp_append(&replacement, &replacement_len, &replacement_cap,
                             "\\", 1))
                do {
                  if (getenv("NYC_PP_TRACE"))
                    fprintf(stderr, "PP-TRACE goto expansion_done at line %d\n",
                            __LINE__);
                  goto expansion_done;
                } while (0);
              if (!pp_append(&replacement, &replacement_len, &replacement_cap,
                             args[argument_index] + char_index, 1))
                do {
                  if (getenv("NYC_PP_TRACE"))
                    fprintf(stderr, "PP-TRACE goto expansion_done at line %d\n",
                            __LINE__);
                  goto expansion_done;
                } while (0);
            }
          }
          if (!pp_append(&replacement, &replacement_len, &replacement_cap, "\"",
                         1))
            do {
              if (getenv("NYC_PP_TRACE"))
                fprintf(stderr, "PP-TRACE goto expansion_done at line %d\n",
                        __LINE__);
              goto expansion_done;
            } while (0);
          pos = parameter_end;
          continue;
        }
      }
    }
    if (ch == '#' && pos + 1 < body_len && body[pos + 1] == '#') {
      while (replacement_len &&
             isspace((unsigned char)replacement[replacement_len - 1]))
        replacement[--replacement_len] = '\0';
      pos += 2;
      while (pos < body_len && isspace((unsigned char)body[pos]))
        ++pos;
      paste_next = true;
      continue;
    }
    if (pp_identifier_start(ch)) {
      size_t start = pos++;
      while (pos < body_len && pp_identifier_continue(body[pos]))
        ++pos;
      size_t ident_len = pos - start;
      if (variadic && ident_len == variadic_name_len &&
          memcmp(body + start, variadic_name, ident_len) == 0) {
        size_t first_variadic_arg = parameter_count;
        bool empty_variadic = true;
        for (size_t i = first_variadic_arg; i < arg_count; ++i) {
          const char *argument = args[i] ? args[i] : "";
          size_t start = 0, end = arg_lens[i];
          while (start < end && isspace((unsigned char)argument[start]))
            ++start;
          while (end > start && isspace((unsigned char)argument[end - 1]))
            --end;
          if (end != start) {
            empty_variadic = false;
            break;
          }
        }
        if (paste_next && empty_variadic) {
          while (replacement_len &&
                 isspace((unsigned char)replacement[replacement_len - 1]))
            replacement[--replacement_len] = '\0';
          if (replacement_len && replacement[replacement_len - 1] == ',') {
            replacement[--replacement_len] = '\0';
            while (replacement_len &&
                   isspace((unsigned char)replacement[replacement_len - 1]))
              replacement[--replacement_len] = '\0';
          }
          paste_next = false;
          continue;
        }
        for (size_t i = first_variadic_arg; i < arg_count; ++i) {
          const char *argument = args[i] ? args[i] : "";
          size_t argument_len = arg_lens[i];
          size_t arg_start = 0;
          while (arg_start < argument_len &&
                 isspace((unsigned char)argument[arg_start]))
            ++arg_start;
          while (argument_len > arg_start &&
                 isspace((unsigned char)argument[argument_len - 1]))
            --argument_len;
          if (i > first_variadic_arg &&
              !pp_append(&replacement, &replacement_len, &replacement_cap, ",",
                         1))
            do {
              if (getenv("NYC_PP_TRACE"))
                fprintf(stderr, "PP-TRACE goto expansion_done at line %d\n",
                        __LINE__);
              goto expansion_done;
            } while (0);
          if (argument_len == arg_start)
            continue;
          if (!paste_next) {
            if (!prescanned[i]) {
              if (!expand_macro_argument(
                      p, macros, macro_count, argument + arg_start,
                      argument_len - arg_start, line, col, expansions,
                      expansion_depth, &expanded_args[i], &expanded_lens[i]))
                do {
                  if (getenv("NYC_PP_TRACE"))
                    fprintf(stderr, "PP-TRACE goto expansion_done at line %d\n",
                            __LINE__);
                  goto expansion_done;
                } while (0);
              prescanned[i] = true;
            }
            if (expanded_args[i]) {
              argument = expanded_args[i];
              argument_len = expanded_lens[i];
              arg_start = 0;
              while (arg_start < argument_len &&
                     isspace((unsigned char)argument[arg_start]))
                ++arg_start;
              while (argument_len > arg_start &&
                     isspace((unsigned char)argument[argument_len - 1]))
                --argument_len;
            }
          }
          if (!pp_append(&replacement, &replacement_len, &replacement_cap,
                         argument + arg_start, argument_len - arg_start))
            do {
              if (getenv("NYC_PP_TRACE"))
                fprintf(stderr, "PP-TRACE goto expansion_done at line %d\n",
                        __LINE__);
              goto expansion_done;
            } while (0);
        }
        paste_next = false;
        continue;
      }
      size_t parameter_cursor = 0, arg_index = 0;
      bool matched = false;
      while (parameter_cursor < macro->parameters_len) {
        while (parameter_cursor < macro->parameters_len &&
               (isspace((unsigned char)macro->parameters[parameter_cursor]) ||
                macro->parameters[parameter_cursor] == ',' ||
                macro->parameters[parameter_cursor] == '\\'))
          ++parameter_cursor;
        if (parameter_cursor + 2 < macro->parameters_len &&
            memcmp(macro->parameters + parameter_cursor, "...", 3) == 0)
          break;
        size_t param_start = parameter_cursor;
        while (parameter_cursor < macro->parameters_len &&
               pp_identifier_continue(macro->parameters[parameter_cursor]))
          ++parameter_cursor;
        if (parameter_cursor - param_start == ident_len &&
            memcmp(macro->parameters + param_start, body + start, ident_len) ==
                0) {
          matched = true;
          break;
        }
        ++arg_index;
      }
      if (matched) {
        if (arg_index < arg_count && args[arg_index]) {
          size_t following = pos;
          while (following < body_len &&
                 isspace((unsigned char)body[following]))
            ++following;
          bool pasted = paste_next ||
                        (following + 1 < body_len && body[following] == '#' &&
                         body[following + 1] == '#');
          if (!pasted && !prescanned[arg_index]) {
            if (!expand_macro_argument(
                    p, macros, macro_count, args[arg_index],
                    arg_lens[arg_index], line, col, expansions, expansion_depth,
                    &expanded_args[arg_index], &expanded_lens[arg_index]))
              do {
                if (getenv("NYC_PP_TRACE"))
                  fprintf(stderr, "PP-TRACE goto expansion_done at line %d\n",
                          __LINE__);
                goto expansion_done;
              } while (0);
            prescanned[arg_index] = true;
          }
          const char *argument = !pasted && expanded_args[arg_index]
                                     ? expanded_args[arg_index]
                                     : args[arg_index];
          size_t arg_start = 0;
          size_t arg_end = !pasted && expanded_args[arg_index]
                               ? expanded_lens[arg_index]
                               : arg_lens[arg_index];
          while (arg_start < arg_end &&
                 isspace((unsigned char)argument[arg_start]))
            ++arg_start;
          while (arg_end > arg_start &&
                 isspace((unsigned char)argument[arg_end - 1]))
            --arg_end;
          if (!paste_next && replacement_len && arg_end > arg_start &&
              !isspace((unsigned char)replacement[replacement_len - 1]) &&
              pp_texts_need_space(replacement, replacement_len,
                                  argument + arg_start, arg_end - arg_start) &&
              !pp_append(&replacement, &replacement_len, &replacement_cap, " ",
                         1))
            do {
              if (getenv("NYC_PP_TRACE"))
                fprintf(stderr, "PP-TRACE goto expansion_done at line %d\n",
                        __LINE__);
              goto expansion_done;
            } while (0);
          if (!pp_append(&replacement, &replacement_len, &replacement_cap,
                         argument + arg_start, arg_end - arg_start))
            do {
              if (getenv("NYC_PP_TRACE"))
                fprintf(stderr, "PP-TRACE goto expansion_done at line %d\n",
                        __LINE__);
              goto expansion_done;
            } while (0);
        }
        paste_next = false;
      } else {
        if (!pp_append(&replacement, &replacement_len, &replacement_cap,
                       body + start, ident_len))
          do {
            if (getenv("NYC_PP_TRACE"))
              fprintf(stderr, "PP-TRACE goto expansion_done at line %d\n",
                      __LINE__);
            goto expansion_done;
          } while (0);
        paste_next = false;
      }
      continue;
    }
    if (paste_next && isspace((unsigned char)ch)) {
      ++pos;
      continue;
    }
    if (!pp_append(&replacement, &replacement_len, &replacement_cap, &ch, 1))
      do {
        if (getenv("NYC_PP_TRACE"))
          fprintf(stderr, "PP-TRACE goto expansion_done at line %d\n",
                  __LINE__);
        goto expansion_done;
      } while (0);
    paste_next = false;
    ++pos;
  }
  if (!replacement &&
      !pp_append(&replacement, &replacement_len, &replacement_cap, "", 0))
    do {
      if (getenv("NYC_PP_TRACE"))
        fprintf(stderr, "PP-TRACE goto expansion_done at line %d\n", __LINE__);
      goto expansion_done;
    } while (0);
  if (!parser_keep_macro_text(p, replacement))
    do {
      if (getenv("NYC_PP_TRACE"))
        fprintf(stderr, "PP-TRACE goto expansion_done at line %d\n", __LINE__);
      goto expansion_done;
    } while (0);
  c_pp_expansion_t *expansion = &expansions[(*expansion_depth)++];
  ny_lex_init(&expansion->lexer, replacement, replacement_len);
  expansion->macro = macro;
  expansion->line = line;
  expansion->col = col;
  replacement = NULL;
  *expanded = true;
  ok = true;
expansion_done:
  free(replacement);
done:
  for (size_t i = 0; i < MAX_ARGS; ++i) {
    free(args[i]);
    free(expanded_args[i]);
  }
  return ok;
}

static bool expand_pp_identifier(c_parser_t *p, const c_pp_macro_t *macros,
                                 size_t macro_count, ny_ctok_t token,
                                 ny_lexer_t *input,
                                 c_pp_expansion_t *expansions,
                                 size_t *expansion_depth,
                                 size_t expansion_floor,
                                 ny_lexer_t *outer_input, bool *expanded) {
  *expanded = false;
  bool file_macro =
      token.len == sizeof("__FILE__") - 1 &&
      memcmp(token.start, "__FILE__", sizeof("__FILE__") - 1) == 0;
  bool base_file_macro =
      token.len == sizeof("__BASE_FILE__") - 1 &&
      memcmp(token.start, "__BASE_FILE__", sizeof("__BASE_FILE__") - 1) == 0;
  if ((file_macro || base_file_macro) && p->file_count) {
    if (*expansion_depth == NY_C_MAX_RECURSION_DEPTH)
      do {
        if (getenv("NYC_PP_TRACE"))
          fprintf(stderr, "PP-TRACE false at __LINE__ %d\n", __LINE__);
        return false;
      } while (0);
    size_t file = p->active_file;
    if (file >= p->file_count)
      file = 0;
    char *text = NULL;
    size_t length = 0;
    const char *path =
        base_file_macro && p->base_path ? p->base_path : p->files[file].path;
    if (!pp_filename_replacement(p, path, &text, &length))
      do {
        if (getenv("NYC_PP_TRACE"))
          fprintf(stderr, "PP-TRACE false at __LINE__ %d\n", __LINE__);
        return false;
      } while (0);
    if (!parser_keep_macro_text(p, text)) {
      free(text);
      do {
        if (getenv("NYC_PP_TRACE"))
          fprintf(stderr, "PP-TRACE false at __LINE__ %d\n", __LINE__);
        return false;
      } while (0);
    }
    c_pp_expansion_t *expansion = &expansions[(*expansion_depth)++];
    ny_lex_init(&expansion->lexer, text, length);
    expansion->macro = NULL;
    expansion->line = token.line;
    expansion->col = token.col;
    *expanded = true;
    return true;
  }
  if (token.len == sizeof("__LINE__") - 1 &&
      memcmp(token.start, "__LINE__", sizeof("__LINE__") - 1) == 0) {
    if (*expansion_depth == NY_C_MAX_RECURSION_DEPTH)
      do {
        if (getenv("NYC_PP_TRACE"))
          fprintf(stderr, "PP-TRACE false at __LINE__ %d\n", __LINE__);
        return false;
      } while (0);
    char text[32];
    int written = snprintf(text, sizeof(text), "%u", token.line);
    if (written < 0 || (size_t)written >= sizeof(text))
      do {
        if (getenv("NYC_PP_TRACE"))
          fprintf(stderr, "PP-TRACE false at __LINE__ %d\n", __LINE__);
        return false;
      } while (0);
    char *owned = strdup(text);
    if (!owned || !parser_keep_macro_text(p, owned)) {
      free(owned);
      do {
        if (getenv("NYC_PP_TRACE"))
          fprintf(stderr, "PP-TRACE false at __LINE__ %d\n", __LINE__);
        return false;
      } while (0);
    }
    c_pp_expansion_t *expansion = &expansions[(*expansion_depth)++];
    ny_lex_init(&expansion->lexer, owned, (size_t)written);
    expansion->macro = NULL;
    expansion->line = token.line;
    expansion->col = token.col;
    *expanded = true;
    return true;
  }
  if (token.len == sizeof("__COUNTER__") - 1 &&
      memcmp(token.start, "__COUNTER__", sizeof("__COUNTER__") - 1) == 0) {
    if (*expansion_depth == NY_C_MAX_RECURSION_DEPTH ||
        p->pp_counter > UINT_MAX)
      return false;
    char text[32];
    int written = snprintf(text, sizeof(text), "%u", (unsigned)p->pp_counter);
    if (written < 0 || (size_t)written >= sizeof(text))
      return false;
    char *owned = strdup(text);
    if (!owned || !parser_keep_macro_text(p, owned)) {
      free(owned);
      return false;
    }
    ++p->pp_counter;
    c_pp_expansion_t *expansion = &expansions[(*expansion_depth)++];
    ny_lex_init(&expansion->lexer, owned, (size_t)written);
    expansion->macro = NULL;
    expansion->line = token.line;
    expansion->col = token.col;
    *expanded = true;
    return true;
  }
  const c_pp_macro_t *macro =
      preproc_find_macro(macros, macro_count, token.start, token.len);
  if (!macro)
    return true;
  for (size_t i = 0; i < *expansion_depth; ++i) {
    const c_pp_macro_t *active = expansions[i].macro;
    if (active && active->name_len == macro->name_len &&
        memcmp(active->name, macro->name, macro->name_len) == 0)
      return true;
  }
  if (macro->function_like)
    return expand_function_macro(
        p, macros, macro_count, macro, input, token.line, token.col, expansions,
        expansion_depth, expansion_floor, outer_input, expanded);
  if (macro->replacement_len) {
    if (*expansion_depth == NY_C_MAX_RECURSION_DEPTH)
      do {
        if (getenv("NYC_PP_TRACE"))
          fprintf(stderr, "PP-TRACE false at __LINE__ %d\n", __LINE__);
        return false;
      } while (0);
    c_pp_expansion_t *expansion = &expansions[(*expansion_depth)++];
    ny_lex_init(&expansion->lexer, macro->replacement, macro->replacement_len);
    expansion->macro = macro;
    expansion->line = token.line;
    expansion->col = token.col;
  }
  *expanded = true;
  return true;
}

static bool remember_pp_constant(c_parser_t *p, const char *name,
                                 size_t name_len, bool define,
                                 bool integer_value, int64_t value,
                                 const char *replacement,
                                 size_t replacement_len) {
  size_t i = 0;
  while (i < p->pp_constant_count &&
         (p->pp_constants[i].name_len != name_len ||
          memcmp(p->pp_constants[i].name, name, name_len) != 0))
    ++i;
  if (!define) {
    if (i < p->pp_constant_count) {
      free(p->pp_constants[i].name);
      free(p->pp_constants[i].replacement);
      memmove(&p->pp_constants[i], &p->pp_constants[i + 1],
              (p->pp_constant_count - i - 1) * sizeof(*p->pp_constants));
      --p->pp_constant_count;
    }
    return true;
  }
  if (i == p->pp_constant_count &&
      p->pp_constant_count == p->pp_constant_capacity) {
    size_t next = p->pp_constant_capacity ? p->pp_constant_capacity * 2 : 32;
    if (next > SIZE_MAX / sizeof(*p->pp_constants))
      return false;
    c_pp_constant_t *grown =
        realloc(p->pp_constants, next * sizeof(*p->pp_constants));
    if (!grown)
      return false;
    p->pp_constants = grown;
    p->pp_constant_capacity = next;
  }
  char *replacement_copy = NULL;
  if (replacement_len) {
    replacement_copy = malloc(replacement_len + 1);
    if (!replacement_copy)
      return false;
    memcpy(replacement_copy, replacement, replacement_len);
    replacement_copy[replacement_len] = '\0';
  }
  if (i < p->pp_constant_count) {
    free(p->pp_constants[i].replacement);
    p->pp_constants[i].replacement = replacement_copy;
    p->pp_constants[i].replacement_len = replacement_len;
    p->pp_constants[i].integer_value = integer_value;
    p->pp_constants[i].value = value;
    return true;
  }
  char *copy = malloc(name_len + 1);
  if (!copy) {
    free(replacement_copy);
    return false;
  }
  memcpy(copy, name, name_len);
  copy[name_len] = '\0';
  p->pp_constants[p->pp_constant_count++] =
      (c_pp_constant_t){.name = copy,
                        .name_len = name_len,
                        .value = value,
                        .integer_value = integer_value,
                        .replacement = replacement_copy,
                        .replacement_len = replacement_len};
  return true;
}

typedef struct {
  ny_ctok_t token;
  size_t pos;
  const c_pp_macro_t *macros;
  size_t macro_count;
  bool valid;
  bool skip_evaluation;
  bool unsigned_value;
} c_pp_expr_t;

static int64_t preproc_expr_conditional(c_pp_expr_t *expr);

static int64_t preproc_expr_primary(c_pp_expr_t *expr) {
  preproc_skip_space(expr->token.start, expr->token.len, &expr->pos);
  if (expr->pos >= expr->token.len) {
    expr->valid = false;
    return 0;
  }
  if (expr->token.start[expr->pos] == '(') {
    ++expr->pos;
    int64_t value = preproc_expr_conditional(expr);
    preproc_skip_space(expr->token.start, expr->token.len, &expr->pos);
    if (expr->pos >= expr->token.len || expr->token.start[expr->pos++] != ')')
      expr->valid = false;
    return value;
  }
  const char *name = NULL;
  size_t name_len = 0;
  size_t saved_pos = expr->pos;
  if (preproc_read_identifier(expr->token, &expr->pos, &name, &name_len)) {
    if (name_len != 7 || memcmp(name, "defined", 7) != 0) {
      static const char *const feature_tests[] = {
          "__has_extension", "__has_builtin",      "__has_attribute",
          "__has_include",   "__has_include_next", "__has_feature",
          "__has_warning"};
      for (size_t i = 0; i < sizeof(feature_tests) / sizeof(feature_tests[0]);
           ++i) {
        if (strlen(feature_tests[i]) != name_len ||
            memcmp(feature_tests[i], name, name_len) != 0)
          continue;
        bool is_has_attribute = strlen("__has_attribute") == name_len &&
                                memcmp(name, "__has_attribute", name_len) == 0;
        preproc_skip_space(expr->token.start, expr->token.len, &expr->pos);
        if (expr->pos >= expr->token.len ||
            expr->token.start[expr->pos++] != '(') {
          expr->valid = false;
          return 0;
        }
        if (is_has_attribute) {
          /*
           * `__has_attribute(name)`: 1 when the frontend accepts the
           * attribute, 0 otherwise so guarded definitions degrade to
           * empty macros (kernel compiler_attributes.h expects this).
           */
          preproc_skip_space(expr->token.start, expr->token.len, &expr->pos);
          const char *attribute = NULL;
          size_t attribute_len = 0;
          if (!preproc_read_identifier(expr->token, &expr->pos, &attribute,
                                       &attribute_len)) {
            expr->valid = false;
            return 0;
          }
          size_t depth = 1;
          while (expr->pos < expr->token.len && depth) {
            char c = expr->token.start[expr->pos++];
            if (c == '(')
              ++depth;
            else if (c == ')')
              --depth;
          }
          if (depth)
            expr->valid = false;
          return preproc_attribute_accepted(attribute, attribute_len) ? 1 : 0;
        }
        size_t depth = 1;
        while (expr->pos < expr->token.len && depth) {
          char c = expr->token.start[expr->pos++];
          if (c == '(')
            ++depth;
          else if (c == ')')
            --depth;
        }
        if (depth)
          expr->valid = false;
        return 0;
      }
      const c_pp_macro_t *macro =
          preproc_find_macro(expr->macros, expr->macro_count, name, name_len);
      if (!macro)
        return 0;
      if (!macro->integer_value && !expr->skip_evaluation)
        expr->valid = false;
      return macro->integer_value ? macro->value : 0;
    }
    preproc_skip_space(expr->token.start, expr->token.len, &expr->pos);
    bool parenthesized =
        expr->pos < expr->token.len && expr->token.start[expr->pos] == '(';
    if (parenthesized) {
      ++expr->pos;
      preproc_skip_space(expr->token.start, expr->token.len, &expr->pos);
    }
    if (!preproc_read_identifier(expr->token, &expr->pos, &name, &name_len)) {
      expr->valid = false;
      return 0;
    }
    bool value =
        preproc_macro_defined(expr->macros, expr->macro_count, name, name_len);
    if (parenthesized) {
      preproc_skip_space(expr->token.start, expr->token.len, &expr->pos);
      if (expr->pos >= expr->token.len || expr->token.start[expr->pos++] != ')')
        expr->valid = false;
    }
    return value ? 1 : 0;
  }
  expr->pos = saved_pos;
  if (expr->pos < expr->token.len && expr->token.start[expr->pos] == '\'') {
    ++expr->pos;
    if (expr->pos >= expr->token.len) {
      expr->valid = false;
      return 0;
    }
    unsigned value = (unsigned char)expr->token.start[expr->pos++];
    if (value == '\\') {
      if (expr->pos >= expr->token.len) {
        expr->valid = false;
        return 0;
      }
      char escape = expr->token.start[expr->pos++];
      switch (escape) {
      case 'a':
        value = 7;
        break;
      case 'b':
        value = 8;
        break;
      case 'f':
        value = 12;
        break;
      case 'n':
        value = 10;
        break;
      case 'r':
        value = 13;
        break;
      case 't':
        value = 9;
        break;
      case 'v':
        value = 11;
        break;
      case '\\':
        value = '\\';
        break;
      case '\'':
        value = '\'';
        break;
      case '"':
        value = '"';
        break;
      case '?':
        value = '?';
        break;
      case 'x': {
        value = 0;
        size_t digits = 0;
        while (expr->pos < expr->token.len) {
          char digit_char = expr->token.start[expr->pos];
          unsigned digit;
          if (digit_char >= '0' && digit_char <= '9')
            digit = (unsigned)(digit_char - '0');
          else if (digit_char >= 'a' && digit_char <= 'f')
            digit = (unsigned)(digit_char - 'a' + 10);
          else if (digit_char >= 'A' && digit_char <= 'F')
            digit = (unsigned)(digit_char - 'A' + 10);
          else
            break;
          value = (value << 4) | digit;
          ++expr->pos;
          ++digits;
        }
        if (!digits)
          expr->valid = false;
        break;
      }
      default:
        if (escape >= '0' && escape <= '7') {
          value = (unsigned)(escape - '0');
          for (size_t digits = 1; digits < 3 && expr->pos < expr->token.len &&
                                  expr->token.start[expr->pos] >= '0' &&
                                  expr->token.start[expr->pos] <= '7';
               ++digits)
            value =
                (value << 3) | (unsigned)(expr->token.start[expr->pos++] - '0');
        } else {
          expr->valid = false;
        }
        break;
      }
    }
    if (expr->pos >= expr->token.len || expr->token.start[expr->pos++] != '\'')
      expr->valid = false;
    return (int64_t)value;
  }
  size_t start = expr->pos;
  unsigned base = 10;
  if (expr->pos + 1 < expr->token.len && expr->token.start[expr->pos] == '0' &&
      (expr->token.start[expr->pos + 1] == 'x' ||
       expr->token.start[expr->pos + 1] == 'X')) {
    base = 16;
    expr->pos += 2;
  } else if (expr->token.start[expr->pos] == '0') {
    base = 8;
  }
  uint64_t value = 0;
  size_t digits = 0;
  while (expr->pos < expr->token.len) {
    char c = expr->token.start[expr->pos];
    unsigned digit;
    if (c >= '0' && c <= '9')
      digit = (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f')
      digit = (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      digit = (unsigned)(c - 'A' + 10);
    else
      break;
    if (digit >= base)
      break;
    if (value > (UINT64_MAX - digit) / base) {
      expr->valid = false;
      return 0;
    }
    value = value * base + digit;
    ++expr->pos;
    ++digits;
  }
  if (digits == 0) {
    expr->pos = start;
    expr->valid = false;
    return 0;
  }
  expr->unsigned_value = value > INT64_MAX;
  while (expr->pos < expr->token.len && (expr->token.start[expr->pos] == 'u' ||
                                         expr->token.start[expr->pos] == 'U' ||
                                         expr->token.start[expr->pos] == 'l' ||
                                         expr->token.start[expr->pos] == 'L')) {
    if (expr->token.start[expr->pos] == 'u' ||
        expr->token.start[expr->pos] == 'U')
      expr->unsigned_value = true;
    ++expr->pos;
  }
  return (int64_t)value;
}

static int64_t preproc_expr_unary(c_pp_expr_t *expr) {
  preproc_skip_space(expr->token.start, expr->token.len, &expr->pos);
  if (expr->pos < expr->token.len && expr->token.start[expr->pos] == '!') {
    ++expr->pos;
    return preproc_expr_unary(expr) == 0;
  }
  if (expr->pos < expr->token.len && (expr->token.start[expr->pos] == '+' ||
                                      expr->token.start[expr->pos] == '-' ||
                                      expr->token.start[expr->pos] == '~')) {
    char op = expr->token.start[expr->pos++];
    int64_t value = preproc_expr_unary(expr);
    return op == '-'   ? (int64_t)(0u - (uint64_t)value)
           : op == '~' ? (int64_t)~(uint64_t)value
                       : value;
  }
  return preproc_expr_primary(expr);
}

static int64_t preproc_expr_multiply(c_pp_expr_t *expr) {
  int64_t value = preproc_expr_unary(expr);
  for (;;) {
    preproc_skip_space(expr->token.start, expr->token.len, &expr->pos);
    if (expr->pos >= expr->token.len || (expr->token.start[expr->pos] != '*' &&
                                         expr->token.start[expr->pos] != '/' &&
                                         expr->token.start[expr->pos] != '%'))
      return value;
    char op = expr->token.start[expr->pos++];
    int64_t rhs = preproc_expr_unary(expr);
    if ((op == '/' || op == '%') &&
        (rhs == 0 || (value == INT64_MIN && rhs == -1))) {
      if (!expr->skip_evaluation)
        expr->valid = false;
      value = 0;
      continue;
    }
    value = op == '*'   ? (int64_t)((uint64_t)value * (uint64_t)rhs)
            : op == '/' ? value / rhs
                        : value % rhs;
  }
}

static int64_t preproc_expr_add(c_pp_expr_t *expr) {
  int64_t value = preproc_expr_multiply(expr);
  for (;;) {
    preproc_skip_space(expr->token.start, expr->token.len, &expr->pos);
    if (expr->pos >= expr->token.len || (expr->token.start[expr->pos] != '+' &&
                                         expr->token.start[expr->pos] != '-'))
      return value;
    char op = expr->token.start[expr->pos++];
    int64_t rhs = preproc_expr_multiply(expr);
    value = op == '+' ? (int64_t)((uint64_t)value + (uint64_t)rhs)
                      : (int64_t)((uint64_t)value - (uint64_t)rhs);
  }
}

static int64_t preproc_expr_shift(c_pp_expr_t *expr) {
  int64_t value = preproc_expr_add(expr);
  for (;;) {
    preproc_skip_space(expr->token.start, expr->token.len, &expr->pos);
    size_t pos = expr->pos;
    bool left = pos + 1 < expr->token.len && expr->token.start[pos] == '<' &&
                expr->token.start[pos + 1] == '<';
    bool right = pos + 1 < expr->token.len && expr->token.start[pos] == '>' &&
                 expr->token.start[pos + 1] == '>';
    if (!left && !right)
      return value;
    expr->pos += 2;
    int64_t amount = preproc_expr_add(expr);
    if (amount < 0 || amount >= 64 || (left && value < 0) ||
        (left && amount != 0 &&
         (uint64_t)value > ((uint64_t)INT64_MAX >> (unsigned)amount))) {
      if (!expr->skip_evaluation)
        expr->valid = false;
      value = 0;
      continue;
    }
    value = left         ? (int64_t)((uint64_t)value << (unsigned)amount)
            : value >= 0 ? value >> (unsigned)amount
                         : (int64_t)~((~(uint64_t)value) >> (unsigned)amount);
  }
}

static int64_t preproc_expr_relational(c_pp_expr_t *expr) {
  int64_t value = preproc_expr_shift(expr);
  bool value_unsigned = expr->unsigned_value;
  for (;;) {
    preproc_skip_space(expr->token.start, expr->token.len, &expr->pos);
    size_t pos = expr->pos;
    bool le = pos + 1 < expr->token.len && expr->token.start[pos] == '<' &&
              expr->token.start[pos + 1] == '=';
    bool ge = pos + 1 < expr->token.len && expr->token.start[pos] == '>' &&
              expr->token.start[pos + 1] == '=';
    bool lt = pos < expr->token.len && expr->token.start[pos] == '<' && !le &&
              (pos + 1 == expr->token.len || expr->token.start[pos + 1] != '<');
    bool gt = pos < expr->token.len && expr->token.start[pos] == '>' && !ge &&
              (pos + 1 == expr->token.len || expr->token.start[pos + 1] != '>');
    if (!(le || ge || lt || gt))
      return value;
    expr->pos += (le || ge) ? 2 : 1;
    int64_t rhs = preproc_expr_shift(expr);
    bool rhs_unsigned = expr->unsigned_value;
    if (value_unsigned || rhs_unsigned)
      value = le   ? (uint64_t)value <= (uint64_t)rhs
              : ge ? (uint64_t)value >= (uint64_t)rhs
                   : lt
                  ? (uint64_t)value<(uint64_t)rhs : (uint64_t)value>(uint64_t)
                        rhs;
    else
      value = le   ? value <= rhs
              : ge ? value >= rhs
              : lt ? value < rhs
                   : value > rhs;
    value_unsigned = false;
    expr->unsigned_value = false;
  }
}

static int64_t preproc_expr_equality(c_pp_expr_t *expr) {
  int64_t value = preproc_expr_relational(expr);
  for (;;) {
    preproc_skip_space(expr->token.start, expr->token.len, &expr->pos);
    if (expr->pos + 1 >= expr->token.len ||
        expr->token.start[expr->pos + 1] != '=' ||
        (expr->token.start[expr->pos] != '=' &&
         expr->token.start[expr->pos] != '!'))
      return value;
    bool equal = expr->token.start[expr->pos] == '=';
    expr->pos += 2;
    int64_t rhs = preproc_expr_relational(expr);
    value = equal ? value == rhs : value != rhs;
  }
}

static int64_t preproc_expr_bitand(c_pp_expr_t *expr) {
  int64_t value = preproc_expr_equality(expr);
  for (;;) {
    preproc_skip_space(expr->token.start, expr->token.len, &expr->pos);
    if (expr->pos >= expr->token.len || expr->token.start[expr->pos] != '&' ||
        (expr->pos + 1 < expr->token.len &&
         expr->token.start[expr->pos + 1] == '&'))
      return value;
    ++expr->pos;
    value &= preproc_expr_equality(expr);
  }
}

static int64_t preproc_expr_bitxor(c_pp_expr_t *expr) {
  int64_t value = preproc_expr_bitand(expr);
  for (;;) {
    preproc_skip_space(expr->token.start, expr->token.len, &expr->pos);
    if (expr->pos >= expr->token.len || expr->token.start[expr->pos] != '^')
      return value;
    ++expr->pos;
    value ^= preproc_expr_bitand(expr);
  }
}

static int64_t preproc_expr_bitor(c_pp_expr_t *expr) {
  int64_t value = preproc_expr_bitxor(expr);
  for (;;) {
    preproc_skip_space(expr->token.start, expr->token.len, &expr->pos);
    if (expr->pos >= expr->token.len || expr->token.start[expr->pos] != '|' ||
        (expr->pos + 1 < expr->token.len &&
         expr->token.start[expr->pos + 1] == '|'))
      return value;
    ++expr->pos;
    value |= preproc_expr_bitxor(expr);
  }
}

static int64_t preproc_expr_and(c_pp_expr_t *expr) {
  int64_t value = preproc_expr_bitor(expr);
  for (;;) {
    preproc_skip_space(expr->token.start, expr->token.len, &expr->pos);
    if (expr->pos + 1 >= expr->token.len ||
        expr->token.start[expr->pos] != '&' ||
        expr->token.start[expr->pos + 1] != '&')
      return value;
    expr->pos += 2;
    bool saved = expr->skip_evaluation;
    expr->skip_evaluation = saved || value == 0;
    int64_t rhs = preproc_expr_bitor(expr);
    expr->skip_evaluation = saved;
    value = value != 0 && rhs != 0;
  }
}

static int64_t preproc_expr_or(c_pp_expr_t *expr) {
  int64_t value = preproc_expr_and(expr);
  for (;;) {
    preproc_skip_space(expr->token.start, expr->token.len, &expr->pos);
    if (expr->pos + 1 >= expr->token.len ||
        expr->token.start[expr->pos] != '|' ||
        expr->token.start[expr->pos + 1] != '|')
      return value;
    expr->pos += 2;
    bool saved = expr->skip_evaluation;
    expr->skip_evaluation = saved || value != 0;
    int64_t rhs = preproc_expr_and(expr);
    expr->skip_evaluation = saved;
    value = value != 0 || rhs != 0;
  }
}

static int64_t preproc_expr_conditional(c_pp_expr_t *expr) {
  int64_t condition = preproc_expr_or(expr);
  preproc_skip_space(expr->token.start, expr->token.len, &expr->pos);
  if (expr->pos >= expr->token.len || expr->token.start[expr->pos] != '?')
    return condition;
  ++expr->pos;
  bool saved = expr->skip_evaluation;
  expr->skip_evaluation = saved || condition == 0;
  int64_t selected = preproc_expr_conditional(expr);
  preproc_skip_space(expr->token.start, expr->token.len, &expr->pos);
  if (expr->pos >= expr->token.len || expr->token.start[expr->pos] != ':') {
    expr->valid = false;
    expr->skip_evaluation = saved;
    return 0;
  }
  ++expr->pos;
  expr->skip_evaluation = saved || condition != 0;
  int64_t alternate = preproc_expr_conditional(expr);
  expr->skip_evaluation = saved;
  return condition ? selected : alternate;
}

static int preproc_condition(c_parser_t *p, ny_ctok_t token, size_t argument,
                             const c_pp_macro_t *macros, size_t macro_count,
                             c_pp_expansion_t *expansions,
                             size_t *expansion_depth) {
  char *protected = NULL, *expanded = NULL;
  size_t length = 0, capacity = 0, copied = 0;
  size_t scratch = p->macro_storage_count;
  int result = -1;
  const char *input = token.start + argument;
  size_t input_len = token.len - argument;
  ny_lexer_t lexer;
  ny_lex_init(&lexer, input, input_len);
  for (;;) {
    ny_ctok_t item = ny_lex_next(&lexer);
    if (item.kind == NY_CTOK_EOF)
      break;
    if (item.kind == NY_CTOK_ERROR)
      goto done;
    if (item.kind != NY_CTOK_IDENT || !ny_ctok_eq(item, "defined"))
      continue;
    ny_ctok_t operand = ny_lex_next(&lexer);
    bool parenthesized = pp_token_is(operand, "(");
    if (parenthesized)
      operand = ny_lex_next(&lexer);
    if (!tok_ident(operand))
      goto done;
    bool defined =
        preproc_macro_defined(macros, macro_count, operand.start, operand.len);
    ny_ctok_t last = operand;
    if (parenthesized) {
      last = ny_lex_next(&lexer);
      if (!pp_token_is(last, ")"))
        goto done;
    }
    size_t before = (size_t)(item.start - input);
    if (!pp_append(&protected, &length, &capacity, input + copied,
                   before - copied) ||
        !pp_append(&protected, &length, &capacity, defined ? " 1 " : " 0 ", 3))
      goto done;
    copied = (size_t)(last.start + last.len - input);
  }
  if (protected) {
    if (!pp_append(&protected, &length, &capacity, input + copied,
                   input_len - copied))
      goto done;
    input = protected;
    input_len = length;
  }
  size_t expanded_len = 0;
  if (!expand_macro_argument(p, macros, macro_count, input, input_len,
                             token.line, token.col, expansions, expansion_depth,
                             &expanded, &expanded_len))
    goto done;
  if (expanded) {
    input = expanded;
    input_len = expanded_len;
  }
  c_pp_expr_t expr = {.token = {.start = input, .len = input_len},
                      .valid = true};
  int64_t value = preproc_expr_conditional(&expr);
  preproc_skip_space(input, input_len, &expr.pos);
  if (expr.valid && expr.pos == input_len)
    result = value != 0;
done:
  free(protected);
  free(expanded);
  while (p->macro_storage_count > scratch)
    free(p->macro_storage[--p->macro_storage_count]);
  return result;
}

static bool preproc_integer_define(ny_ctok_t token, size_t pos,
                                   const c_pp_macro_t *macros,
                                   size_t macro_count, int64_t *value) {
  c_pp_expr_t expr = {
      .token = token,
      .pos = pos,
      .macros = macros,
      .macro_count = macro_count,
      .valid = true,
  };
  int64_t result = preproc_expr_conditional(&expr);
  preproc_skip_space(token.start, token.len, &expr.pos);
  if (expr.pos < token.len && token.start[expr.pos] != '/' &&
      token.start[expr.pos] != '\n')
    expr.valid = false;
  if (!expr.valid)
    return false;
  *value = result;
  return true;
}

static bool c_switch_case_value(c_parser_t *p, size_t start, size_t end,
                                int64_t *out) {
  c_parser_t probe = *p;
  char error[1] = {0};
  probe.err = error;
  probe.err_len = sizeof(error);
  size_t at = start;
  return start < end && parse_global_const_expr(&probe, &at, out) && at == end;
}

static bool preproc_directive(ny_ctok_t token, const char *name,
                              size_t *argument) {
  size_t pos = 0;
  preproc_skip_space(token.start, token.len, &pos);
  if (pos >= token.len || token.start[pos++] != '#')
    return false;
  preproc_skip_space(token.start, token.len, &pos);
  size_t name_len = strlen(name);
  if (token.len - pos < name_len ||
      memcmp(token.start + pos, name, name_len) != 0 ||
      (pos + name_len < token.len &&
       (isalnum((unsigned char)token.start[pos + name_len]) ||
        token.start[pos + name_len] == '_')))
    return false;
  pos += name_len;
  preproc_skip_space(token.start, token.len, &pos);
  *argument = pos;
  return true;
}

static bool parser_add_file(c_parser_t *p, char *path, char *text,
                            size_t length, size_t *index) {
  if (p->file_count == p->file_capacity) {
    size_t capacity = p->file_capacity ? p->file_capacity * 2 : 8;
    if (capacity < p->file_capacity || capacity > SIZE_MAX / sizeof(*p->files))
      return false;
    c_source_file_t *files = realloc(p->files, capacity * sizeof(*files));
    if (!files)
      return false;
    p->files = files;
    p->file_capacity = capacity;
  }
  *index = p->file_count;
  p->files[p->file_count++] = (c_source_file_t){.path = path,
                                                .owned_path = path,
                                                .text = text,
                                                .owned_text = text,
                                                .length = length};
  return true;
}

/*
 * Record file transitions, not a filename/index on every token.
 */
static bool record_token_source(c_parser_t *p, size_t file) {
  size_t previous =
      p->source_run_count ? p->source_runs[p->source_run_count - 1].file : 0;
  if (file == previous)
    return true;
  if (p->source_run_count == p->source_run_capacity) {
    size_t capacity = p->source_run_capacity ? p->source_run_capacity * 2 : 8;
    if (capacity < p->source_run_capacity ||
        capacity > SIZE_MAX / sizeof(*p->source_runs))
      return false;
    c_source_run_t *runs = realloc(p->source_runs, capacity * sizeof(*runs));
    if (!runs)
      return false;
    p->source_runs = runs;
    p->source_run_capacity = capacity;
  }
  p->source_runs[p->source_run_count++] =
      (c_source_run_t){.token = p->token_count, .file = file};
  return true;
}

static char *source_realpath(const char *path) {
#ifdef _WIN32
  return _fullpath(NULL, path, 0);
#else
  return realpath(path, NULL);
#endif
}

/*
 * 1: loaded/cached; 0: not found; -1: a real read/allocation error.
 */
static int parser_load_file(c_parser_t *p, const char *path, size_t *index) {
  char *canonical = source_realpath(path);
  if (!canonical) {
    if (errno == ENOENT || errno == ENOTDIR)
      return 0;
    snprintf(p->err, p->err_len, "%s: %s", path, strerror(errno));
    return -1;
  }
  for (size_t i = 0; i < p->file_count; ++i) {
    if (strcmp(p->files[i].path, canonical) == 0) {
      free(canonical);
      *index = i;
      return 1;
    }
  }
  FILE *file = fopen(canonical, "rb");
  if (!file) {
    int code = errno;
    if (code != ENOENT && code != ENOTDIR)
      snprintf(p->err, p->err_len, "%s: %s", canonical, strerror(code));
    free(canonical);
    return code == ENOENT || code == ENOTDIR ? 0 : -1;
  }
  bool ok = fseek(file, 0, SEEK_END) == 0;
  long size = ok ? ftell(file) : -1;
  ok = ok && size >= 0 && (uintmax_t)size < SIZE_MAX &&
       fseek(file, 0, SEEK_SET) == 0;
  char *text = ok ? malloc((size_t)size + 1) : NULL;
  ok = ok && text && fread(text, 1, (size_t)size, file) == (size_t)size &&
       !ferror(file);
  fclose(file);
  if (ok) {
    text[size] = '\0';
    strip_c_comments(text, (size_t)size);
    ok = parser_add_file(p, canonical, text, (size_t)size, index);
  }
  if (!ok) {
    snprintf(p->err, p->err_len, "%s: unable to read or retain include",
             canonical);
    free(text);
    free(canonical);
    return -1;
  }
  return 1;
}

static int parser_try_include(c_parser_t *p, const char *directory,
                              size_t directory_len, ny_ctok_t name,
                              size_t *index) {
  bool separator = directory_len && directory[directory_len - 1] != '/' &&
                   directory[directory_len - 1] != '\\';
  if (directory_len > SIZE_MAX - name.len - 2) {
    snprintf(p->err, p->err_len, "include path is too long");
    return -1;
  }
  char *path = malloc(directory_len + separator + name.len + 1);
  if (!path) {
    snprintf(p->err, p->err_len, "out of memory resolving include");
    return -1;
  }
  memcpy(path, directory, directory_len);
  if (separator)
    path[directory_len++] = '/';
  memcpy(path + directory_len, name.start, name.len);
  path[directory_len + name.len] = '\0';
  int result = parser_load_file(p, path, index);
  free(path);
  return result;
}

static int parser_find_include(c_parser_t *p, const nyc_source_t *source,
                               size_t current_file, ny_ctok_t name, bool quoted,
                               size_t *index) {
  if (name.len && name.start[0] == '/')
    return parser_try_include(p, "", 0, name, index);
  if (quoted) {
    const char *path = p->files[current_file].path;
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *backslash = strrchr(path, '\\');
    if (backslash && (!slash || backslash > slash))
      slash = backslash;
#endif
    size_t length = slash ? (size_t)(slash - path) + 1 : 0;
    int result = parser_try_include(p, path, length, name, index);
    if (result)
      return result;
  }
  for (size_t i = 0; i < source->include_dir_count; ++i) {
    const char *directory = source->include_dirs[i];
    int result =
        parser_try_include(p, directory, strlen(directory), name, index);
    if (result)
      return result;
  }
  /*
   * Linux trace headers stringify include paths written with whitespace
   * between path tokens (for example "../../ arch / x86 /").  Treat that
   * formatting whitespace as insignificant only when it touches a path
   * separator, and only after the literal header name failed to resolve.
   */
  bool spaced_path_separator = false;
  for (size_t i = 0; i < name.len; ++i) {
    if (!isspace((unsigned char)name.start[i]))
      continue;
    if ((i && name.start[i - 1] == '/') ||
        (i + 1 < name.len && name.start[i + 1] == '/')) {
      spaced_path_separator = true;
      break;
    }
  }
  if (spaced_path_separator) {
    char *compact = malloc(name.len + 1);
    if (!compact)
      return -1;
    size_t length = 0;
    for (size_t i = 0; i < name.len; ++i)
      if (!isspace((unsigned char)name.start[i]))
        compact[length++] = name.start[i];
    compact[length] = '\0';
    ny_ctok_t normalized = {.start = compact, .len = length};
    int result =
        parser_find_include(p, source, current_file, normalized, quoted, index);
    free(compact);
    return result;
  }
  return 0;
}

static bool preproc_header_name(ny_ctok_t token, size_t argument,
                                ny_ctok_t *name, bool *quoted) {
  preproc_skip_space(token.start, token.len, &argument);
  if (argument == token.len ||
      (token.start[argument] != '"' && token.start[argument] != '<'))
    return false;
  *quoted = token.start[argument] == '"';
  char close = *quoted ? '"' : '>';
  size_t start = ++argument;
  while (argument < token.len && token.start[argument] != close &&
         token.start[argument] != '\n')
    ++argument;
  if (argument == token.len || token.start[argument] != close ||
      argument == start)
    return false;
  *name = (ny_ctok_t){.kind = NY_CTOK_STRING,
                      .start = token.start + start,
                      .len = argument - start,
                      .line = token.line,
                      .col = token.col};
  ++argument;
  preproc_skip_space(token.start, token.len, &argument);
  return argument == token.len || token.start[argument] == '\n';
}

static int parser_system_include(c_parser_t *p, const nyc_source_t *source,
                                 ny_ctok_t name, size_t *index) {
  if (!source->system_includes)
    return 0;
#ifdef __linux__
  /*
   * Never import the host's configured ABI headers into a cross compilation.
   * Modeled headers are tried first, preserving their bounded libc surface.
   */
#if defined(__x86_64__)
  if (p->abi != NYC_ABI_SYSV_X86_64)
    return 0;
#elif defined(__aarch64__)
  if (p->abi != NYC_ABI_AAPCS64)
    return 0;
#elif defined(__i386__)
  if (p->abi != NYC_ABI_I386)
    return 0;
#else
  return 0;
#endif
  static const char *const directories[] = {"/usr/local/include",
#if defined(__x86_64__)
                                            "/usr/include/x86_64-linux-gnu",
#elif defined(__aarch64__)
                                            "/usr/include/aarch64-linux-gnu",
#elif defined(__i386__)
                                            "/usr/include/i386-linux-gnu",
#endif
                                            "/usr/include"};
  for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); ++i) {
    int found = parser_try_include(p, directories[i], strlen(directories[i]),
                                   name, index);
    if (found)
      return found;
  }
  if (ny_ctok_eq(name, "stdatomic.h")) {
    static bool clang_resource_checked;
    static char clang_resource_include[PATH_MAX];
    if (!clang_resource_checked) {
      clang_resource_checked = true;
      DIR *versions = opendir("/usr/lib/clang");
      if (versions) {
        unsigned long newest_version = 0;
        struct dirent *entry;
        while ((entry = readdir(versions))) {
          char *end = NULL;
          unsigned long version = strtoul(entry->d_name, &end, 10);
          if (end == entry->d_name || *end || version <= newest_version)
            continue;
          char candidate[PATH_MAX];
          int length = snprintf(candidate, sizeof(candidate),
                                "/usr/lib/clang/%s/include", entry->d_name);
          if (length < 0 || (size_t)length >= sizeof(candidate))
            continue;
          DIR *include = opendir(candidate);
          if (include) {
            newest_version = version;
            memcpy(clang_resource_include, candidate, (size_t)length + 1);
            closedir(include);
          }
        }
        closedir(versions);
      }
    }
    if (clang_resource_include[0]) {
      int found =
          parser_try_include(p, clang_resource_include,
                             strlen(clang_resource_include), name, index);
      if (found)
        return found;
    }
  }
#else
  (void)p;
  (void)name;
  (void)index;
#endif
  return 0;
}

static int parser_builtin_include(c_parser_t *p, ny_ctok_t name,
                                  size_t *index) {
  for (size_t i = 0; i < sizeof(nyc_headers) / sizeof(nyc_headers[0]); ++i) {
    const nyc_header_t *header = &nyc_headers[i];
    if (!ny_ctok_eq(name, header->name))
      continue;
    if (header->sysv_x86_64 && p->abi != NYC_ABI_SYSV_X86_64) {
      snprintf(p->err, p->err_len, "modeled <%s> requires the SysV x86-64 ABI",
               header->name);
      return -1;
    }
    for (size_t file = 0; file < p->file_count; ++file) {
      if (p->files[file].path == header->path) {
        *index = file;
        return 1;
      }
    }
    if (!parser_add_file(p, NULL, NULL, header->length, index)) {
      snprintf(p->err, p->err_len, "out of memory caching modeled C header");
      return -1;
    }
    p->files[*index].path = header->path;
    p->files[*index].text = header->text;
    return 1;
  }
  return 0;
}

static bool parser_preprocess(c_parser_t *p, const nyc_source_t *source,
                              nyc_abi_t abi, char *err, size_t err_len,
                              FILE *macro_output) {
  *p = (c_parser_t){.abi = abi, .err = err, .err_len = err_len};
  p->base_path = source->base_path;
  p->path_maps = source->path_maps;
  p->path_map_count = source->path_map_count;
  p->unsigned_char = source->unsigned_char;
  p->no_builtin = source->no_builtin;
  p->disable_sse = source->disable_sse;
  p->i386_regparm = source->i386_regparm;
  p->optimization = source->optimization;
  p->ms_extensions = source->ms_extensions;
  p->plan9_extensions = source->plan9_extensions;
  size_t macro_capacity = 256;
  preproc_hash_reset();
  c_pp_macro_t *macros = calloc(macro_capacity, sizeof(*macros));
  if (!macros) {
    snprintf(err, err_len, "out of memory allocating C macro table");
    parser_destroy(p);
    return false;
  }
  macros[0] = (c_pp_macro_t){
      .name = "__NYC__",
      .name_len = sizeof("__NYC__") - 1,
      .value = 1,
      .integer_value = true,
      .replacement = "1",
      .replacement_len = 1,
      .name_hash = preproc_macro_hash("__NYC__", sizeof("__NYC__") - 1)};
  size_t macro_count = 1;
  const struct {
    const char *name;
    size_t value;
  } target_macros[] = {
      {"__x86_64__", 1},
      {"__LP64__", 1},
      {"__i386__", 1},
      {"__ILP32__", 1},
#if defined(__linux__)
      {"__linux__", 1},
#endif
      {"__ORDER_LITTLE_ENDIAN__", 1234},
      {"__ORDER_BIG_ENDIAN__", 4321},
      {"__ORDER_PDP_ENDIAN__", 3412},
      {"__BYTE_ORDER__", 1234},
      {"__CHAR_BIT__", 8},
      {"__STDC__", 1},
      {"__GNUC__", 12},
      {"__GNUC_MINOR__", 0},
      {"__GNUC_PATCHLEVEL__", 0},
      {"__ATOMIC_RELAXED", 0},
      {"__ATOMIC_CONSUME", 1},
      {"__ATOMIC_ACQUIRE", 2},
      {"__ATOMIC_RELEASE", 3},
      {"__ATOMIC_ACQ_REL", 4},
      {"__ATOMIC_SEQ_CST", 5},
      {"__SIZEOF_SHORT__", sizeof(short)},
      {"__SIZEOF_INT__", sizeof(int)},
      {"__SIZEOF_LONG__", abi == NYC_ABI_I386 ? 4 : sizeof(long)},
      {"__SIZEOF_LONG_LONG__", sizeof(long long)},
      {"__SIZEOF_POINTER__", abi == NYC_ABI_I386 ? 4 : sizeof(void *)},
#if defined(__x86_64__) || defined(__aarch64__)
      {"__SIZEOF_INT128__", 16},
#endif
      {"__SIZEOF_WCHAR_T__",
       source->short_wchar ? sizeof(unsigned short) : sizeof(int)},
      {"__WCHAR_WIDTH__", source->short_wchar ? 16 : 32},
      {"__WCHAR_MAX__", source->short_wchar ? USHRT_MAX : INT_MAX},
  };
  for (size_t i = 0; i < sizeof(target_macros) / sizeof(target_macros[0]);
       ++i) {
    if ((strcmp(target_macros[i].name, "__i386__") == 0 &&
         abi != NYC_ABI_I386) ||
        (strcmp(target_macros[i].name, "__ILP32__") == 0 &&
         abi != NYC_ABI_I386) ||
        ((strcmp(target_macros[i].name, "__x86_64__") == 0 ||
          strcmp(target_macros[i].name, "__LP64__") == 0) &&
         abi == NYC_ABI_I386) ||
        (strcmp(target_macros[i].name, "__SIZEOF_INT128__") == 0 &&
         abi == NYC_ABI_I386))
      continue;
    char value[32];
    int length = snprintf(value, sizeof(value), "%zu", target_macros[i].value);
    char *replacement = length > 0 && (size_t)length < sizeof(value)
                            ? malloc((size_t)length + 1)
                            : NULL;
    if (!replacement) {
      snprintf(err, err_len, "cannot seed C target size macros");
      goto fail;
    }
    memcpy(replacement, value, (size_t)length + 1);
    if (!parser_keep_macro_text(p, replacement)) {
      free(replacement);
      snprintf(err, err_len, "cannot seed C target size macros");
      goto fail;
    }
    if (!preproc_update_macro(p, &macros, &macro_count, &macro_capacity,
                              target_macros[i].name,
                              strlen(target_macros[i].name), NULL, 0, true,
                              (int64_t)target_macros[i].value, true,
                              replacement, (size_t)length, false, NULL)) {
      snprintf(err, err_len, "cannot seed C target size macros");
      goto fail;
    }
  }
  const char *wchar_type = source->short_wchar ? "short unsigned int" : "int";
  if (!preproc_update_macro(p, &macros, &macro_count, &macro_capacity,
                            "__WCHAR_TYPE__", sizeof("__WCHAR_TYPE__") - 1,
                            NULL, 0, true, 0, false, wchar_type,
                            strlen(wchar_type), false, NULL)) {
    snprintf(err, err_len, "cannot seed C wchar type macro");
    goto fail;
  }
  static const char *const target_type_macros[] = {
      "__SIZE_TYPE__",    "__PTRDIFF_TYPE__", "__INTPTR_TYPE__",
      "__UINTPTR_TYPE__", "__INT64_TYPE__",   "__UINT64_TYPE__"};
  const char *const target_type_values[] = {
      "long unsigned int", "long int", "long int",
      "long unsigned int", "long int", "long unsigned int"};
  for (size_t i = 0;
       i < sizeof(target_type_macros) / sizeof(target_type_macros[0]); ++i) {
    const char *type_value = target_type_values[i];
    if (abi == NYC_ABI_I386)
      type_value = i < 4    ? ((i == 0 || i == 3) ? "unsigned int" : "int")
                   : i == 4 ? "long long int"
                            : "long long unsigned int";
    if (!preproc_update_macro(
            p, &macros, &macro_count, &macro_capacity, target_type_macros[i],
            strlen(target_type_macros[i]), NULL, 0, true, 0, false, type_value,
            strlen(type_value), false, NULL)) {
      snprintf(err, err_len, "cannot seed C target type macro");
      goto fail;
    }
  }
  const char *wchar_min = source->short_wchar ? "0" : "(-2147483647 - 1)";
  if (!preproc_update_macro(p, &macros, &macro_count, &macro_capacity,
                            "__WCHAR_MIN__", sizeof("__WCHAR_MIN__") - 1, NULL,
                            0, true, 0, false, wchar_min, strlen(wchar_min),
                            false, NULL)) {
    snprintf(err, err_len, "cannot seed C wchar bounds macro");
    goto fail;
  }
  if (source->unsigned_char &&
      !preproc_update_macro(p, &macros, &macro_count, &macro_capacity,
                            "__CHAR_UNSIGNED__",
                            sizeof("__CHAR_UNSIGNED__") - 1, NULL, 0, true, 1,
                            true, "1", 1, false, NULL)) {
    snprintf(err, err_len, "cannot seed C char signedness macro");
    goto fail;
  }
  static const char *const version_names[] = {"__NYC_MAJOR__", "__NYC_MINOR__",
                                              "__NYC_PATCHLEVEL__"};
  const char *component = ny_version_string();
  for (size_t i = 0; i < sizeof(version_names) / sizeof(version_names[0]);
       ++i) {
    size_t length = 0;
    while (component[length] >= '0' && component[length] <= '9')
      ++length;
    ny_ctok_t number = {.start = component, .len = length};
    int64_t value;
    if (!preproc_integer_define(number, 0, macros, macro_count, &value) ||
        !preproc_update_macro(p, &macros, &macro_count, &macro_capacity,
                              version_names[i], strlen(version_names[i]), NULL,
                              0, true, value, true, component, length, false,
                              NULL)) {
      snprintf(err, err_len, "invalid NYC build version '%s'",
               ny_version_string());
      goto fail;
    }
    component += length;
    if (i < 2) {
      if (*component != '.') {
        snprintf(err, err_len, "invalid NYC build version '%s'",
                 ny_version_string());
        goto fail;
      }
      ++component;
    }
  }
  for (size_t i = 0; i < source->macro_count; ++i) {
    const nyc_macro_option_t *option = &source->macros[i];
    ny_ctok_t argument = {.start = option->text, .len = strlen(option->text)};
    size_t pos = 0, name_len = 0;
    const char *name = NULL;
    if (!preproc_read_identifier(argument, &pos, &name, &name_len) ||
        (pos < argument.len &&
         (option->undefine || argument.start[pos] != '='))) {
      snprintf(err, err_len, "invalid -%c argument '%s'; expected %s",
               option->undefine ? 'U' : 'D', option->text,
               option->undefine ? "NAME" : "NAME[=REPLACEMENT]");
      goto fail;
    }
    const char *replacement =
        pos < argument.len ? argument.start + pos + 1 : "1";
    size_t replacement_len = strlen(replacement);
    int64_t value = 0;
    ny_ctok_t expression = {.start = replacement, .len = replacement_len};
    bool integer =
        !option->undefine &&
        preproc_integer_define(expression, 0, macros, macro_count, &value);
    if (!preproc_update_macro(p, &macros, &macro_count, &macro_capacity, name,
                              name_len, NULL, 0, !option->undefine, value,
                              integer, replacement, replacement_len, false,
                              NULL)) {
      snprintf(err, err_len, "command-line macro limit exceeded (%u max)",
               NYC_MAX_PREPROC_MACROS);
      goto fail;
    }
  }
  char *main_text =
      source->length < SIZE_MAX ? malloc(source->length + 1) : NULL;
  const char *source_path = source->path ? source->path : "<input>";
  char *main_path = source->path ? source_realpath(source->path) : NULL;
  if (!main_path)
    main_path = strdup(source_path);
  size_t current_file = 0;
  if (!main_text || !main_path ||
      !parser_add_file(p, main_path, main_text, source->length,
                       &current_file)) {
    free(main_text);
    free(main_path);
    snprintf(err, err_len, "out of memory preparing C source");
    goto fail;
  }
  memcpy(main_text, source->text, source->length);
  main_text[source->length] = '\0';
  strip_c_comments(main_text, source->length);
  ny_lexer_t lexer;
  ny_lex_init(&lexer, main_text, source->length);
  size_t capacity = 256;
  c_pp_cond_t conditions[NY_C_MAX_COND_STACK];
  size_t condition_depth = 0;
  struct {
    ny_lexer_t lexer;
    size_t file;
    size_t condition_base;
  } files[NY_C_MAX_RECURSION_DEPTH];
  size_t file_depth = 0, condition_base = 0;
  c_pp_expansion_t expansions[NY_C_MAX_RECURSION_DEPTH];
  size_t expansion_depth = 0;
  bool active = true;
  p->tokens = malloc(capacity * sizeof(*p->tokens));
  if (!p->tokens) {
    snprintf(err, err_len, "out of memory allocating C token table");
    goto fail;
  }
  for (;;) {
    ny_ctok_t token;
    bool from_expansion = false;
    while (expansion_depth) {
      c_pp_expansion_t *expansion = &expansions[expansion_depth - 1];
      token = ny_lex_next(&expansion->lexer);
      if (token.kind == NY_CTOK_EOF) {
        --expansion_depth;
        continue;
      }
      token.line = expansion->line;
      token.col += expansion->col - 1;
      from_expansion = true;
      break;
    }
    if (!from_expansion)
      token = ny_lex_next(&lexer);
    p->active_file = current_file;
    if (!from_expansion && token.kind == NY_CTOK_EOF) {
      if (condition_depth != condition_base) {
        snprintf(err, err_len, "%s:%u: unterminated preprocessor conditional",
                 p->files[current_file].path, token.line);
        goto fail;
      }
      if (file_depth) {
        --file_depth;
        lexer = files[file_depth].lexer;
        current_file = files[file_depth].file;
        condition_base = files[file_depth].condition_base;
        continue;
      }
    }
    if (token.kind == NY_CTOK_PREPROC) {
      if (from_expansion) {
        snprintf(err, err_len,
                 "%u:%u: macro expansion cannot create a "
                 "preprocessor directive",
                 token.line, token.col);
        goto fail;
      }
      size_t argument = 0;
      bool is_if = preproc_directive(token, "if", &argument);
      bool is_ifdef = preproc_directive(token, "ifdef", &argument);
      bool is_ifndef = preproc_directive(token, "ifndef", &argument);
      if (is_if || is_ifdef || is_ifndef) {
        if (condition_depth == NY_C_MAX_COND_STACK) {
          snprintf(err, err_len,
                   "%u: preprocessor condition nesting exceeds "
                   "the nyc limit",
                   token.line);
          goto fail;
        }
        if (!active) {
          conditions[condition_depth++] =
              (c_pp_cond_t){.parent_active = false, .branch_taken = false};
          continue;
        }
        if (!is_if) {
          const char *name = NULL;
          size_t name_len = 0;
          if (!preproc_read_identifier(token, &argument, &name, &name_len)) {
            snprintf(err, err_len, "%u: expected macro name after #%s",
                     token.line, is_ifdef ? "ifdef" : "ifndef");
            goto fail;
          }
          preproc_skip_space(token.start, token.len, &argument);
          if (argument < token.len && token.start[argument] != '/' &&
              token.start[argument] != '\n') {
            snprintf(err, err_len, "%u: unexpected text after #%s macro name",
                     token.line, is_ifdef ? "ifdef" : "ifndef");
            goto fail;
          }
          bool defined =
              preproc_macro_defined(macros, macro_count, name, name_len);
          bool condition = is_ifdef ? defined : !defined;
          conditions[condition_depth++] = (c_pp_cond_t){
              .parent_active = active,
              .branch_taken = condition,
          };
          active = condition;
          continue;
        }
        int literal = preproc_condition(p, token, argument, macros, macro_count,
                                        expansions, &expansion_depth);
        if (literal < 0) {
          snprintf(err, err_len, "%u: unsupported #if expression near `%.120s`",
                   token.line, token.start);
          goto fail;
        }
        bool condition = literal != 0;
        conditions[condition_depth++] = (c_pp_cond_t){
            .parent_active = active, .branch_taken = active && condition};
        active = active && condition;
      } else if (preproc_directive(token, "else", &argument)) {
        if (condition_depth == condition_base) {
          snprintf(err, err_len, "%u: unmatched #else", token.line);
          goto fail;
        }
        c_pp_cond_t *condition = &conditions[condition_depth - 1];
        if (condition->else_seen) {
          snprintf(err, err_len, "%u: duplicate #else", token.line);
          goto fail;
        }
        condition->else_seen = true;
        active = condition->parent_active && !condition->branch_taken;
        condition->branch_taken = true;
      } else if (preproc_directive(token, "elif", &argument)) {
        if (condition_depth == condition_base) {
          snprintf(err, err_len, "%u: unmatched #elif", token.line);
          goto fail;
        }
        c_pp_cond_t *condition = &conditions[condition_depth - 1];
        if (condition->else_seen) {
          snprintf(err, err_len, "%u: #elif after #else", token.line);
          goto fail;
        }
        if (!condition->parent_active || condition->branch_taken) {
          active = false;
          continue;
        }
        int literal = preproc_condition(p, token, argument, macros, macro_count,
                                        expansions, &expansion_depth);
        if (literal < 0) {
          snprintf(err, err_len,
                   "%u: unsupported #elif expression near `%.120s`", token.line,
                   token.start);
          goto fail;
        }
        active = condition->parent_active && !condition->branch_taken &&
                 literal != 0;
        condition->branch_taken = condition->branch_taken || active;
      } else if (preproc_directive(token, "endif", &argument)) {
        if (condition_depth == condition_base) {
          snprintf(err, err_len, "%u: unmatched #endif", token.line);
          goto fail;
        }
        active = conditions[--condition_depth].parent_active;
      } else if (active && preproc_directive(token, "error", &argument)) {
        snprintf(err, err_len, "%s:%u: #error %.*s",
                 p->files[current_file].path, token.line,
                 (int)(token.len - argument), token.start + argument);
        goto fail;
      } else if (active && preproc_directive(token, "pragma", &argument)) {
        const char *name = NULL;
        size_t length = 0;
        if (preproc_read_identifier(token, &argument, &name, &length) &&
            length == 4 && memcmp(name, "once", 4) == 0)
          p->files[current_file].once = true;
      } else if (active && preproc_directive(token, "include", &argument)) {
        if (argument < token.len && token.start[argument] != '"' &&
            token.start[argument] != '<') {
          char *expanded_name = NULL;
          size_t expanded_len = 0;
          if (!expand_macro_argument(
                  p, macros, macro_count, token.start + argument,
                  token.len - argument, token.line, token.col, expansions,
                  &expansion_depth, &expanded_name, &expanded_len)) {
            snprintf(err, err_len, "%s:%u: invalid include macro",
                     p->files[current_file].path, token.line);
            goto fail;
          }
          if (expanded_name) {
            if (!parser_keep_macro_text(p, expanded_name)) {
              free(expanded_name);
              snprintf(err, err_len, "out of memory expanding include name");
              goto fail;
            }
            token.start = expanded_name;
            token.len = expanded_len;
            argument = 0;
          }
        }
        ny_ctok_t include_name;
        bool quoted = false;
        if (!preproc_header_name(token, argument, &include_name, &quoted)) {
          snprintf(err, err_len,
                   "%s:%u: expected a quoted or angle header name",
                   p->files[current_file].path, token.line);
          goto fail;
        }
        size_t included_file = 0;
        int found = parser_find_include(p, source, current_file, include_name,
                                        quoted, &included_file);
        if (!found)
          found = parser_builtin_include(p, include_name, &included_file);
        if (!found)
          found =
              parser_system_include(p, source, include_name, &included_file);
        if (found < 0)
          goto fail;
        if (!found) {
          snprintf(err, err_len, "%s:%u: include file '%.*s' not found",
                   p->files[current_file].path, token.line,
                   (int)include_name.len, include_name.start);
          goto fail;
        }
        if (p->files[included_file].once)
          continue;
        if (file_depth == NY_C_MAX_RECURSION_DEPTH) {
          snprintf(err, err_len, "%s:%u: include nesting exceeds %u",
                   p->files[current_file].path, token.line,
                   NY_C_MAX_RECURSION_DEPTH);
          goto fail;
        }
        files[file_depth].lexer = lexer;
        files[file_depth].file = current_file;
        files[file_depth++].condition_base = condition_base;
        current_file = included_file;
        condition_base = condition_depth;
        ny_lex_init(&lexer, p->files[included_file].text,
                    p->files[included_file].length);
      } else if (active && (preproc_directive(token, "define", &argument) ||
                            preproc_directive(token, "undef", &argument))) {
        bool define = preproc_directive(token, "define", &argument);
        const char *name = NULL;
        size_t name_len = 0;
        if (!preproc_read_identifier(token, &argument, &name, &name_len)) {
          snprintf(err, err_len, "%u: expected macro name after #%s",
                   token.line, define ? "define" : "undef");
          goto fail;
        }
        bool function_like =
            define && argument < token.len && token.start[argument] == '(';
        const char *parameters = NULL;
        size_t parameters_len = 0;
        if (function_like) {
          size_t parameter_start = argument + 1;
          /*
           * The opening parenthesis is already consumed; depth=1 balance.
           */
          size_t depth = 1;
          ++argument;
          while (depth) {
            if (argument >= token.len) {
              snprintf(err, err_len, "%u: unterminated function-like macro",
                       token.line);
              goto fail;
            }
            if (token.start[argument] == '(')
              ++depth;
            else if (token.start[argument] == ')')
              --depth;
            ++argument;
          }
          parameters = token.start + parameter_start;
          parameters_len = argument - parameter_start - 1;
        }
        preproc_skip_space(token.start, token.len, &argument);
        size_t replacement_end = token.len;
        for (size_t i = argument; i < token.len; ++i) {
          if (token.start[i] == '\\' && i + 1 < token.len &&
              token.start[i + 1] == '\n') {
            ++i;
            continue;
          }
          if (token.start[i] == '\\' && i + 2 < token.len &&
              token.start[i + 1] == '\r' && token.start[i + 2] == '\n') {
            i += 2;
            continue;
          }
          if (token.start[i] == '\n') {
            replacement_end = i;
            break;
          }
        }
        while (replacement_end > argument &&
               isspace((unsigned char)token.start[replacement_end - 1]))
          --replacement_end;
        /*
         * A continuation at the end of a macro immediately before an empty
         * physical line may be retained by the source token after its newline
         * has been consumed.  A bare backslash cannot be a preprocessing
         * replacement token; discard it rather than leaking it into C tokens.
         */
        if (replacement_end > argument &&
            token.start[replacement_end - 1] == '\\')
          --replacement_end;
        size_t raw_len = define ? replacement_end - argument : 0;
        char *replacement_copy = define ? malloc(raw_len + 1) : NULL;
        if (define && !replacement_copy) {
          snprintf(err, err_len, "%u: out of memory storing macro replacement",
                   token.line);
          goto fail;
        }
        size_t replacement_len = 0;
        for (size_t i = argument; define && i < replacement_end; ++i) {
          if (token.start[i] == '\\' && i + 1 < replacement_end &&
              token.start[i + 1] == '\n') {
            ++i;
            continue;
          }
          if (token.start[i] == '\\' && i + 2 < replacement_end &&
              token.start[i + 1] == '\r' && token.start[i + 2] == '\n') {
            i += 2;
            continue;
          }
          replacement_copy[replacement_len++] = token.start[i];
        }
        if (replacement_copy)
          replacement_copy[replacement_len] = '\0';
        int64_t value = 0;
        bool integer_value =
            define && preproc_integer_define(token, argument, macros,
                                             macro_count, &value);
        const char *replacement = replacement_copy;
        if (!preproc_update_macro(p, &macros, &macro_count, &macro_capacity,
                                  name, name_len, parameters, parameters_len,
                                  define, value, integer_value, replacement,
                                  replacement_len, function_like,
                                  replacement_copy)) {
          free(replacement_copy);
          snprintf(err, err_len, "%u: macro limit exceeded (%u max)",
                   token.line, NYC_MAX_PREPROC_MACROS);
          goto fail;
        }
        if (!remember_pp_constant(p, name, name_len, define && !function_like,
                                  integer_value, value, replacement,
                                  replacement_len)) {
          snprintf(err, err_len, "%u: out of memory retaining integer macro",
                   token.line);
          goto fail;
        }
      }
      continue;
    }
    if (!active)
      continue;
    if (token.kind == NY_CTOK_IDENT) {
      ny_lexer_t *input =
          from_expansion ? &expansions[expansion_depth - 1].lexer : &lexer;
      bool expanded;
      if (!expand_pp_identifier(p, macros, macro_count, token, input,
                                expansions, &expansion_depth, 0, &lexer,
                                &expanded)) {
        snprintf(err, err_len,
                 "%u:%u: unsupported or malformed expansion of C macro "
                 "`%.*s`",
                 token.line, token.col, (int)token.len, token.start);
        goto fail;
      }
      if (p->pp_pending_condition_depth) {
        if (p->pp_pending_condition_depth >
            NY_C_MAX_COND_STACK - condition_depth) {
          snprintf(err, err_len, "%u: conditional nesting exceeds %u",
                   token.line, NY_C_MAX_COND_STACK);
          goto fail;
        }
        for (size_t i = 0; i < p->pp_pending_condition_depth; ++i) {
          conditions[condition_depth++] =
              (c_pp_cond_t){.parent_active = p->pp_pending_condition_parent[i],
                            .branch_taken = p->pp_pending_condition_taken[i],
                            .else_seen = p->pp_pending_condition_else_seen[i]};
        }
        active = p->pp_pending_active;
        p->pp_pending_condition_depth = 0;
      }
      while (p->pp_consumed_endif) {
        if (condition_depth == condition_base) {
          snprintf(err, err_len, "%u: unmatched #endif", token.line);
          goto fail;
        }
        active = conditions[--condition_depth].parent_active;
        --p->pp_consumed_endif;
      }
      if (expanded)
        continue;
    }
    if (token.kind == NY_CTOK_ERROR) {
      snprintf(err, err_len, "%u:%u: %s", token.line, token.col,
               token.error ? token.error : "invalid C token");
      goto fail;
    }
    if (p->token_count == capacity) {
      if (capacity > SIZE_MAX / 2 / sizeof(*p->tokens)) {
        snprintf(err, err_len, "C translation unit is too large");
        goto fail;
      }
      capacity *= 2;
      ny_ctok_t *grown = realloc(p->tokens, capacity * sizeof(*grown));
      if (!grown) {
        snprintf(err, err_len, "out of memory growing C token table");
        goto fail;
      }
      p->tokens = grown;
    }
    if (!record_token_source(p, current_file)) {
      snprintf(err, err_len, "out of memory retaining C source locations");
      goto fail;
    }
    p->tokens[p->token_count++] = token;
    if (token.kind == NY_CTOK_EOF)
      break;
  }
  if (condition_depth) {
    snprintf(err, err_len, "unterminated C preprocessor conditional");
    goto fail;
  }
  if (!p->token_count)
    goto fail;
  if (macro_output) {
    for (size_t i = 0; i < macro_count; ++i) {
      const c_pp_macro_t *macro = &macros[i];
      if (fputs("#define ", macro_output) < 0 ||
          fwrite(macro->name, 1, macro->name_len, macro_output) !=
              macro->name_len)
        goto macro_write_failed;
      if (macro->function_like &&
          (fputc('(', macro_output) == EOF ||
           (macro->parameters_len &&
            fwrite(macro->parameters, 1, macro->parameters_len, macro_output) !=
                macro->parameters_len) ||
           fputc(')', macro_output) == EOF))
        goto macro_write_failed;
      if ((macro->replacement_len &&
           (fputc(' ', macro_output) == EOF ||
            fwrite(macro->replacement, 1, macro->replacement_len,
                   macro_output) != macro->replacement_len)) ||
          fputc('\n', macro_output) == EOF)
        goto macro_write_failed;
    }
  }
  for (size_t i = 0; i < macro_count; ++i) {
    char *owned = macros[i].owned_replacement;
    if (!parser_keep_macro_text(p, owned)) {
      snprintf(err, err_len, "out of memory retaining macro token storage");
      goto fail;
    }
    macros[i].owned_replacement = NULL;
  }
  for (size_t i = 0; i < macro_count; ++i)
    free(macros[i].owned_replacement);
  free(macros);
  return true;
macro_write_failed:
  snprintf(err, err_len, "cannot write C macro definitions");
fail:
  for (size_t i = 0; i < macro_count; ++i)
    free(macros[i].owned_replacement);
  free(macros);
  parser_destroy(p);
  return false;
}

static bool parser_prepare(c_parser_t *p, const nyc_source_t *source,
                           nyc_abi_t abi, char *err, size_t err_len) {
  if (!parser_preprocess(p, source, abi, err, err_len, NULL))
    return false;
  if (p->token_count > SIZE_MAX / sizeof(*p->locals) ||
      p->token_count > SIZE_MAX / sizeof(*p->functions) ||
      p->token_count > SIZE_MAX / sizeof(*p->typedefs) ||
      p->token_count > SIZE_MAX / sizeof(*p->globals)) {
    snprintf(err, err_len, "C translation unit is too large");
    goto fail;
  }
  p->locals = calloc(p->token_count, sizeof(*p->locals));
  p->functions = calloc(p->token_count, sizeof(*p->functions));
  p->typedefs = calloc(p->token_count, sizeof(*p->typedefs));
  p->globals = calloc(p->token_count, sizeof(*p->globals));
  if (!p->locals || !p->functions || !p->typedefs || !p->globals) {
    snprintf(err, err_len, "out of memory allocating C declaration tables");
    goto fail;
  }
  p->local_capacity = p->token_count;
  p->function_capacity = p->token_count;
  p->typedef_capacity = p->token_count;
  p->global_capacity = p->token_count;
  p->allow_raw_file_assembly = source->allow_raw_file_assembly;
  if (collect_functions(p))
    return true;
  if (!err[0])
    fail(p, "unsupported or invalid file-scope declaration");
fail:
  parser_destroy(p);
  return false;
}

static bool c_label_type_keyword(c_parser_t *p, size_t pos) {
  static const char *const keywords[] = {
      "void",   "char",     "short",    "int",      "long",
      "float",  "double",   "_Float32", "_Float64", "_Float32x",
      "signed", "unsigned", "_Bool",    "_Complex", "_Atomic"};
  for (size_t i = 0; i < sizeof(keywords) / sizeof(*keywords); ++i)
    if (tok_is(p, pos, keywords[i]))
      return true;
  return false;
}

static bool c_case_label_colon(c_parser_t *p, const c_function_t *fn,
                               size_t colon_pos) {
  size_t conditional_depth = 0;
  for (size_t pos = colon_pos; pos-- > fn->body_start;) {
    if (tok_is(p, pos, ";") || tok_is(p, pos, "{") || tok_is(p, pos, "}"))
      return false;
    if (tok_is(p, pos, ":")) {
      if (!conditional_depth)
        return false;
      --conditional_depth;
    } else if (tok_is(p, pos, "?")) {
      ++conditional_depth;
    }
    if (tok_is(p, pos, "case"))
      return true;
  }
  return false;
}

static bool function_has_local_label_decl(const c_parser_t *p,
                                          const c_function_t *fn,
                                          ny_ctok_t name, size_t before) {
  for (size_t i = fn->body_start; i < before && i < fn->body_end; ++i) {
    if (!tok_is(p, i, "__label__"))
      continue;
    for (size_t declaration = i + 1;
         declaration < before && declaration < fn->body_end &&
         !tok_is(p, declaration, ";");
         ++declaration)
      if (tok_same(p->tokens[declaration], name))
        return true;
  }
  return false;
}

static bool register_function_label(c_parser_t *p, c_function_t *fn,
                                    size_t token_pos) {
  ny_ctok_t name = p->tokens[token_pos];
  bool is_local = function_has_local_label_decl(p, fn, name, token_pos);
  for (size_t previous = 0; previous < p->label_count; ++previous) {
    if (tok_same(p->labels[previous].name, name) &&
        !(is_local && p->labels[previous].is_local)) {
      char message[128];
      snprintf(message, sizeof(message), "duplicate C label near `%.64s`",
               name.start);
      fail(p, message);
      return false;
    }
  }
  if (p->next_label == INT_MAX) {
    fail(p, "C function has too many control-flow labels");
    return false;
  }
  p->labels[p->label_count++] = (c_label_t){.name = name,
                                            .id = (size_t)p->next_label++,
                                            .token_pos = token_pos,
                                            .is_local = is_local};
  return true;
}

#include "code/lang/c/lower/parts/function.h"
static bool write_line_marker(FILE *output, unsigned line, const char *path) {
  if (fprintf(output, "# %u \"", line) < 0)
    return false;
  for (const unsigned char *at = (const unsigned char *)path; *at; ++at) {
    if (*at == '\\' || *at == '"') {
      if (fputc('\\', output) == EOF || fputc(*at, output) == EOF)
        return false;
    } else if (*at < 32 || *at == 127) {
      if (fprintf(output, "\\%03o", *at) < 0)
        return false;
    } else if (fputc(*at, output) == EOF) {
      return false;
    }
  }
  return fputs("\"\n", output) >= 0;
}

int nyc_frontend_preprocess(const nyc_source_t *source, nyc_abi_t abi,
                            FILE *output, bool line_markers, char *err,
                            size_t err_len) {
  if (!source || !source->text || !output || !err || !err_len)
    return 0;
  err[0] = '\0';
  c_parser_t parser;
  if (!parser_preprocess(&parser, source, abi, err, err_len, NULL))
    return 0;
  size_t run = 0, file = 0, previous_file = SIZE_MAX;
  unsigned line = 0;
  bool start_of_line = true;
  ny_ctok_t previous = {0};
  for (size_t i = 0; i + 1 < parser.token_count; ++i) {
    ny_ctok_t token = parser.tokens[i];
    while (run < parser.source_run_count && parser.source_runs[run].token <= i)
      file = parser.source_runs[run++].file;
    if (line_markers &&
        (file != previous_file || token.line < line || token.line - line > 8)) {
      if ((!start_of_line && ((source->assembly_input && token.continued &&
                               fputc('\\', output) == EOF) ||
                              fputc('\n', output) == EOF)) ||
          !write_line_marker(output, token.line, parser.files[file].path))
        goto write_failed;
      start_of_line = true;
      line = token.line;
    } else if (file != previous_file || token.line != line) {
      unsigned gap = line_markers && file == previous_file && token.line > line
                         ? token.line - line
                         : 1;
      if (!i)
        gap = 0;
      if (gap && source->assembly_input && token.continued &&
          fputc('\\', output) == EOF)
        goto write_failed;
      for (unsigned n = 0; n < gap; ++n)
        if (fputc('\n', output) == EOF)
          goto write_failed;
      start_of_line = true;
      line = token.line;
    }
    const char *separator = NULL;
    size_t separator_len = 0;
    if (!start_of_line) {
      /*
       * Assembly input is whitespace-significant (`.section .text`,
       * `.incbin "file"`, `movq $1, %rax`), so reproduce the horizontal
       * spacing between adjacent source tokens instead of normalizing it.
       */
      if (source->assembly_input && previous.len &&
          token.start >= previous.start + previous.len) {
        const char *gap = previous.start + previous.len;
        size_t length = (size_t)(token.start - gap);
        bool horizontal = length <= 64;
        for (size_t n = 0; horizontal && n < length; ++n)
          horizontal = gap[n] == ' ' || gap[n] == '\t';
        if (horizontal) {
          separator = gap;
          separator_len = length;
        }
      }
      if (!separator && source->assembly_input) {
        /*
         * Macro-argument tokens can come from another buffer, so there is no
         * trustworthy source gap to copy. Separate them, except after the
         * prefixes that must stay attached to their operand (`%rax`, `$1`,
         * `.section`, `.Llabel`).
         */
        bool attaches = previous.len == 1 &&
                        (previous.start[0] == '%' || previous.start[0] == '$' ||
                         previous.start[0] == '.');
        if (!attaches) {
          separator = " ";
          separator_len = 1;
        }
      }
      if (!separator && pp_tokens_need_space(previous, token)) {
        separator = " ";
        separator_len = 1;
      }
    }
    if ((separator &&
         fwrite(separator, 1, separator_len, output) != separator_len) ||
        fwrite(token.start, 1, token.len, output) != token.len)
      goto write_failed;
    previous = token;
    previous_file = file;
    start_of_line = false;
  }
  if (!start_of_line && fputc('\n', output) == EOF)
    goto write_failed;
  parser_destroy(&parser);
  return 1;
write_failed:
  snprintf(err, err_len, "cannot write preprocessed C source");
  parser_destroy(&parser);
  return 0;
}

int nyc_frontend_dump_macros(const nyc_source_t *source, nyc_abi_t abi,
                             FILE *output, char *err, size_t err_len) {
  if (!source || !source->text || !output || !err || !err_len)
    return 0;
  err[0] = '\0';
  c_parser_t parser;
  if (!parser_preprocess(&parser, source, abi, err, err_len, output))
    return 0;
  parser_destroy(&parser);
  return 1;
}

int nyc_frontend_visit_dependencies(const nyc_source_t *source, nyc_abi_t abi,
                                    nyc_dependency_visitor_t visitor,
                                    void *context, char *err, size_t err_len) {
  if (!source || !source->text || !visitor || !err || !err_len)
    return 0;
  err[0] = '\0';
  c_parser_t parser;
  if (!parser_preprocess(&parser, source, abi, err, err_len, NULL))
    return 0;
  for (size_t i = 0; i < parser.file_count; ++i) {
    const char *path = parser.files[i].path;
    if (!path || path[0] == '<')
      continue;
    if (!visitor(path, context)) {
      snprintf(err, err_len, "cannot write dependency path");
      parser_destroy(&parser);
      return 0;
    }
  }
  parser_destroy(&parser);
  return 1;
}

int nyc_frontend_compile(const nyc_source_t *source, const char *symbol,
                         nyc_abi_t abi, nyr_func_t *out, char *err,
                         size_t err_len) {
  if (!source || !source->text || !symbol || !out || !err || err_len == 0)
    return 0;
  err[0] = '\0';
  c_parser_t parser;
  if (!parser_prepare(&parser, source, abi, err, err_len))
    return 0;
  ny_ctok_t wanted = {
      .kind = NY_CTOK_IDENT, .start = symbol, .len = strlen(symbol)};
  c_function_t *fn = find_function(&parser, wanted);
  bool ok = false;
  if (!fn || !fn->defined)
    snprintf(err, err_len, "function definition '%s' not found", symbol);
  else
    ok = compile_function(&parser, fn, out);
  parser_destroy(&parser);
  return ok;
}

static void free_exported_data(nyc_data_t *data) {
  free(data->name);
  free(data->section);
  free(data->bytes);
  free(data->owner_function);
  for (size_t r = 0; r < data->reloc_count; ++r)
    free(data->relocs[r].symbol);
  free(data->relocs);
}

enum {
  C_GLOBAL_ATTRIBUTE_WEAK = 1u << 0,
  C_GLOBAL_ATTRIBUTE_USED = 1u << 1,
};

static unsigned global_declaration_attributes(c_parser_t *p, size_t begin,
                                              size_t end) {
  unsigned attributes = 0;
  for (size_t i = begin; i < end; ++i) {
    if (!tok_is_attribute(p, i) || !tok_is(p, i + 1, "("))
      continue;
    size_t after = skip_balanced(p, i + 1, "(", ")");
    if (after <= i + 1 || after > end)
      continue;
    for (size_t j = i + 2; j < after; ++j) {
      if (tok_is(p, j, "weak") || tok_is(p, j, "__weak") ||
          tok_is(p, j, "__weak__"))
        attributes |= C_GLOBAL_ATTRIBUTE_WEAK;
      if (tok_is(p, j, "used") || tok_is(p, j, "__used__") ||
          tok_is(p, j, "retain") || tok_is(p, j, "__retain__"))
        attributes |= C_GLOBAL_ATTRIBUTE_USED;
    }
    i = after - 1;
  }
  return attributes;
}

static bool parse_global_binding_declarator(c_parser_t *p, ny_ctok_t *name) {
  while (tok_is_attribute(p, p->pos))
    if (!parse_function_type_attributes(p))
      return false;
  unsigned levels = 0;
  bool pointer_const = false;
  if (!parse_type_pointers(p, &levels, &pointer_const))
    return false;
  if (accept(p, "(")) {
    if (!parse_global_binding_declarator(p, name) || !expect(p, ")"))
      return false;
  } else if (!parse_name(p, name)) {
    return false;
  }
  for (;;) {
    if (tok_is_attribute(p, p->pos)) {
      if (!parse_function_type_attributes(p))
        return false;
    } else if (tok_is(p, p->pos, "(") || tok_is(p, p->pos, "[")) {
      const char *left = tok_is(p, p->pos, "(") ? "(" : "[";
      size_t after = skip_balanced(p, p->pos, left, left[0] == '(' ? ")" : "]");
      if (after <= p->pos || after > p->token_count)
        return false;
      p->pos = after;
    } else {
      return true;
    }
  }
}

/*
 * Reuse type and pointer parsing, not identifier occurrences in expressions or
 * prototypes. Declaration-specifier attributes apply to every declarator;
 * attributes after a pointer star or a name belong only to that declarator.
 */
static bool index_global_declaration(c_parser_t *p, size_t begin, size_t end,
                                     const size_t *index, size_t mask) {
  for (size_t i = begin; i < end; ++i)
    if (tok_is(p, i, "typedef"))
      return true;
  c_parser_t probe = *p;
  char error[1] = {0};
  probe.err = error;
  probe.err_len = sizeof(error);
  probe.type_lookahead = true;
  probe.last_function_alias = (ny_ctok_t){0};
  probe.pos = begin;
  c_type_t type;
  if (!parse_type(&probe, &type, true)) {
    fail(p, "cannot parse file-scope declaration binding");
    return false;
  }
  if (probe.pos >= end)
    return true;
  size_t declarator = probe.pos;
  for (size_t i = begin; i < probe.pos; ++i) {
    if (tok_is(p, i, "*")) {
      declarator = i;
      break;
    }
    if (tok_is(p, i, "(") || tok_is(p, i, "{") || tok_is(p, i, "[")) {
      const char *left = tok_is(p, i, "(")   ? "("
                         : tok_is(p, i, "{") ? "{"
                                             : "[";
      const char *right = left[0] == '(' ? ")" : left[0] == '{' ? "}" : "]";
      size_t after = skip_balanced(p, i, left, right);
      if (after > i && after <= probe.pos)
        i = after - 1;
    }
  }
  unsigned common_attributes =
      global_declaration_attributes(p, begin, declarator);
  bool is_local = false;
  for (size_t i = begin; i < declarator; ++i)
    is_local |= tok_is(p, i, "static");
  for (;;) {
    ny_ctok_t name = {0};
    probe.last_function_alias = (ny_ctok_t){0};
    if (!parse_global_binding_declarator(&probe, &name)) {
      char message[192];
      ny_ctok_t token = *current(&probe);
      snprintf(message, sizeof(message),
               "cannot locate file-scope declaration name near `%.*s`",
               (int)(token.len > 96 ? 96 : token.len), token.start);
      size_t saved_pos = p->pos;
      p->pos = probe.pos;
      fail(p, message);
      p->pos = saved_pos;
      return false;
    }
    size_t attributes_end = end;
    size_t at = probe.pos;
    for (; at < end; ++at) {
      if (tok_is(p, at, "=") && attributes_end == end)
        attributes_end = at;
      if (tok_is(p, at, ",") || tok_is(p, at, ";"))
        break;
      if (tok_is(p, at, "(") || tok_is(p, at, "{") || tok_is(p, at, "[")) {
        const char *left = tok_is(p, at, "(")   ? "("
                           : tok_is(p, at, "{") ? "{"
                                                : "[";
        const char *right = left[0] == '(' ? ")" : left[0] == '{' ? "}" : "]";
        size_t after = skip_balanced(p, at, left, right);
        if (after > at && after <= end)
          at = after - 1;
      }
    }
    if (attributes_end > at)
      attributes_end = at;
    size_t slot = (size_t)c_name_hash(name) & mask;
    while (index[slot]) {
      c_global_t *global = &p->globals[index[slot] - 1];
      if (tok_same(global->name, name)) {
        global->is_local |= is_local;
        unsigned attributes =
            common_attributes |
            global_declaration_attributes(p, declarator, attributes_end);
        global->is_weak |= (attributes & C_GLOBAL_ATTRIBUTE_WEAK) != 0;
        global->is_used |= (attributes & C_GLOBAL_ATTRIBUTE_USED) != 0;
        break;
      }
      slot = (slot + 1) & mask;
    }
    if (at >= end || !tok_is(p, at, ","))
      return true;
    declarator = probe.pos = at + 1;
  }
}

static bool index_global_bindings(c_parser_t *p) {
  if (!p->global_count)
    return true;
  if (p->global_count > SIZE_MAX / 2)
    return false;
  size_t capacity = 64;
  while (capacity < p->global_count * 2) {
    if (capacity > SIZE_MAX / 2)
      return false;
    capacity *= 2;
  }
  size_t body_bytes = p->token_count / 8 + (p->token_count % 8 != 0);
  if (capacity > (SIZE_MAX - body_bytes) / sizeof(size_t))
    return false;
  size_t *index = calloc(1, capacity * sizeof(*index) + body_bytes);
  if (!index)
    return false;
  size_t mask = capacity - 1;
  unsigned char *bodies = (unsigned char *)(index + capacity);
  for (size_t i = 0; i < p->function_count; ++i)
    if (p->functions[i].defined && p->functions[i].body_start > 0 &&
        p->functions[i].body_start < p->token_count) {
      size_t start = p->functions[i].body_start;
      bodies[start / 8] |= (unsigned char)(1u << (start % 8));
    }
  for (size_t i = 0; i < p->global_count; ++i) {
    size_t slot = (size_t)c_name_hash(p->globals[i].name) & mask;
    while (index[slot])
      slot = (slot + 1) & mask;
    index[slot] = i + 1;
  }
  size_t begin = 0, braces = 0, parentheses = 0, brackets = 0;
  bool relevant = false, ok = true;
  for (size_t i = 0; ok && i < p->token_count; ++i) {
    if (bodies[i / 8] & (1u << (i % 8))) {
      if (relevant)
        ok = index_global_declaration(p, begin, i - 1, index, mask);
      size_t after = skip_balanced(p, i - 1, "{", "}");
      i = after - 1;
      begin = after;
      braces = parentheses = brackets = 0;
      relevant = false;
      continue;
    }
    if (tok_is(p, i, "{")) {
      ++braces;
      continue;
    }
    if (tok_is(p, i, "}")) {
      if (braces)
        --braces;
      continue;
    }
    if (braces)
      continue;
    if (tok_is_attribute(p, i) && tok_is(p, i + 1, "(")) {
      size_t after = skip_balanced(p, i + 1, "(", ")");
      relevant |= global_declaration_attributes(p, i, after);
      i = after - 1;
      continue;
    }
    if (!parentheses && !brackets && tok_is(p, i, "static"))
      relevant = true;
    if (tok_is(p, i, "("))
      ++parentheses;
    else if (tok_is(p, i, ")") && parentheses)
      --parentheses;
    else if (tok_is(p, i, "["))
      ++brackets;
    else if (tok_is(p, i, "]") && brackets)
      --brackets;
    else if (!parentheses && !brackets && tok_is(p, i, ";")) {
      if (relevant)
        ok = index_global_declaration(p, begin, i, index, mask);
      begin = i + 1;
      relevant = false;
    }
  }
  free(index);
  return ok;
}

static size_t global_data_alignment(nyc_abi_t abi, const c_global_t *global) {
  size_t alignment = global->alignment;
  if (global->type == C_AGG && global->agg) {
    if (global->agg->align > alignment)
      alignment = global->agg->align;
  } else {
    size_t natural = c_type_alignment(abi, global->type);
    if (natural > alignment)
      alignment = natural;
  }
  return alignment;
}

static bool export_global_data(c_parser_t *p, c_global_t *global,
                               nyc_data_t *out) {
  nyc_data_t data = {
      .name = strndup(global->name.start, global->name.len),
      .len = global->len,
      .section = global->section ? strdup(global->section) : NULL,
      .owner_function = global->owner_function.len
                            ? strndup(global->owner_function.start,
                                      global->owner_function.len)
                            : NULL,
      .section_flags = global->section_flags ? global->section_flags
                                             : (global->is_const ? 2 : 3),
      .alignment = global_data_alignment(p->abi, global),
      .section_entsize = global->section_entsize,
      .function_symbol = global->function_symbol,
      .is_weak = global->is_weak,
      .is_used = global->is_used,
      .is_local = global->is_local ||
                  (global->name.len >= 13 &&
                   memcmp(global->name.start, "__nyc_string_", 13) == 0) ||
                  (global->name.len >= 13 &&
                   memcmp(global->name.start, "__nyc_static_", 13) == 0)};
  if (!data.name || (global->section && !data.section) ||
      (global->owner_function.len && !data.owner_function))
    goto allocation_failed;
  if (global->reloc_count) {
    data.relocs = calloc(global->reloc_count, sizeof(*data.relocs));
    if (!data.relocs)
      goto allocation_failed;
    for (size_t r = 0; r < global->reloc_count; ++r) {
      ny_ctok_t target = global->relocs[r].target;
      char *symbol = strndup(target.start, target.len);
      if (!symbol)
        goto allocation_failed;
      data.relocs[data.reloc_count++] =
          (nyc_data_reloc_t){.offset = global->relocs[r].offset,
                             .symbol = symbol,
                             .addend = global->relocs[r].addend,
                             .width = global->relocs[r].width,
                             .type = global->relocs[r].type};
    }
  }
  /*
   * Bytes move to the unit; parser-owned relocation tokens do not.
   */
  data.bytes = global->bytes;
  global->bytes = NULL;
  *out = data;
  return true;

allocation_failed:
  free_exported_data(&data);
  fail(p, "out of memory exporting C global data");
  return false;
}

typedef struct {
  const c_function_t **slots;
  uint32_t *hashes;
  size_t mask;
} c_fn_name_index_t;

static uint32_t fn_name_hash_bytes(const char *name, size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; ++i)
    h = (h ^ (unsigned char)name[i]) * 16777619u;
  return h ? h : 1u;
}

static void fn_name_index_free(c_fn_name_index_t *idx) {
  if (!idx)
    return;
  free(idx->slots);
  free(idx->hashes);
  idx->slots = NULL;
  idx->hashes = NULL;
  idx->mask = 0;
}

static bool fn_name_index_build(const c_parser_t *parser,
                                c_fn_name_index_t *idx) {
  memset(idx, 0, sizeof(*idx));
  if (!parser || !parser->function_count)
    return true;
  size_t need = 1;
  while (need < parser->function_count * 2)
    need <<= 1;
  idx->slots = calloc(need, sizeof(*idx->slots));
  idx->hashes = calloc(need, sizeof(*idx->hashes));
  if (!idx->slots || !idx->hashes) {
    fn_name_index_free(idx);
    return false;
  }
  idx->mask = need - 1;
  for (size_t i = 0; i < parser->function_count; ++i) {
    const c_function_t *fn = &parser->functions[i];
    if (!fn->name.start || !fn->name.len)
      continue;
    uint32_t h = fn_name_hash_bytes(fn->name.start, fn->name.len);
    size_t slot = h & idx->mask;
    while (idx->slots[slot]) {
      const c_function_t *cur = idx->slots[slot];
      if (idx->hashes[slot] == h && cur->name.len == fn->name.len &&
          memcmp(cur->name.start, fn->name.start, fn->name.len) == 0)
        break;
      slot = (slot + 1) & idx->mask;
    }
    if (!idx->slots[slot]) {
      idx->slots[slot] = fn;
      idx->hashes[slot] = h;
    }
  }
  return true;
}

static const c_function_t *fn_name_index_lookup(const c_fn_name_index_t *idx,
                                                const char *name, size_t len) {
  if (!idx || !idx->slots || !name)
    return NULL;
  uint32_t h = fn_name_hash_bytes(name, len);
  size_t slot = h & idx->mask;
  while (idx->slots[slot]) {
    const c_function_t *fn = idx->slots[slot];
    if (idx->hashes[slot] == h && fn->name.len == len &&
        memcmp(fn->name.start, name, len) == 0)
      return fn;
    slot = (slot + 1) & idx->mask;
  }
  return NULL;
}

static const c_function_t *unit_function_declaration(const c_parser_t *parser,
                                                     const char *name) {
  if (!parser || !name)
    return NULL;
  size_t len = strlen(name);
  for (size_t i = 0; i < parser->function_count; ++i) {
    const c_function_t *fn = &parser->functions[i];
    if (fn->name.len == len && memcmp(fn->name.start, name, len) == 0)
      return fn;
  }
  return NULL;
}

static bool unit_symbol_matches_function(const c_parser_t *parser,
                                         const nyc_unit_t *unit, size_t index,
                                         const char *symbol,
                                         const c_fn_name_index_t *fnidx) {
  if (!symbol || index >= unit->count)
    return false;
  if (strcmp(symbol, unit->names[index]) == 0)
    return true;
  const c_function_t *fn =
      fnidx && fnidx->slots
          ? fn_name_index_lookup(fnidx, unit->names[index],
                                 strlen(unit->names[index]))
          : unit_function_declaration(parser, unit->names[index]);
  return fn && fn->asm_alias.len && strlen(symbol) == fn->asm_alias.len &&
         memcmp(symbol, fn->asm_alias.start, fn->asm_alias.len) == 0;
}

static size_t unit_data_owner_index(const nyc_unit_t *unit,
                                    const nyc_data_t *data, size_t count) {
  if (!data->owner_function || !data->owner_function[0])
    return SIZE_MAX;
  for (size_t i = 0; i < count; ++i)
    if (strcmp(unit->names[i], data->owner_function) == 0)
      return i;
  return SIZE_MAX;
}

static bool unit_symbol_matches_data(const nyc_data_t *data,
                                     const char *symbol) {
  static const char prefix[] = "__nyc_data_";
  if (!data->name || !symbol)
    return false;
  if (strcmp(symbol, data->name) == 0)
    return true;
  return strncmp(symbol, prefix, sizeof(prefix) - 1) == 0 &&
         strcmp(symbol + sizeof(prefix) - 1, data->name) == 0;
}

static void reference_functions_from_data(const c_parser_t *parser,
                                          const nyc_unit_t *unit,
                                          const c_fn_name_index_t *fnidx,
                                          const nyc_data_t *data,
                                          const bool *candidate,
                                          bool *referenced, size_t *worklist,
                                          size_t *worklist_len, size_t count) {
  for (size_t reloc = 0; reloc < data->reloc_count; ++reloc)
    for (size_t fn = 0; fn < count; ++fn)
      if (candidate[fn] && !referenced[fn] &&
          unit_symbol_matches_function(parser, unit, fn,
                                       data->relocs[reloc].symbol, fnidx)) {
        referenced[fn] = true;
        worklist[(*worklist_len)++] = fn;
      }
}

static bool unit_asm_mentions_function(const c_parser_t *parser,
                                       const nyc_unit_t *unit, size_t index,
                                       const char *assembly,
                                       const c_fn_name_index_t *fnidx) {
  if (!assembly || index >= unit->count)
    return false;
  size_t assembly_len = strlen(assembly);
  const char *name = unit->names[index];
  size_t name_len = strlen(name);
  if (name_len <= assembly_len)
    for (size_t i = 0; i <= assembly_len - name_len; ++i) {
      if (memcmp(assembly + i, name, name_len) != 0 ||
          (i && (isalnum((unsigned char)assembly[i - 1]) ||
                 assembly[i - 1] == '_')) ||
          isalnum((unsigned char)assembly[i + name_len]) ||
          assembly[i + name_len] == '_')
        continue;
      const char *begin = assembly + i;
      while (begin > assembly && begin[-1] != '\n' && begin[-1] != ';')
        --begin;
      while (*begin == ' ' || *begin == '\t')
        ++begin;
      const char *end = assembly + i + name_len;
      while (*end == ' ' || *end == '\t' || *end == '\r')
        ++end;
      /*
       * A bare instruction mnemonic is not a C function reference.
       */
      if (begin != assembly + i || (*end && *end != '\n' && *end != ';'))
        return true;
    }
  const c_function_t *fn = fnidx && fnidx->slots
                               ? fn_name_index_lookup(fnidx, name, name_len)
                               : unit_function_declaration(parser, name);
  if (!fn || !fn->asm_alias.len || fn->asm_alias.len > assembly_len)
    return false;
  for (size_t i = 0; i <= assembly_len - fn->asm_alias.len; ++i)
    if (memcmp(assembly + i, fn->asm_alias.start, fn->asm_alias.len) == 0)
      return true;
  return false;
}

static size_t unit_data_reference_index(const nyc_unit_t *unit,
                                        const size_t *index, size_t mask,
                                        const char *symbol, size_t len) {
  static const char data_prefix[] = "__nyc_data_";
  static const char external_prefix[] = "__nyc_extern_";
  if (!symbol || !len)
    return SIZE_MAX;
  if (len >= sizeof(data_prefix) - 1 &&
      memcmp(symbol, data_prefix, sizeof(data_prefix) - 1) == 0) {
    symbol += sizeof(data_prefix) - 1;
    len -= sizeof(data_prefix) - 1;
  } else if (len >= sizeof(external_prefix) - 1 &&
             memcmp(symbol, external_prefix, sizeof(external_prefix) - 1) ==
                 0) {
    symbol += sizeof(external_prefix) - 1;
    len -= sizeof(external_prefix) - 1;
  }
  size_t slot =
      (size_t)c_name_hash((ny_ctok_t){.start = symbol, .len = len}) & mask;
  while (index[slot]) {
    size_t at = index[slot] - 1;
    const char *name = unit->data[at].name;
    if (strlen(name) == len && memcmp(name, symbol, len) == 0)
      return at;
    slot = (slot + 1) & mask;
  }
  return SIZE_MAX;
}

static void unit_mark_assembly_data(const nyc_unit_t *unit, const size_t *index,
                                    size_t mask, const char *assembly,
                                    bool *referenced) {
  if (!assembly)
    return;
  for (const char *at = assembly; *at;) {
    if (*at == '"') {
      for (++at; *at && *at != '"'; ++at)
        if (*at == '\\' && at[1])
          ++at;
      if (*at)
        ++at;
      continue;
    }
    if (!(isalnum((unsigned char)*at) || *at == '_' || *at == '.' ||
          *at == '$')) {
      ++at;
      continue;
    }
    const char *begin = at++;
    while (isalnum((unsigned char)*at) || *at == '_' || *at == '.' ||
           *at == '$')
      ++at;
    size_t data = unit_data_reference_index(unit, index, mask, begin,
                                            (size_t)(at - begin));
    if (data != SIZE_MAX)
      referenced[data] = true;
  }
}

/*
 * Conservatively count references from every function and initializer,
 * including currently unreachable ones. This removes unreferenced leaves
 * without requiring a new whole-program data reachability convention.
 */
static bool unit_mark_data_references(const nyc_unit_t *unit, bool *referenced,
                                      bool *roots) {
  if (!unit->data_count)
    return true;
  if (unit->data_count > SIZE_MAX / 2)
    return false;
  size_t capacity = 16;
  while (capacity < unit->data_count * 2) {
    if (capacity > SIZE_MAX / 2)
      return false;
    capacity *= 2;
  }
  size_t *index = calloc(capacity, sizeof(*index));
  if (!index)
    return false;
  size_t mask = capacity - 1;
  for (size_t i = 0; i < unit->data_count; ++i) {
    ny_ctok_t name = {.start = unit->data[i].name,
                      .len = strlen(unit->data[i].name)};
    size_t slot = (size_t)c_name_hash(name) & mask;
    while (index[slot])
      slot = (slot + 1) & mask;
    index[slot] = i + 1;
  }
  for (size_t i = 0; i < unit->count; ++i)
    for (size_t j = 0; j < unit->funcs[i].len; ++j) {
      const nyr_inst_t *in = &unit->funcs[i].data[j];
      if (in->op == NYIR_INLINE_ASM)
        unit_mark_assembly_data(unit, index, mask, in->symbol, referenced);
      else if (in->op == NYIR_ADDR_SYMBOL && in->symbol) {
        size_t data = unit_data_reference_index(unit, index, mask, in->symbol,
                                                strlen(in->symbol));
        if (data != SIZE_MAX)
          referenced[data] = true;
      }
    }
  for (size_t i = 0; i < unit->data_count; ++i)
    for (size_t j = 0; j < unit->data[i].reloc_count; ++j) {
      const char *symbol = unit->data[i].relocs[j].symbol;
      size_t data = unit_data_reference_index(unit, index, mask, symbol,
                                              symbol ? strlen(symbol) : 0);
      if (data != SIZE_MAX)
        referenced[data] = true;
    }
  for (size_t i = 0; i < unit->variable_alias_count; ++i) {
    const char *symbol = unit->variable_aliases[i].target;
    size_t data = unit_data_reference_index(unit, index, mask, symbol,
                                            symbol ? strlen(symbol) : 0);
    if (data != SIZE_MAX)
      roots[data] = true;
  }
  unit_mark_assembly_data(unit, index, mask, unit->raw_file_scope_asm, roots);
  free(index);
  return true;
}

static bool unit_data_is_discardable(const nyc_data_t *data) {
  return data->is_local && !data->is_used && !data->section &&
         !data->function_symbol &&
         (!data->owner_function || !data->owner_function[0]);
}

static bool prune_unreferenced_static_functions(c_parser_t *parser,
                                                nyc_unit_t *unit, char *err,
                                                size_t err_len) {
  size_t count = unit->count;
  if (!count)
    return true;
  c_fn_name_index_t fnidx = {0};
  if (parser && !fn_name_index_build(parser, &fnidx)) {
    snprintf(err, err_len, "out of memory indexing functions for pruning");
    return false;
  }
  bool *candidate = calloc(count, sizeof(*candidate));
  bool *referenced = calloc(count, sizeof(*referenced));
  size_t *worklist = calloc(count, sizeof(*worklist));
  bool *data_referenced =
      unit->data_count ? calloc(unit->data_count, 2 * sizeof(*data_referenced))
                       : NULL;
  bool *file_data_referenced =
      data_referenced ? data_referenced + unit->data_count : NULL;
  if (!candidate || !referenced || !worklist ||
      (unit->data_count && !data_referenced)) {
    free(candidate);
    free(referenced);
    free(worklist);
    free(data_referenced);
    fn_name_index_free(&fnidx);
    snprintf(err, err_len, "out of memory pruning unused static functions");
    return false;
  }
  size_t worklist_len = 0;
  for (size_t i = 0; i < count; ++i) {
    const c_function_t *fn =
        fnidx.slots ? fn_name_index_lookup(&fnidx, unit->names[i],
                                           strlen(unit->names[i]))
                    : unit_function_declaration(parser, unit->names[i]);
    candidate[i] = parser ? fn && fn->defined && fn->is_static &&
                                !fn->is_used && !fn->has_asm_alias &&
                                (!fn->section || fn->is_inline)
                          : unit->function_discardable[i];
    /*
     * A GNU extern-inline body is available to the inliner, but references
     * that survive inlining bind to an external definition, not this body.
     */
    if (!parser && unit->function_no_emit[i]) {
      candidate[i] = false;
      continue;
    }
    referenced[i] = !candidate[i];
    if (referenced[i])
      worklist[worklist_len++] = i;
  }
  if (!unit_mark_data_references(unit, file_data_referenced, data_referenced)) {
    free(candidate);
    free(referenced);
    free(worklist);
    free(data_referenced);
    fn_name_index_free(&fnidx);
    snprintf(err, err_len, "out of memory indexing C data references");
    return false;
  }
  for (size_t alias = 0; alias < unit->alias_count; ++alias)
    for (size_t fn = 0; fn < count; ++fn)
      if (candidate[fn] && !referenced[fn] &&
          unit_symbol_matches_function(parser, unit, fn,
                                       unit->aliases[alias].target, &fnidx)) {
        referenced[fn] = true;
        worklist[worklist_len++] = fn;
      }
  for (size_t data = 0; data < unit->data_count; ++data)
    if (data_referenced[data] ||
        (unit_data_owner_index(unit, &unit->data[data], count) == SIZE_MAX &&
         (!unit_data_is_discardable(&unit->data[data]) ||
          file_data_referenced[data])))
      reference_functions_from_data(parser, unit, &fnidx, &unit->data[data],
                                    candidate, referenced, worklist,
                                    &worklist_len, count);

  /*
   * Resolve asm-name references only from reachable functions' templates.
   * Other source strings (for example static-assert messages) are not symbol
   * references and must not keep static functions alive.
   */
  for (size_t queued = 0; queued < worklist_len; ++queued) {
    size_t from = worklist[queued];
    for (size_t data = 0; data < unit->data_count; ++data)
      if (unit_data_owner_index(unit, &unit->data[data], count) == from)
        reference_functions_from_data(parser, unit, &fnidx, &unit->data[data],
                                      candidate, referenced, worklist,
                                      &worklist_len, count);
    for (size_t inst = 0; inst < unit->funcs[from].len; ++inst) {
      const nyr_inst_t *in = &unit->funcs[from].data[inst];
      if (in->op == NYIR_ADDR_SYMBOL && in->symbol)
        for (size_t data = 0; data < unit->data_count; ++data)
          if (!data_referenced[data] &&
              unit_symbol_matches_data(&unit->data[data], in->symbol)) {
            data_referenced[data] = true;
            reference_functions_from_data(
                parser, unit, &fnidx, &unit->data[data], candidate, referenced,
                worklist, &worklist_len, count);
          }
      bool direct_reference = in->op == NYIR_CALL || in->op == NYIR_ADDR_SYMBOL;
      if ((!direct_reference && in->op != NYIR_INLINE_ASM) || !in->symbol)
        continue;
      for (size_t to = 0; to < count; ++to) {
        if (!candidate[to] || referenced[to])
          continue;
        bool use =
            direct_reference &&
            unit_symbol_matches_function(parser, unit, to, in->symbol, &fnidx);
        if (!use && in->op == NYIR_INLINE_ASM)
          use =
              unit_asm_mentions_function(parser, unit, to, in->symbol, &fnidx);
        if (use) {
          referenced[to] = true;
          worklist[worklist_len++] = to;
        }
      }
    }
  }

  size_t data_output = 0;
  for (size_t i = 0; i < unit->data_count; ++i) {
    size_t owner = unit_data_owner_index(unit, &unit->data[i], count);
    if (owner != SIZE_MAX &&
        ((!parser && unit->function_no_emit[owner]) ||
         (candidate[owner] && !referenced[owner])) &&
        !data_referenced[i]) {
      free_exported_data(&unit->data[i]);
      continue;
    }
    if (owner == SIZE_MAX && unit_data_is_discardable(&unit->data[i]) &&
        !file_data_referenced[i] && !data_referenced[i]) {
      free_exported_data(&unit->data[i]);
      continue;
    }
    if (data_output != i)
      unit->data[data_output] = unit->data[i];
    ++data_output;
  }
  unit->data_count = data_output;

  size_t output = 0;
  for (size_t i = 0; i < count; ++i) {
    if ((!parser && unit->function_no_emit[i]) ||
        (candidate[i] && !referenced[i])) {
      nyr_func_destroy(&unit->funcs[i]);
      free(unit->names[i]);
      free(unit->function_sections[i]);
      unit->names[i] = NULL;
      unit->function_sections[i] = NULL;
      continue;
    }
    if (output != i) {
      unit->funcs[output] = unit->funcs[i];
      memset(&unit->funcs[i], 0, sizeof(unit->funcs[i]));
      unit->names[output] = unit->names[i];
      unit->names[i] = NULL;
      unit->function_sections[output] = unit->function_sections[i];
      unit->function_sections[i] = NULL;
      unit->function_local[output] = unit->function_local[i];
      unit->function_inline[output] = unit->function_inline[i];
      unit->function_always_inline[output] = unit->function_always_inline[i];
      unit->function_noinline[output] = unit->function_noinline[i];
      unit->function_no_stack_protector[output] =
          unit->function_no_stack_protector[i];
      unit->function_no_instrument_function[output] =
          unit->function_no_instrument_function[i];
      unit->function_weak[output] = unit->function_weak[i];
      unit->function_no_emit[output] = unit->function_no_emit[i];
      unit->function_discardable[output] = unit->function_discardable[i];
    }
    ++output;
  }
  unit->count = output;
  free(candidate);
  free(referenced);
  free(worklist);
  free(data_referenced);
  fn_name_index_free(&fnidx);
  return true;
}

bool nyc_unit_prune_static(nyc_unit_t *unit, char *err, size_t err_len) {
  return prune_unreferenced_static_functions(NULL, unit, err, err_len);
}

void nyc_unit_destroy(nyc_unit_t *unit) {
  if (!unit)
    return;
  for (size_t i = 0; unit->funcs && i < unit->count; ++i)
    nyr_func_destroy(&unit->funcs[i]);
  for (size_t i = 0; unit->names && i < unit->count; ++i)
    free(unit->names[i]);
  for (size_t i = 0; unit->data && i < unit->data_count; ++i)
    free_exported_data(&unit->data[i]);
  for (size_t i = 0; i < unit->alias_count; ++i) {
    free(unit->aliases[i].name);
    free(unit->aliases[i].target);
  }
  for (size_t i = 0; i < unit->variable_alias_count; ++i) {
    free(unit->variable_aliases[i].name);
    free(unit->variable_aliases[i].target);
  }
  free(unit->aliases);
  free(unit->variable_aliases);
  free(unit->funcs);
  free(unit->names);
  for (size_t i = 0; unit->function_sections && i < unit->count; ++i)
    free(unit->function_sections[i]);
  free(unit->function_sections);
  free(unit->function_local);
  free(unit->function_inline);
  free(unit->function_always_inline);
  free(unit->function_noinline);
  free(unit->function_no_stack_protector);
  free(unit->function_no_instrument_function);
  free(unit->function_weak);
  free(unit->function_no_emit);
  free(unit->function_discardable);
  free(unit->data);
  free(unit->raw_file_scope_asm);
  memset(unit, 0, sizeof(*unit));
}

int nyc_frontend_compile_unit(const nyc_source_t *source, nyc_abi_t abi,
                              nyc_unit_t *out, char *err, size_t err_len) {
  if (!source || !source->text || !out || !err || err_len == 0)
    return 0;
  memset(out, 0, sizeof(*out));
  err[0] = '\0';
  c_parser_t parser;
  if (!parser_prepare(&parser, source, abi, err, err_len))
    return 0;
  size_t count = 0;
  for (size_t i = 0; i < parser.function_count; ++i)
    count += parser.functions[i].defined;
  if (count > SIZE_MAX / sizeof(*out->funcs) ||
      count > SIZE_MAX / sizeof(*out->names)) {
    snprintf(err, err_len, "C translation unit has too many functions");
    parser_destroy(&parser);
    return 0;
  }
  if (count) {
    out->funcs = calloc(count, sizeof(*out->funcs));
    out->names = calloc(count, sizeof(*out->names));
    out->function_sections = calloc(count, sizeof(*out->function_sections));
    out->function_local = calloc(count, sizeof(*out->function_local));
    out->function_inline = calloc(count, sizeof(*out->function_inline));
    out->function_always_inline =
        calloc(count, sizeof(*out->function_always_inline));
    out->function_noinline = calloc(count, sizeof(*out->function_noinline));
    out->function_no_stack_protector =
        calloc(count, sizeof(*out->function_no_stack_protector));
    out->function_no_instrument_function =
        calloc(count, sizeof(*out->function_no_instrument_function));
    out->function_weak = calloc(count, sizeof(*out->function_weak));
    out->function_no_emit = calloc(count, sizeof(*out->function_no_emit));
    out->function_discardable =
        calloc(count, sizeof(*out->function_discardable));
    if (!out->funcs || !out->names || !out->function_sections ||
        !out->function_local || !out->function_inline ||
        !out->function_always_inline || !out->function_noinline ||
        !out->function_no_stack_protector ||
        !out->function_no_instrument_function || !out->function_weak ||
        !out->function_no_emit || !out->function_discardable) {
      snprintf(err, err_len, "out of memory allocating C function bundle");
      out->count = count;
      nyc_unit_destroy(out);
      parser_destroy(&parser);
      return 0;
    }
  }
  out->count = count;
  for (size_t i = 0; i < count; ++i)
    nyr_func_init(&out->funcs[i]);
  size_t output_index = 0;
  bool ok = index_global_bindings(&parser);
  if (!ok && !err[0])
    snprintf(err, err_len, "out of memory indexing C global bindings");
  for (size_t i = 0; ok && i < parser.function_count; ++i) {
    c_function_t *fn = &parser.functions[i];
    if (!fn->defined)
      continue;
    char *name = malloc(fn->name.len + 1);
    if (!name) {
      snprintf(err, err_len, "out of memory copying C function name");
      ok = false;
      break;
    }
    memcpy(name, fn->name.start, fn->name.len);
    name[fn->name.len] = '\0';
    out->names[output_index] = name;
    out->function_local[output_index] = fn->is_static;
    out->function_inline[output_index] = fn->is_inline;
    out->function_always_inline[output_index] = fn->always_inline;
    out->function_noinline[output_index] = fn->noinline;
    out->function_no_stack_protector[output_index] = fn->no_stack_protector;
    out->function_no_instrument_function[output_index] =
        fn->no_instrument_function;
    out->function_weak[output_index] = fn->is_weak;
    out->function_no_emit[output_index] = fn->no_emit;
    out->function_discardable[output_index] = fn->is_static && !fn->is_used &&
                                              !fn->has_asm_alias &&
                                              (!fn->section || fn->is_inline);
    if (fn->section) {
      out->function_sections[output_index] = strdup(fn->section);
      if (!out->function_sections[output_index]) {
        snprintf(err, err_len, "out of memory copying C function section");
        ok = false;
        ++output_index;
        break;
      }
    }
    ok = compile_function(&parser, fn, &out->funcs[output_index]);
    out->function_noinline[output_index] = fn->noinline;
    if (!ok && !err[0])
      snprintf(err, err_len, "failed to compile C function `%s`", name);
    ++output_index;
  }
  if (ok) {
    size_t data_count = 0;
    for (size_t i = 0; i < parser.global_count; ++i)
      data_count += !parser.globals[i].is_external;
    if (data_count > SIZE_MAX / sizeof(*out->data) ||
        (data_count &&
         !(out->data = malloc(data_count * sizeof(*out->data))))) {
      snprintf(err, err_len, "out of memory exporting C global data");
      ok = false;
    }
  }
  for (size_t i = 0; ok && i < parser.global_count; ++i) {
    c_global_t *global = &parser.globals[i];
    if (global->is_external)
      continue;
    ok = export_global_data(&parser, global, &out->data[out->data_count]);
    if (ok)
      ++out->data_count;
  }
  if (ok) {
    size_t alias_count = 0;
    for (size_t i = 0; i < parser.function_count; ++i)
      alias_count += parser.functions[i].alias_target.len != 0;
    if (alias_count) {
      out->aliases = calloc(alias_count, sizeof(*out->aliases));
      if (!out->aliases) {
        snprintf(err, err_len, "out of memory exporting C function aliases");
        ok = false;
      }
    }
    for (size_t i = 0; ok && i < parser.function_count; ++i) {
      c_function_t *alias = &parser.functions[i];
      if (!alias->alias_target.len)
        continue;
      char target_name[256] = {0};
      if (!copy_quoted_token(&parser, alias->alias_target, target_name,
                             sizeof(target_name))) {
        ok = false;
        break;
      }
      c_function_t *target = NULL;
      for (size_t j = 0; j < parser.function_count; ++j) {
        c_function_t *candidate = &parser.functions[j];
        if (candidate->name.len == strlen(target_name) &&
            memcmp(candidate->name.start, target_name, candidate->name.len) ==
                0) {
          target = candidate;
          break;
        }
      }
      if (!target || !target->defined) {
        snprintf(err, err_len,
                 "function alias target `%s` must be defined in this "
                 "translation unit",
                 target_name);
        ok = false;
        break;
      }
      target->is_used = true;
      nyc_function_alias_t *out_alias = &out->aliases[out->alias_count];
      out_alias->name = strndup(alias->name.start, alias->name.len);
      out_alias->target = strndup(target->name.start, target->name.len);
      out_alias->target_local = target->is_static;
      out_alias->is_weak = alias->is_weak;
      if (!out_alias->name || !out_alias->target) {
        free(out_alias->name);
        free(out_alias->target);
        memset(out_alias, 0, sizeof(*out_alias));
        snprintf(err, err_len, "out of memory copying C function alias");
        ok = false;
        break;
      }
      ++out->alias_count;
    }
  }
  if (ok) {
    size_t alias_count = 0;
    for (size_t i = 0; i < parser.global_count; ++i)
      alias_count += parser.globals[i].alias_target.len != 0;
    if (alias_count) {
      out->variable_aliases =
          calloc(alias_count, sizeof(*out->variable_aliases));
      if (!out->variable_aliases) {
        snprintf(err, err_len, "out of memory exporting C object aliases");
        ok = false;
      }
    }
    for (size_t i = 0; ok && i < parser.global_count; ++i) {
      c_global_t *alias = &parser.globals[i];
      if (!alias->alias_target.len)
        continue;
      char target_name[256] = {0};
      if (!copy_quoted_token(&parser, alias->alias_target, target_name,
                             sizeof(target_name))) {
        ok = false;
        break;
      }
      c_global_t *target = NULL;
      for (size_t j = 0; j < parser.global_count; ++j) {
        c_global_t *candidate = &parser.globals[j];
        if (candidate->name.len == strlen(target_name) &&
            memcmp(candidate->name.start, target_name, candidate->name.len) ==
                0) {
          target = candidate;
          break;
        }
      }
      if (!target || target->is_external || target->type != alias->type) {
        snprintf(err, err_len,
                 "C object alias target `%s` must be a defined object of the "
                 "same type in this translation unit",
                 target_name);
        ok = false;
        break;
      }
      nyc_variable_alias_t *out_alias =
          &out->variable_aliases[out->variable_alias_count];
      out_alias->name = strndup(alias->name.start, alias->name.len);
      out_alias->target = strndup(target->name.start, target->name.len);
      out_alias->is_local = alias->is_local;
      if (!out_alias->name || !out_alias->target) {
        free(out_alias->name);
        free(out_alias->target);
        memset(out_alias, 0, sizeof(*out_alias));
        snprintf(err, err_len, "out of memory copying C object alias");
        ok = false;
        break;
      }
      ++out->variable_alias_count;
    }
  }
  if (ok)
    ok = prune_unreferenced_static_functions(&parser, out, err, err_len);
  if (ok) {
    out->raw_file_scope_asm = parser.raw_file_scope_asm;
    parser.raw_file_scope_asm = NULL;
    parser.raw_file_scope_asm_len = 0;
    parser.raw_file_scope_asm_capacity = 0;
  }
  parser_destroy(&parser);
  if (!ok)
    nyc_unit_destroy(out);
  return ok;
}
