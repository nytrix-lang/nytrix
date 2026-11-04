struct nyc_volatile_counter {
  unsigned int value;
};

int main(void) {
  struct nyc_volatile_counter object = {4};
  unsigned int previous = (*(volatile unsigned int *)&object.value)++;
  return previous == 4 && object.value == 5 ? 0 : 1;
}
