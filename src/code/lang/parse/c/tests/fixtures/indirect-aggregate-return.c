/*
 * C parser fixture for indirect calls returning aggregate values by value.
 */
struct register_value {
  unsigned int value;
};

struct large_value {
  unsigned long long first;
  unsigned long long second;
  unsigned long long third;
};

struct device {
  struct register_value (*read_register)(struct device *);
  struct large_value (*read_large)(struct device *);
};

static struct register_value read_register(struct device *device) {
  (void)device;
  return (struct register_value){.value = 42};
}

static struct large_value read_large(struct device *device) {
  (void)device;
  return (struct large_value){.first = 10, .second = 20, .third = 30};
}

int main(void) {
  struct device device = {
      .read_register = read_register,
      .read_large = read_large,
  };
  struct register_value value = device.read_register(&device);
  struct large_value large = device.read_large(&device);
  return value.value == 42 && large.first == 10 && large.second == 20 &&
                 large.third == 30
             ? 0
             : 1;
}
