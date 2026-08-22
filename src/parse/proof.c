/*
 * Proof-parser bridge: extracts proof constraints and refinement
 * annotations from parse nodes for the semantic proof-checking layer.
 */
#include "base/strbuf.h"
#include "parse/json.h"
#include "parse/proof.h"
#include "priv.h"
#include <stdlib.h>
#include <string.h>

extern int64_t rt_free(int64_t ptr);

/*
 * Canonical proof propositions are compiler metadata, never source text.
 *
 * Normalization passes (applied in order):
 *   1. Constant folding for binary ops with two int literal operands +
 *      comparison folding to bool:true/bool:false
 *   2. Boolean literal folding (true&&x→x, false||x→x, x&&x→x, etc.)
 *   3. Arithmetic identity reduction (x+0→x, x*1→x, x*0→0, etc.)
 *   4. Associative flattening for +, *, &, |
 *   5. Multiplication distribution (small literal multiplier, |X|≤16)
 *   6. Commutative sorting for +, *, &, |, ^, &&, || plus comparison normalization
 *   7. Canonical form output
 *
 * All other expression shapes retain their AST structure via JSON fallback.
 * Recursion depth is bounded at PROOF_NORM_MAX_DEPTH.
 */
#define PROOF_NORM_MAX_DEPTH 64

static char *proof_prop_expr_depth(expr_t *e, int depth);

static char *proof_prop_expr(expr_t *e) {
  return proof_prop_expr_depth(e, 0);
}

static bool proof_is_int_literal(const char *s, int64_t *out) {
  if (!s || strncmp(s, "int:", 4) != 0)
    return false;
  if (out)
    *out = strtoll(s + 4, NULL, 10);
  return true;
}

/*
 * Wrap s in a canonical negation form: "unary:!(s)", folding double
 * negation away so !(!x) and !!x normalize to the same token.
 */
static char *proof_negate_form(const char *s) {
  if (s && strncmp(s, "unary:!(", 8) == 0) {
    size_t len = strlen(s);
    if (len > 9 && s[len - 1] == ')')
      return ny_strndup(s + 8, len - 9);
  }
  ny_strbuf_t b;
  ny_strbuf_init(&b);
  ny_strbuf_appendf(&b, "unary:!(%s)", s ? s : "invalid");
  return ny_strbuf_take(&b);
}

/*
 * Build "binary:OP(a,b)", sorting operands lexicographically for
 * commutative ops so De Morgan output matches the direct-write form.
 */
static char *proof_build_sorted_binary(const char *op, char *a, char *b) {
  if (strcmp(op, "&&") == 0 || strcmp(op, "||") == 0 ||
      strcmp(op, "+") == 0 || strcmp(op, "*") == 0 ||
      strcmp(op, "&") == 0 || strcmp(op, "|") == 0 ||
      strcmp(op, "^^") == 0) {
    if (strcmp(a ? a : "", b ? b : "") > 0) {
      char *tmp = a;
      a = b;
      b = tmp;
    }
  }
  ny_strbuf_t buf;
  ny_strbuf_init(&buf);
  ny_strbuf_appendf(&buf, "binary:%s(%s,%s)", op, a ? a : "invalid",
                    b ? b : "invalid");
  return ny_strbuf_take(&buf);
}

static char *proof_prop_expr_depth(expr_t *e, int depth) {
  if (!e || depth > PROOF_NORM_MAX_DEPTH)
    return ny_strdup("invalid");

  /*
   * identifiers and literals (leaf nodes)
   */
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    ny_strbuf_t b;
    ny_strbuf_init(&b);
    ny_strbuf_appendf(&b, "name:%s", e->as.ident.name);
    return ny_strbuf_take(&b);
  }

  if (e->kind == NY_E_LITERAL) {
    ny_strbuf_t b;
    ny_strbuf_init(&b);
    switch (e->as.literal.kind) {
    case NY_LIT_INT:
      ny_strbuf_appendf(&b, "int:%lld", (long long)e->as.literal.as.i);
      break;
    case NY_LIT_FLOAT:
      ny_strbuf_appendf(&b, "float:%.17g", e->as.literal.as.f);
      break;
    case NY_LIT_BOOL:
      ny_strbuf_append(&b, e->as.literal.as.b ? "bool:true" : "bool:false");
      break;
    case NY_LIT_STR:
      ny_strbuf_append(&b, "str:");
      ny_strbuf_json_str(&b, e->as.literal.as.s.data);
      break;
    }
    return ny_strbuf_take(&b);
  }

  /*
   * unary expressions
   */
  if (e->kind == NY_E_UNARY && e->as.unary.op) {
    char *right = proof_prop_expr_depth(e->as.unary.right, depth + 1);

    /*
     * Negation of boolean (!)
     */
    if (strcmp(e->as.unary.op, "!") == 0 && right) {
      /*
       * De Morgan: !(a && b) → (!a || !b),  !(a || b) → (!a && !b)
       */
      if (strncmp(right, "binary:&&(", 10) == 0 ||
          strncmp(right, "binary:||(", 10) == 0) {
        const char *de_morgan_op =
            strncmp(right, "binary:&&(", 10) == 0 ? "||" : "&&";
        size_t rlen = strlen(right);
        if (rlen > 11 && right[rlen - 1] == ')') {
          char *inner = ny_strndup(right + 10, rlen - 11);
          char *comma = strchr(inner, ',');
          if (comma) {
            *comma = '\0';
            char *na = proof_negate_form(inner);
            char *nb = proof_negate_form(comma + 1);
            free(inner);
            free(right);
            char *out = proof_build_sorted_binary(de_morgan_op, na, nb);
            free(na);
            free(nb);
            return out;
          }
          free(inner);
        }
      }
      if (strncmp(right, "unary:!(", 8) == 0) {
        size_t rlen = strlen(right);
        if (rlen > 9 && right[rlen - 1] == ')') {
          char *inner = ny_strndup(right + 8, rlen - 9);
          free(right);
          return inner;
        }
      }
      if (strcmp(right, "bool:true") == 0) {
        free(right);
        return ny_strdup("bool:false");
      }
      if (strcmp(right, "bool:false") == 0) {
        free(right);
        return ny_strdup("bool:true");
      }
    }

    /*
     * Arithmetic negation (-)
     */
    if (strcmp(e->as.unary.op, "-") == 0 && right) {
      if (strncmp(right, "unary:-(", 8) == 0) {
        size_t rlen = strlen(right);
        if (rlen > 9 && right[rlen - 1] == ')') {
          char *inner = ny_strndup(right + 8, rlen - 9);
          free(right);
          return inner;
        }
      }
      int64_t v = 0;
      if (proof_is_int_literal(right, &v) && v != INT64_MIN) {
        free(right);
        ny_strbuf_t b;
        ny_strbuf_init(&b);
        ny_strbuf_appendf(&b, "int:%lld", (long long)-v);
        return ny_strbuf_take(&b);
      }
    }

    ny_strbuf_t b;
    ny_strbuf_init(&b);
    ny_strbuf_appendf(&b, "unary:%s(%s)", e->as.unary.op,
                      right ? right : "invalid");
    free(right);
    return ny_strbuf_take(&b);
  }

  /*
   * binary and logical expressions
   */
  if ((e->kind == NY_E_BINARY || e->kind == NY_E_LOGICAL) &&
      e->as.binary.op) {
    char *left = proof_prop_expr_depth(e->as.binary.left, depth + 1);
    char *right = proof_prop_expr_depth(e->as.binary.right, depth + 1);
    const char *op = e->as.binary.op;

    /*
     * PASS 1: Constant folding (both operands are int literals)
     */
    int64_t lv = 0, rv = 0;
    bool left_int = proof_is_int_literal(left, &lv);
    bool right_int = proof_is_int_literal(right, &rv);

    if (left_int && right_int) {
      int64_t result = 0;
      bool folded = true;
      if (strcmp(op, "+") == 0) {
        if (__builtin_add_overflow(lv, rv, &result))
          folded = false;
      } else if (strcmp(op, "-") == 0) {
        if (__builtin_sub_overflow(lv, rv, &result))
          folded = false;
      } else if (strcmp(op, "*") == 0) {
        if (__builtin_mul_overflow(lv, rv, &result))
          folded = false;
      } else if (strcmp(op, "/") == 0) {
        if (rv == 0 || (lv == INT64_MIN && rv == -1))
          folded = false;
        else
          result = lv / rv;
      } else if (strcmp(op, "%") == 0) {
        if (rv == 0 || (lv == INT64_MIN && rv == -1))
          folded = false;
        else
          result = lv % rv;
      } else if (strcmp(op, "&") == 0) {
        result = lv & rv;
      } else if (strcmp(op, "|") == 0) {
        result = lv | rv;
      } else if (strcmp(op, "^^") == 0) {
        result = lv ^ rv;
      } else if (strcmp(op, "<<") == 0 && rv >= 0 && rv < 64) {
        result = lv << rv;
      } else if (strcmp(op, ">>") == 0 && rv >= 0 && rv < 64) {
        result = lv >> rv;
      } else if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
                 strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
                 strcmp(op, ">") == 0 || strcmp(op, ">=") == 0) {
        /*
         * keep comparison structure for proof-matching; operands already folded
         */
        folded = false;
      } else {
        folded = false;
      }
      if (folded) {
        free(left); free(right);
        ny_strbuf_t b;
        ny_strbuf_init(&b);
        ny_strbuf_appendf(&b, "int:%lld", (long long)result);
        return ny_strbuf_take(&b);
      }
    }

    /*
     * PASS 2: Boolean literal folding (for logical ops)
     */
    if (e->kind == NY_E_LOGICAL) {
      bool lt = left && strcmp(left, "bool:true") == 0;
      bool lf = left && strcmp(left, "bool:false") == 0;
      bool rt = right && strcmp(right, "bool:true") == 0;
      bool rf = right && strcmp(right, "bool:false") == 0;

      if (strcmp(op, "&&") == 0) {
        if (lf || rf) { free(left); free(right); return ny_strdup("bool:false"); }
        if (lt) { free(left); return right; }
        if (rt) { free(right); return left; }
      } else if (strcmp(op, "||") == 0) {
        if (lt || rt) { free(left); free(right); return ny_strdup("bool:true"); }
        if (lf) { free(left); return right; }
        if (rf) { free(right); return left; }
      }
      /*
       * x && x → x,  x || x → x
       */
      if (left && right && strcmp(left, right) == 0) {
        free(right); return left;
      }
      /*
       * x && !x → false,  !x && x → false (symmetric; PASS 6 sorts later)
       */
      if (strcmp(op, "&&") == 0) {
        /*
         * check right side for negation
         */
        if (right && strncmp(right, "unary:!(", 8) == 0) {
          size_t rlen = strlen(right);
          char *inner = (rlen > 9 && right[rlen - 1] == ')')
                            ? ny_strndup(right + 8, rlen - 9) : NULL;
          if (inner && strcmp(left, inner) == 0) {
            free(inner); free(left); free(right);
            return ny_strdup("bool:false");
          }
          free(inner);
        }
        /*
         * check left side for negation (!x && x)
         */
        if (left && strncmp(left, "unary:!(", 8) == 0) {
          size_t llen = strlen(left);
          char *inner = (llen > 9 && left[llen - 1] == ')')
                            ? ny_strndup(left + 8, llen - 9) : NULL;
          if (inner && strcmp(right, inner) == 0) {
            free(inner); free(left); free(right);
            return ny_strdup("bool:false");
          }
          free(inner);
        }
      }
      /*
       * x || !x → true,  !x || x → true (symmetric)
       */
      if (strcmp(op, "||") == 0) {
        if (right && strncmp(right, "unary:!(", 8) == 0) {
          size_t rlen = strlen(right);
          char *inner = (rlen > 9 && right[rlen - 1] == ')')
                            ? ny_strndup(right + 8, rlen - 9) : NULL;
          if (inner && strcmp(left, inner) == 0) {
            free(inner); free(left); free(right);
            return ny_strdup("bool:true");
          }
          free(inner);
        }
        if (left && strncmp(left, "unary:!(", 8) == 0) {
          size_t llen = strlen(left);
          char *inner = (llen > 9 && left[llen - 1] == ')')
                            ? ny_strndup(left + 8, llen - 9) : NULL;
          if (inner && strcmp(right, inner) == 0) {
            free(inner); free(left); free(right);
            return ny_strdup("bool:true");
          }
          free(inner);
        }
      }
    }

    /*
     * PASS 3: Arithmetic identity reduction
     */
    bool lz = left && strcmp(left, "int:0") == 0;
    bool rz = right && strcmp(right, "int:0") == 0;
    bool lo = left && strcmp(left, "int:1") == 0;
    bool ro = right && strcmp(right, "int:1") == 0;
    bool ln1 = left && strcmp(left, "int:-1") == 0;
    bool rn1 = right && strcmp(right, "int:-1") == 0;

    char *id_result = NULL;
    if (strcmp(op, "+") == 0) {
      if (rz)      id_result = ny_strdup(left);
      else if (lz) id_result = ny_strdup(right);
    } else if (strcmp(op, "-") == 0) {
      if (rz) id_result = ny_strdup(left);
      else if (lz) {
        ny_strbuf_t b; ny_strbuf_init(&b);
        ny_strbuf_appendf(&b, "unary:-(%s)", right);
        id_result = ny_strbuf_take(&b);
      }
    } else if (strcmp(op, "*") == 0) {
      if (ro) id_result = ny_strdup(left);
      else if (lo) id_result = ny_strdup(right);
      else if (rz || lz) id_result = ny_strdup("int:0");
    } else if (strcmp(op, "/") == 0) {
      if (ro) id_result = ny_strdup(left);
    } else if (strcmp(op, "%") == 0) {
      if (ro) id_result = ny_strdup("int:0");
    } else if (strcmp(op, "&") == 0) {
      if (ln1) id_result = ny_strdup(right);
      else if (rn1) id_result = ny_strdup(left);
      else if (lz || rz) id_result = ny_strdup("int:0");
    } else if (strcmp(op, "|") == 0) {
      if (rz) id_result = ny_strdup(left);
      else if (lz) id_result = ny_strdup(right);
    } else if (strcmp(op, "^^") == 0) {
      if (rz) id_result = ny_strdup(left);
      else if (lz) id_result = ny_strdup(right);
    }
    if (id_result) { free(left); free(right); return id_result; }

    /*
     * PASS 4: Associative flattening (a+b)+c → a+(b+c)
     */
    if (strcmp(op, "+") == 0 || strcmp(op, "*") == 0 ||
        strcmp(op, "&") == 0 || strcmp(op, "|") == 0) {
      const char *pfx = NULL;
      if (strcmp(op, "+") == 0) pfx = "binary:+(";
      else if (strcmp(op, "*") == 0) pfx = "binary:*(";
      else if (strcmp(op, "&") == 0) pfx = "binary:&(";
      else if (strcmp(op, "|") == 0) pfx = "binary:|(";

      if (pfx && left && strncmp(left, pfx, strlen(pfx)) == 0) {
        size_t plen = strlen(pfx);
        char *inner = ny_strdup(left + plen);
        char *comma = strchr(inner, ',');
        if (comma) {
          *comma = '\0';
          char *il = inner;
          char *ir = comma + 1;
          size_t irl = strlen(ir);
          if (irl > 0 && ir[irl - 1] == ')') ir[irl - 1] = '\0';

          ny_strbuf_t rb; ny_strbuf_init(&rb);
          ny_strbuf_appendf(&rb, "binary:%s(%s,%s)", op, ir, right);
          char *nr = ny_strbuf_take(&rb);

          ny_strbuf_t rr; ny_strbuf_init(&rr);
          ny_strbuf_appendf(&rr, "binary:%s(%s,%s)", op, il, nr);

          free(il); free(nr); free(left); free(right);
          return ny_strbuf_take(&rr);
        }
        free(inner);
      }
    }

    /*
     * PASS 5: Multiplication distribution (limited)
     *   X*(Y+Z) → X*Y + X*Z  when X is int literal with |X| ≤ 16
     */
    if (strcmp(op, "*") == 0 && right &&
        strncmp(right, "binary:+(", 10) == 0) {
      int64_t x = 0;
      if (proof_is_int_literal(left, &x) && x >= -16 && x <= 16) {
        size_t rlen = strlen(right);
        char *inner = ny_strndup(right + 10, rlen - 11);
        char *comma = strchr(inner, ',');
        if (comma) {
          *comma = '\0';
          char *y = inner;
          char *z = comma + 1;
          ny_strbuf_t b; ny_strbuf_init(&b);
          ny_strbuf_appendf(&b, "binary:+(binary:*(%s,%s),binary:*(%s,%s))",
                            left, y, left, z);
          free(inner); free(left); free(right);
          return ny_strbuf_take(&b);
        }
        free(inner);
      }
    }

    /*
     * PASS 6: Commutative sorting and comparison normalization
     */

    /*
     * Equality: sort operands lexicographically
     */
    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0) {
      if (strcmp(left ? left : "", right ? right : "") > 0) {
        char *tmp = left; left = right; right = tmp;
      }
    }
    /*
     * Ordered comparisons: normalize so smaller operand is on the left
     */
    else if (strcmp(op, ">") == 0) {
      op = "<"; char *tmp = left; left = right; right = tmp;
    } else if (strcmp(op, ">=") == 0) {
      op = "<="; char *tmp = left; left = right; right = tmp;
    } else if (strcmp(op, "<") == 0 || strcmp(op, "<=") == 0) {
      /*
       * Already canonical. Sort int-int pairs numerically.
       */
      int64_t lvi = 0, rvi = 0;
      if (proof_is_int_literal(left, &lvi) &&
          proof_is_int_literal(right, &rvi) && lvi > rvi) {
        char *tmp = left; left = right; right = tmp;
      }
    }
    /*
     * Commutative arithmetic/bitwise/logical: sort lexicographically
     */
    else if (strcmp(op, "+") == 0 || strcmp(op, "*") == 0 ||
             strcmp(op, "&") == 0 || strcmp(op, "|") == 0 ||
             strcmp(op, "^^") == 0 || strcmp(op, "&&") == 0 ||
             strcmp(op, "||") == 0) {
      if (strcmp(left ? left : "", right ? right : "") > 0) {
        char *tmp = left; left = right; right = tmp;
      }
    }

    /*
     * PASS 7: Output the canonical form
     */
    ny_strbuf_t b;
    ny_strbuf_init(&b);
    ny_strbuf_appendf(&b, "binary:%s(%s,%s)", op, left ? left : "invalid",
                      right ? right : "invalid");
    free(left);
    free(right);
    return ny_strbuf_take(&b);
  }

  char *json = ny_expr_to_json(e);
  ny_strbuf_t b;
  ny_strbuf_init(&b);
  ny_strbuf_append(&b, "ast:");
  ny_strbuf_append(&b, json ? json : "null");
  if (json)
    rt_free((int64_t)(uintptr_t)json);
  return ny_strbuf_take(&b);
}

/*
 * Canonical proposition structural matching. Literal tokens must be identical;
 * name:X matches any name:Y because names denote values whose range
 * satisfaction is verified later by the codegen proof-parameter check.
 */

/*
 * Skip one canonical atom; *p advances past it (boundary or terminator).
 */
static void proof_canon_skip(const char **p) {
  const char *s = *p;
  if (strncmp(s, "binary:", 7) == 0 || strncmp(s, "unary:", 6) == 0) {
    const char *open = strchr(s, '(');
    if (!open) {
      *p = s + strlen(s);
      return;
    }
    int depth = 0;
    for (const char *q = open; *q; ++q) {
      if (*q == '(')
        ++depth;
      else if (*q == ')') {
        if (--depth == 0) {
          *p = q + 1;
          return;
        }
      }
    }
    *p = s + strlen(s);
    return;
  }
  const char *q = s;
  while (*q && *q != ',' && *q != ')')
    ++q;
  *p = q;
}

static bool proof_canon_shape_match(const char **pa, const char **pb,
                                    int depth) {
  const char *a = *pa, *b = *pb;
  if (!a || !b || depth > PROOF_NORM_MAX_DEPTH)
    return false;
  /*
   * A value name is a wildcard: `name:X` matches any atom or nested
   * proposition (literal, another name, or a whole binary/unary subtree)
   * on the other side. Range satisfaction is enforced separately at call
   * sites by ny_proof_call_params_ok, so shape matching here can be
   * permissive.
   */
  if (strncmp(a, "name:", 5) == 0 || strncmp(b, "name:", 5) == 0) {
    proof_canon_skip(pa);
    proof_canon_skip(pb);
    return true;
  }
  if (strncmp(a, "binary:", 7) == 0 && strncmp(b, "binary:", 7) == 0) {
    const char *ao = strchr(a, '('), *bo = strchr(b, '(');
    if (!ao || !bo)
      return false;
    size_t alen = (size_t)(ao - a), blen = (size_t)(bo - b);
    if (alen != blen || memcmp(a, b, alen) != 0)
      return false;
    *pa = ao + 1;
    *pb = bo + 1;
    if (!proof_canon_shape_match(pa, pb, depth + 1))
      return false;
    if (**pa != ',' || **pb != ',')
      return false;
    *pa += 1;
    *pb += 1;
    if (!proof_canon_shape_match(pa, pb, depth + 1))
      return false;
    if (**pa != ')' || **pb != ')')
      return false;
    *pa += 1;
    *pb += 1;
    return true;
  }
  if (strncmp(a, "unary:", 6) == 0 && strncmp(b, "unary:", 6) == 0) {
    const char *ao = strchr(a, '('), *bo = strchr(b, '(');
    if (!ao || !bo)
      return false;
    size_t alen = (size_t)(ao - a), blen = (size_t)(bo - b);
    if (alen != blen || memcmp(a, b, alen) != 0)
      return false;
    *pa = ao + 1;
    *pb = bo + 1;
    if (!proof_canon_shape_match(pa, pb, depth + 1))
      return false;
    if (**pa != ')' || **pb != ')')
      return false;
    *pa += 1;
    *pb += 1;
    return true;
  }
  /*
   * Literals and everything else: atoms must be textually identical.
   */
  {
    const char *sa = a, *sb = b;
    proof_canon_skip(&sa);
    proof_canon_skip(&sb);
    size_t la = (size_t)(sa - a), lb = (size_t)(sb - b);
    if (la != lb || memcmp(a, b, la) != 0)
      return false;
    *pa = sa;
    *pb = sb;
    return true;
  }
}

bool ny_proof_proposition_shape_matches(const char *a, const char *b) {
  if (!a || !b)
    return false;
  const char *pa_s = a, *pb_s = b;
  if (strncmp(pa_s, "proof<", 6) == 0)
    pa_s += 6;
  if (strncmp(pb_s, "proof<", 6) == 0)
    pb_s += 6;
  size_t la = strlen(pa_s), lb = strlen(pb_s);
  if (la > 0 && pa_s[la - 1] == '>')
    --la;
  if (lb > 0 && pb_s[lb - 1] == '>')
    --lb;
  char *ca = ny_strndup(pa_s, la);
  char *cb = ny_strndup(pb_s, lb);
  if (!ca || !cb) {
    free(ca);
    free(cb);
    return false;
  }
  const char *pa = ca, *pb = cb;
  bool ok = proof_canon_shape_match(&pa, &pb, 0);
  if (ok && (*pa || *pb))
    ok = false;
  free(ca);
  free(cb);
  return ok;
}

char *ny_proof_type_from_expr(expr_t *expr) {
  char *prop_text = proof_prop_expr(expr);
  if (!prop_text)
    return NULL;
  ny_strbuf_t type;
  ny_strbuf_init(&type);
  ny_strbuf_append(&type, "proof<");
  ny_strbuf_append(&type, prop_text);
  ny_strbuf_append_c(&type, '>');
  free(prop_text);
  return ny_strbuf_take(&type);
}

const char *parser_parse_proof_type_arg(parser_t *p) {
  p->proof_type_depth++;
  expr_t *prop = p_parse_expr(p, 0);
  p->proof_type_depth--;
  if (!prop) {
    parser_error(p, p->cur, "expected proposition in proof<...>", NULL);
    return NULL;
  }
  if (parser_match(p, NY_T_GT)) {
  } else if (p->cur.kind == NY_T_RSHIFT) {
    token_t tok = p->cur;
    p->cur.kind = NY_T_GT;
    p->cur.lexeme = tok.lexeme + 1;
    p->cur.len = 1;
    p->cur.col = tok.col + 1;
  } else {
    parser_error(p, p->cur, "'>' after proof proposition", NULL);
    return NULL;
  }
  char *owned = ny_proof_type_from_expr(prop);
  if (!owned)
    return NULL;
  const char *out = parser_intern(p, owned, strlen(owned));
  free(owned);
  return out;
}
