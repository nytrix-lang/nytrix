#include "base/trace.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Every verbose trace variable understood by the compiler front end and the
 * optimizer.  Kept in one place so the master switch and the summary dump are
 * always in sync with the actual instrumentation sites.
 */
static const char *const kKnownTraceEnv[] = {
    "NY_TRACE_ADCE",        "NY_TRACE_ALIAS",     "NY_TRACE_BCE",
    "NY_TRACE_CONSTTAB",    "NY_TRACE_DEVIRT",    "NY_TRACE_EMIT",
    "NY_TRACE_INLINE",      "NY_TRACE_IPA_CP",    "NY_TRACE_LOWER",
    "NY_TRACE_MACHINE_FAIL", "NY_TRACE_NATIVE_REACHABLE", "NY_TRACE_NCE",
    "NY_TRACE_O3",          "NY_TRACE_OCE",       "NY_TRACE_PASS_FAIL",
    "NY_TRACE_PF",          "NY_TRACE_PURE_CALLS", "NY_TRACE_TCO",
    "NY_TRACE_VECTORIZE",   "NY_EGRAPH_TRACE",    "NY_DEBUG_DOM",
    "NY_TRACE_DEEP",        "NYTRIX_TRACE_DEEP",
    "NY_NATIVE_TRACE",
    "NY_DUMP_MACH",         "NY_DUMP_OBJ_NYIR",   "NY_KEEP_ASM",
    "NYTRIX_TRACE",         "NYTRIX_TRACE_CACHE", "NYTRIX_TRACE_CALLS",
    "NYTRIX_TRACE_CODEGEN", "NYTRIX_TRACE_COMPILE", "NYTRIX_TRACE_IMPORTS",
    "NYTRIX_TRACE_RESOLVE", "NYTRIX_TRACE_VALUES", "NYTRIX_TRACE_VERBOSE",
    "NYTRIX_DEBUG_INFER",   "NYTRIX_HM_DEBUG",    "NYTRIX_PROOF_DEBUG",
    "NYTRIX_MEM_TRACE",     "NYTRIX_INDEX_READ_PARITY",
};

static bool env_set(const char *name) {
  const char *v = getenv(name);
  return v && v[0];
}

static bool env_enabled(const char *name) {
  return env_set(name) && strcmp(getenv(name), "0") != 0;
}

bool ny_trace_all_enabled(void) {
  return env_enabled("NY_TRACE_ALL") ||
         env_enabled("NY_TRACE_DEEP") || env_enabled("NYTRIX_TRACE_DEEP") ||
         (env_set("NYTRIX_TRACE") &&
          strcmp(getenv("NYTRIX_TRACE"), "all") == 0);
}

bool ny_trace_known(const char *env) {
  for (size_t i = 0; i < sizeof(kKnownTraceEnv) / sizeof(kKnownTraceEnv[0]);
       ++i)
    if (strcmp(env, kKnownTraceEnv[i]) == 0)
      return true;
  return false;
}

bool ny_trace_enabled(const char *env) {
  ny_trace_maybe_summarize();
  const char *v = getenv(env);
  if (v && v[0])
    return strcmp(v, "0") != 0;
  if (!ny_trace_all_enabled())
    return false;
  const char *filter = getenv("NYTRIX_TRACE_FILTER");
  if (!filter || !filter[0])
    return true;
  const char *name = strrchr(env, '_');
  name = name ? name + 1 : env;
  size_t n = strlen(name);
  const char *p = filter;
  while (*p) {
    while (*p == ',' || *p == ' ' || *p == '\t') ++p;
    const char *q = p;
    while (*q && *q != ',') ++q;
    const char *end = q;
    while (end > p && (end[-1] == ' ' || end[-1] == '\t')) --end;
    size_t len = (size_t)(end - p);
    if ((len == n && strncasecmp(p, name, n) == 0) ||
        (len == strlen(env) && strncasecmp(p, env, len) == 0))
      return true;
    p = q;
  }
  return false;
}

/*
 * Emit the compact configuration table once, on the first trace check, when a
 * master switch turned traces on.  The header lands before any instrumented
 * output so a captured log begins with a self-describing, agent-friendly block.
 */
static bool summary_emitted = false;
static char g_trace_last_line[1024];
static unsigned long g_trace_line_repeats = 0;
static bool g_trace_line_flush_registered = false;

static void ny_trace_flush_line_repeats(void) {
  if (g_trace_line_repeats == 0 || g_trace_last_line[0] == '\0')
    return;
  fprintf(stderr, "%s (x%lu)\n", g_trace_last_line,
          g_trace_line_repeats + 1);
  g_trace_line_repeats = 0;
}

void ny_trace_maybe_summarize(void) {
  if (summary_emitted)
    return;
  summary_emitted = true;
  if (ny_trace_all_enabled())
    ny_trace_summary(stderr);
}

/*
 * Single-line trace writer with consecutive-repeat suppression: an identical
 * line re-printed back-to-back (the classic log-flood pattern in per-instruction
 * passes) is held and emitted once as `... (xN)` when a different line follows,
 * collapsing tens of thousands of lines into a handful of distinct records.
 */
void ny_trace_line(const char *tag, const char *fmt, ...) {
  (void)tag;
  if (!fmt)
    return;
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  if (!g_trace_line_flush_registered) {
    atexit(ny_trace_flush_line_repeats);
    g_trace_line_flush_registered = true;
  }
  if (g_trace_last_line[0] != '\0' && strcmp(buf, g_trace_last_line) == 0) {
    ++g_trace_line_repeats;
    return;
  }
  ny_trace_flush_line_repeats();
  snprintf(g_trace_last_line, sizeof(g_trace_last_line), "%s", buf);
  fprintf(stderr, "%s\n", buf);
}

/*
 * Compact, agent-friendly text table of the effective trace configuration:
 * master state, every tag currently active, and every tag explicitly silenced
 * with "=0".  Empty categories collapse to a single `-` so the dump carries
 * only the information that differs from a default run.
 */
void ny_trace_summary(FILE *out) {
  if (!out)
    return;
  fprintf(out, "[nysum] master=%d NY_TRACE_ALL\n", ny_trace_all_enabled());
  const char *filter = getenv("NYTRIX_TRACE_FILTER");
  if (filter && filter[0])
    fprintf(out, "[nysum] filter=%s\n", filter);

  fputs("[nysum] on:", out);
  bool any_on = false;
  for (size_t i = 0; i < sizeof(kKnownTraceEnv) / sizeof(kKnownTraceEnv[0]);
       ++i) {
    if (ny_trace_enabled(kKnownTraceEnv[i])) {
      fprintf(out, " %s", kKnownTraceEnv[i]);
      any_on = true;
    }
  }
  fputs(any_on ? "\n" : " -\n", out);

  fputs("[nysum] off:", out);
  bool any_off = false;
  for (size_t i = 0; i < sizeof(kKnownTraceEnv) / sizeof(kKnownTraceEnv[0]);
       ++i) {
    if (env_set(kKnownTraceEnv[i]) &&
        strcmp(getenv(kKnownTraceEnv[i]), "0") == 0) {
      fprintf(out, " %s", kKnownTraceEnv[i]);
      any_off = true;
    }
  }
  fputs(any_off ? "\n" : " -\n", out);
}
