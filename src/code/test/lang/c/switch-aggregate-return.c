/*
 * Regression: all-returning switch arms preserve aggregate function results.
 */
typedef struct {
  unsigned value;
} result_t;

static result_t choose_result(unsigned selector) {
  switch (selector) {
  case 0:
    return (result_t){11};
  case 1:
    return (result_t){29};
  default:
    return (result_t){47};
  }
}

int main(void) {
  return choose_result(0).value != 11 || choose_result(1).value != 29 ||
         choose_result(2).value != 47;
}
