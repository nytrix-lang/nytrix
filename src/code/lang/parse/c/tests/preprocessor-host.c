#include <string.h>

extern long c_pp_defined(void);
extern long c_pp_defined_expr(void);
extern long c_pp_integer_macro(void);
extern long c_pp_integer_alias(void);
extern long c_pp_integer_expression(void);
extern long c_pp_bitwise_expression(void);
extern long c_pp_shift_expression(void);
extern long c_pp_character_constant(void);
extern long c_pp_multiline_comment(void);
extern long c_pp_undefined_macro_value(void);
extern long c_pp_undefined(void);
extern long c_pp_undefined_after_undef(void);
extern const char *c_pp_expanded_string(void);
extern const char *c_pp_raw_string(void);
extern const char *c_pp_negative_string(void);
extern const char *c_pp_nested_string(void);
extern long c_pp_expanded_paste(void);
extern long c_pp_raw_paste(void);
extern long c_pp_line_order(void);

int main(void) {
  return c_pp_defined() == 41 && c_pp_defined_expr() == 1 &&
                 c_pp_integer_macro() == 1 && c_pp_integer_alias() == 1 &&
                 c_pp_integer_expression() == 1 &&
                 c_pp_bitwise_expression() == 1 &&
                 c_pp_shift_expression() == 1 &&
                 c_pp_character_constant() == 1 &&
                 c_pp_multiline_comment() == 1 &&
                 c_pp_undefined_macro_value() == 1 && c_pp_undefined() == 1 &&
                 c_pp_undefined_after_undef() == 1 &&
                 strcmp(c_pp_expanded_string(), "42") == 0 &&
                 strcmp(c_pp_raw_string(), "PP_VALUE") == 0 &&
                 strcmp(c_pp_negative_string(), "-2000") == 0 &&
                 strcmp(c_pp_nested_string(), "42") == 0 &&
                 c_pp_expanded_paste() == 9 && c_pp_raw_paste() == 17 &&
                 c_pp_line_order() == 1
             ? 0
             : 1;
}
