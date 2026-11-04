struct nyc_pointer_initializer_value {
  int value;
};

int main(void) {
  struct nyc_pointer_initializer_value object = {37};
  struct nyc_pointer_initializer_value *pointer = pointer = &object;
  char first[2] = "a";
  char second[2] = "b";
  char *parts[2];
  parts[0] = first;
  parts[1] = second;
  parts[1][0] = 'z';
  return pointer->value == 37 && parts[0][0] == 'a' && parts[1][0] == 'z' ? 0
                                                                          : 1;
}
