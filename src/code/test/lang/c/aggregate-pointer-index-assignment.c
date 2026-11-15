/*
 * Fixture: assign an aggregate field through indexed pointer access.
 */
struct record {
  long value;
};

struct task {
  struct record *bio_list;
};

static struct record source[1];
static struct record destination[1];
static struct task task;

static struct task *get_task(void) { return &task; }

int main(void) {
  source[0].value = 0x123456789L;
  task.bio_list = destination;
  get_task()->bio_list[0] = source[0];
  return destination[0].value == 0x123456789L ? 0 : 1;
}
