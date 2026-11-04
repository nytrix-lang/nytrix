static unsigned long values[__roundup_pow_of_two(65)];

int main(void) { return sizeof(values) != 1024; }
