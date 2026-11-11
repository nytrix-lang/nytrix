/*
 * C parser fixture for alignment of local aggregate objects.
 */
typedef struct {
  unsigned long long words[2];
} key_t;

static unsigned long long digest(void) {
  key_t __attribute__((aligned(16))) key = {{17, 23}};
  key_t __attribute__((aligned(16))) scratch;
  scratch.words[0] = key.words[1];
  scratch.words[1] = key.words[0];
  if (((unsigned long)&key & 15) || ((unsigned long)&scratch & 15))
    return 0;
  return scratch.words[0] * 3 + scratch.words[1];
}

int main(void) { return digest() != 86; }
