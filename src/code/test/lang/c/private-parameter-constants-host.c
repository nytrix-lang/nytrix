void mutate_parameter(int *value) { *value = 31; }

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
extern long receive_mixed(struct mixed_aggregate, long);
extern long receive_rollback(long, long, long, long, long,
                             struct pair_aggregate, long);
extern long receive_memory(struct memory_aggregate, long);

int exercise_aggregate_parameters(void) {
  struct mixed_aggregate mixed = {17, -3.0};
  struct pair_aggregate pair = {19, 23};
  struct memory_aggregate memory = {29, 31, 37};
  if (receive_mixed(mixed, 7) != 21)
    return 1;
  if (receive_rollback(1, 2, 3, 4, 5, pair, 11) != 68 || pair.first != 19 ||
      pair.second != 23)
    return 2;
  if (receive_memory(memory, 13) != 110 || memory.second != 31)
    return 3;
  return 0;
}
