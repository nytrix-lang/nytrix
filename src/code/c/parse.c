#include "code/c/c.h"
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Internal C declaration parser and primitive layout model.
 *
 * This parser is deliberately not a full C compiler yet. It accepts the common
 * FFI/header spine first: typedefs, extern/static/inline declarations,
 * primitive signedness/width flags, pointers, simple prototypes, parameters,
 * variadics, arrays as declarator suffixes, and tag-only struct/union/enum
 * declarations. Unsupported shapes should report exact diagnostics and remain
 * eligible for libclang fallback in auto mode.
 */

static size_t c_pack_cap_align(size_t align, unsigned pack_align);

static ny_ctok_t cempty_tok(void) { return (ny_ctok_t){NY_CTOK_EOF, "", 0, 0, 0}; }

const char *ny_ctype_kind_name(ny_ctype_kind_t kind) {
  switch (kind) {
  case NY_CTYPE_INVALID:
    return "invalid";
  case NY_CTYPE_VOID:
    return "void";
  case NY_CTYPE_BOOL:
    return "bool";
  case NY_CTYPE_CHAR:
    return "char";
  case NY_CTYPE_SHORT:
    return "short";
  case NY_CTYPE_INT:
    return "int";
  case NY_CTYPE_LONG:
    return "long";
  case NY_CTYPE_FLOAT:
    return "float";
  case NY_CTYPE_HALF:
    return "_Float16";
  case NY_CTYPE_DOUBLE:
    return "double";
  case NY_CTYPE_LONG_DOUBLE:
    return "long double";
  case NY_CTYPE_STRUCT:
    return "struct";
  case NY_CTYPE_UNION:
    return "union";
  case NY_CTYPE_ENUM:
    return "enum";
  case NY_CTYPE_NAMED:
    return "named";
  }
  return "unknown";
}

const char *ny_cdecl_kind_name(ny_cdecl_kind_t kind) {
  switch (kind) {
  case NY_CDECL_NONE:
    return "none";
  case NY_CDECL_VAR:
    return "var";
  case NY_CDECL_FUNC:
    return "func";
  case NY_CDECL_TYPEDEF:
    return "typedef";
  }
  return "unknown";
}


static int ny_c_abi_is_32(const char *abi) {
  return abi && (strstr(abi, "i386") || strstr(abi, "i686") ||
                 strstr(abi, "x86-32") || strstr(abi, "wasm32") ||
                 strstr(abi, "arm32") || strstr(abi, "mips32"));
}

static int ny_c_abi_is_win64(const char *abi) {
  return abi && (strstr(abi, "win64") || strstr(abi, "windows") ||
                 strstr(abi, "msvc"));
}

static size_t ny_c_align_up(size_t value, size_t align) {
  if (align <= 1)
    return value;
  size_t rem = value % align;
  return rem ? value + (align - rem) : value;
}

int ny_ctype_layout(const ny_ctype_t *ty, const char *abi, ny_c_layout_t *out) {
  if (!ty || !out)
    return 0;
  memset(out, 0, sizeof(*out));
  if (ty->array_unknown)
    return 0;
  if (ty->array_invalid)
    return 0;
  ny_c_layout_t base = {0};
  if (ty->ptr_depth > 0) {
    base.size = ny_c_abi_is_32(abi) ? 4 : 8;
    base.align = base.size;
    base.is_pointer = 1;
    base.is_integer = 1;
  } else if (ty->flags & NY_CTYPEF_INT128) {
    /* __int128 / unsigned __int128: 16-byte integer regardless of base kind. */
    base.size = 16;
    base.align = 16;
    base.is_integer = 1;
  } else if (ty->flags & NY_CTYPEF_COMPLEX) {
    /* _Complex T is 2 * sizeof(T): complex half = 4, complex float = 8, complex double = 16,
     * complex long double = 32. Alignment follows the element type. */
    if (ty->kind == NY_CTYPE_HALF) {
      base.size = 4;
      base.align = 2;
      base.is_float = 1;
    } else if (ty->kind == NY_CTYPE_FLOAT) {
      base.size = 8;
      base.align = 4;
      base.is_float = 1;
    } else if (ty->kind == NY_CTYPE_DOUBLE) {
      base.size = 16;
      base.align = 8;
      base.is_float = 1;
    } else if (ty->kind == NY_CTYPE_LONG_DOUBLE) {
      base.size = ny_c_abi_is_win64(abi) ? 16 : 32;
      base.align = ny_c_abi_is_win64(abi) ? 8 : 16;
      base.is_float = 1;
    } else {
      /* _Complex int/short: 2 * sizeof(integer). */
      base.size = 8;
      base.align = 4;
      base.is_integer = 1;
    }
  } else {
    switch (ty->kind) {
    case NY_CTYPE_VOID:
      base.size = 0;
      base.align = 1;
      break;
    case NY_CTYPE_BOOL:
    case NY_CTYPE_CHAR:
      base.size = 1;
      base.align = 1;
      base.is_integer = 1;
      break;
    case NY_CTYPE_SHORT:
      base.size = 2;
      base.align = 2;
      base.is_integer = 1;
      break;
    case NY_CTYPE_INT:
    case NY_CTYPE_ENUM:
      base.size = 4;
      base.align = 4;
      base.is_integer = 1;
      if (ty->kind == NY_CTYPE_ENUM && ty->enum_underlying >= 2) {
        base.size = 8;
        base.align = 8;
      }
      break;
    case NY_CTYPE_LONG:
      base.size = (ty->flags & NY_CTYPEF_LONG_LONG) || !ny_c_abi_is_win64(abi)
                      ? 8
                      : 4;
      base.align = base.size;
      base.is_integer = 1;
      break;
    case NY_CTYPE_FLOAT:
      base.size = 4;
      base.align = 4;
      base.is_float = 1;
      break;
    case NY_CTYPE_DOUBLE:
      base.size = 8;
      base.align = 8;
      base.is_float = 1;
      break;
    case NY_CTYPE_LONG_DOUBLE:
      base.size = ny_c_abi_is_win64(abi) ? 8 : 16;
      base.align = base.size;
      base.is_float = 1;
      break;
    case NY_CTYPE_HALF:
      base.size = 2;
      base.align = 2;
      base.is_float = 1;
      break;
    case NY_CTYPE_STRUCT:
    case NY_CTYPE_UNION:
      if (!ty->aggregate_has_layout)
        return 0;
      if (ty->aggregate_pack_align == 1 ||
          (ty->flags & NY_CTYPEF_PACKED) != 0) {
        base.size = ty->aggregate_packed_size;
        base.align = 1;
      } else {
        base.size = ty->aggregate_size;
        base.align = ty->aggregate_align ? ty->aggregate_align : 1;
        base.align = c_pack_cap_align(base.align, ty->aggregate_pack_align);
      }
      if (ty->align_override > 0 && ty->align_override > base.align) {
        base.align = ty->align_override;
        base.size = ny_c_align_up(base.size, base.align);
      }
      break;
    case NY_CTYPE_NAMED:
      return 0;
    default:
      return 0;
    }
  }
  *out = base;
  if (ty->array_elems > 0)
    out->size *= ty->array_elems;
  return 1;
}

const char *ny_parse_error(const ny_parser_t *p) {
  return p && p->error[0] ? p->error : "";
}

static int parse_errorf(ny_parser_t *p, const char *fmt, ...) {
  if (!p)
    return -1;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(p->error, sizeof(p->error), fmt, ap);
  va_end(ap);
  return -1;
}

static int parser_recursion_enter(ny_parser_t *p, unsigned *depth,
                                  const char *what) {
  if (!p || !depth)
    return 0;
  if (*depth >= NY_C_MAX_RECURSION_DEPTH) {
    parse_errorf(p, "C parser %s recursion limit (%u) exceeded at %u:%u",
                 what, NY_C_MAX_RECURSION_DEPTH, p->tok.line, p->tok.col);
    p->fatal_error = 1;
    p->tok = (ny_ctok_t){.kind = NY_CTOK_EOF};
    return 0;
  }
  (*depth)++;
  return 1;
}

static void parse_advance(ny_parser_t *p) {
  p->tok = ny_lex_next(&p->lx);
  p->token_count++;
  if (p->deadline_ns > 0) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
      int64_t now_ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
      if (now_ns > p->deadline_ns) {
        snprintf(p->error, sizeof(p->error),
                 "nytrix C frontend timeout — use --c-frontend=libclang for this header");
        p->tok = (ny_ctok_t){.kind = NY_CTOK_EOF};
      }
    }
  }
  if (p->token_limit > 0 && p->token_count > p->token_limit) {
    snprintf(p->error, sizeof(p->error),
             "nytrix C frontend token limit — use --c-frontend=libclang for this header");
    p->tok = (ny_ctok_t){.kind = NY_CTOK_EOF};
  }
}

static int parse_is(ny_parser_t *p, const char *lit) { return ny_ctok_eq(p->tok, lit); }
static int parse_kw(ny_parser_t *p, const char *lit) { return ny_ctok_is_ident(p->tok, lit); }

static int tok_same_ident(ny_ctok_t a, ny_ctok_t b) {
  return a.kind == NY_CTOK_IDENT && b.kind == NY_CTOK_IDENT && a.len == b.len &&
         strncmp(a.start, b.start, a.len) == 0;
}

static int parse_accept(ny_parser_t *p, const char *lit) {
  if (!parse_is(p, lit))
    return 0;
  parse_advance(p);
  return 1;
}

static void skip_balanced(ny_parser_t *p, const char *open, const char *close) {
  int depth = 0;
  if (!parse_accept(p, open))
    return;
  depth = 1;
  while (p->tok.kind != NY_CTOK_EOF && depth > 0) {
    if (parse_is(p, open))
      depth++;
    else if (parse_is(p, close))
      depth--;
    parse_advance(p);
  }
}

/* P-2 fix: intern a string into stable storage so that tokens from included
 * files survive after free(inc_src). */
static const char *parser_intern(ny_parser_t *p, const char *s, size_t len) {
  if (!p || !s || len == 0)
    return s;
  size_t need = p->intern_len + len + 1;
  if (need > p->intern_cap) {
    size_t new_cap = p->intern_cap ? p->intern_cap * 2 : 4096;
    while (new_cap < need)
      new_cap *= 2;
    char *buf = realloc(p->intern_buf, new_cap);
    if (!buf)
      return s;
    p->intern_buf = buf;
    p->intern_cap = new_cap;
  }
  char *dst = p->intern_buf + p->intern_len;
  memcpy(dst, s, len);
  dst[len] = '\0';
  p->intern_len += len + 1;
  return dst;
}

/* Return a stable copy of a token's name bytes. For tokens that originate
 * from included-file source buffers that will be freed, the returned pointer
 * lives in the parser's intern arena. For tokens already in stable storage
 * (e.g. the main source), this is a no-op. */
static ny_ctok_t ctok_intern(ny_parser_t *p, ny_ctok_t tok) {
  if (tok.kind != NY_CTOK_IDENT || !tok.start || tok.len == 0)
    return tok;
  tok.start = parser_intern(p, tok.start, tok.len);
  return tok;
}

static int parse_calling_conv(ny_parser_t *p) {
  if (parse_kw(p, "__cdecl") || parse_kw(p, "__cdecl__") ||
      parse_kw(p, "__stdcall") || parse_kw(p, "__stdcall__") ||
      parse_kw(p, "__fastcall") || parse_kw(p, "__fastcall__") ||
      parse_kw(p, "__thiscall") || parse_kw(p, "__vectorcall") ||
      parse_kw(p, "__attribute_const__") || parse_kw(p, "__extension__")) {
    parse_advance(p);
    return 1;
  }
  return 0;
}

static int parse_array_extent_expr(ny_parser_t *p, size_t *out);

static void parse_attribute_align_arg(ny_parser_t *p, ny_ctype_t *ty) {
  if (!parse_accept(p, "("))
    return;
  size_t value = 0;
  if (ty && parse_array_extent_expr(p, &value) && value > 0 &&
      (value & (value - 1)) == 0) {
    if (value > ty->align_override)
      ty->align_override = (unsigned)value;
  }
  int depth = 1;
  while (p->tok.kind != NY_CTOK_EOF && depth > 0) {
    if (parse_is(p, "("))
      depth++;
    else if (parse_is(p, ")"))
      depth--;
    parse_advance(p);
  }
}

static int parse_attribute(ny_parser_t *p, ny_ctype_t *ty) {
  if (parse_kw(p, "__attribute__") || parse_kw(p, "__attribute")) {
    parse_advance(p);
    if (parse_accept(p, "(")) {
      int depth = 1;
      while (p->tok.kind != NY_CTOK_EOF && depth > 0) {
        if (parse_is(p, "("))
          depth++;
        else if (parse_is(p, ")"))
          depth--;
        else if (ty && (parse_kw(p, "packed") || parse_kw(p, "__packed__"))) {
          /* __attribute__((packed)) is equivalent to #pragma pack(1): it both
           * sets the packed flag (size selection uses packed_size, align=1) and
           * caps every field's alignment to 1 (field offset/storage helpers
           * read aggregate_pack_align). Setting both keeps the two paths in
           * sync; the size-selection block accepts either signal. */
          ty->flags |= NY_CTYPEF_PACKED;
          ty->aggregate_pack_align = 1;
        }
        else if ((parse_kw(p, "aligned") || parse_kw(p, "__aligned__")) &&
                 depth == 2) {
          parse_advance(p);
          if (parse_is(p, "(")) {
            parse_attribute_align_arg(p, ty);
            continue;
          }
          continue;
        } else if (parse_kw(p, "unused") || parse_kw(p, "__unused__")) {
          /* Recognized, skipped */
        } else if (parse_kw(p, "deprecated") || parse_kw(p, "__deprecated__")) {
          /* Recognized, skipped */
        } else if (parse_kw(p, "weak") || parse_kw(p, "__weak__")) {
          /* Recognized, skipped */
        } else if (parse_kw(p, "const") || parse_kw(p, "__const__")) {
          /* Recognized, skipped */
        } else if (parse_kw(p, "pure") || parse_kw(p, "__pure__")) {
          /* Recognized, skipped */
        } else if (parse_kw(p, "noreturn") || parse_kw(p, "__noreturn__") ||
                   parse_kw(p, "__NORETURN__")) {
          /* Recognized, skipped */
        } else if (parse_kw(p, "malloc") || parse_kw(p, "__malloc__")) {
          /* Recognized, skipped */
        } else if (parse_kw(p, "warn_unused_result") ||
                   parse_kw(p, "__warn_unused_result__")) {
          /* Recognized, skipped */
        } else if (parse_kw(p, "format") || parse_kw(p, "__format__")) {
          /* __format__(type, str_idx, first_arg) — skip 3 args */
          if (depth == 2 && parse_accept(p, "(")) {
            depth++;
            while (depth > 2 && p->tok.kind != NY_CTOK_EOF) {
              if (parse_is(p, "("))
                depth++;
              else if (parse_is(p, ")"))
                depth--;
              else
                parse_advance(p);
            }
            if (depth == 2)
              parse_accept(p, ")");
          }
        } else if (parse_kw(p, "visibility")) {
          if (depth == 2 && parse_accept(p, "(")) {
            depth++;
            while (depth > 2 && p->tok.kind != NY_CTOK_EOF) {
              if (parse_is(p, "("))
                depth++;
              else if (parse_is(p, ")"))
                depth--;
              else
                parse_advance(p);
            }
            if (depth == 2)
              parse_accept(p, ")");
          }
        } else if (parse_kw(p, "alloc_size")) {
          if (depth == 2 && parse_accept(p, "(")) {
            depth++;
            while (depth > 2 && p->tok.kind != NY_CTOK_EOF) {
              if (parse_is(p, "("))
                depth++;
              else if (parse_is(p, ")"))
                depth--;
              else
                parse_advance(p);
            }
            if (depth == 2)
              parse_accept(p, ")");
          }
        } else if (parse_kw(p, "vector_size") || parse_kw(p, "__vector_size__")) {
          if (depth == 2 && parse_accept(p, "(")) {
            depth++;
            while (depth > 2 && p->tok.kind != NY_CTOK_EOF) {
              if (parse_is(p, "("))
                depth++;
              else if (parse_is(p, ")"))
                depth--;
              else
                parse_advance(p);
            }
            if (depth == 2)
              parse_accept(p, ")");
          }
        }
        parse_advance(p);
      }
    }
    return 1;
  }
  if (parse_kw(p, "__declspec")) {
    parse_advance(p);
    if (parse_is(p, "("))
      skip_balanced(p, "(", ")");
    return 1;
  }
  if (parse_kw(p, "_Alignas") || parse_kw(p, "alignas")) {
    parse_advance(p);
    if (parse_is(p, "(")) {
      parse_attribute_align_arg(p, ty);
    }
    return 1;
  }
  return 0;
}

static int parse_decl_marker(ny_parser_t *p) {
  return parse_calling_conv(p) || parse_attribute(p, NULL);
}

static int parse_type_marker(ny_parser_t *p, ny_ctype_t *ty) {
  return parse_calling_conv(p) || parse_attribute(p, ty);
}

static void type_init(ny_ctype_t *ty);
static void parse_ptrs(ny_parser_t *p, ny_ctype_t *ty);
static void parse_array_suffix(ny_parser_t *p, ny_ctype_t *ty);
static int parse_decimal_bits(ny_ctok_t tok, unsigned *out);
static int parse_array_extent_expr(ny_parser_t *p, size_t *out);
static int skip_function_suffix(ny_parser_t *p);
static void aggregate_add_storage(ny_ctype_t *ty, const ny_c_layout_t *layout,
                                  unsigned align_override, size_t *size,
                                  size_t *packed_size, size_t *align);
static size_t aggregate_field_offset(const ny_ctype_t *owner,
                                     const ny_c_layout_t *layout,
                                     unsigned align_override, size_t size);
static void aggregate_note_field(ny_ctype_t *owner, ny_ctok_t name,
                                 const ny_ctype_t *field_ty,
                                 const ny_c_layout_t *layout, size_t offset,
                                 unsigned bitfield_width);
static int parse_named_type(ny_parser_t *p, ny_ctype_t *ty, ny_ctok_t *name,
                            int allow_abstract);

static ny_ctype_t c_type_without_array(ny_ctype_t ty) {
  ty.array_elems = 0;
  ty.array_unknown = 0;
  return ty;
}

static int c_type_is_flexible_array(const ny_ctype_t *ty) {
  return ty && ty->array_unknown && ty->array_elems == 0;
}

static int c_type_is_function_pointer(const ny_ctype_t *ty) {
  return ty && (ty->flags & NY_CTYPEF_FUNCTION_PTR) != 0;
}

static unsigned c_type_function_pointer_slots(const ny_ctype_t *ty) {
  if (!c_type_is_function_pointer(ty))
    return 0;
  if (ty->array_elems == 0)
    return 1;
  return ty->array_elems > (size_t)((unsigned)-1) ? (unsigned)-1
                                                  : (unsigned)ty->array_elems;
}

static int c_type_is_anonymous_aggregate_field(const ny_ctype_t *ty,
                                               ny_ctok_t name) {
  return ty && name.kind != NY_CTOK_IDENT && ty->aggregate_has_layout &&
         (ty->kind == NY_CTYPE_STRUCT || ty->kind == NY_CTYPE_UNION);
}

/* Flatten child fields of an anonymous struct/union into the parent.
 * base_offset is where the anonymous aggregate itself starts within the parent.
 * For union parents base_offset is always 0; for struct parents it is the
 * aligned start of the anonymous member. */
static void aggregate_flatten_anonymous(ny_ctype_t *parent,
                                        const ny_ctype_t *anon,
                                        size_t base_offset) {
  if (!parent || !anon)
    return;
  for (unsigned i = 0; i < anon->field_count && i < NY_C_MAX_FIELDS; i++) {
    if (parent->field_count >= NY_C_MAX_FIELDS)
      break;
    const ny_c_field_t *src = &anon->fields[i];
    if (src->name.kind != NY_CTOK_IDENT)
      continue;
    ny_c_field_t *dst = &parent->fields[parent->field_count++];
    *dst = *src;
    dst->offset = base_offset + src->offset;
  }
}

static size_t c_pack_cap_align(size_t align, unsigned pack_align) {
  if (pack_align > 0 && align > pack_align)
    return pack_align;
  return align;
}

static void skip_to_decl_end(ny_parser_t *p) {
  while (p->tok.kind != NY_CTOK_EOF) {
    if (parse_is(p, ";")) {
      parse_advance(p);
      return;
    }
    if (parse_is(p, "{")) {
      skip_balanced(p, "{", "}");
      return;
    }
    parse_advance(p);
  }
}

/* Forward declarations for statement/expression skipping. */
static void skip_expr(ny_parser_t *p);
static void skip_stmt(ny_parser_t *p);
static void skip_compound_stmt(ny_parser_t *p);
static void skip_initializer(ny_parser_t *p);

static void skip_postfix_expr(ny_parser_t *p) {
  /* Consume the primary expression first: an identifier, number, string, or
   * character literal. Without this, an expression that begins with one of
   * these (the common case, e.g. `return a + b;`) left the primary unconsumed
   * and skip_expr made no progress, hanging the C frontend forever. */
  if (p->tok.kind == NY_CTOK_IDENT || p->tok.kind == NY_CTOK_NUMBER ||
      p->tok.kind == NY_CTOK_STRING || p->tok.kind == NY_CTOK_CHAR) {
    parse_advance(p);
  }
  for (;;) {
    if (parse_is(p, "[")) {
      parse_advance(p);
      skip_expr(p);
      parse_accept(p, "]");
    } else if (parse_is(p, "(")) {
      skip_balanced(p, "(", ")");
    } else if (parse_is(p, ".") || parse_is(p, "->")) {
      parse_advance(p);
      if (p->tok.kind == NY_CTOK_IDENT)
        parse_advance(p);
    } else if (parse_is(p, "++") || parse_is(p, "--")) {
      parse_advance(p);
    } else {
      break;
    }
  }
}

static void skip_unary_expr(ny_parser_t *p) {
  if (parse_is(p, "++") || parse_is(p, "--")) {
    parse_advance(p);
    skip_unary_expr(p);
    return;
  }
  if (parse_is(p, "!") || parse_is(p, "~") || parse_is(p, "+") ||
      parse_is(p, "-")) {
    parse_advance(p);
    skip_unary_expr(p);
    return;
  }
  if (parse_is(p, "*") || parse_is(p, "&")) {
    parse_advance(p);
    skip_unary_expr(p);
    return;
  }
  if (parse_kw(p, "sizeof")) {
    parse_advance(p);
    if (parse_is(p, "(")) {
      ny_parser_t try_type = *p;
      parse_advance(&try_type);
      ny_ctype_t ty;
      ny_ctok_t nm;
      type_init(&ty);
      if (parse_named_type(&try_type, &ty, &nm, 1) > 0 &&
          parse_is(&try_type, ")")) {
        *p = try_type;
      } else {
        skip_balanced(p, "(", ")");
      }
    } else {
      skip_unary_expr(p);
    }
    return;
  }
  if (parse_kw(p, "_Generic")) {
    parse_advance(p);
    if (parse_is(p, "(")) {
      skip_balanced(p, "(", ")");
    }
    return;
  }
  if (parse_kw(p, "_Alignof") || parse_kw(p, "_Alignof")) {
    parse_advance(p);
    if (parse_is(p, "("))
      skip_balanced(p, "(", ")");
    return;
  }
  if (parse_kw(p, "__extension__")) {
    parse_advance(p);
    skip_unary_expr(p);
    return;
  }
  skip_postfix_expr(p);
}

static int skip_cast_expr_check(ny_parser_t *p) {
  if (!parse_is(p, "("))
    return 0;
  ny_parser_t try_cast = *p;
  parse_advance(&try_cast);
  ny_ctype_t ty;
  ny_ctok_t nm;
  type_init(&ty);
  if (parse_named_type(&try_cast, &ty, &nm, 1) > 0 &&
      parse_is(&try_cast, ")")) {
    return 1;
  }
  return 0;
}

static void skip_expr_impl(ny_parser_t *p) {
  if (parse_is(p, "(") && skip_cast_expr_check(p)) {
    parse_advance(p);
    skip_unary_expr(p);
    skip_postfix_expr(p);
  } else {
    skip_unary_expr(p);
  }
  for (;;) {
    if (parse_is(p, "*") || parse_is(p, "/") || parse_is(p, "%")) {
      parse_advance(p);
      skip_unary_expr(p);
    } else if (parse_is(p, "+") || parse_is(p, "-")) {
      parse_advance(p);
      skip_unary_expr(p);
    } else if (parse_is(p, "<<") || parse_is(p, ">>")) {
      parse_advance(p);
      skip_unary_expr(p);
    } else if (parse_is(p, "<") || parse_is(p, ">") ||
               parse_is(p, "<=") || parse_is(p, ">=")) {
      parse_advance(p);
      skip_unary_expr(p);
    } else if (parse_is(p, "==") || parse_is(p, "!=")) {
      parse_advance(p);
      skip_unary_expr(p);
    } else if (parse_is(p, "&")) {
      parse_advance(p);
      skip_unary_expr(p);
    } else if (parse_is(p, "^")) {
      parse_advance(p);
      skip_unary_expr(p);
    } else if (parse_is(p, "|")) {
      parse_advance(p);
      skip_unary_expr(p);
    } else if (parse_is(p, "&&")) {
      parse_advance(p);
      skip_unary_expr(p);
    } else if (parse_is(p, "||")) {
      parse_advance(p);
      skip_unary_expr(p);
    } else if (parse_is(p, "?")) {
      parse_advance(p);
      skip_expr(p);
      if (parse_accept(p, ":"))
        skip_expr(p);
    } else if (parse_is(p, "=") || parse_is(p, "+=") ||
               parse_is(p, "-=") || parse_is(p, "*=") ||
               parse_is(p, "/=") || parse_is(p, "%=") ||
               parse_is(p, "<<=") || parse_is(p, ">>=") ||
               parse_is(p, "&=") || parse_is(p, "^=") ||
               parse_is(p, "|=")) {
      parse_advance(p);
      skip_expr(p);
    } else {
      break;
    }
  }
}

static void skip_expr(ny_parser_t *p) {
  if (!parser_recursion_enter(p, &p->skip_expr_depth, "expression"))
    return;
  skip_expr_impl(p);
  p->skip_expr_depth--;
}

static void skip_initializer(ny_parser_t *p) {
  if (!parse_is(p, "{")) {
    skip_expr(p);
    return;
  }
  parse_advance(p);
  while (p->tok.kind != NY_CTOK_EOF && !parse_is(p, "}")) {
    if (parse_is(p, ".") || (parse_is(p, "[") && p->tok.kind == NY_CTOK_PUNCT)) {
      while (parse_is(p, ".") || parse_is(p, "[")) {
        if (parse_is(p, ".")) {
          parse_advance(p);
          if (p->tok.kind == NY_CTOK_IDENT)
            parse_advance(p);
        } else {
          skip_balanced(p, "[", "]");
        }
        if (parse_accept(p, "="))
          ;
      }
    }
    skip_initializer(p);
    parse_accept(p, ",");
  }
  parse_accept(p, "}");
}

static void skip_compound_stmt(ny_parser_t *p) {
  if (!parse_is(p, "{")) {
    skip_stmt(p);
    return;
  }
  parse_advance(p);
  while (p->tok.kind != NY_CTOK_EOF && !parse_is(p, "}")) {
    skip_stmt(p);
  }
  parse_accept(p, "}");
}

static void skip_stmt(ny_parser_t *p) {
  if (parse_is(p, "{")) {
    skip_compound_stmt(p);
    return;
  }
  if (parse_kw(p, "if")) {
    parse_advance(p);
    if (parse_is(p, "("))
      skip_balanced(p, "(", ")");
    skip_stmt(p);
    if (parse_kw(p, "else")) {
      parse_advance(p);
      skip_stmt(p);
    }
    return;
  }
  if (parse_kw(p, "while")) {
    parse_advance(p);
    if (parse_is(p, "("))
      skip_balanced(p, "(", ")");
    skip_stmt(p);
    return;
  }
  if (parse_kw(p, "for")) {
    parse_advance(p);
    if (parse_is(p, "(")) {
      parse_advance(p);
      while (p->tok.kind != NY_CTOK_EOF && !parse_is(p, ")")) {
        if (parse_is(p, ";")) {
          parse_advance(p);
        } else {
          skip_expr(p);
          parse_accept(p, ";");
        }
      }
      parse_accept(p, ")");
    }
    skip_stmt(p);
    return;
  }
  if (parse_kw(p, "switch")) {
    parse_advance(p);
    if (parse_is(p, "("))
      skip_balanced(p, "(", ")");
    skip_compound_stmt(p);
    return;
  }
  if (parse_kw(p, "case")) {
    parse_advance(p);
    while (p->tok.kind != NY_CTOK_EOF && !parse_is(p, ":")) {
      if (parse_is(p, "...")) {
        parse_advance(p);
        skip_expr(p);
      } else {
        skip_expr(p);
      }
      parse_accept(p, ":");
    }
    parse_accept(p, ":");
    skip_stmt(p);
    return;
  }
  if (parse_kw(p, "default")) {
    parse_advance(p);
    parse_accept(p, ":");
    skip_stmt(p);
    return;
  }
  if (parse_kw(p, "return")) {
    parse_advance(p);
    if (!parse_is(p, ";") && p->tok.kind != NY_CTOK_EOF)
      skip_expr(p);
    parse_accept(p, ";");
    return;
  }
  if (parse_kw(p, "break")) {
    parse_advance(p);
    parse_accept(p, ";");
    return;
  }
  if (parse_kw(p, "continue")) {
    parse_advance(p);
    parse_accept(p, ";");
    return;
  }
  if (parse_kw(p, "goto")) {
    parse_advance(p);
    if (p->tok.kind == NY_CTOK_IDENT)
      parse_advance(p);
    parse_accept(p, ";");
    return;
  }
  if (parse_kw(p, "do")) {
    parse_advance(p);
    skip_stmt(p);
    if (parse_kw(p, "while")) {
      parse_advance(p);
      if (parse_is(p, "("))
        skip_balanced(p, "(", ")");
      parse_accept(p, ";");
    }
    return;
  }
  skip_expr(p);
  parse_accept(p, ";");
}

static void skip_function_body(ny_parser_t *p) {
  skip_compound_stmt(p);
}

static void type_init(ny_ctype_t *ty) {
  memset(ty, 0, sizeof(*ty));
  ty->kind = NY_CTYPE_INVALID;
  ty->name = cempty_tok();
}

static int parser_lookup_typedef(ny_parser_t *p, ny_ctok_t name,
                                 ny_ctype_t *out) {
  if (!p || name.kind != NY_CTOK_IDENT || !out)
    return 0;
  for (unsigned i = p->typedef_count; i > 0; --i) {
    unsigned idx = i - 1;
    if (tok_same_ident(p->typedef_names[idx], name)) {
      *out = p->typedef_types[idx];
      return 1;
    }
  }
  return 0;
}

static int parser_lookup_abi_typedef(ny_parser_t *p, ny_ctok_t name,
                                     ny_ctype_t *out) {
  if (!p || name.kind != NY_CTOK_IDENT || !out)
    return 0;
  bool is_size = ny_ctok_eq(name, "size_t") ||
                 ny_ctok_eq(name, "uintptr_t");
  bool is_signed_size = ny_ctok_eq(name, "ssize_t") ||
                        ny_ctok_eq(name, "ptrdiff_t") ||
                        ny_ctok_eq(name, "intptr_t");
  if (!is_size && !is_signed_size)
    return 0;
  type_init(out);
  out->flags = is_size ? NY_CTYPEF_UNSIGNED : NY_CTYPEF_SIGNED;
  if (ny_c_abi_is_32(p->abi)) {
    out->kind = NY_CTYPE_INT;
  } else {
    out->kind = NY_CTYPE_LONG;
    if (ny_c_abi_is_win64(p->abi))
      out->flags |= NY_CTYPEF_LONG_LONG;
  }
  return 1;
}

static void parser_note_typedef(ny_parser_t *p, ny_ctok_t name,
                                const ny_ctype_t *ty) {
  if (!p || !ty || name.kind != NY_CTOK_IDENT)
    return;
  for (unsigned i = 0; i < p->typedef_count; ++i) {
    if (tok_same_ident(p->typedef_names[i], name)) {
      p->typedef_types[i] = *ty;
      return;
    }
  }
  if (p->typedef_count >= NY_C_MAX_TYPEDEFS) {
    if (!p->fatal_error) {
      snprintf(p->error, sizeof(p->error),
               "typedef limit exceeded (%u max)", NY_C_MAX_TYPEDEFS);
      p->fatal_error = 1;
    }
    return;
  }
  p->typedef_names[p->typedef_count] = name;
  p->typedef_types[p->typedef_count] = *ty;
  p->typedef_count++;
}

static int parser_lookup_tag(ny_parser_t *p, ny_ctok_t name,
                             ny_ctype_kind_t kind, ny_ctype_t *out) {
  if (!p || name.kind != NY_CTOK_IDENT || !out)
    return 0;
  for (unsigned i = p->tag_count; i > 0; --i) {
    unsigned idx = i - 1;
    if (p->tag_types[idx].kind == kind && tok_same_ident(p->tag_names[idx], name)) {
      *out = p->tag_types[idx];
      return 1;
    }
  }
  return 0;
}

static void parser_note_tag(ny_parser_t *p, ny_ctok_t name,
                            const ny_ctype_t *ty) {
  if (!p || !ty || name.kind != NY_CTOK_IDENT ||
      (ty->kind != NY_CTYPE_STRUCT && ty->kind != NY_CTYPE_UNION))
    return;
  for (unsigned i = 0; i < p->tag_count; ++i) {
    if (p->tag_types[i].kind == ty->kind && tok_same_ident(p->tag_names[i], name)) {
      p->tag_types[i] = *ty;
      return;
    }
  }
  if (p->tag_count >= NY_C_MAX_TAGS) {
    if (!p->fatal_error) {
      snprintf(p->error, sizeof(p->error),
               "tag limit exceeded (%u max)", NY_C_MAX_TAGS);
      p->fatal_error = 1;
    }
    return;
  }
  p->tag_names[p->tag_count] = name;
  p->tag_types[p->tag_count] = *ty;
  p->tag_count++;
}

static int ctok_is_ident_slice(ny_ctok_t tok, const char *start, size_t len) {
  return tok.kind == NY_CTOK_IDENT && tok.len == len &&
         strncmp(tok.start, start, len) == 0;
}

static int parser_lookup_define(ny_parser_t *p, ny_ctok_t name, int64_t *out) {
  if (!p || name.kind != NY_CTOK_IDENT || !out)
    return 0;
  for (unsigned i = p->define_count; i > 0; --i) {
    unsigned idx = i - 1;
    if (tok_same_ident(p->define_names[idx], name)) {
      *out = p->define_values[idx];
      return 1;
    }
  }
  return 0;
}

static void parser_note_define(ny_parser_t *p, const char *name,
                               size_t name_len, int64_t value) {
  if (!p || !name || name_len == 0)
    return;
  for (unsigned i = 0; i < p->define_count; ++i) {
    if (ctok_is_ident_slice(p->define_names[i], name, name_len)) {
      p->define_values[i] = value;
      return;
    }
  }
  if (p->define_count >= NY_C_MAX_DEFINES) {
    if (!p->fatal_error) {
      snprintf(p->error, sizeof(p->error),
               "define limit exceeded (%u max)", NY_C_MAX_DEFINES);
      p->fatal_error = 1;
    }
    return;
  }
  p->define_names[p->define_count] =
      (ny_ctok_t){NY_CTOK_IDENT, name, name_len, 0, 0};
  p->define_values[p->define_count] = value;
  p->define_count++;
}

static void parser_forget_define(ny_parser_t *p, const char *name,
                                 size_t name_len) {
  if (!p || !name || name_len == 0)
    return;
  for (unsigned i = 0; i < p->define_count; ++i) {
    if (!ctok_is_ident_slice(p->define_names[i], name, name_len))
      continue;
    if (i + 1 < p->define_count) {
      memmove(&p->define_names[i], &p->define_names[i + 1],
              sizeof(p->define_names[0]) * (p->define_count - i - 1));
      memmove(&p->define_values[i], &p->define_values[i + 1],
              sizeof(p->define_values[0]) * (p->define_count - i - 1));
    }
    p->define_count--;
    return;
  }
  /* Also forget string defines. */
  for (unsigned i = 0; i < p->str_define_count; ++i) {
    if (!ctok_is_ident_slice(p->str_define_names[i], name, name_len))
      continue;
    if (i + 1 < p->str_define_count) {
      memmove(&p->str_define_names[i], &p->str_define_names[i + 1],
              sizeof(p->str_define_names[0]) * (p->str_define_count - i - 1));
      memmove(&p->str_define_values[i], &p->str_define_values[i + 1],
              sizeof(p->str_define_values[0]) * (p->str_define_count - i - 1));
    }
    p->str_define_count--;
    return;
  }
}

static void parser_note_str_define(ny_parser_t *p, const char *name,
                                   size_t name_len, const char *value,
                                   size_t value_len) {
  if (!p || !name || name_len == 0 || !value || value_len == 0 ||
      value_len >= 64)
    return;
  for (unsigned i = 0; i < p->str_define_count; ++i) {
    if (ctok_is_ident_slice(p->str_define_names[i], name, name_len)) {
      memcpy(p->str_define_values[i], value, value_len);
      p->str_define_values[i][value_len] = '\0';
      return;
    }
  }
  if (p->str_define_count >= NY_C_MAX_DEFINES) {
    if (!p->fatal_error) {
      snprintf(p->error, sizeof(p->error),
               "string define limit exceeded (%u max)", NY_C_MAX_DEFINES);
      p->fatal_error = 1;
    }
    return;
  }
  p->str_define_names[p->str_define_count] =
      (ny_ctok_t){NY_CTOK_IDENT, name, name_len, 0, 0};
  memcpy(p->str_define_values[p->str_define_count], value, value_len);
  p->str_define_values[p->str_define_count][value_len] = '\0';
  p->str_define_count++;
}

static const char *parser_lookup_str_define(ny_parser_t *p, ny_ctok_t name) {
  if (!p || name.kind != NY_CTOK_IDENT)
    return NULL;
  for (unsigned i = p->str_define_count; i > 0; --i) {
    unsigned idx = i - 1;
    if (tok_same_ident(p->str_define_names[idx], name))
      return p->str_define_values[idx];
  }
  return NULL;
}

static int parser_lookup_func_macro(ny_parser_t *p, ny_ctok_t name,
                                     unsigned *idx_out) {
  if (!p || name.kind != NY_CTOK_IDENT)
    return 0;
  for (unsigned i = 0; i < p->func_macro_count; ++i) {
    if (tok_same_ident(p->func_macro_names[i], name)) {
      if (idx_out)
        *idx_out = i;
      return 1;
    }
  }
  return 0;
}

static void parser_note_func_macro(ny_parser_t *p, const char *name,
                                    size_t name_len, const char *params_str,
                                    size_t params_len, const char *body,
                                    size_t body_len, int is_variadic) {
  if (!p || !name || name_len == 0 || p->func_macro_count >= NY_C_MAX_FUNC_MACROS) {
    if (p && p->func_macro_count >= NY_C_MAX_FUNC_MACROS && !p->fatal_error) {
      snprintf(p->error, sizeof(p->error),
               "function macro limit exceeded (%u max)", NY_C_MAX_FUNC_MACROS);
      p->fatal_error = 1;
    }
    return;
  }
  unsigned idx = p->func_macro_count;
  p->func_macro_names[idx] =
      (ny_ctok_t){NY_CTOK_IDENT, name, name_len, 0, 0};
  p->func_macro_is_variadic[idx] = is_variadic;
  p->func_macro_param_count[idx] = 0;
  if (params_str && params_len > 0) {
    const char *ps = params_str;
    const char *pe = params_str + params_len;
    while (ps < pe && p->func_macro_param_count[idx] < NY_C_MAX_MACRO_PARAMS) {
      while (ps < pe && (*ps == ' ' || *ps == '\t'))
        ps++;
      if (ps < pe && *ps == '.')
        break;
      const char *start = ps;
      while (ps < pe && *ps != ',' && *ps != ' ' && *ps != '\t')
        ps++;
      size_t plen = (size_t)(ps - start);
      if (plen > 0 && plen < 32) {
        memcpy(p->func_macro_params[idx][p->func_macro_param_count[idx]], start,
               plen);
        p->func_macro_params[idx][p->func_macro_param_count[idx]][plen] = '\0';
        p->func_macro_param_count[idx]++;
      }
      if (ps < pe && *ps == ',')
        ps++;
    }
  }
  if (body && body_len > 0 && body_len < NY_C_MAX_MACRO_BODY) {
    memcpy(p->func_macro_bodies[idx], body, body_len);
    p->func_macro_bodies[idx][body_len] = '\0';
  } else {
    p->func_macro_bodies[idx][0] = '\0';
  }
  p->func_macro_count++;
}

static void parser_forget_func_macro(ny_parser_t *p, const char *name,
                                      size_t name_len) {
  if (!p || !name || name_len == 0)
    return;
  for (unsigned i = 0; i < p->func_macro_count; ++i) {
    if (!ctok_is_ident_slice(p->func_macro_names[i], name, name_len))
      continue;
    if (i + 1 < p->func_macro_count) {
      size_t rest = p->func_macro_count - i - 1;
      memmove(&p->func_macro_names[i], &p->func_macro_names[i + 1],
              sizeof(p->func_macro_names[0]) * rest);
      memmove(p->func_macro_params[i], p->func_macro_params[i + 1],
              sizeof(p->func_macro_params[0]) * rest);
      memmove(&p->func_macro_param_count[i], &p->func_macro_param_count[i + 1],
              sizeof(p->func_macro_param_count[0]) * rest);
      memmove(&p->func_macro_is_variadic[i], &p->func_macro_is_variadic[i + 1],
              sizeof(p->func_macro_is_variadic[0]) * rest);
      memmove(p->func_macro_bodies[i], p->func_macro_bodies[i + 1],
              sizeof(p->func_macro_bodies[0]) * rest);
    }
    p->func_macro_count--;
    return;
  }
}

static int c_ident_start_char(char c) {
  return c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int c_ident_char(char c) {
  return c_ident_start_char(c) || (c >= '0' && c <= '9');
}

static int c_digit_value(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F')
    return 10 + (c - 'A');
  return -1;
}

static int c_parse_integer_slice(const char *s, size_t n, size_t *pos,
                                 int64_t *out) {
  if (!s || !pos || !out || *pos >= n)
    return 0;
  size_t i = *pos;
  unsigned base = 10;
  if (i + 1 < n && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
    base = 16;
    i += 2;
  } else if (i + 1 < n && s[i] == '0' &&
             (s[i + 1] == 'b' || s[i + 1] == 'B')) {
    base = 2;
    i += 2;
  } else if (s[i] == '0') {
    base = 8;
  }
  int64_t value = 0;
  size_t digits = 0;
  for (; i < n; ++i) {
    char c = s[i];
    if (c == '_')
      continue;
    int digit = c_digit_value(c);
    if (digit < 0 || (unsigned)digit >= base)
      break;
    if (value > (INT64_MAX - digit) / base)
      return 0;
    value = value * base + digit;
    digits++;
  }
  if (digits == 0)
    return 0;
  while (i < n && (s[i] == 'u' || s[i] == 'U' || s[i] == 'l' ||
                   s[i] == 'L' || s[i] == 'z' || s[i] == 'Z'))
    i++;
  *pos = i;
  *out = value;
  return 1;
}

static int c_parse_char_escape(const char *s, size_t n, size_t *pos,
                               size_t *out) {
  if (!s || !pos || !out || *pos >= n)
    return 0;
  char c = s[(*pos)++];
  switch (c) {
  case '\'':
    *out = '\'';
    return 1;
  case '"':
    *out = '"';
    return 1;
  case '?':
    *out = '?';
    return 1;
  case '\\':
    *out = '\\';
    return 1;
  case 'a':
    *out = 7;
    return 1;
  case 'b':
    *out = 8;
    return 1;
  case 'f':
    *out = 12;
    return 1;
  case 'n':
    *out = 10;
    return 1;
  case 'r':
    *out = 13;
    return 1;
  case 't':
    *out = 9;
    return 1;
  case 'v':
    *out = 11;
    return 1;
  case 'x': {
    size_t value = 0;
    size_t digits = 0;
    while (*pos < n) {
      int digit = c_digit_value(s[*pos]);
      if (digit < 0 || digit >= 16)
        break;
      value = value * 16u + (size_t)digit;
      (*pos)++;
      digits++;
    }
    if (digits == 0 || value > 255)
      return 0;
    *out = value;
    return 1;
  }
  default:
    if (c >= '0' && c <= '7') {
      size_t value = (size_t)(c - '0');
      unsigned digits = 1;
      while (*pos < n && digits < 3 && s[*pos] >= '0' && s[*pos] <= '7') {
        value = value * 8u + (size_t)(s[*pos] - '0');
        (*pos)++;
        digits++;
      }
      if (value > 255)
        return 0;
      *out = value;
      return 1;
    }
    *out = (unsigned char)c;
    return 1;
  }
}

static int c_parse_char_literal_slice(const char *s, size_t n, size_t *pos,
                                      size_t *out) {
  if (!s || !pos || !out || *pos >= n || s[*pos] != '\'')
    return 0;
  size_t i = *pos + 1;
  size_t value = 0;
  if (i >= n)
    return 0;
  if (s[i] == '\\') {
    i++;
    if (!c_parse_char_escape(s, n, &i, &value))
      return 0;
  } else {
    if (s[i] == '\'')
      return 0;
    value = (unsigned char)s[i++];
  }
  if (i >= n || s[i] != '\'')
    return 0;
  *pos = i + 1;
  *out = value;
  return 1;
}

static int c_line_continuation_at(const char *s, size_t n, size_t i,
                                  size_t *advance) {
  if (!s || i >= n || s[i] != '\\')
    return 0;
  if (i + 1 < n && s[i + 1] == '\n') {
    if (advance)
      *advance = 2;
    return 1;
  }
  if (i + 2 < n && s[i + 1] == '\r' && s[i + 2] == '\n') {
    if (advance)
      *advance = 3;
    return 1;
  }
  return 0;
}

static void macro_skip_ws(const char *s, size_t n, size_t *i) {
  for (;;) {
    while (*i < n && (s[*i] == ' ' || s[*i] == '\t' || s[*i] == '\r'))
      (*i)++;
    size_t advance = 0;
    if (!c_line_continuation_at(s, n, *i, &advance))
      break;
    *i += advance;
  }
}

static int macro_lookup_name(ny_parser_t *p, const char *name, size_t name_len,
                             int64_t *out) {
  if (!p || !name || name_len == 0 || !out)
    return 0;
  for (unsigned i = p->define_count; i > 0; --i) {
    unsigned idx = i - 1;
    if (ctok_is_ident_slice(p->define_names[idx], name, name_len)) {
      *out = p->define_values[idx];
      return 1;
    }
  }
  return 0;
}

static int macro_parse_expr(ny_parser_t *p, const char *s, size_t n, size_t *i,
                            int64_t *out);
static int macro_parse_expr_impl(ny_parser_t *p, const char *s, size_t n,
                                 size_t *i, int64_t *out);

static int macro_parse_primary(ny_parser_t *p, const char *s, size_t n,
                               size_t *i, int64_t *out) {
  macro_skip_ws(s, n, i);
  if (*i >= n)
    return 0;
  if (s[*i] == '!' || s[*i] == '~' || s[*i] == '+') {
    char op = s[(*i)++];
    int64_t value = 0;
    if (!macro_parse_primary(p, s, n, i, &value))
      return 0;
    *out = op == '!' ? !value : op == '~' ? (int64_t)(~(uint64_t)value) : value;
    return 1;
  }
  if (s[*i] == '-') {
    int64_t value = 0;
    (*i)++;
    if (!macro_parse_primary(p, s, n, i, &value))
      return 0;
    *out = -value;
    return 1;
  }
  if (s[*i] == '(') {
    (*i)++;
    if (!macro_parse_expr(p, s, n, i, out))
      return 0;
    macro_skip_ws(s, n, i);
    if (*i >= n || s[*i] != ')')
      return 0;
    (*i)++;
    return 1;
  }
  if (s[*i] >= '0' && s[*i] <= '9') {
    int64_t value = 0;
    if (!c_parse_integer_slice(s, n, i, &value))
      return 0;
    *out = value;
    return 1;
  }
  if (s[*i] == '\'') {
    size_t ch = 0;
    if (!c_parse_char_literal_slice(s, n, i, &ch))
      return 0;
    *out = (int64_t)ch;
    return 1;
  }
  if (c_ident_start_char(s[*i])) {
    size_t name = *i;
    while (*i < n && c_ident_char(s[*i]))
      (*i)++;
    if (*i - name == 7 && strncmp(s + name, "defined", 7) == 0) {
      macro_skip_ws(s, n, i);
      int paren = *i < n && s[*i] == '(';
      if (paren) {
        (*i)++;
        macro_skip_ws(s, n, i);
      }
      if (*i >= n || !c_ident_start_char(s[*i]))
        return 0;
      size_t def_name = *i;
      while (*i < n && c_ident_char(s[*i]))
        (*i)++;
      size_t def_name_len = *i - def_name;
      if (paren) {
        macro_skip_ws(s, n, i);
        if (*i >= n || s[*i] != ')')
          return 0;
        (*i)++;
      }
      int64_t value = 0;
      *out = macro_lookup_name(p, s + def_name, def_name_len, &value) ? 1 : 0;
      return 1;
    }
    size_t name_len = *i - name;
    if ((name_len == 12 && strncmp(s + name, "__has_include", 12) == 0) ||
        (name_len == 13 && strncmp(s + name, "__has_builtin", 13) == 0) ||
        (name_len == 12 && strncmp(s + name, "__has_feature", 12) == 0) ||
        (name_len == 14 && strncmp(s + name, "__has_attribute", 14) == 0) ||
        (name_len == 12 && strncmp(s + name, "__has_warning", 12) == 0)) {
      macro_skip_ws(s, n, i);
      if (*i < n && s[*i] == '(') {
        int depth = 1;
        (*i)++;
        while (*i < n && depth > 0) {
          if (s[*i] == '(') depth++;
          else if (s[*i] == ')') depth--;
          (*i)++;
        }
      }
      *out = 0;
      return 1;
    }
    if (name_len == 19 && strncmp(s + name, "__builtin_offsetof", 19) == 0) {
      macro_skip_ws(s, n, i);
      if (*i < n && s[*i] == '(') {
        int depth = 1;
        (*i)++;
        while (*i < n && depth > 0) {
          if (s[*i] == '(') depth++;
          else if (s[*i] == ')') depth--;
          (*i)++;
        }
      }
      *out = 0;
      return 1;
    }
    if (name_len == 24 && strncmp(s + name, "__builtin_choose_expr", 24) == 0) {
      macro_skip_ws(s, n, i);
      if (*i < n && s[*i] == '(') {
        int depth = 1;
        (*i)++;
        while (*i < n && depth > 0) {
          if (s[*i] == '(') depth++;
          else if (s[*i] == ')') depth--;
          (*i)++;
        }
      }
      *out = 0;
      return 1;
    }
    return macro_lookup_name(p, s + name, *i - name, out);
  }
  return 0;
}

static int macro_parse_term(ny_parser_t *p, const char *s, size_t n, size_t *i,
                            int64_t *out) {
  int64_t lhs = 0;
  if (!macro_parse_primary(p, s, n, i, &lhs))
    return 0;
  for (;;) {
    macro_skip_ws(s, n, i);
    if (*i >= n || (s[*i] != '*' && s[*i] != '/' && s[*i] != '%'))
      break;
    char op = s[(*i)++];
    int64_t rhs = 0;
    if (!macro_parse_primary(p, s, n, i, &rhs))
      return 0;
    if (op == '*') {
      lhs *= rhs;
    } else if (op == '/') {
      if (rhs == 0)
        return 0;
      lhs /= rhs;
    } else {
      if (rhs == 0)
        return 0;
      lhs %= rhs;
    }
  }
  *out = lhs;
  return 1;
}

static int macro_parse_add(ny_parser_t *p, const char *s, size_t n, size_t *i,
                           int64_t *out) {
  int64_t lhs = 0;
  if (!macro_parse_term(p, s, n, i, &lhs))
    return 0;
  for (;;) {
    macro_skip_ws(s, n, i);
    if (*i >= n || (s[*i] != '+' && s[*i] != '-'))
      break;
    char op = s[(*i)++];
    int64_t rhs = 0;
    if (!macro_parse_term(p, s, n, i, &rhs))
      return 0;
    if (op == '+') {
      lhs += rhs;
    } else {
      lhs -= rhs;
    }
  }
  *out = lhs;
  return 1;
}

static int macro_parse_shift(ny_parser_t *p, const char *s, size_t n,
                             size_t *i, int64_t *out) {
  int64_t lhs = 0;
  if (!macro_parse_add(p, s, n, i, &lhs))
    return 0;
  for (;;) {
    macro_skip_ws(s, n, i);
    if (*i + 1 >= n ||
        !((s[*i] == '<' && s[*i + 1] == '<') ||
          (s[*i] == '>' && s[*i + 1] == '>')))
      break;
    int is_left = s[*i] == '<';
    *i += 2;
    int64_t rhs = 0;
    if (!macro_parse_add(p, s, n, i, &rhs) || rhs < 0 || rhs >= 64)
      return 0;
    if (is_left) {
      lhs <<= rhs;
    } else {
      lhs >>= rhs;
    }
  }
  *out = lhs;
  return 1;
}

static int macro_parse_bitand(ny_parser_t *p, const char *s, size_t n,
                              size_t *i, int64_t *out) {
  int64_t lhs = 0;
  if (!macro_parse_shift(p, s, n, i, &lhs))
    return 0;
  for (;;) {
    macro_skip_ws(s, n, i);
    if (*i >= n || (s[*i] != '<' && s[*i] != '>'))
      break;
    char op = s[(*i)++];
    int with_eq = *i < n && s[*i] == '=';
    if (with_eq)
      (*i)++;
    int64_t rhs = 0;
    if (!macro_parse_shift(p, s, n, i, &rhs))
      return 0;
    lhs = op == '<' ? (with_eq ? lhs <= rhs : lhs < rhs)
                    : (with_eq ? lhs >= rhs : lhs > rhs);
  }
  for (;;) {
    macro_skip_ws(s, n, i);
    if (*i + 1 >= n ||
        !((s[*i] == '=' && s[*i + 1] == '=') ||
          (s[*i] == '!' && s[*i + 1] == '=')))
      break;
    int is_eq = s[*i] == '=';
    *i += 2;
    int64_t rhs = 0;
    if (!macro_parse_shift(p, s, n, i, &rhs))
      return 0;
    lhs = is_eq ? lhs == rhs : lhs != rhs;
  }
  for (;;) {
    macro_skip_ws(s, n, i);
    if (*i >= n || s[*i] != '&' ||
        (*i + 1 < n && s[*i + 1] == '&'))
      break;
    (*i)++;
    int64_t rhs = 0;
    if (!macro_parse_shift(p, s, n, i, &rhs))
      return 0;
    lhs &= rhs;
  }
  *out = lhs;
  return 1;
}

static int macro_parse_bitxor(ny_parser_t *p, const char *s, size_t n,
                              size_t *i, int64_t *out) {
  int64_t lhs = 0;
  if (!macro_parse_bitand(p, s, n, i, &lhs))
    return 0;
  for (;;) {
    macro_skip_ws(s, n, i);
    if (*i >= n || s[*i] != '^')
      break;
    (*i)++;
    int64_t rhs = 0;
    if (!macro_parse_bitand(p, s, n, i, &rhs))
      return 0;
    lhs ^= rhs;
  }
  *out = lhs;
  return 1;
}

static int macro_parse_bitor(ny_parser_t *p, const char *s, size_t n,
                             size_t *i, int64_t *out) {
  int64_t lhs = 0;
  if (!macro_parse_bitxor(p, s, n, i, &lhs))
    return 0;
  for (;;) {
    macro_skip_ws(s, n, i);
    if (*i >= n || s[*i] != '|' ||
        (*i + 1 < n && s[*i + 1] == '|'))
      break;
    (*i)++;
    int64_t rhs = 0;
    if (!macro_parse_bitxor(p, s, n, i, &rhs))
      return 0;
    lhs |= rhs;
  }
  *out = lhs;
  return 1;
}

static int macro_parse_logand(ny_parser_t *p, const char *s, size_t n,
                              size_t *i, int64_t *out) {
  int64_t lhs = 0;
  if (!macro_parse_bitor(p, s, n, i, &lhs))
    return 0;
  for (;;) {
    macro_skip_ws(s, n, i);
    if (*i + 1 >= n || s[*i] != '&' || s[*i + 1] != '&')
      break;
    *i += 2;
    int64_t rhs = 0;
    if (!macro_parse_bitor(p, s, n, i, &rhs))
      return 0;
    lhs = lhs && rhs;
  }
  *out = lhs;
  return 1;
}

static int macro_parse_logor(ny_parser_t *p, const char *s, size_t n,
                             size_t *i, int64_t *out) {
  int64_t lhs = 0;
  if (!macro_parse_logand(p, s, n, i, &lhs))
    return 0;
  for (;;) {
    macro_skip_ws(s, n, i);
    if (*i + 1 >= n || s[*i] != '|' || s[*i + 1] != '|')
      break;
    *i += 2;
    int64_t rhs = 0;
    if (!macro_parse_logand(p, s, n, i, &rhs))
      return 0;
    lhs = lhs || rhs;
  }
  *out = lhs;
  return 1;
}

static int macro_parse_expr_impl(ny_parser_t *p, const char *s, size_t n,
                                 size_t *i, int64_t *out) {
  int64_t cond = 0;
  if (!macro_parse_logor(p, s, n, i, &cond))
    return 0;
  macro_skip_ws(s, n, i);
  if (*i >= n || s[*i] != '?') {
    *out = cond;
    return 1;
  }
  (*i)++;
  int64_t when_true = 0;
  int64_t when_false = 0;
  if (!macro_parse_expr(p, s, n, i, &when_true))
    return 0;
  macro_skip_ws(s, n, i);
  if (*i >= n || s[*i] != ':')
    return 0;
  (*i)++;
  if (!macro_parse_expr(p, s, n, i, &when_false))
    return 0;
  *out = cond ? when_true : when_false;
  return 1;
}

static int macro_parse_expr(ny_parser_t *p, const char *s, size_t n, size_t *i,
                            int64_t *out) {
  if (!parser_recursion_enter(p, &p->macro_expr_depth, "macro expression"))
    return 0;
  int ok = macro_parse_expr_impl(p, s, n, i, out);
  p->macro_expr_depth--;
  return ok;
}

static int parser_eval_define_value(ny_parser_t *p, const char *s, size_t n,
                                    size_t start, int64_t *out) {
  size_t i = start;
  if (!macro_parse_expr(p, s, n, &i, out))
    return 0;
  macro_skip_ws(s, n, &i);
  return i >= n || s[i] == '\n';
}

static int preproc_word_is(const char *s, size_t n, size_t pos,
                           const char *word) {
  size_t w = 0;
  while (word[w]) {
    if (pos + w >= n || s[pos + w] != word[w])
      return 0;
    w++;
  }
  if (pos + w < n && c_ident_char(s[pos + w]))
    return 0;
  return 1;
}

static void preproc_skip_ws(const char *s, size_t n, size_t *i) {
  while (*i < n && (s[*i] == ' ' || s[*i] == '\t' || s[*i] == '\r'))
    (*i)++;
}

static void parser_pack_push(ny_parser_t *p, unsigned align) {
  if (!p)
    return;
  if (p->pack_depth >= NY_C_MAX_PACK_STACK) {
    if (!p->fatal_error) {
      snprintf(p->error, sizeof(p->error),
               "pack stack overflow (%u max)", NY_C_MAX_PACK_STACK);
      p->fatal_error = 1;
    }
    return;
  }
  p->pack_stack[p->pack_depth++] = p->pack_align;
  p->pack_align = align;
}

static void parser_pack_pop(ny_parser_t *p) {
  if (!p)
    return;
  if (p->pack_depth > 0)
    p->pack_align = p->pack_stack[--p->pack_depth];
  else
    p->pack_align = 0;
}

static unsigned parser_parse_pack_align(const char *s, size_t n, size_t *i) {
  if (!s || !i || *i >= n || s[*i] < '0' || s[*i] > '9')
    return 0;
  size_t value = 0;
  while (*i < n && s[*i] >= '0' && s[*i] <= '9') {
    size_t next = value * 10u + (size_t)(s[*i] - '0');
    if (next < value)
      return 0;
    value = next;
    (*i)++;
  }
  switch (value) {
  case 0:
  case 1:
  case 2:
  case 4:
  case 8:
  case 16:
    return (unsigned)value;
  default:
    return 0;
  }
}

static void parser_note_pragma_pack(ny_parser_t *p, const char *s, size_t n,
                                    size_t i) {
  if (!p || !s)
    return;
  preproc_skip_ws(s, n, &i);
  if (!preproc_word_is(s, n, i, "pack"))
    return;
  i += 4;
  preproc_skip_ws(s, n, &i);
  if (i >= n || s[i] != '(')
    return;
  i++;
  preproc_skip_ws(s, n, &i);
  if (preproc_word_is(s, n, i, "push")) {
    i += 4;
    preproc_skip_ws(s, n, &i);
    if (i < n && s[i] == ',') {
      i++;
      preproc_skip_ws(s, n, &i);
      parser_pack_push(p, parser_parse_pack_align(s, n, &i));
    } else {
      parser_pack_push(p, p->pack_align);
    }
    return;
  }
  if (preproc_word_is(s, n, i, "pop")) {
    parser_pack_pop(p);
    return;
  }
  if (preproc_word_is(s, n, i, "default")) {
    p->pack_align = 0;
    return;
  }
  if (i < n && s[i] >= '0' && s[i] <= '9')
    p->pack_align = parser_parse_pack_align(s, n, &i);
}

static int parser_preproc_active(ny_parser_t *p) {
  return !p || p->pp_depth == 0 || p->pp_active[p->pp_depth - 1] != 0;
}

static int parser_preproc_eval_condition(ny_parser_t *p, const char *s,
                                         size_t n, size_t i, int64_t *out) {
  if (!p || !s || !out || i > n)
    return 0;
  if (!macro_parse_expr(p, s, n, &i, out))
    return 0;
  macro_skip_ws(s, n, &i);
  return i >= n;
}

static int parser_preproc_ident_defined(ny_parser_t *p, const char *s,
                                        size_t n, size_t i) {
  preproc_skip_ws(s, n, &i);
  if (i >= n || !c_ident_start_char(s[i]))
    return 0;
  size_t name = i;
  while (i < n && c_ident_char(s[i]))
    i++;
  int64_t value = 0;
  return macro_lookup_name(p, s + name, i - name, &value);
}

static void parser_push_preproc_cond(ny_parser_t *p, int active) {
  if (!p || p->pp_depth >= NY_C_MAX_COND_STACK) {
    if (p && p->pp_depth >= NY_C_MAX_COND_STACK && !p->fatal_error) {
      snprintf(p->error, sizeof(p->error),
               "preprocessor condition stack overflow (%u max)", NY_C_MAX_COND_STACK);
      p->fatal_error = 1;
    }
    return;
  }
  unsigned parent = (unsigned)parser_preproc_active(p);
  unsigned branch_active = parent && active;
  unsigned idx = p->pp_depth++;
  p->pp_parent_active[idx] = parent;
  p->pp_active[idx] = branch_active;
  p->pp_branch_taken[idx] = branch_active;
}

static void parser_note_conditional_preproc(ny_parser_t *p, const char *s,
                                            size_t n, const char *word,
                                            size_t word_len, size_t i) {
  if (!p || !s || !word)
    return;
  if (word_len == 2 && strncmp(word, "if", 2) == 0) {
    int64_t value = 0;
    parser_push_preproc_cond(p,
                             parser_preproc_eval_condition(p, s, n, i, &value)
                                 ? value != 0
                                 : 0);
  } else if (word_len == 5 && strncmp(word, "ifdef", 5) == 0) {
    parser_push_preproc_cond(p, parser_preproc_ident_defined(p, s, n, i));
  } else if (word_len == 6 && strncmp(word, "ifndef", 6) == 0) {
    parser_push_preproc_cond(p, !parser_preproc_ident_defined(p, s, n, i));
  } else if (word_len == 4 && strncmp(word, "elif", 4) == 0) {
    if (p->pp_depth == 0)
      return;
    unsigned idx = p->pp_depth - 1;
    int64_t value = 0;
    unsigned active = 0;
    if (p->pp_parent_active[idx] && !p->pp_branch_taken[idx] &&
        parser_preproc_eval_condition(p, s, n, i, &value) && value)
      active = 1;
    p->pp_active[idx] = active;
    if (active)
      p->pp_branch_taken[idx] = 1;
  } else if (word_len == 4 && strncmp(word, "else", 4) == 0) {
    if (p->pp_depth == 0)
      return;
    unsigned idx = p->pp_depth - 1;
    unsigned active = p->pp_parent_active[idx] && !p->pp_branch_taken[idx];
    p->pp_active[idx] = active;
    p->pp_branch_taken[idx] = 1;
  } else if (word_len == 5 && strncmp(word, "endif", 5) == 0) {
    if (p->pp_depth > 0)
      p->pp_depth--;
  }
}

static void parser_note_preproc(ny_parser_t *p, ny_ctok_t tok) {
  if (!p || tok.kind != NY_CTOK_PREPROC || !tok.start || tok.len == 0)
    return;
  const char *s = tok.start;
  size_t n = tok.len;
  size_t i = 0;
  while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r'))
    i++;
  if (i >= n || s[i] != '#')
    return;
  i++;
  while (i < n && (s[i] == ' ' || s[i] == '\t'))
    i++;
  size_t word = i;
  while (i < n && c_ident_char(s[i]))
    i++;
  size_t word_len = i - word;
  while (i < n && (s[i] == ' ' || s[i] == '\t'))
    i++;
  if ((word_len == 2 && strncmp(s + word, "if", 2) == 0) ||
      (word_len == 5 && strncmp(s + word, "ifdef", 5) == 0) ||
      (word_len == 6 && strncmp(s + word, "ifndef", 6) == 0) ||
      (word_len == 4 && strncmp(s + word, "elif", 4) == 0) ||
      (word_len == 4 && strncmp(s + word, "else", 4) == 0) ||
      (word_len == 5 && strncmp(s + word, "endif", 5) == 0)) {
    parser_note_conditional_preproc(p, s, n, s + word, word_len, i);
    return;
  }
  if (!parser_preproc_active(p))
    return;
  if (word_len == 7 && strncmp(s + word, "include", 7) == 0) {
    if (p && p->include_read && p->include_depth < NY_C_MAX_INCLUDE_DEPTH) {
      const char *path_start = s + i;
      size_t path_len = n - i;
      bool is_sys = false;
      if (path_len > 0 && path_start[0] == '<') {
        is_sys = true;
        path_start++;
        path_len--;
        while (path_len > 0 && path_start[path_len - 1] != '>')
          path_len--;
        if (path_len > 0)
          path_len--;
      } else if (path_len > 0 && path_start[0] == '"') {
        path_start++;
        path_len--;
        while (path_len > 0 && path_start[path_len - 1] != '"')
          path_len--;
        if (path_len > 0)
          path_len--;
      }
      if (path_len > 0) {
        char path_buf[4096];
        if (!is_sys && p->source_dir && path_start[0] != '/') {
          snprintf(path_buf, sizeof(path_buf), "%s/%.*s",
                   p->source_dir, (int)path_len, path_start);
        } else {
          snprintf(path_buf, sizeof(path_buf), "%.*s", (int)path_len, path_start);
        }
        char *inc_src = p->include_read(path_buf, is_sys, p->include_userdata);
        if (inc_src) {
          /* Skip large transitive includes — let libclang handle them.
           * The nytrix frontend is too slow for deep include trees. */
          size_t inc_size = strlen(inc_src);
          if (inc_size > 32768) {
            free(inc_src);
            goto skip_include;
          }
          ny_parser_t inc_p;
          ny_parse_init_abi(&inc_p, inc_src, inc_size, p->abi);
          inc_p.deadline_ns = p->deadline_ns;
          inc_p.token_limit = p->token_limit;
          inc_p.token_count = p->token_count;
          inc_p.include_read = p->include_read;
          inc_p.include_userdata = p->include_userdata;
          inc_p.source_file = path_buf;
          const char *slash = strrchr(path_buf, '/');
          if (slash) {
            static char dir_buf[4096];
            size_t dlen = (size_t)(slash - path_buf);
            if (dlen < sizeof(dir_buf)) {
              memcpy(dir_buf, path_buf, dlen);
              dir_buf[dlen] = '\0';
              inc_p.source_dir = dir_buf;
            }
          }
          inc_p.include_depth = p->include_depth + 1;
          while (inc_p.tok.kind != NY_CTOK_EOF) {
            ny_cdecl_t inc_decl;
            int rc = ny_parse_decl(&inc_p, &inc_decl);
            if (rc > 0) {
              if (inc_decl.kind == NY_CDECL_TYPEDEF &&
                  inc_decl.name.kind == NY_CTOK_IDENT &&
                  p->typedef_count < NY_C_MAX_TYPEDEFS) {
                parser_note_typedef(p, ctok_intern(p, inc_decl.name),
                                    &inc_decl.type);
              }
              if ((inc_decl.type.kind == NY_CTYPE_STRUCT ||
                   inc_decl.type.kind == NY_CTYPE_UNION ||
                   inc_decl.type.kind == NY_CTYPE_ENUM) &&
                  inc_decl.type.name.kind == NY_CTOK_IDENT &&
                  p->tag_count < NY_C_MAX_TAGS) {
                parser_note_tag(p, ctok_intern(p, inc_decl.type.name),
                                &inc_decl.type);
              }
              if (inc_decl.type.aggregate_has_layout &&
                  p->tag_count < NY_C_MAX_TAGS &&
                  inc_decl.type.name.kind == NY_CTOK_IDENT) {
                parser_note_tag(p, ctok_intern(p, inc_decl.type.name),
                                &inc_decl.type);
              }
            } else if (rc < 0) {
              continue;
            } else {
              break;
            }
          }
          free(inc_src);
          p->token_count = inc_p.token_count;
        skip_include:;
        }
      }
    }
    return;
  }
  if (word_len == 6 && strncmp(s + word, "define", 6) == 0) {
    if (i >= n || !c_ident_start_char(s[i]))
      return;
    size_t name = i;
    while (i < n && c_ident_char(s[i]))
      i++;
    size_t name_len = i - name;
    if (i < n && s[i] == '(') {
      i++;
      const char *params_start = s + i;
      size_t params_len = 0;
      int depth = 1;
      while (i < n && depth > 0) {
        if (s[i] == '(')
          depth++;
        else if (s[i] == ')')
          depth--;
        if (depth > 0) {
          params_len++;
          i++;
        } else {
          i++;
          break;
        }
      }
      int is_variadic = 0;
      if (params_len > 2) {
        const char *pp = params_start + params_len - 1;
        while (pp > params_start && (*pp == ' ' || *pp == '\t'))
          pp--;
        if (pp >= params_start + 2 && pp[-1] == '.' && pp[-2] == '.') {
          is_variadic = 1;
          while (params_len > 0 && (params_start[params_len - 1] == ' ' ||
                                     params_start[params_len - 1] == '\t' ||
                                     params_start[params_len - 1] == '.')) {
            params_len--;
          }
        }
      }
      while (i < n && (s[i] == ' ' || s[i] == '\t'))
        i++;
      const char *body_start = s + i;
      size_t body_len = (i < n) ? (n - i) : 0;
      while (body_len > 0 &&
             (body_start[body_len - 1] == ' ' || body_start[body_len - 1] == '\t'))
        body_len--;
      parser_note_func_macro(p, s + name, name_len, params_start, params_len,
                             body_start, body_len, is_variadic);
      return;
    }
    while (i < n && (s[i] == ' ' || s[i] == '\t'))
      i++;
    int64_t value = 0;
    if (parser_eval_define_value(p, s, n, i, &value)) {
      parser_note_define(p, s + name, name_len, value);
    } else {
      /* Non-integer define: store the body as a string define if it's a
       * storage-class keyword or a simple identifier. This handles macros
       * like `#define CURL_EXTERN extern` or `#define CURL_EXTERN static`. */
      const char *body = s + i;
      size_t body_len = n - i;
      while (body_len > 0 && (body[body_len - 1] == ' ' ||
                               body[body_len - 1] == '\t' ||
                               body[body_len - 1] == '\n' ||
                               body[body_len - 1] == '\r'))
        body_len--;
      if (body_len > 0 && body_len < 64) {
        parser_note_str_define(p, s + name, name_len, body, body_len);
      }
    }
  } else if (word_len == 5 && strncmp(s + word, "undef", 5) == 0) {
    if (i >= n || !c_ident_start_char(s[i]))
      return;
    size_t name = i;
    while (i < n && c_ident_char(s[i]))
      i++;
    parser_forget_define(p, s + name, i - name);
    parser_forget_func_macro(p, s + name, i - name);
  } else if (word_len == 6 && strncmp(s + word, "pragma", 6) == 0) {
    parser_note_pragma_pack(p, s, n, i);
  }
}

#define NY_MACRO_MAX_ARGS 8
#define NY_MACRO_MAX_ARG_LEN 256
#define NY_MACRO_MAX_EXPANDED 2048

static int parse_func_macro_args(ny_parser_t *p, char args[][NY_MACRO_MAX_ARG_LEN],
                                  unsigned *arg_count, int *has_parens) {
  if (!parse_is(p, "("))
    return 0;
  *has_parens = 1;
  parse_advance(p);
  *arg_count = 0;
  int depth = 1;
  char buf[NY_MACRO_MAX_ARG_LEN];
  size_t bpos = 0;
  while (p->tok.kind != NY_CTOK_EOF && depth > 0) {
    if (parse_is(p, "(")) {
      depth++;
      if (bpos < NY_MACRO_MAX_ARG_LEN - 1)
        buf[bpos++] = '(';
      parse_advance(p);
    } else if (parse_is(p, ")")) {
      depth--;
      if (depth > 0) {
        if (bpos < NY_MACRO_MAX_ARG_LEN - 1)
          buf[bpos++] = ')';
        parse_advance(p);
      } else {
        parse_advance(p);
      }
    } else if (parse_is(p, ",") && depth == 1) {
      buf[bpos] = '\0';
      while (bpos > 0 && (buf[bpos - 1] == ' ' || buf[bpos - 1] == '\t'))
        bpos--;
      buf[bpos] = '\0';
      if (*arg_count < NY_MACRO_MAX_ARGS) {
        memcpy(args[*arg_count], buf, bpos + 1);
        (*arg_count)++;
      }
      bpos = 0;
      parse_advance(p);
    } else {
      const char *start = p->tok.start;
      size_t len = p->tok.len;
      for (size_t k = 0; k < len && bpos < NY_MACRO_MAX_ARG_LEN - 1; k++)
        buf[bpos++] = start[k];
      parse_advance(p);
    }
  }
  if (bpos > 0 && *arg_count < NY_MACRO_MAX_ARGS) {
    buf[bpos] = '\0';
    while (bpos > 0 && (buf[bpos - 1] == ' ' || buf[bpos - 1] == '\t'))
      bpos--;
    buf[bpos] = '\0';
    memcpy(args[*arg_count], buf, bpos + 1);
    (*arg_count)++;
  }
  return 1;
}

static int expand_func_macro(ny_parser_t *p, unsigned macro_idx) {
  char args[NY_MACRO_MAX_ARGS][NY_MACRO_MAX_ARG_LEN];
  unsigned arg_count = 0;
  int has_parens = 0;
  ny_parser_bookmark_t save = parser_bookmark(p);
  if (!parse_func_macro_args(p, args, &arg_count, &has_parens)) {
    parser_rewind(p, save);
    return 0;
  }
  const char *body = p->func_macro_bodies[macro_idx];
  unsigned pcount = p->func_macro_param_count[macro_idx];
  int is_variadic = p->func_macro_is_variadic[macro_idx];
  if (!body || body[0] == '\0') {
    p->tok = cempty_tok();
    return 1;
  }
  char expanded[NY_MACRO_MAX_EXPANDED];
  size_t epos = 0;
  const char *bs = body;
  while (*bs && epos < NY_MACRO_MAX_EXPANDED - 1) {
    if (c_ident_start_char(*bs)) {
      const char *ident_start = bs;
      while (c_ident_char(*bs))
        bs++;
      size_t ilen = (size_t)(bs - ident_start);
      int found = 0;
      for (unsigned pi = 0; pi < pcount; pi++) {
        size_t plen = strlen(p->func_macro_params[macro_idx][pi]);
        if (ilen == plen &&
            memcmp(ident_start, p->func_macro_params[macro_idx][pi], ilen) == 0) {
          if (pi < arg_count) {
            const char *arg = args[pi];
            while (*arg && epos < NY_MACRO_MAX_EXPANDED - 1)
              expanded[epos++] = *arg++;
          }
          found = 1;
          break;
        }
      }
      if (!found) {
        while (ident_start < bs && epos < NY_MACRO_MAX_EXPANDED - 1)
          expanded[epos++] = *ident_start++;
      }
    } else if (is_variadic && *bs == '_' && strncmp(bs, "__VA_ARGS__", 11) == 0) {
      bs += 11;
      if (pcount < arg_count) {
        for (unsigned ai = pcount; ai < arg_count; ai++) {
          if (ai > pcount && epos < NY_MACRO_MAX_EXPANDED - 2) {
            expanded[epos++] = ',';
            expanded[epos++] = ' ';
          }
          const char *arg = args[ai];
          while (*arg && epos < NY_MACRO_MAX_EXPANDED - 1)
            expanded[epos++] = *arg++;
        }
      }
    } else {
      expanded[epos++] = *bs;
      bs++;
    }
  }
  expanded[epos] = '\0';
  const char *interned = parser_intern(p, expanded, epos);
  ny_lex_init(&p->lx, interned, epos);
  p->tok = ny_lex_next(&p->lx);
  return 1;
}

static void skip_preproc(ny_parser_t *p) {
  for (;;) {
    while (p->tok.kind == NY_CTOK_PREPROC) {
      parser_note_preproc(p, p->tok);
      parse_advance(p);
    }
    if (parser_preproc_active(p) || p->tok.kind == NY_CTOK_EOF)
      break;
    parse_advance(p);
  }
}

static int type_qual(ny_parser_t *p, ny_ctype_t *ty) {
  if (parse_kw(p, "const") || parse_kw(p, "__const") || parse_kw(p, "__const__")) {
    ty->flags |= NY_CTYPEF_CONST;
    parse_advance(p);
    return 1;
  }
  if (parse_kw(p, "volatile") || parse_kw(p, "__volatile") || parse_kw(p, "__volatile__")) {
    ty->flags |= NY_CTYPEF_VOLATILE;
    parse_advance(p);
    return 1;
  }
  if (parse_kw(p, "restrict") || parse_kw(p, "__restrict") || parse_kw(p, "__restrict__")) {
    parse_advance(p);
    return 1;
  }
  if (parse_kw(p, "_Atomic")) {
    parse_advance(p);
    if (parse_is(p, "("))
      skip_balanced(p, "(", ")");
    return 1;
  }
  if (parse_kw(p, "_Complex") || parse_kw(p, "__complex__") ||
      parse_kw(p, "complex")) {
    ty->flags |= NY_CTYPEF_COMPLEX;
    parse_advance(p);
    return 1;
  }
  if (parse_kw(p, "_Imaginary") || parse_kw(p, "__imaginary__") ||
      parse_kw(p, "imaginary")) {
    ty->flags |= NY_CTYPEF_IMAGINARY;
    parse_advance(p);
    return 1;
  }
  return 0;
}

static int parse_storage(ny_parser_t *p, ny_cdecl_t *decl) {
  /* Expand string macros for storage-class keywords (e.g. CURL_EXTERN). */
  if (p->tok.kind == NY_CTOK_IDENT) {
    const char *macro = parser_lookup_str_define(p, p->tok);
    if (macro) {
      if (strcmp(macro, "extern") == 0) {
        decl->flags |= NY_CDECLF_EXTERN;
        parse_advance(p);
        return 1;
      }
      if (strcmp(macro, "static") == 0) {
        decl->flags |= NY_CDECLF_STATIC;
        parse_advance(p);
        return 1;
      }
      if (strcmp(macro, "inline") == 0 || strcmp(macro, "__inline") == 0 ||
          strcmp(macro, "__inline__") == 0) {
        decl->flags |= NY_CDECLF_INLINE;
        parse_advance(p);
        return 1;
      }
      /* Other string macros (e.g. __declspec(dllexport)): skip and continue. */
      parse_advance(p);
      return 1;
    }
  }
  if (parse_kw(p, "typedef")) {
    decl->kind = NY_CDECL_TYPEDEF;
    parse_advance(p);
    return 1;
  }
  if (parse_kw(p, "extern")) {
    decl->flags |= NY_CDECLF_EXTERN;
    parse_advance(p);
    return 1;
  }
  if (parse_kw(p, "static")) {
    decl->flags |= NY_CDECLF_STATIC;
    parse_advance(p);
    return 1;
  }
  if (parse_kw(p, "inline") || parse_kw(p, "__inline") || parse_kw(p, "__inline__")) {
    decl->flags |= NY_CDECLF_INLINE;
    parse_advance(p);
    return 1;
  }
  if (parse_kw(p, "auto") || parse_kw(p, "register") || parse_kw(p, "_Thread_local")) {
    parse_advance(p);
    return 1;
  }
  if (parse_kw(p, "_Noreturn") || parse_kw(p, "noreturn") ||
      parse_kw(p, "__noreturn") || parse_kw(p, "__noreturn__")) {
    decl->flags |= NY_CDECLF_NORETURN;
    parse_advance(p);
    return 1;
  }
  if (parse_decl_marker(p))
    return 1;
  return 0;
}

static int parse_enum_body(ny_parser_t *p, ny_ctype_t *ty) {
  if (!parse_accept(p, "{"))
    return 1;
  size_t next_value = 0;
  size_t max_value = 0;
  int has_negative = 0;
  while (p->tok.kind != NY_CTOK_EOF && !parse_is(p, "}")) {
    if (p->tok.kind != NY_CTOK_IDENT) {
      skip_balanced(p, "{", "}");
      return 1;
    }
    ny_ctok_t name = p->tok;
    size_t value = next_value;
    parse_advance(p);
    if (parse_accept(p, "=")) {
      if (!parse_array_extent_expr(p, &value)) {
        skip_balanced(p, "{", "}");
        return 1;
      }
      /* Check if the value is negative (was written as a negative literal). */
      if (p->tok.start && p->tok.start > name.start && p->tok.start[-1] == '-')
        has_negative = 1;
    }
    if (value > max_value)
      max_value = value;
    parser_note_define(p, name.start, name.len, value);
    next_value = value < ((size_t)-1) ? value + 1u : value;
    if (!parse_accept(p, ","))
      break;
  }
  if (!parse_accept(p, "}"))
    return parse_errorf(p, "expected '}' after C enum body at %u:%u",
                        p->tok.line, p->tok.col);
  /* Determine enum underlying type from max value. C enums are int by default,
   * but can be unsigned int, long, or unsigned long for large values. */
  if (ty && ty->kind == NY_CTYPE_ENUM) {
    if (has_negative || max_value <= 0x7FFFFFFF)
      ty->enum_underlying = 0; /* int */
    else if (max_value <= 0xFFFFFFFF)
      ty->enum_underlying = 1; /* unsigned int */
    else if (max_value <= 0x7FFFFFFFFFFFFFFF)
      ty->enum_underlying = 2; /* long */
    else
      ty->enum_underlying = 3; /* unsigned long */
  }
  return 1;
}

static int parse_tag_body(ny_parser_t *p, ny_ctype_t *ty) {
  if (!parse_is(p, "{"))
    return 1;
  if (ty && ty->kind == NY_CTYPE_ENUM)
    return parse_enum_body(p, ty);
  if (!ty) {
    skip_balanced(p, "{", "}");
    return 1;
  }
  if (p && p->pack_align > 0 &&
      (ty->kind == NY_CTYPE_STRUCT || ty->kind == NY_CTYPE_UNION)) {
    ty->aggregate_pack_align = p->pack_align;
    if (p->pack_align == 1)
      ty->flags |= NY_CTYPEF_PACKED;
  }
  parse_advance(p);
  size_t size = 0;
  size_t packed_size = 0;
  size_t align = 1;
  unsigned fields = 0;
  unsigned function_pointers = 0;
  unsigned bitfield_unit_bits = 0;
  unsigned bitfield_used_bits = 0;
  int layout_ok = 1;
  int diagnosed_layout = 0;
  while (p->tok.kind != NY_CTOK_EOF && !parse_is(p, "}")) {
    ny_ctype_t field_ty;
    ny_ctok_t field_name;
    type_init(&field_ty);
    while (parse_decl_marker(p))
      ;
    if (parse_named_type(p, &field_ty, &field_name, 1) < 0) {
      layout_ok = 0;
      skip_to_decl_end(p);
      continue;
    }
    int flexible_array = c_type_is_flexible_array(&field_ty);
    ny_ctype_t comma_base_ty = c_type_without_array(field_ty);
    ny_c_layout_t field_layout = {0};
    if (ny_ctype_layout(&field_ty, p->abi, &field_layout) &&
        field_layout.align > 0) {
      if (parse_accept(p, ":")) {
        unsigned width = 0;
        if (p->tok.kind != NY_CTOK_NUMBER ||
            !parse_decimal_bits(p->tok, &width)) {
          layout_ok = 0;
        } else {
          parse_advance(p);
          if (field_name.kind == NY_CTOK_IDENT)
            fields++;
          unsigned storage_bits = (unsigned)(field_layout.size * 8u);
          if (!field_layout.is_integer || field_ty.ptr_depth > 0 ||
              storage_bits == 0 || width > storage_bits) {
            layout_ok = 0;
          } else if (width == 0 && field_name.kind == NY_CTOK_IDENT) {
            layout_ok = 0;
          } else if (ty->kind == NY_CTYPE_UNION) {
            aggregate_add_storage(ty, &field_layout, field_ty.align_override,
                                  &size, &packed_size, &align);
          } else if (width > 0) {
            bool same_unit = bitfield_unit_bits == storage_bits &&
                             bitfield_used_bits > 0 &&
                             width <= bitfield_unit_bits - bitfield_used_bits;
            if (!same_unit) {
              aggregate_add_storage(ty, &field_layout, field_ty.align_override,
                                    &size, &packed_size, &align);
              bitfield_unit_bits = storage_bits;
              bitfield_used_bits = 0;
            }
            bitfield_used_bits += width;
            /* L-4: Record named bitfield fields so they appear in the layout. */
            if (field_name.kind == NY_CTOK_IDENT) {
              aggregate_note_field(ty, field_name, &field_ty, &field_layout,
                                   aggregate_field_offset(ty, &field_layout,
                                                          field_ty.align_override,
                                                          size),
                                   width);
            }
          } else if (ty->kind == NY_CTYPE_STRUCT) {
            /* Zero-width unnamed bitfield: pad size to the next boundary of
               the storage type, but do NOT increase struct alignment.
               E.g. struct { char x; int : 0; } → sizeof=4, alignof=1. */
            size_t storage_align = field_layout.align > 0 ? field_layout.align : 1;
            size = ny_c_align_up(size, storage_align);
            bitfield_unit_bits = 0;
            bitfield_used_bits = 0;
          } else {
            bitfield_unit_bits = 0;
            bitfield_used_bits = 0;
          }
        }
      } else {
        if (c_type_is_anonymous_aggregate_field(&field_ty, field_name)) {
          fields += field_ty.aggregate_fields;
          size_t anon_offset = aggregate_field_offset(ty, &field_layout, field_ty.align_override, size);
          aggregate_flatten_anonymous(ty, &field_ty, anon_offset);
        } else {
          fields++;
          aggregate_note_field(ty, field_name, &field_ty, &field_layout,
                               aggregate_field_offset(ty, &field_layout,
                                                      field_ty.align_override,
                                                      size),
                               0);
        }
        function_pointers += c_type_function_pointer_slots(&field_ty);
        bitfield_unit_bits = 0;
        bitfield_used_bits = 0;
        aggregate_add_storage(ty, &field_layout, field_ty.align_override, &size,
                              &packed_size, &align);
      }
    } else if (flexible_array && ty->kind == NY_CTYPE_STRUCT &&
               field_name.kind == NY_CTOK_IDENT && parse_is(p, ";")) {
      ny_parser_t lookahead = *p;
      parse_accept(&lookahead, ";");
      if (parse_is(&lookahead, "}")) {
        fields++;
        function_pointers += c_type_function_pointer_slots(&field_ty);
        bitfield_unit_bits = 0;
        bitfield_used_bits = 0;
      } else {
        fields++;
        layout_ok = 0;
      }
    } else {
      if (field_ty.kind == NY_CTYPE_NAMED && field_ty.ptr_depth == 0) {
        parse_errorf(p, "unsupported named C type '%.*s' by value at %u:%u",
                     (int)field_ty.name.len, field_ty.name.start,
                     field_name.line, field_name.col);
        diagnosed_layout = 1;
      }
      layout_ok = 0;
    }
    while (parse_accept(p, ",")) {
      if (comma_base_ty.kind == NY_CTYPE_INVALID) {
        layout_ok = 0;
        continue;
      }
      ny_ctype_t next_ty = comma_base_ty;
      ny_ctok_t next_name = cempty_tok();
      while (parse_type_marker(p, &next_ty))
        ;
      parse_ptrs(p, &next_ty);
      while (parse_type_marker(p, &next_ty))
        ;
      if (parse_is(p, "(")) {
        ny_parser_bookmark_t save = parser_bookmark(p);
        ny_ctype_t save_ty = next_ty;
        parse_advance(p);
        while (parse_type_marker(p, &next_ty))
          ;
        parse_ptrs(p, &next_ty);
        while (parse_type_marker(p, &next_ty))
          ;
        if (p->tok.kind == NY_CTOK_IDENT) {
          next_name = p->tok;
          parse_advance(p);
          while (parse_type_marker(p, &next_ty))
            ;
          parse_array_suffix(p, &next_ty);
          while (parse_type_marker(p, &next_ty))
            ;
          if (!parse_accept(p, ")")) {
            layout_ok = 0;
            parser_rewind(p, save);
            next_ty = save_ty;
          } else {
            while (parse_type_marker(p, &next_ty))
              ;
            if (skip_function_suffix(p))
              next_ty.flags |= NY_CTYPEF_FUNCTION_PTR;
            else if (next_ty.ptr_depth > 0)
              parse_array_suffix(p, NULL);
            while (parse_type_marker(p, &next_ty))
              ;
            if (!c_type_is_function_pointer(&next_ty) && next_ty.ptr_depth == 0)
              parse_array_suffix(p, &next_ty);
          }
        } else {
          layout_ok = 0;
          parser_rewind(p, save);
          next_ty = save_ty;
        }
      } else if (p->tok.kind == NY_CTOK_IDENT) {
        next_name = p->tok;
        parse_advance(p);
        parse_array_suffix(p, &next_ty);
      }
      if (next_name.kind == NY_CTOK_IDENT) {
        ny_c_layout_t next_layout = {0};
        if (ny_ctype_layout(&next_ty, p->abi, &next_layout) &&
            next_layout.align > 0) {
          fields++;
          aggregate_note_field(ty, next_name, &next_ty, &next_layout,
                               aggregate_field_offset(ty, &next_layout,
                                                      next_ty.align_override,
                                                      size),
                               0);
          function_pointers += c_type_function_pointer_slots(&next_ty);
          bitfield_unit_bits = 0;
          bitfield_used_bits = 0;
          aggregate_add_storage(ty, &next_layout, next_ty.align_override,
                                &size, &packed_size, &align);
        } else {
          if (next_ty.kind == NY_CTYPE_NAMED && next_ty.ptr_depth == 0) {
            parse_errorf(p, "unsupported named C type '%.*s' by value at %u:%u",
                         (int)next_ty.name.len, next_ty.name.start,
                         next_name.line, next_name.col);
            diagnosed_layout = 1;
          }
          fields++;
          layout_ok = 0;
        }
      } else {
        break;
      }
    }
    if (!parse_accept(p, ";")) {
      layout_ok = 0;
      skip_to_decl_end(p);
    }
  }
  if (!parse_accept(p, "}"))
    return parse_errorf(p, "expected '}' after C aggregate body at %u:%u",
                        p->tok.line, p->tok.col);
  if (layout_ok) {
    ty->aggregate_fields = fields;
    ty->aggregate_function_pointers = function_pointers;
    if (p && p->pack_align > 0)
      ty->aggregate_pack_align = p->pack_align;
    ty->aggregate_align = align ? align : 1;
    ty->aggregate_size = ny_c_align_up(size, ty->aggregate_align);
    ty->aggregate_packed_size = packed_size;
    ty->aggregate_has_layout = 1;
  } else {
    if (p->fatal_error || diagnosed_layout)
      return -1;
    return parse_errorf(p, "unsupported C field layout at %u:%u",
                        p->tok.line, p->tok.col);
  }
  return 1;
}

static int parse_type_spec(ny_parser_t *p, ny_ctype_t *ty) {
  int saw = 0;
  int long_count = 0;
  for (;;) {
    if (parse_type_marker(p, ty)) {
      saw = 1;
      continue;
    }
    if (type_qual(p, ty)) {
      saw = 1;
      continue;
    }
    if (parse_kw(p, "unsigned")) {
      ty->flags |= NY_CTYPEF_UNSIGNED;
      saw = 1;
      parse_advance(p);
      continue;
    }
    if (parse_kw(p, "signed")) {
      ty->flags |= NY_CTYPEF_SIGNED;
      saw = 1;
      parse_advance(p);
      continue;
    }
    if (parse_kw(p, "void")) {
      ty->kind = NY_CTYPE_VOID;
      saw = 1;
      parse_advance(p);
      continue;
    }
    if (parse_kw(p, "_Bool") || parse_kw(p, "bool")) {
      ty->kind = NY_CTYPE_BOOL;
      saw = 1;
      parse_advance(p);
      continue;
    }
    if (parse_kw(p, "char")) {
      ty->kind = NY_CTYPE_CHAR;
      saw = 1;
      parse_advance(p);
      continue;
    }
    if (parse_kw(p, "short")) {
      ty->kind = NY_CTYPE_SHORT;
      saw = 1;
      parse_advance(p);
      continue;
    }
    if (parse_kw(p, "int")) {
      if (ty->kind == NY_CTYPE_INVALID)
        ty->kind = NY_CTYPE_INT;
      saw = 1;
      parse_advance(p);
      continue;
    }
    if (parse_kw(p, "long")) {
      ty->kind = NY_CTYPE_LONG;
      if (++long_count >= 2)
        ty->flags |= NY_CTYPEF_LONG_LONG;
      saw = 1;
      parse_advance(p);
      continue;
    }
    if (parse_kw(p, "_BitInt") || parse_kw(p, "__BITINT_MAX_WIDTH__")) {
      ty->kind = NY_CTYPE_LONG;
      saw = 1;
      parse_advance(p);
      if (parse_accept(p, "(")) {
        size_t width = 0;
        if (parse_array_extent_expr(p, &width) && width > 0 && width <= 4096)
          ty->bitint_width = (unsigned)width;
        parse_accept(p, ")");
      }
      continue;
    }
    if (parse_kw(p, "typeof") || parse_kw(p, "__typeof__") ||
        parse_kw(p, "__typeof")) {
      saw = 1;
      parse_advance(p);
      if (parse_accept(p, "(")) {
        int depth = 1;
        while (p->tok.kind != NY_CTOK_EOF && depth > 0) {
          if (parse_is(p, "("))
            depth++;
          else if (parse_is(p, ")"))
            depth--;
          if (depth > 0)
            parse_advance(p);
        }
        parse_accept(p, ")");
      }
      ty->kind = NY_CTYPE_LONG;
      continue;
    }
    if (parse_kw(p, "float")) {
      ty->kind = NY_CTYPE_FLOAT;
      saw = 1;
      parse_advance(p);
      continue;
    }
    if (parse_kw(p, "double")) {
      if (ty->kind == NY_CTYPE_LONG)
        ty->kind = NY_CTYPE_LONG_DOUBLE;
      else
        ty->kind = NY_CTYPE_DOUBLE;
      saw = 1;
      parse_advance(p);
      continue;
    }
    if (parse_kw(p, "__int128") || parse_kw(p, "__int128_t")) {
      ty->kind = NY_CTYPE_LONG;
      ty->flags |= NY_CTYPEF_LONG_LONG | NY_CTYPEF_INT128;
      saw = 1;
      parse_advance(p);
      continue;
    }
    if (parse_kw(p, "__float128") || parse_kw(p, "_Float128")) {
      ty->kind = NY_CTYPE_DOUBLE;
      saw = 1;
      parse_advance(p);
      continue;
    }
    if (parse_kw(p, "_Float16") || parse_kw(p, "__fp16")) {
      ty->kind = NY_CTYPE_HALF;
      saw = 1;
      parse_advance(p);
      continue;
    }
    if (parse_kw(p, "struct") || parse_kw(p, "union") || parse_kw(p, "enum")) {
      if (parse_kw(p, "struct"))
        ty->kind = NY_CTYPE_STRUCT;
      else if (parse_kw(p, "union"))
        ty->kind = NY_CTYPE_UNION;
      else
        ty->kind = NY_CTYPE_ENUM;
      saw = 1;
      parse_advance(p);
      ny_ctok_t tag_name = cempty_tok();
      if (p->tok.kind == NY_CTOK_IDENT) {
        tag_name = p->tok;
        ty->name = tag_name;
        parse_advance(p);
      }
      if (parse_tag_body(p, ty) < 0)
        return -1;
      if ((ty->kind == NY_CTYPE_STRUCT || ty->kind == NY_CTYPE_UNION) &&
          tag_name.kind == NY_CTOK_IDENT) {
        if (ty->aggregate_has_layout) {
          parser_note_tag(p, tag_name, ty);
        } else {
          ny_ctype_t tagged;
          if (parser_lookup_tag(p, tag_name, ty->kind, &tagged))
            *ty = tagged;
        }
      }
      continue;
    }
    if (ty->kind == NY_CTYPE_INVALID && p->tok.kind == NY_CTOK_IDENT) {
      ny_ctype_t named;
      if (parser_lookup_typedef(p, p->tok, &named) ||
          parser_lookup_abi_typedef(p, p->tok, &named)) {
        *ty = named;
        saw = 1;
        parse_advance(p);
        continue;
      }
      ty->kind = NY_CTYPE_NAMED;
      ty->name = p->tok;
      saw = 1;
      parse_advance(p);
      continue;
    }
    break;
  }
  if (saw && ty->kind == NY_CTYPE_INVALID)
    ty->kind = NY_CTYPE_INT;
  return saw;
}

static void parse_ptrs(ny_parser_t *p, ny_ctype_t *ty) {
  while (parse_accept(p, "*")) {
    ty->ptr_depth++;
    while (type_qual(p, ty) || parse_type_marker(p, ty))
      ;
  }
}

static int parse_integer_size(ny_ctok_t tok, size_t *out) {
  if (!out || tok.kind != NY_CTOK_NUMBER || tok.len == 0)
    return 0;
  size_t i = 0;
  int64_t value = 0;
  if (!c_parse_integer_slice(tok.start, tok.len, &i, &value) || i != tok.len)
    return 0;
  if (value < 0)
    return 0;
  *out = (size_t)value;
  return 1;
}

static int parse_char_size(ny_ctok_t tok, size_t *out) {
  if (!out || tok.kind != NY_CTOK_CHAR || tok.len == 0)
    return 0;
  size_t i = 0;
  return c_parse_char_literal_slice(tok.start, tok.len, &i, out) && i == tok.len;
}

static int parse_decimal_bits(ny_ctok_t tok, unsigned *out) {
  if (!out || tok.kind != NY_CTOK_NUMBER || tok.len == 0)
    return 0;
  size_t value = 0;
  if (!parse_integer_size(tok, &value))
    return 0;
  if (value > 4096)
    return 0;
  *out = (unsigned)value;
  return 1;
}

static void aggregate_add_storage(ny_ctype_t *ty, const ny_c_layout_t *layout,
                                  unsigned align_override, size_t *size,
                                  size_t *packed_size, size_t *align) {
  if (!ty || !layout || !size || !packed_size || !align)
    return;
  size_t field_align = align_override > 0
                            ? (align_override > layout->align ? align_override
                                                               : layout->align)
                            : c_pack_cap_align(layout->align,
                                              ty->aggregate_pack_align);
  if (ty->kind == NY_CTYPE_STRUCT) {
    *size = ny_c_align_up(*size, field_align);
    *size += layout->size;
    *packed_size += layout->size;
  } else {
    if (layout->size > *size)
      *size = layout->size;
    if (layout->size > *packed_size)
      *packed_size = layout->size;
  }
  if (field_align > *align)
    *align = field_align;
}

static size_t aggregate_field_offset(const ny_ctype_t *owner,
                                     const ny_c_layout_t *layout,
                                     unsigned align_override, size_t size) {
  if (!owner || !layout)
    return 0;
  if (owner->kind == NY_CTYPE_UNION)
    return 0;
  size_t field_align = align_override > 0
                           ? (align_override > layout->align ? align_override
                                                              : layout->align)
                           : c_pack_cap_align(layout->align,
                                             owner->aggregate_pack_align);
  return ny_c_align_up(size, field_align ? field_align : 1);
}

static void aggregate_note_field(ny_ctype_t *owner, ny_ctok_t name,
                                 const ny_ctype_t *field_ty,
                                 const ny_c_layout_t *layout, size_t offset,
                                 unsigned bitfield_width) {
  if (!owner || !field_ty || !layout || name.kind != NY_CTOK_IDENT)
    return;
  if (owner->field_count >= NY_C_MAX_FIELDS)
    return;
  ny_c_field_t *f = &owner->fields[owner->field_count++];
  memset(f, 0, sizeof(*f));
  f->name = name;
  f->kind = field_ty->kind;
  f->flags = field_ty->flags;
  f->ptr_depth = field_ty->ptr_depth;
  f->array_elems = field_ty->array_elems;
  f->array_unknown = field_ty->array_unknown;
  f->type_name = field_ty->name;
  f->offset = offset;
  f->size = layout->size;
  f->align = layout->align;
  f->bitfield_width = bitfield_width;
}

static int parse_array_extent_primary(ny_parser_t *p, size_t *out) {
  if (!p || !out)
    return 0;
  if (parse_is(p, "!") || parse_is(p, "~") || parse_is(p, "+")) {
    const char *op = p->tok.start;
    parse_advance(p);
    size_t value = 0;
    if (!parse_array_extent_primary(p, &value))
      return 0;
    *out = op[0] == '!' ? !value : op[0] == '~' ? ~value : value;
    return 1;
  }
  if (parse_accept(p, "-")) {
    size_t value = 0;
    if (!parse_array_extent_primary(p, &value) || value > (size_t)INT64_MAX)
      return 0;
    *out = (size_t)(-(int64_t)value);
    return 1;
  }
  if (parse_kw(p, "sizeof")) {
    parse_advance(p);
    if (!parse_accept(p, "("))
      return 0;
    ny_ctype_t ty;
    ny_ctok_t name;
    type_init(&ty);
    if (parse_named_type(p, &ty, &name, 1) < 0)
      return 0;
    if (!parse_accept(p, ")"))
      return 0;
    ny_c_layout_t layout = {0};
    if (!ny_ctype_layout(&ty, p->abi, &layout))
      return 0;
    *out = layout.size;
    return layout.size > 0;
  }
  if (parse_accept(p, "(")) {
    ny_parser_t cast_try = *p;
    ny_ctype_t cast_ty;
    ny_ctok_t cast_name;
    type_init(&cast_ty);
    if (parse_named_type(&cast_try, &cast_ty, &cast_name, 1) > 0 &&
        parse_accept(&cast_try, ")")) {
      *p = cast_try;
      return parse_array_extent_primary(p, out);
    }
    if (!parse_array_extent_expr(p, out))
      return 0;
    return parse_accept(p, ")");
  }
  if (p->tok.kind == NY_CTOK_NUMBER && parse_integer_size(p->tok, out)) {
    parse_advance(p);
    return 1;
  }
  if (p->tok.kind == NY_CTOK_CHAR && parse_char_size(p->tok, out)) {
    parse_advance(p);
    return 1;
  }
  if (p->tok.kind == NY_CTOK_IDENT) {
    int64_t def_value = 0;
    if (parser_lookup_define(p, p->tok, &def_value) && def_value >= 0) {
      *out = (size_t)def_value;
      parse_advance(p);
      return 1;
    }
  }
  return 0;
}

static int parse_array_extent_term(ny_parser_t *p, size_t *out) {
  size_t lhs = 0;
  if (!parse_array_extent_primary(p, &lhs))
    return 0;
  for (;;) {
    if (!parse_is(p, "*") && !parse_is(p, "/") && !parse_is(p, "%"))
      break;
    int is_mul = parse_is(p, "*");
    int is_div = parse_is(p, "/");
    parse_advance(p);
    size_t rhs = 0;
    if (!parse_array_extent_primary(p, &rhs))
      return 0;
    if (is_mul) {
      if (rhs != 0 && lhs > ((size_t)-1) / rhs)
        return 0;
      lhs *= rhs;
    } else if (is_div) {
      if (rhs == 0)
        return 0;
      lhs /= rhs;
    } else {
      if (rhs == 0)
        return 0;
      lhs %= rhs;
    }
  }
  *out = lhs;
  return 1;
}

static int parse_array_extent_add(ny_parser_t *p, size_t *out) {
  size_t lhs = 0;
  if (!parse_array_extent_term(p, &lhs))
    return 0;
  for (;;) {
    if (!parse_is(p, "+") && !parse_is(p, "-"))
      break;
    int is_add = parse_is(p, "+");
    parse_advance(p);
    size_t rhs = 0;
    if (!parse_array_extent_term(p, &rhs))
      return 0;
    if (is_add) {
      size_t next = lhs + rhs;
      if (next < lhs && lhs <= (size_t)INT64_MAX && rhs <= (size_t)INT64_MAX)
        return 0;
      lhs = next;
    } else {
      if (rhs > lhs && rhs <= (size_t)INT64_MAX)
        return 0;
      lhs -= rhs;
    }
  }
  *out = lhs;
  return 1;
}

static int parse_array_extent_shift(ny_parser_t *p, size_t *out) {
  size_t lhs = 0;
  if (!parse_array_extent_add(p, &lhs))
    return 0;
  for (;;) {
    if (!parse_is(p, "<<") && !parse_is(p, ">>"))
      break;
    int is_left = parse_is(p, "<<");
    parse_advance(p);
    size_t rhs = 0;
    if (!parse_array_extent_add(p, &rhs) || rhs >= sizeof(size_t) * 8u)
      return 0;
    if (is_left) {
      if (lhs > ((size_t)-1) >> rhs)
        return 0;
      lhs <<= rhs;
    } else {
      lhs >>= rhs;
    }
  }
  *out = lhs;
  return 1;
}

static int parse_array_extent_bitand(ny_parser_t *p, size_t *out) {
  size_t lhs = 0;
  if (!parse_array_extent_shift(p, &lhs))
    return 0;
  for (;;) {
    int is_lt = parse_is(p, "<");
    int is_gt = parse_is(p, ">");
    int is_le = parse_is(p, "<=");
    int is_ge = parse_is(p, ">=");
    if (!is_lt && !is_gt && !is_le && !is_ge)
      break;
    parse_advance(p);
    size_t rhs = 0;
    if (!parse_array_extent_shift(p, &rhs))
      return 0;
    lhs = is_lt ? lhs < rhs
                : is_gt ? lhs > rhs : is_le ? lhs <= rhs : lhs >= rhs;
  }
  for (;;) {
    int is_eq = parse_is(p, "==");
    int is_ne = parse_is(p, "!=");
    if (!is_eq && !is_ne)
      break;
    parse_advance(p);
    size_t rhs = 0;
    if (!parse_array_extent_shift(p, &rhs))
      return 0;
    lhs = is_eq ? lhs == rhs : lhs != rhs;
  }
  while (parse_is(p, "&")) {
    parse_advance(p);
    size_t rhs = 0;
    if (!parse_array_extent_shift(p, &rhs))
      return 0;
    lhs &= rhs;
  }
  *out = lhs;
  return 1;
}

static int parse_array_extent_bitxor(ny_parser_t *p, size_t *out) {
  size_t lhs = 0;
  if (!parse_array_extent_bitand(p, &lhs))
    return 0;
  while (parse_is(p, "^")) {
    parse_advance(p);
    size_t rhs = 0;
    if (!parse_array_extent_bitand(p, &rhs))
      return 0;
    lhs ^= rhs;
  }
  *out = lhs;
  return 1;
}

static int parse_array_extent_bitor(ny_parser_t *p, size_t *out) {
  size_t lhs = 0;
  if (!parse_array_extent_bitxor(p, &lhs))
    return 0;
  while (parse_is(p, "|")) {
    parse_advance(p);
    size_t rhs = 0;
    if (!parse_array_extent_bitxor(p, &rhs))
      return 0;
    lhs |= rhs;
  }
  *out = lhs;
  return 1;
}

static int parse_array_extent_logand(ny_parser_t *p, size_t *out) {
  size_t lhs = 0;
  if (!parse_array_extent_bitor(p, &lhs))
    return 0;
  while (parse_is(p, "&&")) {
    parse_advance(p);
    size_t rhs = 0;
    if (!parse_array_extent_bitor(p, &rhs))
      return 0;
    lhs = lhs && rhs;
  }
  *out = lhs;
  return 1;
}

static int parse_array_extent_logor(ny_parser_t *p, size_t *out) {
  size_t lhs = 0;
  if (!parse_array_extent_logand(p, &lhs))
    return 0;
  while (parse_is(p, "||")) {
    parse_advance(p);
    size_t rhs = 0;
    if (!parse_array_extent_logand(p, &rhs))
      return 0;
    lhs = lhs || rhs;
  }
  *out = lhs;
  return 1;
}

static int parse_array_extent_expr_impl(ny_parser_t *p, size_t *out) {
  size_t cond = 0;
  if (!parse_array_extent_logor(p, &cond))
    return 0;
  if (!parse_accept(p, "?")) {
    *out = cond;
    return 1;
  }
  size_t when_true = 0;
  size_t when_false = 0;
  if (!parse_array_extent_expr(p, &when_true) || !parse_accept(p, ":") ||
      !parse_array_extent_expr(p, &when_false))
    return 0;
  *out = cond ? when_true : when_false;
  return 1;
}

static int parse_array_extent_expr(ny_parser_t *p, size_t *out) {
  if (!parser_recursion_enter(p, &p->array_expr_depth, "array extent"))
    return 0;
  int ok = parse_array_extent_expr_impl(p, out);
  p->array_expr_depth--;
  return ok;
}

static void parse_array_suffix(ny_parser_t *p, ny_ctype_t *ty) {
  while (parse_accept(p, "[")) {
    size_t extent = 0;
    int parsed = parse_array_extent_expr(p, &extent);
    if (parsed && extent > 0 && extent <= (size_t)INT64_MAX) {
      if (ty) {
        if (ty->array_elems == 0)
          ty->array_elems = extent;
        else
          ty->array_elems *= extent;
      }
    } else if (parsed && ty) {
      if (ty)
        ty->array_invalid = 1;
    } else if (ty) {
      ty->array_unknown = 1;
    }
    if (!parse_accept(p, "]")) {
      if (ty)
        ty->array_unknown = 1;
      while (p->tok.kind != NY_CTOK_EOF && !parse_is(p, "]") &&
             !parse_is(p, ";") && !parse_is(p, ","))
        parse_advance(p);
      parse_accept(p, "]");
    }
  }
}

static int skip_function_suffix(ny_parser_t *p) {
  if (parse_is(p, "(")) {
    skip_balanced(p, "(", ")");
    return 1;
  }
  return 0;
}

static int parse_named_type(ny_parser_t *p, ny_ctype_t *ty, ny_ctok_t *name,
                             int allow_abstract) {
  int ts = parse_type_spec(p, ty);
  if (ts < 0)
    return -1;
  if (!ts)
    return parse_errorf(p, "expected C type at %u:%u", p->tok.line, p->tok.col);
  while (parse_type_marker(p, ty))
    ;
  parse_ptrs(p, ty);
  while (parse_type_marker(p, ty))
    ;
  *name = cempty_tok();
  if (parse_is(p, "(")) {
    ny_parser_bookmark_t save = parser_bookmark(p);
    ny_ctype_t save_ty = *ty;
    ny_ctok_t save_name = *name;
    parse_advance(p);
    while (parse_type_marker(p, ty))
      ;
    if (parse_is(p, "(")) {
      ny_ctype_t inner_ty;
      ny_ctok_t inner_name;
      type_init(&inner_ty);
      inner_ty.ptr_depth = ty->ptr_depth;
      if (parse_named_type(p, &inner_ty, &inner_name, allow_abstract) > 0) {
        ty->ptr_depth = inner_ty.ptr_depth;
        ty->flags = inner_ty.flags;
        if (inner_name.kind == NY_CTOK_IDENT)
          *name = inner_name;
        while (parse_type_marker(p, ty))
          ;
        if (!parse_accept(p, ")"))
          return parse_errorf(p, "expected ')' after parenthesized C declarator at %u:%u",
                       p->tok.line, p->tok.col);
        while (parse_type_marker(p, ty))
          ;
        if (skip_function_suffix(p))
          ty->flags |= NY_CTYPEF_FUNCTION_PTR;
        else if (ty->ptr_depth > 0)
          parse_array_suffix(p, NULL);
        while (parse_type_marker(p, ty))
          ;
        if (!c_type_is_function_pointer(ty) && ty->ptr_depth == 0)
          parse_array_suffix(p, ty);
        return 1;
      }
    }
    parse_ptrs(p, ty);
    while (parse_type_marker(p, ty))
      ;
    if (p->tok.kind == NY_CTOK_IDENT) {
      *name = p->tok;
      parse_advance(p);
      while (parse_type_marker(p, ty))
        ;
      parse_array_suffix(p, ty);
      while (parse_type_marker(p, ty))
        ;
      if (!parse_accept(p, ")"))
        return parse_errorf(p, "expected ')' after parenthesized C declarator at %u:%u",
                     p->tok.line, p->tok.col);
      while (parse_type_marker(p, ty))
        ;
      if (skip_function_suffix(p))
        ty->flags |= NY_CTYPEF_FUNCTION_PTR;
      else if (ty->ptr_depth > 0)
        parse_array_suffix(p, NULL);
      while (parse_type_marker(p, ty))
        ;
      if (!c_type_is_function_pointer(ty) && ty->ptr_depth == 0)
        parse_array_suffix(p, ty);
      return 1;
    }
    if (allow_abstract && parse_accept(p, ")")) {
      while (parse_type_marker(p, ty))
        ;
      if (skip_function_suffix(p))
        ty->flags |= NY_CTYPEF_FUNCTION_PTR;
      else if (ty->ptr_depth > 0)
        parse_array_suffix(p, NULL);
      while (parse_type_marker(p, ty))
        ;
      if (!c_type_is_function_pointer(ty) && ty->ptr_depth == 0)
        parse_array_suffix(p, ty);
      return 1;
    }
    parser_rewind(p, save);
    *ty = save_ty;
    *name = save_name;
  }
  if (p->tok.kind == NY_CTOK_IDENT) {
    *name = p->tok;
    parse_advance(p);
    while (parse_type_marker(p, ty))
      ;
    parse_array_suffix(p, ty);
    while (parse_type_marker(p, ty))
      ;
  } else if ((ty->kind == NY_CTYPE_STRUCT || ty->kind == NY_CTYPE_UNION ||
              ty->kind == NY_CTYPE_ENUM) &&
             (parse_is(p, ";") || parse_is(p, ","))) {
    return 1;
  } else if (!allow_abstract) {
    return parse_errorf(p, "expected C declarator name at %u:%u", p->tok.line,
                 p->tok.col);
  }
  return 1;
}

static int parse_params(ny_parser_t *p, ny_cdecl_t *decl) {
  if (!parse_accept(p, "("))
    return 0;
  while (parse_decl_marker(p))
    ;
  if (parse_accept(p, ")"))
    return 1;
  if (parse_kw(p, "void")) {
    ny_parser_bookmark_t save = parser_bookmark(p);
    parse_advance(p);
    if (parse_accept(p, ")"))
      return 1;
    parser_rewind(p, save);
  }
  while (p->tok.kind != NY_CTOK_EOF && !parse_is(p, ")")) {
    if (parse_is(p, "...")) {
      decl->is_variadic = 1;
      parse_advance(p);
      break;
    }
    if (decl->param_count >= NY_C_MAX_PARAMS)
      return parse_errorf(p, "too many C parameters at %u:%u", p->tok.line, p->tok.col);
    ny_ctype_t ty;
    ny_ctok_t name;
    type_init(&ty);
    while (parse_decl_marker(p))
      ;
    if (parse_named_type(p, &ty, &name, 1) < 0)
      return -1;
    while (parse_decl_marker(p))
      ;
    decl->params[decl->param_count] = ty;
    decl->param_names[decl->param_count] = name;
    decl->param_count++;
    if (!parse_accept(p, ","))
      break;
  }
  if (!parse_accept(p, ")"))
    return parse_errorf(p, "expected ')' after C parameter list at %u:%u", p->tok.line,
                 p->tok.col);
  return 1;
}

static int parse_non_import_decl(ny_parser_t *p) {
  if (parse_accept(p, ";"))
    return 1;
  if (parse_kw(p, "_Pragma")) {
    parse_advance(p);
    if (parse_is(p, "("))
      skip_balanced(p, "(", ")");
    parse_accept(p, ";");
    return 1;
  }
  if (parse_kw(p, "_Static_assert") || parse_kw(p, "static_assert")) {
    parse_advance(p);
    if (parse_is(p, "(")) {
      /* _Static_assert(expr, "msg"): evaluate expr and fail if 0. */
      const char *expr_start = NULL;
      size_t expr_len = 0;
      const char *msg_str = NULL;
      size_t msg_len = 0;
      /* Save position before the '(' */
      ny_ctok_t open_tok = p->tok;
      skip_balanced(p, "(", ")");
      /* Now extract the text between parens from the source */
      if (open_tok.start && open_tok.len == 1) {
        const char *src = open_tok.start + 1; /* skip '(' */
        /* Find the matching ')' by counting parens */
        int depth = 1;
        const char *end = src;
        while (*end && depth > 0) {
          if (*end == '(') depth++;
          else if (*end == ')') depth--;
          if (depth > 0) end++;
        }
        size_t inner_len = (size_t)(end - src);
        /* Find the last comma to split expr from message */
        const char *last_comma = NULL;
        {
          int d = 0;
          for (size_t i = 0; i < inner_len; i++) {
            if (src[i] == '(') d++;
            else if (src[i] == ')') d--;
            else if (src[i] == ',' && d == 0)
              last_comma = src + i;
          }
        }
        if (last_comma) {
          expr_start = src;
          expr_len = (size_t)(last_comma - src);
          /* Skip whitespace after comma to find message string */
          const char *mp = last_comma + 1;
          while (mp < end && (*mp == ' ' || *mp == '\t'))
            mp++;
          if (mp < end && (*mp == '"' || *mp == 'L')) {
            int is_wide = (*mp == 'L');
            if (is_wide) mp++;
            if (mp < end && *mp == '"') {
              mp++;
              const char *me = mp;
              while (me < end && *me != '"')
                me++;
              msg_str = mp;
              msg_len = (size_t)(me - mp);
            }
          }
        } else {
          /* Single-arg form: _Static_assert(expr) */
          expr_start = src;
          expr_len = inner_len;
        }
      }
      if (expr_start && expr_len > 0) {
        int64_t value = 0;
        size_t pos = 0;
        if (macro_parse_expr(p, expr_start, expr_len, &pos, &value) &&
            value == 0) {
          if (msg_str && msg_len > 0)
            parse_errorf(p, "_Static_assert failed: %.*s", (int)msg_len,
                         msg_str);
          else
            parse_errorf(p, "_Static_assert condition evaluated to false");
          return -1;
        }
      }
    } else {
      parse_accept(p, ";");
    }
    return 1;
  }
  if (parse_kw(p, "__asm__") || parse_kw(p, "__asm") || parse_kw(p, "asm")) {
    parse_advance(p);
    if (parse_is(p, "("))
      skip_balanced(p, "(", ")");
    parse_accept(p, ";");
    return 1;
  }
  return 0;
}

static void parser_seed_predefined_macros(ny_parser_t *p) {
  if (!p)
    return;
  /* Standard compliance */
  parser_note_define(p, "__STDC__", 7, 1);
  parser_note_define(p, "__STDC_VERSION__", 16, 201710L);
  parser_note_define(p, "__STDC_HOSTED__", 15, 1);
  /* Platform detection (linux x86-64) */
#ifdef __linux__
  parser_note_define(p, "__linux__", 9, 1);
  parser_note_define(p, "linux", 5, 1);
#endif
#ifdef __x86_64__
  parser_note_define(p, "__x86_64__", 10, 1);
  parser_note_define(p, "__LP64__", 8, 1);
  parser_note_define(p, "__SIZEOF_POINTER__", 18, 8);
  parser_note_define(p, "__SIZEOF_LONG__", 15, 8);
  parser_note_define(p, "__SIZEOF_SIZE_T__", 17, 8);
#endif
#ifdef __aarch64__
  parser_note_define(p, "__aarch64__", 11, 1);
  parser_note_define(p, "__LP64__", 8, 1);
  parser_note_define(p, "__SIZEOF_POINTER__", 18, 8);
  parser_note_define(p, "__SIZEOF_LONG__", 15, 8);
  parser_note_define(p, "__SIZEOF_SIZE_T__", 17, 8);
#endif
  parser_note_define(p, "__SIZEOF_INT__", 14, 4);
  parser_note_define(p, "__SIZEOF_SHORT__", 16, 2);
  parser_note_define(p, "__SIZEOF_FLOAT__", 16, 4);
  parser_note_define(p, "__SIZEOF_DOUBLE__", 17, 8);
  parser_note_define(p, "__SIZEOF_LONG_LONG__", 20, 8);
  parser_note_define(p, "__CHAR_BIT__", 12, 8);
  parser_note_define(p, "__INT_MAX__", 11, 2147483647);
  parser_note_define(p, "__LONG_MAX__", 12, 9223372036854775807L);
  parser_note_define(p, "__LLONG_MAX__", 13, 9223372036854775807LL);
  parser_note_define(p, "__BYTE_ORDER__", 14, 1234);
  parser_note_define(p, "__ORDER_LITTLE_ENDIAN__", 23, 1234);
  parser_note_define(p, "__ORDER_BIG_ENDIAN__", 20, 4321);
  parser_note_define(p, "__ELF__", 7, 1);
  parser_note_define(p, "__unix__", 8, 1);
  parser_note_define(p, "__unix", 6, 1);
  parser_note_define(p, "unix", 4, 1);
  /* GCC compatibility */
  parser_note_define(p, "__GNUC__", 8, 4);
  parser_note_define(p, "__GNUC_MINOR__", 14, 2);
  parser_note_define(p, "__GNUC_PATCHLEVEL__", 20, 0);
  parser_note_define(p, "__VERSION__", 11, 0);
}

void ny_parse_init_abi(ny_parser_t *p, const char *src, size_t len,
                       const char *abi) {
  if (!p)
    return;
  memset(p, 0, sizeof(*p));
  p->abi = abi;
  ny_lex_init(&p->lx, src, len);
  p->tok = ny_lex_next(&p->lx);
  parser_seed_predefined_macros(p);
}

void ny_parse_init(ny_parser_t *p, const char *src, size_t len) {
  ny_parse_init_abi(p, src, len, NULL);
}

void ny_parse_cleanup(ny_parser_t *p) {
  if (!p)
    return;
  free(p->intern_buf);
  p->intern_buf = NULL;
  p->intern_len = 0;
  p->intern_cap = 0;
}

int ny_parse_decl(ny_parser_t *p, ny_cdecl_t *out) {
  if (!p || !out)
    return -1;
  memset(out, 0, sizeof(*out));
  out->kind = NY_CDECL_VAR;
  out->name = cempty_tok();
  type_init(&out->type);
  skip_preproc(p);
  if (p->fatal_error)
    return -1;
  {
    unsigned fidx;
    if (p->tok.kind == NY_CTOK_IDENT &&
        parser_lookup_func_macro(p, p->tok, &fidx)) {
      ny_parser_bookmark_t peek = parser_bookmark(p);
      ny_ctok_t next = ny_lex_next(&p->lx);
      if (next.kind == NY_CTOK_PUNCT && next.len == 1 && next.start[0] == '(') {
        p->tok = next;
        if (!expand_func_macro(p, fidx)) {
          parser_rewind(p, peek);
        }
      } else {
        p->tok = next;
      }
    }
  }
  while (parse_non_import_decl(p)) {
    skip_preproc(p);
    if (p->fatal_error)
      return -1;
  }
  if (p->fatal_error)
    return -1;
  if (p->tok.kind == NY_CTOK_EOF)
    return 0;
  while (parse_storage(p, out) || type_qual(p, &out->type) ||
         parse_decl_marker(p))
    ;
  if (parse_named_type(p, &out->type, &out->name, 0) < 0) {
    skip_to_decl_end(p);
    return -1;
  }
  if (out->name.kind == NY_CTOK_EOF &&
      (out->type.kind == NY_CTYPE_STRUCT || out->type.kind == NY_CTYPE_UNION ||
       out->type.kind == NY_CTYPE_ENUM)) {
    out->kind = NY_CDECL_NONE;
    skip_to_decl_end(p);
    return 1;
  }
  if (parse_params(p, out)) {
    if (out->kind != NY_CDECL_TYPEDEF)
      out->kind = NY_CDECL_FUNC;
  } else if (out->kind == NY_CDECL_NONE) {
    out->kind = NY_CDECL_VAR;
  }
  if (out->kind == NY_CDECL_TYPEDEF)
    parser_note_typedef(p, out->name, &out->type);
  if (out->kind == NY_CDECL_FUNC && parse_is(p, "{")) {
    skip_function_body(p);
  } else if (out->kind == NY_CDECL_VAR && parse_is(p, "=")) {
    parse_advance(p);
    skip_initializer(p);
    parse_accept(p, ";");
  } else {
    skip_to_decl_end(p);
  }
  return p->fatal_error ? -1 : 1;
}


static bool ny_c_preproc_word_is(const char *src, size_t len, size_t pos,
                                 const char *word) {
  size_t w = 0;
  while (word[w]) {
    if (pos + w >= len || src[pos + w] != word[w])
      return false;
    w++;
  }
  if (pos + w < len) {
    char c = src[pos + w];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '_')
      return false;
  }
  return true;
}

static size_t ny_c_preproc_word_len(const char *src, size_t len, size_t pos) {
  size_t n = 0;
  while (pos + n < len && c_ident_char(src[pos + n]))
    n++;
  return n;
}

static int ny_c_preproc_ident_defined(ny_parser_t *p, const char *src,
                                      size_t len, size_t pos) {
  macro_skip_ws(src, len, &pos);
  if (pos < len && c_ident_start_char(src[pos])) {
    size_t name = pos;
    while (pos < len && c_ident_char(src[pos]))
      pos++;
    int64_t value = 0;
    return macro_lookup_name(p, src + name, pos - name, &value);
  }
  return 0;
}

static int ny_c_preproc_eval_condition(ny_parser_t *p, const char *src,
                                       size_t len, size_t expr, size_t line_end,
                                       int64_t *out) {
  if (!p || !src || !out || expr > line_end || line_end > len)
    return 0;
  size_t i = expr;
  if (!macro_parse_expr(p, src, line_end, &i, out))
    return 0;
  macro_skip_ws(src, line_end, &i);
  return i >= line_end;
}

static void ny_c_preproc_note_define_summary(ny_parser_t *p, const char *src,
                                             size_t len, size_t expr,
                                             size_t line_end,
                                             ny_ctok_t preproc_tok,
                                             ny_c_header_summary_t *summary) {
  if (!summary || !src || expr > line_end || line_end > len)
    return;
  size_t i = expr;
  macro_skip_ws(src, line_end, &i);
  if (i >= line_end || !c_ident_start_char(src[i])) {
    summary->unsupported_define_lines++;
    return;
  }
  size_t name = i;
  while (i < line_end && c_ident_char(src[i]))
    i++;
  size_t name_len = i - name;
  if (i < line_end && src[i] == '(') {
    summary->function_like_define_lines++;
    /* function-like macros are noted but not expanded by internal frontend yet */
    return;
  }
  summary->object_like_define_lines++;
  macro_skip_ws(src, line_end, &i);
  if (i >= line_end) {
    parser_note_define(p, src + name, name_len, 1);
    return;
  }
  int64_t value = 0;
  if (ny_c_preproc_eval_condition(p, src, len, i, line_end, &value))
    parser_note_preproc(p, preproc_tok);
  else
    summary->unsupported_define_lines++;
}

static void ny_c_count_preproc_lines(const char *src, size_t len,
                                     ny_parser_t *p,
                                     ny_c_header_summary_t *summary) {
  bool at_line_start = true;
  if (!summary)
    return;
  for (size_t i = 0; src && i < len; ++i) {
    char c = src[i];
    if (at_line_start) {
      size_t j = i;
      while (j < len && (src[j] == ' ' || src[j] == '\t' || src[j] == '\r'))
        j++;
      if (j < len && src[j] == '#') {
        summary->preprocessor_lines++;
        size_t line_end = j;
        while (line_end < len) {
          size_t advance = 0;
          if (c_line_continuation_at(src, len, line_end, &advance)) {
            line_end += advance;
            continue;
          }
          if (src[line_end] == '\n')
            break;
          line_end++;
        }
        ny_ctok_t preproc_tok = {NY_CTOK_PREPROC, src + j, line_end - j, 0, 0};
        j++;
        macro_skip_ws(src, line_end, &j);
        size_t word_len = ny_c_preproc_word_len(src, len, j);
        size_t expr = j + word_len;
        macro_skip_ws(src, line_end, &expr);
        if (ny_c_preproc_word_is(src, len, j, "include"))
          summary->include_lines++;
        else if (ny_c_preproc_word_is(src, len, j, "define")) {
          summary->define_lines++;
          ny_c_preproc_note_define_summary(p, src, len, expr, line_end,
                                           preproc_tok, summary);
        } else if (ny_c_preproc_word_is(src, len, j, "undef")) {
          summary->undef_lines++;
          parser_note_preproc(p, preproc_tok);
        } else if (ny_c_preproc_word_is(src, len, j, "if") ||
                   ny_c_preproc_word_is(src, len, j, "ifdef") ||
                   ny_c_preproc_word_is(src, len, j, "ifndef") ||
                   ny_c_preproc_word_is(src, len, j, "elif") ||
                   ny_c_preproc_word_is(src, len, j, "else") ||
                   ny_c_preproc_word_is(src, len, j, "endif")) {
          summary->conditional_lines++;
          if (ny_c_preproc_word_is(src, len, j, "if") ||
              ny_c_preproc_word_is(src, len, j, "elif")) {
            int64_t value = 0;
            if (ny_c_preproc_eval_condition(p, src, len, expr, line_end, &value)) {
              if (value)
                summary->conditional_active_lines++;
              else
                summary->conditional_inactive_lines++;
            }
          } else if (ny_c_preproc_word_is(src, len, j, "ifdef")) {
            if (ny_c_preproc_ident_defined(p, src, len, expr))
              summary->conditional_active_lines++;
            else
              summary->conditional_inactive_lines++;
          } else if (ny_c_preproc_word_is(src, len, j, "ifndef")) {
            if (ny_c_preproc_ident_defined(p, src, len, expr))
              summary->conditional_inactive_lines++;
            else
              summary->conditional_active_lines++;
          } else if (ny_c_preproc_word_is(src, len, j, "else")) {
            summary->conditional_active_lines++;
          }
        }
        i = line_end;
        at_line_start = true;
        continue;
      }
      at_line_start = false;
    }
    if (c == '\n')
      at_line_start = true;
  }
}

int ny_parse_header_summary(const char *src, size_t len,
                            ny_c_header_summary_t *summary, char *err,
                            size_t err_len) {
  if (summary)
    memset(summary, 0, sizeof(*summary));
  if (err && err_len > 0)
    err[0] = '\0';
  if (!src && len > 0) {
    if (err && err_len > 0)
      snprintf(err, err_len, "missing C header source");
    return 0;
  }
  ny_c_header_summary_t local = {0};
  ny_parser_t pp;
  memset(&pp, 0, sizeof(pp));
  ny_c_count_preproc_lines(src, len, &pp, &local);
  if (pp.fatal_error) {
    local.unsupported++;
    if (err && err_len > 0)
      snprintf(err, err_len, "%s", pp.error);
    if (summary)
      *summary = local;
    return 0;
  }
  ny_parser_t p;
  ny_parse_init(&p, src, len);
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
    p.deadline_ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec + 5000000000LL;
  p.token_limit = 2000000;
  while (p.tok.kind != NY_CTOK_EOF) {
    const char *stall_pos = p.tok.start;
    ny_cdecl_t decl;
    int rc = ny_parse_decl(&p, &decl);
    if (rc > 0) {
      local.declarations++;
      switch (decl.kind) {
      case NY_CDECL_FUNC:
        local.functions++;
        break;
      case NY_CDECL_TYPEDEF:
        local.typedefs++;
        break;
      case NY_CDECL_VAR:
        local.variables++;
        break;
      case NY_CDECL_NONE:
        local.tag_decls++;
        break;
      default:
        break;
      }
      if (decl.type.aggregate_has_layout) {
        ny_c_layout_t layout = {0};
        local.aggregate_layouts++;
        local.aggregate_fields += decl.type.aggregate_fields;
        local.function_pointers += decl.type.aggregate_function_pointers;
        if (ny_ctype_layout(&decl.type, p.abi, &layout))
          local.aggregate_bytes += layout.size;
      }
      if (c_type_is_function_pointer(&decl.type))
        local.function_pointers++;
      goto summary_next;
    }
    if (rc < 0) {
      local.unsupported++;
      if (err && err_len > 0 && err[0] == '\0')
        snprintf(err, err_len, "%s", ny_parse_error(&p));
      goto summary_next;
    }
    break;
  summary_next:
    /* Stall guard: if ny_parse_decl returned without advancing the lexer
     * (e.g. pathological macro expansion that never consumes input), force a
     * single token of progress and mark the declaration unsupported so the
     * header falls back to libclang instead of looping forever. */
    if (p.tok.kind != NY_CTOK_EOF && p.tok.start == stall_pos) {
      parse_advance(&p);
      local.unsupported++;
    }
  }
  if (summary)
    *summary = local;
  ny_parse_cleanup(&p);
  return local.unsupported == 0;
}
