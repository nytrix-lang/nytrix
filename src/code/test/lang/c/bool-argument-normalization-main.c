/* Host driver for bool-argument-normalization.c. */
#include <stdbool.h>
bool g1(unsigned long); int g2(unsigned long); unsigned char g3(unsigned long); bool g4(unsigned long);
int g5(unsigned long); int g7(unsigned long);
struct s { int a; bool b; };
int s1(unsigned long, struct s *); int s2(unsigned long);
int take(bool b) { return b; }
int main(void) {
  unsigned long f = 0x225;
  struct s v = {0, false};
  return !(g1(f) == 1 && g2(f) == 1 && g3(f) == 1 && g4(f) == 1 && g5(f) == 1 &&
           g7(f) == 1 && s1(f, &v) == 1 && s2(f) == 1);
}
