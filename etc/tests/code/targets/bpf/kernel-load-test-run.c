#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/bpf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static int bpf_call(enum bpf_cmd command, union bpf_attr *attr) {
  return (int)syscall(__NR_bpf, command, attr, sizeof(*attr));
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s SOCKET_SECTION_BINARY\n", argv[0]);
    return 2;
  }

  int object = open(argv[1], O_RDONLY);
  if (object < 0) {
    perror("open BPF instructions");
    return 2;
  }
  off_t length = lseek(object, 0, SEEK_END);
  if (length <= 0 || length % (off_t)sizeof(struct bpf_insn) != 0 ||
      lseek(object, 0, SEEK_SET) < 0) {
    fprintf(stderr, "invalid BPF instruction section size\n");
    close(object);
    return 2;
  }

  struct bpf_insn *instructions = malloc((size_t)length);
  if (!instructions ||
      read(object, instructions, (size_t)length) != (ssize_t)length) {
    perror("read BPF instructions");
    free(instructions);
    close(object);
    return 2;
  }
  close(object);

  char verifier_log[65536] = {0};
  union bpf_attr load = {0};
  load.prog_type = BPF_PROG_TYPE_SOCKET_FILTER;
  load.insn_cnt = (uint32_t)(length / (off_t)sizeof(*instructions));
  load.insns = (uint64_t)(uintptr_t)instructions;
  load.license = (uint64_t)(uintptr_t)"GPL";
  load.log_buf = (uint64_t)(uintptr_t)verifier_log;
  load.log_size = sizeof(verifier_log);
  load.log_level = 1;

  int program = bpf_call(BPF_PROG_LOAD, &load);
  free(instructions);
  if (program < 0) {
    fprintf(stderr, "BPF_PROG_LOAD: %s\n%s\n", strerror(errno), verifier_log);
    return 1;
  }

  uint8_t packet[64] = {0};
  union bpf_attr test = {0};
  test.test.prog_fd = (uint32_t)program;
  test.test.data_in = (uint64_t)(uintptr_t)packet;
  test.test.data_size_in = sizeof(packet);
  test.test.repeat = 1;
  if (bpf_call(BPF_PROG_TEST_RUN, &test) < 0) {
    fprintf(stderr, "BPF_PROG_TEST_RUN: %s\n", strerror(errno));
    close(program);
    return 1;
  }
  close(program);

  if (test.test.retval != 33) {
    fprintf(stderr, "BPF_PROG_TEST_RUN returned %u; expected 33\n",
            test.test.retval);
    return 1;
  }
  printf("BPF kernel load/test-run returned %u\n", test.test.retval);
  return 0;
}
