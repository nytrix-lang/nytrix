/*
 * C parser fixture for typeof casts involving aggregate array members.
 */
typedef unsigned long long u64;

struct frame {
  u64 before;
  unsigned char retcode[8];
  u64 after;
};

static u64 initialize(struct frame *frame) {
  static const struct {
    unsigned short instruction;
    unsigned int syscall;
    unsigned short interrupt;
  } __attribute__((packed)) code = {0xb858, 119, 0x80cd};
  __typeof__(*(((u64 *)frame->retcode))) value =
      ((__typeof__(*((u64 *)frame->retcode)))(*((u64 *)&code)));
  __typeof__(((u64 *)frame->retcode)) pointer = (((u64 *)frame->retcode));
  if (sizeof(value) != 8 || sizeof(*pointer) != 8)
    return 0;
  *pointer = value;
  return value;
}

int main(void) {
  struct frame frame = {.before = 0x123456789abcdef0ULL,
                        .after = 0xfedcba9876543210ULL};
  return initialize(&frame) != 0x80cd00000077b858ULL ||
         frame.retcode[0] != 0x58 || frame.retcode[7] != 0x80 ||
         frame.before != 0x123456789abcdef0ULL ||
         frame.after != 0xfedcba9876543210ULL;
}
