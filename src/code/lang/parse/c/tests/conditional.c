long c_cond_mutate(long *slot) {
  *slot = 1;
  return 7;
}

long c_cond_true(long *slot) { return 2 ? c_cond_mutate(slot) : 9; }

long c_cond_false(long *slot) { return 0 ? c_cond_mutate(slot) : 9; }

long c_cond_precedence(long value) { return value + 1 ? 7 : 9; }

long c_cond_nested(long value) { return value ? 0 : (1 ? 7 : 9); }
