/*
 * Fixture: initialize rows of a file-scope function-pointer matrix by
 * designator.
 */
typedef int (*operation_fn)(int);

static int increment(int value) { return value + 1; }
static int twice(int value) { return value * 2; }

static operation_fn operations[2][2] = {
    [0] = {[0] = increment, [1] = twice},
    [1] = {[0] = twice, [1] = increment},
};

int main(void) {
  return operations[0][0](2) == 3 && operations[0][1](3) == 6 &&
                 operations[1][0](4) == 8 && operations[1][1](5) == 6
             ? 0
             : 1;
}
