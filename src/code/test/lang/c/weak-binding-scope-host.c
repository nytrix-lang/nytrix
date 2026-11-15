__attribute__((weak)) int strong_body = -11;
__attribute__((weak)) int strong_parameter = -13;
__attribute__((weak)) int strong_typeof = -17;
__attribute__((weak)) int strong_type_name = -19;
__attribute__((weak)) int suffix_strong = -29;
__attribute__((weak)) int pointer_target = -31;
__attribute__((weak)) int pointer_neighbor = -37;
__attribute__((weak)) int initializer_strong = -47;

int suffix_weak = 59;
static int override_target = 61;
int *pointer_weak = &override_target;
int common_a = 67, common_b = 71;
int *initializer_weak = &override_target;
int local_chain = 73;

extern int check_binding_scope(void);

int main(void) {
  if (strong_body != 11 || strong_parameter != 13 || strong_typeof != 17 ||
      strong_type_name != 19)
    return 10;
  if (suffix_strong != 29 || pointer_target != 31 || pointer_neighbor != 37 ||
      initializer_strong != 47)
    return 11;
  if (suffix_weak != 59 || pointer_weak != &override_target || common_a != 67 ||
      common_b != 71 || initializer_weak != &override_target ||
      local_chain != 73)
    return 12;
  return check_binding_scope();
}
