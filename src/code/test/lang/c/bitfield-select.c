struct slot {
  unsigned char low : 7;
  unsigned char present : 1;
};

static volatile int observed;

__attribute__((noinline)) static int before(struct slot *slots) {
  return slots[0].low == 77 ? -5 : 0;
}

__attribute__((noinline)) static void after(struct slot *slots) {
  observed += slots[0].low;
}

static int slot_status(struct slot *slots, int n) {
  int ret = before(slots);
  if (ret)
    goto out;
  if (slots[n].present)
    ret = 4;
  else
    ret = 1;
out:
  after(slots);
  return ret;
}

int main(void) {
  struct slot slots[2] = {{1, 0}, {127, 1}};
  if (slot_status(slots, 0) != 1 || slot_status(slots, 1) != 4)
    return 1;
  slots[0].low = 77;
  return slot_status(slots, 1) != -5 || observed != 79;
}
