#include <stdint.h>

static uint32_t add_carry32(uint32_t sum) {
  uint32_t result;
  __asm__ volatile("  addl %1,%0\n"
                   "  adcl $0xffff,%0"
                   : "=r"(result)
                   : "r"(sum << 16), "0"(sum & 0xffff0000));
  return result;
}

static uint32_t add_carry32_zero(uint32_t initial, uint32_t addend) {
  uint32_t result;
  __asm__ volatile("addl %2,%0\n\t"
                   "adcl $0,%0"
                   : "=r"(result)
                   : "0"(initial), "rm"(addend));
  return result;
}

static uint32_t add_carry32_chain(uint32_t initial, uint32_t first,
                                  uint32_t second, uint32_t third) {
  uint32_t result;
  __asm__ volatile("  addl %1, %0\n"
                   "  adcl %2, %0\n"
                   "  adcl %3, %0\n"
                   "  adcl $0, %0"
                   : "=r"(result)
                   : "g"(first), "g"(second), "g"(third), "0"(initial));
  return result;
}

int main(void) {
  return add_carry32(0x0001ffffu) != 0x00010000u ||
         add_carry32(0xffffffffu) != 0xffff0000u ||
         add_carry32_zero(0x7fffffffu, 1u) != 0x80000000u ||
         add_carry32_zero(0xffffffffu, 1u) != 1u ||
         add_carry32_chain(0xffffffffu, 1u, 0xffffffffu, 0u) != 1u ||
         add_carry32_chain(1u, 2u, 3u, 4u) != 10u;
}
