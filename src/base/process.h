#ifndef NY_PROCESS_H
#define NY_PROCESS_H

#include <stdbool.h>

/* Runs argv without a command shell and captures stdout in an owned buffer.
 * Returns the child exit code, or 127 when the process could not be started. */
int ny_process_capture(const char *const argv[], char **out,
                       bool discard_stderr);

/* Runs argv directly, with platform-specific executable lookup and optional
 * verbose command reporting. Returns the child status or -1 on wait failure. */
int ny_exec_spawn(const char *const argv[]);

#endif
