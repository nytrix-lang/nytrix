typedef unsigned int u32;
typedef unsigned short u16;

struct far_pointer {
  u32 offset;
  u16 selector;
} __attribute__((packed));

void invoke_far_pointer(struct far_pointer *target) {
  __asm__ volatile("lcallw *%0"
                   :
                   : "m"(*target)
                   : "eax", "ebx", "ecx", "edx");
}
