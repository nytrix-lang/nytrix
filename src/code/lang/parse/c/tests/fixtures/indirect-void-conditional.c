/*
 * Fixture: evaluate conditional expressions that select void-returning calls.
 */
static int result;

static void record(int value) { result = result * 10 + value; }

int main(void) {
  void (*callback)(int) = record;
  1 ? callback(3) : (0 ? callback(4) : callback(5));
  return result != 3;
}
