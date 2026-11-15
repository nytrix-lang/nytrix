/*
 * Fixture: replicate a local aggregate initializer over a GNU index range.
 */
static int target = 19;

struct item {
  int value;
  int *pointer;
};

int main(void) {
  struct item items[4] = {[1 ... 2] = {.value = 7, .pointer = &target}};
  struct item simple[2] = {[0 ... 1] = {23, 0}};
  return items[0].value == 0 && items[1].value == 7 && items[2].value == 7 &&
                 items[3].value == 0 && items[0].pointer == 0 &&
                 items[1].pointer == &target && items[2].pointer == &target &&
                 items[3].pointer == 0 && simple[0].value == 23 &&
                 simple[1].value == 23
             ? 0
             : 1;
}
