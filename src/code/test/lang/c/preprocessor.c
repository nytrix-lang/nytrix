/*
 * Parser fixture: exercise conditional definitions in preprocessed C source.
 */
#define NYC_FEATURE_ENABLED

#ifdef NYC_FEATURE_ENABLED
long c_pp_defined(void) { return 41; }
#else
long c_pp_defined(void) { return 0; }
#endif

#if defined(NYC_FEATURE_ENABLED) && !defined(NYC_FEATURE_DISABLED)
long c_pp_defined_expr(void) { return 1; }
#elif 1
long c_pp_defined_expr(void) { return 0; }
#else
long c_pp_defined(void) { return 0; }
#endif

#define NYC_FEATURE_LEVEL 3
#if NYC_FEATURE_LEVEL >= 2 && NYC_FEATURE_LEVEL < 4 /* expression comment */
long c_pp_integer_macro(void) { return 1; }
#else
long c_pp_integer_macro(void) { return 0; }
#endif

#define NYC_FEATURE_ALIAS NYC_FEATURE_LEVEL
#if NYC_FEATURE_ALIAS == 3
long c_pp_integer_alias(void) { return 1; }
#else
long c_pp_integer_alias(void) { return 0; }
#endif

#define NYC_FEATURE_CALC (NYC_FEATURE_LEVEL * 10 + 2)
#if NYC_FEATURE_CALC == 32
long c_pp_integer_expression(void) { return 1; }
#else
long c_pp_integer_expression(void) { return 0; }
#endif

#if (NYC_FEATURE_LEVEL & 1) == 1
long c_pp_bitwise_expression(void) { return 1; }
#else
long c_pp_bitwise_expression(void) { return 0; }
#endif

#if (NYC_FEATURE_LEVEL << 2) == 12 && (12 >> 2) == 3
long c_pp_shift_expression(void) { return 1; }
#else
long c_pp_shift_expression(void) { return 0; }
#endif

#if 'A' == '\101'
long c_pp_character_constant(void) { return 1; }
#else
long c_pp_character_constant(void) { return 0; }
#endif

#if 0 /* comment spanning the directive line                                   \
      ** remains whitespace to the condition parser */
long c_pp_multiline_comment(void) { return 0; }
#else
long c_pp_multiline_comment(void) { return 1; }
#endif

#if defined(NYC_FEATURE_ENABLED) && !defined(NYC_FEATURE_DISABLED) &&          \
    _MSC_VER < 1800
long c_pp_undefined_macro_value(void) { return 1; }
#else
long c_pp_undefined_macro_value(void) { return 0; }
#endif

#ifndef NYC_FEATURE_DISABLED
long c_pp_undefined(void) { return 1; }
#else
long c_pp_undefined(void) { return 0; }
#endif

#undef NYC_FEATURE_ENABLED
#ifdef NYC_FEATURE_ENABLED
long c_pp_undefined_after_undef(void) { return 0; }
#else
long c_pp_undefined_after_undef(void) { return 1; }
#endif

#if 0
#ifdef NYC_IGNORED_IN_INACTIVE_BRANCH
this is not C and must remain unparsed
#endif
#endif

#define PP_STRINGIFY_RAW(value) #value
#define PP_STRINGIFY(value) PP_STRINGIFY_RAW(value)
#define PP_VALUE 42
#define PP_NEGATIVE -2000
#define PP_ID(value) value
#define PP_PREFIX answer
#define PP_JOIN_RAW(a, b) a##b
#define PP_JOIN(a, b) PP_JOIN_RAW(a, b)
long answer42 = 9;
long PP_PREFIXPP_VALUE = 17;

const char *c_pp_expanded_string(void) { return PP_STRINGIFY(PP_VALUE); }
const char *c_pp_raw_string(void) { return PP_STRINGIFY_RAW(PP_VALUE); }
const char *c_pp_negative_string(void) { return PP_STRINGIFY(PP_NEGATIVE); }
const char *c_pp_nested_string(void) { return PP_STRINGIFY(PP_ID(PP_VALUE)); }
long c_pp_expanded_paste(void) { return PP_JOIN(PP_PREFIX, PP_VALUE); }
long c_pp_raw_paste(void) { return PP_JOIN_RAW(PP_PREFIX, PP_VALUE); }

long c_pp_line_order(void) {
  long earlier = __LINE__;
  long later = __LINE__;
  return later > earlier;
}
