/*
 * C frontend test fixture: elf callback table section.
 */
static int callback(void) { return 41; }

static int (*callbacks[])(void)
    __attribute__((section(".init.data"))) = {callback};

int main(void) { return callbacks[0]() != 41; }
