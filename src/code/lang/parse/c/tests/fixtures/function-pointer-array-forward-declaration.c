static int (*callbacks[])(int);

static int add_one(int value) { return value + 1; }

static int (*callbacks[])(int) = {add_one};

int main(void) { return callbacks[0](41) == 42 ? 0 : 1; }
