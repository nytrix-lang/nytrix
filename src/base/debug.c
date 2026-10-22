#include "base/debug.h"
#include "base/json.h"
#include "base/common.h"
#include "code/backend/ir/opt/util.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <nyr/ir.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(NY_TOOL_ENTRY)
/* Small tools use the shared logger without linking the NYIR implementation. */
bool nyr_diff_is_empty(const nyr_func_t *before, const nyr_func_t *after) {
  (void)before;
  (void)after;
  return 1;
}

void nyr_dump_diff(FILE *out, const nyr_func_t *before,
                    const nyr_func_t *after, const char *tag) {
  (void)out;
  (void)before;
  (void)after;
  (void)tag;
}

void nyr_dump(FILE *out, const nyr_func_t *func, const char *phase) {
  (void)out;
  (void)func;
  (void)phase;
}

uint64_t nyr_debug_fingerprint(const nyr_func_t *func) {
  (void)func;
  return 0;
}
#endif

ny_dbg_flag_t ny_dbg_enabled = 0;

static bool dbg_color_enabled = false;
static int dbg_indent_level = 0;
#define DBG_CONTEXT_MAX 64
static const char *dbg_context_stack[DBG_CONTEXT_MAX];
static bool dbg_show_location = false;
static size_t dbg_last_lower_length = 0;
static uint64_t dbg_last_lower_fingerprint = 0;
static bool dbg_have_last_lower = false;
static uint64_t dbg_last_ir_full_fingerprint;
static bool dbg_have_last_ir_full;
static int dbg_ir_unchanged_strikes; /* skipped identical full dumps */
static char dbg_last_ir_full_context[256];
#define DBG_HEURISTIC_DEDUP_CAP 128
typedef struct {
  char algorithm[96];
  char detail[768];
  unsigned repeats;
} dbg_heuristic_seen_t;
static dbg_heuristic_seen_t dbg_heuristic_seen[DBG_HEURISTIC_DEDUP_CAP];
static size_t dbg_heuristic_seen_count;
static bool dbg_heuristic_flush_registered;
typedef struct {
  int vreg;
  int preg;
  char reason[128];
  unsigned count;
} dbg_regalloc_entry_t;
static dbg_regalloc_entry_t *dbg_regalloc_entries;
static size_t dbg_regalloc_count;
static size_t dbg_regalloc_capacity;
static char dbg_filter_value[512];
static bool dbg_filter_active = false;
static bool dbg_delta_only = false;
static bool dbg_json_enabled = false;
static ny_dbg_flag_t dbg_forced_on = 0;
static ny_dbg_flag_t dbg_forced_off = 0;
static const char *dbg_flag_name(ny_dbg_flag_t flag);
static void dbg_flush_heuristic_repeat(void);
int ny_dbg_msg_names_module_fn(const char *msg);
static void dbg_json_emit(ny_dbg_flag_t flag, const char *event,
                          const char *detail);

void ny_dbg_set_filter(const char *filter) {
  if (!filter || !*filter) {
    dbg_filter_value[0] = '\0';
    dbg_filter_active = false;
    return;
  }
  snprintf(dbg_filter_value, sizeof(dbg_filter_value), "%s", filter);
  dbg_filter_active = true;
}

void ny_dbg_set_delta_only(bool enabled) {
  dbg_delta_only = enabled;
  if (enabled) {
    dbg_forced_on |= NY_DBG_IR_DIFF;
    dbg_forced_off &= ~NY_DBG_IR_DIFF;
    ny_dbg_enabled |= NY_DBG_IR_DIFF;
  } else {
    dbg_forced_on &= ~NY_DBG_IR_DIFF;
  }
}

void ny_dbg_set_json(bool enabled) { dbg_json_enabled = enabled; }
bool ny_dbg_json_enabled(void) { return dbg_json_enabled; }

void ny_dbg_logv(const char *marker, const char *fmt, va_list ap) {
  if (!dbg_json_enabled || !fmt)
    return;
  char message[2048];
  vsnprintf(message, sizeof(message), fmt, ap);
  dbg_json_emit(NY_DBG_PASS, marker && marker[0] ? marker : "MESSAGE", message);
}

static void dbg_json_string(const char *s) {
  fputc('"', stderr);
  for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; ++p) {
    char escaped[7];
    size_t n = ny_json_escape_byte(*p, escaped);
    (void)fwrite(escaped, 1, n, stderr);
  }
  fputc('"', stderr);
}

static void dbg_json_emit(ny_dbg_flag_t flag, const char *event,
                          const char *detail) {
  char timebuf[32];
  time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%S%z", &tm);
  fputs("{\"time\":", stderr);
  dbg_json_string(timebuf);
  fputs(",\"category\":", stderr);
  dbg_json_string(dbg_flag_name(flag));
  fputs(",\"event\":", stderr);
  dbg_json_string(event);
  fputs(",\"detail\":", stderr);
  dbg_json_string(detail);
  if (dbg_indent_level > 0 && dbg_indent_level <= DBG_CONTEXT_MAX &&
      dbg_context_stack[dbg_indent_level - 1]) {
    fputs(",\"context\":", stderr);
    dbg_json_string(dbg_context_stack[dbg_indent_level - 1]);
  }
  fputs("}\n", stderr);
}

static void dbg_json_printf(ny_dbg_flag_t flag, const char *event,
                            const char *fmt, ...) {
  char detail[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(detail, sizeof(detail), fmt ? fmt : "", ap);
  va_end(ap);
  dbg_json_emit(flag, event, detail);
}

static void dbg_json_emit_file(ny_dbg_flag_t flag, const char *event,
                               FILE *stream) {
  if (!stream)
    return;
  long end = ftell(stream);
  if (end < 0 || fseek(stream, 0, SEEK_SET) != 0)
    return;
  size_t cap = (size_t)end;
  char *contents = (char *)malloc(cap + 1);
  if (!contents)
    return;
  size_t n = fread(contents, 1, cap, stream);
  contents[n] = '\0';
  dbg_json_emit(flag, event, contents);
  free(contents);
}

static bool dbg_filter_category_matches(const char *list, ny_dbg_flag_t flag) {
  if (!list)
    return false;
  const char *name = dbg_flag_name(flag);
  size_t name_len = strlen(name);
  const char *p = list;
  while (*p) {
    while (*p == ',')
      ++p;
    const char *end = strchr(p, ',');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len == name_len && strncmp(p, name, len) == 0)
      return true;
    if (!end)
      break;
    p = end + 1;
  }
  return false;
}

static bool dbg_filter_value_matches(const char *needle, const char *message) {
  if (!needle || !*needle || !message)
    return false;
  char *end = NULL;
  long wanted = strtol(needle, &end, 10);
  if (end == needle || *end || wanted < 0)
    return false;
  char token[32];
  snprintf(token, sizeof(token), "v%ld", wanted);
  size_t len = strlen(token);
  const char *p = message;
  while ((p = strstr(p, token)) != NULL) {
    bool left = p == message ||
                !(p[-1] == '0' || p[-1] == '1' || p[-1] == '2' ||
                  p[-1] == '3' || p[-1] == '4' || p[-1] == '5' ||
                  p[-1] == '6' || p[-1] == '7' || p[-1] == '8' || p[-1] == '9');
    char next = p[len];
    bool right = !(next == '0' || next == '1' || next == '2' || next == '3' ||
                   next == '4' || next == '5' || next == '6' || next == '7' ||
                   next == '8' || next == '9');
    if (left && right)
      return true;
    p += len;
  }
  return false;
}

/*
 * Filters are ORed clauses separated by ';'. The category list itself may
 * contain commas: --dbg-filter='cat:PASS,IR_DIFF;func:main'.
 */
static bool dbg_filter_allows(ny_dbg_flag_t flag, const char *message,
                              const char *pass) {
  if (!dbg_filter_active)
    return true;
  char clauses[sizeof(dbg_filter_value)];
  memcpy(clauses, dbg_filter_value, sizeof(clauses));
  clauses[sizeof(clauses) - 1] = '\0';
  for (char *clause = clauses; clause && *clause;) {
    char *next = strchr(clause, ';');
    if (next)
      *next++ = '\0';
    while (*clause == ' ' || *clause == '\t')
      ++clause;
    char *tail = clause + strlen(clause);
    while (tail > clause && (tail[-1] == ' ' || tail[-1] == '\t'))
      *--tail = '\0';
    if (strncmp(clause, "cat:", 4) == 0 &&
        dbg_filter_category_matches(clause + 4, flag))
      return true;
    if (strncmp(clause, "pass:", 5) == 0 && pass &&
        strcmp(clause + 5, pass) == 0)
      return true;
    if (strncmp(clause, "func:", 5) == 0 && clause[5]) {
      const char *ctx =
          dbg_indent_level > 0
              ? dbg_context_stack[(dbg_indent_level - 1) < DBG_CONTEXT_MAX
                                      ? dbg_indent_level - 1
                                      : DBG_CONTEXT_MAX - 1]
              : NULL;
      if ((ctx && strstr(ctx, clause + 5)) ||
          (message && strstr(message, clause + 5)))
        return true;
    }
    if (strncmp(clause, "value:", 6) == 0 &&
        dbg_filter_value_matches(clause + 6, message))
      return true;
    clause = next;
  }
  return false;
}

bool ny_dbg_function_selected(const char *function) {
  if (!dbg_filter_active)
    return true;
  char clauses[sizeof(dbg_filter_value)];
  memcpy(clauses, dbg_filter_value, sizeof(clauses));
  clauses[sizeof(clauses) - 1] = '\0';
  bool has_func_clause = false;
  bool selected = false;
  for (char *clause = clauses; clause && *clause;) {
    char *next = strchr(clause, ';');
    if (next)
      *next++ = '\0';
    while (*clause == ' ' || *clause == '\t')
      ++clause;
    char *tail = clause + strlen(clause);
    while (tail > clause && (tail[-1] == ' ' || tail[-1] == '\t'))
      *--tail = '\0';
    if (strncmp(clause, "func:", 5) == 0 && clause[5]) {
      has_func_clause = true;
      const char *ctx =
          dbg_indent_level > 0
              ? dbg_context_stack[(dbg_indent_level - 1) < DBG_CONTEXT_MAX
                                      ? dbg_indent_level - 1
                                      : DBG_CONTEXT_MAX - 1]
              : NULL;
      if ((function && strstr(function, clause + 5)) ||
          (!function && ctx && strstr(ctx, clause + 5)))
        selected = true;
    } else if (*clause) {
      /*
       * The public filter combines clauses with OR. A category or
       * pass clause may independently select raw records, so only
       * apply this cheap prefilter for function-only filters.
       */
      return true;
    }
    clause = next;
  }
  return !has_func_clause || selected;
}

static bool dbg_pass_has_delta(const char *message) {
  if (!message)
    return true;
  const char *insts = strstr(message, "insts=");
  const char *values = strstr(message, "values=");
  if (!insts || !values)
    return true;
  char *end = NULL;
  long before_i = strtol(insts + 6, &end, 10);
  if (end == insts + 6 || strncmp(end, "->", 2) != 0)
    return true;
  long after_i = strtol(end + 2, &end, 10);
  if (end == NULL)
    return true;
  long before_v = strtol(values + 7, &end, 10);
  if (end == values + 7 || strncmp(end, "->", 2) != 0)
    return true;
  long after_v = strtol(end + 2, NULL, 10);
  return before_i != after_i || before_v != after_v;
}

/*
 * Nerd-font glyph per category: same icon vocabulary as the CLI banners,
 * ASCII fallback via ny_ui_glyph.
 * Per-category mark: plain BMP Unicode that renders in any terminal font
 * (check/arrow/gear/bolt/document/...), same vocabulary as the CLI marks.
 */
static const char *dbg_flag_icon(ny_dbg_flag_t flag) {
  switch (flag) {
  case NY_DBG_PASS:
    return "\xe2\x9c\x93"; /* check */
  case NY_DBG_LOWER:
    return "\xe2\x86\x92"; /* arrow */
  case NY_DBG_REGALLOC:
    return "\xe2\x9a\x99"; /* gear */
  case NY_DBG_ABI:
    return "\xe2\x87\x84"; /* interface */
  case NY_DBG_IR_FULL:
    return "\xe2\x96\xa4"; /* document */
  case NY_DBG_IR_DIFF:
    return "\xc2\xb1"; /* plus/minus */
  case NY_DBG_TYPE:
  case NY_DBG_TYPE_DETAIL:
    return "\xe2\x89\xa1"; /* identical */
  case NY_DBG_EFFECT:
    return "\xe2\x88\x85"; /* effects set */
  case NY_DBG_OPT_MISS:
    return "\xe2\x97\x8c"; /* dotted circle */
  case NY_DBG_MEM:
    return "\xe2\x96\xa6"; /* grid */
  case NY_DBG_HEURISTIC:
    return "\xe2\x9c\xa6"; /* star */
  case NY_DBG_VERIFY:
    return "\xe2\x9a\x91"; /* flag */
  case NY_DBG_TRAIT:
    return "\xe2\x9a\x91"; /* flag */
  case NY_DBG_ELF:
    return "\xe2\x96\xa3"; /* box */
  default:
    return "";
  }
}

static const char *dbg_flag_name(ny_dbg_flag_t flag) {
  switch (flag) {
  case NY_DBG_PASS:
    return "PASS";
  case NY_DBG_IR_DIFF:
    return "IR_DIFF";
  case NY_DBG_CFG:
    return "CFG";
  case NY_DBG_INST:
    return "INST";
  case NY_DBG_VALUE:
    return "VALUE";
  case NY_DBG_LOWER:
    return "LOWER";
  case NY_DBG_REGALLOC:
    return "REGALLOC";
  case NY_DBG_ABI:
    return "ABI";
  case NY_DBG_ELF:
    return "ELF";
  case NY_DBG_TYPE:
    return "TYPE";
  case NY_DBG_EFFECT:
    return "EFFECT";
  case NY_DBG_LEX:
    return "LEX";
  case NY_DBG_PARSE:
    return "PARSE";
  case NY_DBG_VERIFY:
    return "VERIFY";
  case NY_DBG_ROLLBACK:
    return "ROLLBACK";
  case NY_DBG_ESCAPE:
    return "ESCAPE";
  case NY_DBG_IR_FULL:
    return "IR_FULL";
  case NY_DBG_VALUE_TRACE:
    return "VALUE_TRACE";
  case NY_DBG_LOWER_DETAILED:
    return "LOWER_DETAIL";
  case NY_DBG_TYPE_DETAIL:
    return "TYPE_DETAIL";
  case NY_DBG_OPT_MISS:
    return "OPT_MISS";
  case NY_DBG_MEM:
    return "MEM";
  case NY_DBG_HEURISTIC:
    return "HEURISTIC";
  case NY_DBG_TRAIT:
    return "TRAIT";
  case NY_DBG_DEEP:
    return "DEEP";
  default:
    return "UNKNOWN";
  }
}

static const char *dbg_flag_color(ny_dbg_flag_t flag) {
  if (!dbg_color_enabled)
    return "";
  switch (flag) {
  case NY_DBG_PASS:
    return "\x1b[36m"; // cyan
  case NY_DBG_IR_DIFF:
    return "\x1b[35m"; // magenta
  case NY_DBG_CFG:
    return "\x1b[33m"; // yellow
  case NY_DBG_INST:
    return "\x1b[32m"; // green
  case NY_DBG_VALUE:
    return "\x1b[34m"; // blue
  case NY_DBG_LOWER:
    return "\x1b[93m"; // bright yellow
  case NY_DBG_REGALLOC:
    return "\x1b[31m"; // red
  case NY_DBG_ABI:
    return "\x1b[95m"; // bright magenta
  case NY_DBG_ELF:
    return "\x1b[93m"; // bright yellow
  case NY_DBG_TYPE:
    return "\x1b[96m"; // bright cyan
  case NY_DBG_EFFECT:
    return "\x1b[95m"; // bright magenta
  case NY_DBG_LEX:
    return "\x1b[33m"; // yellow
  case NY_DBG_PARSE:
    return "\x1b[93m"; // bright yellow
  case NY_DBG_VERIFY:
    return "\x1b[31;1m"; // bright red
  case NY_DBG_ROLLBACK:
    return "\x1b[33;1m"; // bright yellow
  case NY_DBG_ESCAPE:
    return "\x1b[92m"; // bright green
  case NY_DBG_IR_FULL:
    return "\x1b[95m"; // bright magenta
  case NY_DBG_VALUE_TRACE:
    return "\x1b[96m"; // bright cyan
  case NY_DBG_LOWER_DETAILED:
    return "\x1b[93m"; // bright yellow
  case NY_DBG_TYPE_DETAIL:
    return "\x1b[96m"; // bright cyan
  case NY_DBG_OPT_MISS:
    return "\x1b[91m"; // bright red
  case NY_DBG_MEM:
    return "\x1b[93m"; // bright yellow
  case NY_DBG_HEURISTIC:
    return "\x1b[1;93m"; // bright yellow
  case NY_DBG_TRAIT:
    return "\x1b[1;96m";
  default:
    return "\x1b[0m";
  }
}

static const char *dbg_reset_color(void) {
  return dbg_color_enabled ? "\x1b[0m" : "";
}

/*
 * Give the action word a second visual cue while keeping every diagnostic
 * readable on the standard 16-color terminal palette.
 */
static const char *dbg_event_color(const char *message) {
  if (!dbg_color_enabled || !message)
    return NULL;
  if (strncmp(message, "FAIL", 4) == 0 || strncmp(message, "ERROR", 5) == 0 ||
      strncmp(message, "GLOBAL", 6) == 0)
    return "\x1b[1;31m";
  if (strncmp(message, "ENTER", 5) == 0)
    return "\x1b[1;32m";
  if (strncmp(message, "EXIT", 4) == 0 || strncmp(message, "TRACK", 5) == 0)
    return "\x1b[1;36m";
  if (strncmp(message, "PASS", 4) == 0)
    return "\x1b[1;34m";
  if (strncmp(message, "pass=", 5) == 0)
    return "\x1b[1;34m";
  if (strncmp(message, "suppressing", 11) == 0)
    return "\x1b[2m";
  return NULL;
}

/*
 * Debug crash sink: the last NY_DBG_RING_LINES formatted debug lines are
 * kept in a fixed static ring while debug verbosity is active.  On a fatal
 * signal (SIGSEGV/SIGABRT/SIGBUS/SIGFPE) the handler writes the ring to
 * NYTRIX_DBG_CRASH_DUMP (default build/debug/last_debug.log) using only
 * async-signal-safe write(), so the final diagnostics before a crash
 * survive even when stderr scrolling or buffering lost them.  Zero cost
 * when debug output is disabled: the capture hook and the handler
 * registration are both gated on ny_dbg_enabled.
 */
static int dbg_verbosity;
static int dbg_scope_stdlib;
int ny_dbg_verbosity(void) { return dbg_verbosity; }
void ny_dbg_scope_stdlib(int on) { dbg_scope_stdlib = on; }
int ny_dbg_scope_is_stdlib(void) { return dbg_scope_stdlib; }
/*
 * Stdlib-scoped detail is reserved for the deepest tier (-vvvvvv).
 */
int ny_dbg_suppressed_by_scope(void) {
  return dbg_scope_stdlib && dbg_verbosity < 6;
}

#define NY_DBG_RING_LINES 256
#define NY_DBG_RING_LINE_MAX 240
static char dbg_ring[NY_DBG_RING_LINES][NY_DBG_RING_LINE_MAX];
static unsigned dbg_ring_head;
static int dbg_ring_active;
static char dbg_ring_path[512];

static void ny_dbg_crash_sink(int sig) {
  ny_dbg_crash_flush(sig, NULL);
  if (!dbg_ring_active)
    return;
  int fd = open(dbg_ring_path[0] ? dbg_ring_path : "build/debug/last_debug.log",
                O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    return;
  const char *header = "nytrix debug crash sink\n";
  size_t header_len = 25;
  if (write(fd, header, header_len) < 0) {
  }
  unsigned i = dbg_ring_head;
  do {
    if (dbg_ring[i][0]) {
      size_t len = 0;
      while (len < NY_DBG_RING_LINE_MAX - 1 && dbg_ring[i][len])
        len++;
      dbg_ring[i][len] = '\n';
      if (write(fd, dbg_ring[i], len + 1) < 0) {
      }
      dbg_ring[i][len] = '\0';
    }
    i = (i + 1) % NY_DBG_RING_LINES;
  } while (i != dbg_ring_head);
  close(fd);
  /*
   * Re-raise with the default disposition: returning from a fault handler
   * re-executes the faulting instruction, which would spin forever.
   */
  signal(sig, SIG_DFL);
  raise(sig);
}

static void ny_dbg_ring_capture(const char *flag_name, const char *message) {
  if (!dbg_ring_active || !message)
    return;
  char *dst = dbg_ring[dbg_ring_head];
  unsigned used = 0;
  const char *src = flag_name;
  while (src && *src && used < NY_DBG_RING_LINE_MAX - 2)
    dst[used++] = *src++;
  if (used < NY_DBG_RING_LINE_MAX - 2)
    dst[used++] = ' ';
  src = message;
  while (src && *src && used < NY_DBG_RING_LINE_MAX - 2)
    dst[used++] = *src++;
  dst[used] = '\0';
  dbg_ring_head = (dbg_ring_head + 1) % NY_DBG_RING_LINES;
}

void ny_dbg_init_from_verbosity(int verbosity) {
  dbg_verbosity = verbosity;
  ny_dbg_enabled = 0;
  if (verbosity >= 1)
    ny_dbg_enabled |= NY_DBG_PASS;
  if (verbosity >= 2)
    ny_dbg_enabled |= NY_DBG_IR_DIFF | NY_DBG_CFG | NY_DBG_VALUE |
                      NY_DBG_VERIFY | NY_DBG_ROLLBACK | NY_DBG_TYPE |
                      NY_DBG_EFFECT;
  if (verbosity >= 3)
    ny_dbg_enabled |= NY_DBG_INST | NY_DBG_LOWER | NY_DBG_ABI | NY_DBG_ESCAPE |
                      NY_DBG_LEX | NY_DBG_PARSE;
  if (verbosity >= 4)
    ny_dbg_enabled |= NY_DBG_REGALLOC | NY_DBG_ELF | NY_DBG_IR_FULL |
                      NY_DBG_VALUE_TRACE | NY_DBG_LOWER_DETAILED |
                      NY_DBG_TYPE_DETAIL | NY_DBG_HEURISTIC;
  if (verbosity >= 5)
    ny_dbg_enabled |=
        NY_DBG_OPT_MISS | NY_DBG_MEM | NY_DBG_REGALLOC | NY_DBG_ELF;
  if (verbosity >= 6)
    ny_dbg_enabled |= NY_DBG_DEEP;
  ny_dbg_enabled |= dbg_forced_on;
  ny_dbg_enabled &= ~dbg_forced_off;

  /*
   * Routine pass output stays compact. Source locations are useful for the
   * detailed tiers and can be forced on without increasing verbosity.
   */
  dbg_show_location = ny_env_truthy(getenv("NY_DEBUG_LOCATION"));

  /*
   * Crash sink: only while debug output is active.
   */
  dbg_ring_active = ny_dbg_enabled != 0;
  if (dbg_ring_active) {
    const char *dump = getenv("NYTRIX_DBG_CRASH_DUMP");
    snprintf(dbg_ring_path, sizeof(dbg_ring_path), "%s",
             dump && *dump ? dump : "build/debug/last_debug.log");
    signal(SIGSEGV, ny_dbg_crash_sink);
    signal(SIGABRT, ny_dbg_crash_sink);
    signal(SIGBUS, ny_dbg_crash_sink);
    signal(SIGFPE, ny_dbg_crash_sink);
    dbg_ring_head = 0;
    memset(dbg_ring, 0, sizeof(dbg_ring));
  }

  /*
   * Auto-detect color support
   */
  dbg_color_enabled = ny_color_enabled_for_fd(STDERR_FILENO);
}

void ny_dbg_set_color(bool enable) { dbg_color_enabled = enable; }

void ny_dbg_set_flag(ny_dbg_flag_t flag, bool on) {
  if (on) {
    dbg_forced_on |= flag;
    dbg_forced_off &= ~flag;
    ny_dbg_enabled |= flag;
  } else {
    dbg_forced_off |= flag;
    dbg_forced_on &= ~flag;
    ny_dbg_enabled &= ~flag;
  }
}

bool ny_dbg_is_enabled(ny_dbg_flag_t flag) {
  return (ny_dbg_enabled & flag) != 0;
}

static void dbg_print_prefix(ny_dbg_flag_t flag) {
  char timebuf[32];
  time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tm);

  const char *color = dbg_flag_color(flag);

  /*
   * Compact horizontal format: [TIME] CATEGORY message
   * ASCII glyph mode collapses icons to "" (the category name carries
   * the meaning), keeping the format clean on plain terminals.
   */
  const char *icon = ny_ui_ascii() ? "" : dbg_flag_icon(flag);
  if (dbg_color_enabled) {
    fprintf(stderr, "\x1b[90m[%s]\x1b[0m %s%s%s%-12s\x1b[0m ", timebuf, icon,
            icon[0] ? " " : "", color, dbg_flag_name(flag));
  } else {
    fprintf(stderr, "[%s] %s%s%-12s ", timebuf, icon, icon[0] ? " " : "",
            dbg_flag_name(flag));
  }
}

static void dbg_flush_heuristic_repeat(void) {
  for (size_t i = 0; i < dbg_heuristic_seen_count; ++i) {
    dbg_heuristic_seen_t *seen = &dbg_heuristic_seen[i];
    if (!seen->repeats)
      continue;
    if (dbg_verbosity < 6 && ny_dbg_msg_names_module_fn(seen->detail))
      continue;
    if (dbg_json_enabled) {
      char detail[896];
      snprintf(detail, sizeof(detail), "%s (repeated=%u)", seen->detail,
               seen->repeats);
      dbg_json_emit(NY_DBG_HEURISTIC, seen->algorithm, detail);
    } else {
      dbg_print_prefix(NY_DBG_HEURISTIC);
      /*
       * ×1 restates the whole detail for a single duplicate; collapse
       * it to a bare marker and truncate long echoed texts.
       */
      if (seen->repeats == 1) {
        fprintf(stderr, "%-12s ×1 (same)\n", seen->algorithm);
      } else {
        char brief[72];
        snprintf(brief, sizeof(brief), "%s", seen->detail);
        size_t bl = strlen(brief);
        if (bl >= sizeof(brief) - 1 && strcmp(brief + (bl - 3), "...") != 0)
          snprintf(brief + (sizeof(brief) - 4), 4, "...");
        fprintf(stderr, "%-12s ×%u: %s\n", seen->algorithm, seen->repeats,
                brief);
      }
    }
  }
  memset(dbg_heuristic_seen, 0, sizeof(dbg_heuristic_seen));
  dbg_heuristic_seen_count = 0;
}

void ny_dbg_enter_context(const char *context) {
  if (!ny_dbg_enabled)
    return;
  /*
   * Filtering controls which records are printed, not whether the context
   * exists.  Skipping a filtered push makes child records inherit the
   * caller's context and can accidentally let them through an outer
   * func: filter.
   */
  bool show_context = dbg_filter_allows(NY_DBG_PASS, context, NULL);
  dbg_flush_heuristic_repeat();
  if (dbg_indent_level < DBG_CONTEXT_MAX)
    dbg_context_stack[dbg_indent_level] = context;
  dbg_indent_level++;
  if (!show_context)
    return;
  if (dbg_json_enabled) {
    dbg_json_emit(NY_DBG_PASS, "context_enter", context);
    return;
  }
  if (!ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    return;
  if (ny_dbg_suppressed_by_scope())
    return;
  fprintf(stderr, "%*s%s%s%s %s\n", dbg_indent_level * 2, "",
          dbg_flag_color(NY_DBG_PASS), ny_ui_glyph("▶", ">"), dbg_reset_color(),
          context ? context : "(unnamed)");
}

static void dbg_flush_ir_unchanged_strikes(void) {
  if (dbg_ir_unchanged_strikes <= 0)
    return;
  dbg_print_prefix(NY_DBG_IR_FULL);
  fprintf(stderr, "%s%d pass dump%s unchanged since last print%s\n",
          dbg_flag_color(NY_DBG_IR_FULL), dbg_ir_unchanged_strikes,
          dbg_ir_unchanged_strikes == 1 ? "" : "s", dbg_reset_color());
  dbg_ir_unchanged_strikes = 0;
}

void ny_dbg_exit_context(const char *context) {
  dbg_flush_heuristic_repeat();
  dbg_flush_ir_unchanged_strikes();
  if (!ny_dbg_enabled)
    return;
  bool show_context = dbg_filter_allows(NY_DBG_PASS, context, NULL);
  if (dbg_indent_level > 0) {
    dbg_indent_level--;
    if (dbg_indent_level < DBG_CONTEXT_MAX)
      dbg_context_stack[dbg_indent_level] = NULL;
  }
  if (!show_context)
    return;
  if (dbg_json_enabled) {
    dbg_json_emit(NY_DBG_PASS, "context_exit", context);
    return;
  }
  if (!ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    return;
  if (ny_dbg_suppressed_by_scope())
    return;
  fprintf(stderr, "%*s%s%s%s %s\n", dbg_indent_level * 2, "",
          dbg_flag_color(NY_DBG_PASS), ny_ui_glyph("◀", "<"), dbg_reset_color(),
          context ? context : "(unnamed)");
}

void ny_dbg_print(ny_dbg_flag_t flag, const char *file, int line,
                  const char *fn, const char *fmt, ...) {
  if (!(ny_dbg_enabled & flag))
    return;
  va_list ap;
  va_start(ap, fmt);
  char message[4096];
  vsnprintf(message, sizeof(message), fmt, ap);
  va_end(ap);
  ny_dbg_ring_capture(dbg_flag_name(flag), message);
  if (flag == NY_DBG_PASS && dbg_scope_stdlib &&
      (strncmp(message, "PHASE ", 6) == 0 ||
       strncmp(message, "ROUTE ", 6) == 0 ||
       strncmp(message, "PIPELINE ", 9) == 0))
    dbg_scope_stdlib = 0;
  if (ny_dbg_suppressed_by_scope())
    return;
  const char *pass = NULL;
  if (strncmp(message, "pass='", 6) == 0) {
    pass = message + 6;
    const char *end = strchr(pass, '\'');
    if (end) {
      static char pass_name[128];
      size_t len = (size_t)(end - pass);
      if (len >= sizeof(pass_name))
        len = sizeof(pass_name) - 1;
      memcpy(pass_name, pass, len);
      pass_name[len] = '\0';
      pass = pass_name;
    }
  }
  if (!dbg_filter_allows(flag, message, pass))
    return;
  if (dbg_delta_only && flag == NY_DBG_PASS &&
      strncmp(message, "pass=", 5) == 0 && !dbg_pass_has_delta(message))
    return;
  if (dbg_json_enabled) {
    const char *end = strpbrk(message, " \t");
    char event[96];
    size_t n = end ? (size_t)(end - message) : strlen(message);
    if (n >= sizeof(event))
      n = sizeof(event) - 1;
    memcpy(event, message, n);
    event[n] = '\0';
    dbg_json_emit(flag, event, end ? end + 1 : "");
    return;
  }
  dbg_print_prefix(flag);
  const char *event_color = dbg_event_color(message);
  if (event_color) {
    size_t word_len = strcspn(message, " \t");
    fprintf(stderr, "%s%.*s%s%s", event_color, (int)word_len, message,
            dbg_reset_color(), message + word_len);
  } else {
    fputs(message, stderr);
  }
  if (dbg_show_location && file && fn) {
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;
    fprintf(stderr, "  %s%s:%d %s%s()\n", dbg_reset_color(), base, line,
            dbg_flag_color(flag), fn);
  } else {
    fputc('\n', stderr);
  }
}

void ny_dbg_ir_diff(ny_dbg_flag_t flag, const void *func, const char *tag,
                    const void *before, const void *after) {
  if (!(ny_dbg_enabled & flag))
    return;
  if (ny_dbg_suppressed_by_scope())
    return;
  (void)func;
  const nyr_func_t *b = (const nyr_func_t *)before;
  const nyr_func_t *a = (const nyr_func_t *)after;
  if (!a)
    return;
  if (!dbg_filter_allows(flag, tag, tag))
    return;
  if (dbg_delta_only && nyr_diff_is_empty(b, a))
    return;
  /*
   * The pass event already reports unchanged IR; reserve diffs for edits.
   */
  if (nyr_diff_is_empty(b, a))
    return;
  if (dbg_json_enabled) {
    FILE *capture = tmpfile();
    if (capture) {
      nyr_dump_diff(capture, b, a, tag);
      fflush(capture);
      dbg_json_emit_file(flag, tag ? tag : "ir_diff", capture);
      fclose(capture);
    }
    return;
  }
  dbg_print_prefix(flag);
  const char *color = dbg_flag_color(flag);
  const char *reset = dbg_reset_color();
  fprintf(stderr, "%s%s%s  ▸ ", color, tag, reset);
  /*
   * Use the existing IR diff engine
   */
  nyr_dump_diff(stderr, b, a, tag);
}

void ny_dbg_inst_rewrite(const void *before, const void *after,
                         const char *reason) {
  if (!dbg_filter_allows(NY_DBG_INST, NULL, NULL))
    return;
  if (!(ny_dbg_enabled & NY_DBG_INST))
    return;
  (void)before;
  (void)after;
  if (dbg_json_enabled) {
    dbg_json_emit(NY_DBG_INST, "rewrite", reason);
    return;
  }
  dbg_print_prefix(NY_DBG_INST);
  const char *color = dbg_flag_color(NY_DBG_INST);
  const char *reset = dbg_reset_color();
  fprintf(stderr, "%s%s%s\n", color, reason, reset);
}

void ny_dbg_value_lifetime(int value, const char *event, const char *detail) {
  if (!dbg_filter_allows(NY_DBG_VALUE, NULL, NULL))
    return;
  if (!(ny_dbg_enabled & NY_DBG_VALUE))
    return;
  if (dbg_json_enabled) {
    dbg_json_printf(NY_DBG_VALUE, event, "v%d %s", value, detail ? detail : "");
    return;
  }
  dbg_print_prefix(NY_DBG_VALUE);
  const char *color = dbg_flag_color(NY_DBG_VALUE);
  const char *reset = dbg_reset_color();
  fprintf(stderr, "%sv%-4d%s %s  %s\n", color, value, reset, event, detail);
}

/*
 * Per-function lowering flow: instruction lines accumulate into one wide
 * buffer and flush as a single wrapped line at the next function boundary,
 * turning ~66 lines per function into one (width over height at -vvvv+).
 */
#define NY_LOWER_FLOW_MAX 24576
static char dbg_lower_flow[NY_LOWER_FLOW_MAX];
static size_t dbg_lower_flow_len;
static char dbg_lower_fn[128];
static size_t dbg_lower_inst_count;

static void dbg_lower_flow_flush(void);

/*
 * Module prefixes seen on armed function names (e.g. "str", "core"):
 * heuristic/ABI lines reference functions mid-message, so the tier gate
 * matches these prefixes instead of exact position.
 */
#define DBG_MODULE_PREFIX_MAX 48
#define DBG_MODULE_PREFIX_CAP 64
static char dbg_module_prefixes[DBG_MODULE_PREFIX_CAP][DBG_MODULE_PREFIX_MAX];
static size_t dbg_module_prefix_count;

static void dbg_module_prefix_add(const char *name) {
  if (!name)
    return;
  /*
   * Register every dot-delimited prefix of the qualified name so trace
   * lines match at any spelling: "core.str.find" registers "std",
   * "core", "core.str", and the full name.
   */
  const char *start = name;
  for (const char *scan = name;; ++scan) {
    if (*scan == '.' || *scan == '\0') {
      size_t len = (size_t)(scan - start);
      if (len > 0 && len < DBG_MODULE_PREFIX_MAX) {
        char prefix[DBG_MODULE_PREFIX_MAX];
        memcpy(prefix, start, len);
        prefix[len] = '\0';
        bool known = false;
        for (size_t i = 0; i < dbg_module_prefix_count; ++i)
          if (strcmp(dbg_module_prefixes[i], prefix) == 0) {
            known = true;
            break;
          }
        if (!known && dbg_module_prefix_count < DBG_MODULE_PREFIX_CAP)
          snprintf(dbg_module_prefixes[dbg_module_prefix_count++],
                   DBG_MODULE_PREFIX_MAX, "%s", prefix);
      }
      if (*scan == '\0')
        break;
      start = scan + 1;
    }
  }
}

/*
 * Module leaves seeded at stdlib load ("str", "dict", "iter", ...).  A
 * message token starting with "<leaf>." names a module function and
 * defers to -vvvvvv.
 */
static char dbg_module_leaves[DBG_MODULE_PREFIX_CAP][DBG_MODULE_PREFIX_MAX];
static size_t dbg_module_leaf_count;

void ny_dbg_register_module_prefix(const char *module_name) {
  if (!module_name)
    return;
  const char *leaf = strrchr(module_name, '.');
  leaf = leaf ? leaf + 1 : module_name;
  if (!*leaf)
    return;
  for (size_t i = 0; i < dbg_module_leaf_count; ++i)
    if (strcmp(dbg_module_leaves[i], leaf) == 0)
      return;
  if (dbg_module_leaf_count < DBG_MODULE_PREFIX_CAP)
    snprintf(dbg_module_leaves[dbg_module_leaf_count++], DBG_MODULE_PREFIX_MAX,
             "%s", leaf);
}

/*
 * True when any whitespace/bracket-delimited token starts with
 * "<module-leaf>." (e.g. "str.find", "dict.dict_clone") — a reference to
 * module code, which defers to -vvvvvv.  "string"/"str/str" don't match.
 */
int ny_dbg_msg_names_module_fn(const char *msg) {
  if (!msg || dbg_module_leaf_count == 0)
    return 0;
  const char *p = msg;
  while (*p) {
    while (*p == ' ' || *p == '[' || *p == ']' || *p == '(' || *p == ')' ||
           *p == '=' || *p == ':' || *p == ',')
      p++;
    if (!*p)
      break;
    const char *start = p;
    while (*p && *p != ' ' && *p != ']' && *p != ',' && *p != ')')
      p++;
    /*
     * Every outer-loop iteration must consume input, even if a future
     * token delimiter is added to the scanner without being added to the
     * leading-skip set above.  In particular, a trace ending in `)` used
     * to leave p unchanged forever while classifying `(no overload)`.
     */
    if (p == start) {
      ++p;
      continue;
    }
    for (size_t i = 0; i < dbg_module_leaf_count; ++i) {
      size_t llen = strlen(dbg_module_leaves[i]);
      size_t tlen = (size_t)(p - start);
      if (tlen > llen && strncmp(start, dbg_module_leaves[i], llen) == 0 &&
          start[llen] == '.')
        return 1;
    }
  }
  return 0;
}

void ny_dbg_lower_fn(const char *name) {
  dbg_module_prefix_add(name);
  if (name)
    ny_dbg_register_module_prefix(name);
  dbg_lower_flow_flush();
  snprintf(dbg_lower_fn, sizeof(dbg_lower_fn), "%s",
           name && *name ? name : "<fn>");
}

static void dbg_lower_flow_flush(void) {
  if (!dbg_lower_flow_len)
    return;
  /*
   * Module-qualified functions (stdlib and user modules) defer their
   * per-instruction detail to -vvvvvv; the user's own top-level code
   * always reports. Width-preserving: the packed flow keeps every
   * instruction, the tier decides who sees it.
   */
  if (dbg_verbosity < 6 && strchr(dbg_lower_fn, '.')) {
    dbg_lower_flow_len = 0;
    dbg_lower_inst_count = 0;
    dbg_lower_fn[0] = '\0';
    return;
  }
  dbg_print_prefix(NY_DBG_LOWER);
  const char *color = dbg_flag_color(NY_DBG_LOWER);
  const char *reset = dbg_reset_color();
  fprintf(stderr, "%s%s%s  %zu insts: ", color,
          dbg_lower_fn[0] ? dbg_lower_fn : "<fn>", reset, dbg_lower_inst_count);
  size_t off = 0;
  while (off < dbg_lower_flow_len) {
    if (dbg_lower_flow[off] == '\n') {
      fputc('\n', stderr);
      fputs("  ", stderr);
      off++;
      continue;
    }
    fputc(dbg_lower_flow[off], stderr);
    off++;
  }
  fputc('\n', stderr);
  dbg_lower_flow_len = 0;
  dbg_lower_inst_count = 0;
  dbg_lower_fn[0] = '\0';
}

void ny_dbg_lower_step(const char *stage, const void *input, const void *output,
                       const char *detail) {
  if (!dbg_filter_allows(NY_DBG_LOWER, NULL, NULL))
    return;
  if (!(ny_dbg_enabled & NY_DBG_LOWER))
    return;
  (void)input;
  (void)output;
  if (dbg_json_enabled) {
    dbg_json_emit(NY_DBG_LOWER, stage, detail);
    return;
  }
  /*
   * A new function's "building <name>" marker flushes the previous
   * function's packed flow and opens a fresh one.
   */
  if (stage && strncmp(stage, "building ", 9) == 0) {
    dbg_lower_flow_flush();
    snprintf(dbg_lower_fn, sizeof(dbg_lower_fn), "%s", stage + 9);
    return;
  }
  dbg_lower_flow_flush();
  if (ny_dbg_suppressed_by_scope())
    return;
  dbg_print_prefix(NY_DBG_LOWER);
  const char *color = dbg_flag_color(NY_DBG_LOWER);
  const char *reset = dbg_reset_color();
  fprintf(stderr, "%s%s%s  %s\n", color, stage, reset, detail);
}

void ny_dbg_lower_instruction(size_t index, const char *op, int dst, int a,
                              int b, int c, int64_t imm, const char *symbol) {
  if (!dbg_filter_allows(NY_DBG_LOWER, NULL, NULL))
    return;
  /*
   * Value summaries at -vv must not expand into a full instruction dump.
   */
  if (!(ny_dbg_enabled & NY_DBG_LOWER))
    return;
  if (dbg_json_enabled) {
    dbg_json_printf(NY_DBG_LOWER, "instruction",
                    "index=%zu op=%s dst=%d args=%d,%d,%d imm=%lld symbol=%s",
                    index, op ? op : "?", dst, a, b, c, (long long)imm,
                    symbol ? symbol : "");
    return;
  }
  /*
   * Append one compact token to the per-function flow (flushed as a
   * single wrapped line at the function boundary).
   */
  char token[192];
  size_t used = 0;
  used += (size_t)snprintf(token + used, sizeof(token) - used, "i%zu ", index);
  used += (size_t)snprintf(token + used, sizeof(token) - used, "%s ",
                           op ? op : "<unknown>");
  if (symbol && *symbol)
    used +=
        (size_t)snprintf(token + used, sizeof(token) - used, "@%s ", symbol);
  else if (op && strncmp(op, "const.", 6) == 0)
    used += (size_t)snprintf(token + used, sizeof(token) - used, "#%lld ",
                             (long long)imm);
  else if (op &&
           (strcmp(op, "load.local") == 0 || strcmp(op, "store.local") == 0))
    used += (size_t)snprintf(token + used, sizeof(token) - used, "slot=%lld ",
                             (long long)imm);
  if (dst >= 0)
    used += (size_t)snprintf(token + used, sizeof(token) - used, "v%d", dst);
  int inputs[] = {a, b, c};
  bool first = true;
  for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
    if (inputs[i] < 0)
      continue;
    used += (size_t)snprintf(token + used, sizeof(token) - used, "%sv%d",
                             first ? " [" : ",", inputs[i]);
    first = false;
  }
  if (!first)
    used += (size_t)snprintf(token + used, sizeof(token) - used, "]");
  if (used >= sizeof(token))
    used = sizeof(token) - 1;
  token[used] = '\0';
  if (dbg_lower_flow_len + used + 4 < NY_LOWER_FLOW_MAX) {
    if (dbg_lower_flow_len)
      dbg_lower_flow[dbg_lower_flow_len++] = ' ';
    memcpy(dbg_lower_flow + dbg_lower_flow_len, token, used);
    dbg_lower_flow_len += used;
    dbg_lower_flow[dbg_lower_flow_len] = '\0';
    dbg_lower_inst_count++;
  } else {
    dbg_lower_flow_flush();
  }
}

bool ny_dbg_lower_sequence_duplicate(size_t length, uint64_t fingerprint) {
  bool duplicate = dbg_have_last_lower && dbg_last_lower_length == length &&
                   dbg_last_lower_fingerprint == fingerprint &&
                   !ny_env_truthy(getenv("NY_DEBUG_LOWER_DUPLICATES"));
  dbg_last_lower_length = length;
  dbg_last_lower_fingerprint = fingerprint;
  dbg_have_last_lower = true;
  return duplicate;
}

void ny_dbg_regalloc_batch_begin(void) {
  if (!ny_dbg_is_enabled(NY_DBG_REGALLOC) ||
      !dbg_filter_allows(NY_DBG_REGALLOC, NULL, NULL))
    return;
  dbg_regalloc_count = 0;
}

static const char *dbg_regalloc_class(const char *reason, const char **choice) {
  static const struct {
    const char *name;
    const char *short_name;
  } classes[] = {{"class=GPR ", "G"},
                 {"class=FPR ", "F"},
                 {"class=VEC ", "V"},
                 {"class=", "?"}};
  if (choice)
    *choice = reason ? reason : "";
  if (!reason)
    return "?";
  for (size_t i = 0; i < sizeof(classes) / sizeof(classes[0]); ++i) {
    size_t n = strlen(classes[i].name);
    if (strncmp(reason, classes[i].name, n) == 0) {
      if (choice)
        *choice = reason + n;
      return classes[i].short_name;
    }
  }
  return "?";
}

void ny_dbg_regalloc_batch_end(void) {
  if (!dbg_regalloc_count) {
    free(dbg_regalloc_entries);
    dbg_regalloc_entries = NULL;
    dbg_regalloc_capacity = 0;
    return;
  }
  /*
   * A batch belongs to one function's allocation: stdlib-scoped batches
   * defer their detail to -vvvvvv like every other channel.
   */
  if (ny_dbg_suppressed_by_scope()) {
    free(dbg_regalloc_entries);
    dbg_regalloc_entries = NULL;
    dbg_regalloc_capacity = 0;
    dbg_regalloc_count = 0;
    return;
  }
  for (size_t i = 0; i < dbg_regalloc_count; ++i) {
    const dbg_regalloc_entry_t *entry = &dbg_regalloc_entries[i];
    if (dbg_json_enabled) {
      dbg_json_printf(NY_DBG_REGALLOC, "allocation_summary",
                      "vreg=%d preg=%d reason=%s segments=%u", entry->vreg,
                      entry->preg, entry->reason, entry->count);
      continue;
    }
  }
  if (!dbg_json_enabled) {
    dbg_print_prefix(NY_DBG_REGALLOC);
    fprintf(stderr,
            "assignments=%zu (G=GPR, F=FPR; !reason only for non-default "
            "choices; ×N=segments)\n",
            dbg_regalloc_count);
    for (size_t base = 0; base < dbg_regalloc_count; base += 6) {
      dbg_print_prefix(NY_DBG_REGALLOC);
      fputs("  ", stderr);
      size_t end = base + 6;
      if (end > dbg_regalloc_count)
        end = dbg_regalloc_count;
      for (size_t i = base; i < end; ++i) {
        const dbg_regalloc_entry_t *entry = &dbg_regalloc_entries[i];
        const char *choice = NULL;
        const char *reg_class = dbg_regalloc_class(entry->reason, &choice);
        if (i > base)
          fputs("  ", stderr);
        fprintf(stderr, "%s:", reg_class);
        if (entry->preg >= 0)
          fprintf(stderr, "v%d→r%d", entry->vreg, entry->preg);
        else
          fprintf(stderr, "v%d→SPILL%d", entry->vreg, -entry->preg);
        if (strcmp(choice, "lowest-free") != 0)
          fprintf(stderr, "!%s", choice);
        if (entry->count > 1)
          fprintf(stderr, "×%u", entry->count);
      }
      fputc('\n', stderr);
    }
  }
  free(dbg_regalloc_entries);
  dbg_regalloc_entries = NULL;
  dbg_regalloc_count = 0;
  dbg_regalloc_capacity = 0;
}

void ny_dbg_regalloc_decision(int vreg, int preg, const char *reason) {
  if (!dbg_filter_allows(NY_DBG_REGALLOC, NULL, NULL))
    return;
  if (!(ny_dbg_enabled & NY_DBG_REGALLOC))
    return;
  const char *why = reason ? reason : "";
  for (size_t i = 0; i < dbg_regalloc_count; ++i) {
    dbg_regalloc_entry_t *entry = &dbg_regalloc_entries[i];
    if (entry->vreg == vreg && entry->preg == preg &&
        strcmp(entry->reason, why) == 0) {
      if (entry->count < UINT_MAX)
        ++entry->count;
      return;
    }
  }
  if (dbg_regalloc_count == dbg_regalloc_capacity) {
    size_t capacity = dbg_regalloc_capacity ? dbg_regalloc_capacity * 2 : 32;
    dbg_regalloc_entry_t *entries =
        realloc(dbg_regalloc_entries, capacity * sizeof(*entries));
    if (!entries) {
      /*
       * Debug output must never make allocation fail.
       */
      if (dbg_json_enabled)
        dbg_json_printf(NY_DBG_REGALLOC, "allocation",
                        "vreg=%d preg=%d reason=%s", vreg, preg, why);
      else {
        dbg_print_prefix(NY_DBG_REGALLOC);
        if (ny_dbg_suppressed_by_scope())
          return;
        fprintf(stderr, "v%-4d → %s%d  %s\n", vreg, preg >= 0 ? "r" : "SPILL",
                preg >= 0 ? preg : -preg, why);
      }
      return;
    }
    dbg_regalloc_entries = entries;
    dbg_regalloc_capacity = capacity;
  }
  dbg_regalloc_entry_t *entry = &dbg_regalloc_entries[dbg_regalloc_count++];
  entry->vreg = vreg;
  entry->preg = preg;
  snprintf(entry->reason, sizeof(entry->reason), "%s", why);
  entry->count = 1;
}

void ny_dbg_regalloc_block(size_t block, size_t peak_live, size_t live_in,
                           size_t live_out, size_t spills, size_t reloads) {
  if (!dbg_filter_allows(NY_DBG_REGALLOC, NULL, NULL))
    return;
  if (!(ny_dbg_enabled & NY_DBG_REGALLOC))
    return;
  if (ny_dbg_suppressed_by_scope())
    return;
  if (dbg_json_enabled) {
    dbg_json_printf(NY_DBG_REGALLOC, "block_pressure",
                    "block=%zu peak_live=%zu live_in=%zu live_out=%zu "
                    "home_stores=%zu home_reloads=%zu",
                    block, peak_live, live_in, live_out, spills, reloads);
    return;
  }
  dbg_print_prefix(NY_DBG_REGALLOC);
  fprintf(
      stderr,
      "B%-4zu peak_live=%zu live=%zu→%zu home_stores=%zu home_reloads=%zu\n",
      block, peak_live, live_in, live_out, spills, reloads);
}

void ny_dbg_abi_classify(const char *name, const char *cls,
                         const char *detail) {
  if (!dbg_filter_allows(NY_DBG_ABI, NULL, NULL))
    return;
  if (!(ny_dbg_enabled & NY_DBG_ABI))
    return;
  /*
   * Module-qualified functions defer ABI detail to -vvvvvv.
   */
  if (dbg_verbosity < 6 && name && strchr(name, '.'))
    return;
  if (dbg_json_enabled) {
    char payload[1024];
    snprintf(payload, sizeof(payload), "%s: %s", cls ? cls : "?",
             detail ? detail : "");
    dbg_json_emit(NY_DBG_ABI, name ? name : "classification", payload);
    return;
  }
  dbg_print_prefix(NY_DBG_ABI);
  const char *color = dbg_flag_color(NY_DBG_ABI);
  const char *reset = dbg_reset_color();
  fprintf(stderr, "%s%-20s%s  %s  %s\n", color, name, reset, cls, detail);
}

void ny_dbg_elf_section(const char *name, size_t offset, size_t size,
                        int flags) {
  if (!dbg_filter_allows(NY_DBG_ELF, NULL, NULL))
    return;
  if (!(ny_dbg_enabled & NY_DBG_ELF))
    return;
  if (dbg_json_enabled) {
    char payload[256];
    snprintf(payload, sizeof(payload), "offset=%#zx size=%#zx flags=%#x",
             offset, size, flags);
    dbg_json_emit(NY_DBG_ELF, name ? name : "section", payload);
    return;
  }
  dbg_print_prefix(NY_DBG_ELF);
  const char *color = dbg_flag_color(NY_DBG_ELF);
  const char *reset = dbg_reset_color();
  fprintf(stderr, "%s%-10s%s  off=%#zx sz=%#zx flg=%#x\n", color, name, reset,
          offset, size, flags);
}

void ny_dbg_elf_reloc(size_t offset, const char *symbol, const char *type) {
  if (!dbg_filter_allows(NY_DBG_ELF, NULL, NULL))
    return;
  if (!(ny_dbg_enabled & NY_DBG_ELF))
    return;
  if (dbg_json_enabled) {
    char payload[512];
    snprintf(payload, sizeof(payload), "offset=%#zx type=%s symbol=%s", offset,
             type ? type : "?", symbol ? symbol : "?");
    dbg_json_emit(NY_DBG_ELF, "relocation", payload);
    return;
  }
  dbg_print_prefix(NY_DBG_ELF);
  fprintf(stderr, "reloc off=%#zx type=%s symbol=%s\n", offset,
          type ? type : "?", symbol ? symbol : "?");
}

void ny_dbg_elf_symbol(const char *name, const char *section, size_t value,
                       size_t size) {
  if (!dbg_filter_allows(NY_DBG_ELF, NULL, NULL))
    return;
  if (!(ny_dbg_enabled & NY_DBG_ELF))
    return;
  if (dbg_json_enabled) {
    char payload[512];
    snprintf(payload, sizeof(payload), "section=%s value=%#zx size=%#zx",
             section ? section : "?", value, size);
    dbg_json_emit(NY_DBG_ELF, name ? name : "symbol", payload);
    return;
  }
  dbg_print_prefix(NY_DBG_ELF);
  fprintf(stderr, "symbol %-28s section=%s value=%#zx size=%#zx\n",
          name ? name : "?", section ? section : "?", value, size);
}

void ny_dbg_type_unify(const char *a, const char *b, const char *result) {
  if (!dbg_filter_allows(NY_DBG_TYPE, NULL, NULL))
    return;
  if (!(ny_dbg_enabled & NY_DBG_TYPE))
    return;
  if (dbg_json_enabled) {
    dbg_json_printf(NY_DBG_TYPE, "unify", "%s ~ %s => %s", a ? a : "?",
                    b ? b : "?", result ? result : "?");
    return;
  }
  dbg_print_prefix(NY_DBG_TYPE);
  const char *color = dbg_flag_color(NY_DBG_TYPE);
  const char *reset = dbg_reset_color();
  fprintf(stderr, "%s%s%s ~ %s%s%s → %s%s%s\n", color, a, reset, color, b,
          reset, color, result, reset);
}

void ny_dbg_effect_row(const char *fn, const char *reads, const char *writes,
                       int alloc) {
  if (!dbg_filter_allows(NY_DBG_EFFECT, NULL, NULL))
    return;
  if (!(ny_dbg_enabled & NY_DBG_EFFECT))
    return;
  if (dbg_json_enabled) {
    dbg_json_printf(NY_DBG_EFFECT, "effect_row",
                    "fn=%s reads=%s writes=%s alloc=%d", fn ? fn : "?",
                    reads ? reads : "", writes ? writes : "", alloc);
    return;
  }
  dbg_print_prefix(NY_DBG_EFFECT);
  const char *color = dbg_flag_color(NY_DBG_EFFECT);
  const char *reset = dbg_reset_color();
  fprintf(stderr, "%s%-25s%s  R:{%s} W:{%s} A:%d\n", color, fn, reset, reads,
          writes, alloc);
}

void ny_dbg_verify_fail(const char *pass, const char *err) {
  if (!dbg_filter_allows(NY_DBG_VERIFY, NULL, NULL))
    return;
  if (!(ny_dbg_enabled & NY_DBG_VERIFY))
    return;
  if (dbg_json_enabled) {
    dbg_json_printf(NY_DBG_VERIFY, "verify_fail", "pass=%s error=%s",
                    pass ? pass : "?", err ? err : "?");
    return;
  }
  dbg_print_prefix(NY_DBG_VERIFY);
  const char *color = dbg_flag_color(NY_DBG_VERIFY);
  const char *reset = dbg_reset_color();
  fprintf(stderr, "%s✗ FAIL%s after %s: %s\n", color, reset, pass, err);
}

void ny_dbg_rollback(const char *pass) {
  if (!dbg_filter_allows(NY_DBG_ROLLBACK, NULL, NULL))
    return;
  if (!(ny_dbg_enabled & NY_DBG_ROLLBACK))
    return;
  if (dbg_json_enabled) {
    dbg_json_emit(NY_DBG_ROLLBACK, "rollback", pass);
    return;
  }
  dbg_print_prefix(NY_DBG_ROLLBACK);
  const char *color = dbg_flag_color(NY_DBG_ROLLBACK);
  const char *reset = dbg_reset_color();
  fprintf(stderr, "%s↩ ROLLBACK%s %s\n", color, reset, pass);
}

void ny_dbg_cfg_mutate(const char *mutate, int from_block, int to_block) {
  if (!dbg_filter_allows(NY_DBG_CFG, NULL, NULL))
    return;
  if (!(ny_dbg_enabled & NY_DBG_CFG))
    return;
  if (dbg_json_enabled) {
    dbg_json_printf(NY_DBG_CFG, "cfg_mutation", "%s from=B%d to=B%d",
                    mutate ? mutate : "?", from_block, to_block);
    return;
  }
  dbg_print_prefix(NY_DBG_CFG);
  const char *color = dbg_flag_color(NY_DBG_CFG);
  const char *reset = dbg_reset_color();
  fprintf(stderr, "%s%s%s  B%d → B%d\n", color, mutate, reset, from_block,
          to_block);
}

void ny_dbg_escape_mark(int slot, const char *reason) {
  if (!dbg_filter_allows(NY_DBG_ESCAPE, NULL, NULL))
    return;
  if (!(ny_dbg_enabled & NY_DBG_ESCAPE))
    return;
  if (dbg_json_enabled) {
    dbg_json_printf(NY_DBG_ESCAPE, "escape", "slot=%d reason=%s", slot,
                    reason ? reason : "");
    return;
  }
  dbg_print_prefix(NY_DBG_ESCAPE);
  const char *color = dbg_flag_color(NY_DBG_ESCAPE);
  const char *reset = dbg_reset_color();
  fprintf(stderr, "%ssl%03d%s  %s\n", color, slot, reset, reason);
}

void ny_dbg_sroa_promote(int slot, const char *detail) {
  if (!dbg_filter_allows(NY_DBG_ESCAPE, NULL, NULL))
    return;
  if (!(ny_dbg_enabled & NY_DBG_ESCAPE))
    return;
  if (dbg_json_enabled) {
    dbg_json_printf(NY_DBG_ESCAPE, "sroa_promote", "slot=%d %s", slot,
                    detail ? detail : "");
    return;
  }
  dbg_print_prefix(NY_DBG_ESCAPE);
  const char *color = dbg_flag_color(NY_DBG_ESCAPE);
  const char *reset = dbg_reset_color();
  fprintf(stderr, "%sPROMOTE sl%03d%s  %s\n", color, slot, reset, detail);
}

void ny_dbg_sccp_overdefined(int value, const char *reason) {
  if (!dbg_filter_allows(NY_DBG_VALUE, NULL, NULL))
    return;
  if (!(ny_dbg_enabled & NY_DBG_VALUE))
    return;
  if (dbg_json_enabled) {
    dbg_json_printf(NY_DBG_VALUE, "overdefined", "value=%d reason=%s", value,
                    reason ? reason : "");
    return;
  }
  dbg_print_prefix(NY_DBG_VALUE);
  const char *color = dbg_flag_color(NY_DBG_VALUE);
  const char *reset = dbg_reset_color();
  fprintf(stderr, "%sv%-4d OVERDEFINED%s  %s\n", color, value, reset, reason);
}

void ny_dbg_licm_hoist(int inst_idx, const char *detail) {
  if (!dbg_filter_allows(NY_DBG_INST, NULL, NULL))
    return;
  if (!(ny_dbg_enabled & NY_DBG_INST))
    return;
  if (dbg_json_enabled) {
    dbg_json_printf(NY_DBG_INST, "licm_hoist", "inst=%d %s", inst_idx,
                    detail ? detail : "");
    return;
  }
  dbg_print_prefix(NY_DBG_INST);
  const char *color = dbg_flag_color(NY_DBG_INST);
  const char *reset = dbg_reset_color();
  fprintf(stderr, "%sHOIST i%04d%s  %s\n", color, inst_idx, reset, detail);
}

void ny_dbg_value_trace(int value, const char *event, const char *detail) {
  if (!dbg_filter_allows(NY_DBG_VALUE_TRACE, NULL, NULL))
    return;
  if (!(ny_dbg_enabled & NY_DBG_VALUE_TRACE))
    return;
  if (dbg_json_enabled) {
    dbg_json_printf(NY_DBG_VALUE_TRACE, event, "value=%d %s", value,
                    detail ? detail : "");
    return;
  }
  dbg_print_prefix(NY_DBG_VALUE_TRACE);
  const char *color = dbg_flag_color(NY_DBG_VALUE_TRACE);
  const char *reset = dbg_reset_color();
  fprintf(stderr, "%sv%-4d %s%s  %s\n", color, value, event, reset, detail);
}

void ny_dbg_lower_detailed(const char *stage, const void *input,
                           const void *output, const char *detail) {
  if (!dbg_filter_allows(NY_DBG_LOWER_DETAILED, NULL, NULL))
    return;
  if (!(ny_dbg_enabled & NY_DBG_LOWER_DETAILED))
    return;
  (void)input;
  (void)output;
  if (dbg_json_enabled) {
    dbg_json_emit(NY_DBG_LOWER_DETAILED, stage, detail);
    return;
  }
  dbg_print_prefix(NY_DBG_LOWER_DETAILED);
  const char *color = dbg_flag_color(NY_DBG_LOWER_DETAILED);
  const char *reset = dbg_reset_color();
  fprintf(stderr, "%s%s%s  %s\n", color, stage, reset, detail);
}

void ny_dbg_type_detail(const char *phase, const char *detail) {
  if (!dbg_filter_allows(NY_DBG_TYPE_DETAIL, NULL, NULL))
    return;
  if (!(ny_dbg_enabled & NY_DBG_TYPE_DETAIL))
    return;
  if (dbg_json_enabled) {
    dbg_json_emit(NY_DBG_TYPE_DETAIL, phase ? phase : "type", detail);
    return;
  }
  dbg_print_prefix(NY_DBG_TYPE_DETAIL);
  const char *color = dbg_flag_color(NY_DBG_TYPE_DETAIL);
  const char *reset = dbg_reset_color();
  fprintf(stderr, "%s%s%s  %s\n", color, phase, reset, detail);
}

void ny_dbg_trait_detail(const char *detail) {
  if (!dbg_filter_allows(NY_DBG_TRAIT, NULL, NULL))
    return;
  if (!(ny_dbg_enabled & NY_DBG_TRAIT))
    return;
  if (dbg_json_enabled) {
    dbg_json_emit(NY_DBG_TRAIT, "resolution", detail);
    return;
  }
  dbg_print_prefix(NY_DBG_TRAIT);
  fprintf(stderr, "%s\n", detail ? detail : "trait resolution");
}

void ny_dbg_opt_missed(const char *pass, const char *pattern,
                       const char *detail) {
  if (!dbg_filter_allows(NY_DBG_OPT_MISS, NULL, NULL))
    return;
  if (!(ny_dbg_enabled & NY_DBG_OPT_MISS))
    return;
  if (dbg_json_enabled) {
    dbg_json_printf(NY_DBG_OPT_MISS, "missed_optimization",
                    "pass=%s pattern=%s %s", pass ? pass : "?",
                    pattern ? pattern : "?", detail ? detail : "");
    return;
  }
  dbg_print_prefix(NY_DBG_OPT_MISS);
  const char *color = "\x1b[33m"; // yellow
  const char *reset = dbg_reset_color();
  fprintf(stderr, "%sMISSED %s%s  %s: %s\n", color, pass, reset, pattern,
          detail);
}

void ny_dbg_mem_layout(const char *name, size_t offset, size_t size, int align,
                       const char *detail) {
  if (!dbg_filter_allows(NY_DBG_MEM, NULL, NULL))
    return;
  if (!(ny_dbg_enabled & NY_DBG_MEM))
    return;
  if (ny_dbg_suppressed_by_scope())
    return;
  if (dbg_json_enabled) {
    dbg_json_printf(
        NY_DBG_MEM, "memory_layout", "name=%s offset=%zu size=%zu align=%d %s",
        name ? name : "?", offset, size, align, detail ? detail : "");
    return;
  }
  dbg_print_prefix(NY_DBG_MEM);
  const char *color = dbg_flag_color(NY_DBG_MEM);
  const char *reset = dbg_reset_color();
  fprintf(stderr, "%s%s%s  off=%#zx sz=%#zx al=%d  %s\n", color, name, reset,
          offset, size, align, detail);
}

void ny_dbg_opt_profile(const char *name, const char *metric, double value) {
  if (!dbg_filter_allows(NY_DBG_PASS, NULL, NULL))
    return;
  if (!(ny_dbg_enabled & NY_DBG_PASS))
    return;
  if (dbg_json_enabled) {
    dbg_json_printf(NY_DBG_PASS, "profile", "name=%s metric=%s value=%.9g",
                    name ? name : "", metric ? metric : "?", value);
    return;
  }
  (void)name;
  dbg_print_prefix(NY_DBG_PASS);
  const char *color = dbg_flag_color(NY_DBG_PASS);
  const char *reset = dbg_reset_color();
  fprintf(stderr, "%s%s%s %s=%.6f\n", color, "PROFILE", reset, metric, value);
}

void ny_dbg_ir_full_dump(const void *func, const char *phase) {
  if (!dbg_filter_allows(NY_DBG_IR_FULL, NULL, NULL))
    return;
  if (!(ny_dbg_enabled & NY_DBG_IR_FULL))
    return;
  if (ny_dbg_suppressed_by_scope())
    return;
  const nyr_func_t *f = (const nyr_func_t *)func;
  if (f) {
    uint64_t fingerprint = nyr_debug_fingerprint(f);
    const char *context =
        dbg_indent_level > 0 && dbg_indent_level <= DBG_CONTEXT_MAX
            ? dbg_context_stack[dbg_indent_level - 1]
            : NULL;
    if (dbg_have_last_ir_full && fingerprint == dbg_last_ir_full_fingerprint &&
        strcmp(context ? context : "", dbg_last_ir_full_context) == 0) {
      if (dbg_json_enabled)
        dbg_json_printf(NY_DBG_IR_FULL, "ir_unchanged",
                        "%s fingerprint=%016llx dump=omitted",
                        phase ? phase : "IR", (unsigned long long)fingerprint);
      else
        dbg_ir_unchanged_strikes++; /* flushed when the IR actually moves */
      return;
    }
    if (!dbg_json_enabled && dbg_ir_unchanged_strikes > 0) {
      dbg_print_prefix(NY_DBG_IR_FULL);
      fprintf(stderr, "%s%s: %d pass dump%s unchanged since last print%s\n",
              dbg_flag_color(NY_DBG_IR_FULL),
              context && *context ? context : "IR", dbg_ir_unchanged_strikes,
              dbg_ir_unchanged_strikes == 1 ? "" : "s", dbg_reset_color());
      dbg_ir_unchanged_strikes = 0;
    }
    dbg_last_ir_full_fingerprint = fingerprint;
    snprintf(dbg_last_ir_full_context, sizeof(dbg_last_ir_full_context), "%s",
             context ? context : "");
    dbg_have_last_ir_full = true;
  }
  if (dbg_json_enabled) {
    FILE *capture = tmpfile();
    if (capture && f) {
      nyr_dump(capture, f, phase);
      fflush(capture);
      dbg_json_emit_file(NY_DBG_IR_FULL, phase ? phase : "ir_dump", capture);
    }
    if (capture)
      fclose(capture);
    return;
  }
  dbg_print_prefix(NY_DBG_IR_FULL);
  const char *color = dbg_flag_color(NY_DBG_IR_FULL);
  const char *reset = dbg_reset_color();
  fprintf(stderr, "%s%s%s  %s\n", color, phase, reset, "");
  /*
   * Delegate to the existing IR dump function
   */
  if (f) {
    nyr_dump(stderr, f, phase);
  }
}

/*
 * Crash forensics ring: the last N heuristic records (the pass-history and
 * decision trace at -vvvv) are retained in static storage so a fatal signal
 * can flush them without allocating or formatting.  The SIGSEGV/SIGABRT/
 * SIGFPE/SIGBUS/SIGILL handler writes only pre-formatted bytes with write(2)
 * and then restores the default disposition so the process still dies with
 * the expected signal code.
 */
#define NY_CRASH_RING_CAP 96
typedef struct {
  char text[176];
  unsigned repeats; /* consecutive duplicates folded into this slot */
} ny_crash_ent_t;
static ny_crash_ent_t g_ny_crash_ring[NY_CRASH_RING_CAP];
static volatile unsigned g_ny_crash_ring_at;

static size_t ny_crash_write_all(int fd, const char *buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t w = write(fd, buf + off, len - off);
    if (w <= 0) {
      if (w < 0 && errno == EINTR)
        continue;
      break;
    }
    off += (size_t)w;
  }
  return off;
}

static void ny_crash_write_uint(int fd, unsigned v) {
  char buf[12];
  int n = 0;
  do {
    buf[sizeof(buf) - 1 - n] = (char)('0' + (v % 10u));
    v /= 10u;
    n++;
  } while (v && n < (int)sizeof(buf));
  ny_crash_write_all(fd, buf + sizeof(buf) - 1 - (size_t)n, (size_t)n);
}

static void ny_crash_record(const char *algorithm, const char *detail) {
  if (g_ny_crash_ring_at) {
    ny_crash_ent_t *prev =
        &g_ny_crash_ring[(g_ny_crash_ring_at - 1) % NY_CRASH_RING_CAP];
    if (strncmp(prev->text, detail, sizeof(prev->text)) == 0) {
      prev->repeats++;
      return;
    }
  }
  ny_crash_ent_t *ent =
      &g_ny_crash_ring[g_ny_crash_ring_at % NY_CRASH_RING_CAP];
  ent->repeats = 0;
  size_t at = 0;
  size_t cap = sizeof(ent->text) - 1;
  for (const char *p = algorithm; p && *p && at < cap; ++p)
    ent->text[at++] = *p;
  if (at < cap)
    ent->text[at++] = ' ';
  for (const char *p = detail; p && *p && at < cap; ++p)
    ent->text[at++] = *p;
  ent->text[at] = '\0';
  g_ny_crash_ring_at++;
}

/*
 * Called from main's fatal-signal handler before its own report: writes the
 * recorded pass history to $NYTRIX_CRASH_DIR (or TMPDIR, or /tmp) using only
 * async-signal-safe primitives, then returns so the caller's report and exit
 * code stay unchanged.
 */
void ny_dbg_crash_flush(int sig, const void *fault_addr) {
  char path[256];
  const char *dir = getenv("NYTRIX_CRASH_DIR");
  if (!dir || !dir[0])
    dir = getenv("TMPDIR");
  if (!dir || !dir[0])
    dir = "/tmp";
  snprintf(path, sizeof(path), "%s/nytrix-crash-%ld.log", dir, (long)getpid());
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    return;
  char header[192];
  int n = snprintf(header, sizeof(header),
                   "Nytrix crash log\nsignal=%d address=%p pid=%ld\n"
                   "last compiler decision records (most recent last):\n",
                   sig, fault_addr, (long)getpid());
  if (n > 0)
    ny_crash_write_all(fd, header, (size_t)n);
  unsigned total = g_ny_crash_ring_at;
  unsigned start = total > NY_CRASH_RING_CAP ? total - NY_CRASH_RING_CAP : 0;
  for (unsigned i = start; i < total; ++i) {
    const ny_crash_ent_t *ent = &g_ny_crash_ring[i % NY_CRASH_RING_CAP];
    if (!ent->text[0])
      continue;
    ny_crash_write_all(fd, ent->text, strlen(ent->text));
    if (ent->repeats) {
      static const char suffix[] = " (x";
      ny_crash_write_all(fd, suffix, sizeof(suffix) - 1);
      ny_crash_write_uint(fd, ent->repeats + 1);
      ny_crash_write_all(fd, ")", 1);
    }
    ny_crash_write_all(fd, "\n", 1);
  }
  close(fd);
}

void ny_dbg_heuristic_event(const char *algorithm, const char *fmt, ...) {
  if (!dbg_filter_allows(NY_DBG_HEURISTIC, NULL, NULL))
    return;
  if (!(ny_dbg_enabled & NY_DBG_HEURISTIC) && !dbg_json_enabled)
    return;
  if (!dbg_heuristic_flush_registered) {
    atexit(dbg_flush_heuristic_repeat);
    dbg_heuristic_flush_registered = true;
  }
  char detail[2048];
  va_list ap;
  va_start(ap, fmt);
  if (fmt)
    vsnprintf(detail, sizeof(detail), fmt, ap);
  else
    detail[0] = '\0';
  va_end(ap);
  if (ny_dbg_suppressed_by_scope())
    return;
  /*
   * -vvvv is the decision-trace tier: retain function-scoped records here
   * so heuristics can be audited at the same point as their pass output.
   * Deduplication and the normal debug filters keep repeated records
   * manageable without hiding module-qualified compiler decisions.
   */
  const char *name = algorithm && *algorithm ? algorithm : "compiler";
  for (size_t i = 0; i < dbg_heuristic_seen_count; ++i) {
    dbg_heuristic_seen_t *seen = &dbg_heuristic_seen[i];
    if (strcmp(name, seen->algorithm) == 0 &&
        strcmp(detail, seen->detail) == 0) {
      ++seen->repeats;
      ny_crash_record(name, detail);
      return;
    }
  }
  if (dbg_heuristic_seen_count == DBG_HEURISTIC_DEDUP_CAP)
    dbg_flush_heuristic_repeat();
  dbg_heuristic_seen_t *seen = &dbg_heuristic_seen[dbg_heuristic_seen_count++];
  snprintf(seen->algorithm, sizeof(seen->algorithm), "%s", name);
  snprintf(seen->detail, sizeof(seen->detail), "%s", detail);
  ny_crash_record(name, detail);
  if (dbg_json_enabled) {
    dbg_json_emit(NY_DBG_HEURISTIC, name, detail);
    return;
  }
  dbg_print_prefix(NY_DBG_HEURISTIC);
  const char *color = dbg_flag_color(NY_DBG_HEURISTIC);
  const char *reset = dbg_reset_color();
  fprintf(stderr, "%s%-12s%s ", color, name, reset);
  int context_index = dbg_indent_level - 1;
  if (context_index >= 0 && context_index < DBG_CONTEXT_MAX &&
      dbg_context_stack[context_index] && *dbg_context_stack[context_index])
    fprintf(stderr, "{%s} ", dbg_context_stack[context_index]);
  fputs(detail, stderr);
  fputc('\n', stderr);
}
