struct lock {
  unsigned long word;
};

struct entry {
  struct lock list_lock;
};

static struct entry entries[2];
static struct entry *table[2] = { &entries[0], &entries[1] };

static void set_lock_word(struct lock *lock, unsigned long value) {
  lock->word = value;
}

int main(void) {
  set_lock_word(&table[1]->list_lock, 73);
  return entries[1].list_lock.word != 73;
}
