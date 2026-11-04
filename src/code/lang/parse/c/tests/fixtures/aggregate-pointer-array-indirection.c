struct rcu_head {
  struct rcu_head *next;
  int value;
};

struct callback_list {
  struct rcu_head *head;
  struct rcu_head **tails[4];
};

#define WRITE_ONCE(object, value)                                              \
  do {                                                                         \
    *(volatile __typeof__(object) *)&(object) = (value);                       \
  } while (0)

int main(void) {
  struct rcu_head first;
  struct rcu_head second;
  struct rcu_head *tail = &first;
  struct callback_list callbacks;

  callbacks.head = 0;
  callbacks.tails[0] = &tail;
  callbacks.head = *callbacks.tails[0];
  if (callbacks.head != &first)
    return 1;

  WRITE_ONCE(*callbacks.tails[0], &second);
  if (tail != &second)
    return 2;

  return 0;
}
