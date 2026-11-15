/*
 * Check data boundaries and both tied pointer outputs at every width.
 */
#define MOVE(name, instruction, width)                                       \
  static int name(unsigned char *destination, const unsigned char *source) { \
    unsigned char *to = destination;                                         \
    const unsigned char *from = source;                                      \
    __asm__ volatile(instruction                                             \
                     : "=&D"(to), "=&S"(from)                                \
                     : "0"(to), "1"(from)                                    \
                     : "memory");                                            \
    return to == destination + width && from == source + width;              \
  }
MOVE(move_byte, "movsb", 1)
MOVE(move_word, "movsw", 2)
MOVE(move_dword, "movsl", 4)
MOVE(move_qword, "movsq", 8)

typedef int (*move_fn)(unsigned char *, const unsigned char *);
static move_fn moves[4] = {move_byte, move_word, move_dword, move_qword};

int main(void) {
  unsigned char source[40], destination[40];
  for (int i = 0; i < 40; ++i)
    source[i] = i + 17;
  for (int form = 0; form < 4; ++form) {
    int width = 1 << form;
    for (int offset = 0; offset < 8; ++offset) {
      for (int i = 0; i < 40; ++i)
        destination[i] = 0xcc;
      int start = 8 + offset;
      int input = 15 - offset;
      if (!moves[form](destination + start, source + input))
        return 1;
      for (int i = 0; i < 40; ++i) {
        int expected =
            i >= start && i < start + width ? source[input + i - start] : 0xcc;
        if (destination[i] != expected)
          return 2;
      }
    }
  }
  return 0;
}
