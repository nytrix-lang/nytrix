/*
 * Fixture: infer a structure member type from a string literal with typeof.
 */
struct nyc_string_typeof {
  typeof("abc") text;
};
int main(void) { return sizeof(struct nyc_string_typeof) != 4; }
