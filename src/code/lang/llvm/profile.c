#include "code/lang/code.h"
#include "code/lang/priv.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  uint64_t count;
  double total_ms;
  double self_ms;
} ny_expr_kind_profile_t;

static ny_expr_kind_profile_t g_expr_kind_profile[64];
static double g_expr_kind_child_ms[4096];
static int g_expr_kind_depth;
static int g_expr_kind_profile_enabled = -1;
static bool g_expr_kind_profile_registered;

bool ny_expr_kind_profile_is_enabled(void) {
  if (g_expr_kind_profile_enabled < 0) {
    const char *env = getenv("NYTRIX_PROFILE_CODEGEN_KINDS");
    if (!env || !*env)
      env = getenv("NYTRIX_PROFILE_CODEGEN_EXPR");
    g_expr_kind_profile_enabled = env && *env && strcmp(env, "0") != 0 &&
                                          strcmp(env, "false") != 0 &&
                                          strcmp(env, "off") != 0
                                      ? 1
                                      : 0;
  }
  return g_expr_kind_profile_enabled == 1;
}

static const char *ny_expr_kind_profile_name(int kind) {
  switch (kind) {
  case NY_E_IDENT:
    return "IDENT";
  case NY_E_LITERAL:
    return "LITERAL";
  case NY_E_UNARY:
    return "UNARY";
  case NY_E_BINARY:
    return "BINARY";
  case NY_E_LOGICAL:
    return "LOGICAL";
  case NY_E_TERNARY:
    return "TERNARY";
  case NY_E_CALL:
    return "CALL";
  case NY_E_MEMCALL:
    return "MEMCALL";
  case NY_E_INDEX:
    return "INDEX";
  case NY_E_LAMBDA:
    return "LAMBDA";
  case NY_E_FN:
    return "FN";
  case NY_E_LIST:
    return "LIST";
  case NY_E_TUPLE:
    return "TUPLE";
  case NY_E_DICT:
    return "DICT";
  case NY_E_SET:
    return "SET";
  case NY_E_ASM:
    return "ASM";
  case NY_E_COMPTIME:
    return "COMPTIME";
  case NY_E_FSTRING:
    return "FSTRING";
  case NY_E_INFERRED_MEMBER:
    return "INFERRED_MEMBER";
  case NY_E_EMBED:
    return "EMBED";
  case NY_E_MATCH:
    return "MATCH";
  case NY_E_MEMBER:
    return "MEMBER";
  case NY_E_PTR_TYPE:
    return "PTR_TYPE";
  case NY_E_DEREF:
    return "DEREF";
  case NY_E_SIZEOF:
    return "SIZEOF";
  case NY_E_TRY:
    return "TRY";
  default:
    return "UNKNOWN";
  }
}

static void ny_expr_kind_profile_report(void) {
  if (!ny_expr_kind_profile_is_enabled())
    return;
  for (int i = 0;
       i < (int)(sizeof(g_expr_kind_profile) / sizeof(g_expr_kind_profile[0]));
       ++i) {
    ny_expr_kind_profile_t p = g_expr_kind_profile[i];
    if (p.count == 0)
      continue;
    fprintf(stderr,
            "[codegen-expr-kind] kind=%s count=%" PRIu64
            " total_ms=%.3f self_ms=%.3f avg_us=%.3f\n",
            ny_expr_kind_profile_name(i), p.count, p.total_ms, p.self_ms,
            (p.total_ms * 1000.0) / (double)p.count);
  }
}

static void ny_expr_kind_profile_add(int kind, double total_ms,
                                     double child_ms) {
  if (kind < 0 || kind >= (int)(sizeof(g_expr_kind_profile) /
                                sizeof(g_expr_kind_profile[0])))
    kind =
        (int)(sizeof(g_expr_kind_profile) / sizeof(g_expr_kind_profile[0])) - 1;
  double self_ms = total_ms - child_ms;
  if (self_ms < 0.0)
    self_ms = 0.0;
  g_expr_kind_profile[kind].count++;
  g_expr_kind_profile[kind].total_ms += total_ms;
  g_expr_kind_profile[kind].self_ms += self_ms;
}

int ny_expr_kind_profile_begin(void) {
  if (!ny_expr_kind_profile_is_enabled())
    return -1;
  if (!g_expr_kind_profile_registered) {
    atexit(ny_expr_kind_profile_report);
    g_expr_kind_profile_registered = true;
  }
  int slot = g_expr_kind_depth;
  if (slot >= 0 && slot < (int)(sizeof(g_expr_kind_child_ms) /
                                sizeof(g_expr_kind_child_ms[0])))
    g_expr_kind_child_ms[slot] = 0.0;
  g_expr_kind_depth++;
  return slot;
}

void ny_expr_kind_profile_end(int kind, int slot, double total_ms) {
  if (slot < 0 || !ny_expr_kind_profile_is_enabled())
    return;
  if (g_expr_kind_depth > 0)
    g_expr_kind_depth--;
  double child_ms = 0.0;
  if (slot <
      (int)(sizeof(g_expr_kind_child_ms) / sizeof(g_expr_kind_child_ms[0])))
    child_ms = g_expr_kind_child_ms[slot];
  if (g_expr_kind_depth > 0) {
    int parent = g_expr_kind_depth - 1;
    if (parent <
        (int)(sizeof(g_expr_kind_child_ms) / sizeof(g_expr_kind_child_ms[0])))
      g_expr_kind_child_ms[parent] += total_ms;
  }
  ny_expr_kind_profile_add(kind, total_ms, child_ms);
}
