struct console_line {
  char devname[64];
  char name[32];
};

static struct console_line console_cmdline[1];

int main(void) {
  char buffer[sizeof(console_cmdline[0].devname)];
  buffer[0] = 0;
  return sizeof(buffer) == 64 && sizeof(console_cmdline[0].devname) >=
                                     sizeof(console_cmdline[0].name) + 4
             ? 0
             : 1;
}
