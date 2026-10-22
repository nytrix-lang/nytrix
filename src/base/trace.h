#ifndef NY_BASE_TRACE_H
#define NY_BASE_TRACE_H

#include <stdbool.h>
#include <stdio.h>

/*
 * Centralized verbose-trace gate.
 *
 * Every per-feature trace reads a dedicated environment variable (NY_TRACE_*,
 * NY_*_TRACE, NYTRIX_TRACE_*).  This module gives each of those sites one
 * call site whose behavior is:
 *
 *   - the specific variable is authoritative when set: any value other than
 *     "0" enables that single trace, "0" disables it even under the master
 *     switch;
 *   - otherwise the master switch applies: NYTRIX_TRACE=all (the value the
 *     -trace option family centers on) enables every known trace, and the
 *     NY_TRACE_DEEP/NYTRIX_TRACE_DEEP deep-trace variables do the same.
 *
 * So a single `NYTRIX_TRACE=all ny file.ny` reproduces the full verbose
 * diagnostics of a dozen hand-set variables, with per-tag "=0" still available
 * to silence one noisy stream.  Use ny_trace_summary() to emit a compact,
 * machine-friendly text table of the effective state that can be pasted into
 * a bug report or an agent session.
 */

bool ny_trace_enabled(const char *env);
bool ny_trace_all_enabled(void);
bool ny_trace_known(const char *env);
void ny_trace_summary(FILE *out);
void ny_trace_maybe_summarize(void);
void ny_trace_line(const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif
