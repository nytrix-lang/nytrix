#include "nyc_frontend.h"

#include "code/lang/c/c.h"
#include "code/lang/c/c_abi.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  C_I64,
  C_F64,
  C_I64_PTR,
  C_F64_PTR,
  C_VOID_PTR,
  C_BAD,
  /*
   * By-value struct/union. Values are i64 addresses of storage.
   */
  C_AGG,
  /*
   * Pointer to struct/union.
   */
  C_AGG_PTR
} c_type_t;

static bool c_type_is_pointer(c_type_t type) {
  return type == C_I64_PTR || type == C_F64_PTR || type == C_VOID_PTR ||
         type == C_AGG_PTR;
}

static bool c_type_is_arithmetic_pointer(c_type_t type) {
  return type == C_I64_PTR || type == C_F64_PTR;
}

typedef struct {
  ny_ctok_t name;
  c_type_t type;
  c_agg_type_t *agg;
  bool is_const;
} c_param_t;

typedef struct {
  ny_ctok_t name;
  c_type_t result;
  c_agg_type_t *result_agg;
  c_param_t *params;
  size_t param_count;
  size_t body_start;
  size_t body_end;
  bool defined;
} c_function_t;

typedef struct {
  ny_ctok_t name;
  c_type_t type;
  c_agg_type_t *agg;
  size_t slot;
  bool is_const;
  /*
   * Aggregate locals: element count for array locals, 0 for plain values.
   */
  size_t array_count;
} c_local_t;

typedef struct {
  ny_ctok_t *tokens;
  size_t token_count;
  size_t pos;
  c_agg_type_t **tags;
  size_t tag_count;
  size_t tag_capacity;
  c_function_t *functions;
  size_t function_count;
  size_t function_capacity;
  c_local_t *locals;
  size_t local_count;
  size_t local_capacity;
  size_t scope_start;
  size_t param_count;
  size_t *break_target;
  size_t *continue_target;
  bool last_type_const;
  bool last_value_const;
  /*
   * Aggregate type referenced by the most recent parse_type result.
   */
  c_agg_type_t *last_type_agg;
  bool last_value_const_addr;
  size_t next_local;
  int next_label;
  c_function_t *function;
  nyr_func_t *out;
  /*
   * Entry-block aggregate storage area. agg_base is the ALLOCA result value;
   * declarations and temporaries carve 8-byte-aligned chunks from it.
   */
  int agg_base;
  size_t agg_area_next;
  nyc_abi_t abi;
  char *err;
  size_t err_len;
} c_parser_t;

typedef struct {
  int value;
  c_type_t type;
  const c_agg_type_t *agg;
} c_value_t;

static bool tok_is(const c_parser_t *p, size_t pos, const char *text) {
  return pos < p->token_count && ny_ctok_eq(p->tokens[pos], text);
}

static ny_ctok_t *current(c_parser_t *p) {
  return &p->tokens[p->pos < p->token_count ? p->pos : p->token_count - 1];
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
  if (p->err && p->err_len && !p->err[0])
    snprintf(p->err, p->err_len, "%u:%u: expected '%s', found '%.*s'",
             current(p)->line, current(p)->col, text, (int)current(p)->len,
             current(p)->start);
  return false;
}

static bool tok_ident(ny_ctok_t tok) { return tok.kind == NY_CTOK_IDENT; }

static bool starts_scalar_type(const c_parser_t *p) {
  return tok_is(p, p->pos, "const") || tok_is(p, p->pos, "long") ||
         tok_is(p, p->pos, "double") || tok_is(p, p->pos, "unsigned") ||
         tok_is(p, p->pos, "int") || tok_is(p, p->pos, "void");
}

static bool starts_aggregate_type(const c_parser_t *p) {
  return tok_is(p, p->pos, "struct") || tok_is(p, p->pos, "union");
}

static bool tok_same(ny_ctok_t a, ny_ctok_t b) {
  return a.len == b.len && memcmp(a.start, b.start, a.len) == 0;
}

static void fail(c_parser_t *p, const char *message) {
  if (p->err && p->err_len && !p->err[0])
    snprintf(p->err, p->err_len, "%u:%u: %s", current(p)->line, current(p)->col,
             message);
}

static c_agg_type_t *find_tag(const c_parser_t *p, ny_ctok_t name,
                              bool is_union) {
  for (size_t i = p->tag_count; i > 0; --i) {
    c_agg_type_t *tag = p->tags[i - 1];
    if (tag->is_union == is_union && tok_same(tag->name, name))
      return tag;
  }
  return NULL;
}

static c_agg_type_t *find_tag_any(const c_parser_t *p, ny_ctok_t name) {
  for (size_t i = p->tag_count; i > 0; --i)
    if (tok_same(p->tags[i - 1]->name, name))
      return p->tags[i - 1];
  return NULL;
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
  tag->is_union = is_union;
  p->tags[p->tag_count++] = tag;
  return tag;
}

static void free_tag_table(c_parser_t *p) {
  for (size_t i = p->tag_count; i > 0; --i)
    free(p->tags[i - 1]);
  free(p->tags);
  p->tags = NULL;
  p->tag_count = 0;
  p->tag_capacity = 0;
}

static bool parse_name(c_parser_t *p, ny_ctok_t *name);

/*
 * Parse one field declarator inside a tag body. The field's type has already
 * been consumed; this parses optional pointer stars, the field name, and an
 * optional constant array extent.
 */
static bool parse_field_declarator(c_parser_t *p, c_type_t type,
                                   c_agg_type_t *type_agg,
                                   c_agg_field_t *field) {
  unsigned pointers = 0;
  while (accept(p, "*"))
    ++pointers;
  ny_ctok_t name;
  if (!parse_name(p, &name)) {
    fail(p, "expected struct field name");
    return false;
  }
  size_t count = 1;
  if (accept(p, "[")) {
    if (current(p)->kind != NY_CTOK_NUMBER) {
      fail(p, "array fields require a constant extent");
      return false;
    }
    ny_ctok_t extent = *current(p);
    ++p->pos;
    char buffer[32];
    if (extent.len >= sizeof(buffer)) {
      fail(p, "array field extent exceeds frontend limit");
      return false;
    }
    memcpy(buffer, extent.start, extent.len);
    buffer[extent.len] = '\0';
    char *end = NULL;
    unsigned long long parsed = strtoull(buffer, &end, 0);
    if (!end || end == buffer || *end || parsed == 0 || parsed > 65536) {
      fail(p, "invalid array field extent");
      return false;
    }
    if (!expect(p, "]"))
      return false;
    if (pointers != 0) {
      fail(p, "arrays of pointers are unsupported as struct fields");
      return false;
    }
    count = (size_t)parsed;
  }
  field->name = name;
  field->elem_count = count;
  if (pointers == 1) {
    field->kind = C_AGG_FIELD_PTR;
    field->agg = NULL;
    field->elem_f64 = false;
    field->size = 8;
    field->align = 8;
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
    field->kind = count > 1 ? C_AGG_FIELD_ARRAY : C_AGG_FIELD_AGG;
    return true;
  }
  if (type != C_I64 && type != C_F64) {
    fail(p, "struct fields must be long, double, pointer, struct, or union");
    return false;
  }
  field->agg = NULL;
  field->elem_f64 = type == C_F64;
  field->size = 8 * count;
  field->align = 8;
  field->kind = count > 1 ? C_AGG_FIELD_ARRAY
                          : (type == C_F64 ? C_AGG_FIELD_F64 : C_AGG_FIELD_I64);
  return true;
}

static bool parse_type(c_parser_t *p, c_type_t *type, bool allow_void);
static bool parse_tag_body(c_parser_t *p, ny_ctok_t name, bool is_union);
static bool parse_name(c_parser_t *p, ny_ctok_t *name);

static size_t align_up(size_t value, size_t align) {
  return align <= 1 ? value : (value + align - 1) / align * align;
}

static bool parse_type(c_parser_t *p, c_type_t *type, bool allow_void) {
  bool is_unsigned = false, is_double = false, is_long = false, is_void = false;
  p->last_type_const = false;
  p->last_type_agg = NULL;
  while (tok_is(p, p->pos, "const") || tok_is(p, p->pos, "volatile") ||
         tok_is(p, p->pos, "static") || tok_is(p, p->pos, "extern") ||
         tok_is(p, p->pos, "register") || tok_is(p, p->pos, "inline") ||
         tok_is(p, p->pos, "signed")) {
    if (tok_is(p, p->pos, "const")) {
      p->last_type_const = true;
      ++p->pos;
      continue;
    }
    if (tok_is(p, p->pos, "volatile")) {
      fail(p, "volatile C objects are unsupported");
      return false;
    }
    if (tok_is(p, p->pos, "static") || tok_is(p, p->pos, "inline")) {
      fail(p, "static and inline linkage are unsupported");
      return false;
    }
    if (tok_is(p, p->pos, "signed")) {
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
    ny_ctok_t tag_name = {0};
    if (tok_ident(*current(p))) {
      tag_name = *current(p);
      ++p->pos;
    }
    if (accept(p, "{")) {
      if (!tag_name.len) {
        fail(p, "anonymous struct/union definitions are unsupported");
        return false;
      }
      if (!parse_tag_body(p, tag_name, is_union))
        return false;
    } else if (!tag_name.len) {
      fail(p, "struct/union types require a tag name");
      return false;
    } else if (!find_tag(p, tag_name, is_union)) {
      if (!declare_tag(p, tag_name, is_union))
        return false;
    }
    *type = C_AGG;
    p->last_type_agg = find_tag(p, tag_name, is_union);
    unsigned pointers = 0;
    while (accept(p, "*"))
      ++pointers;
    if (pointers == 1) {
      *type = C_AGG_PTR;
    } else if (pointers > 1) {
      fail(p, "only single-level struct/union pointers are supported");
      return false;
    }
    return true;
  }
  while (tok_is(p, p->pos, "unsigned")) {
    is_unsigned = true;
    ++p->pos;
  }
  if (accept(p, "double"))
    is_double = true;
  else if (accept(p, "long")) {
    is_long = true;
    (void)accept(p, "long");
    (void)accept(p, "int");
  } else if (accept(p, "int")) {
    /*
     * int is only accepted where it is the conventional spelling of void.
     */
  } else if (accept(p, "void"))
    is_void = true;
  else {
    fail(p, "unsupported C type; expected signed long, double, or void");
    return false;
  }
  if (is_unsigned) {
    fail(p, "parameters and values must be signed 64-bit integers or double");
    return false;
  }
  if (is_double)
    *type = C_F64;
  else if (is_void)
    *type = C_BAD;
  else if (is_long)
    *type = C_I64;
  else {
    fail(p, "integer values must use signed 64-bit long");
    return false;
  }
  unsigned pointers = 0;
  while (accept(p, "*"))
    ++pointers;
  if (pointers == 1 && is_void)
    *type = C_VOID_PTR;
  else if (pointers == 1 && *type == C_I64)
    *type = C_I64_PTR;
  else if (pointers == 1 && *type == C_F64)
    *type = C_F64_PTR;
  else if (pointers != 0) {
    fail(p, "only pointers to signed 64-bit integers or double are supported");
    return false;
  }
  if (*type == C_BAD && !allow_void) {
    fail(p, "void is only valid as a result type or opaque pointer");
    return false;
  }
  return true;
}

static bool parse_name(c_parser_t *p, ny_ctok_t *name) {
  if (!tok_ident(*current(p))) {
    fail(p, "expected identifier");
    return false;
  }
  *name = *current(p);
  ++p->pos;
  return true;
}

/*
 * Parse the body of a struct/union definition and commit its layout.
 * Fields follow the same restricted type model as the rest of the
 * frontend: long, double, single-level pointers, nested struct/union
 * tags, and constant-extent arrays of those.
 */
static bool parse_tag_body(c_parser_t *p, ny_ctok_t name, bool is_union) {
  c_agg_type_t *tag = declare_tag(p, name, is_union);
  if (!tag)
    return false;
  if (tag->complete) {
    fail(p, "duplicate struct/union definition");
    return false;
  }
  size_t offset = 0;
  size_t max_align = 1;
  size_t size = 0;
  while (!accept(p, "}")) {
    if (p->pos >= p->token_count) {
      fail(p, "unterminated struct/union body");
      return false;
    }
    c_type_t field_type;
    if (!parse_type(p, &field_type, false))
      return false;
    c_agg_type_t *field_agg = p->last_type_agg;
    bool declarator_ok = true;
    do {
      if (tag->field_count >= C_AGG_MAX_FIELDS) {
        fail(p, "struct/union field count exceeds the frontend limit");
        return false;
      }
      c_agg_field_t field;
      memset(&field, 0, sizeof(field));
      if (!parse_field_declarator(p, field_type, field_agg, &field)) {
        declarator_ok = false;
        break;
      }
      if (tok_is(p, p->pos, ":")) {
        fail(p, "bit-field members are unsupported");
        return false;
      }
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

static c_function_t *find_function(c_parser_t *p, ny_ctok_t name) {
  for (size_t i = 0; i < p->function_count; ++i)
    if (tok_same(p->functions[i].name, name))
      return &p->functions[i];
  return NULL;
}

static void free_function_table(c_parser_t *p) {
  if (!p)
    return;
  for (size_t i = 0; i < p->function_count; ++i)
    free(p->functions[i].params);
  free(p->functions);
  p->functions = NULL;
  p->function_count = 0;
}

static bool parse_signature(c_parser_t *p, c_function_t *fn) {
  c_param_t *params = NULL;
  size_t count = 0;
  size_t capacity = 0;
  c_type_t result;
  if (!parse_type(p, &result, true))
    return false;
  c_agg_type_t *result_agg = result == C_AGG ? p->last_type_agg : NULL;
  ny_ctok_t name;
  if (!parse_name(p, &name) || !expect(p, "("))
    return false;
  if (!accept(p, ")")) {
    if (tok_is(p, p->pos, "void") && tok_is(p, p->pos + 1, ")")) {
      p->pos += 2;
    } else {
      for (;;) {
        c_type_t type;
        if (!parse_type(p, &type, false))
          goto error;
        c_agg_type_t *param_agg = type == C_AGG ? p->last_type_agg : NULL;
        bool is_const = p->last_type_const;
        ny_ctok_t param_name = {0};
        if (tok_ident(*current(p))) {
          param_name = *current(p);
          ++p->pos;
        }
        if (accept(p, "[")) {
          if (!expect(p, "]")) {
            fail(p,
                 "only unsized one-dimensional array parameters are supported");
            goto error;
          }
          if (type == C_I64)
            type = C_I64_PTR;
          else if (type == C_F64)
            type = C_F64_PTR;
          else if (type == C_AGG)
            type = C_AGG_PTR;
          else {
            fail(p, "array parameters require signed long, double, or struct "
                    "elements");
            goto error;
          }
        }
        if (type == C_BAD) {
          fail(p, "parameter type must be a signed 64-bit integer, double, "
                  "pointer, struct, or union");
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
        params[count++] = (c_param_t){.name = param_name,
                                      .type = type,
                                      .agg = param_agg,
                                      .is_const = is_const};
        if (accept(p, ")"))
          break;
        if (!expect(p, ","))
          goto error;
      }
    }
  }
  if (fn) {
    fn->name = name;
    fn->result = result;
    fn->result_agg = result_agg;
    fn->params = params;
    fn->param_count = count;
  } else {
    free(params);
  }
  return true;

error:
  free(params);
  return false;
}

static bool collect_functions(c_parser_t *p) {
  p->pos = 0;
  while (p->pos + 1 < p->token_count) {
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
      } else if (tok_ident(*current(p)) && tok_is(p, p->pos + 1, "(")) {
        /*
         * `struct Tag name(args)` — a function whose return type is an
         * aggregate; rewind and parse it as a signature.
         */
        p->pos = before;
      } else {
        if (!declare_tag(p, tag_name, is_union))
          return false;
        if (!expect(p, ";")) {
          p->pos = before;
          fail(p, "unsupported file-scope construct after struct/union "
                  "declaration");
          return false;
        }
        continue;
      }
    }
    c_function_t candidate = {0};
    if (!parse_signature(p, &candidate))
      return false;
    if (p->function_count >= p->function_capacity) {
      free(candidate.params);
      fail(p, "C frontend function-table capacity exceeded");
      return false;
    }
    c_function_t *fn = find_function(p, candidate.name);
    bool inserted = false;
    if (!fn) {
      fn = &p->functions[p->function_count++];
      *fn = candidate;
      candidate.params = NULL;
      inserted = true;
    } else {
      bool conflict = fn->result != candidate.result ||
                      fn->result_agg != candidate.result_agg ||
                      fn->param_count != candidate.param_count;
      for (size_t i = 0; !conflict && i < fn->param_count; ++i)
        conflict = fn->params[i].type != candidate.params[i].type ||
                   fn->params[i].agg != candidate.params[i].agg;
      if (conflict) {
        free(candidate.params);
        fail(p, "conflicting function declarations");
        return false;
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

static bool emit(c_parser_t *p, nyr_inst_t inst, int *value) {
  char local_error[256] = {0};
  if (!nyr_emit(p->out, inst, value, local_error, sizeof(local_error))) {
    if (p->err && p->err_len && !p->err[0])
      snprintf(p->err, p->err_len, "%s", local_error);
    return false;
  }
  return true;
}

static int emit_value(c_parser_t *p, nyr_inst_t inst) {
  int value = -1;
  return emit(p, inst, &value) ? value : -1;
}

static int lower_pointer_offset(c_parser_t *p, int base, int index,
                                bool subtract) {
  int scale =
      emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = 8});
  if (scale < 0)
    return -1;
  int offset = emit_value(
      p, (nyr_inst_t){.op = NYIR_MUL_I64, .dst = -1, .a = index, .b = scale});
  if (offset < 0)
    return -1;
  return emit_value(p,
                    (nyr_inst_t){.op = subtract ? NYIR_SUB_I64 : NYIR_ADD_I64,
                                  .dst = -1,
                                  .a = base,
                                  .b = offset});
}

static int lower_pointer_index_address(c_parser_t *p, int base, int index) {
  return lower_pointer_offset(p, base, index, false);
}

/*
 * Allocate `bytes` (8-byte aligned) of callee stack storage.  Each
 * allocation lowers to its own NYIR_ALLOCA; the result address is the
 * aggregate's base.
 */
static bool agg_area_alloc(c_parser_t *p, size_t bytes, size_t *offset_out) {
  size_t padded = align_up(bytes, 8);
  if (padded < bytes || padded > (size_t)INT32_MAX) {
    fail(p, "aggregate storage area exceeds the frontend limit");
    return false;
  }
  int base = emit_value(p, (nyr_inst_t){.op = NYIR_ALLOCA,
                                         .dst = -1,
                                         .imm = (int64_t)padded,
                                         .scalar_class = NYIR_SCALAR_I64_RAW});
  if (base < 0) {
    if (p->err && p->err_len && !p->err[0])
      fail(p, "aggregate storage allocation failed");
    return false;
  }
  p->agg_base = base;
  *offset_out = 0;
  return true;
}

static int lower_add_const(c_parser_t *p, int base, size_t offset) {
  if (offset == 0)
    return base;
  int constant = emit_value(
      p,
      (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = (int64_t)offset});
  if (constant < 0)
    return -1;
  return emit_value(
      p,
      (nyr_inst_t){.op = NYIR_ADD_I64, .dst = -1, .a = base, .b = constant});
}

static int lower_load_field(c_parser_t *p, int address, bool is_f64) {
  return emit_value(
      p, (nyr_inst_t){
             .op = NYIR_LOAD_I64,
             .dst = -1,
             .a = address,
             .effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP,
             .scalar_class = is_f64 ? NYIR_SCALAR_F64 : NYIR_SCALAR_I64_RAW});
}

static bool lower_store_field(c_parser_t *p, int address, int value,
                              bool is_f64) {
  return emit(
      p,
      (nyr_inst_t){.op = NYIR_STORE_I64,
                    .dst = -1,
                    .a = address,
                    .c = value,
                    .effects = NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP,
                    .scalar_class =
                        is_f64 ? NYIR_SCALAR_F64 : NYIR_SCALAR_I64_RAW},
      NULL);
}

static bool lower_copy_struct(c_parser_t *p, int dst, int src, size_t size) {
  return emit(p,
              (nyr_inst_t){.op = NYIR_COPY_STRUCT,
                            .dst = -1,
                            .a = dst,
                            .b = src,
                            .imm = (int64_t)size,
                            .effects = NYIR_EFFECT_READ_MEMORY |
                                       NYIR_EFFECT_WRITE_MEMORY},
              NULL);
}

static bool lower_zero_fill(c_parser_t *p, int address, size_t size) {
  int zero = emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                         .dst = -1,
                                         .imm = 0,
                                         .scalar_class = NYIR_SCALAR_I64_RAW});
  if (zero < 0)
    return false;
  size_t words = align_up(size, 8) / 8;
  for (size_t word = 0; word < words; ++word) {
    int target = word ? lower_add_const(p, address, word * 8) : address;
    if (target < 0 || !lower_store_field(p, target, zero, false))
      return false;
  }
  return true;
}

static const c_agg_field_t *find_field(const c_agg_type_t *agg,
                                       ny_ctok_t name) {
  if (!agg)
    return NULL;
  for (size_t i = 0; i < agg->field_count; ++i)
    if (tok_same(agg->fields[i].name, name))
      return &agg->fields[i];
  return NULL;
}

/*
 * Address of `name.field...` starting from an aggregate pointer. Produces
 * the field address for aggregate fields and loads scalar fields.
 */
static bool lower_member_rvalue(c_parser_t *p, c_value_t base, c_value_t *out) {
  c_value_t value = base;
  for (;;) {
    bool arrow = accept(p, "->");
    bool dot = !arrow && accept(p, ".");
    if (!dot && !arrow)
      break;
    if (dot && value.type != C_AGG) {
      fail(p, "'.' member access requires a struct or union value");
      return false;
    }
    if (arrow && value.type != C_AGG_PTR) {
      fail(p, "'->' member access requires a pointer to struct or union");
      return false;
    }
    ny_ctok_t field_name;
    if (!parse_name(p, &field_name))
      return false;
    const c_agg_type_t *agg = value.agg;
    if (!agg || !agg->complete) {
      fail(p, "member access requires a complete struct or union");
      return false;
    }
    const c_agg_field_t *field = find_field(agg, field_name);
    if (!field) {
      fail(p, "struct/union has no such member");
      return false;
    }
    int address = value.value;
    if (field->offset)
      address = lower_add_const(p, address, field->offset);
    if (address < 0)
      return false;
    if (field->kind == C_AGG_FIELD_AGG) {
      value = (c_value_t){address, C_AGG, field->agg};
      continue;
    }
    if (field->kind == C_AGG_FIELD_ARRAY) {
      /*
       * Arrays decay to a pointer to their first element.
       */
      if (field->agg)
        value = (c_value_t){address, C_AGG, field->agg};
      else if (field->elem_f64)
        value = (c_value_t){address, C_F64_PTR, NULL};
      else
        value = (c_value_t){address, C_I64_PTR, NULL};
      continue;
    }
    bool is_f64 = field->kind == C_AGG_FIELD_F64;
    int loaded = lower_load_field(p, address, is_f64);
    if (loaded < 0)
      return false;
    value = (c_value_t){loaded, is_f64 ? C_F64 : C_I64, NULL};
  }
  *out = value;
  return true;
}

static c_local_t *find_local(c_parser_t *p, ny_ctok_t name) {
  for (size_t i = p->local_count; i > 0; --i)
    if (tok_same(p->locals[i - 1].name, name))
      return &p->locals[i - 1];
  return NULL;
}

static int precedence(c_parser_t *p) {
  if (tok_is(p, p->pos, "||"))
    return 1;
  if (tok_is(p, p->pos, "&&"))
    return 2;
  if (tok_is(p, p->pos, "|"))
    return 3;
  if (tok_is(p, p->pos, "^"))
    return 4;
  if (tok_is(p, p->pos, "&"))
    return 5;
  if (tok_is(p, p->pos, "==") || tok_is(p, p->pos, "!="))
    return 6;
  if (tok_is(p, p->pos, "<") || tok_is(p, p->pos, "<=") ||
      tok_is(p, p->pos, ">") || tok_is(p, p->pos, ">="))
    return 7;
  if (tok_is(p, p->pos, "<<") || tok_is(p, p->pos, ">>"))
    return 8;
  if (tok_is(p, p->pos, "+") || tok_is(p, p->pos, "-"))
    return 9;
  if (tok_is(p, p->pos, "*") || tok_is(p, p->pos, "/") ||
      tok_is(p, p->pos, "%"))
    return 10;
  return 0;
}

static bool is_assignment_operator(const c_parser_t *p, size_t pos) {
  static const char *const operators[] = {
      "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=",
  };
  for (size_t i = 0; i < sizeof(operators) / sizeof(operators[0]); ++i)
    if (tok_is(p, pos, operators[i]))
      return true;
  return false;
}

static nyr_op_t compound_assignment_ir(c_type_t type, ny_ctok_t op) {
  if (type != C_I64 && type != C_F64)
    return NYIR_NOP;
  if (ny_ctok_eq(op, "+="))
    return type == C_F64 ? NYIR_ADD_F64 : NYIR_ADD_I64;
  if (ny_ctok_eq(op, "-="))
    return type == C_F64 ? NYIR_SUB_F64 : NYIR_SUB_I64;
  if (ny_ctok_eq(op, "*="))
    return type == C_F64 ? NYIR_MUL_F64 : NYIR_MUL_I64;
  if (ny_ctok_eq(op, "/="))
    return type == C_F64 ? NYIR_DIV_F64 : NYIR_DIV_I64;
  if (type == C_I64 && ny_ctok_eq(op, "%="))
    return NYIR_MOD_I64;
  if (type == C_I64 && ny_ctok_eq(op, "&="))
    return NYIR_AND_I64;
  if (type == C_I64 && ny_ctok_eq(op, "|="))
    return NYIR_OR_I64;
  if (type == C_I64 && ny_ctok_eq(op, "^="))
    return NYIR_XOR_I64;
  if (type == C_I64 && ny_ctok_eq(op, "<<="))
    return NYIR_SHL_I64;
  if (type == C_I64 && ny_ctok_eq(op, ">>="))
    return NYIR_SAR_I64;
  return NYIR_NOP;
}

static c_value_t parse_expr(c_parser_t *p, int min_precedence);
static int lower_truth_value(c_parser_t *p, c_value_t condition);
static int lower_boolean_value(c_parser_t *p, c_value_t condition);
static int lower_increment(c_parser_t *p, c_local_t *local, bool decrement,
                           bool prefix, int previous_value);

static c_value_t parse_call(c_parser_t *p, ny_ctok_t name) {
  c_function_t *fn = find_function(p, name);
  if (!fn) {
    fail(p, "call target must be a declared function in this translation unit");
    return (c_value_t){-1, C_BAD, NULL};
  }
  if (fn->param_count > NYIR_CALL_MAX_ARGS) {
    fail(p, "direct call exceeds the NYIR limit of 64 arguments");
    return (c_value_t){-1, C_BAD, NULL};
  }
  bool has_agg_result = fn->result == C_AGG;
  uint32_t result_packed = 0;
  bool sret = false;
  if (has_agg_result) {
    if (!fn->result_agg || !fn->result_agg->complete) {
      fail(p, "call result type must be a complete struct or union");
      return (c_value_t){-1, C_BAD, NULL};
    }
    if (!nyc_classify_aggregate(fn->result_agg, p->abi, 0, &result_packed)) {
      fail(p, "aggregate return type is not supported by the C ABI");
      return (c_value_t){-1, C_BAD, NULL};
    }
    sret = nyc_sysv_return_is_memory(result_packed);
  }
  bool has_agg_param = false;
  for (size_t i = 0; i < fn->param_count; ++i)
    if (fn->params[i].type == C_AGG)
      has_agg_param = true;
  size_t argc = fn->param_count + (sret ? 1u : 0u);
  if (argc > NYIR_CALL_MAX_ARGS) {
    fail(p, "direct call exceeds the NYIR limit of 64 arguments");
    return (c_value_t){-1, C_BAD, NULL};
  }
  nyr_inst_t call = {.op = NYIR_CALL,
                      .dst = -1,
                      .a = -1,
                      .b = -1,
                      .c = -1,
                      .d = -1,
                      .e = -1,
                      .f = -1,
                      .imm = (int64_t)argc,
                      .flags =
                          NYIR_INST_F_EXTERN | (sret ? NYIR_INST_F_SRET : 0u),
                      .effects = NYIR_EFFECT_CALL,
                      .symbol = strndup(name.start, name.len)};
  if (!call.symbol) {
    fail(p, "out of memory creating call symbol");
    return (c_value_t){-1, C_BAD, NULL};
  }
  if (argc > 6) {
    call.extra_args_len = argc - 6;
    call.extra_args = malloc(call.extra_args_len * sizeof(*call.extra_args));
    if (!call.extra_args) {
      fail(p, "out of memory allocating direct call arguments");
      free((void *)call.symbol);
      return (c_value_t){-1, C_BAD, NULL};
    }
  }
  if (has_agg_param || sret) {
    call.arg_sizes = calloc(argc, sizeof(*call.arg_sizes));
    if (!call.arg_sizes) {
      fail(p, "out of memory allocating aggregate call metadata");
      free((void *)call.symbol);
      free(call.extra_args);
      return (c_value_t){-1, C_BAD, NULL};
    }
  }
  if (sret) {
    call.scalar_class = NYIR_SCALAR_I64_RAW;
  } else if (has_agg_result) {
    call.scalar_class =
        (NYIR_ARG_AGG_CLASS(result_packed, 0) == NYIR_ARG_CLASS_SSE ||
         NYIR_ARG_AGG_CLASS(result_packed, 0) == NYIR_ARG_CLASS_HFA_F64)
            ? NYIR_SCALAR_F64
            : NYIR_SCALAR_I64_RAW;
    call.ret_aggregate_size = NYIR_ARG_AGG_SIZE(result_packed);
    call.ret_aggregate_classes[0] =
        (uint8_t)NYIR_ARG_AGG_CLASS(result_packed, 0);
    call.ret_aggregate_classes[1] =
        (uint8_t)NYIR_ARG_AGG_CLASS(result_packed, 1);
  } else {
    call.scalar_class = fn->result == C_F64   ? NYIR_SCALAR_F64
                        : fn->result == C_BAD ? NYIR_SCALAR_UNKNOWN
                                              : NYIR_SCALAR_I64_RAW;
  }
  int result_addr = -1;
  if (sret) {
    /*
     * MEMORY-class result: allocate the caller-side return buffer and pass
     * its address as the hidden first argument.
     */
    size_t offset = 0;
    if (!agg_area_alloc(p, fn->result_agg->size, &offset)) {
      free((void *)call.symbol);
      free(call.extra_args);
      free(call.arg_sizes);
      return (c_value_t){-1, C_BAD, NULL};
    }
    result_addr = lower_add_const(p, p->agg_base, offset);
    if (result_addr < 0) {
      free((void *)call.symbol);
      free(call.extra_args);
      free(call.arg_sizes);
      return (c_value_t){-1, C_BAD, NULL};
    }
    int *args[] = {&call.a, &call.b, &call.c, &call.d, &call.e, &call.f};
    *args[0] = result_addr;
    nyr_call_arg_class_set(call.arg_scalar_classes, 0, NYIR_SCALAR_I64_RAW);
  }
  size_t argument = sret ? 1 : 0;
  size_t logical = 0;
  if (!accept(p, ")")) {
    for (;;) {
      if (logical >= fn->param_count) {
        fail(p, "direct call argument count does not match its prototype");
        goto call_error;
      }
      c_value_t value = parse_expr(p, 1);
      c_param_t *param = &fn->params[logical];
      int *args[] = {&call.a, &call.b, &call.c, &call.d, &call.e, &call.f};
      if (param->type == C_AGG) {
        if (value.value < 0 || value.type != C_AGG || value.agg != param->agg) {
          fail(p, "direct calls require matching aggregate argument types");
          goto call_error;
        }
        uint32_t packed = 0;
        if (!nyc_classify_aggregate(param->agg, p->abi, 0, &packed)) {
          fail(p, "aggregate argument type is not supported by the C ABI");
          goto call_error;
        }
        call.arg_sizes[argument] = packed;
        nyr_call_arg_class_set(call.arg_scalar_classes, argument,
                                NYIR_SCALAR_AGGREGATE);
      } else {
        if (value.value < 0 || value.type != param->type) {
          fail(p, "direct calls require matching scalar argument types");
          goto call_error;
        }
        nyr_call_arg_class_set(call.arg_scalar_classes, argument,
                                value.type == C_F64 ? NYIR_SCALAR_F64
                                                    : NYIR_SCALAR_I64_RAW);
      }
      if (argument < sizeof(args) / sizeof(args[0]))
        *args[argument] = value.value;
      else
        call.extra_args[argument - sizeof(args) / sizeof(args[0])] =
            value.value;
      ++argument;
      ++logical;
      if (accept(p, ")"))
        break;
      if (!expect(p, ","))
        break;
    }
  }
  if (logical != fn->param_count) {
    fail(p, "direct call argument count does not match its prototype");
    goto call_error;
  }
  int result_id = -1;
  bool emitted = !sret && !has_agg_result && fn->result == C_BAD
                     ? emit(p, call, NULL)
                     : (result_id = emit_value(p, call)) >= 0;
  if (!emitted) {
    free(call.extra_args);
    free(call.arg_sizes);
    free((void *)call.symbol);
    return (c_value_t){-1, C_BAD, NULL};
  }
  /*
   * On success the call instruction owns extra_args and arg_sizes; only
   * the symbol needs freeing here because nyr_ir_emit copies it.
   */
  free((void *)call.symbol);
  if (has_agg_result && !sret) {
    /*
     * Capture every secondary return register before emitting any stores or
     * address arithmetic: CAPTURE_RET instructions must form an immediate
     * chain directly after the call.
     */
    if (result_id < 0) {
      return (c_value_t){-1, C_BAD, NULL};
    }
    int returned[4] = {result_id, -1, -1, -1};
    unsigned result_class0 = NYIR_ARG_AGG_CLASS(result_packed, 0);
    unsigned class1 = NYIR_ARG_AGG_CLASS(result_packed, 1);
    if (result_class0 == NYIR_ARG_CLASS_HFA_F64) {
      unsigned count = NYIR_ARG_AGG_SIZE(result_packed) / 8u;
      if (count == 0 || count > 4)
        return (c_value_t){-1, C_BAD, NULL};
      static const int selectors[4] = {2, 3, 8, 9};
      for (unsigned piece = 0; piece < count; ++piece) {
        returned[piece] = emit_value(p, (nyr_inst_t){.op = NYIR_CAPTURE_RET,
                                                      .dst = -1,
                                                      .a = -1,
                                                      .b = -1,
                                                      .imm = selectors[piece]});
        if (returned[piece] < 0)
          return (c_value_t){-1, C_BAD, NULL};
      }
    } else {
      if (result_class0 == NYIR_ARG_CLASS_SSE) {
        returned[0] = emit_value(
            p,
            (nyr_inst_t){
                .op = NYIR_CAPTURE_RET, .dst = -1, .a = -1, .b = -1, .imm = 2});
        if (returned[0] < 0)
          return (c_value_t){-1, C_BAD, NULL};
      }
      if (class1 == NYIR_ARG_CLASS_INTEGER || class1 == NYIR_ARG_CLASS_SSE) {
        returned[1] = emit_value(
            p, (nyr_inst_t){.op = NYIR_CAPTURE_RET,
                             .dst = -1,
                             .a = -1,
                             .b = -1,
                             .imm = class1 == NYIR_ARG_CLASS_INTEGER ? 0 : 3});
        if (returned[1] < 0)
          return (c_value_t){-1, C_BAD, NULL};
      }
    }
    size_t offset = 0;
    if (!agg_area_alloc(p, fn->result_agg->size, &offset)) {
      return (c_value_t){-1, C_BAD, NULL};
    }
    int address = lower_add_const(p, p->agg_base, offset);
    if (address < 0)
      return (c_value_t){-1, C_BAD, NULL};
    unsigned pieces = (NYIR_ARG_AGG_SIZE(result_packed) + 7u) / 8u;
    if (pieces == 0 || pieces > 4)
      return (c_value_t){-1, C_BAD, NULL};
    for (unsigned piece = 0; piece < pieces; ++piece) {
      int at =
          piece ? lower_add_const(p, address, (size_t)piece * 8u) : address;
      if (at < 0 || returned[piece] < 0 ||
          !lower_store_field(p, at, returned[piece], false))
        return (c_value_t){-1, C_BAD, NULL};
    }
    return (c_value_t){address, C_AGG, fn->result_agg};
  }
  if (sret)
    return (c_value_t){result_id, C_AGG, fn->result_agg};
  return (c_value_t){result_id, fn->result, NULL};

call_error:
  free((void *)call.symbol);
  free(call.extra_args);
  free(call.arg_sizes);
  return (c_value_t){-1, C_BAD, NULL};
}

static c_value_t parse_primary(c_parser_t *p) {
  p->last_value_const = false;
  ny_ctok_t tok = *current(p);
  if (accept(p, "(")) {
    if (tok_is(p, p->pos, "long") || tok_is(p, p->pos, "double")) {
      fail(p, "unsupported cast or implicit conversion");
      return (c_value_t){-1, C_BAD, NULL};
    }
    c_value_t value = parse_expr(p, 1);
    (void)expect(p, ")");
    if (value.value >= 0 && (value.type == C_AGG || value.type == C_AGG_PTR)) {
      c_value_t member;
      if (!lower_member_rvalue(p, value, &member))
        return (c_value_t){-1, C_BAD, NULL};
      value = member;
    }
    return value;
  }
  if (tok.kind == NY_CTOK_NUMBER) {
    ++p->pos;
    char buffer[128];
    if (tok.len >= sizeof(buffer)) {
      fail(p, "numeric literal exceeds frontend limit");
      return (c_value_t){-1, C_BAD, NULL};
    }
    memcpy(buffer, tok.start, tok.len);
    buffer[tok.len] = '\0';
    bool floating = strpbrk(buffer, ".eEpP") != NULL;
    if (floating) {
      char *end = NULL;
      double number = strtod(buffer, &end);
      if (!end || end == buffer || *end) {
        fail(p, "invalid double literal");
        return (c_value_t){-1, C_BAD, NULL};
      }
      int64_t bits = 0;
      memcpy(&bits, &number, sizeof(bits));
      return (c_value_t){
          emit_value(p, (nyr_inst_t){.op = NYIR_CONST_F64,
                                      .dst = -1,
                                      .imm = bits,
                                      .scalar_class = NYIR_SCALAR_F64}),
          C_F64, NULL};
    }
    size_t end = tok.len;
    while (end && (buffer[end - 1] == 'u' || buffer[end - 1] == 'U' ||
                   buffer[end - 1] == 'l' || buffer[end - 1] == 'L'))
      buffer[--end] = '\0';
    char *endptr = NULL;
    errno = 0;
    long long number = strtoll(buffer, &endptr, 0);
    if (!endptr || endptr == buffer || *endptr || errno == ERANGE) {
      fail(p, "invalid integer literal");
      return (c_value_t){-1, C_BAD, NULL};
    }
    return (c_value_t){emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                                   .dst = -1,
                                                   .imm = (int64_t)number}),
                       C_I64, NULL};
  }
  if (tok_ident(tok)) {
    ++p->pos;
    if (accept(p, "(")) {
      c_value_t call_result = parse_call(p, tok);
      if (call_result.value >= 0 &&
          (call_result.type == C_AGG || call_result.type == C_AGG_PTR)) {
        c_value_t member;
        if (!lower_member_rvalue(p, call_result, &member))
          return (c_value_t){-1, C_BAD, NULL};
        call_result = member;
      }
      return call_result;
    }
    c_local_t *local = find_local(p, tok);
    if (!local) {
      fail(p, "references must name a function parameter or declared local");
      return (c_value_t){-1, C_BAD, NULL};
    }
    int value =
        emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                    .dst = -1,
                                    .imm = (int64_t)local->slot,
                                    .scalar_class = local->type == C_F64
                                                        ? NYIR_SCALAR_F64
                                                        : NYIR_SCALAR_I64_RAW});
    p->last_value_const = local->is_const;
    if (value < 0)
      return (c_value_t){-1, C_BAD, NULL};
    if (local->type == C_AGG && local->array_count > 0) {
      /*
       * Array of structs: index to a pointer to the selected element.
       */
      if (!accept(p, "["))
        return (c_value_t){value, C_AGG, local->agg};
      c_value_t index = parse_expr(p, 1);
      if (index.value < 0 || index.type != C_I64 || !expect(p, "]")) {
        fail(p, "array index must be a signed 64-bit integer");
        return (c_value_t){-1, C_BAD, NULL};
      }
      int scale =
          emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                      .dst = -1,
                                      .imm = (int64_t)local->agg->size});
      if (scale < 0)
        return (c_value_t){-1, C_BAD, NULL};
      int offset = emit_value(
          p, (nyr_inst_t){
                 .op = NYIR_MUL_I64, .dst = -1, .a = index.value, .b = scale});
      if (offset < 0)
        return (c_value_t){-1, C_BAD, NULL};
      int address = emit_value(
          p, (nyr_inst_t){
                 .op = NYIR_ADD_I64, .dst = -1, .a = value, .b = offset});
      if (address < 0)
        return (c_value_t){-1, C_BAD, NULL};
      c_value_t element = {address, C_AGG, local->agg};
      c_value_t member;
      if (!lower_member_rvalue(p, element, &member))
        return (c_value_t){-1, C_BAD, NULL};
      return member;
    }
    if (accept(p, "[")) {
      if (local->type != C_I64_PTR && local->type != C_F64_PTR) {
        fail(p, "array indexing requires a pointer to signed 64-bit integer or "
                "double");
        return (c_value_t){-1, C_BAD, NULL};
      }
      c_value_t index = parse_expr(p, 1);
      if (index.value < 0 || index.type != C_I64 || !expect(p, "]")) {
        fail(p, "array index must be a signed 64-bit integer");
        return (c_value_t){-1, C_BAD, NULL};
      }
      int address = lower_pointer_index_address(p, value, index.value);
      if (address < 0)
        return (c_value_t){-1, C_BAD, NULL};
      p->last_value_const = local->is_const;
      c_type_t element_type = local->type == C_F64_PTR ? C_F64 : C_I64;
      return (c_value_t){
          emit_value(p,
                     (nyr_inst_t){.op = NYIR_LOAD_I64,
                                   .dst = -1,
                                   .a = address,
                                   .effects = NYIR_EFFECT_READ_MEMORY |
                                              NYIR_EFFECT_MAY_TRAP,
                                   .scalar_class = element_type == C_F64
                                                       ? NYIR_SCALAR_F64
                                                       : NYIR_SCALAR_I64_RAW}),
          element_type, NULL};
    }
    if (accept(p, "++") || accept(p, "--")) {
      ny_ctok_t op = p->tokens[p->pos - 1];
      bool decrement = ny_ctok_eq(op, "--");
      p->last_value_const = false;
      return (c_value_t){lower_increment(p, local, decrement, false, value),
                         local->type, NULL};
    }
    if (local->type == C_AGG || local->type == C_AGG_PTR) {
      c_value_t base = {value, local->type, local->agg};
      c_value_t member;
      if (!lower_member_rvalue(p, base, &member))
        return (c_value_t){-1, C_BAD, NULL};
      return member;
    }
    return (c_value_t){value, local->type, NULL};
  }
  fail(p, "expected scalar expression");
  return (c_value_t){-1, C_BAD, NULL};
}

static c_value_t parse_unary(c_parser_t *p) {
  if (accept(p, "++") || accept(p, "--")) {
    bool decrement = ny_ctok_eq(p->tokens[p->pos - 1], "--");
    ny_ctok_t name;
    if (!parse_name(p, &name))
      return (c_value_t){-1, C_BAD, NULL};
    c_local_t *local = find_local(p, name);
    int value = lower_increment(p, local, decrement, true, -1);
    p->last_value_const = false;
    return (c_value_t){value, local ? local->type : C_BAD, NULL};
  }
  if (accept(p, "*")) {
    c_value_t address = parse_unary(p);
    if (address.type == C_AGG_PTR) {
      /*
       * Dereferencing a struct pointer yields the struct itself (still
       * addressed in place).
       */
      c_value_t pointee = {address.value, C_AGG, address.agg};
      c_value_t member;
      if (!lower_member_rvalue(p, pointee, &member))
        return (c_value_t){-1, C_BAD, NULL};
      return member;
    }
    if (address.type != C_I64_PTR && address.type != C_F64_PTR) {
      fail(p,
           "dereference requires a pointer to signed 64-bit integer or double");
      return (c_value_t){-1, C_BAD, NULL};
    }
    c_type_t element_type = address.type == C_F64_PTR ? C_F64 : C_I64;
    int value = emit_value(
        p, (nyr_inst_t){
               .op = NYIR_LOAD_I64,
               .dst = -1,
               .a = address.value,
               .imm = 0,
               .effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP,
               .scalar_class = element_type == C_F64 ? NYIR_SCALAR_F64
                                                     : NYIR_SCALAR_I64_RAW});
    return (c_value_t){value, element_type, NULL};
  }
  if (accept(p, "+"))
    return parse_unary(p);
  if (accept(p, "-") || accept(p, "!") || accept(p, "~")) {
    ny_ctok_t op = p->tokens[p->pos - 1];
    c_value_t value = parse_unary(p);
    if (value.value < 0)
      return value;
    if (ny_ctok_eq(op, "!")) {
      int zero = emit_value(
          p, (nyr_inst_t){
                 .op = value.type == C_F64 ? NYIR_CONST_F64 : NYIR_CONST_I64,
                 .dst = -1,
                 .scalar_class = value.type == C_F64 ? NYIR_SCALAR_F64
                                                     : NYIR_SCALAR_I64_RAW});
      int result =
          emit_value(p, (nyr_inst_t){.op = value.type == C_F64 ? NYIR_CMP_F64
                                                                : NYIR_CMP_I64,
                                      .dst = -1,
                                      .a = value.value,
                                      .b = zero,
                                      .cmp = NYIR_CMP_EQ});
      return (c_value_t){result, C_I64, NULL};
    }
    if (ny_ctok_eq(op, "~")) {
      if (value.type != C_I64) {
        fail(p, "bitwise complement requires signed 64-bit integer");
        return (c_value_t){-1, C_BAD, NULL};
      }
      int all = emit_value(
          p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = -1});
      return (c_value_t){emit_value(p, (nyr_inst_t){.op = NYIR_XOR_I64,
                                                     .dst = -1,
                                                     .a = value.value,
                                                     .b = all}),
                         C_I64, NULL};
    }
    if (c_type_is_pointer(value.type)) {
      fail(p, "unary minus requires a signed integer or double");
      return (c_value_t){-1, C_BAD, NULL};
    }
    int zero = emit_value(
        p, (nyr_inst_t){
               .op = value.type == C_F64 ? NYIR_CONST_F64 : NYIR_CONST_I64,
               .dst = -1,
               .scalar_class = value.type == C_F64 ? NYIR_SCALAR_F64
                                                   : NYIR_SCALAR_I64_RAW});
    return (c_value_t){
        emit_value(p, (nyr_inst_t){.op = value.type == C_F64 ? NYIR_SUB_F64
                                                              : NYIR_SUB_I64,
                                    .dst = -1,
                                    .a = zero,
                                    .b = value.value}),
        value.type, NULL};
  }
  return parse_primary(p);
}

static c_value_t parse_expr(c_parser_t *p, int min_precedence) {
  c_value_t left = parse_unary(p);
  for (;;) {
    int prec = precedence(p);
    if (prec < min_precedence) {
      if (min_precedence != 1 || !accept(p, "?"))
        return left;
      if (left.value < 0 || left.type == C_BAD || p->next_local == SIZE_MAX ||
          p->next_label > INT_MAX - 3) {
        fail(p, "conditional expression requires scalar values and storage");
        return (c_value_t){-1, C_BAD, NULL};
      }
      int condition = lower_truth_value(p, left);
      if (condition < 0) {
        fail(p, "conditional expression condition is invalid");
        return (c_value_t){-1, C_BAD, NULL};
      }
      size_t result_slot = p->next_local++;
      size_t true_label = (size_t)p->next_label++;
      size_t false_label = (size_t)p->next_label++;
      size_t merge_label = (size_t)p->next_label++;
      if (!emit(p,
                (nyr_inst_t){.op = NYIR_BR_IF,
                              .dst = -1,
                              .a = condition,
                              .imm = (int64_t)true_label},
                NULL) ||
          !emit(p,
                (nyr_inst_t){
                    .op = NYIR_BR, .dst = -1, .imm = (int64_t)false_label},
                NULL) ||
          !emit(p,
                (nyr_inst_t){
                    .op = NYIR_LABEL, .dst = -1, .imm = (int64_t)true_label},
                NULL))
        return (c_value_t){-1, C_BAD, NULL};
      c_value_t true_value = parse_expr(p, 1);
      if (true_value.value < 0 || true_value.type == C_BAD || !expect(p, ":")) {
        fail(p, "conditional expression requires supported scalar arms");
        return (c_value_t){-1, C_BAD, NULL};
      }
      nyr_scalar_class_t result_class =
          true_value.type == C_F64 ? NYIR_SCALAR_F64 : NYIR_SCALAR_I64_RAW;
      if (!emit(p,
                (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                              .dst = -1,
                              .a = true_value.value,
                              .imm = (int64_t)result_slot,
                              .scalar_class = result_class},
                NULL) ||
          !emit(p,
                (nyr_inst_t){
                    .op = NYIR_BR, .dst = -1, .imm = (int64_t)merge_label},
                NULL) ||
          !emit(p,
                (nyr_inst_t){
                    .op = NYIR_LABEL, .dst = -1, .imm = (int64_t)false_label},
                NULL))
        return (c_value_t){-1, C_BAD, NULL};
      c_value_t false_value = parse_expr(p, 1);
      if (false_value.value < 0 || false_value.type == C_BAD ||
          true_value.type != false_value.type) {
        fail(p,
             "conditional expression arms must have the same supported type");
        return (c_value_t){-1, C_BAD, NULL};
      }
      if (!emit(p,
                (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                              .dst = -1,
                              .a = false_value.value,
                              .imm = (int64_t)result_slot,
                              .scalar_class = result_class},
                NULL) ||
          !emit(p,
                (nyr_inst_t){
                    .op = NYIR_BR, .dst = -1, .imm = (int64_t)merge_label},
                NULL) ||
          !emit(p,
                (nyr_inst_t){
                    .op = NYIR_LABEL, .dst = -1, .imm = (int64_t)merge_label},
                NULL))
        return (c_value_t){-1, C_BAD, NULL};
      int result = emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                               .dst = -1,
                                               .imm = (int64_t)result_slot,
                                               .scalar_class = result_class});
      left = (c_value_t){result, true_value.type, NULL};
      continue;
    }
    ny_ctok_t op = *current(p);
    ++p->pos;
    bool logical_and = ny_ctok_eq(op, "&&");
    bool logical_or = ny_ctok_eq(op, "||");
    if (logical_and || logical_or) {
      if (left.value < 0 || left.type == C_BAD) {
        fail(p, "logical operators require scalar operands");
        return (c_value_t){-1, C_BAD, NULL};
      }
      int left_truth = lower_truth_value(p, left);
      if (left_truth < 0 || p->next_local == SIZE_MAX ||
          p->next_label > INT_MAX - 3) {
        fail(p, "logical expression temporary storage exhausted");
        return (c_value_t){-1, C_BAD, NULL};
      }
      size_t result_slot = p->next_local++;
      size_t rhs_label = (size_t)p->next_label++;
      size_t short_label = (size_t)p->next_label++;
      size_t merge_label = (size_t)p->next_label++;
      int short_value = logical_and ? 0 : 1;
      size_t true_target = logical_and ? rhs_label : short_label;
      size_t false_target = logical_and ? short_label : rhs_label;
      if (!emit(p,
                (nyr_inst_t){.op = NYIR_BR_IF,
                              .dst = -1,
                              .a = left_truth,
                              .imm = (int64_t)true_target},
                NULL) ||
          !emit(p,
                (nyr_inst_t){
                    .op = NYIR_BR, .dst = -1, .imm = (int64_t)false_target},
                NULL) ||
          !emit(p,
                (nyr_inst_t){
                    .op = NYIR_LABEL, .dst = -1, .imm = (int64_t)short_label},
                NULL))
        return (c_value_t){-1, C_BAD, NULL};
      int short_constant =
          emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                      .dst = -1,
                                      .imm = short_value,
                                      .scalar_class = NYIR_SCALAR_I64_RAW});
      if (short_constant < 0 ||
          !emit(p,
                (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                              .dst = -1,
                              .a = short_constant,
                              .imm = (int64_t)result_slot,
                              .scalar_class = NYIR_SCALAR_I64_RAW},
                NULL) ||
          !emit(p,
                (nyr_inst_t){
                    .op = NYIR_BR, .dst = -1, .imm = (int64_t)merge_label},
                NULL) ||
          !emit(p,
                (nyr_inst_t){
                    .op = NYIR_LABEL, .dst = -1, .imm = (int64_t)rhs_label},
                NULL))
        return (c_value_t){-1, C_BAD, NULL};
      c_value_t right = parse_expr(p, prec + 1);
      if (right.value < 0 || right.type == C_BAD) {
        fail(p, "logical operators require scalar operands");
        return (c_value_t){-1, C_BAD, NULL};
      }
      int right_truth = lower_boolean_value(p, right);
      if (right_truth < 0 ||
          !emit(p,
                (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                              .dst = -1,
                              .a = right_truth,
                              .imm = (int64_t)result_slot,
                              .scalar_class = NYIR_SCALAR_I64_RAW},
                NULL) ||
          !emit(p,
                (nyr_inst_t){
                    .op = NYIR_BR, .dst = -1, .imm = (int64_t)merge_label},
                NULL) ||
          !emit(p,
                (nyr_inst_t){
                    .op = NYIR_LABEL, .dst = -1, .imm = (int64_t)merge_label},
                NULL))
        return (c_value_t){-1, C_BAD, NULL};
      int result =
          emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                      .dst = -1,
                                      .imm = (int64_t)result_slot,
                                      .scalar_class = NYIR_SCALAR_I64_RAW});
      left = (c_value_t){result, C_I64, NULL};
      continue;
    }
    c_value_t right = parse_expr(p, prec + 1);
    if (left.value < 0 || right.value < 0)
      return (c_value_t){-1, C_BAD, NULL};
    bool left_pointer = left.type == C_I64_PTR || left.type == C_F64_PTR;
    bool right_pointer = right.type == C_I64_PTR || right.type == C_F64_PTR;
    bool left_opaque_pointer = left.type == C_VOID_PTR;
    bool right_opaque_pointer = right.type == C_VOID_PTR;
    if ((left_pointer && right.type == C_I64 &&
         (ny_ctok_eq(op, "+") || ny_ctok_eq(op, "-"))) ||
        (right_pointer && left.type == C_I64 && ny_ctok_eq(op, "+"))) {
      c_value_t pointer = left_pointer ? left : right;
      c_value_t index = left_pointer ? right : left;
      int address = lower_pointer_offset(p, pointer.value, index.value,
                                         left_pointer && ny_ctok_eq(op, "-"));
      if (address < 0)
        return (c_value_t){-1, C_BAD, NULL};
      left = (c_value_t){address, pointer.type, NULL};
      continue;
    }
    if (left_opaque_pointer || right_opaque_pointer) {
      bool equality = ny_ctok_eq(op, "==") || ny_ctok_eq(op, "!=");
      if (!equality || left.type != right.type) {
        fail(p, "opaque void pointers support only same-type equality");
        return (c_value_t){-1, C_BAD, NULL};
      }
      int result = emit_value(p, (nyr_inst_t){.op = NYIR_CMP_I64,
                                               .dst = -1,
                                               .a = left.value,
                                               .b = right.value,
                                               .cmp = ny_ctok_eq(op, "==")
                                                          ? NYIR_CMP_EQ
                                                          : NYIR_CMP_NE});
      left = (c_value_t){result, C_I64, NULL};
      continue;
    }
    if (left.type != right.type) {
      fail(p, "mixed integer/double arithmetic is unsupported");
      return (c_value_t){-1, C_BAD, NULL};
    }
    if (left.type == C_I64_PTR || left.type == C_F64_PTR) {
      fail(p, "pointer arithmetic and pointer comparisons are unsupported");
      return (c_value_t){-1, C_BAD, NULL};
    }
    bool cmp = ny_ctok_eq(op, "==") || ny_ctok_eq(op, "!=") ||
               ny_ctok_eq(op, "<") || ny_ctok_eq(op, "<=") ||
               ny_ctok_eq(op, ">") || ny_ctok_eq(op, ">=");
    nyr_op_t ir_op = NYIR_NOP;
    nyr_cmp_t cmp_op = NYIR_CMP_EQ;
    if (ny_ctok_eq(op, "+"))
      ir_op = left.type == C_F64 ? NYIR_ADD_F64 : NYIR_ADD_I64;
    else if (ny_ctok_eq(op, "-"))
      ir_op = left.type == C_F64 ? NYIR_SUB_F64 : NYIR_SUB_I64;
    else if (ny_ctok_eq(op, "*"))
      ir_op = left.type == C_F64 ? NYIR_MUL_F64 : NYIR_MUL_I64;
    else if (ny_ctok_eq(op, "/"))
      ir_op = left.type == C_F64 ? NYIR_DIV_F64 : NYIR_DIV_I64;
    else if (ny_ctok_eq(op, "%"))
      ir_op = NYIR_MOD_I64;
    else if (ny_ctok_eq(op, "&"))
      ir_op = NYIR_AND_I64;
    else if (ny_ctok_eq(op, "|"))
      ir_op = NYIR_OR_I64;
    else if (ny_ctok_eq(op, "^"))
      ir_op = NYIR_XOR_I64;
    else if ((ny_ctok_eq(op, "<<") || ny_ctok_eq(op, ">>")) &&
             left.type == C_I64)
      ir_op = ny_ctok_eq(op, "<<") ? NYIR_SHL_I64 : NYIR_SAR_I64;
    else if (cmp) {
      ir_op = left.type == C_F64 ? NYIR_CMP_F64 : NYIR_CMP_I64;
      cmp_op = ny_ctok_eq(op, "==")   ? NYIR_CMP_EQ
               : ny_ctok_eq(op, "!=") ? NYIR_CMP_NE
               : ny_ctok_eq(op, "<")  ? NYIR_CMP_LT
               : ny_ctok_eq(op, "<=") ? NYIR_CMP_LE
               : ny_ctok_eq(op, ">")  ? NYIR_CMP_GT
                                      : NYIR_CMP_GE;
    }
    if (ir_op == NYIR_NOP ||
        (left.type == C_F64 && !cmp && ny_ctok_eq(op, "%"))) {
      fail(p, "unsupported binary operator for operand type");
      return (c_value_t){-1, C_BAD, NULL};
    }
    int result = emit_value(p, (nyr_inst_t){.op = ir_op,
                                             .dst = -1,
                                             .a = left.value,
                                             .b = right.value,
                                             .cmp = cmp_op});
    left = (c_value_t){result, cmp ? C_I64 : left.type, NULL};
  }
}

static int lower_increment(c_parser_t *p, c_local_t *local, bool decrement,
                           bool prefix, int previous_value) {
  bool pointer = local && c_type_is_arithmetic_pointer(local->type);
  if (!local || (local->is_const && !pointer)) {
    fail(p, "increment and decrement require a mutable scalar or "
            "pointer local");
    return -1;
  }
  if (local->type == C_VOID_PTR) {
    fail(p, "arithmetic on opaque void pointers is unsupported");
    return -1;
  }
  if (previous_value < 0)
    previous_value =
        emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                    .dst = -1,
                                    .imm = (int64_t)local->slot,
                                    .scalar_class = local->type == C_F64
                                                        ? NYIR_SCALAR_F64
                                                        : NYIR_SCALAR_I64_RAW});
  if (previous_value < 0)
    return -1;
  int one;
  if (local->type == C_F64) {
    double value = 1.0;
    int64_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    one = emit_value(p, (nyr_inst_t){.op = NYIR_CONST_F64,
                                      .dst = -1,
                                      .imm = bits,
                                      .scalar_class = NYIR_SCALAR_F64});
  } else {
    one = emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                      .dst = -1,
                                      .imm = 1,
                                      .scalar_class = NYIR_SCALAR_I64_RAW});
  }
  if (one < 0)
    return -1;
  int updated =
      pointer
          ? lower_pointer_offset(p, previous_value, one, decrement)
          : emit_value(
                p, (nyr_inst_t){
                       .op = local->type == C_F64
                                 ? (decrement ? NYIR_SUB_F64 : NYIR_ADD_F64)
                                 : (decrement ? NYIR_SUB_I64 : NYIR_ADD_I64),
                       .dst = -1,
                       .a = previous_value,
                       .b = one,
                       .scalar_class = local->type == C_F64
                                           ? NYIR_SCALAR_F64
                                           : NYIR_SCALAR_I64_RAW});
  if (updated < 0 ||
      !emit(p,
            (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                          .dst = -1,
                          .a = updated,
                          .imm = (int64_t)local->slot,
                          .scalar_class = local->type == C_F64
                                              ? NYIR_SCALAR_F64
                                              : NYIR_SCALAR_I64_RAW},
            NULL))
    return -1;
  return prefix ? updated : previous_value;
}

static bool add_local(c_parser_t *p, ny_ctok_t name, c_type_t type, int value,
                      bool is_const) {
  for (size_t i = p->local_count; i > p->scope_start; --i) {
    if (tok_same(p->locals[i - 1].name, name)) {
      fail(p, "duplicate declaration in the same C scope");
      return false;
    }
  }
  if (p->local_count >= p->local_capacity || p->next_local == SIZE_MAX) {
    fail(p, "C frontend local-variable storage capacity exceeded");
    return false;
  }
  size_t slot = p->next_local++;
  if (value >= 0 &&
      !emit(p,
            (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                          .dst = -1,
                          .a = value,
                          .imm = (int64_t)slot,
                          .scalar_class = type == C_F64 ? NYIR_SCALAR_F64
                                                        : NYIR_SCALAR_I64_RAW},
            NULL))
    return false;
  p->locals[p->local_count++] = (c_local_t){
      .name = name, .type = type, .slot = slot, .is_const = is_const};
  return true;
}

/*
 * Declare one struct/union local (optionally an array of them). Storage
 * lives in the entry-block aggregate area; uninitialized declarations are
 * zero-filled so reads are deterministic.
 */
static bool lower_aggregate_declaration(c_parser_t *p, bool semicolon) {
  c_type_t type;
  if (!parse_type(p, &type, false))
    return false;
  if (type != C_AGG && type != C_AGG_PTR) {
    fail(p, "aggregate declaration requires a struct or union type");
    return false;
  }
  c_agg_type_t *decl_agg = p->last_type_agg;
  bool is_const = p->last_type_const;
  bool ok = true;
  do {
    ny_ctok_t name;
    if (!parse_name(p, &name)) {
      ok = false;
      break;
    }
    size_t count = 1;
    bool is_array = false;
    if (type == C_AGG && accept(p, "[")) {
      is_array = true;
      if (current(p)->kind != NY_CTOK_NUMBER) {
        fail(p, "array declarations require a constant extent");
        ok = false;
        break;
      }
      ny_ctok_t extent = *current(p);
      ++p->pos;
      long long parsed = strtoll(extent.start, NULL, 10);
      if (parsed <= 0 || (unsigned long long)parsed > 65536) {
        fail(p, "array extent must be between 1 and 65536");
        ok = false;
        break;
      }
      count = (size_t)parsed;
      if (!expect(p, "]")) {
        ok = false;
        break;
      }
    }
    int initial_value = -1;
    if (accept(p, "=")) {
      c_value_t initial = parse_expr(p, 1);
      if (initial.value < 0) {
        ok = false;
        break;
      }
      if (type == C_AGG) {
        if (initial.type != C_AGG || initial.agg != decl_agg) {
          fail(p, "aggregate initializer type must match its declaration");
          ok = false;
          break;
        }
      } else if (initial.type != C_AGG_PTR || initial.agg != decl_agg) {
        fail(p, "aggregate pointer initializer must have the same type");
        ok = false;
        break;
      }
      initial_value = initial.value;
    } else if (is_const && type == C_AGG_PTR) {
      fail(p, "const aggregate pointer declaration requires an initializer");
      ok = false;
      break;
    }
    size_t bytes = type == C_AGG ? decl_agg->size * count : 8;
    size_t offset = 0;
    if (!agg_area_alloc(p, bytes, &offset)) {
      ok = false;
      break;
    }
    int base = lower_add_const(p, p->agg_base, offset);
    if (base < 0) {
      ok = false;
      break;
    }
    if (type == C_AGG && !lower_zero_fill(p, base, bytes)) {
      ok = false;
      break;
    }
    if (initial_value >= 0 && type == C_AGG) {
      for (size_t element = 0; element < count; ++element) {
        int dst =
            element ? lower_add_const(p, base, element * decl_agg->size) : base;
        int src = element ? lower_add_const(p, initial_value,
                                            element * decl_agg->size)
                          : initial_value;
        if (dst < 0 || src < 0 ||
            !lower_copy_struct(p, dst, src, decl_agg->size)) {
          ok = false;
          break;
        }
      }
      if (!ok)
        break;
    }
    if (p->local_count >= p->local_capacity || p->next_local == SIZE_MAX) {
      fail(p, "C frontend local-variable storage capacity exceeded");
      ok = false;
      break;
    }
    size_t slot = p->next_local++;
    if (!emit(p,
              (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                            .dst = -1,
                            .a = type == C_AGG ? base : initial_value,
                            .imm = (int64_t)slot,
                            .scalar_class = NYIR_SCALAR_I64_RAW},
              NULL)) {
      ok = false;
      break;
    }
    p->locals[p->local_count++] =
        (c_local_t){.name = name,
                    .type = type,
                    .agg = decl_agg,
                    .slot = slot,
                    .is_const = is_const,
                    .array_count = is_array ? count : 0};
  } while (accept(p, ","));
  if (!ok)
    return false;
  if (semicolon && !expect(p, ";"))
    return false;
  return true;
}

static bool lower_scalar_assignment(c_parser_t *p, bool semicolon) {
  ny_ctok_t name;
  if (!parse_name(p, &name))
    return false;
  c_local_t *local = find_local(p, name);
  if (!local) {
    fail(p, "assignment target must be a declared scalar local or "
            "parameter");
    return false;
  }
  /*
   * Aggregate targets: `s = other;` copies through NYIR_COPY_STRUCT, and a
   * member path (`s.inner.f = v` / `s.inner = other`) stores through the
   * field address.
   */
  if (local->type == C_AGG) {
    int base =
        emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                    .dst = -1,
                                    .imm = (int64_t)local->slot,
                                    .scalar_class = NYIR_SCALAR_I64_RAW});
    if (base < 0)
      return false;
    c_value_t target = {base, C_AGG, local->agg};
    bool descending = true;
    if (local->array_count > 0 || accept(p, ".")) {
      /*
       * Member (or array-element) path.
       */
      if (local->array_count > 0) {
        if (!expect(p, "[")) {
          fail(p, "array assignment requires an element index");
          return false;
        }
        c_value_t index = parse_expr(p, 1);
        if (index.value < 0 || index.type != C_I64 || !expect(p, "]")) {
          fail(p, "array index must be a signed 64-bit integer");
          return false;
        }
        int scale =
            emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                        .dst = -1,
                                        .imm = (int64_t)local->agg->size});
        int offset = scale < 0 ? -1
                               : emit_value(p, (nyr_inst_t){.op = NYIR_MUL_I64,
                                                             .dst = -1,
                                                             .a = index.value,
                                                             .b = scale});
        int address = offset < 0
                          ? -1
                          : emit_value(p, (nyr_inst_t){.op = NYIR_ADD_I64,
                                                        .dst = -1,
                                                        .a = base,
                                                        .b = offset});
        if (address < 0)
          return false;
        target = (c_value_t){address, C_AGG, local->agg};
        descending = accept(p, ".");
      }
      while (descending) {
        ny_ctok_t field_name;
        if (!parse_name(p, &field_name))
          return false;
        const c_agg_field_t *field = find_field(target.agg, field_name);
        if (!field || !target.agg->complete) {
          fail(p, "unknown struct field in assignment target");
          return false;
        }
        if (target.value < 0)
          return false;
        int address = lower_add_const(p, target.value, field->offset);
        if (address < 0)
          return false;
        if (field->kind == C_AGG_FIELD_AGG ||
            field->kind == C_AGG_FIELD_ARRAY) {
          target = (c_value_t){address, C_AGG, field->agg};
          descending = accept(p, ".");
          continue;
        }
        /*
         * Scalar leaf: store through the field address.
         */
        if (!is_assignment_operator(p, p->pos) ||
            !ny_ctok_eq(*current(p), "=")) {
          fail(p, "only simple assignment is supported for struct fields");
          return false;
        }
        ++p->pos;
        c_value_t value = parse_expr(p, 1);
        bool is_f64 = field->kind == C_AGG_FIELD_F64;
        if (value.value < 0 || value.type != (is_f64 ? C_F64 : C_I64)) {
          if (value.value >= 0)
            fail(p, "field value type must match the field declaration");
          return false;
        }
        if (semicolon && !expect(p, ";"))
          return false;
        return lower_store_field(p, address, value.value, is_f64);
      }
      if (!is_assignment_operator(p, p->pos) || !ny_ctok_eq(*current(p), "=")) {
        fail(p, "only simple assignment is supported for struct targets");
        return false;
      }
      ++p->pos;
      c_value_t value = parse_expr(p, 1);
      if (value.value < 0 || value.type != C_AGG || value.agg != target.agg) {
        if (value.value >= 0)
          fail(p, "aggregate assignment value type must match the target");
        return false;
      }
      if (semicolon && !expect(p, ";"))
        return false;
      return lower_copy_struct(p, target.value, value.value, target.agg->size);
    }
    if (local->is_const) {
      fail(p, "cannot assign to a const C object");
      return false;
    }
    if (!is_assignment_operator(p, p->pos) || !ny_ctok_eq(*current(p), "=")) {
      fail(p, "only simple assignment is supported for struct targets");
      return false;
    }
    ++p->pos;
    c_value_t value = parse_expr(p, 1);
    if (value.value < 0 || value.type != C_AGG || value.agg != local->agg) {
      if (value.value >= 0)
        fail(p, "aggregate assignment value type must match the target");
      return false;
    }
    if (semicolon && !expect(p, ";"))
      return false;
    return lower_copy_struct(p, base, value.value, local->agg->size);
  }
  bool pointer = c_type_is_pointer(local->type);
  if (local->is_const && !pointer) {
    fail(p, "cannot assign to a const C object");
    return false;
  }
  if (!is_assignment_operator(p, p->pos)) {
    (void)expect(p, "=");
    return false;
  }
  ny_ctok_t op = *current(p);
  bool simple = ny_ctok_eq(op, "=");
  ++p->pos;
  bool pointer_offset = c_type_is_arithmetic_pointer(local->type) &&
                        (ny_ctok_eq(op, "+=") || ny_ctok_eq(op, "-="));
  if (!simple && local->type != C_I64 && local->type != C_F64 &&
      !pointer_offset) {
    fail(p, "compound assignment operator is unsupported for this type");
    return false;
  }
  c_value_t value = parse_expr(p, 1);
  c_type_t expected_type = pointer_offset ? C_I64 : local->type;
  if (value.value < 0 || value.type != expected_type) {
    if (value.value >= 0)
      fail(p, "assignment value type must match the scalar target");
    return false;
  }
  if (!simple) {
    int old_value =
        emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                    .dst = -1,
                                    .imm = (int64_t)local->slot,
                                    .scalar_class = local->type == C_F64
                                                        ? NYIR_SCALAR_F64
                                                        : NYIR_SCALAR_I64_RAW});
    nyr_op_t ir_op = compound_assignment_ir(local->type, op);
    if (pointer_offset) {
      if (old_value < 0)
        return false;
      value.value =
          lower_pointer_offset(p, old_value, value.value, ny_ctok_eq(op, "-="));
      value.type = local->type;
    } else if (old_value < 0 || ir_op == NYIR_NOP) {
      fail(p, "compound assignment operator is unsupported for this type");
      return false;
    } else {
      value.value = emit_value(
          p, (nyr_inst_t){.op = ir_op,
                           .dst = -1,
                           .a = old_value,
                           .b = value.value,
                           .scalar_class = local->type == C_F64
                                               ? NYIR_SCALAR_F64
                                               : NYIR_SCALAR_I64_RAW});
    }
    if (value.value < 0)
      return false;
  }
  if (semicolon && !expect(p, ";"))
    return false;
  return emit(p,
              (nyr_inst_t){.op = NYIR_STORE_LOCAL,
                            .dst = -1,
                            .a = value.value,
                            .imm = (int64_t)local->slot,
                            .scalar_class = local->type == C_F64
                                                ? NYIR_SCALAR_F64
                                                : NYIR_SCALAR_I64_RAW},
              NULL);
}

static bool lower_pointer_store(c_parser_t *p, c_value_t address,
                                bool read_only) {
  if (address.type != C_I64_PTR && address.type != C_F64_PTR)
    return false;
  if (!is_assignment_operator(p, p->pos)) {
    (void)expect(p, "=");
    return false;
  }
  ny_ctok_t op = *current(p);
  bool simple = ny_ctok_eq(op, "=");
  ++p->pos;
  c_value_t value = parse_expr(p, 1);
  c_type_t element_type = address.type == C_F64_PTR ? C_F64 : C_I64;
  if (value.type != element_type || value.value < 0) {
    fail(p, element_type == C_F64
                ? "pointer assignment requires a double value"
                : "pointer assignment requires a signed 64-bit integer value");
    return false;
  }
  if (read_only) {
    fail(p, "cannot write through a pointer to const C data");
    return false;
  }
  if (!simple) {
    int old_value = emit_value(
        p, (nyr_inst_t){
               .op = NYIR_LOAD_I64,
               .dst = -1,
               .a = address.value,
               .imm = 0,
               .effects = NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_MAY_TRAP,
               .scalar_class = element_type == C_F64 ? NYIR_SCALAR_F64
                                                     : NYIR_SCALAR_I64_RAW});
    nyr_op_t ir_op = compound_assignment_ir(element_type, op);
    if (old_value < 0 || ir_op == NYIR_NOP) {
      fail(p, "compound pointer assignment operator is unsupported for type");
      return false;
    }
    value.value =
        emit_value(p, (nyr_inst_t){.op = ir_op,
                                    .dst = -1,
                                    .a = old_value,
                                    .b = value.value,
                                    .scalar_class = element_type == C_F64
                                                        ? NYIR_SCALAR_F64
                                                        : NYIR_SCALAR_I64_RAW});
    if (value.value < 0)
      return false;
  }
  if (!expect(p, ";"))
    return false;
  return emit(p,
              (nyr_inst_t){
                  .op = NYIR_STORE_I64,
                  .dst = -1,
                  .a = address.value,
                  .c = value.value,
                  .imm = 0,
                  .effects = NYIR_EFFECT_WRITE_MEMORY | NYIR_EFFECT_MAY_TRAP,
                  .scalar_class = element_type == C_F64 ? NYIR_SCALAR_F64
                                                        : NYIR_SCALAR_I64_RAW},
              NULL);
}

static size_t statement_end(c_parser_t *p, size_t at) {
  if (tok_is(p, at, "{"))
    return skip_balanced(p, at, "{", "}");
  if (tok_is(p, at, "if")) {
    ++at;
    at = skip_balanced(p, at, "(", ")");
    at = statement_end(p, at);
    if (tok_is(p, at, "else"))
      at = statement_end(p, at + 1);
    return at;
  }
  if (tok_is(p, at, "while")) {
    ++at;
    at = skip_balanced(p, at, "(", ")");
    return statement_end(p, at);
  }
  if (tok_is(p, at, "for")) {
    ++at;
    at = skip_balanced(p, at, "(", ")");
    return statement_end(p, at);
  }
  if (tok_is(p, at, "do")) {
    size_t after_body = statement_end(p, at + 1);
    if (!tok_is(p, after_body, "while"))
      return p->token_count;
    ++after_body;
    after_body = skip_balanced(p, after_body, "(", ")");
    return tok_is(p, after_body, ";") ? after_body + 1 : p->token_count;
  }
  while (at < p->token_count && !tok_is(p, at, ";"))
    ++at;
  return at < p->token_count ? at + 1 : at;
}

static bool lower_statement_range(c_parser_t *p, size_t start, size_t end,
                                  bool *returned);

static bool lower_return(c_parser_t *p, bool *returned) {
  if (p->function->result == C_BAD) {
    if (!accept(p, ";")) {
      fail(p, "void function return must not include a value");
      return false;
    }
    if (!emit(p,
              (nyr_inst_t){.op = NYIR_RET,
                            .dst = -1,
                            .a = -1,
                            .b = -1,
                            .scalar_class = NYIR_SCALAR_UNKNOWN},
              NULL))
      return false;
    *returned = true;
    return true;
  }
  c_value_t value = parse_expr(p, 1);
  if (value.value < 0 || !expect(p, ";"))
    return false;
  c_type_t expected = p->function->result;
  if (value.type != expected) {
    fail(p, "return expression type does not match function result");
    return false;
  }
  if (expected == C_AGG) {
    /*
     * MEMORY-class results use the hidden caller buffer; register-class
     * results load their ABI pieces into RET.
     */
    const c_agg_type_t *agg = value.agg ? value.agg : p->function->result_agg;
    uint32_t packed = 0;
    if (!agg || !nyc_classify_aggregate(agg, p->abi, 0, &packed)) {
      fail(p, "aggregate return type is not supported by the C ABI");
      return false;
    }
    if (nyc_sysv_return_is_memory(packed)) {
      int hidden =
          emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                      .dst = -1,
                                      .imm = 0,
                                      .scalar_class = NYIR_SCALAR_I64_RAW});
      if (hidden < 0)
        return false;
      size_t words = align_up(agg->size, 8) / 8;
      for (size_t word = 0; word < words; ++word) {
        int src =
            word ? lower_add_const(p, value.value, word * 8) : value.value;
        int dst = word ? lower_add_const(p, hidden, word * 8) : hidden;
        int piece = lower_load_field(p, src, false);
        if (src < 0 || dst < 0 || piece < 0 ||
            !lower_store_field(p, dst, piece, false))
          return false;
      }
      if (!emit(p,
                (nyr_inst_t){.op = NYIR_RET,
                              .dst = -1,
                              .a = hidden,
                              .b = -1,
                              .scalar_class = NYIR_SCALAR_I64_RAW},
                NULL))
        return false;
      *returned = true;
      return true;
    }
    unsigned class0 = NYIR_ARG_AGG_CLASS(packed, 0);
    if (class0 == NYIR_ARG_CLASS_HFA_F64) {
      if (!emit(p,
                (nyr_inst_t){.op = NYIR_RET,
                              .dst = -1,
                              .a = value.value,
                              .b = -1,
                              .scalar_class = NYIR_SCALAR_I64_RAW},
                NULL))
        return false;
      *returned = true;
      return true;
    }
    int piece0 = lower_load_field(p, value.value, class0 == NYIR_ARG_CLASS_SSE);
    if (piece0 < 0)
      return false;
    int piece1 = -1;
    unsigned class1 = NYIR_ARG_AGG_CLASS(packed, 1);
    if (class1 == NYIR_ARG_CLASS_INTEGER || class1 == NYIR_ARG_CLASS_SSE) {
      int high = lower_add_const(p, value.value, 8);
      if (high < 0)
        return false;
      piece1 = lower_load_field(p, high, class1 == NYIR_ARG_CLASS_SSE);
      if (piece1 < 0)
        return false;
    }
    if (!emit(p,
              (nyr_inst_t){.op = NYIR_RET,
                            .dst = -1,
                            .a = piece0,
                            .b = piece1,
                            .scalar_class = class0 == NYIR_ARG_CLASS_SSE
                                                ? NYIR_SCALAR_F64
                                                : NYIR_SCALAR_I64_RAW},
              NULL))
      return false;
    *returned = true;
    return true;
  }
  if (!emit(p,
            (nyr_inst_t){.op = NYIR_RET,
                          .dst = -1,
                          .a = value.value,
                          .b = -1,
                          .scalar_class = expected == C_F64
                                              ? NYIR_SCALAR_F64
                                              : NYIR_SCALAR_I64_RAW},
            NULL))
    return false;
  *returned = true;
  return true;
}

static int lower_truth_value(c_parser_t *p, c_value_t condition) {
  if (condition.type != C_F64)
    return condition.value;
  int zero = emit_value(p, (nyr_inst_t){.op = NYIR_CONST_F64,
                                         .dst = -1,
                                         .scalar_class = NYIR_SCALAR_F64});
  return emit_value(p, (nyr_inst_t){.op = NYIR_CMP_F64,
                                     .dst = -1,
                                     .a = condition.value,
                                     .b = zero,
                                     .cmp = NYIR_CMP_NE});
}

static int lower_boolean_value(c_parser_t *p, c_value_t value) {
  if (value.type == C_F64)
    return lower_truth_value(p, value);
  int zero = emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64,
                                         .dst = -1,
                                         .scalar_class = NYIR_SCALAR_I64_RAW});
  return emit_value(p, (nyr_inst_t){.op = NYIR_CMP_I64,
                                     .dst = -1,
                                     .a = value.value,
                                     .b = zero,
                                     .cmp = NYIR_CMP_NE});
}

static bool lower_if(c_parser_t *p, size_t limit, bool *returned) {
  ++p->pos;
  if (!expect(p, "("))
    return false;
  c_value_t condition = parse_expr(p, 1);
  if (condition.value < 0 || !expect(p, ")"))
    return false;
  int truth = lower_truth_value(p, condition);
  size_t true_start = p->pos;
  size_t false_start = statement_end(p, true_start);
  if (false_start > limit) {
    fail(p, "if body exceeds function body");
    return false;
  }
  bool has_else = tok_is(p, false_start, "else");
  size_t true_end = false_start;
  size_t true_body = true_start;
  bool true_is_block = tok_is(p, true_body, "{");
  if (true_is_block) {
    ++true_body;
    --true_end;
  }
  int true_label = p->next_label++;
  int false_label = p->next_label++;
  int merge_label = p->next_label++;
  if (!emit(p,
            (nyr_inst_t){
                .op = NYIR_BR_IF, .dst = -1, .a = truth, .imm = true_label},
            NULL))
    return false;
  if (has_else) {
    if (!emit(p, (nyr_inst_t){.op = NYIR_BR, .dst = -1, .imm = false_label},
              NULL))
      return false;
  } else if (!emit(p,
                   (nyr_inst_t){.op = NYIR_BR, .dst = -1, .imm = merge_label},
                   NULL)) {
    return false;
  }
  if (!emit(p, (nyr_inst_t){.op = NYIR_LABEL, .dst = -1, .imm = true_label},
            NULL))
    return false;
  size_t true_local_count = p->local_count;
  size_t true_next_local = p->next_local;
  size_t true_scope_start = p->scope_start;
  if (true_is_block)
    p->scope_start = p->local_count;
  bool true_returns = false;
  if (!lower_statement_range(p, true_body, true_end, &true_returns)) {
    p->scope_start = true_scope_start;
    return false;
  }
  p->scope_start = true_scope_start;
  p->local_count = true_local_count;
  p->next_local = true_next_local;
  if (!has_else) {
    if (!true_returns &&
        !emit(p, (nyr_inst_t){.op = NYIR_BR, .dst = -1, .imm = merge_label},
              NULL))
      return false;
    if (!emit(p, (nyr_inst_t){.op = NYIR_LABEL, .dst = -1, .imm = merge_label},
              NULL))
      return false;
    p->pos = false_start;
    *returned = false;
    return true;
  }

  size_t false_body = false_start + 1;
  size_t false_end = statement_end(p, false_body);
  if (false_end > limit) {
    fail(p, "else body exceeds function body");
    return false;
  }
  size_t after_if = false_end;
  bool false_is_block = tok_is(p, false_body, "{");
  if (false_is_block) {
    ++false_body;
    --false_end;
  }
  if (!emit(p, (nyr_inst_t){.op = NYIR_LABEL, .dst = -1, .imm = false_label},
            NULL))
    return false;
  size_t false_local_count = p->local_count;
  size_t false_next_local = p->next_local;
  size_t false_scope_start = p->scope_start;
  if (false_is_block)
    p->scope_start = p->local_count;
  bool false_returns = false;
  if (!lower_statement_range(p, false_body, false_end, &false_returns)) {
    p->scope_start = false_scope_start;
    return false;
  }
  p->scope_start = false_scope_start;
  p->local_count = false_local_count;
  p->next_local = false_next_local;
  if (!true_returns &&
      !emit(p, (nyr_inst_t){.op = NYIR_BR, .dst = -1, .imm = merge_label},
            NULL))
    return false;
  if (!false_returns &&
      !emit(p, (nyr_inst_t){.op = NYIR_BR, .dst = -1, .imm = merge_label},
            NULL))
    return false;
  if (!true_returns || !false_returns) {
    if (!emit(p, (nyr_inst_t){.op = NYIR_LABEL, .dst = -1, .imm = merge_label},
              NULL))
      return false;
  }
  p->pos = after_if;
  *returned = true_returns && false_returns;
  return true;
}

static bool lower_while(c_parser_t *p, size_t limit, bool *returned) {
  ++p->pos;
  if (!expect(p, "("))
    return false;
  size_t header_label = (size_t)p->next_label++;
  size_t body_label = (size_t)p->next_label++;
  size_t exit_label = (size_t)p->next_label++;
  if (!emit(p,
            (nyr_inst_t){
                .op = NYIR_LABEL, .dst = -1, .imm = (int64_t)header_label},
            NULL))
    return false;
  c_value_t condition = parse_expr(p, 1);
  if (condition.value < 0 || !expect(p, ")"))
    return false;
  int truth = lower_truth_value(p, condition);
  size_t body_start = p->pos;
  size_t after_body = statement_end(p, body_start);
  if (after_body > limit || after_body <= body_start) {
    fail(p, "malformed while-loop body");
    return false;
  }
  size_t body_end = after_body;
  bool body_is_block = tok_is(p, body_start, "{");
  if (body_is_block) {
    ++body_start;
    --body_end;
  }
  if (!emit(p,
            (nyr_inst_t){.op = NYIR_BR_IF,
                          .dst = -1,
                          .a = truth,
                          .imm = (int64_t)body_label},
            NULL) ||
      !emit(p,
            (nyr_inst_t){.op = NYIR_BR, .dst = -1, .imm = (int64_t)exit_label},
            NULL) ||
      !emit(p,
            (nyr_inst_t){
                .op = NYIR_LABEL, .dst = -1, .imm = (int64_t)body_label},
            NULL))
    return false;
  size_t saved_local_count = p->local_count;
  size_t saved_next_local = p->next_local;
  size_t saved_scope_start = p->scope_start;
  if (body_is_block)
    p->scope_start = p->local_count;
  size_t *saved_break_target = p->break_target;
  size_t *saved_continue_target = p->continue_target;
  p->break_target = &exit_label;
  p->continue_target = &header_label;
  bool body_returns = false;
  if (!lower_statement_range(p, body_start, body_end, &body_returns)) {
    p->break_target = saved_break_target;
    p->continue_target = saved_continue_target;
    p->scope_start = saved_scope_start;
    return false;
  }
  p->break_target = saved_break_target;
  p->continue_target = saved_continue_target;
  p->scope_start = saved_scope_start;
  p->local_count = saved_local_count;
  p->next_local = saved_next_local;
  if (!emit(
          p,
          (nyr_inst_t){.op = NYIR_BR, .dst = -1, .imm = (int64_t)header_label},
          NULL) ||
      !emit(p,
            (nyr_inst_t){
                .op = NYIR_LABEL, .dst = -1, .imm = (int64_t)exit_label},
            NULL))
    return false;
  p->pos = after_body;
  *returned = false;
  return true;
}

static bool lower_do_while(c_parser_t *p, size_t limit, bool *returned) {
  ++p->pos;
  if (p->next_label > INT_MAX - 3) {
    fail(p, "C control-flow label capacity exceeded");
    return false;
  }
  size_t body_label = (size_t)p->next_label++;
  size_t condition_label = (size_t)p->next_label++;
  size_t exit_label = (size_t)p->next_label++;
  if (!emit(p,
            (nyr_inst_t){
                .op = NYIR_LABEL, .dst = -1, .imm = (int64_t)body_label},
            NULL))
    return false;
  size_t body_start = p->pos;
  size_t after_body = statement_end(p, body_start);
  if (after_body > limit || after_body <= body_start ||
      !tok_is(p, after_body, "while")) {
    fail(p, "malformed do-while body or missing trailing while condition");
    return false;
  }
  size_t body_end = after_body;
  bool body_is_block = tok_is(p, body_start, "{");
  if (body_is_block) {
    ++body_start;
    --body_end;
  }
  size_t saved_local_count = p->local_count;
  size_t saved_next_local = p->next_local;
  size_t saved_scope_start = p->scope_start;
  if (body_is_block)
    p->scope_start = p->local_count;
  size_t *saved_break_target = p->break_target;
  size_t *saved_continue_target = p->continue_target;
  p->break_target = &exit_label;
  p->continue_target = &condition_label;
  bool body_returns = false;
  if (!lower_statement_range(p, body_start, body_end, &body_returns)) {
    p->break_target = saved_break_target;
    p->continue_target = saved_continue_target;
    p->scope_start = saved_scope_start;
    return false;
  }
  p->break_target = saved_break_target;
  p->continue_target = saved_continue_target;
  p->scope_start = saved_scope_start;
  p->local_count = saved_local_count;
  p->next_local = saved_next_local;
  if (!emit(p,
            (nyr_inst_t){
                .op = NYIR_LABEL, .dst = -1, .imm = (int64_t)condition_label},
            NULL))
    return false;
  p->pos = after_body + 1;
  if (!expect(p, "("))
    return false;
  c_value_t condition = parse_expr(p, 1);
  if (condition.value < 0 || !expect(p, ")") || !expect(p, ";"))
    return false;
  int truth = lower_truth_value(p, condition);
  if (truth < 0 ||
      !emit(p,
            (nyr_inst_t){.op = NYIR_BR_IF,
                          .dst = -1,
                          .a = truth,
                          .imm = (int64_t)body_label},
            NULL) ||
      !emit(p,
            (nyr_inst_t){.op = NYIR_BR, .dst = -1, .imm = (int64_t)exit_label},
            NULL) ||
      !emit(p,
            (nyr_inst_t){
                .op = NYIR_LABEL, .dst = -1, .imm = (int64_t)exit_label},
            NULL))
    return false;
  *returned = false;
  return true;
}

static bool lower_for(c_parser_t *p, size_t limit, bool *returned) {
  ++p->pos;
  if (!expect(p, "("))
    return false;
  size_t saved_local_count = p->local_count;
  size_t saved_next_local = p->next_local;
  size_t saved_scope_start = p->scope_start;
  p->scope_start = saved_local_count;
  if (accept(p, ";")) {
    /*
     * Empty initializer.
     */
  } else if (starts_scalar_type(p)) {
    c_type_t type;
    if (!parse_type(p, &type, false) || p->last_type_const ||
        (type != C_I64 && type != C_F64)) {
      fail(p, "for-loop initializer must be a mutable scalar");
      return false;
    }
    bool declaration_ok = true;
    do {
      ny_ctok_t name;
      if (!parse_name(p, &name)) {
        declaration_ok = false;
        break;
      }
      int initial_value = -1;
      if (accept(p, "=")) {
        c_value_t initial = parse_expr(p, 1);
        if (initial.value < 0 || initial.type != type) {
          if (initial.value >= 0)
            fail(p, "for-loop initializer type must match its declaration");
          declaration_ok = false;
          break;
        }
        initial_value = initial.value;
      }
      if (!add_local(p, name, type, initial_value, false)) {
        declaration_ok = false;
        break;
      }
    } while (accept(p, ","));
    if (!declaration_ok || !expect(p, ";"))
      return false;
  } else if (!lower_scalar_assignment(p, true)) {
    return false;
  }

  size_t header_label = (size_t)p->next_label++;
  size_t body_label = (size_t)p->next_label++;
  size_t step_label = (size_t)p->next_label++;
  size_t exit_label = (size_t)p->next_label++;
  if (!emit(p,
            (nyr_inst_t){
                .op = NYIR_LABEL, .dst = -1, .imm = (int64_t)header_label},
            NULL))
    return false;
  c_value_t condition;
  if (tok_is(p, p->pos, ";")) {
    int value =
        emit_value(p, (nyr_inst_t){.op = NYIR_CONST_I64, .dst = -1, .imm = 1});
    condition = (c_value_t){value, C_I64, NULL};
  } else {
    condition = parse_expr(p, 1);
  }
  if (condition.value < 0 || !expect(p, ";"))
    return false;
  int truth = lower_truth_value(p, condition);

  size_t step_start = p->pos;
  size_t depth = 0;
  bool found_close = false;
  while (p->pos < p->token_count) {
    if (tok_is(p, p->pos, "(")) {
      ++depth;
    } else if (tok_is(p, p->pos, ")")) {
      if (depth == 0) {
        found_close = true;
        break;
      }
      --depth;
    }
    ++p->pos;
  }
  if (!found_close) {
    fail(p, "unterminated for-loop header");
    return false;
  }
  size_t step_end = p->pos;
  if (!expect(p, ")"))
    return false;
  size_t body_start = p->pos;
  size_t after_body = statement_end(p, body_start);
  if (after_body > limit || after_body <= body_start) {
    fail(p, "malformed for-loop body");
    return false;
  }
  size_t body_end = after_body;
  bool body_is_block = tok_is(p, body_start, "{");
  if (body_is_block) {
    ++body_start;
    --body_end;
  }
  if (!emit(p,
            (nyr_inst_t){.op = NYIR_BR_IF,
                          .dst = -1,
                          .a = truth,
                          .imm = (int64_t)body_label},
            NULL) ||
      !emit(p,
            (nyr_inst_t){.op = NYIR_BR, .dst = -1, .imm = (int64_t)exit_label},
            NULL) ||
      !emit(p,
            (nyr_inst_t){
                .op = NYIR_LABEL, .dst = -1, .imm = (int64_t)body_label},
            NULL))
    return false;
  size_t body_local_count = p->local_count;
  size_t body_next_local = p->next_local;
  size_t body_scope_start = p->scope_start;
  if (body_is_block)
    p->scope_start = p->local_count;
  size_t *saved_break_target = p->break_target;
  size_t *saved_continue_target = p->continue_target;
  p->break_target = &exit_label;
  p->continue_target = &step_label;
  bool body_returns = false;
  if (!lower_statement_range(p, body_start, body_end, &body_returns)) {
    p->break_target = saved_break_target;
    p->continue_target = saved_continue_target;
    p->scope_start = body_scope_start;
    return false;
  }
  p->break_target = saved_break_target;
  p->continue_target = saved_continue_target;
  p->scope_start = body_scope_start;
  p->local_count = body_local_count;
  p->next_local = body_next_local;
  if (!emit(p,
            (nyr_inst_t){
                .op = NYIR_LABEL, .dst = -1, .imm = (int64_t)step_label},
            NULL))
    return false;
  p->pos = step_start;
  if (p->pos < step_end) {
    bool increment_step =
        tok_is(p, p->pos, "++") || tok_is(p, p->pos, "--") ||
        (tok_ident(*current(p)) &&
         (tok_is(p, p->pos + 1, "++") || tok_is(p, p->pos + 1, "--")));
    if (increment_step) {
      c_value_t ignored = parse_expr(p, 1);
      if (ignored.value < 0 || p->pos != step_end) {
        fail(p, "for-loop increment step must be one scalar update");
        return false;
      }
    } else if (!lower_scalar_assignment(p, false) || p->pos != step_end) {
      fail(p, "for-loop step must be one scalar assignment or increment");
      return false;
    }
  }
  if (!emit(
          p,
          (nyr_inst_t){.op = NYIR_BR, .dst = -1, .imm = (int64_t)header_label},
          NULL) ||
      !emit(p,
            (nyr_inst_t){
                .op = NYIR_LABEL, .dst = -1, .imm = (int64_t)exit_label},
            NULL))
    return false;
  p->local_count = saved_local_count;
  p->next_local = saved_next_local;
  p->scope_start = saved_scope_start;
  p->pos = after_body;
  *returned = false;
  return true;
}

static bool lower_statement_range(c_parser_t *p, size_t start, size_t end,
                                  bool *returned) {
  size_t saved_pos = p->pos;
  p->pos = start;
  *returned = false;
  while (p->pos < end && !*returned) {
    if (accept(p, ";"))
      continue;
    if (tok_is(p, p->pos, "{")) {
      size_t block_start = p->pos;
      size_t block_after = skip_balanced(p, block_start, "{", "}");
      if (block_after > end || block_after <= block_start + 1) {
        fail(p, "malformed compound statement");
        break;
      }
      size_t saved_local_count = p->local_count;
      size_t saved_next_local = p->next_local;
      size_t saved_scope_start = p->scope_start;
      p->scope_start = p->local_count;
      bool block_returns = false;
      if (!lower_statement_range(p, block_start + 1, block_after - 1,
                                 &block_returns)) {
        p->scope_start = saved_scope_start;
        break;
      }
      p->local_count = saved_local_count;
      p->next_local = saved_next_local;
      p->scope_start = saved_scope_start;
      p->pos = block_after;
      *returned = block_returns;
      continue;
    }
    if (accept(p, "return")) {
      if (!lower_return(p, returned))
        break;
      continue;
    }
    if (accept(p, "break")) {
      if (!p->break_target) {
        fail(p, "break statement is not inside a loop");
        break;
      }
      if (!expect(p, ";") ||
          !emit(p,
                (nyr_inst_t){
                    .op = NYIR_BR, .dst = -1, .imm = (int64_t)*p->break_target},
                NULL))
        break;
      *returned = true;
      continue;
    }
    if (accept(p, "continue")) {
      if (!p->continue_target) {
        fail(p, "continue statement is not inside a loop");
        break;
      }
      if (!expect(p, ";") ||
          !emit(p,
                (nyr_inst_t){.op = NYIR_BR,
                              .dst = -1,
                              .imm = (int64_t)*p->continue_target},
                NULL))
        break;
      *returned = true;
      continue;
    }
    if (tok_is(p, p->pos, "if")) {
      if (!lower_if(p, end, returned))
        break;
      continue;
    }
    if (tok_is(p, p->pos, "while")) {
      if (!lower_while(p, end, returned))
        break;
      continue;
    }
    if (tok_is(p, p->pos, "do")) {
      if (!lower_do_while(p, end, returned))
        break;
      continue;
    }
    if (tok_is(p, p->pos, "for")) {
      if (!lower_for(p, end, returned))
        break;
      continue;
    }
    if (tok_ident(*current(p)) && tok_is(p, p->pos + 1, "(")) {
      ny_ctok_t name = *current(p);
      ++p->pos;
      ++p->pos;
      (void)parse_call(p, name);
      if ((p->err && p->err_len && p->err[0]) || !expect(p, ";"))
        break;
      continue;
    }
    c_type_t type;
    if (starts_aggregate_type(p)) {
      if (!lower_aggregate_declaration(p, true))
        break;
      continue;
    }
    if (starts_scalar_type(p)) {
      if (!parse_type(p, &type, false))
        break;
      bool is_const = p->last_type_const;
      bool declaration_ok = true;
      do {
        ny_ctok_t name;
        if (!parse_name(p, &name)) {
          declaration_ok = false;
          break;
        }
        int initial_value = -1;
        bool initialized = accept(p, "=");
        if (initialized) {
          c_value_t initial = parse_expr(p, 1);
          if (initial.value < 0 || initial.type != type) {
            if (initial.value >= 0)
              fail(p, "local initializer type must match its declaration");
            declaration_ok = false;
            break;
          }
          initial_value = initial.value;
        } else if (is_const) {
          fail(p, "const local declaration requires an initializer");
          declaration_ok = false;
          break;
        } else if (c_type_is_pointer(type)) {
          fail(p, "uninitialized pointer locals are unsupported");
          declaration_ok = false;
          break;
        }
        if (!add_local(p, name, type, initial_value, is_const)) {
          declaration_ok = false;
          break;
        }
      } while (accept(p, ","));
      if (!declaration_ok || !expect(p, ";"))
        break;
      continue;
    }
    if (accept(p, "*")) {
      c_value_t address = parse_unary(p);
      bool read_only = p->last_value_const;
      if (!lower_pointer_store(p, address, read_only))
        break;
      continue;
    }
    if (tok_ident(*current(p)) && tok_is(p, p->pos + 1, "[")) {
      ny_ctok_t name = *current(p);
      c_local_t *indexed = find_local(p, name);
      if (indexed && indexed->type == C_AGG) {
        /*
         * struct array element assignment: points[i].field = v / points[i] = v
         */
        if (!lower_scalar_assignment(p, true))
          break;
        continue;
      }
      ++p->pos;
      c_local_t *local = indexed;
      ++p->pos;
      c_value_t index = parse_expr(p, 1);
      if (!local || (local->type != C_I64_PTR && local->type != C_F64_PTR) ||
          index.value < 0 || index.type != C_I64 || !expect(p, "]")) {
        fail(p,
             "indexed assignment requires a pointer and signed 64-bit index");
        break;
      }
      int base =
          emit_value(p, (nyr_inst_t){.op = NYIR_LOAD_LOCAL,
                                      .dst = -1,
                                      .imm = (int64_t)local->slot,
                                      .scalar_class = NYIR_SCALAR_I64_RAW});
      if (base < 0)
        break;
      int address = lower_pointer_index_address(p, base, index.value);
      if (address < 0)
        break;
      c_value_t pointer = {address, local->type, NULL};
      if (!lower_pointer_store(p, pointer, local->is_const))
        break;
      continue;
    }
    if (tok_ident(*current(p)) &&
        (is_assignment_operator(p, p->pos + 1) ||
         (find_local(p, *current(p)) && tok_is(p, p->pos + 1, ".")))) {
      if (!lower_scalar_assignment(p, true))
        break;
      continue;
    }
    if (tok_is(p, p->pos, "++") || tok_is(p, p->pos, "--") ||
        (tok_ident(*current(p)) &&
         (tok_is(p, p->pos + 1, "++") || tok_is(p, p->pos + 1, "--")))) {
      c_value_t ignored = parse_expr(p, 1);
      if (ignored.value < 0 || !expect(p, ";"))
        break;
      continue;
    }
    fail(p, "unsupported C statement in function body");
    break;
  }
  bool ok = p->pos == end && (!p->err || !p->err_len || !p->err[0]);
  if (!ok && p->err && p->err_len && !p->err[0])
    fail(p, "could not lower C statement");
  if (start != saved_pos && !*returned)
    p->pos = saved_pos;
  return ok;
}

int nyc_frontend_compile(const char *source, size_t source_len,
                         const char *symbol, nyc_abi_t abi, nyr_func_t *out,
                         char *err, size_t err_len) {
  if (!source || !symbol || !out || !err || err_len == 0)
    return 0;
  err[0] = '\0';
  c_parser_t parser = {.abi = abi, .err = err, .err_len = err_len};
  ny_lexer_t lexer;
  ny_lex_init(&lexer, source, source_len);
  size_t capacity = 256;
  parser.tokens = malloc(capacity * sizeof(*parser.tokens));
  if (!parser.tokens)
    return 0;
  for (;;) {
    ny_ctok_t token = ny_lex_next(&lexer);
    if (token.kind == NY_CTOK_PREPROC)
      continue;
    if (token.kind == NY_CTOK_ERROR) {
      snprintf(err, err_len, "%u:%u: %s", token.line, token.col,
               token.error ? token.error : "invalid C token");
      goto done;
    }
    if (parser.token_count == capacity) {
      if (capacity > SIZE_MAX / 2 / sizeof(*parser.tokens)) {
        free(parser.tokens);
        return 0;
      }
      capacity *= 2;
      ny_ctok_t *grown = realloc(parser.tokens, capacity * sizeof(*grown));
      if (!grown) {
        free(parser.tokens);
        return 0;
      }
      parser.tokens = grown;
    }
    parser.tokens[parser.token_count++] = token;
    if (token.kind == NY_CTOK_EOF)
      break;
  }
  if (!parser.token_count)
    goto done;
  if (parser.token_count > SIZE_MAX / sizeof(*parser.locals)) {
    snprintf(err, err_len, "C translation unit is too large");
    goto done;
  }
  parser.locals = calloc(parser.token_count, sizeof(*parser.locals));
  if (!parser.locals) {
    snprintf(err, err_len, "out of memory allocating C local-variable table");
    goto done;
  }
  parser.local_capacity = parser.token_count;
  if (parser.token_count > SIZE_MAX / sizeof(*parser.functions)) {
    snprintf(err, err_len, "C translation unit is too large");
    goto done;
  }
  parser.functions = calloc(parser.token_count, sizeof(*parser.functions));
  if (!parser.functions) {
    snprintf(err, err_len, "out of memory allocating C function table");
    goto done;
  }
  parser.function_capacity = parser.token_count;
  if (!collect_functions(&parser))
    goto done;
  ny_ctok_t wanted = {
      .kind = NY_CTOK_IDENT, .start = symbol, .len = strlen(symbol)};
  c_function_t *fn = find_function(&parser, wanted);
  if (!fn || !fn->defined) {
    snprintf(err, err_len, "function definition '%s' not found", symbol);
    goto done;
  }
  parser.function = fn;
  parser.out = out;
  parser.pos = fn->body_start;
  parser.param_count = fn->param_count;
  parser.next_local = fn->param_count;
  parser.local_count = fn->param_count;
  parser.scope_start = 0;
  /*
   * MEMORY-class aggregate returns take a hidden caller buffer; register
   * classes return their ABI pieces through RET.
   */
  bool sret_result = false;
  uint32_t result_packed = 0;
  if (fn->result == C_AGG) {
    if (!fn->result_agg || !fn->result_agg->complete) {
      snprintf(err, err_len,
               "aggregate return type must be a complete struct or union");
      goto done;
    }
    if (!nyc_classify_aggregate(fn->result_agg, parser.abi, 0,
                                &result_packed)) {
      snprintf(err, err_len,
               "aggregate return type is not supported by the C ABI");
      goto done;
    }
    sret_result = nyc_sysv_return_is_memory(result_packed);
  }
  size_t hidden_params = sret_result ? 1u : 0u;
  bool has_agg_param = false;
  for (size_t i = 0; i < fn->param_count; ++i)
    if (fn->params[i].type == C_AGG)
      has_agg_param = true;
  out->param_count = fn->param_count + hidden_params;
  if (out->param_count) {
    out->param_types = calloc(out->param_count, sizeof(*out->param_types));
    if (!out->param_types) {
      snprintf(err, err_len, "out of memory allocating C parameter metadata");
      goto done;
    }
    if (has_agg_param) {
      out->param_abi = calloc(out->param_count, sizeof(*out->param_abi));
      if (!out->param_abi) {
        snprintf(err, err_len,
                 "out of memory allocating C parameter ABI metadata");
        goto done;
      }
    }
  }
  if (sret_result) {
    /*
     * The hidden result buffer arrives in rdi like any leading integer
     * argument.
     */
    out->param_types[0] = NYIR_PARAM_I64;
    parser.locals[0] =
        (c_local_t){.name = {0}, .type = C_I64, .slot = 0, .is_const = true};
  }
  parser.next_local = out->param_count;
  parser.local_count = out->param_count;
  for (size_t i = 0; i < fn->param_count; ++i) {
    c_param_t param = fn->params[i];
    size_t slot = hidden_params + i;
    if (!param.name.len) {
      snprintf(err, err_len, "function definition parameters require names");
      goto done;
    }
    for (size_t previous = 0; previous < i; ++previous) {
      if (tok_same(fn->params[previous].name, param.name)) {
        snprintf(err, err_len, "duplicate parameter name in C function");
        goto done;
      }
    }
    if (param.type == C_AGG) {
      uint32_t packed = 0;
      if (!param.agg || !param.agg->complete) {
        snprintf(err, err_len,
                 "aggregate parameter type must be a complete struct or "
                 "union");
        goto done;
      }
      if (!nyc_classify_aggregate(param.agg, parser.abi, 0, &packed)) {
        snprintf(err, err_len,
                 "aggregate parameter type is not supported by the C ABI");
        goto done;
      }
      out->param_types[slot] = NYIR_PARAM_AGGREGATE;
      out->param_abi[slot] = packed;
      parser.locals[slot] = (c_local_t){.name = param.name,
                                        .type = C_AGG,
                                        .slot = (int)slot,
                                        .is_const = param.is_const,
                                        .agg = param.agg};
      continue;
    }
    out->param_types[slot] =
        param.type == C_F64 ? NYIR_PARAM_F64 : NYIR_PARAM_I64;
    parser.locals[slot] = (c_local_t){.name = param.name,
                                      .type = param.type,
                                      .slot = (int)slot,
                                      .is_const = param.is_const};
  }
  for (size_t i = 0; i < fn->param_count; ++i)
    if (fn->params[i].type == C_I64_PTR)
      out->param_types[hidden_params + i] = NYIR_PARAM_I64;
  if (fn->result == C_AGG) {
    out->return_aggregate_size = NYIR_ARG_AGG_SIZE(result_packed);
    out->return_aggregate_classes[0] =
        (uint8_t)NYIR_ARG_AGG_CLASS(result_packed, 0);
    out->return_aggregate_classes[1] =
        (uint8_t)NYIR_ARG_AGG_CLASS(result_packed, 1);
  }
  while (parser.pos < fn->body_end) {
    bool returned = false;
    size_t start = parser.pos;
    size_t end = statement_end(&parser, start);
    if (end <= start || end > fn->body_end) {
      fail(&parser, "malformed function statement");
      goto done;
    }
    if (!lower_statement_range(&parser, start, end, &returned))
      goto done;
    parser.pos = end;
    if (returned && parser.pos < fn->body_end) {
      fail(&parser, "statements after return are unsupported");
      goto done;
    }
  }
  bool is_void = fn->result == C_BAD;
  if (is_void &&
      (!parser.out->len ||
       parser.out->data[parser.out->len - 1].op != NYIR_RET) &&
      !emit(&parser,
            (nyr_inst_t){.op = NYIR_RET,
                          .dst = -1,
                          .a = -1,
                          .b = -1,
                          .scalar_class = NYIR_SCALAR_UNKNOWN},
            NULL))
    goto done;
  if (!parser.out->len ||
      parser.out->data[parser.out->len - 1].op != NYIR_RET) {
    snprintf(err, err_len, "function body must return a value");
    goto done;
  }
  if (is_void)
    parser.out->data[parser.out->len - 1].scalar_class = NYIR_SCALAR_UNKNOWN;
  else if (fn->result == C_F64)
    parser.out->data[parser.out->len - 1].scalar_class = NYIR_SCALAR_F64;
  else if (fn->result == C_AGG)
    parser.out->data[parser.out->len - 1].scalar_class =
        out->return_aggregate_classes[0] == NYIR_ARG_CLASS_SSE
            ? NYIR_SCALAR_F64
            : NYIR_SCALAR_I64_RAW;
  else
    parser.out->data[parser.out->len - 1].scalar_class = NYIR_SCALAR_I64_RAW;
  free_tag_table(&parser);
  free_function_table(&parser);
  free(parser.locals);
  free(parser.tokens);
  return 1;
done:
  free_tag_table(&parser);
  free_function_table(&parser);
  free(parser.locals);
  free(parser.tokens);
  return 0;
}
