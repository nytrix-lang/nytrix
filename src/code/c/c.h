#ifndef NY_CODE_C_H
#define NY_CODE_C_H

#include <stddef.h>

#define NY_C_MAX_PARAMS 16
#define NY_C_MAX_TYPEDEFS 64
#define NY_C_MAX_DEFINES 64
#define NY_C_MAX_TAGS 64
#define NY_C_MAX_FIELDS 32
#define NY_C_MAX_PACK_STACK 16
#define NY_C_MAX_COND_STACK 32

typedef enum {
  NY_CTOK_EOF = 0,
  NY_CTOK_IDENT,
  NY_CTOK_NUMBER,
  NY_CTOK_STRING,
  NY_CTOK_CHAR,
  NY_CTOK_PUNCT,
  NY_CTOK_PREPROC,
} ny_ctok_kind_t;

typedef struct {
  ny_ctok_kind_t kind;
  const char *start;
  size_t len;
  unsigned line;
  unsigned col;
} ny_ctok_t;

typedef struct {
  const char *src;
  size_t len;
  size_t pos;
  unsigned line;
  unsigned col;
} ny_lexer_t;

typedef enum {
  NY_CTYPE_INVALID = 0,
  NY_CTYPE_VOID,
  NY_CTYPE_BOOL,
  NY_CTYPE_CHAR,
  NY_CTYPE_SHORT,
  NY_CTYPE_INT,
  NY_CTYPE_LONG,
  NY_CTYPE_FLOAT,
  NY_CTYPE_DOUBLE,
  NY_CTYPE_LONG_DOUBLE,
  NY_CTYPE_STRUCT,
  NY_CTYPE_UNION,
  NY_CTYPE_ENUM,
  NY_CTYPE_NAMED,
} ny_ctype_kind_t;

typedef enum {
  NY_CDECL_NONE = 0,
  NY_CDECL_VAR,
  NY_CDECL_FUNC,
  NY_CDECL_TYPEDEF,
} ny_cdecl_kind_t;

typedef enum {
  NY_CTYPEF_CONST = 1u << 0,
  NY_CTYPEF_VOLATILE = 1u << 1,
  NY_CTYPEF_SIGNED = 1u << 2,
  NY_CTYPEF_UNSIGNED = 1u << 3,
  NY_CTYPEF_LONG_LONG = 1u << 4,
  NY_CTYPEF_PACKED = 1u << 5,
  NY_CTYPEF_FUNCTION_PTR = 1u << 6,
} ny_ctype_flags_t;

typedef enum {
  NY_CDECLF_EXTERN = 1u << 0,
  NY_CDECLF_STATIC = 1u << 1,
  NY_CDECLF_INLINE = 1u << 2,
  NY_CDECLF_NORETURN = 1u << 3,
} ny_cdecl_flags_t;

typedef struct {
  ny_ctok_t name;
  ny_ctype_kind_t kind;
  unsigned flags;
  unsigned ptr_depth;
  ny_ctok_t type_name;
  size_t offset;
  size_t size;
  size_t align;
} ny_c_field_t;

typedef struct {
  ny_ctype_kind_t kind;
  unsigned flags;
  unsigned ptr_depth;
  size_t array_elems;
  unsigned array_unknown;
  unsigned array_invalid;
  ny_ctok_t name;
  unsigned align_override;
  unsigned aggregate_fields;
  size_t aggregate_size;
  size_t aggregate_align;
  size_t aggregate_packed_size;
  unsigned aggregate_pack_align;
  unsigned aggregate_function_pointers;
  unsigned aggregate_has_layout;
  unsigned field_count;
  ny_c_field_t fields[NY_C_MAX_FIELDS];
} ny_ctype_t;

typedef struct {
  size_t size;
  size_t align;
  unsigned is_integer;
  unsigned is_float;
  unsigned is_pointer;
} ny_c_layout_t;

typedef struct {
  ny_cdecl_kind_t kind;
  unsigned flags;
  ny_ctype_t type;
  ny_ctok_t name;
  unsigned param_count;
  ny_ctype_t params[NY_C_MAX_PARAMS];
  ny_ctok_t param_names[NY_C_MAX_PARAMS];
  unsigned is_variadic;
} ny_cdecl_t;

typedef struct {
  ny_lexer_t lx;
  ny_ctok_t tok;
  const char *abi;
  unsigned typedef_count;
  ny_ctok_t typedef_names[NY_C_MAX_TYPEDEFS];
  ny_ctype_t typedef_types[NY_C_MAX_TYPEDEFS];
  unsigned define_count;
  ny_ctok_t define_names[NY_C_MAX_DEFINES];
  size_t define_values[NY_C_MAX_DEFINES];
  unsigned tag_count;
  ny_ctok_t tag_names[NY_C_MAX_TAGS];
  ny_ctype_t tag_types[NY_C_MAX_TAGS];
  unsigned pack_align;
  unsigned pack_depth;
  unsigned pack_stack[NY_C_MAX_PACK_STACK];
  unsigned pp_depth;
  unsigned pp_parent_active[NY_C_MAX_COND_STACK];
  unsigned pp_branch_taken[NY_C_MAX_COND_STACK];
  unsigned pp_active[NY_C_MAX_COND_STACK];
  char error[256];
} ny_parser_t;

typedef struct {
  size_t declarations;
  size_t functions;
  size_t variables;
  size_t typedefs;
  size_t tag_decls;
  size_t aggregate_layouts;
  size_t aggregate_fields;
  size_t aggregate_bytes;
  size_t function_pointers;
  size_t preprocessor_lines;
  size_t include_lines;
  size_t define_lines;
  size_t object_like_define_lines;
  size_t function_like_define_lines;
  size_t unsupported_define_lines;
  size_t undef_lines;
  size_t conditional_lines;
  size_t conditional_active_lines;
  size_t conditional_inactive_lines;
  size_t unsupported;
} ny_c_header_summary_t;

void ny_lex_init(ny_lexer_t *lx, const char *src, size_t len);
ny_ctok_t ny_lex_next(ny_lexer_t *lx);
const char *ny_ctok_kind_name(ny_ctok_kind_t kind);
int ny_ctok_eq(ny_ctok_t tok, const char *lit);
int ny_ctok_is_ident(ny_ctok_t tok, const char *lit);

void ny_parse_init(ny_parser_t *p, const char *src, size_t len);
void ny_parse_init_abi(ny_parser_t *p, const char *src, size_t len,
                       const char *abi);
int ny_parse_decl(ny_parser_t *p, ny_cdecl_t *out);
int ny_parse_header_summary(const char *src, size_t len,
                            ny_c_header_summary_t *summary, char *err,
                            size_t err_len);
const char *ny_ctype_kind_name(ny_ctype_kind_t kind);
const char *ny_cdecl_kind_name(ny_cdecl_kind_t kind);
int ny_ctype_layout(const ny_ctype_t *ty, const char *abi, ny_c_layout_t *out);
const char *ny_parse_error(const ny_parser_t *p);

#endif
