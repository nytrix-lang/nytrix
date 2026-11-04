#include <stdint.h>

static uint64_t checksum_add(uint64_t sum, const uint64_t *source,
                             const uint64_t *destination) {
  uint64_t sum64 = sum;
  __asm__ volatile("addq %1,%[sum64]\n"
                   "\tadcq %2,%[sum64]\n"
                   "\tadcq %3,%[sum64]\n"
                   "\tadcq %4,%[sum64]\n"
                   "\tadcq $0,%[sum64]"
                   : [sum64] "+r"(sum64)
                   : "m"(source[0]), "m"(source[1]), "m"(destination[0]),
                     "m"(destination[1]));
  return sum64;
}

static uint64_t add_one(uint64_t sum, uint64_t value) {
  __asm__("addq %1,%0\n\tadcq $0,%0" : "+r"(sum) : "r"(value));
  return sum;
}

static uint64_t add_memory(uint64_t sum, const uint64_t *source) {
  __asm__("addq 0*8(%[src]),%[res]\n\t"
          "adcq 1*8(%[src]),%[res]\n\t"
          "adcq 2*8(%[src]),%[res]\n\t"
          "adcq 3*8(%[src]),%[res]\n\t"
          "adcq $0,%[res]"
          : [res] "+r"(sum)
          : [src] "r"(source), "m"(*(const char (*)[32])source));
  return sum;
}

int main(void) {
  const uint64_t source[2] = {1, UINT64_MAX};
  const uint64_t destination[2] = {0, 0};
  const uint64_t memory[4] = {UINT64_MAX, UINT64_MAX, 1, 0};
  return checksum_add(UINT64_MAX, source, destination) != 1 ||
         checksum_add(7, destination, destination) != 7 ||
         add_one(UINT64_MAX, 1) != 1 || add_one(7, 3) != 10 ||
         add_memory(1, memory) != 2 || add_memory(0, memory) != 1;
}
