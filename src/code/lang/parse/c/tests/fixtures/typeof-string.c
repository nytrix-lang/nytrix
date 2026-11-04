struct nyc_string_typeof { typeof("abc") text; };
int main(void) { return sizeof(struct nyc_string_typeof) != 4; }
