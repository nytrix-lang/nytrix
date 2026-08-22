/*
 * Pure i64 arithmetic evaluator for the thin ny launcher.
 * No LLVM/Z3/stdlib — process can finish in sub-millisecond range.
 */
#include "cmd/ny/pureexpr.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const char *g_p;

static void skip_ws(void) {
  while (g_p && *g_p && isspace((unsigned char)*g_p))
    g_p++;
}

static bool parse_expr(int64_t *out);
static bool parse_bor(int64_t *out);
static bool parse_bxor(int64_t *out);
static bool parse_band(int64_t *out);
static bool parse_shift(int64_t *out);
static bool parse_add(int64_t *out);
static bool parse_mul(int64_t *out);
static bool parse_unary(int64_t *out);
static bool parse_primary(int64_t *out);

static bool parse_primary(int64_t *out) {
  skip_ws();
  if (!g_p || !*g_p)
    return false;
  if (*g_p == '(') {
    g_p++;
    if (!parse_expr(out))
      return false;
    skip_ws();
    if (*g_p != ')')
      return false;
    g_p++;
    return true;
  }
  if (!isdigit((unsigned char)*g_p))
    return false;
  int64_t v = 0;
  if (g_p[0] == '0' && (g_p[1] == 'x' || g_p[1] == 'X')) {
    g_p += 2;
    int any = 0;
    while (isxdigit((unsigned char)*g_p)) {
      any = 1;
      int d = isdigit((unsigned char)*g_p) ? *g_p - '0'
            : (*g_p | 32) - 'a' + 10;
      v = (v << 4) | (int64_t)d;
      g_p++;
    }
    if (!any)
      return false;
  } else {
    while (isdigit((unsigned char)*g_p)) {
      v = v * 10 + (*g_p - '0');
      g_p++;
    }
  }
  if (g_p[0] == 'i' && g_p[1] == '6' && g_p[2] == '4')
    g_p += 3;
  else if (g_p[0] == 'u' && g_p[1] == '6' && g_p[2] == '4')
    g_p += 3;
  *out = v;
  return true;
}

static bool parse_unary(int64_t *out) {
  skip_ws();
  if (*g_p == '+') {
    g_p++;
    return parse_unary(out);
  }
  if (*g_p == '-') {
    g_p++;
    if (!parse_unary(out))
      return false;
    *out = -*out;
    return true;
  }
  if (*g_p == '~') {
    g_p++;
    if (!parse_unary(out))
      return false;
    *out = ~*out;
    return true;
  }
  return parse_primary(out);
}

static bool parse_mul(int64_t *out) {
  if (!parse_unary(out))
    return false;
  for (;;) {
    skip_ws();
    char op = *g_p;
    if (op != '*' && op != '/' && op != '%')
      break;
    g_p++;
    int64_t r = 0;
    if (!parse_unary(&r))
      return false;
    if (op == '*')
      *out *= r;
    else if (op == '/') {
      if (r == 0)
        return false;
      *out /= r;
    } else {
      if (r == 0)
        return false;
      *out %= r;
    }
  }
  return true;
}

static bool parse_add(int64_t *out) {
  if (!parse_mul(out))
    return false;
  for (;;) {
    skip_ws();
    if (*g_p == '+') {
      g_p++;
      int64_t r = 0;
      if (!parse_mul(&r))
        return false;
      *out += r;
    } else if (*g_p == '-') {
      g_p++;
      int64_t r = 0;
      if (!parse_mul(&r))
        return false;
      *out -= r;
    } else
      break;
  }
  return true;
}

static bool parse_shift(int64_t *out) {
  if (!parse_add(out))
    return false;
  for (;;) {
    skip_ws();
    if (g_p[0] == '<' && g_p[1] == '<') {
      g_p += 2;
      int64_t r = 0;
      if (!parse_add(&r))
        return false;
      *out <<= (r & 63);
    } else if (g_p[0] == '>' && g_p[1] == '>') {
      g_p += 2;
      int64_t r = 0;
      if (!parse_add(&r))
        return false;
      *out >>= (r & 63);
    } else
      break;
  }
  return true;
}

static bool parse_band(int64_t *out) {
  if (!parse_shift(out))
    return false;
  for (;;) {
    skip_ws();
    if (*g_p == '&' && g_p[1] != '&') {
      g_p++;
      int64_t r = 0;
      if (!parse_shift(&r))
        return false;
      *out &= r;
    } else
      break;
  }
  return true;
}

static bool parse_bxor(int64_t *out) {
  if (!parse_band(out))
    return false;
  for (;;) {
    skip_ws();
    if (*g_p == '^') {
      g_p++;
      int64_t r = 0;
      if (!parse_band(&r))
        return false;
      *out ^= r;
    } else
      break;
  }
  return true;
}

static bool parse_bor(int64_t *out) {
  if (!parse_bxor(out))
    return false;
  for (;;) {
    skip_ws();
    if (*g_p == '|' && g_p[1] != '|') {
      g_p++;
      int64_t r = 0;
      if (!parse_bxor(&r))
        return false;
      *out |= r;
    } else
      break;
  }
  return true;
}

static bool parse_expr(int64_t *out) { return parse_bor(out); }

bool ny_pure_expr_eval(const char *src, int64_t *out) {
  if (!src || !out)
    return false;
  for (const char *s = src; *s; ++s) {
    unsigned char c = (unsigned char)*s;
    if (isalpha(c) || c == '_') {
      /*
       * Only i64/u64 type suffixes after a digit are allowed.
       */
      if (s == src || !isdigit((unsigned char)s[-1]))
        return false;
      if (!(strncmp(s, "i64", 3) == 0 || strncmp(s, "u64", 3) == 0))
        return false;
      s += 2; /* loop +1 */
      continue;
    }
    if (c == '"' || c == '\'' || c == '`' || c == '{' || c == '}' ||
        c == ';' || c == '=' || c == '!' || c == '?' || c == '[' || c == ']' ||
        c == ',' || c == '@' || c == '#' || c == '$' || c == ':')
      return false;
  }
  g_p = src;
  int64_t v = 0;
  if (!parse_expr(&v))
    return false;
  skip_ws();
  if (*g_p != '\0')
    return false;
  *out = v;
  return true;
}

bool ny_pure_native_c_match(int argc, char **argv, const char **expr_out) {
  if (!argv || argc < 3 || !expr_out)
    return false;
  bool native_only = false;
  const char *expr = NULL;
  for (int i = 1; i < argc; ++i) {
    const char *a = argv[i];
    if (!a)
      return false;
    if (strcmp(a, "--native-only") == 0) {
      native_only = true;
      continue;
    }
    if (strcmp(a, "-c") == 0 || strcmp(a, "--command") == 0) {
      if (i + 1 >= argc)
        return false;
      expr = argv[++i];
      continue;
    }
    if (strncmp(a, "-c", 2) == 0 && a[2] != '\0' && a[2] != '-') {
      expr = a + 2;
      continue;
    }
    if (strncmp(a, "--command=", 10) == 0) {
      expr = a + 10;
      continue;
    }
    if (strcmp(a, "--no-progress") == 0 || strcmp(a, "-no-std") == 0 ||
        strcmp(a, "--no-std") == 0)
      continue;
    if (strncmp(a, "--color=", 8) == 0)
      continue;
    if (strcmp(a, "--color") == 0) {
      if (i + 1 < argc && argv[i + 1][0] != '-')
        i++;
      continue;
    }
    if (strncmp(a, "--native-backend=", 17) == 0)
      continue;
    if (strcmp(a, "--native-backend") == 0 && i + 1 < argc) {
      i++;
      continue;
    }
    if (a[0] == '-' && a[1] == 'O' && a[2] >= '0' && a[2] <= '3' && !a[3])
      continue;
    if (strncmp(a, "--native-tier=", 14) == 0)
      continue;
    return false;
  }
  if (!native_only || !expr || !*expr)
    return false;
  *expr_out = expr;
  return true;
}
