struct nyc_target {
  long value;
};

struct nyc_holder {
  struct nyc_target ***targets;
};

static struct nyc_holder holder;
static struct nyc_target target;
static struct nyc_target *target_pointer = &target;
static struct nyc_target **target_paths[2] = {0, &target_pointer};
static struct nyc_target *target_row[1] = {&target};
static struct nyc_target **target_table[1] = {target_row};
static struct nyc_target ***target_global = target_table;
static struct nyc_target target_array[1];
typedef struct nyc_target nyc_target_array[1];
static nyc_target_array *target_array_pointer = &target_array;
typedef struct nyc_target *nyc_target_vector[4];
static nyc_target_vector target_vector;
typedef nyc_target_vector nyc_target_vectors[2];
static nyc_target_vectors target_vectors;
typedef int nyc_function_type(int);
static int nyc_function_target(int value) { return value + 1; }

static int nyc_typeof_function_pointer(nyc_function_type *handler, int value) {
  typeof(*handler) *copy = handler;
  return copy(value);
}

struct nyc_target ***nyc_get_targets(void) { return holder.targets; }

static void nyc_nested_address_dereference(void) {
  *(*(&target_pointer)) = (struct nyc_target){.value = 41};
}

static int nyc_goto_after_case(int value) {
  switch (value) {
  case 1:
    goto target_label;
  case 2:
  target_label:
    return 3;
  default:
    return 0;
  }
}

static struct nyc_target ***nyc_triple_identity(struct nyc_target ***paths) {
  struct nyc_target ***local = paths;
  return local;
}

static struct nyc_target **nyc_hop(struct nyc_target ***paths) {
  struct nyc_target **previous, **current = *(struct nyc_target ***)paths;
  previous = *((struct nyc_target ***)paths - 1);
  return current ? current : previous;
}

static long nyc_distance(struct nyc_holder value,
                         struct nyc_target ***current) {
  return current - value.targets;
}

static long nyc_local_distance(struct nyc_holder value,
                               struct nyc_target ***input) {
  struct nyc_target ***current = input;
  return current - value.targets;
}

static int nyc_nested_pointer_update(void) {
  struct nyc_target ***masks = &target_table[0];
  masks[0][0] = &target;
  return masks[0][0] != &target;
}

static int nyc_global_pointer_index(void) {
  return target_global[0][0] != &target;
}

static int nyc_global_relocated_pointer_index(void) {
  return ((__typeof__(target_global))((unsigned long)target_global +
                                      0))[0][0] != &target;
}

static int nyc_global_array_pointer_index(void) {
  return target_array_pointer[0] != target_array;
}

static int nyc_relocated_vector_assignment(void) {
  (*(__typeof__(&target_vector))((unsigned long)&target_vector + 0))[2] =
      &target;
  (*(__typeof__(&target_vectors))((unsigned long)&target_vectors + 0))[1][3] =
      &target;
  return target_vector[2] != &target || target_vectors[1][3] != &target ||
         target_vectors[0][3] != 0;
}

int main(void) {
  holder.targets = &target_paths[0];
  nyc_nested_address_dereference();
  return nyc_goto_after_case(1) != 3 ||
         nyc_typeof_function_pointer(nyc_function_target, 40) != 41 ||
         target.value != 41 ||
         nyc_get_targets() != holder.targets ||
         sizeof(holder.targets) != sizeof(void *) ||
         nyc_triple_identity(holder.targets) != holder.targets ||
         nyc_hop(&target_paths[1]) != &target_pointer ||
         nyc_distance(holder, &target_paths[1]) != 1 ||
         nyc_local_distance(holder, &target_paths[1]) != 1 ||
         nyc_nested_pointer_update() || nyc_global_pointer_index() ||
         nyc_global_relocated_pointer_index() ||
         nyc_global_array_pointer_index() || nyc_relocated_vector_assignment();
}
