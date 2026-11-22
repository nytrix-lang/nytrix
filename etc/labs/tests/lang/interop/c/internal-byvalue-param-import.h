typedef struct SumPairLayout {
  long a;
  long b;
  long c;
} SumPairLayout;

long sum_pair(SumPairLayout p);
SumPairLayout make_sum_pair(void);

typedef struct MixedPairLayout {
  long integer;
  double real;
} MixedPairLayout;

typedef struct SsePairLayout {
  double first;
  double second;
} SsePairLayout;

typedef struct SingleSseLayout {
  double value;
} SingleSseLayout;

typedef struct SmallIntegerPairLayout {
  int first;
  int second;
} SmallIntegerPairLayout;

typedef struct SmallFloatPairLayout {
  float first;
  float second;
} SmallFloatPairLayout;

typedef struct SingleIntegerLayout {
  long long value;
} SingleIntegerLayout;

typedef struct SingleInteger32Layout {
  int value;
} SingleInteger32Layout;

typedef struct SingleFloatLayout {
  float value;
} SingleFloatLayout;

typedef struct PackedWideLayout {
  char tag;
  long a;
  long b;
  long c;
} __attribute__((packed)) PackedWideLayout;

typedef struct AlignedWideLayout {
  long a;
  long b;
  long c;
} __attribute__((aligned(16))) AlignedWideLayout;

MixedPairLayout mixed_pair(void);
SsePairLayout sse_pair(void);
long sum_mixed_pair(MixedPairLayout value);
long sum_sse_pair(SsePairLayout value);
double inspect_single_sse(SingleSseLayout value);
SingleSseLayout make_single_sse(void);
long inspect_small_integer_pair(SmallIntegerPairLayout value);
SmallIntegerPairLayout make_small_integer_pair(void);
long inspect_small_float_pair(SmallFloatPairLayout value);
SmallFloatPairLayout make_small_float_pair(void);
long long inspect_single_integer(SingleIntegerLayout value);
SingleIntegerLayout make_single_integer(void);
long inspect_single_integer32(SingleInteger32Layout value);
SingleInteger32Layout make_single_integer32(void);
long inspect_single_float(SingleFloatLayout value);
SingleFloatLayout make_single_float(void);
long sum_packed_wide(PackedWideLayout value);
long sum_aligned_wide(AlignedWideLayout value);
long check_mixed_pair(const MixedPairLayout *value);
long check_sse_pair(const SsePairLayout *value);

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

NestedMixedLayout nested_mixed_pair(void);
NestedSseLayout nested_sse_pair(void);
long sum_nested_mixed(NestedMixedLayout value);
long sum_nested_sse(NestedSseLayout value);
long check_nested_mixed(const NestedMixedLayout *value);
long check_nested_sse(const NestedSseLayout *value);

typedef struct NestedMemoryLayout {
  NestedMixedLayout head;
  long tail;
} NestedMemoryLayout;

NestedMemoryLayout nested_memory_value(void);
long sum_nested_memory(NestedMemoryLayout value);

typedef union AggregateUnionLayout {
  long integer;
  double real;
} AggregateUnionLayout;

typedef struct AggregateArrayLayout {
  double values[2];
} AggregateArrayLayout;

AggregateUnionLayout aggregate_union_value(void);
long check_aggregate_union(AggregateUnionLayout value);
AggregateArrayLayout aggregate_array_value(void);
long check_aggregate_array(AggregateArrayLayout value);
