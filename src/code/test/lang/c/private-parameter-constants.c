extern int absent_private_feature(void);
static volatile int visits;

__attribute__((noinline)) static int private_choice(int mode, int value) {
  ++visits;
  if (mode)
    return absent_private_feature();
  return value + 7;
}

__attribute__((noinline)) static int private_chain(int mode, int value) {
  return private_choice(mode, value);
}

int call_private_first(int value) { return private_chain(0, value); }
int call_private_second(int value) { return private_chain(0, value + 1); }

__attribute__((noinline)) static int different_arguments(int mode) {
  return mode ? 19 : 13;
}
int call_different_arguments(int mode) {
  return different_arguments(0) + different_arguments(mode);
}

__attribute__((noinline)) static int escaped_argument(int mode) {
  return mode + 23;
}
int (*exported_private_pointer)(int) = escaped_argument;
int call_escaped_argument(void) { return escaped_argument(0); }

extern void mutate_parameter(int *);
__attribute__((noinline)) static int address_taken_parameter(int mode) {
  mutate_parameter(&mode);
  return mode;
}
int call_address_taken_parameter(void) { return address_taken_parameter(0); }

struct scalar_aggregate {
  long value;
};
__attribute__((noinline)) static long
aggregate_neighbor(struct scalar_aggregate aggregate, int mode) {
  if (mode)
    return absent_private_feature();
  return aggregate.value + 37;
}
long call_aggregate_neighbor(long value) {
  struct scalar_aggregate aggregate = {value};
  return aggregate_neighbor(aggregate, 0);
}

struct mixed_aggregate {
  long integer;
  double real;
};
struct pair_aggregate {
  long first, second;
};
struct memory_aggregate {
  long first, second, third;
};
__attribute__((noinline)) long receive_mixed(struct mixed_aggregate value,
                                             long tail) {
  return value.integer + (long)value.real + tail;
}
__attribute__((noinline)) long receive_rollback(long a, long b, long c, long d,
                                                long e,
                                                struct pair_aggregate pair,
                                                long tail) {
  pair.first += tail;
  return a + b + c + d + e + pair.first + pair.second;
}
__attribute__((noinline)) long receive_memory(struct memory_aggregate value,
                                              long tail) {
  value.second += tail;
  return value.first + value.second + value.third;
}
extern int exercise_aggregate_parameters(void);

int main(void) {
  if (call_private_first(3) != 10 || call_private_second(3) != 11 ||
      visits != 2)
    return 1;
  if (call_different_arguments(1) != 32 || call_different_arguments(0) != 26)
    return 2;
  if (call_escaped_argument() != 23 || exported_private_pointer(7) != 30)
    return 3;
  if (call_address_taken_parameter() != 31)
    return 4;
  if (call_aggregate_neighbor(7) != 44 || call_aggregate_neighbor(-7) != 30)
    return 5;
  if (exercise_aggregate_parameters())
    return 6;
  return 0;
}
