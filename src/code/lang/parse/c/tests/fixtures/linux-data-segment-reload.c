void nyc_linux_reload_data_segments(unsigned short selector) {
  __asm__ __volatile__("movl %%eax, %%ds\n"
                       "movl %%eax, %%ss\n"
                       "movl %%eax, %%es\n"
                       :
                       : "a"(selector)
                       : "memory");
}
