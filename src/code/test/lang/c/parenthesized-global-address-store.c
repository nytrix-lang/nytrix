/*
 * Fixture: store through a parenthesized casted address of a global.
 */
static unsigned int value;

int main(void) {
  *((unsigned int *)&(value)) = 7;
  return value != 7;
}
