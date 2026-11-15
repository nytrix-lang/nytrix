/*
 * C parser fixture for function declarations defined through signature
 * typedefs.
 */
typedef unsigned int(nyc_signature_t)(const unsigned int, unsigned int);
nyc_signature_t nyc_signature_add;

unsigned int nyc_signature_add(const unsigned int left, unsigned int right) {
  return left + right;
}

int main(void) { return nyc_signature_add(20, 22) != 42; }
