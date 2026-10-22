#include "base/trace.h"
#include "base/debug.h"
#include "base/common.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool summary_emitted = false;

/*
 * Every verbose trace variable understood by the compiler front end and the
 * optimizer.  Kept in one place so the master switch and the summary dump are
 * always in sync with the actual instrumentation sites.
 */
static const char *const kKnownTraceEnv[] = {
    "NY_TRACE_ADCE",        "NY_TRACE_ALIAS",     "NY_TRACE_BCE",
    "NY_TRACE_CONSTTAB",    "NY_TRACE_DEVIRT",    "NY_TRACE_EMIT",
    "NY_TRACE_INLINE",      "NY_TRACE_IPA_CP",    "NY_TRACE_LOWER",
    "NY_TRACE_MACHINE_FAIL", "NY_TRACE_NATIVE_REACHABLE",
    "NY_TRACE_NATIVE_REACHABLE_EDGES", "NY_TRACE_NCE",
    "NY_TRACE_NATIVE_BUILD",
    "NY_TRACE_PHASE_MEMORY",
    "NY_TRACE_O3",          "NY_TRACE_OCE",       "NY_TRACE_PASS_FAIL",
    "NY_TRACE_PF",          "NY_TRACE_PURE_CALLS", "NY_TRACE_TCO",
    "NY_TRACE_VECTORIZE",   "NY_TRACE_OPRESOLVE", "NY_TRACE_ABI",
    "NY_TRACE_IRCE",
    "NY_TRACE_SR",          "NY_TRACE_LICM",      "NY_TRACE_CFG_SIMPLIFY",
    "NY_TRACE_JUMP_THREAD", "NY_TRACE_TBUF_ALLOCA",
    "NY_EGRAPH_TRACE",      "NY_DEBUG_DOM",
    "NY_TRACE_DEEP",        "NYTRIX_TRACE_DEEP",
    "NY_TRACE_BISECT",      "NY_TRACE_FINGERPRINT", "NY_TRACE_FLAKY_PASS",
    "NY_NATIVE_TRACE",
    "NY_DUMP_MACH",         "NY_DUMP_OBJ_NYIR",   "NY_KEEP_ASM",
    "NYTRIX_TRACE",         "NYTRIX_TRACE_CACHE", "NYTRIX_TRACE_CALLS",
    "NYTRIX_TRACE_CODEGEN", "NYTRIX_TRACE_COMPILE", "NYTRIX_TRACE_IMPORTS",
    "NYTRIX_TRACE_RESOLVE", "NYTRIX_TRACE_VALUES", "NYTRIX_TRACE_VERBOSE",
    "NYTRIX_DEBUG_INFER",   "NYTRIX_HM_DEBUG",    "NYTRIX_PROOF_DEBUG",
    "NYTRIX_MEM_TRACE",     "NYTRIX_INDEX_READ_PARITY",
    /*
     * Trace-like knobs that predate the central gate.  Registering them
     * makes NYTRIX_TRACE=all and NYTRIX_TRACE_FILTER cover the whole
     * surface and lets [nysum] report their effective state.
     */
    "NY_CARRY_DEBUG",       "NY_DBG_CALL",        "NY_DBG_STORE",
    "NY_DEBUG_ARITH_F32",   "NY_DEBUG_CALL_SCAN", "NY_DEBUG_CMP",
    "NY_DEBUG_FNPTR",       "NY_DEBUG_STRICT",
    "NY_DUMP_FN",           "NY_DUMP_LLVM",       "NY_DUMP_REGALLOC",
    "NYTRIX_MONO_TRACE",    "NYTRIX_NATIVE_TRACE_LOCALS",
    "NYTRIX_STD_TRACE",     "NYTRIX_STD_TRACE_CHUNKS",
};

static bool env_set(const char *name) {
  const char *v = getenv(name);
  return v && v[0];
}

static bool env_enabled(const char *name) {
  const char *value = getenv(name);
  return value && value[0] && strcmp(value, "0") != 0;
}

static const char *ny_trace_short_name(const char *name) {
  /*
   * Keep the two master-depth aliases distinguishable in the config list.
   */
  if (name && strcmp(name, "NYTRIX_TRACE_DEEP") == 0)
    return "TRACE_DEEP";
  static const char *const prefixes[] = {
      "NYTRIX_TRACE_", "NYTRIX_", "NY_TRACE_", "NY_",
  };
  if (!name)
    return "?";
  for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
    size_t len = strlen(prefixes[i]);
    if (strncmp(name, prefixes[i], len) == 0)
      return name + len;
  }
  return name;
}

bool ny_trace_all_enabled(void) {
  const char *deep = getenv("NY_TRACE_DEEP");
  if (deep && deep[0] && strcmp(deep, "0") != 0)
    return true;
  deep = getenv("NYTRIX_TRACE_DEEP");
  if (deep && deep[0] && strcmp(deep, "0") != 0)
    return true;
  const char *master = getenv("NYTRIX_TRACE");
  return master && strcmp(master, "all") == 0;
}

bool ny_trace_known(const char *env) {
  for (size_t i = 0; i < sizeof(kKnownTraceEnv) / sizeof(kKnownTraceEnv[0]);
       ++i)
    if (strcmp(env, kKnownTraceEnv[i]) == 0)
      return true;
  return false;
}

static bool ny_trace_default_heuristic(const char *env) {
  static const char *const defaults[] = {
      "NY_TRACE_ADCE",       "NY_TRACE_ALIAS",       "NY_TRACE_BCE",
      "NY_TRACE_CONSTTAB",   "NY_TRACE_DEVIRT",      "NY_TRACE_EMIT",
      "NY_TRACE_INLINE",     "NY_TRACE_IPA_CP",      "NY_TRACE_LOWER",
      "NY_TRACE_MACHINE_FAIL", "NY_TRACE_NATIVE_REACHABLE",
      "NY_TRACE_NCE",        "NY_TRACE_O3",          "NY_TRACE_OCE",
      "NY_TRACE_PASS_FAIL",  "NY_TRACE_PF",          "NY_TRACE_PURE_CALLS",
      "NY_TRACE_BISECT",     "NY_TRACE_FINGERPRINT",
      "NY_NATIVE_TRACE",
      "NY_TRACE_TBUF_ALLOCA", "NY_TRACE_TCO",         "NY_TRACE_VECTORIZE",
      "NY_TRACE_OPRESOLVE",  "NY_TRACE_ABI",          "NY_TRACE_IRCE",
      "NY_TRACE_SR",
      "NY_TRACE_LICM",       "NY_TRACE_CFG_SIMPLIFY", "NY_TRACE_JUMP_THREAD",
      "NY_EGRAPH_TRACE",     "NY_DEBUG_DOM",
  };
  for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); ++i)
    if (strcmp(env, defaults[i]) == 0)
      return true;
  return false;
}

bool ny_trace_enabled(const char *env) {
  const char *v = getenv(env);
  bool all = ny_trace_all_enabled();
  if (!summary_emitted &&
      (all || ny_dbg_is_enabled(NY_DBG_HEURISTIC))) {
    summary_emitted = true;
    ny_trace_summary(stderr);
  }
  if (v && v[0])
    return strcmp(v, "0") != 0;
  if (!all)
    return ny_dbg_is_enabled(NY_DBG_HEURISTIC) &&
           ny_trace_default_heuristic(env);
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
static long g_trace_suppressed_count;

/*
 * Windowed repeat compaction.  Instead of collapsing only back-to-back
 * duplicates (which never fires on interleaved channels like the ABI traces
 * that alternate arg/contract/expanded records), a small ring of recent
 * distinct lines holds each line until it leaves the window.  A line that
 * recurs inside the window is swallowed and its occurrence counted; when the
 * slot is evicted — or at exit — the line prints exactly once, as `... (xN)`
 * when it repeated.  A captured verbose log therefore carries every distinct
 * record once, with multiplicity, instead of the full flood.
 */
#define NY_TRACE_RING_SIZE 8
typedef struct {
  char line[1024];
  char tag[48];
  unsigned long repeats; /* occurrences beyond the first */
  bool used;
} ny_trace_slot_t;
static ny_trace_slot_t g_trace_ring[NY_TRACE_RING_SIZE];
static size_t g_trace_ring_next = 0;
static bool g_trace_ring_flush_registered = false;
static unsigned long g_trace_repeat_threshold = 1;
static bool g_trace_repeat_threshold_ready = false;
static unsigned long g_trace_dedup_saved = 0;

static unsigned long ny_trace_repeat_threshold(void) {
  if (!g_trace_repeat_threshold_ready) {
    const char *raw = getenv("NYTRIX_TRACE_REPEAT_THRESHOLD");
    if (raw && *raw) {
      char *end = NULL;
      unsigned long value = strtoul(raw, &end, 10);
      if (end != raw && *end == '\0' && value > 0)
        g_trace_repeat_threshold = value;
    }
    g_trace_repeat_threshold_ready = true;
  }
  return g_trace_repeat_threshold;
}

static void ny_trace_emit_slot(const ny_trace_slot_t *slot) {
  if (!slot->used)
    return;
  unsigned long total = slot->repeats + 1;
  if (total < ny_trace_repeat_threshold()) {
    for (unsigned long i = 0; i < total; ++i) {
      if (slot->tag[0] &&
          (ny_dbg_is_enabled(NY_DBG_HEURISTIC) || ny_dbg_json_enabled()))
        ny_dbg_heuristic_event(slot->tag, "%s", slot->line);
      else
        fprintf(stderr, "%s\n", slot->line);
    }
    return;
  }
  if (slot->tag[0] &&
      (ny_dbg_is_enabled(NY_DBG_HEURISTIC) || ny_dbg_json_enabled())) {
    if (slot->repeats)
      ny_dbg_heuristic_event(slot->tag, "%s (x%lu)", slot->line,
                             slot->repeats + 1);
    else
      ny_dbg_heuristic_event(slot->tag, "%s", slot->line);
  } else if (slot->repeats) {
    fprintf(stderr, "%s (x%lu)\n", slot->line, slot->repeats + 1);
  } else {
    fprintf(stderr, "%s\n", slot->line);
  }
}

static void ny_trace_flush_ring(void) {
  for (size_t k = 0; k < NY_TRACE_RING_SIZE; ++k)
    ny_trace_emit_slot(
        &g_trace_ring[(g_trace_ring_next + k) % NY_TRACE_RING_SIZE]);
  memset(g_trace_ring, 0, sizeof(g_trace_ring));
  g_trace_ring_next = 0;
  if (g_trace_suppressed_count > 0 && ny_dbg_verbosity() < 6) {
    fprintf(stderr, "%s %ld module-fn trace lines withheld (-vvvvvv shows them)\n",
            ny_ui_mark("hint"), g_trace_suppressed_count);
    g_trace_suppressed_count = 0;
  }
  if (g_trace_dedup_saved >= 16 && ny_dbg_verbosity() < 6) {
    fprintf(stderr, "%s trace compaction withheld %lu duplicate lines "
                    "(NYTRIX_TRACE_REPEAT_THRESHOLD=1 disables)\n",
            ny_ui_mark("hint"), g_trace_dedup_saved);
    g_trace_dedup_saved = 0;
  }
}

void ny_trace_maybe_summarize(void) {
  if (summary_emitted ||
      (!ny_trace_all_enabled() && !ny_dbg_is_enabled(NY_DBG_HEURISTIC)))
    return;
  summary_emitted = true;
  ny_trace_summary(stderr);
}

/*
 * Single-line trace writer with windowed repeat suppression: a line matching
 * any of the last NY_TRACE_RING_SIZE distinct records is swallowed and
 * counted; the surviving record prints once as `... (xN)` when it leaves the
 * window (or at exit), collapsing interleaved flood patterns such as
 * arg/contract/expanded ABI triples into one compact record per distinct
 * line.
 */
void ny_trace_line(const char *tag, const char *fmt, ...) {
  if (!fmt)
    return;
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  /*
   * Trace tags choose which subsystem is noisy; --dbg-filter=func:NAME
   * narrows those records to the selected function or its active context.
   */
  if (!ny_dbg_function_selected(buf) &&
      !ny_dbg_function_selected(NULL))
    return;

  /*
   * Tier gate: trace lines naming a module-qualified function (stdlib and
   * module code, e.g. "str.find", "core.dict.dict_clone") defer to
   * -vvvvvv, matching every other debug channel. The leading tag word
   * ("TYPE_RESOLVE str.find ...") and any later reference are both checked
   * against the module-prefix registry fed by ny_dbg_lower_fn.
   */
  if (getenv("NY_TRACE_PROBE") && strstr(buf, "str.find"))
    fprintf(stderr, "[probe verb=%d hit=%d registry=%s]\n",
            ny_dbg_verbosity(), ny_dbg_msg_names_module_fn(buf),
            ny_dbg_msg_names_module_fn("std") ? "has-std" : "no-std");
  if (ny_dbg_verbosity() < 6 && !getenv("NY_DEBUG_NO_TIERGATE") &&
      ny_dbg_msg_names_module_fn(buf)) {
    g_trace_suppressed_count++;
    return;
  }

  if (!g_trace_ring_flush_registered) {
    atexit(ny_trace_flush_ring);
    g_trace_ring_flush_registered = true;
  }
  const char *next_tag = tag ? tag : "";
  bool structured = next_tag[0] && (ny_dbg_is_enabled(NY_DBG_HEURISTIC) ||
                                    ny_dbg_json_enabled());
  for (size_t i = 0; i < NY_TRACE_RING_SIZE; ++i) {
    ny_trace_slot_t *slot = &g_trace_ring[i];
    if (slot->used && strcmp(buf, slot->line) == 0 &&
        strcmp(structured ? next_tag : "", slot->tag) == 0) {
      slot->repeats++;
      g_trace_dedup_saved++;
      return;
    }
  }
  ny_trace_slot_t *slot = &g_trace_ring[g_trace_ring_next];
  ny_trace_emit_slot(slot); /* evict: emit the outgoing record with its count */
  snprintf(slot->line, sizeof(slot->line), "%s", buf);
  snprintf(slot->tag, sizeof(slot->tag), "%s",
           structured ? next_tag : "");
  slot->repeats = 0;
  slot->used = true;
  g_trace_ring_next = (g_trace_ring_next + 1) % NY_TRACE_RING_SIZE;
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
  char on[1024] = {0}, off[1024] = {0};
  size_t on_len = 0, off_len = 0;
  size_t on_count = 0, off_count = 0;
  bool any_on = false, any_off = false;
  const char *filter = getenv("NYTRIX_TRACE_FILTER");
  for (size_t i = 0; i < sizeof(kKnownTraceEnv) / sizeof(kKnownTraceEnv[0]);
       ++i) {
    if (ny_trace_enabled(kKnownTraceEnv[i])) {
      on_count++;
      int n = snprintf(on + on_len, sizeof(on) - on_len, "%s%s",
                       any_on ? "," : "",
                       ny_trace_short_name(kKnownTraceEnv[i]));
      if (n > 0 && (size_t)n < sizeof(on) - on_len)
        on_len += (size_t)n;
      any_on = true;
    }
  }
  for (size_t i = 0; i < sizeof(kKnownTraceEnv) / sizeof(kKnownTraceEnv[0]);
       ++i) {
    if (env_set(kKnownTraceEnv[i]) &&
        strcmp(getenv(kKnownTraceEnv[i]), "0") == 0) {
      off_count++;
      int n = snprintf(off + off_len, sizeof(off) - off_len, "%s%s",
                       any_off ? "," : "",
                       ny_trace_short_name(kKnownTraceEnv[i]));
      if (n > 0 && (size_t)n < sizeof(off) - off_len)
        off_len += (size_t)n;
      any_off = true;
    }
  }
  char detail[3072];
  /*
   * At the full heuristic tier the active trace list is the index into the
   * decision records that follow.  Keep it visible without requiring a
   * second environment switch; lower-volume explicit trace runs can retain
   * the one-line count unless they request names.
   */
  if (env_enabled("NYTRIX_TRACE_CONFIG_VERBOSE") ||
      ny_dbg_is_enabled(NY_DBG_HEURISTIC)) {
    snprintf(detail, sizeof(detail),
             "deep=%s heuristic=%s filter=%s enabled=%zu[%s] disabled=%zu[%s]",
             ny_trace_all_enabled() ? "true" : "false",
             ny_dbg_is_enabled(NY_DBG_HEURISTIC) ? "true" : "false",
             filter && filter[0] ? filter : "-", on_count,
             any_on ? on : "-", off_count, any_off ? off : "-");
  } else {
    snprintf(detail, sizeof(detail),
             "deep=%s heuristic=%s filter=%s enabled=%zu disabled=%zu (set NYTRIX_TRACE_CONFIG_VERBOSE=1 for names)",
             ny_trace_all_enabled() ? "true" : "false",
             ny_dbg_is_enabled(NY_DBG_HEURISTIC) ? "true" : "false",
             filter && filter[0] ? filter : "-", on_count, off_count);
  }
  ny_dbg_heuristic_event("TRACE_CONFIG", "%s", detail);
}
