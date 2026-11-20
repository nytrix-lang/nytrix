typedef _Bool boolean;

__attribute__((noinline)) boolean masked(unsigned flags) {
  return flags & 0x100u;
}

__attribute__((noinline)) _Bool from_byte(unsigned char value) { return value; }

__attribute__((noinline)) _Bool from_double(double value) { return value; }

__attribute__((noinline)) unsigned cast_mask(unsigned value) {
  return (boolean)value << 6;
}

__attribute__((noinline)) unsigned cast_double(double value) {
  return (boolean)value << 4;
}

__attribute__((aligned(256))) static void hook(void) {}

__attribute__((noinline)) unsigned hook_mask(void (*fn)(void), unsigned bit) {
  return (boolean)fn << bit;
}

int main(void) {
  return masked(0xdc0u) != 1 || masked(0xcc0u) != 0 ||
         masked(0x80000000u) != 0 || from_byte(254) != 1 || from_byte(0) != 0 ||
         from_double(-0.0) != 0 || from_double(-0.125) != 1 ||
         cast_mask(0x100u) != 64 || cast_mask(0) != 0 ||
         cast_double(-0.125) != 16 || cast_double(-0.0) != 0 ||
         hook_mask(hook, 5) != 32 || hook_mask((void (*)(void))0, 0) != 0;
}
