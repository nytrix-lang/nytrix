#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <signal.h>
#include <stddef.h>
#include <sys/mman.h>
#include <unistd.h>

extern unsigned long capture_r9(const unsigned long *address);
__asm__(".text\n.globl capture_r9\n.type capture_r9,@function\n"
        "capture_r9:\n\tsubq $8,%rsp\n\tcall fault_in\n"
        "\tmovq %r9,%rax\n\taddq $8,%rsp\n\tret\n"
        ".size capture_r9,.-capture_r9\n");

static void *mapping;
static size_t page_size;
static unsigned long *probe;
static volatile sig_atomic_t faults;

static void handle_fault(int signal_number, siginfo_t *info, void *context) {
  (void)context;
  if (signal_number != SIGSEGV || info->si_addr != (void *)probe || faults)
    _exit(90);
  if (mprotect(mapping, page_size, PROT_READ | PROT_WRITE))
    _exit(91);
  ++faults;
}

int main(void) {
  long size = sysconf(_SC_PAGESIZE);
  if (size <= 0)
    return 1;
  page_size = (size_t)size;
  mapping = mmap(0, page_size * 2, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED)
    return 2;
  probe = (unsigned long *)((char *)mapping + page_size - sizeof(*probe));
  *probe = 0xfedcba9876543210ul;
  struct sigaction action = {0}, old_action;
  action.sa_sigaction = handle_fault;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGSEGV, &action, &old_action) ||
      mprotect(mapping, page_size * 2, PROT_NONE))
    return 3;
  if (capture_r9(probe) != 0xfedcba9876543210ul || faults != 1)
    return 4;
  *probe = 0x1234567887654321ul;
  if (capture_r9(probe) != 0x1234567887654321ul || faults != 1)
    return 5;
  if (sigaction(SIGSEGV, &old_action, 0) || munmap(mapping, page_size * 2))
    return 6;
  return 0;
}
