/*
 * Fixture: call a void-returning function through a global aggregate member.
 */
struct handler {
  void (*invoke)(int);
};
static int result;
static void save(int value) { result = value; }
static struct handler handler = {save};
static struct handler *handlers[] = {&handler};

int main(void) {
  handlers[0]->invoke(19);
  return result != 19;
}
