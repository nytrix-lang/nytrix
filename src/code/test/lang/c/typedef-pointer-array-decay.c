/*
 * C parser fixture for array typedef decay in aggregate and pointer contexts.
 */
struct object {
  int value;
};
typedef struct object *object_pointers[4];

static struct object object_value;
extern typeof(object_pointers) pointers;
object_pointers pointers = {&object_value};
static void *identity(const void *pointer) { return (void *)pointer; }

int main(void) {
  return identity((*((typeof(*(&pointers)) *)&pointers))[0]) != &object_value;
}
