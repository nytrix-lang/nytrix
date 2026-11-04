int nyc_no_strict_alias(int *, float *);
int main(void) { union { int integer; float real; } value; return nyc_no_strict_alias(&value.integer, &value.real) != 0x3f800000; }
