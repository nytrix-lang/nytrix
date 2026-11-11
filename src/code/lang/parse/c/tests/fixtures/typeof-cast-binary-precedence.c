/*
 * C parser fixture for precedence in typeof casts and binary expressions.
 */
struct node {
  unsigned long value;
};
static struct node object;
static struct node *pop(void) { return &object; }

int main(void) {
  unsigned long tag = 0;
  typeof(*((struct node *)((unsigned long)pop() | tag))) *pointer =
      (struct node *)((unsigned long)pop() | tag);
  pointer->value = 23;
  return object.value != 23;
}
