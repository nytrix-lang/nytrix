/*
 * Fixture: access an aggregate-array member through the arrow operator.
 */
struct item {
  int value;
};

int main(void) {
  struct item values[2];
  values->value = 3;
  values[1].value = 5;
  return values[0].value != 3 || values[1].value != 5;
}
