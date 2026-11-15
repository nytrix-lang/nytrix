int strong_body = 11;
__attribute__((weak)) int weak_reader(void) { return strong_body; }

int strong_parameter = 13;
extern __attribute__((weak)) int weak_prototype(int strong_parameter);
int strong_typeof = 17;
extern __attribute__((weak)) __typeof__(strong_typeof)
weak_typed_prototype(void);

typedef int weak;
weak strong_type_name = 19;

int suffix_weak __attribute__((weak)) = 23, suffix_strong = 29;
int pointer_target = 31;
int *__attribute__((weak)) pointer_weak = &pointer_target,
                           pointer_neighbor = 37;
__attribute__((weak)) int common_a = 41, common_b = 43;

int initializer_strong = 47;
int *initializer_weak __attribute__((weak)) = &initializer_strong;

static int local_chain = 53;
extern int local_chain;

int scalar_first = 79, *trailing_pointer = &pointer_target;
int scalar_first, *trailing_pointer;
float *single_pointer = 0, single_value = 1.25f;
double *double_pointer = 0, double_value = 2.5;

int check_binding_scope(void) {
  if (strong_body != 11 || weak_reader() != 11)
    return 1;
  if (strong_parameter != 13 || strong_typeof != 17 || strong_type_name != 19)
    return 2;
  if (suffix_weak != 59 || suffix_strong != 29)
    return 3;
  if (!pointer_weak || *pointer_weak != 61 || pointer_target != 31 ||
      pointer_neighbor != 37)
    return 4;
  if (common_a != 67 || common_b != 71)
    return 5;
  if (initializer_strong != 47 || !initializer_weak || *initializer_weak != 61)
    return 6;
  if (local_chain != 53)
    return 7;
  if (scalar_first != 79 || !trailing_pointer || *trailing_pointer != 31)
    return 8;
  if (single_pointer || single_value != 1.25f || double_pointer ||
      double_value != 2.5)
    return 9;
  return 0;
}
