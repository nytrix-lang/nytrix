/*
 * Fixture: constrain inline assembly writes to a guarded memory output.
 */
struct guarded {
  unsigned long before;
  unsigned long counter;
  unsigned long after;
};

static volatile unsigned address_calls;
static struct guarded state = {91, 40, 93};

__attribute__((noinline)) static unsigned long *counter_address(void) {
  ++address_calls;
  return &state.counter;
}

int main(void) {
  __asm__ volatile(" incq %0" : "+m"(*counter_address()));
  __asm__ volatile("incq %0" : "+m"(state.counter) : : "cc");
  unsigned long snapshot;
  __asm__ volatile("movq $73, %%rdx; movq %%rdx, %0"
                   : "=m"(snapshot)
                   :
                   : "rdx");
  unsigned long total = 5;
  __asm__ volatile("incq %0" : "+m"(total));
  unsigned long exchange_input = 11;
  unsigned long exchange_memory = 20;
  __asm__ volatile("xaddq %0, %1"
                   : "+r"(exchange_input), "+m"(exchange_memory)
                   :
                   : "memory");
  volatile unsigned long volatile_snapshot;
  __asm__ volatile("movq $81, %0" : "=m"(volatile_snapshot));
  return address_calls != 1 || state.counter != 42 || snapshot != 73 ||
         total != 6 || exchange_input != 20 || exchange_memory != 31 ||
         volatile_snapshot != 81 || state.before != 91 || state.after != 93;
}
