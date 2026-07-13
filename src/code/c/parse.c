#include "code/c/c.h"
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

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

static void parse_advance(ny_parser_t *p) { p->tok = ny_lex_next(&p->lx); }

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
        else if (ty && (parse_kw(p, "packed") || parse_kw(p, "__packed__")))
          ty->flags |= NY_CTYPEF_PACKED;
        else if ((parse_kw(p, "aligned") || parse_kw(p, "__aligned__")) &&
                 depth == 2) {
          parse_advance(p);
          if (parse_is(p, "(")) {
            parse_attribute_align_arg(p, ty);
            continue;
          }
          continue;
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
                                 const ny_c_layout_t *layout, size_t offset);
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
  if (p->typedef_count >= NY_C_MAX_TYPEDEFS)
    return;
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
  if (p->tag_count >= NY_C_MAX_TAGS)
    return;
  p->tag_names[p->tag_count] = name;
  p->tag_types[p->tag_count] = *ty;
  p->tag_count++;
}

static int ctok_is_ident_slice(ny_ctok_t tok, const char *start, size_t len) {
  return tok.kind == NY_CTOK_IDENT && tok.len == len &&
         strncmp(tok.start, start, len) == 0;
}

static int parser_lookup_define(ny_parser_t *p, ny_ctok_t name, size_t *out) {
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
                               size_t name_len, size_t value) {
  if (!p || !name || name_len == 0)
    return;
  for (unsigned i = 0; i < p->define_count; ++i) {
    if (ctok_is_ident_slice(p->define_names[i], name, name_len)) {
      p->define_values[i] = value;
      return;
    }
  }
  if (p->define_count >= NY_C_MAX_DEFINES)
    return;
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
                                 size_t *out) {
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
  size_t value = 0;
  size_t digits = 0;
  for (; i < n; ++i) {
    char c = s[i];
    if (c == '_')
      continue;
    int digit = c_digit_value(c);
    if (digit < 0 || (unsigned)digit >= base)
      break;
    if (value > (((size_t)-1) - (size_t)digit) / base)
      return 0;
    value = value * base + (size_t)digit;
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
                             size_t *out) {
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
                            size_t *out);

static int macro_parse_primary(ny_parser_t *p, const char *s, size_t n,
                               size_t *i, size_t *out) {
  macro_skip_ws(s, n, i);
  if (*i >= n)
    return 0;
  if (s[*i] == '!' || s[*i] == '~' || s[*i] == '+') {
    char op = s[(*i)++];
    size_t value = 0;
    if (!macro_parse_primary(p, s, n, i, &value))
      return 0;
    *out = op == '!' ? !value : op == '~' ? ~value : value;
    return 1;
  }
  if (s[*i] == '-') {
    size_t value = 0;
    (*i)++;
    if (!macro_parse_primary(p, s, n, i, &value) || value > (size_t)INT64_MAX)
      return 0;
    *out = (size_t)(-(int64_t)value);
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
    size_t value = 0;
    if (!c_parse_integer_slice(s, n, i, &value))
      return 0;
    *out = value;
    return 1;
  }
  if (s[*i] == '\'') {
    size_t value = 0;
    if (!c_parse_char_literal_slice(s, n, i, &value))
      return 0;
    *out = value;
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
      size_t value = 0;
      *out = macro_lookup_name(p, s + def_name, def_name_len, &value) ? 1 : 0;
      return 1;
    }
    return macro_lookup_name(p, s + name, *i - name, out);
  }
  return 0;
}

static int macro_parse_term(ny_parser_t *p, const char *s, size_t n, size_t *i,
                            size_t *out) {
  size_t lhs = 0;
  if (!macro_parse_primary(p, s, n, i, &lhs))
    return 0;
  for (;;) {
    macro_skip_ws(s, n, i);
    if (*i >= n || (s[*i] != '*' && s[*i] != '/' && s[*i] != '%'))
      break;
    char op = s[(*i)++];
    size_t rhs = 0;
    if (!macro_parse_primary(p, s, n, i, &rhs))
      return 0;
    if (op == '*') {
      if (rhs != 0 && lhs > ((size_t)-1) / rhs)
        return 0;
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
                           size_t *out) {
  size_t lhs = 0;
  if (!macro_parse_term(p, s, n, i, &lhs))
    return 0;
  for (;;) {
    macro_skip_ws(s, n, i);
    if (*i >= n || (s[*i] != '+' && s[*i] != '-'))
      break;
    char op = s[(*i)++];
    size_t rhs = 0;
    if (!macro_parse_term(p, s, n, i, &rhs))
      return 0;
    if (op == '+') {
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

static int macro_parse_shift(ny_parser_t *p, const char *s, size_t n,
                             size_t *i, size_t *out) {
  size_t lhs = 0;
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
    size_t rhs = 0;
    if (!macro_parse_add(p, s, n, i, &rhs) || rhs >= sizeof(size_t) * 8u)
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

static int macro_parse_bitand(ny_parser_t *p, const char *s, size_t n,
                              size_t *i, size_t *out) {
  size_t lhs = 0;
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
    size_t rhs = 0;
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
    size_t rhs = 0;
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
    size_t rhs = 0;
    if (!macro_parse_shift(p, s, n, i, &rhs))
      return 0;
    lhs &= rhs;
  }
  *out = lhs;
  return 1;
}

static int macro_parse_bitxor(ny_parser_t *p, const char *s, size_t n,
                              size_t *i, size_t *out) {
  size_t lhs = 0;
  if (!macro_parse_bitand(p, s, n, i, &lhs))
    return 0;
  for (;;) {
    macro_skip_ws(s, n, i);
    if (*i >= n || s[*i] != '^')
      break;
    (*i)++;
    size_t rhs = 0;
    if (!macro_parse_bitand(p, s, n, i, &rhs))
      return 0;
    lhs ^= rhs;
  }
  *out = lhs;
  return 1;
}

static int macro_parse_bitor(ny_parser_t *p, const char *s, size_t n,
                             size_t *i, size_t *out) {
  size_t lhs = 0;
  if (!macro_parse_bitxor(p, s, n, i, &lhs))
    return 0;
  for (;;) {
    macro_skip_ws(s, n, i);
    if (*i >= n || s[*i] != '|' ||
        (*i + 1 < n && s[*i + 1] == '|'))
      break;
    (*i)++;
    size_t rhs = 0;
    if (!macro_parse_bitxor(p, s, n, i, &rhs))
      return 0;
    lhs |= rhs;
  }
  *out = lhs;
  return 1;
}

static int macro_parse_logand(ny_parser_t *p, const char *s, size_t n,
                              size_t *i, size_t *out) {
  size_t lhs = 0;
  if (!macro_parse_bitor(p, s, n, i, &lhs))
    return 0;
  for (;;) {
    macro_skip_ws(s, n, i);
    if (*i + 1 >= n || s[*i] != '&' || s[*i + 1] != '&')
      break;
    *i += 2;
    size_t rhs = 0;
    if (!macro_parse_bitor(p, s, n, i, &rhs))
      return 0;
    lhs = lhs && rhs;
  }
  *out = lhs;
  return 1;
}

static int macro_parse_logor(ny_parser_t *p, const char *s, size_t n,
                             size_t *i, size_t *out) {
  size_t lhs = 0;
  if (!macro_parse_logand(p, s, n, i, &lhs))
    return 0;
  for (;;) {
    macro_skip_ws(s, n, i);
    if (*i + 1 >= n || s[*i] != '|' || s[*i + 1] != '|')
      break;
    *i += 2;
    size_t rhs = 0;
    if (!macro_parse_logand(p, s, n, i, &rhs))
      return 0;
    lhs = lhs || rhs;
  }
  *out = lhs;
  return 1;
}

static int macro_parse_expr(ny_parser_t *p, const char *s, size_t n, size_t *i,
                            size_t *out) {
  size_t cond = 0;
  if (!macro_parse_logor(p, s, n, i, &cond))
    return 0;
  macro_skip_ws(s, n, i);
  if (*i >= n || s[*i] != '?') {
    *out = cond;
    return 1;
  }
  (*i)++;
  size_t when_true = 0;
  size_t when_false = 0;
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

static int parser_eval_define_value(ny_parser_t *p, const char *s, size_t n,
                                    size_t start, size_t *out) {
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
  if (p->pack_depth < NY_C_MAX_PACK_STACK)
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
                                         size_t n, size_t i, size_t *out) {
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
  size_t value = 0;
  return macro_lookup_name(p, s + name, i - name, &value);
}

static void parser_push_preproc_cond(ny_parser_t *p, int active) {
  if (!p || p->pp_depth >= NY_C_MAX_COND_STACK)
    return;
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
    size_t value = 0;
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
    size_t value = 0;
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
  if (word_len == 6 && strncmp(s + word, "define", 6) == 0) {
    if (i >= n || !c_ident_start_char(s[i]))
      return;
    size_t name = i;
    while (i < n && c_ident_char(s[i]))
      i++;
    size_t name_len = i - name;
    if (i < n && s[i] == '(')
      return;
    while (i < n && (s[i] == ' ' || s[i] == '\t'))
      i++;
    size_t value = 0;
    if (!parser_eval_define_value(p, s, n, i, &value))
      return;
    parser_note_define(p, s + name, name_len, value);
  } else if (word_len == 5 && strncmp(s + word, "undef", 5) == 0) {
    if (i >= n || !c_ident_start_char(s[i]))
      return;
    size_t name = i;
    while (i < n && c_ident_char(s[i]))
      i++;
    parser_forget_define(p, s + name, i - name);
  } else if (word_len == 6 && strncmp(s + word, "pragma", 6) == 0) {
    parser_note_pragma_pack(p, s, n, i);
  }
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
  return 0;
}

static int parse_storage(ny_parser_t *p, ny_cdecl_t *decl) {
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

static int parse_enum_body(ny_parser_t *p) {
  if (!parse_accept(p, "{"))
    return 1;
  size_t next_value = 0;
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
    }
    parser_note_define(p, name.start, name.len, value);
    next_value = value < ((size_t)-1) ? value + 1u : value;
    if (!parse_accept(p, ","))
      break;
  }
  if (!parse_accept(p, "}"))
    return parse_errorf(p, "expected '}' after C enum body at %u:%u",
                        p->tok.line, p->tok.col);
  return 1;
}

static int parse_tag_body(ny_parser_t *p, ny_ctype_t *ty) {
  if (!parse_is(p, "{"))
    return 1;
  if (ty && ty->kind == NY_CTYPE_ENUM)
    return parse_enum_body(p);
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
                                                      size));
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
        ny_parser_t save = *p;
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
            *p = save;
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
          *p = save;
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
                                                      size));
          function_pointers += c_type_function_pointer_slots(&next_ty);
          bitfield_unit_bits = 0;
          bitfield_used_bits = 0;
          aggregate_add_storage(ty, &next_layout, next_ty.align_override,
                                &size, &packed_size, &align);
        } else {
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
    if (!saw && p->tok.kind == NY_CTOK_IDENT) {
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
  return c_parse_integer_slice(tok.start, tok.len, &i, out) && i == tok.len;
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
                                 const ny_c_layout_t *layout, size_t offset) {
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
  f->type_name = field_ty->name;
  f->offset = offset;
  f->size = layout->size;
  f->align = layout->align;
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
  if (p->tok.kind == NY_CTOK_IDENT && parser_lookup_define(p, p->tok, out)) {
    parse_advance(p);
    return 1;
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

static int parse_array_extent_expr(ny_parser_t *p, size_t *out) {
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
    ny_parser_t save = *p;
    ny_ctype_t save_ty = *ty;
    parse_advance(p);
    while (parse_type_marker(p, ty))
      ;
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
    *p = save;
    *ty = save_ty;
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
    ny_parser_t save = *p;
    parse_advance(p);
    if (parse_accept(p, ")"))
      return 1;
    *p = save;
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
  if (!parse_kw(p, "_Static_assert") && !parse_kw(p, "static_assert"))
    return 0;
  parse_advance(p);
  if (parse_is(p, "("))
    skip_balanced(p, "(", ")");
  parse_accept(p, ";");
  return 1;
}

void ny_parse_init_abi(ny_parser_t *p, const char *src, size_t len,
                       const char *abi) {
  if (!p)
    return;
  memset(p, 0, sizeof(*p));
  p->abi = abi;
  ny_lex_init(&p->lx, src, len);
  p->tok = ny_lex_next(&p->lx);
}

void ny_parse_init(ny_parser_t *p, const char *src, size_t len) {
  ny_parse_init_abi(p, src, len, NULL);
}

int ny_parse_decl(ny_parser_t *p, ny_cdecl_t *out) {
  if (!p || !out)
    return -1;
  memset(out, 0, sizeof(*out));
  out->kind = NY_CDECL_VAR;
  out->name = cempty_tok();
  type_init(&out->type);
  skip_preproc(p);
  while (parse_non_import_decl(p)) {
    skip_preproc(p);
  }
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
  skip_to_decl_end(p);
  return 1;
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
    size_t value = 0;
    return macro_lookup_name(p, src + name, pos - name, &value);
  }
  return 0;
}

static int ny_c_preproc_eval_condition(ny_parser_t *p, const char *src,
                                       size_t len, size_t expr, size_t line_end,
                                       size_t *out) {
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
  size_t value = 0;
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
            size_t value = 0;
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
  ny_parser_t p;
  ny_parse_init(&p, src, len);
  while (p.tok.kind != NY_CTOK_EOF) {
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
      continue;
    }
    if (rc < 0) {
      local.unsupported++;
      if (err && err_len > 0 && err[0] == '\0')
        snprintf(err, err_len, "%s", ny_parse_error(&p));
      continue;
    }
    break;
  }
  if (summary)
    *summary = local;
  return local.unsupported == 0;
}
