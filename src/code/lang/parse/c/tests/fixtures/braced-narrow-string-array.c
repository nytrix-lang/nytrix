static char core_modes[] = {"file\npipe"
                            "\nsocket"};

int main(void) {
  static const char expected[] = "file\npipe\nsocket";
  if (sizeof(core_modes) != sizeof(expected))
    return 1;
  for (unsigned i = 0; i < sizeof(expected); ++i)
    if (core_modes[i] != expected[i])
      return 2;
  return 0;
}
