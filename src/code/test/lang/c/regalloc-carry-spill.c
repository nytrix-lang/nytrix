/* A value carried across blocks after being spilled under register pressure must be reloaded, not read from a stale register (fuzzer seed 245). */
#include <stdint.h>
#include <stdio.h>
static uint64_t sink = 1469598103934665603ull;
static void mix(uint64_t v) { sink = (sink ^ v) * 1099511628211ull; }
static uint32_t g0 = 2022256929;
static int8_t g1 = 1939127790;
static int32_t g2 = 1990942702;
static uint8_t g3 = 922411674;
static __attribute__((noinline)) uint64_t f0(int64_t a0) {
  uint64_t e1 = (uint64_t)(uint32_t)(g2);
  uint64_t e2 = (uint64_t)(a0) * (uint64_t)(a0);
  uint64_t e3 = (uint64_t)(e1) & (uint64_t)(e2);
  int64_t t0 = (uint64_t)(e3);
  uint64_t e4 = (uint64_t)(g0) / ((uint64_t)(60u) | 1u);
  int64_t t1 = (uint64_t)(e4);
  uint64_t e5 = (uint64_t)(1475890002u) + (uint64_t)(t1);
  uint64_t e6 = (uint16_t)(a0) != (uint16_t)(436051446u) ? (uint64_t)(a0) : (uint64_t)(1114573671u);
  uint64_t e7 = (uint64_t)(e5) ^ (uint64_t)(e6);
  uint64_t e8 = (uint64_t)(t1) << (g0 & 63u);
  uint64_t e9 = (uint64_t)(9004u) | (uint64_t)(g0);
  uint64_t e10 = (uint64_t)(41u) | (uint64_t)(t0);
  uint64_t e11 = (uint64_t)(50335u) - (uint64_t)(g3);
  uint64_t e12 = (int32_t)(e8) == (int32_t)(e9) ? (uint64_t)(e10) : (uint64_t)(e11);
  uint64_t e13 = (uint64_t)(e7) ^ (uint64_t)(e12);
  uint8_t t2 = (uint64_t)(e13);
  uint64_t e14 = (uint64_t)(t1) & (uint64_t)(t1);
  uint64_t e15 = (uint16_t)(479079606u) > (uint16_t)(t1) ? (uint64_t)(t1) : (uint64_t)(a0);
  uint64_t e16 = (uint64_t)(uint32_t)(e14);
  uint64_t e17 = (uint64_t)(g3) - (uint64_t)(g3);
  uint64_t e18 = (uint64_t)(1u) ^ (uint64_t)(t0);
  uint64_t e19 = (uint64_t)(e17) * (uint64_t)(e18);
  uint64_t e20 = (uint64_t)(e16) + (uint64_t)(e19);
  int8_t t3 = (uint64_t)(e20);
  uint64_t e21 = (uint64_t)(g0) >> (t2 & 63u);
  uint64_t e22 = (uint64_t)(g3) - (uint64_t)(g2);
  uint64_t e23 = (uint64_t)(e21) * (uint64_t)(e22);
  g0 = (uint32_t)(e23);
  uint64_t e24 = (uint64_t)(t2) != (uint64_t)(291516245u) ? (uint64_t)(a0) : (uint64_t)(t0);
  uint64_t e25 = (uint64_t)(t2) ^ (uint64_t)(102u);
  uint64_t e26 = (uint64_t)(e24) / ((uint64_t)(e25) | 1u);
  uint64_t e27 = (uint64_t)(g1) >> (t2 & 63u);
  uint64_t e28 = (uint64_t)(t2) % ((uint64_t)(a0) | 1u);
  uint64_t e29 = (uint64_t)(160u) ^ (uint64_t)(21139u);
  uint64_t e30 = (uint64_t)(t0) ^ (uint64_t)(g2);
  uint64_t e31 = (uint8_t)(e27) != (uint8_t)(e28) ? (uint64_t)(e29) : (uint64_t)(e30);
  uint64_t e32 = (uint64_t)(e26) | (uint64_t)(e31);
  return (uint64_t)(e32);
}
static __attribute__((noinline)) uint64_t f1(int16_t a0) {
  uint64_t e33 = (uint64_t)(g2) >> (g2 & 63u);
  uint64_t e34 = (uint64_t)(g2) - (uint64_t)(g3);
  uint64_t e35 = (uint64_t)(e33) << (e34 & 63u);
  uint64_t e36 = (uint64_t)(g3) + (uint64_t)(g1);
  uint64_t e37 = (uint64_t)(743819350u) * (uint64_t)(g0);
  uint64_t e38 = (uint64_t)(e36) & (uint64_t)(e37);
  uint64_t e39 = (uint64_t)(e35) & (uint64_t)(e38);
  int16_t t0 = (uint64_t)(e39);
  uint64_t e40 = (uint64_t)(213062336u) * (uint64_t)(g3);
  int32_t t1 = (uint64_t)(e40);
  for (int i = 0; i < 5; ++i) {
    uint64_t e41 = (uint64_t)(g3) >> (t1 & 63u);
    uint64_t e42 = (uint64_t)(t0) & (uint64_t)(a0);
    uint64_t e43 = (uint64_t)(e41) | (uint64_t)(e42);
    t1 = (uint64_t)(e43);
  }
  uint64_t e44 = (uint64_t)(a0) | (uint64_t)(t0);
  uint64_t e45 = (uint64_t)(g1) | (uint64_t)(t1);
  uint64_t e46 = (uint64_t)(e44) * (uint64_t)(e45);
  g2 = (uint32_t)(e46);
  uint64_t e47 = (int64_t)(g2) < (int64_t)(1984647540u) ? (uint64_t)(g3) : (uint64_t)(g0);
  uint64_t e48 = (uint64_t)(t1) ^ (uint64_t)(319051486u);
  uint64_t e49 = (uint64_t)(e47) * (uint64_t)(e48);
  uint64_t e50 = (uint64_t)(g2) | (uint64_t)(g1);
  uint64_t e51 = (uint64_t)(7u) % ((uint64_t)(t1) | 1u);
  uint64_t e52 = (uint64_t)(g3) | (uint64_t)(a0);
  uint64_t e53 = (uint16_t)(g1) > (uint16_t)(g1) ? (uint64_t)(g1) : (uint64_t)(8u);
  uint64_t e54 = (int32_t)(e50) >= (int32_t)(e51) ? (uint64_t)(e52) : (uint64_t)(e53);
  uint64_t e55 = (uint64_t)(e49) | (uint64_t)(e54);
  return (uint64_t)(e55);
}
static __attribute__((noinline)) uint64_t f2(uint64_t a0, uint16_t a1) {
  uint64_t e56 = (uint64_t)(g0) % ((uint64_t)(g1) | 1u);
  uint64_t t0 = (uint64_t)(e56);
  uint64_t e57 = (uint64_t)(t0) >> (g2 & 63u);
  uint64_t e58 = (uint64_t)(t0) * (uint64_t)(t0);
  uint64_t e59 = (uint64_t)(e57) | (uint64_t)(e58);
  uint64_t e60 = (uint64_t)(g1) << (g3 & 63u);
  uint64_t e61 = (uint64_t)(1267288151u) << (a1 & 63u);
  uint64_t e62 = (uint64_t)(49236u) % ((uint64_t)(0u) | 1u);
  uint64_t e63 = (int16_t)(g3) != (int16_t)(g3) ? (uint64_t)(a1) : (uint64_t)(a0);
  uint64_t e64 = (int64_t)(e60) < (int64_t)(e61) ? (uint64_t)(e62) : (uint64_t)(e63);
  uint64_t e65 = (uint64_t)(e59) - (uint64_t)(e64);
  uint32_t t1 = (uint64_t)(e65);
  uint64_t e66 = (uint64_t)(g1) >> (g3 & 63u);
  uint64_t t2 = (uint64_t)(e66);
  uint64_t e67 = (int16_t)(a0) != (int16_t)(g1) ? (uint64_t)(0u) : (uint64_t)(a1);
  uint64_t e68 = (uint64_t)(t0) - (uint64_t)(g0);
  uint64_t e69 = (uint64_t)(e67) + (uint64_t)(e68);
  int8_t t3 = (uint64_t)(e69);
  for (int i = 0; i < 4; ++i) {
    uint64_t e70 = (uint64_t)(1678u) >> ((uint64_t)i & 63u);
    uint64_t e71 = (uint64_t)(a0) >> (a1 & 63u);
    uint64_t e72 = (uint64_t)((uint64_t)i) << (t3 & 63u);
    uint64_t e73 = (uint32_t)(g1) > (uint32_t)(g1) ? (uint64_t)(8u) : (uint64_t)(g2);
    uint64_t e74 = (uint64_t)(e70) <= (uint64_t)(e71) ? (uint64_t)(e72) : (uint64_t)(e73);
    t3 = (uint64_t)(e74);
  }
  uint64_t e75 = (uint64_t)(1552645214u) + (uint64_t)(t1);
  uint64_t e76 = (uint64_t)(53663u) | (uint64_t)(6u);
  uint64_t e77 = (uint64_t)(e75) - (uint64_t)(e76);
  g2 = (uint32_t)(e77);
  uint64_t e78 = (uint64_t)(a0) | (uint64_t)(t1);
  uint64_t e79 = (uint64_t)(a0) + (uint64_t)(170u);
  uint64_t e80 = (uint64_t)(e78) - (uint64_t)(e79);
  uint64_t e81 = (uint64_t)(52416u) / ((uint64_t)(a1) | 1u);
  uint64_t e82 = (uint64_t)(g2) >> (g1 & 63u);
  uint64_t e83 = (uint64_t)(e81) - (uint64_t)(e82);
  uint64_t e84 = (uint64_t)(e80) >> (e83 & 63u);
  return (uint64_t)(e84);
}
int main(void) {
  mix(f1((int16_t)1206538086u));
  mix(f2((uint64_t)1702501324u, (uint16_t)1u));
  mix(f2((uint64_t)62u, (uint16_t)635953465u));
  mix(f1((int16_t)3u));
  mix(f0((int64_t)583242u));
  mix(f0((int64_t)2u));
  mix(f0((int64_t)7u));
  mix(f1((int16_t)20u));
  mix(f1((int16_t)1320681148u));
  mix(f0((int64_t)673708u));
  mix((uint64_t)g0);
  mix((uint64_t)g1);
  mix((uint64_t)g2);
  mix((uint64_t)g3);
  printf("%llu\n", (unsigned long long)sink);
  return 0;
}
