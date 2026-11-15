/*
 * Fixture driver: exercise the x86 data-segment reload helper.
 */
extern void nyc_linux_reload_data_segments(unsigned short selector);

int main(void) {
  unsigned short saved_ds, saved_ss, saved_es;
  __asm__ volatile("mov %%ds, %0\n\tmov %%ss, %1\n\tmov %%es, %2"
                   : "=r"(saved_ds), "=r"(saved_ss), "=r"(saved_es));
  nyc_linux_reload_data_segments(saved_ss);
  unsigned short actual_ds, actual_ss, actual_es;
  __asm__ volatile("mov %%ds, %0\n\tmov %%ss, %1\n\tmov %%es, %2"
                   : "=r"(actual_ds), "=r"(actual_ss), "=r"(actual_es));
  __asm__ volatile("mov %0, %%ds\n\tmov %1, %%es"
                   :
                   : "r"(saved_ds), "r"(saved_es)
                   : "memory");
  return actual_ds != saved_ss || actual_ss != saved_ss ||
         actual_es != saved_ss;
}
