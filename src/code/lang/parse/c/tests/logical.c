long c_logic_mutate(long *slot) {
  *slot = 1;
  return 7;
}

long c_logic_and_skip(long *slot) { return 0 && c_logic_mutate(slot); }

long c_logic_or_skip(long *slot) { return 1 || c_logic_mutate(slot); }

long c_logic_and_run(long *slot) { return 1 && c_logic_mutate(slot); }

long c_logic_or_run(long *slot) { return 0 || c_logic_mutate(slot); }

long c_logic_and_precedence(void) { return 1 + 1 && 0; }

long c_logic_or_precedence(void) { return 2 * 0 || 1; }
