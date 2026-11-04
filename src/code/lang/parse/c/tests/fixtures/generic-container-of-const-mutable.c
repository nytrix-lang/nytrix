struct sock {
  int value;
};

struct inet_sock {
  struct sock sk;
};

struct udp_sock {
  struct inet_sock inet;
  _Bool peeking_with_offset;
};

#define same_type(left, right)                                                 \
  __builtin_types_compatible_p(typeof(left), typeof(right))
#define typeof_member(type, member) typeof(((type *)0)->member)
#define container_of(ptr, type, member)                                        \
  ({                                                                           \
    _Static_assert(same_type(*(ptr), typeof_member(type, member)) ||           \
                       same_type(*(ptr), void),                                \
                   "pointer type mismatch");                                   \
    (type *)((void *)(ptr) - __builtin_offsetof(type, member));                \
  })
#define container_of_const(ptr, type, member)                                  \
  _Generic((ptr),                                                              \
      const typeof(*(ptr)) *: ((const type *)container_of(ptr, type, member)), \
      default: ((type *)container_of(ptr, type, member)))
#define udp_sk(ptr) container_of_const(ptr, struct udp_sock, inet.sk)
#define WRITE_ONCE(location, value)                                            \
  (*(volatile typeof(location) *)&(location) = (value))

static void sk_set_peek_off(struct sock *sk, int val) {
  (void)sk;
  (void)val;
}

static int udp_set_peek_off(struct sock *sk, int val) {
  sk_set_peek_off(sk, val);
  WRITE_ONCE(udp_sk(sk)->peeking_with_offset, val >= 0);
  return 0;
}

int main(void) {
  struct udp_sock object = {0};
  struct sock *sk = &object.inet.sk;
  udp_set_peek_off(sk, 42);
  return !object.peeking_with_offset;
}
