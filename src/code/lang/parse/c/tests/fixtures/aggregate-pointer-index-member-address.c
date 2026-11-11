/*
 * C parser fixture for taking an indexed member address through a pointer.
 */
struct list_head {
  struct list_head *next;
  struct list_head *prev;
};

struct padata_work {
  struct list_head pw_list;
};

static struct padata_work work_storage[2];
static struct padata_work *padata_works;
static struct list_head head;
static struct list_head *inserted;

static void list_add(struct list_head *entry, struct list_head *list) {
  inserted = entry;
  list->next = entry;
}

static void add_work(unsigned index, struct list_head *list) {
  list_add(&padata_works[index].pw_list, list);
}

int main(void) {
  padata_works = work_storage;
  add_work(0, &head);
  return inserted == &work_storage[0].pw_list && head.next == inserted ? 0 : 1;
}
