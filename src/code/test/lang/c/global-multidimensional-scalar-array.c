/*
 * C parser fixture for global multidimensional scalar-array initialization.
 */
struct nyc_matrix_fixture {
  signed char counters[2][3];
  unsigned int ids[2][2];
};
struct nyc_array_target {
  unsigned int value;
};
struct nyc_pointer_array_fixture {
  struct nyc_array_target *targets[2];
};
struct nyc_pointer_matrix_fixture {
  unsigned short **rows[2];
};
struct nyc_topology_entry {
  void *untyped;
};
struct nyc_topology_holder {
  struct nyc_topology_entry **topology;
};
struct nyc_void_pointer_array {
  void *slots[3];
};
struct nyc_array_leaf {
  unsigned int value;
};
struct nyc_array_leaf_matrix {
  struct nyc_array_leaf leaves[2][2][2];
};
struct nyc_zero_pair {
  int left, right;
};

static struct nyc_matrix_fixture matrix = {
    .counters = {{4, 5, -1}, {6, 7, -1}},
    .ids = {{17, 18}, {19, 20}},
};
static unsigned long words[2];
static struct nyc_array_target target = {.value = 23};
static struct nyc_array_target *target_array[3]
    __attribute__((section(".data.nyc_pointer_table"))) = {[2] = &target};
extern int inferred_units[];
int inferred_units[] = {31, 32, 33};
extern int global_zero_list_left, global_zero_list_right;
int global_zero_list_left, global_zero_list_right;
struct nyc_zero_pair global_zero_pair;
extern struct nyc_zero_pair external_zero_pair;
struct nyc_zero_pair external_zero_pair;
struct nyc_zero_pair global_pair, *global_pair_pointer = &global_pair;
static struct nyc_pointer_array_fixture pointers = {
    .targets[1] = &target,
};
static unsigned short pointer_matrix_value[1];
static unsigned short *pointer_matrix_row[1] = {pointer_matrix_value};
static struct nyc_pointer_matrix_fixture pointer_matrix = {
    .rows[1] = pointer_matrix_row,
};
static unsigned short *pointer_table[2][3] = {
    [1][2] = pointer_matrix_value,
};
static struct nyc_array_target *target_matrix[2][3];
static struct nyc_topology_entry topology_entries[1];
static struct nyc_topology_entry *topology_rows[1] = {topology_entries};
static struct nyc_topology_holder topology_holder = {
    .topology = topology_rows,
};

static void set_topology_entry(struct nyc_topology_entry **topology) {
  topology[0][0].untyped = (void *)1;
}
static void set_topology_holder_entry(struct nyc_topology_holder *type) {
  type->topology[0][0].untyped = (void *)2;
}
static void set_pointer_row(unsigned short **rows, unsigned short *row) {
  rows[0] = row;
}
static void set_u16_value(unsigned short *row, unsigned short value) {
  row[0] = value;
}
static long void_pointer_slot_offset(struct nyc_void_pointer_array *array,
                                     void **slot) {
  return slot - array->slots;
}
static void *extract_return_address(void *address) {
  return __builtin_extract_return_addr(address);
}
static unsigned long sum_local_declarators_with_attributes(void) {
  unsigned long first = 1, __attribute__((unused)) second = 2;
  return first + second;
}
static struct nyc_array_leaf *
last_array_leaf(struct nyc_array_leaf_matrix *matrix) {
  return &matrix->leaves[1][1][1];
}
static void *copy_void_pointer_slot(void **a, void **b) {
  void **p = a, **destination = b;
  destination[0] = p[0];
  return destination[0];
}
static void update_pointer_table(struct nyc_pointer_matrix_fixture *map,
                                 unsigned short **rows, unsigned short *row) {
  unsigned short **dir, *selected;
  dir = map->rows[1];
  if (!dir)
    dir = map->rows[1] = rows;
  selected = dir[0];
  if (!selected)
    selected = dir[0] = row;
  selected[0] = 0x66;
}
static void set_target_matrix(int row, int column,
                              struct nyc_array_target *value) {
  target_matrix[row][column] = value;
}

static unsigned long *typeof_array_pointer(void) {
  return *(typeof(*(&words)) *)(&words);
}

int main(void) {
  set_topology_entry(topology_rows);
  set_topology_holder_entry(&topology_holder);
  unsigned short *pointer_rows[1] = {0};
  set_u16_value(pointer_matrix_value, 0x55);
  struct nyc_pointer_matrix_fixture local_pointer_matrix = {0};
  update_pointer_table(&local_pointer_matrix, pointer_rows,
                       pointer_matrix_value);
  set_target_matrix(1, 2, &target);
  void *void_pointer_source[1] = {pointer_matrix_value};
  void *void_pointer_destination[1] = {0};
  struct nyc_void_pointer_array local_void_pointer_array = {{0}};
  struct nyc_array_leaf_matrix local_array_leaf_matrix = {0};
  void *copied_void_pointer =
      copy_void_pointer_slot(void_pointer_source, void_pointer_destination);
  return topology_entries[0].untyped != (void *)2 ||
         typeof_array_pointer() != words || target_array[2] != &target ||
         target_matrix[1][2] != &target || target_matrix[0][0] != 0 ||
         target_array[1] != 0 || pointers.targets[1] != &target ||
         pointer_matrix.rows[1] != pointer_matrix_row ||
         pointer_matrix.rows[1][0] != pointer_matrix_value ||
         pointer_table[1][2] != pointer_matrix_value ||
         pointer_table[0][0] != 0 || pointer_rows[0] != pointer_matrix_value ||
         copied_void_pointer != pointer_matrix_value ||
         void_pointer_slot_offset(&local_void_pointer_array,
                                  &local_void_pointer_array.slots[2]) != 2 ||
         extract_return_address((void *)3) != (void *)3 || '\e' != 27 ||
         sum_local_declarators_with_attributes() != 3 ||
         last_array_leaf(&local_array_leaf_matrix) !=
             &local_array_leaf_matrix.leaves[1][1][1] ||
         pointer_matrix_value[0] != 0x66 || inferred_units[0] != 31 ||
         local_pointer_matrix.rows[1][0][0] != 0x66 ||
         inferred_units[2] != 33 || global_zero_list_left != 0 ||
         global_zero_list_right != 0 || global_zero_pair.left != 0 ||
         global_zero_pair.right != 0 || external_zero_pair.left != 0 ||
         external_zero_pair.right != 0 || global_pair_pointer != &global_pair ||
         matrix.counters[0][0] != 4 || matrix.counters[0][2] != -1 ||
         matrix.counters[1][1] != 7 || matrix.counters[1][2] != -1 ||
         matrix.ids[0][1] != 18 || matrix.ids[1][0] != 19 ||
         matrix.ids[1][1] != 20;
}
