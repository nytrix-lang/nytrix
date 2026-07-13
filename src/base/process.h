#ifndef NY_PROCESS_H
#define NY_PROCESS_H

#include <stdbool.h>

/* Runs argv without a command shell and captures stdout in an owned buffer.
 * Returns the child exit code, or 127 when the process could not be started. */
int ny_process_capture(const char *const argv[], char **out,
                       bool discard_stderr);

#endif
