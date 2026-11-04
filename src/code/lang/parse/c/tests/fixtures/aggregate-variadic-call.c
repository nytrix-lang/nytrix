struct nyc_uid {
  unsigned int value;
};

int nyc_read_uid(int marker, ...);

int main(void) {
  struct nyc_uid uid = {.value = 42};
  return nyc_read_uid(7, uid) != 49;
}
