struct attributed_state {
  unsigned int generation;
  unsigned int flags;
} state __attribute__((section(".data..read_mostly"))) = {
    .generation = 4,
    .flags = 8,
};

int main(void) { return state.generation == 4 && state.flags == 8 ? 0 : 1; }
