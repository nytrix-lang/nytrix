static void delay_loop(unsigned long loops) {
  __asm__ volatile("\ttest %0,%0\t\n"
                   "\tjz 3f\t\t\n"
                   "\tjmp 1f\t\t\n"
                   ".align 16\t\t\n"
                   "1:\tjmp 2f\t\t\n"
                   ".align 16\t\t\n"
                   "2:\tdec %0\t\t\n"
                   "\tjnz 2b\t\t\n"
                   "3:\tdec %0\t\t\n"
                   : "+a"(loops)
                   :);
}

int main(void) {
  delay_loop(0);
  delay_loop(1);
  delay_loop(8);
  return 0;
}
