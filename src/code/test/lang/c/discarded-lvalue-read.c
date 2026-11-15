/*
 * Discarding nonvolatile lvalues must not introduce external reads.
 */
struct token {};
extern const struct token *token;
extern int unused_scalar;
extern const int unused_const;
static volatile int observed = 7;
static int calls;
static int values[2] = {3, 5};
static int *address(void) {
  ++calls;
  return values;
}
int main(void) {
  (void)(token);
  (void)unused_scalar;
  (void)unused_const;
  (void)observed;
  (void)*address();
  int index = 0;
  (void)values[index++];
  int comma = ((void)token, (void)unused_scalar, (void)values[index++], 11);
  int selected = 0;
  if (1 && (selected = 1, (void)selected, selected = 2, 1))
    ++selected;
  return calls != 1 || index != 2 || observed != 7 || comma != 11 ||
         selected != 3;
}
