typedef unsigned long size_t;

struct object_with_array {
  char name[16];
};

static size_t member_size(struct object_with_array *object) {
  return __builtin_object_size(object->name, 1);
}

int main(void) {
  struct object_with_array object = {{0}};
  return member_size(&object) != sizeof(object.name);
}
