/*
 * Runtime fixture for NULL entries in arrays of function pointers.
 */
#define NULL ((void *)0)

static int increment(int value) { return value + 1; }

static int (*const handlers[])(int) = {increment, NULL};

int main(void) { return handlers[0](4) != 5 || handlers[1] != 0; }
