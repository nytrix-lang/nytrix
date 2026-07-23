#include "base/strbuf.h"
#include "parse/json.h"
#include "parse/proof.h"
#include "priv.h"
#include <stdlib.h>
#include <string.h>

/*
 * Canonical proof propositions are compiler metadata, never source text.  The
 * compact form below deliberately handles only equivalences whose validity is
 * independent of a theorem prover: equality symmetry and reversed ordered
 * comparisons.  All other expression shapes retain their AST structure.
 */
static char *proof_prop_expr(expr_t *e) {
  if (!e)
    return ny_strdup("invalid");
  ny_strbuf_t b;
  ny_strbuf_init(&b);
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    ny_strbuf_appendf(&b, "name:%s", e->as.ident.name);
  } else if (e->kind == NY_E_LITERAL) {
    switch (e->as.literal.kind) {
    case NY_LIT_INT: ny_strbuf_appendf(&b, "int:%lld", (long long)e->as.literal.as.i); break;
    case NY_LIT_FLOAT: ny_strbuf_appendf(&b, "float:%.17g", e->as.literal.as.f); break;
    case NY_LIT_BOOL: ny_strbuf_append(&b, e->as.literal.as.b ? "bool:true" : "bool:false"); break;
    case NY_LIT_STR: ny_strbuf_append(&b, "str:"); ny_strbuf_json_str(&b, e->as.literal.as.s.data); break;
    }
  } else if (e->kind == NY_E_UNARY && e->as.unary.op) {
    char *right = proof_prop_expr(e->as.unary.right);
    ny_strbuf_appendf(&b, "unary:%s(%s)", e->as.unary.op, right ? right : "invalid");
    free(right);
  } else if ((e->kind == NY_E_BINARY || e->kind == NY_E_LOGICAL) &&
             e->as.binary.op) {
    char *left = proof_prop_expr(e->as.binary.left);
    char *right = proof_prop_expr(e->as.binary.right);
    const char *op = e->as.binary.op;
    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0) {
      if (strcmp(left ? left : "", right ? right : "") > 0) {
        char *tmp = left; left = right; right = tmp;
      }
    } else if (strcmp(op, ">") == 0) {
      op = "<"; char *tmp = left; left = right; right = tmp;
    } else if (strcmp(op, ">=") == 0) {
      op = "<="; char *tmp = left; left = right; right = tmp;
    }
    ny_strbuf_appendf(&b, "binary:%s(%s,%s)", op, left ? left : "invalid",
                      right ? right : "invalid");
    free(left);
    free(right);
  } else {
    char *json = ny_expr_to_json(e);
    ny_strbuf_append(&b, "ast:");
    ny_strbuf_append(&b, json ? json : "null");
    if (json)
      free(json);
  }
  return ny_strbuf_take(&b);
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
