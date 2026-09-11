/*
 * Hindley-Milner type inference: constraint generation, unification,
 * and monomorphic specialization with let-polymorphism support.
 */
#include "code/typing/typeinfer.h"
#include "code/parse/ast.h"
#include "base/util.h"
#include "code/priv.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void typeinfer_walk_stmt(typeinfer_ctx_t *ctx, stmt_t *s);
static void typeinfer_walk_expr(typeinfer_ctx_t *ctx, expr_t *e);

static const char *ny_type_arena_strdup(ny_type_arena_t *arena, const char *s) {
  if (!s)
    s = "";
  if (!arena)
    return ny_strdup(s);
  return arena_strndup(&arena->arena, s, strlen(s));
}

void ny_type_arena_init(ny_type_arena_t *arena) {
  if (!arena)
    return;
  memset(arena, 0, sizeof(*arena));
}

void ny_type_arena_reset(ny_type_arena_t *arena) {
  if (!arena)
    return;
  arena_reset(&arena->arena);
  arena->nodes_allocated = 0;
  arena->next_var_id = 0;
}

static ny_type_t *ny_type_new(ny_type_arena_t *arena, ny_type_kind_t kind) {
  ny_type_t *t = (ny_type_t *)arena_alloc(arena ? &arena->arena : NULL, sizeof(*t));
  if (!t)
    return NULL;
  t->kind = kind;
  if (arena)
    arena->nodes_allocated++;
  return t;
}

ny_type_t *ny_type_concrete(ny_type_arena_t *arena, const char *name) {
  ny_type_t *t = ny_type_new(arena, NY_TYPE_CONCRETE);
  if (!t)
    return NULL;
  t->as.name = ny_type_arena_strdup(arena, name && *name ? name : "any");
  return t;
}

ny_type_t *ny_type_var(ny_type_arena_t *arena) {
  ny_type_t *t = ny_type_new(arena, NY_TYPE_VAR);
  if (!t)
    return NULL;
  t->id = arena ? arena->next_var_id++ : 0;
  t->as.var.parent = t;
  t->as.var.bound = NULL;
  return t;
}

ny_type_t *ny_type_arrow_effects(ny_type_arena_t *arena, ny_type_t *param,
                                 ny_type_t *ret, ny_type_t *effects) {
  ny_type_t *t = ny_type_new(arena, NY_TYPE_ARROW);
  if (!t)
    return NULL;
  t->as.arrow.param = param;
  t->as.arrow.ret = ret;
  t->as.arrow.effects = effects;
  return t;
}

ny_type_t *ny_type_arrow(ny_type_arena_t *arena, ny_type_t *param, ny_type_t *ret) {
  return ny_type_arrow_effects(arena, param, ret, NULL);
}

ny_type_t *ny_type_effect(ny_type_arena_t *arena, uint32_t mask, const char *capability) {
  ny_type_t *t = ny_type_new(arena, NY_TYPE_EFFECT);
  if (!t)
    return NULL;
  t->as.effect.mask = mask;
  t->as.effect.capability = capability ? ny_type_arena_strdup(arena, capability) : NULL;
  return t;
}

ny_type_t *ny_type_effect_var(ny_type_arena_t *arena) {
  ny_type_t *t = ny_type_new(arena, NY_TYPE_EFFECT_VAR);
  if (!t)
    return NULL;
  t->id = arena ? arena->next_var_id++ : 0;
  t->as.effect_var.var_id = t->id;
  t->as.effect_var.parent = t;
  t->as.effect_var.bound = NULL;
  return t;
}

ny_type_t *ny_type_row(ny_type_arena_t *arena, const char *label,
                       ny_type_t *field_type, ny_type_t *tail) {
  ny_type_t *t = ny_type_new(arena, NY_TYPE_ROW);
  if (!t)
    return NULL;
  t->as.row.label = label ? ny_type_arena_strdup(arena, label) : NULL;
  t->as.row.field_type = field_type;
  t->as.row.tail = tail;
  return t;
}

bool ny_type_row_has_label(ny_type_t *row, const char *label) {
  if (!label)
    return false;
  for (ny_type_t *curr = row; curr; curr = ny_type_find(curr->as.row.tail)) {
    if (curr->kind != NY_TYPE_ROW)
      break;
    if (curr->as.row.label && strcmp(curr->as.row.label, label) == 0)
      return true;
  }
  return false;
}

ny_type_t *ny_type_row_get_label(ny_type_t *row, const char *label) {
  if (!label)
    return NULL;
  for (ny_type_t *curr = row; curr; curr = ny_type_find(curr->as.row.tail)) {
    if (curr->kind != NY_TYPE_ROW)
      break;
    if (curr->as.row.label && strcmp(curr->as.row.label, label) == 0)
      return curr->as.row.field_type;
  }
  return NULL;
}

ny_type_t *ny_type_row_compose(ny_type_arena_t *arena, ny_type_t *row_a, ny_type_t *row_b) {
  if (!row_a)
    return row_b;
  if (!row_b)
    return row_a;
  row_a = ny_type_find(row_a);
  row_b = ny_type_find(row_b);
  if (row_a->kind != NY_TYPE_ROW)
    return row_b;
  if (row_b->kind != NY_TYPE_ROW)
    return row_a;

  ny_type_t *head = NULL;
  ny_type_t *prev = NULL;

  for (ny_type_t *curr = row_a; curr && curr->kind == NY_TYPE_ROW;
       curr = ny_type_find(curr->as.row.tail)) {
    ny_type_t *node = ny_type_row(arena, curr->as.row.label, curr->as.row.field_type, NULL);
    if (!head)
      head = node;
    else if (prev)
      prev->as.row.tail = node;
    prev = node;
  }

  for (ny_type_t *curr = row_b; curr && curr->kind == NY_TYPE_ROW;
       curr = ny_type_find(curr->as.row.tail)) {
    if (!curr->as.row.label || !ny_type_row_has_label(row_a, curr->as.row.label)) {
      ny_type_t *node = ny_type_row(arena, curr->as.row.label, curr->as.row.field_type, NULL);
      if (!head)
        head = node;
      else if (prev)
        prev->as.row.tail = node;
      prev = node;
    }
  }

  ny_type_t *b_tail = row_b;
  while (b_tail && b_tail->kind == NY_TYPE_ROW)
    b_tail = ny_type_find(b_tail->as.row.tail);
  if (b_tail && (b_tail->kind == NY_TYPE_VAR || b_tail->kind == NY_TYPE_EFFECT_VAR)) {
    if (prev)
      prev->as.row.tail = b_tail;
  }

  return head ? head : row_a;
}

ny_type_t *ny_type_capability(ny_type_arena_t *arena, const char *cap_name) {
  return ny_type_effect(arena, 0, cap_name);
}

ny_type_t *ny_type_typestate(ny_type_arena_t *arena, const char *state_label,
                             const char *from_state, const char *to_state,
                             ny_type_t *tail) {
  ny_type_t *from_t = from_state ? ny_type_concrete(arena, from_state) : NULL;
  ny_type_t *to_t = to_state ? ny_type_concrete(arena, to_state) : NULL;
  ny_type_t *transition = (from_t && to_t) ? ny_type_arrow(arena, from_t, to_t)
                                           : (from_t ? from_t : to_t);
  return ny_type_row(arena, state_label ? state_label : "state", transition, tail);
}

ny_type_t *ny_type_apply(ny_type_arena_t *arena, const char *name, ny_type_t *arg0,
                         ny_type_t *arg1, int arity) {
  ny_type_t *t = ny_type_new(arena, NY_TYPE_APPLY);
  if (!t)
    return NULL;
  t->as.apply.name = ny_type_arena_strdup(arena, name && *name ? name : "type");
  t->as.apply.arg0 = arg0;
  t->as.apply.arg1 = arg1;
  t->as.apply.arity = arity;
  t->as.apply.value_arg = 0;
  t->as.apply.value_resolved = false;
  t->as.apply.value_symbol = NULL;
  return t;
}

ny_type_t *ny_type_apply_bounded(ny_type_arena_t *arena, const char *name,
                                 ny_type_t *arg0, ny_type_t *arg1, int arity,
                                 int64_t value_arg, bool value_resolved,
                                 const char *value_symbol) {
  ny_type_t *t = ny_type_apply(arena, name, arg0, arg1, arity);
  if (!t)
    return NULL;
  t->as.apply.value_arg = value_arg;
  t->as.apply.value_resolved = value_resolved;
  t->as.apply.value_symbol =
      value_symbol ? ny_type_arena_strdup(arena, value_symbol) : NULL;
  return t;
}

ny_type_t *ny_type_find(ny_type_t *type) {
  if (!type)
    return NULL;
  if (type->kind == NY_TYPE_VAR) {
    if (type->as.var.parent && type->as.var.parent != type) {
      type->as.var.parent = ny_type_find(type->as.var.parent);
      return type->as.var.parent;
    }
    if (type->as.var.bound) {
      type->as.var.bound = ny_type_find(type->as.var.bound);
      return type->as.var.bound;
    }
    return type;
  }
  if (type->kind == NY_TYPE_EFFECT_VAR) {
    if (type->as.effect_var.parent && type->as.effect_var.parent != type) {
      type->as.effect_var.parent = ny_type_find(type->as.effect_var.parent);
      return type->as.effect_var.parent;
    }
    if (type->as.effect_var.bound) {
      type->as.effect_var.bound = ny_type_find(type->as.effect_var.bound);
      return type->as.effect_var.bound;
    }
    return type;
  }
  return type;
}

bool ny_type_occurs(ny_type_t *needle, ny_type_t *haystack) {
  needle = ny_type_find(needle);
  haystack = ny_type_find(haystack);
  if (!needle || !haystack)
    return false;
  if (needle == haystack)
    return true;
  switch (haystack->kind) {
  case NY_TYPE_VAR:
  case NY_TYPE_EFFECT_VAR:
  case NY_TYPE_CONCRETE:
  case NY_TYPE_EFFECT:
    return false;
  case NY_TYPE_ARROW:
    return ny_type_occurs(needle, haystack->as.arrow.param) ||
           ny_type_occurs(needle, haystack->as.arrow.ret) ||
           ny_type_occurs(needle, haystack->as.arrow.effects);
  case NY_TYPE_APPLY:
    return ny_type_occurs(needle, haystack->as.apply.arg0) ||
           ny_type_occurs(needle, haystack->as.apply.arg1);
  case NY_TYPE_ROW:
    return ny_type_occurs(needle, haystack->as.row.field_type) ||
           ny_type_occurs(needle, haystack->as.row.tail);
  }
  return false;
}

static bool ny_type_is_any(ny_type_t *type) {
  type = ny_type_find(type);
  return type && type->kind == NY_TYPE_CONCRETE && type->as.name &&
         strcmp(type->as.name, "any") == 0;
}

static bool ny_type_unify_rows_internal(ny_subst_t *s, ny_type_t *a, ny_type_t *b) {
  a = ny_type_find(a);
  b = ny_type_find(b);
  if (!a && !b)
    return true;
  if (!a) {
    if (b && (b->kind == NY_TYPE_VAR || b->kind == NY_TYPE_EFFECT_VAR)) {
      if (s)
        ny_subst_bind(s, b->id, NULL);
      b->as.var.bound = NULL;
      return true;
    }
    return false;
  }
  if (!b) {
    if (a && (a->kind == NY_TYPE_VAR || a->kind == NY_TYPE_EFFECT_VAR)) {
      if (s)
        ny_subst_bind(s, a->id, NULL);
      a->as.var.bound = NULL;
      return true;
    }
    return false;
  }
  if (a == b)
    return true;
  if (a->kind == NY_TYPE_VAR || a->kind == NY_TYPE_EFFECT_VAR) {
    if (s)
      ny_subst_bind(s, a->id, b);
    a->as.var.bound = b;
    return true;
  }
  if (b->kind == NY_TYPE_VAR || b->kind == NY_TYPE_EFFECT_VAR) {
    if (s)
      ny_subst_bind(s, b->id, a);
    b->as.var.bound = a;
    return true;
  }
  if (a->kind != NY_TYPE_ROW || b->kind != NY_TYPE_ROW)
    return ny_type_unify_with_subst(s, a, b);

  const char *label_a = a->as.row.label;
  ny_type_t *type_a = a->as.row.field_type;

  ny_type_t *prev_b = NULL;
  ny_type_t *match_b = NULL;
  ny_type_t *curr_b = b;
  while (curr_b && curr_b->kind == NY_TYPE_ROW) {
    if ((!label_a && !curr_b->as.row.label) ||
        (label_a && curr_b->as.row.label &&
         strcmp(label_a, curr_b->as.row.label) == 0)) {
      match_b = curr_b;
      break;
    }
    prev_b = curr_b;
    curr_b = ny_type_find(curr_b->as.row.tail);
  }

  if (match_b) {
    if (!ny_type_unify_with_subst(s, type_a, match_b->as.row.field_type))
      return false;

    ny_type_t *b_rest = NULL;
    if (prev_b) {
      prev_b->as.row.tail = match_b->as.row.tail;
      b_rest = b;
    } else {
      b_rest = ny_type_find(match_b->as.row.tail);
    }
    return ny_type_unify_rows_internal(s, ny_type_find(a->as.row.tail), b_rest);
  }

  ny_type_t *b_tail = b;
  while (b_tail && b_tail->kind == NY_TYPE_ROW)
    b_tail = ny_type_find(b_tail->as.row.tail);

  if (b_tail &&
      (b_tail->kind == NY_TYPE_VAR || b_tail->kind == NY_TYPE_EFFECT_VAR)) {
    ny_type_t *fresh_tail = ny_type_var(NULL);
    ny_type_t *extension = ny_type_row(NULL, label_a, type_a, fresh_tail);
    if (s)
      ny_subst_bind(s, b_tail->id, extension);
    b_tail->as.var.bound = extension;
    return ny_type_unify_rows_internal(s, ny_type_find(a->as.row.tail), b);
  }

  return false;
}

bool ny_type_unify_with_subst(ny_subst_t *s, ny_type_t *a, ny_type_t *b) {
  a = ny_type_find(a);
  b = ny_type_find(b);
  if (!a || !b || a == b)
    return true;
  if (ny_type_is_any(a) || ny_type_is_any(b))
    return true;

  if (a->kind == NY_TYPE_VAR) {
    if (ny_type_occurs(a, b))
      return false;
    if (s)
      ny_subst_bind(s, a->id, b);
    a->as.var.parent = b;
    a->as.var.bound = b;
    return true;
  }
  if (b->kind == NY_TYPE_VAR)
    return ny_type_unify_with_subst(s, b, a);

  if (a->kind == NY_TYPE_EFFECT_VAR) {
    if (ny_type_occurs(a, b))
      return false;
    if (s)
      ny_subst_bind(s, a->id, b);
    a->as.effect_var.parent = b;
    a->as.effect_var.bound = b;
    return true;
  }
  if (b->kind == NY_TYPE_EFFECT_VAR)
    return ny_type_unify_with_subst(s, b, a);

  if (a->kind != b->kind)
    return false;

  switch (a->kind) {
  case NY_TYPE_CONCRETE:
    return a->as.name && b->as.name && strcmp(a->as.name, b->as.name) == 0;
  case NY_TYPE_EFFECT:
    if (a->as.effect.capability || b->as.effect.capability) {
      if (!a->as.effect.capability || !b->as.effect.capability)
        return false;
      return strcmp(a->as.effect.capability, b->as.effect.capability) == 0;
    }
    return a->as.effect.mask == b->as.effect.mask ||
           a->as.effect.mask == NY_FX_ALL || b->as.effect.mask == NY_FX_ALL;
  case NY_TYPE_ARROW:
    return ny_type_unify_with_subst(s, a->as.arrow.param, b->as.arrow.param) &&
           ny_type_unify_with_subst(s, a->as.arrow.ret, b->as.arrow.ret) &&
           ny_type_unify_with_subst(s, a->as.arrow.effects, b->as.arrow.effects);
  case NY_TYPE_APPLY:
    return a->as.apply.name && b->as.apply.name &&
           strcmp(a->as.apply.name, b->as.apply.name) == 0 &&
           a->as.apply.arity == b->as.apply.arity &&
           a->as.apply.value_resolved == b->as.apply.value_resolved &&
           (!a->as.apply.value_resolved ||
            a->as.apply.value_arg == b->as.apply.value_arg) &&
           ((!a->as.apply.value_symbol && !b->as.apply.value_symbol) ||
            (a->as.apply.value_symbol && b->as.apply.value_symbol &&
             strcmp(a->as.apply.value_symbol, b->as.apply.value_symbol) == 0)) &&
           ny_type_unify_with_subst(s, a->as.apply.arg0, b->as.apply.arg0) &&
           ny_type_unify_with_subst(s, a->as.apply.arg1, b->as.apply.arg1);
  case NY_TYPE_ROW:
    return ny_type_unify_rows_internal(s, a, b);
  case NY_TYPE_VAR:
  case NY_TYPE_EFFECT_VAR:
    return true;
  }
  return false;
}

bool ny_type_unify(ny_type_t *a, ny_type_t *b) {
  return ny_type_unify_with_subst(NULL, a, b);
}

const char *ny_type_kind_name(ny_type_kind_t kind) {
  switch (kind) {
  case NY_TYPE_CONCRETE:
    return "concrete";
  case NY_TYPE_VAR:
    return "var";
  case NY_TYPE_ARROW:
    return "arrow";
  case NY_TYPE_APPLY:
    return "apply";
  case NY_TYPE_EFFECT:
    return "effect";
  case NY_TYPE_EFFECT_VAR:
    return "effect_var";
  case NY_TYPE_ROW:
    return "row";
  }
  return "unknown";
}

static void ny_type_effect_mask_to_buf(uint32_t mask, char *buf, size_t cap) {
  if (!buf || cap == 0)
    return;
  buf[0] = '\0';
  if (mask == NY_FX_NONE) {
    snprintf(buf, cap, "pure");
    return;
  }
  size_t used = 0;
  struct {
    uint32_t bit;
    const char *name;
  } parts[] = {
      {NY_FX_IO, "io"},
      {NY_FX_ALLOC, "alloc"},
      {NY_FX_FFI, "ffi"},
      {NY_FX_THREAD, "thread"}};
  for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
    if ((mask & parts[i].bit) == 0)
      continue;
    int n = snprintf(buf + used, cap - used, "%s%s", used ? "|" : "", parts[i].name);
    if (n < 0)
      break;
    size_t wrote = (size_t)n;
    if (wrote >= cap - used) {
      used = cap - 1;
      break;
    }
    used += wrote;
  }
}

static char *ny_type_fmt2(const char *fmt, const char *a, const char *b) {
  if (!a)
    a = "any";
  if (!b)
    b = "any";
  int n = snprintf(NULL, 0, fmt, a, b);
  if (n < 0)
    return ny_strdup("any");
  char *out = (char *)malloc((size_t)n + 1);
  if (!out)
    return ny_strdup("any");
  snprintf(out, (size_t)n + 1, fmt, a, b);
  return out;
}

char *ny_type_to_string(ny_type_t *type) {
  type = ny_type_find(type);
  if (!type)
    return ny_strdup("any");
  switch (type->kind) {
  case NY_TYPE_CONCRETE:
    return ny_strdup(type->as.name ? type->as.name : "any");
  case NY_TYPE_VAR: {
    char buf[32];
    snprintf(buf, sizeof(buf), "'t%d", type->id);
    return ny_strdup(buf);
  }
  case NY_TYPE_EFFECT_VAR: {
    char buf[32];
    snprintf(buf, sizeof(buf), "'e%d", type->id);
    return ny_strdup(buf);
  }
  case NY_TYPE_EFFECT: {
    if (type->as.effect.capability) {
      char buf[128];
      snprintf(buf, sizeof(buf), "cap(%s)", type->as.effect.capability);
      return ny_strdup(buf);
    }
    char buf[128];
    ny_type_effect_mask_to_buf(type->as.effect.mask, buf, sizeof(buf));
    return ny_strdup(buf[0] ? buf : "pure");
  }
  case NY_TYPE_ROW: {
    char buf[512] = "{ ";
    size_t len = 2;
    for (ny_type_t *curr = type; curr; curr = ny_type_find(curr->as.row.tail)) {
      if (curr->kind != NY_TYPE_ROW) {
        if (curr->kind == NY_TYPE_VAR || curr->kind == NY_TYPE_EFFECT_VAR) {
          int n = snprintf(buf + len, sizeof(buf) - len, "| 'r%d ", curr->id);
          if (n > 0)
            len += (size_t)n;
        }
        break;
      }
      char *ft = curr->as.row.field_type
                     ? ny_type_to_string(curr->as.row.field_type)
                     : NULL;
      int n = snprintf(buf + len, sizeof(buf) - len, "%s%s: %s",
                       (curr != type) ? ", " : "",
                       curr->as.row.label ? curr->as.row.label : "_",
                       ft ? ft : "any");
      if (ft)
        free(ft);
      if (n > 0)
        len += (size_t)n;
      if (len >= sizeof(buf) - 16)
        break;
    }
    if (len < sizeof(buf) - 2) {
      buf[len++] = ' ';
      buf[len++] = '}';
      buf[len] = '\0';
    }
    return ny_strdup(buf);
  }
  case NY_TYPE_ARROW: {
    char *p = ny_type_to_string(type->as.arrow.param);
    char *r = ny_type_to_string(type->as.arrow.ret);
    char *out = NULL;
    if (type->as.arrow.effects) {
      char *eff = ny_type_to_string(type->as.arrow.effects);
      int n = snprintf(NULL, 0, "fn(%s) -[%s]-> %s", p ? p : "any",
                       eff ? eff : "pure", r ? r : "any");
      if (n > 0) {
        out = (char *)malloc((size_t)n + 1);
        if (out)
          snprintf(out, (size_t)n + 1, "fn(%s) -[%s]-> %s", p ? p : "any",
                   eff ? eff : "pure", r ? r : "any");
      }
      free(eff);
    } else {
      out = ny_type_fmt2("fn(%s)->%s", p, r);
    }
    free(p);
    free(r);
    return out ? out : ny_strdup("any");
  }
  case NY_TYPE_APPLY: {
    const char *name = type->as.apply.name ? type->as.apply.name : "type";
    char *a = ny_type_to_string(type->as.apply.arg0);
    char *b = ny_type_to_string(type->as.apply.arg1);
    char *out = NULL;
    if (type->as.apply.value_resolved) {
      int n = snprintf(NULL, 0, "%s<%" PRId64 ">", name,
                       type->as.apply.value_arg);
      if (n >= 0) {
        out = (char *)malloc((size_t)n + 1);
        if (out)
          snprintf(out, (size_t)n + 1, "%s<%" PRId64 ">", name,
                   type->as.apply.value_arg);
      }
    } else if (type->as.apply.value_symbol) {
      int n = snprintf(NULL, 0, "%s<%s>", name, type->as.apply.value_symbol);
      if (n >= 0) {
        out = (char *)malloc((size_t)n + 1);
        if (out)
          snprintf(out, (size_t)n + 1, "%s<%s>", name,
                   type->as.apply.value_symbol);
      }
    } else if (type->as.apply.arity <= 0) {
      out = ny_strdup(name);
    } else if (type->as.apply.arity == 1) {
      int n = snprintf(NULL, 0, "%s<%s>", name, a ? a : "any");
      if (n < 0)
        n = 3;
      out = (char *)malloc((size_t)n + 1);
      if (out)
        snprintf(out, (size_t)n + 1, "%s<%s>", name, a ? a : "any");
    } else {
      int n = snprintf(NULL, 0, "%s<%s, %s>", name, a ? a : "any", b ? b : "any");
      if (n < 0)
        n = 3;
      out = (char *)malloc((size_t)n + 1);
      if (out)
        snprintf(out, (size_t)n + 1, "%s<%s, %s>", name, a ? a : "any", b ? b : "any");
    }
    free(a);
    free(b);
    return out ? out : ny_strdup("any");
  }
  }
  return ny_strdup("any");
}

ny_subst_t *ny_subst_new(int cap) {
  if (cap <= 0)
    cap = 64;
  ny_subst_t *s = (ny_subst_t *)calloc(1, sizeof(*s));
  if (!s)
    return NULL;
  s->capacity = cap;
  s->count = 0;
  s->parent = (int *)malloc((size_t)cap * sizeof(int));
  s->rank = (int *)calloc((size_t)cap, sizeof(int));
  s->binding = (ny_type_t **)calloc((size_t)cap, sizeof(ny_type_t *));
  if (!s->parent || !s->rank || !s->binding) {
    ny_subst_free(s);
    return NULL;
  }
  for (int i = 0; i < cap; ++i)
    s->parent[i] = i;
  return s;
}

void ny_subst_free(ny_subst_t *s) {
  if (!s)
    return;
  free(s->parent);
  free(s->rank);
  free(s->binding);
  free(s);
}

int ny_subst_fresh(ny_subst_t *s) {
  if (!s)
    return 0;
  if (s->count >= s->capacity) {
    int ncap = s->capacity ? s->capacity * 2 : 64;
    int *np = (int *)realloc(s->parent, (size_t)ncap * sizeof(int));
    int *nr = (int *)realloc(s->rank, (size_t)ncap * sizeof(int));
    ny_type_t **nb =
        (ny_type_t **)realloc(s->binding, (size_t)ncap * sizeof(ny_type_t *));
    if (!np || !nr || !nb)
      return -1;
    s->parent = np;
    s->rank = nr;
    s->binding = nb;
    for (int i = s->capacity; i < ncap; ++i) {
      s->parent[i] = i;
      s->rank[i] = 0;
      s->binding[i] = NULL;
    }
    s->capacity = ncap;
  }
  int id = s->count++;
  s->parent[id] = id;
  s->rank[id] = 0;
  s->binding[id] = NULL;
  return id;
}

ny_type_t *ny_subst_find(ny_subst_t *s, int var_id) {
  if (!s || var_id < 0 || var_id >= s->count)
    return NULL;
  int root = var_id;
  while (root >= 0 && root < s->count && s->parent[root] != root)
    root = s->parent[root];
  int curr = var_id;
  while (curr >= 0 && curr < s->count && s->parent[curr] != root) {
    int next = s->parent[curr];
    s->parent[curr] = root;
    curr = next;
  }
  if (root >= 0 && root < s->count && s->binding[root]) {
    ny_type_t *b = s->binding[root];
    if (b && b->kind == NY_TYPE_VAR && b->id != root && b->id >= 0 &&
        b->id < s->count) {
      ny_type_t *resolved = ny_subst_find(s, b->id);
      if (resolved)
        s->binding[root] = resolved;
      return s->binding[root];
    }
    return b;
  }
  return NULL;
}

void ny_subst_union(ny_subst_t *s, int a, int b) {
  if (!s || a < 0 || a >= s->count || b < 0 || b >= s->count)
    return;
  int ra = a, rb = b;
  while (s->parent[ra] != ra)
    ra = s->parent[ra];
  while (s->parent[rb] != rb)
    rb = s->parent[rb];
  if (ra == rb)
    return;
  if (s->rank[ra] < s->rank[rb]) {
    s->parent[ra] = rb;
    if (s->binding[ra] && !s->binding[rb])
      s->binding[rb] = s->binding[ra];
  } else if (s->rank[ra] > s->rank[rb]) {
    s->parent[rb] = ra;
    if (s->binding[rb] && !s->binding[ra])
      s->binding[ra] = s->binding[rb];
  } else {
    s->parent[rb] = ra;
    s->rank[ra]++;
    if (s->binding[rb] && !s->binding[ra])
      s->binding[ra] = s->binding[rb];
  }
}

static bool ny_type_occurs_id(int id, ny_type_t *t) {
  if (!t)
    return false;
  if (t->kind == NY_TYPE_VAR || t->kind == NY_TYPE_EFFECT_VAR)
    return t->id == id;
  if (t->kind == NY_TYPE_ARROW)
    return ny_type_occurs_id(id, t->as.arrow.param) ||
           ny_type_occurs_id(id, t->as.arrow.ret) ||
           ny_type_occurs_id(id, t->as.arrow.effects);
  if (t->kind == NY_TYPE_APPLY)
    return ny_type_occurs_id(id, t->as.apply.arg0) ||
           ny_type_occurs_id(id, t->as.apply.arg1);
  if (t->kind == NY_TYPE_ROW)
    return ny_type_occurs_id(id, t->as.row.field_type) ||
           ny_type_occurs_id(id, t->as.row.tail);
  return false;
}

void ny_subst_bind(ny_subst_t *s, int var_id, ny_type_t *t) {
  if (!s || var_id < 0 || var_id >= s->count || !t)
    return;
  int root = var_id;
  while (s->parent[root] != root)
    root = s->parent[root];
  if ((t->kind == NY_TYPE_VAR || t->kind == NY_TYPE_EFFECT_VAR) && t->id >= 0 &&
      t->id < s->count) {
    int tr = t->id;
    while (s->parent[tr] != tr)
      tr = s->parent[tr];
    if (root == tr)
      return;
    ny_subst_union(s, root, tr);
    return;
  }
  if (!ny_type_occurs_id(root, t))
    s->binding[root] = t;
}

void ny_subst_apply(ny_subst_t *s, ny_type_t *t) {
  if (!s || !t)
    return;
  if (t->kind == NY_TYPE_VAR || t->kind == NY_TYPE_EFFECT_VAR) {
    ny_type_t *bound = ny_subst_find(s, t->id);
    if (bound && bound != t) {
      ny_subst_apply(s, bound);
      *t = *bound;
    }
  } else if (t->kind == NY_TYPE_ARROW) {
    ny_subst_apply(s, t->as.arrow.param);
    ny_subst_apply(s, t->as.arrow.ret);
    ny_subst_apply(s, t->as.arrow.effects);
  } else if (t->kind == NY_TYPE_APPLY) {
    ny_subst_apply(s, t->as.apply.arg0);
    ny_subst_apply(s, t->as.apply.arg1);
  } else if (t->kind == NY_TYPE_ROW) {
    ny_subst_apply(s, t->as.row.field_type);
    ny_subst_apply(s, t->as.row.tail);
  }
}

static size_t typeinfer_align_up(size_t value, size_t align) {
  if (align == 0)
    return value;
  size_t rem = value % align;
  return rem ? value + (align - rem) : value;
}

void typeinfer_ctx_init(typeinfer_ctx_t *ctx, size_t max_vars, scope *scopes, codegen_t *cg) {
  if (!ctx)
    return;
  memset(ctx, 0, sizeof(*ctx));
  ctx->var_names_cap = max_vars;
  ctx->var_names_len = 0;
  ny_type_arena_init(&ctx->type_arena);
  ctx->formal_hm_enabled = true;
  ctx->subst = ny_subst_new(128);

  ctx->hash_cap = max_vars * 2;
  if (ctx->hash_cap < 16)
    ctx->hash_cap = 16;
  if (max_vars > 0) {
    size_t slots_bytes = sizeof(typeinfer_var_slot_t) * max_vars;
    size_t hash_off = typeinfer_align_up(slots_bytes, sizeof(int));
    size_t hash_bytes = sizeof(int) * ctx->hash_cap;
    unsigned char *storage = (unsigned char *)calloc(1, hash_off + hash_bytes);
    if (storage) {
      ctx->vars = (typeinfer_var_slot_t *)storage;
      ctx->hash_table = (int *)(void *)(storage + hash_off);
    }
  }
  if (ctx->hash_table) {
    for (size_t i = 0; i < ctx->hash_cap; i++)
      ctx->hash_table[i] = -1;
  } else {
    ctx->hash_cap = 0;
  }
  if (max_vars > 0 && (!ctx->vars || !ctx->hash_table)) {
    free(ctx->vars);
    ny_type_arena_reset(&ctx->type_arena);
    ctx->vars = NULL;
    ctx->hash_table = NULL;
    ctx->var_names_cap = 0;
    ctx->var_names_len = 0;
    ctx->hash_cap = 0;
    ctx->formal_hm_enabled = false;
  }

  ctx->scopes = scopes;
  ctx->func_depth = 1;
  ctx->cg = cg;
}

void typeinfer_ctx_dispose(typeinfer_ctx_t *ctx) {
  if (!ctx)
    return;
  if (ctx->subst) {
    ny_subst_free(ctx->subst);
    ctx->subst = NULL;
  }
  free(ctx->vars);
  arena_free(&ctx->type_arena.arena);
  memset(ctx, 0, sizeof(*ctx));
}

static int typeinfer_find_var(typeinfer_ctx_t *ctx, const char *name) {
  if (!ctx || !name || !*name || !ctx->hash_table || ctx->hash_cap == 0)
    return -1;

  uint64_t h = ny_hash64_cstr(name);
  size_t idx = h % ctx->hash_cap;

  for (size_t i = 0; i < ctx->hash_cap; i++) {
    int var_idx = ctx->hash_table[(idx + i) % ctx->hash_cap];
    if (var_idx == -1)
      return -1;
    if (strcmp(ctx->vars[var_idx].name, name) == 0)
      return var_idx;
  }
  return -1;
}

void typeinfer_add_var(typeinfer_ctx_t *ctx, const char *name) {
  if (!ctx || !name || !*name || !ctx->vars || !ctx->hash_table ||
      ctx->hash_cap == 0)
    return;

  if (typeinfer_find_var(ctx, name) >= 0)
    return;

  if (ctx->var_names_len >= ctx->var_names_cap)
    return;

  int new_idx = (int)ctx->var_names_len;
  ctx->vars[ctx->var_names_len++].name = name;
  ctx->vars[new_idx].type = ny_type_var(&ctx->type_arena);

  uint64_t h = ny_hash64_cstr(name);
  size_t h_idx = h % ctx->hash_cap;
  while (ctx->hash_table[h_idx] != -1) {
    h_idx = (h_idx + 1) % ctx->hash_cap;
  }
  ctx->hash_table[h_idx] = new_idx;
}

static bool typeinfer_unify_named(typeinfer_ctx_t *ctx, int idx, const char *type_name) {
  if (!ctx || idx < 0 || (size_t)idx >= ctx->var_names_len || !ctx->vars ||
      !ctx->vars[idx].type || !ctx->formal_hm_enabled)
    return true;
  ny_type_t *want = NULL;
  const char *lt = type_name ? strchr(type_name, '<') : NULL;
  const char *gt = lt ? strrchr(lt, '>') : NULL;
  if (lt && gt && gt > lt + 1) {
    char ctor[64] = {0};
    size_t ctor_len = (size_t)(lt - type_name);
    if (ctor_len < sizeof(ctor)) {
      memcpy(ctor, type_name, ctor_len);
      ctor[ctor_len] = '\0';
      char val_buf[64] = {0};
      size_t val_len = (size_t)(gt - lt - 1);
      if (val_len < sizeof(val_buf)) {
        memcpy(val_buf, lt + 1, val_len);
        val_buf[val_len] = '\0';
        char *end = NULL;
        int64_t val = (int64_t)strtoll(val_buf, &end, 10);
        if (end && *end == '\0' && val > 0) {
          want = ny_type_apply_bounded(&ctx->type_arena, ctor, NULL, NULL, 0,
                                       val, true, NULL);
        } else {
          want = ny_type_apply_bounded(&ctx->type_arena, ctor, NULL, NULL, 0,
                                       0, false, val_buf);
        }
      }
    }
  }
  if (!want)
    want = ny_type_concrete(&ctx->type_arena, type_name);
  bool ok = ny_type_unify_with_subst(ctx->subst, ctx->vars[idx].type, want);
  if (!ok) {
    ctx->type_unify_errors++;
    ctx->vars[idx].is_used_in_dynamic = true;
  }
  return ok;
}

void typeinfer_mark_i64(typeinfer_ctx_t *ctx, const char *name) {
  if (!ctx || !name)
    return;
  int idx = typeinfer_find_var(ctx, name);
  if (idx >= 0 && !ctx->vars[idx].is_i64_proven &&
      typeinfer_unify_named(ctx, idx, "int")) {
    ctx->vars[idx].is_i64_proven = true;
    ctx->changed = true;
  }
}

void typeinfer_mark_f64(typeinfer_ctx_t *ctx, const char *name) {
  if (!ctx || !name)
    return;
  int idx = typeinfer_find_var(ctx, name);
  if (idx >= 0 && !ctx->vars[idx].is_f64_proven &&
      typeinfer_unify_named(ctx, idx, "f64")) {
    ctx->vars[idx].is_f64_proven = true;
    ctx->changed = true;
  }
}

void typeinfer_mark_dynamic(typeinfer_ctx_t *ctx, const char *name) {
  if (!ctx || !name)
    return;
  int idx = typeinfer_find_var(ctx, name);
  if (idx >= 0 && !ctx->vars[idx].is_used_in_dynamic) {
    ctx->vars[idx].is_used_in_dynamic = true;
    ctx->changed = true;
  }
}

bool typeinfer_escapes(typeinfer_ctx_t *ctx, const char *name) {
  if (!ctx || !name)
    return true;
  int idx = typeinfer_find_var(ctx, name);
  if (idx < 0)
    return true;
  return ctx->vars[idx].escapes;
}

void typeinfer_mark_escape(typeinfer_ctx_t *ctx, const char *name) {
  if (!ctx || !name)
    return;
  int idx = typeinfer_find_var(ctx, name);
  if (idx >= 0 && !ctx->vars[idx].escapes) {
    ctx->vars[idx].escapes = true;
    ctx->changed = true;
  }
}

bool typeinfer_is_i64(typeinfer_ctx_t *ctx, const char *name) {
  if (!ctx || !name)
    return false;
  int idx = typeinfer_find_var(ctx, name);
  if (idx < 0)
    return false;
  return ctx->vars[idx].is_i64_proven && !ctx->vars[idx].is_used_in_dynamic;
}

bool typeinfer_is_f64(typeinfer_ctx_t *ctx, const char *name) {
  if (!ctx || !name)
    return false;
  int idx = typeinfer_find_var(ctx, name);
  if (idx < 0)
    return false;
  return ctx->vars[idx].is_f64_proven && !ctx->vars[idx].is_used_in_dynamic;
}

bool typeinfer_needs_dynamic(typeinfer_ctx_t *ctx, const char *name) {
  if (!ctx || !name)
    return false;
  int idx = typeinfer_find_var(ctx, name);
  if (idx < 0)
    return false;
  return ctx->vars[idx].is_used_in_dynamic;
}

static bool expr_is_int_lit(expr_t *e) {
  if (!e)
    return false;
  return e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_INT &&
         e->tok.kind != NY_T_NIL;
}

static bool expr_is_float_lit(expr_t *e) {
  if (!e)
    return false;
  return e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_FLOAT;
}

static bool typeinfer_binary_op_returns_i64(const char *op) {
  if (!op)
    return false;
  return strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0 ||
         strcmp(op, "/") == 0 || strcmp(op, "%") == 0 || strcmp(op, "&") == 0 ||
         strcmp(op, "|") == 0 || strcmp(op, "^^") == 0 || strcmp(op, "<<") == 0 ||
         strcmp(op, ">>") == 0;
}

static bool typeinfer_binary_op_returns_f64(const char *op) {
  if (!op)
    return false;
  return strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0 ||
         strcmp(op, "/") == 0 || strcmp(op, "^") == 0;
}

bool typeinfer_expr_is_i64(typeinfer_ctx_t *ctx, expr_t *e) {
  if (!ctx || !e)
    return false;

  if (expr_is_int_lit(e))
    return true;

  if (e->kind == NY_E_IDENT)
    return typeinfer_is_i64(ctx, e->as.ident.name);

  if (e->kind == NY_E_BINARY) {
    const char *op = e->as.binary.op;
    if (typeinfer_binary_op_returns_i64(op)) {
      return typeinfer_expr_is_i64(ctx, e->as.binary.left) &&
             typeinfer_expr_is_i64(ctx, e->as.binary.right);
    }
  }

  if (e->kind == NY_E_UNARY) {
    if (e->as.unary.op[0] == '-' && !e->as.unary.op[1])
      return typeinfer_expr_is_i64(ctx, e->as.unary.right);
  }

  return false;
}

bool typeinfer_expr_is_f64(typeinfer_ctx_t *ctx, expr_t *e) {
  if (!ctx || !e)
    return false;

  if (expr_is_float_lit(e))
    return true;

  if (e->kind == NY_E_IDENT)
    return typeinfer_is_f64(ctx, e->as.ident.name);

  if (e->kind == NY_E_BINARY) {
    const char *op = e->as.binary.op;
    if (typeinfer_binary_op_returns_f64(op)) {
      return typeinfer_expr_is_f64(ctx, e->as.binary.left) ||
             typeinfer_expr_is_f64(ctx, e->as.binary.right);
    }
  }

  if (e->kind == NY_E_UNARY) {
    if (e->as.unary.op[0] == '-' && !e->as.unary.op[1])
      return typeinfer_expr_is_f64(ctx, e->as.unary.right);
  }

  return false;
}

static void typeinfer_unify_call_site(typeinfer_ctx_t *ctx, expr_t *e) {
  if (!ctx || !e || e->kind != NY_E_CALL)
    return;
  expr_t *callee = e->as.call.callee;
  if (!callee)
    return;

  ny_type_t *callee_type = NULL;
  fun_sig *sig = NULL;

  if (callee->kind == NY_E_IDENT) {
    const char *cname = callee->as.ident.name;
    int var_idx = typeinfer_find_var(ctx, cname);
    if (var_idx >= 0) {
      callee_type = ctx->vars[var_idx].type;
    } else if (ctx->cg) {
      sig = lookup_fun(ctx->cg, cname, 0);
      if (sig && sig->effect_type)
        callee_type = sig->effect_type;
    }
  }

  if (!callee_type && sig) {
    uint32_t eff_mask = sig->effects;
    ny_type_t *eff = ny_type_effect(&ctx->type_arena, eff_mask, NULL);
    callee_type = ny_type_arrow_effects(&ctx->type_arena, NULL, NULL, eff);
  }

  if (!callee_type)
    return;

  callee_type = ny_type_find(callee_type);
  if (callee_type->kind == NY_TYPE_ARROW) {
    ny_type_t *effects = ny_type_find(callee_type->as.arrow.effects);
    if (effects && (effects->kind == NY_TYPE_EFFECT_VAR || effects->kind == NY_TYPE_VAR)) {
      /* Higher-order call effect unification */
      for (size_t i = 0; i < e->as.call.args.len; i++) {
        expr_t *arg_expr = e->as.call.args.data[i].val;
        if (!arg_expr)
          continue;
        if (arg_expr->kind == NY_E_IDENT) {
          int arg_var = typeinfer_find_var(ctx, arg_expr->as.ident.name);
          if (arg_var >= 0 && ctx->vars[arg_var].type) {
            ny_type_t *arg_t = ny_type_find(ctx->vars[arg_var].type);
            if (arg_t->kind == NY_TYPE_ARROW && arg_t->as.arrow.effects) {
              ny_type_unify_with_subst(ctx->subst, effects, arg_t->as.arrow.effects);
            }
          } else if (ctx->cg) {
            fun_sig *arg_sig = lookup_fun(ctx->cg, arg_expr->as.ident.name, 0);
            if (arg_sig) {
              ny_type_t *arg_eff =
                  arg_sig->effect_type
                      ? arg_sig->effect_type
                      : ny_type_effect(&ctx->type_arena, arg_sig->effects, NULL);
              ny_type_unify_with_subst(ctx->subst, effects, arg_eff);
            }
          }
        }
      }
    }

    /* Row-polymorphic shape arguments unification */
    if (callee_type->as.arrow.param &&
        callee_type->as.arrow.param->kind == NY_TYPE_ROW) {
      for (size_t i = 0; i < e->as.call.args.len; i++) {
        expr_t *arg_expr = e->as.call.args.data[i].val;
        if (!arg_expr)
          continue;
        if (arg_expr->kind == NY_E_IDENT) {
          int arg_var = typeinfer_find_var(ctx, arg_expr->as.ident.name);
          if (arg_var >= 0 && ctx->vars[arg_var].type) {
            ny_type_t *arg_t = ny_type_find(ctx->vars[arg_var].type);
            if (arg_t->kind == NY_TYPE_ROW) {
              bool ok = ny_type_unify_with_subst(ctx->subst,
                                                  callee_type->as.arrow.param, arg_t);
              if (!ok) {
                ctx->type_unify_errors++;
              }
            }
          }
        }
      }
    }
  }
}

static void typeinfer_walk_expr(typeinfer_ctx_t *ctx, expr_t *e) {
  if (!ctx || !e)
    return;

  switch (e->kind) {
  case NY_E_IDENT: {
    const char *name = e->as.ident.name;
    typeinfer_add_var(ctx, name);
    break;
  }

  case NY_E_LITERAL:
    break;

  case NY_E_UNARY: {
    if (e->as.unary.right)
      typeinfer_walk_expr(ctx, e->as.unary.right);
    break;
  }

  case NY_E_BINARY: {
    if (e->as.binary.left)
      typeinfer_walk_expr(ctx, e->as.binary.left);
    if (e->as.binary.right)
      typeinfer_walk_expr(ctx, e->as.binary.right);
    break;
  }

  case NY_E_CALL: {
    if (e->as.call.callee) {
      typeinfer_walk_expr(ctx, e->as.call.callee);
      if (e->as.call.callee->kind == NY_E_IDENT) {
        typeinfer_mark_escape(ctx, e->as.call.callee->as.ident.name);
      }
    }
    for (size_t i = 0; i < e->as.call.args.len; i++) {
      if (e->as.call.args.data[i].val) {
        typeinfer_walk_expr(ctx, e->as.call.args.data[i].val);

        if (e->as.call.args.data[i].val->kind == NY_E_IDENT) {
          typeinfer_mark_escape(ctx, e->as.call.args.data[i].val->as.ident.name);
        }
      }
    }
    typeinfer_unify_call_site(ctx, e);
    break;
  }

  case NY_E_MEMCALL: {
    if (e->as.memcall.target) {
      typeinfer_walk_expr(ctx, e->as.memcall.target);
      if (e->as.memcall.target->kind == NY_E_IDENT) {
        typeinfer_mark_escape(ctx, e->as.memcall.target->as.ident.name);
      }
    }
    for (size_t i = 0; i < e->as.memcall.args.len; i++) {
      if (e->as.memcall.args.data[i].val) {
        typeinfer_walk_expr(ctx, e->as.memcall.args.data[i].val);
        if (e->as.memcall.args.data[i].val->kind == NY_E_IDENT) {
          typeinfer_mark_escape(ctx, e->as.memcall.args.data[i].val->as.ident.name);
        }
      }
    }
    break;
  }

  case NY_E_INDEX: {
    if (e->as.index.target) {
      typeinfer_walk_expr(ctx, e->as.index.target);
      if (e->as.index.target->kind == NY_E_IDENT) {
        typeinfer_mark_escape(ctx, e->as.index.target->as.ident.name);
      }
    }
    if (e->as.index.start)
      typeinfer_walk_expr(ctx, e->as.index.start);
    break;
  }

  case NY_E_MEMBER: {
    if (e->as.member.target) {
      typeinfer_walk_expr(ctx, e->as.member.target);
      if (e->as.member.target->kind == NY_E_IDENT) {
        typeinfer_mark_escape(ctx, e->as.member.target->as.ident.name);
      }
    }
    break;
  }

  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET: {
    for (size_t i = 0; i < e->as.list_like.len; i++) {
      if (e->as.list_like.data[i]) {
        typeinfer_walk_expr(ctx, e->as.list_like.data[i]);
        if (e->as.list_like.data[i]->kind == NY_E_IDENT) {
          typeinfer_mark_escape(ctx, e->as.list_like.data[i]->as.ident.name);
        }
      }
    }
    break;
  }

  case NY_E_DICT: {
    for (size_t i = 0; i < e->as.dict.pairs.len; i++) {
      dict_pair_t *pair = &e->as.dict.pairs.data[i];
      if (pair->key) {
        typeinfer_walk_expr(ctx, pair->key);
        if (pair->key->kind == NY_E_IDENT)
          typeinfer_mark_escape(ctx, pair->key->as.ident.name);
      }
      if (pair->value) {
        typeinfer_walk_expr(ctx, pair->value);
        if (pair->value->kind == NY_E_IDENT)
          typeinfer_mark_escape(ctx, pair->value->as.ident.name);
      }
    }
    break;
  }

  case NY_E_LAMBDA:
  case NY_E_FN:
  case NY_E_MATCH:
  case NY_E_FSTRING:
  case NY_E_TRY:
  case NY_E_COMPTIME:
  case NY_E_EMBED:
  case NY_E_ASM:
  case NY_E_PTR_TYPE:
  case NY_E_DEREF:
  case NY_E_SIZEOF:
  case NY_E_INFERRED_MEMBER:
  case NY_E_LOGICAL:
  case NY_E_TERNARY:
    break;
  case NY_E_QUOTE:
    if (e->as.quote.body)
      typeinfer_walk_stmt(ctx, e->as.quote.body);
    if (e->as.quote.expr)
      typeinfer_walk_expr(ctx, e->as.quote.expr);
    break;
  case NY_E_SPLICE:
    if (e->as.splice.expr)
      typeinfer_walk_expr(ctx, e->as.splice.expr);
    break;
  }
}

void typeinfer_walk_stmt(typeinfer_ctx_t *ctx, stmt_t *s) {
  if (!ctx || !s)
    return;

  switch (s->kind) {
  case NY_S_VAR: {

    for (size_t i = 0; i < s->as.var.names.len; i++) {
      const char *name = s->as.var.names.data[i];
      expr_t *init = (i < s->as.var.exprs.len) ? s->as.var.exprs.data[i] : NULL;

      typeinfer_add_var(ctx, name);

      if (!s->as.var.is_decl) {
        if (s->as.var.is_del) {
          typeinfer_mark_dynamic(ctx, name);
          typeinfer_mark_escape(ctx, name);
          continue;
        }
        if (!init) {
          typeinfer_mark_dynamic(ctx, name);
          typeinfer_mark_escape(ctx, name);
          continue;
        }
      }

      const char *vartype = (i < s->as.var.types.len) ? s->as.var.types.data[i] : NULL;
      if (vartype && strcmp(vartype, "int") == 0) {
        typeinfer_mark_i64(ctx, name);
      } else if (vartype && strcmp(vartype, "f64") == 0) {
        typeinfer_mark_f64(ctx, name);
      }

      if (init) {
        typeinfer_walk_expr(ctx, init);

        if (expr_is_int_lit(init)) {
          typeinfer_mark_i64(ctx, name);
        } else if (expr_is_float_lit(init)) {
          typeinfer_mark_f64(ctx, name);
        } else if (init->kind == NY_E_IDENT) {

          if (typeinfer_is_i64(ctx, init->as.ident.name))
            typeinfer_mark_i64(ctx, name);
          else if (typeinfer_is_f64(ctx, init->as.ident.name))
            typeinfer_mark_f64(ctx, name);

          if (typeinfer_escapes(ctx, init->as.ident.name))
            typeinfer_mark_escape(ctx, name);
        } else if (init->kind == NY_E_BINARY) {

          const char *op = init->as.binary.op;
          if (typeinfer_binary_op_returns_f64(op)) {

            bool left_f64 = init->as.binary.left->kind == NY_E_IDENT &&
                            typeinfer_is_f64(ctx, init->as.binary.left->as.ident.name);
            bool right_f64 = init->as.binary.right->kind == NY_E_IDENT &&
                             typeinfer_is_f64(ctx, init->as.binary.right->as.ident.name);
            if (left_f64 && right_f64)
              typeinfer_mark_f64(ctx, name);
          }
          if (typeinfer_binary_op_returns_i64(op)) {

            bool left_i64 = init->as.binary.left->kind == NY_E_IDENT &&
                            typeinfer_is_i64(ctx, init->as.binary.left->as.ident.name);
            bool right_i64 = init->as.binary.right->kind == NY_E_IDENT &&
                             typeinfer_is_i64(ctx, init->as.binary.right->as.ident.name);
            if (left_i64 && right_i64)
              typeinfer_mark_i64(ctx, name);
          }
        }
        if (!s->as.var.is_decl) {
          bool proven_i64 = typeinfer_expr_is_i64(ctx, init);
          bool proven_f64 = typeinfer_expr_is_f64(ctx, init);
          if (!proven_i64 && !proven_f64)
            typeinfer_mark_dynamic(ctx, name);
        }
      } else if (!s->as.var.is_decl) {
        typeinfer_mark_dynamic(ctx, name);
      }

      if (typeinfer_escapes(ctx, name) && init && init->kind == NY_E_IDENT) {
        typeinfer_mark_escape(ctx, init->as.ident.name);
      }
    }
    break;
  }

  case NY_S_EXPR: {
    typeinfer_walk_expr(ctx, s->as.expr.expr);
    break;
  }

  case NY_S_IF: {
    typeinfer_walk_expr(ctx, s->as.iff.test);
    if (s->as.iff.conseq)
      typeinfer_walk_stmt(ctx, s->as.iff.conseq);
    if (s->as.iff.alt)
      typeinfer_walk_stmt(ctx, s->as.iff.alt);
    break;
  }

  case NY_S_GUARD: {
    typeinfer_walk_expr(ctx, s->as.guard.value);
    typeinfer_add_var(ctx, s->as.guard.name);
    typeinfer_mark_dynamic(ctx, s->as.guard.name);
    typeinfer_mark_escape(ctx, s->as.guard.name);
    if (s->as.guard.fallback)
      typeinfer_walk_stmt(ctx, s->as.guard.fallback);
    break;
  }

  case NY_S_WHILE: {
    typeinfer_walk_expr(ctx, s->as.whl.test);
    typeinfer_walk_expr(ctx, s->as.whl.invariant);
    if (s->as.whl.init)
      typeinfer_walk_stmt(ctx, s->as.whl.init);
    typeinfer_walk_stmt(ctx, s->as.whl.body);
    if (s->as.whl.update)
      typeinfer_walk_stmt(ctx, s->as.whl.update);
    break;
  }

  case NY_S_FOR: {
    if (s->as.fr.init)
      typeinfer_walk_stmt(ctx, s->as.fr.init);
    if (s->as.fr.cond)
      typeinfer_walk_expr(ctx, s->as.fr.cond);
    if (s->as.fr.iterable)
      typeinfer_walk_expr(ctx, s->as.fr.iterable);
    if (s->as.fr.update)
      typeinfer_walk_stmt(ctx, s->as.fr.update);
    typeinfer_walk_stmt(ctx, s->as.fr.body);
    break;
  }

  case NY_S_RETURN: {
    if (s->as.ret.value) {
      typeinfer_walk_expr(ctx, s->as.ret.value);
      if (s->as.ret.value->kind == NY_E_IDENT) {
        typeinfer_mark_escape(ctx, s->as.ret.value->as.ident.name);
      }
    }
    break;
  }

  case NY_S_FUNC: {

    if (s->as.fn.body)
      typeinfer_walk_stmt(ctx, s->as.fn.body);
    break;
  }

  case NY_S_EXTERN:
  case NY_S_ENUM:
  case NY_S_STRUCT:
  case NY_S_LAYOUT:
  case NY_S_USE:
  case NY_S_MODULE:
  case NY_S_EXPORT:
  case NY_S_MACRO:
  case NY_S_INCLUDE:
  case NY_S_DEFINE:
  case NY_S_LINK:
  case NY_S_LABEL:
  case NY_S_GOTO:
  case NY_S_DEFER:
  case NY_S_BREAK:
  case NY_S_CONTINUE:
  case NY_S_TRY:
  case NY_S_MATCH:
  case NY_S_OPERATOR:
  case NY_S_IMPL:

    break;

  default:
    break;
  }
}

void typeinfer_func_body(typeinfer_ctx_t *ctx, stmt_t *body) {
  if (!ctx || !body)
    return;

  int max_passes = 20;
  for (int pass = 0; pass < max_passes; pass++) {
    ctx->changed = false;
    typeinfer_walk_stmt(ctx, body);

    if (!ctx->changed) {
      break;
    }
  }
}

void typeinfer_apply_to_scopes(typeinfer_ctx_t *ctx, scope *scopes, size_t depth) {
  if (!ctx || !scopes || depth == 0)
    return;

  for (size_t d = 0; d < depth; d++) {
    scope *s = &scopes[d];
    for (size_t i = 0; i < s->vars.len; i++) {
      binding *b = &s->vars.data[i];
      if (b->name) {
        bool bit_i64 = typeinfer_is_i64(ctx, b->name);
        bool bit_f64 = typeinfer_is_f64(ctx, b->name);
        bool bit_dyn = typeinfer_needs_dynamic(ctx, b->name);
        bool bit_esc = typeinfer_escapes(ctx, b->name);

        b->escapes = bit_esc;

        if (bit_i64 && !bit_dyn) {
          b->is_int_slot = true;
          b->is_int_direct = !b->is_slot;
        } else if (bit_f64 && !bit_dyn) {
          b->is_f64_slot = true;
          b->is_f64_direct = !b->is_slot;
        }

       if (b->stmt_t && b->stmt_t->kind == NY_S_VAR &&
           b->stmt_t->sema_kind == NY_STMT_SEMA_VAR && b->stmt_t->sema) {
         sema_var_t *sv = (sema_var_t *)b->stmt_t->sema;
         arena_t *sema_arena = ctx->cg ? ctx->cg->arena : NULL;
         for (size_t k = 0; k < b->stmt_t->as.var.names.len; k++) {
           if (strcmp(b->stmt_t->as.var.names.data[k], b->name) == 0) {
             while (sv->is_int_proven.len <= k) {
               if (sema_arena)
                 vec_push_arena(sema_arena, &sv->is_int_proven, false);
               else
                 vec_push(&sv->is_int_proven, false);
             }
             while (sv->is_f64_proven.len <= k) {
               if (sema_arena)
                 vec_push_arena(sema_arena, &sv->is_f64_proven, false);
               else
                 vec_push(&sv->is_f64_proven, false);
             }
             while (sv->escapes.len <= k) {
               if (sema_arena)
                 vec_push_arena(sema_arena, &sv->escapes, false);
               else
                 vec_push(&sv->escapes, false);
             }
             sv->is_int_proven.data[k] = bit_i64 && !bit_dyn;
             sv->is_f64_proven.data[k] = bit_f64 && !bit_dyn;
             sv->escapes.data[k] = bit_esc;
              break;
            }
          }
        }
      }
    }
  }
}

static void typeinfer_json_append(char **buf, size_t *len, size_t *cap, const char *fmt, ...) {
  if (!buf || !len || !cap || !fmt)
    return;
  if (!*buf) {
    *cap = 1024;
    *buf = malloc(*cap);
    if (!*buf)
      return;
    (*buf)[0] = '\0';
    *len = 0;
  }
  for (;;) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*buf + *len, *cap - *len, fmt, ap);
    va_end(ap);
    if (n < 0)
      return;
    if (*len + (size_t)n < *cap) {
      *len += (size_t)n;
      return;
    }
    size_t new_cap = *cap * 2 + (size_t)n + 1;
    char *tmp = realloc(*buf, new_cap);
    if (!tmp)
      return;
    *buf = tmp;
    *cap = new_cap;
  }
}

static void typeinfer_json_str(char **buf, size_t *len, size_t *cap, const char *s) {
  typeinfer_json_append(buf, len, cap, "\"");
  if (s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
      switch (*p) {
      case '"':
        typeinfer_json_append(buf, len, cap, "\\\"");
        break;
      case '\\':
        typeinfer_json_append(buf, len, cap, "\\\\");
        break;
      case '\n':
        typeinfer_json_append(buf, len, cap, "\\n");
        break;
      case '\r':
        typeinfer_json_append(buf, len, cap, "\\r");
        break;
      case '\t':
        typeinfer_json_append(buf, len, cap, "\\t");
        break;
      default:
        if (*p < 32)
          typeinfer_json_append(buf, len, cap, "\\u%04x", (unsigned)*p);
        else
          typeinfer_json_append(buf, len, cap, "%c", *p);
        break;
      }
    }
  }
  typeinfer_json_append(buf, len, cap, "\"");
}

static size_t typeinfer_estimate_stmt_vars(stmt_t *s) {
  if (!s)
    return 0;
  size_t n = 0;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      n += typeinfer_estimate_stmt_vars(s->as.block.body.data[i]);
    break;
  case NY_S_VAR:
    n += s->as.var.names.len;
    break;
  case NY_S_FUNC:
    n += s->as.fn.params.len + typeinfer_estimate_stmt_vars(s->as.fn.body);
    break;
  case NY_S_IF:
    n += typeinfer_estimate_stmt_vars(s->as.iff.init);
    n += typeinfer_estimate_stmt_vars(s->as.iff.conseq);
    n += typeinfer_estimate_stmt_vars(s->as.iff.alt);
    break;
  case NY_S_GUARD:
    n += 1 + typeinfer_estimate_stmt_vars(s->as.guard.fallback);
    break;
  case NY_S_WHILE:
    n += typeinfer_estimate_stmt_vars(s->as.whl.init);
    n += typeinfer_estimate_stmt_vars(s->as.whl.body);
    n += typeinfer_estimate_stmt_vars(s->as.whl.update);
    break;
  case NY_S_FOR:
    n += s->as.fr.iter_var ? 1 : 0;
    n += s->as.fr.iter_index_var ? 1 : 0;
    n += typeinfer_estimate_stmt_vars(s->as.fr.init);
    n += typeinfer_estimate_stmt_vars(s->as.fr.body);
    n += typeinfer_estimate_stmt_vars(s->as.fr.update);
    break;
  case NY_S_TRY:
    n += typeinfer_estimate_stmt_vars(s->as.tr.body);
    n += typeinfer_estimate_stmt_vars(s->as.tr.handler);
    break;
  case NY_S_MATCH:
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      n += typeinfer_estimate_stmt_vars(s->as.match.arms.data[i].conseq);
    n += typeinfer_estimate_stmt_vars(s->as.match.default_conseq);
    break;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      n += typeinfer_estimate_stmt_vars(s->as.module.body.data[i]);
    break;
  case NY_S_IMPL:
    for (size_t i = 0; i < s->as.impl.methods.len; ++i)
      n += typeinfer_estimate_stmt_vars(s->as.impl.methods.data[i]);
    break;
  case NY_S_LAYOUT:
    for (size_t i = 0; i < s->as.layout.methods.len; ++i)
      n += typeinfer_estimate_stmt_vars(s->as.layout.methods.data[i]);
    break;
  case NY_S_STRUCT:
    for (size_t i = 0; i < s->as.struc.methods.len; ++i)
      n += typeinfer_estimate_stmt_vars(s->as.struc.methods.data[i]);
    break;
  default:
    break;
  }
  return n;
}

static const char *typeinfer_summary_var_type(typeinfer_ctx_t *ctx, size_t i) {
  if (!ctx || i >= ctx->var_names_len)
    return "unknown";
  if (ctx->vars[i].is_used_in_dynamic)
    return "dynamic";
  if (ctx->vars[i].is_i64_proven)
    return "int";
  if (ctx->vars[i].is_f64_proven)
    return "f64";
  return "any";
}

static bool typeinfer_stmt_in_scope(stmt_t *s, const char *source_name, bool include_std) {
  if (!s)
    return false;
  bool is_std = ny_is_stdlib_tok(s->tok);
  if (include_std)
    return true;
  if (is_std)
    return false;
  if (!source_name || !*source_name || !s->tok.filename || s->tok.filename[0] == '<')
    return true;
  return strcmp(s->tok.filename, source_name) == 0;
}

static void typeinfer_emit_function_summary(char **buf, size_t *len, size_t *cap, stmt_t *fn,
                                            const char *owner, bool *first) {
  if (!fn || fn->kind != NY_S_FUNC)
    return;
  size_t max_vars = typeinfer_estimate_stmt_vars(fn);
  if (max_vars < 64)
    max_vars = 64;
  max_vars += 32;
  typeinfer_ctx_t ctx = {0};
  typeinfer_ctx_init(&ctx, max_vars, NULL, NULL);
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    param_t *p = &fn->as.fn.params.data[i];
    if (!p->name)
      continue;
    typeinfer_add_var(&ctx, p->name);
    if (p->type && strcmp(p->type, "int") == 0)
      typeinfer_mark_i64(&ctx, p->name);
    else if (p->type && strcmp(p->type, "f64") == 0)
      typeinfer_mark_f64(&ctx, p->name);
    else if (!p->type)
      typeinfer_mark_dynamic(&ctx, p->name);
  }
  typeinfer_func_body(&ctx, fn->as.fn.body);
  if (!*first)
    typeinfer_json_append(buf, len, cap, ",");
  *first = false;
  typeinfer_json_append(buf, len, cap, "{\"name\":");
  typeinfer_json_str(buf, len, cap, fn->as.fn.name ? fn->as.fn.name : "");
  typeinfer_json_append(buf, len, cap, ",\"owner\":");
  typeinfer_json_str(buf, len, cap, owner ? owner : "");
  typeinfer_json_append(buf, len, cap, ",\"return_decl\":");
  typeinfer_json_str(buf, len, cap, fn->as.fn.return_type ? fn->as.fn.return_type : "");
  typeinfer_json_append(buf, len, cap,
                        ",\"formal_hm\":{\"arena_nodes\":%zu,\"unify_errors\":%zu}",
                        ctx.type_arena.nodes_allocated, ctx.type_unify_errors);
  typeinfer_json_append(buf, len, cap, ",\"params\":[");
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    if (i)
      typeinfer_json_append(buf, len, cap, ",");
    param_t *p = &fn->as.fn.params.data[i];
    typeinfer_json_append(buf, len, cap, "{\"name\":");
    typeinfer_json_str(buf, len, cap, p->name ? p->name : "");
    typeinfer_json_append(buf, len, cap, ",\"type\":");
    typeinfer_json_str(buf, len, cap, p->type ? p->type : "");
    typeinfer_json_append(buf, len, cap, "}");
  }
  typeinfer_json_append(buf, len, cap, "],\"vars\":[");
  for (size_t i = 0; i < ctx.var_names_len; ++i) {
    if (i)
      typeinfer_json_append(buf, len, cap, ",");
    typeinfer_json_append(buf, len, cap, "{\"name\":");
    typeinfer_json_str(buf, len, cap, ctx.vars[i].name ? ctx.vars[i].name : "");
    char *formal_type = ctx.vars[i].type ? ny_type_to_string(ctx.vars[i].type)
                                         : ny_strdup("any");
    typeinfer_json_append(buf, len, cap,
                          ",\"type\":\"%s\",\"formal_type\":",
                          typeinfer_summary_var_type(&ctx, i));
    typeinfer_json_str(buf, len, cap, formal_type ? formal_type : "any");
    free(formal_type);
    typeinfer_json_append(buf, len, cap,
                          ",\"i64\":%s,\"f64\":%s,\"dynamic\":%s,\"escapes\":%s}",
                          ctx.vars[i].is_i64_proven ? "true" : "false",
                          ctx.vars[i].is_f64_proven ? "true" : "false",
                          ctx.vars[i].is_used_in_dynamic ? "true" : "false",
                          ctx.vars[i].escapes ? "true" : "false");
  }
  typeinfer_json_append(buf, len, cap, "]}");
  typeinfer_ctx_dispose(&ctx);
}

static void typeinfer_emit_stmt_summaries(char **buf, size_t *len, size_t *cap, stmt_t *s,
                                          const char *owner, const char *source_name,
                                          bool include_std, bool *first) {
  if (!s)
    return;
  switch (s->kind) {
  case NY_S_FUNC:
    if (typeinfer_stmt_in_scope(s, source_name, include_std))
      typeinfer_emit_function_summary(buf, len, cap, s, owner, first);
    break;
  case NY_S_MODULE:
    if (!typeinfer_stmt_in_scope(s, source_name, include_std))
      break;
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      typeinfer_emit_stmt_summaries(buf, len, cap, s->as.module.body.data[i],
                                    s->as.module.name, source_name, include_std, first);
    break;
  case NY_S_IMPL:
    if (!typeinfer_stmt_in_scope(s, source_name, include_std))
      break;
    for (size_t i = 0; i < s->as.impl.methods.len; ++i)
      typeinfer_emit_stmt_summaries(buf, len, cap, s->as.impl.methods.data[i],
                                    s->as.impl.type_name, source_name, include_std, first);
    break;
  case NY_S_LAYOUT:
    if (!typeinfer_stmt_in_scope(s, source_name, include_std))
      break;
    for (size_t i = 0; i < s->as.layout.methods.len; ++i)
      typeinfer_emit_stmt_summaries(buf, len, cap, s->as.layout.methods.data[i],
                                    s->as.layout.name, source_name, include_std, first);
    break;
  case NY_S_STRUCT:
    if (!typeinfer_stmt_in_scope(s, source_name, include_std))
      break;
    for (size_t i = 0; i < s->as.struc.methods.len; ++i)
      typeinfer_emit_stmt_summaries(buf, len, cap, s->as.struc.methods.data[i],
                                    s->as.struc.name, source_name, include_std, first);
    break;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      typeinfer_emit_stmt_summaries(buf, len, cap, s->as.block.body.data[i], owner,
                                    source_name, include_std, first);
    break;
  default:
    break;
  }
}

char *typeinfer_program_summary_json(program_t *prog, const char *source_name, bool include_std) {
  char *buf = NULL;
  size_t len = 0, cap = 0;
  typeinfer_json_append(&buf, &len, &cap,
                        "{\"engine\":\"lightweight-flow-types\",\"schema\":\"typed_ast.v1\","
                        "\"notes\":[\"facts reflect current compiler inference, not full HM\"],"
                        "\"functions\":[");
  bool first = true;
  if (prog) {
    for (size_t i = 0; i < prog->body.len; ++i)
      typeinfer_emit_stmt_summaries(&buf, &len, &cap, prog->body.data[i], NULL, source_name,
                                    include_std, &first);
  }
  typeinfer_json_append(&buf, &len, &cap, "]}");
  if (!buf)
    return ny_strdup("{\"engine\":\"lightweight-flow-types\",\"functions\":[]}");
  return buf;
}
