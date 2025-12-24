#include <stdio.h>
#include <string.h>

#ifndef NYTRIX_VERSION
#define NYTRIX_VERSION "0.0.1"
#endif

static void help(void) {
  puts("Nytrix prototype");
  puts("usage: ny [--version] [--help]");
}

int main(int argc, char **argv) {
  if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
    help();
    return 0;
  }
  if (argc > 1 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-version") == 0)) {
    printf("Nytrix %s\n", NYTRIX_VERSION);
    return 0;
  }
  puts("Nytrix prototype 0.0.1");
  return 0;
}
