/*
 * Aggregate ABI coverage: struct parameters by value (register-class,
 * MEMORY-class, and mixed INTEGER/SSE), aggregate returns (two-eightbyte
 * register classes and hidden-pointer MEMORY), struct locals with member
 * stores, and scalar/aggregate arguments interleaved past the six-register
 * boundary. Every function has a host-compiled reference twin in
 * reference.c.
 */

struct c_point {
  long x;
  long y;
};

long c_point_zero_array(void) {
  struct c_point points[2] = {0};
  points[1].x = 3;
  return points[0].x + points[0].y + points[1].x + points[1].y;
}

struct c_big {
  long a;
  long b;
  long c;
  long d;
};

struct c_pair {
  double first;
  double second;
};

struct c_mixed {
  long tag;
  double value;
};

struct c_typeof_const_pointer {
  long value;
};

typeof(struct c_typeof_const_pointer *const)
c_typeof_const_pointer_roundtrip(struct c_typeof_const_pointer *value) {
  return value;
}

long c_point_sum(struct c_point p) { return p.x + p.y; }

long c_point_sum_reference(struct c_point p) { return p.x + p.y; }

long c_point_after_three(long a, long b, long c, struct c_point p) {
  return a + 2 * b + 3 * c + 5 * p.x + 7 * p.y;
}

long c_point_after_four(long a, long b, long c, long d, struct c_point p) {
  return a + 2 * b + 3 * c + 4 * d + 5 * p.x + 7 * p.y;
}

struct c_point_box {
  long guard;
  struct c_point point;
};

struct c_point_array {
  struct c_point *points;
  unsigned short count;
};

typedef struct c_point c_point;
struct c_point_pointer_array {
  c_point **points;
};
c_point *c_point_pointer_element(struct c_point_pointer_array *array,
                                 int index) {
  return array->points[index];
}

void c_point_pointer_set(struct c_point_pointer_array *array, int index,
                         c_point *value) {
  array->points[index] = value;
}

c_point **c_point_pointer_slot(struct c_point_pointer_array *array, int index) {
  return &array->points[index];
}

struct c_byte_pointer_array {
  unsigned char *pointers[3];
};

unsigned char *c_byte_pointer_element(struct c_byte_pointer_array *array,
                                      int index) {
  return array->pointers[index];
}

c_point *c_point_element_address(struct c_point_array *array, int index,
                                 int group) {
  return &(array->points[index + group * array->count]);
}

struct c_bitfields {
  unsigned char marker;
  unsigned char busy;
  unsigned int low : 3;
  signed int delta : 5;
  unsigned int high : 7;
};

struct c_bitfield_tail {
  unsigned int flag : 1;
  unsigned char tail;
  long guard;
};

struct c_unnamed_bitfield_layout {
  unsigned int : 32;
  long long offset;
  int : 32;
  long long constant;
};

long c_unnamed_bitfield_offset_offset(struct c_unnamed_bitfield_layout *value) {
  return (char *)&value->offset - (char *)value;
}

long c_unnamed_bitfield_constant_offset(
    struct c_unnamed_bitfield_layout *value) {
  return (char *)&value->constant - (char *)value;
}

long c_bitfield_tail_update(struct c_bitfield_tail *fields) {
  fields->flag = 1;
  fields->tail = 42;
  return fields->tail + fields->guard;
}

long c_bitfields_update(struct c_bitfields *fields) {
  fields->low = 17;
  fields->high = 9;
  fields->delta = -3;
  fields->low += 2;
  long previous = fields->high++;
  long next = ++fields->high;
  return previous * 100 + next * 10 + fields->low;
}

void c_point_member_assign(struct c_point_box *box, struct c_point point) {
  box->point = point;
}

struct c_point c_point_add(struct c_point a, struct c_point b) {
  struct c_point r;
  r.x = a.x + b.x;
  r.y = a.y + b.y;
  return r;
}

struct c_point c_point_add_reference(struct c_point a, struct c_point b) {
  struct c_point r;
  r.x = a.x + b.x;
  r.y = a.y + b.y;
  return r;
}

long c_big_sum(struct c_big b) { return b.a + b.b + b.c + b.d; }

long c_big_sum_reference(struct c_big b) { return b.a + b.b + b.c + b.d; }

struct c_big c_big_make(long seed) {
  struct c_big b;
  b.a = seed;
  b.b = seed + 1;
  b.c = seed + 2;
  b.d = seed + 3;
  return b;
}

struct c_big c_big_make_reference(long seed) {
  struct c_big b;
  b.a = seed;
  b.b = seed + 1;
  b.c = seed + 2;
  b.d = seed + 3;
  return b;
}

double c_pair_second(struct c_pair p) {
  struct c_pair copy = p;
  return copy.second;
}

double c_pair_second_reference(struct c_pair p) {
  struct c_pair copy = p;
  return copy.second;
}

struct c_pair c_pair_make(double base) {
  struct c_pair p;
  p.first = base;
  p.second = base * 2.0;
  return p;
}

struct c_pair c_pair_make_reference(double base) {
  struct c_pair p;
  p.first = base;
  p.second = base * 2.0;
  return p;
}

long c_mixed_blend(struct c_mixed m, long scale) { return m.tag * scale; }

long c_mixed_blend_reference(struct c_mixed m, long scale) {
  return m.tag * scale;
}

long c_many_with_struct(long a, long b, long c, long d, long e,
                        struct c_point f, long g) {
  return a + b + c + d + e + f.x + f.y + g;
}

long c_many_with_struct_reference(long a, long b, long c, long d, long e,
                                  struct c_point f, long g) {
  return a + b + c + d + e + f.x + f.y + g;
}

long c_struct_array_roundtrip(long seed) {
  struct c_point points[3];
  long i = 0;
  while (i < 3) {
    points[i].x = seed + i;
    points[i].y = seed - i;
    i += 1;
  }
  struct c_point total;
  total.x = 0;
  total.y = 0;
  i = 0;
  while (i < 3) {
    total = points[i];
    i += 1;
  }
  return total.x * 10 + total.y;
}

long c_struct_array_roundtrip_reference(long seed) {
  struct c_point points[3];
  long i = 0;
  while (i < 3) {
    points[i].x = seed + i;
    points[i].y = seed - i;
    i += 1;
  }
  struct c_point total;
  total.x = 0;
  total.y = 0;
  i = 0;
  while (i < 3) {
    total = points[i];
    i += 1;
  }
  return total.x * 10 + total.y;
}

long c_mixed_declarator_aggregate_copy(long seed) {
  struct c_point *unused, source;
  unused = 0;
  if (unused)
    return -1;
  source.x = seed;
  source.y = seed + 1;
  struct c_point copy;
  copy = source;
  return copy.x * 10 + copy.y;
}

typedef unsigned int c_layout_u32;
struct c_pointer_integer_pointer_layout {
  unsigned char *blob;
  c_layout_u32 length;
  c_layout_u32 capacity;
  char *text;
  void *database;
  int text_length;
  c_layout_u32 references;
};

long c_pointer_integer_pointer_layout_offsets(void) {
  struct c_pointer_integer_pointer_layout value;
  char *base = (char *)&value;
  return ((char *)&value.length - base) +
         10 * ((char *)&value.capacity - base) +
         100 * ((char *)&value.text - base) +
         1000 * ((char *)&value.database - base) +
         10000 * ((char *)&value.text_length - base) +
         100000 * ((char *)&value.references - base);
}

void c_pointer_integer_pointer_layout_store(void *storage, long seed) {
  struct c_pointer_integer_pointer_layout *value =
      (struct c_pointer_integer_pointer_layout *)storage;
  value->blob = (unsigned char *)storage + 40;
  value->length = (c_layout_u32)(seed + 1);
  value->capacity = (c_layout_u32)(seed + 2);
  value->text = (char *)storage + 48;
  value->database = storage;
  value->text_length = (int)(seed + 3);
  value->references = (c_layout_u32)(seed + 4);
}

struct c_json_parse_layout {
  unsigned char *blob;
  c_layout_u32 blob_length;
  c_layout_u32 blob_capacity;
  char *json;
  void *database;
  int json_length;
  c_layout_u32 references;
  c_layout_u32 error_offset;
  unsigned short depth;
  unsigned char error_count;
  unsigned char out_of_memory;
  unsigned char json_is_refcounted;
  unsigned char has_nonstandard;
  unsigned char read_only;
  unsigned char edit;
  int delta;
  c_layout_u32 insert_length;
  c_layout_u32 label_offset;
  unsigned char *insert;
};

void c_json_parse_layout_store(void *storage, long seed) {
  struct c_json_parse_layout *value = (struct c_json_parse_layout *)storage;
  value->blob = (unsigned char *)storage + 96;
  value->blob_length = (c_layout_u32)(seed + 1);
  value->blob_capacity = (c_layout_u32)(seed + 2);
  value->json = (char *)storage + 104;
  value->database = storage;
  value->json_length = (int)(seed + 3);
  value->references = (c_layout_u32)(seed + 4);
  value->error_offset = (c_layout_u32)(seed + 5);
  value->depth = (unsigned short)(seed + 6);
  value->error_count = (unsigned char)(seed + 7);
  value->out_of_memory = (unsigned char)(seed + 8);
  value->json_is_refcounted = (unsigned char)(seed + 9);
  value->has_nonstandard = (unsigned char)(seed + 10);
  value->read_only = (unsigned char)(seed + 11);
  value->edit = (unsigned char)(seed + 12);
  value->delta = (int)(seed + 13);
  value->insert_length = (c_layout_u32)(seed + 14);
  value->label_offset = (c_layout_u32)(seed + 15);
  value->insert = (unsigned char *)storage + 112;
}

struct c_json_cache_layout {
  void *database;
  int used;
  struct c_json_parse_layout *entries[4];
};

void c_json_cache_layout_store(void *storage, long seed) {
  struct c_json_cache_layout *value = (struct c_json_cache_layout *)storage;
  unsigned char *bytes = (unsigned char *)storage;
  value->database = storage;
  value->used = (int)(seed + 1);
  value->entries[0] = (struct c_json_parse_layout *)(bytes + 64);
  value->entries[1] = (struct c_json_parse_layout *)(bytes + 72);
  value->entries[2] = (struct c_json_parse_layout *)(bytes + 80);
  value->entries[3] = (struct c_json_parse_layout *)(bytes + 88);
}

void *c_json_cache_layout_entry(void *storage, long index) {
  struct c_json_cache_layout *value = (struct c_json_cache_layout *)storage;
  return value->entries[index];
}

void c_json_cache_layout_insert(void *storage, void *entry) {
  struct c_json_cache_layout *value = (struct c_json_cache_layout *)storage;
  value->entries[value->used++] = (struct c_json_parse_layout *)entry;
}
