typedef struct SumPairLayout {
  long a;
  long b;
  long c;
} SumPairLayout;

long sum_pair(SumPairLayout p) {
  return p.a + p.b + p.c;
}

SumPairLayout make_sum_pair(void) {
  return (SumPairLayout){10, 20, 30};
}

typedef struct MixedPairLayout {
  long integer;
  double real;
} MixedPairLayout;

typedef struct SsePairLayout {
  double first;
  double second;
} SsePairLayout;

MixedPairLayout mixed_pair(void) {
  return (MixedPairLayout){7, 3.5};
}

SsePairLayout sse_pair(void) {
  return (SsePairLayout){1.5, 2.5};
}

long sum_mixed_pair(MixedPairLayout value) {
  return value.integer + (long)value.real;
}

long sum_sse_pair(SsePairLayout value) {
  return (long)(value.first + value.second);
}

long check_mixed_pair(const MixedPairLayout *value) {
  return value && value->integer == 7 && value->real == 3.5;
}

long check_sse_pair(const SsePairLayout *value) {
  return value && value->first == 1.5 && value->second == 2.5;
}

typedef struct NestedRealLayout {
  double value;
} NestedRealLayout;

typedef struct NestedMixedLayout {
  long integer;
  NestedRealLayout real;
} NestedMixedLayout;

typedef struct NestedSseLayout {
  NestedRealLayout first;
  NestedRealLayout second;
} NestedSseLayout;

NestedMixedLayout nested_mixed_pair(void) {
  return (NestedMixedLayout){7, {3.5}};
}

NestedSseLayout nested_sse_pair(void) {
  return (NestedSseLayout){{1.5}, {2.5}};
}

long sum_nested_mixed(NestedMixedLayout value) {
  return value.integer + (long)value.real.value;
}

long sum_nested_sse(NestedSseLayout value) {
  return (long)(value.first.value + value.second.value);
}

long check_nested_mixed(const NestedMixedLayout *value) {
  return value && value->integer == 7 && value->real.value == 3.5;
}

long check_nested_sse(const NestedSseLayout *value) {
  return value && value->first.value == 1.5 && value->second.value == 2.5;
}

typedef struct NestedMemoryLayout {
  NestedMixedLayout head;
  long tail;
} NestedMemoryLayout;

NestedMemoryLayout nested_memory_value(void) {
  return (NestedMemoryLayout){{7, {3.5}}, 11};
}

long sum_nested_memory(NestedMemoryLayout value) {
  return value.head.integer + (long)value.head.real.value + value.tail;
}

typedef union AggregateUnionLayout {
  long integer;
  double real;
} AggregateUnionLayout;

typedef struct AggregateArrayLayout {
  double values[2];
} AggregateArrayLayout;

AggregateUnionLayout aggregate_union_value(void) {
  AggregateUnionLayout value;
  value.integer = 19;
  return value;
}

long check_aggregate_union(AggregateUnionLayout value) {
  return value.integer == 19;
}

AggregateArrayLayout aggregate_array_value(void) {
  return (AggregateArrayLayout){{1.5, 2.5}};
}

long check_aggregate_array(AggregateArrayLayout value) {
  return value.values[0] == 1.5 && value.values[1] == 2.5;
}
