/*
 * C front-end regression test: linux function pointer xchg.
 */
typedef void (*handler_t)(int);

static void old_handler(int value) { (void)value; }
static void new_handler(int value) { (void)value; }

static handler_t exchange_handler(handler_t *slot, handler_t next) {
  asm volatile(""
               "xchg"
               "q %q0, %1"
               : "+r"(next), "+m"(*slot)
               :
               : "memory", "cc");
  return next;
}

int main(void) {
  handler_t slot = old_handler;
  handler_t previous = exchange_handler(&slot, new_handler);
  return previous != old_handler || slot != new_handler;
}
