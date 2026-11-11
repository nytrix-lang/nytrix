/*
 * Fixture: compile robust-futex lock operations with constrained x86 assembly.
 */
typedef unsigned int u32;
typedef unsigned long u64;
static u32 try_unlock(u32 *lock, u32 tid, u64 *pop) {
  asm volatile("lock cmpxchgl %k[zero], %a[lock]\n"
               "jnz 1f\nmovq %[zero], %a[pop]\n1:\n"
               : [tid] "+&a"(tid)
               : [lock] "D"(lock), [pop] "d"(pop), [zero] "r"(0UL)
               : "memory");
  return tid;
}
int main(void) {
  u32 lock = 5;
  u64 pending = 9;
  if (try_unlock(&lock, 5, &pending) != 5 || lock != 0 || pending != 0)
    return 1;
  lock = 7;
  pending = 9;
  if (try_unlock(&lock, 5, &pending) != 7 || lock != 7 || pending != 9)
    return 2;
  return 0;
}
