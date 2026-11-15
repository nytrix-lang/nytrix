/*
 * C parser fixture for const aggregate pointers inferred through typeof.
 */
struct item {
  int value;
};
static struct item object = {17};
extern struct item *const pointer;
struct item *const pointer = &object;
extern typeof(pointer) pointer;

int main(void) { return pointer->value != 17; }
