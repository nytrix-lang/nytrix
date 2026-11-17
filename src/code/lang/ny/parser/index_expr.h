/*
 * Value indices of inductive families (`Vec<A, n + 1>`, `where n is k + 1`).
 *
 * An index is a linear term over at most one name: an integer literal, a name,
 * or a name plus or minus a literal. Such terms have one canonical spelling
 * (`n+1`, `n-2`, `n`, `3`; a zero offset disappears and a literal sum folds),
 * so two indices are equal exactly when their spellings are. Anything richer
 * (two names, products) keeps its compact text and is equal only to itself,
 * which fails closed: the checker never assumes an equation it cannot see.
 */
#ifndef NY_PARSER_INDEX_EXPR_H
#define NY_PARSER_INDEX_EXPR_H

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Parses `text` as a linear index. On success `name` (may be empty) and
 * `offset` describe it. */
static inline bool ny_index_parse(const char *text, char *name, size_t cap,
                                  int64_t *offset) {
  if (!text || !name || cap == 0 || !offset)
    return false;
  name[0] = '\0';
  int64_t total = 0;
  bool have_name = false;
  int sign = 1;
  bool expect_term = true;
  const char *p = text;
  while (*p) {
    if (*p == ' ') {
      p++;
      continue;
    }
    if (expect_term) {
      if (*p == '+' || *p == '-') {
        if (*p == '-')
          sign = -sign;
        p++;
        continue;
      }
      if (isdigit((unsigned char)*p)) {
        int64_t v = 0;
        while (isdigit((unsigned char)*p)) {
          if (v > (INT64_MAX - 9) / 10)
            return false;
          v = v * 10 + (*p - '0');
          p++;
        }
        total += sign * v;
      } else if (isalpha((unsigned char)*p) || *p == '_') {
        if (have_name || sign < 0)
          return false;
        size_t n = 0;
        while (isalnum((unsigned char)*p) || *p == '_' || *p == '.') {
          if (n + 1 < cap)
            name[n++] = *p;
          p++;
        }
        name[n] = '\0';
        have_name = true;
      } else {
        return false;
      }
      expect_term = false;
      sign = 1;
      continue;
    }
    if (*p == '+' || *p == '-') {
      sign = *p == '-' ? -1 : 1;
      expect_term = true;
      p++;
      continue;
    }
    return false;
  }
  if (expect_term)
    return false;
  *offset = total;
  return true;
}

/* Canonical spelling of an index into `out`; returns false (and copies the
 * text without spaces) when it is not linear. */
static inline bool ny_index_canon(const char *text, char *out, size_t cap) {
  char name[128];
  int64_t offset = 0;
  if (!out || cap == 0)
    return false;
  if (!ny_index_parse(text, name, sizeof(name), &offset)) {
    size_t n = 0;
    for (const char *p = text; p && *p && n + 1 < cap; ++p)
      if (*p != ' ')
        out[n++] = *p;
    out[n] = '\0';
    return false;
  }
  if (!name[0])
    snprintf(out, cap, "%lld", (long long)offset);
  else if (offset == 0)
    snprintf(out, cap, "%s", name);
  else
    snprintf(out, cap, "%s%c%lld", name, offset > 0 ? '+' : '-',
             (long long)(offset > 0 ? offset : -offset));
  return true;
}

/* Replaces the name of `pattern` (a linear index over one name) so that it
 * equals `actual`: from `k+1` against `5` it solves `k = 4`; against `n+3`
 * it solves `k = n+2`. Writes the solution for the name into `out`. */
static inline bool ny_index_solve(const char *pattern, const char *actual,
                                  char *var, size_t var_cap, char *out,
                                  size_t cap) {
  char pname[128], aname[128];
  int64_t poff = 0, aoff = 0;
  if (!ny_index_parse(pattern, pname, sizeof(pname), &poff) || !pname[0] ||
      !ny_index_parse(actual, aname, sizeof(aname), &aoff))
    return false;
  snprintf(var, var_cap, "%s", pname);
  char raw[192];
  if (aname[0])
    snprintf(raw, sizeof(raw), "%s%+lld", aname, (long long)(aoff - poff));
  else
    snprintf(raw, sizeof(raw), "%lld", (long long)(aoff - poff));
  return ny_index_canon(raw, out, cap);
}

#endif
