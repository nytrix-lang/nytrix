long c_pointer_add(const long *items, long index) { return *(items + index); }

long c_pointer_add_reversed(const long *items, long index) {
  return *(index + items);
}

long c_pointer_subtract(const long *items, long index) {
  return *(items + 4 - index);
}

long c_pointer_compound_offset(long *items, long index) {
  long *current = items;
  current += index;
  long selected = *current;
  current -= index;
  return selected + *current;
}

long c_pointer_postfix_increment(long *items) {
  long *current = items;
  long previous = *current++;
  return previous + *current;
}

long c_pointer_prefix_increment(long *items) {
  long *current = items;
  return *++current;
}

long c_pointer_prefix_decrement(long *items) {
  long *current = items + 1;
  return *--current;
}

struct c_pointer_stride_item {
  long first;
  long second;
  long third;
};

long c_aggregate_pointer_postfix_increment(
    struct c_pointer_stride_item *items) {
  struct c_pointer_stride_item *current = items;
  current += 1;
  return current->first;
}

long c_aggregate_pointer_index(struct c_pointer_stride_item *items,
                               long index) {
  return items[index].first;
}

struct c_pointer_stride_56 {
  long fields[7];
};

long c_aggregate_pointer_index_stride_56(struct c_pointer_stride_56 *items,
                                         long index) {
  return items[index].fields[6];
}

typedef struct c_pointer_register c_mem_register;

struct c_vdbe_pointer_layout {
  void *db;
  void *previous;
  void *next;
  void *parse;
  short variable_count;
  int memory_count;
  int cursor_count;
  unsigned int cache_counter;
  int program_counter;
  int result_code;
  long change_count;
  int statement;
  long current_time;
  long foreign_key_count;
  long deferred_count;
  long immediate_count;
  c_mem_register *memory;
};

long c_vdbe_register_index(struct c_vdbe_pointer_layout *vm, long index) {
  return vm->memory[index].fields[6];
}

struct c_pointer_register {
  long fields[7];
};

c_mem_register *c_vdbe_output(struct c_vdbe_pointer_layout *vm, long index) {
  return &vm->memory[index];
}

long c_vdbe_register_output_index(struct c_vdbe_pointer_layout *vm,
                                  long index) {
  c_mem_register *output = c_vdbe_output(vm, index);
  return output->fields[6];
}

long c_vdbe_register_cast_index(void *memory, long index) {
  return ((struct c_pointer_register *)memory)[index].fields[6];
}

struct c_pointer_stride_box {
  struct c_pointer_stride_item *top;
};

long c_aggregate_pointer_field_increment(struct c_pointer_stride_box *box) {
  box->top++;
  return box->top->first;
}

struct c_bitfield_layout {
  unsigned char first;
  unsigned char second;
  unsigned int low : 1;
  unsigned int high : 1;
  unsigned int flag : 1;
  void *pointer;
};

long c_bitfield_layout_pointer_offset(struct c_bitfield_layout *value) {
  return (char *)&value->pointer - (char *)value;
}

long c_const_data_pointer_increment(const long *items) {
  const long *current = items;
  long previous = *current++;
  current = items;
  current += 1;
  return previous + *current;
}

double c_double_pointer_add(const double *items, long index) {
  return *(items + index);
}

double c_double_pointer_compound_offset(double *items, long index) {
  double *current = items;
  current += index;
  double selected = *current;
  current -= index;
  return selected + *current;
}

double c_double_pointer_postfix_increment(double *items) {
  double *current = items;
  double previous = *current++;
  return previous + *current;
}
