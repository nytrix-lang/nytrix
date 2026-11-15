/*
 * Regression: preserve aliases declared through the character-array parser.
 */
static const unsigned char array_alias_target[4] = {3, 5, 7, 11};
extern const unsigned char array_alias_value[4]
    __attribute__((alias("array_alias_target")));

int main(void) {
  return array_alias_value[0] != 3 || array_alias_value[3] != 11;
}
