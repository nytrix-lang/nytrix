/*
 * PIC aliases must refer to the same aggregate storage, not a copy.
 */
struct cpu {
  unsigned long flags;
  int id;
};
struct cpu __attribute__((section("cpu_state"))) cpu = {0x1234UL, 17};
extern typeof(cpu) cpu_alias __attribute__((alias("cpu")));
extern struct cpu cpu_forward;
extern typeof(cpu) cpu_forward __attribute__((alias("cpu")));
static struct cpu cpus[2] = {{3, 7}, {5, 11}};
static typeof(cpus) cpu_table __attribute__((used, alias("cpus")));
static struct cpu *lookup(void) { return &cpu_alias; }
int main(void) {
  if (lookup() != &cpu || &cpu_forward != &cpu || lookup()->id != 17 ||
      cpu_table != cpus || cpu_table[1].id != 11)
    return 1;
  cpu_alias.flags = 0x5678UL;
  cpu_forward.id = 23;
  return cpu.flags != 0x5678UL || lookup()->id != 23;
}
