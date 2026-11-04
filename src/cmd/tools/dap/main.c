/*
 * Nytrix DAP entry point. GDB's DAP interpreter provides the actual native
 * debugger; this executable keeps the adapter name stable for VS Code.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int color_mode = -1;
int verbose_enabled = 0;

int main(int argc, char **argv) {
  const char *gdb = getenv("NYTRIX_GDB");
  if (!gdb || !*gdb)
    gdb = "gdb";

  char *gdb_argv[argc + 4];
  memset(gdb_argv, 0, sizeof(gdb_argv));

  gdb_argv[0] = (char *)gdb;
  gdb_argv[1] = "--interpreter=dap";
  gdb_argv[2] = "--nx";
  gdb_argv[3] = "--quiet";
  for (int i = 1; i < argc; ++i)
    gdb_argv[i + 3] = argv[i];

  execvp(gdb, gdb_argv);
  fprintf(stderr, "ny-dap: cannot start GDB DAP interpreter '%s': %s\n", gdb,
          strerror(errno));
  return 127;
}
