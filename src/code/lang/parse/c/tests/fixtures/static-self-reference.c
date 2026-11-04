struct local_list_head {
  struct local_list_head *next;
  struct local_list_head *prev;
};
int static_self_reference_is_linked(void) {
  static struct local_list_head head = {&head, &head};
  return head.next == &head && head.prev == &head;
}
int main(void) { return !static_self_reference_is_linked(); }
