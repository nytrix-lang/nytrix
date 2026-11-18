/* Shared language-pipeline configuration and stage interfaces. */
#ifndef NY_BACKEND_WIRE_PIPE_H
#define NY_BACKEND_WIRE_PIPE_H

#include "base/options.h"

int ny_pipeline_run(ny_options *opt);

/*
 * Set by the driver when the process exits right after this run (no watch
 * loop, no repeated profile runs). The pipeline then skips freeing the
 * program AST and code generator state: the kernel reclaims it at exit, and
 * the teardown walk cost seconds on large UI programs.
 */
extern bool ny_pipeline_exit_after_run;

#endif
