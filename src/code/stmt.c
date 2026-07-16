#include "base/util.h"

#include "llvm.h"
#include "nullnarrow.h"
#include "priv.h"
#include <inttypes.h>
#include <llvm-c/Core.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

typedef struct {
  uint64_t count;
  double total_ms;
  double self_ms;
} ny_codegen_stmt_kind_prof_t;

static ny_codegen_stmt_kind_prof_t g_stmt_kind_prof[64];
static double g_stmt_kind_child_ms[4096];
static int g_stmt_kind_depth;
static int g_stmt_kind_profile_enabled = -1;
static int g_stmt_kind_profile_registered;

static bool ny_codegen_stmt_kind_profile_enabled(void) {
  if (g_stmt_kind_profile_enabled < 0) {
    const char *env = getenv("NYTRIX_PROFILE_CODEGEN_KINDS");
    if (!env || !*env)
      env = getenv("NYTRIX_PROFILE_CODEGEN_STMT");
    g_stmt_kind_profile_enabled =
        (env && *env && strcmp(env, "0") != 0 && strcmp(env, "false") != 0 &&
         strcmp(env, "off") != 0)
            ? 1
            : 0;
  }
  return g_stmt_kind_profile_enabled == 1;
}

static const char *ny_stmt_kind_profile_name(int kind) {
  static const char *names[] = {
      [NY_S_BLOCK] = "BLOCK",       [NY_S_USE] = "USE",
      [NY_S_VAR] = "VAR",           [NY_S_EXPR] = "EXPR",
      [NY_S_IF] = "IF",             [NY_S_GUARD] = "GUARD",
      [NY_S_WHILE] = "WHILE",       [NY_S_FOR] = "FOR",
      [NY_S_TRY] = "TRY",           [NY_S_FUNC] = "FUNC",
      [NY_S_EXTERN] = "EXTERN",     [NY_S_LINK] = "LINK",
      [NY_S_RETURN] = "RETURN",     [NY_S_LABEL] = "LABEL",
      [NY_S_DEFER] = "DEFER",       [NY_S_GOTO] = "GOTO",
      [NY_S_BREAK] = "BREAK",       [NY_S_CONTINUE] = "CONTINUE",
      [NY_S_LAYOUT] = "LAYOUT",     [NY_S_MATCH] = "MATCH",
      [NY_S_MODULE] = "MODULE",     [NY_S_EXPORT] = "EXPORT",
      [NY_S_STRUCT] = "STRUCT",     [NY_S_ENUM] = "ENUM",
      [NY_S_MACRO] = "MACRO",       [NY_S_INCLUDE] = "INCLUDE",
      [NY_S_DEFINE] = "DEFINE",     [NY_S_OPERATOR] = "OPERATOR",
      [NY_S_IMPL] = "IMPL",
  };
  return kind >= 0 && (size_t)kind < sizeof(names) / sizeof(names[0]) &&
                 names[kind]
             ? names[kind]
             : "UNKNOWN";
}

static void ny_codegen_stmt_kind_profile_report(void) {
  if (!ny_codegen_stmt_kind_profile_enabled())
    return;
  for (int i = 0;
       i < (int)(sizeof(g_stmt_kind_prof) / sizeof(g_stmt_kind_prof[0])); ++i) {
    ny_codegen_stmt_kind_prof_t p = g_stmt_kind_prof[i];
    if (p.count == 0)
      continue;
    fprintf(stderr,
            "[codegen-stmt-kind] kind=%s count=%" PRIu64
            " total_ms=%.3f self_ms=%.3f avg_us=%.3f\n",
            ny_stmt_kind_profile_name(i), p.count, p.total_ms, p.self_ms,
            (p.total_ms * 1000.0) / (double)p.count);
  }
}

static void ny_codegen_stmt_kind_profile_add(int kind, double total_ms,
                                             double child_ms) {
  if (kind < 0 ||
      kind >= (int)(sizeof(g_stmt_kind_prof) / sizeof(g_stmt_kind_prof[0])))
    kind = (int)(sizeof(g_stmt_kind_prof) / sizeof(g_stmt_kind_prof[0])) - 1;
  double self_ms = total_ms - child_ms;
  if (self_ms < 0.0)
    self_ms = 0.0;
  g_stmt_kind_prof[kind].count++;
  g_stmt_kind_prof[kind].total_ms += total_ms;
  g_stmt_kind_prof[kind].self_ms += self_ms;
}

static bool can_bind_decl_direct(const codegen_t *cg, const char *name,
                                 bool is_mut);
static void stmt_ownership_check_live_borrows(codegen_t *cg, scope *scopes,
                                              size_t depth, binding *source,
                                              token_t tok, const char *action);
static expr_t *stmt_ownership_return_borrow_arg(codegen_t *cg,
                                                expr_t *call_expr);
static expr_t *stmt_ownership_releases_arg(codegen_t *cg, expr_t *call_expr);
static expr_t *stmt_ownership_forgets_arg(codegen_t *cg, expr_t *call_expr);
static expr_t *stmt_ownership_consumes_arg(codegen_t *cg, expr_t *call_expr);
static bool stmt_ownership_binding_is_immediate(binding *b);
static bool stmt_expr_is_mutating_name(const char *name);
static bool stmt_expr_is_int_list_literal(codegen_t *cg, scope *scopes,
                                          size_t depth, expr_t *e);
static bool stmt_expr_int_range(codegen_t *cg, scope *scopes, size_t depth,
                                expr_t *e, int64_t *out_min,
                                int64_t *out_max);

static binding *stmt_lookup_binding(codegen_t *cg, scope *scopes, size_t depth,
                                    const char *name, size_t name_len,
                                    uint64_t hash) {
  return lookup_binding_hash(cg, scopes, depth, name, name_len, hash);
}

static binding *stmt_lookup_binding_no_mark(scope *scopes, size_t depth,
                                            const char *name, size_t name_len,
                                            uint64_t hash) {
  return lookup_binding_hash_no_mark(scopes, depth, name, name_len, hash);
}

static bool stmt_call_builtin_name_shadowed(codegen_t *cg, scope *scopes,
                                            size_t depth, expr_t *callee) {
  return ny_call_builtin_name_shadowed(cg, scopes, depth, callee);
}

static bool stmt_expr_is_list_ctor(expr_t *e) {
  if (!e || e->kind != NY_E_CALL || !e->as.call.callee ||
      e->as.call.callee->kind != NY_E_IDENT ||
      !e->as.call.callee->as.ident.name)
    return false;
  const char *n = e->as.call.callee->as.ident.name;
  return ny_name_tail_is(n, "list");
}

static bool stmt_expr_is_dict_ctor(expr_t *e) {
  if (!e || e->kind != NY_E_CALL || !e->as.call.callee ||
      e->as.call.callee->kind != NY_E_IDENT ||
      !e->as.call.callee->as.ident.name)
    return false;
  const char *n = e->as.call.callee->as.ident.name;
  return ny_name_tail_is(n, "dict");
}

static const char *stmt_call_tail_name(expr_t *e) {
  if (!e || e->kind != NY_E_CALL || !e->as.call.callee ||
      e->as.call.callee->kind != NY_E_IDENT ||
      !e->as.call.callee->as.ident.name)
    return NULL;
  const char *n = e->as.call.callee->as.ident.name;
  const char *dot = strrchr(n, '.');
  return dot ? dot + 1 : n;
}

static bool stmt_call_tail_is(expr_t *e, const char *tail) {
  const char *n = stmt_call_tail_name(e);
  return n && tail && strcmp(n, tail) == 0;
}

static expr_t *stmt_ownership_unary_arg(expr_t *e, const char *name) {
  if (!stmt_call_tail_is(e, name) || e->as.call.args.len != 1)
    return NULL;
  return e->as.call.args.data[0].val;
}

static void stmt_ownership_check_live_borrows(codegen_t *cg, scope *scopes,
                                              size_t depth, binding *source,
                                              token_t tok, const char *action);

static bool stmt_expr_is_adt_ctor(codegen_t *cg, expr_t *e) {
  char *name = ny_adt_member_call_full_name(cg, e);
  if (!name)
    return false;
  enum_def_t *owner = NULL;
  enum_member_def_t *mem = lookup_enum_member_owner(cg, name, &owner);
  return mem && owner && mem->has_payload;
}

static void stmt_ownership_diag(codegen_t *cg, token_t tok, const char *fmt,
                                ...) {
  va_list ap;
  va_start(ap, fmt);
  char msg[512];
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  if (cg && cg->ownership_strict && !ny_is_stdlib_tok(tok)) {
    ny_diag_error(tok, "%s", msg);
    cg->had_error = 1;
  } else {
    ny_diag_warning(tok, "%s", msg);
  }
}

static binding *stmt_ownership_ident_binding(codegen_t *cg, scope *scopes,
                                             size_t depth, expr_t *e) {
  if (!cg || !scopes || !e || e->kind != NY_E_IDENT || !e->as.ident.name)
    return NULL;
  size_t len = (size_t)e->tok.len;
  if (len == 0)
    len = strlen(e->as.ident.name);
  return stmt_lookup_binding(cg, scopes, depth, e->as.ident.name, len,
                             e->as.ident.hash);
}

static binding *stmt_ownership_root_binding(codegen_t *cg, scope *scopes,
                                            size_t depth, expr_t *e) {
  if (!e)
    return NULL;
  switch (e->kind) {
  case NY_E_IDENT:
    return stmt_ownership_ident_binding(cg, scopes, depth, e);
  case NY_E_INDEX:
    return stmt_ownership_root_binding(cg, scopes, depth, e->as.index.target);
  case NY_E_MEMBER:
    return stmt_ownership_root_binding(cg, scopes, depth, e->as.member.target);
  case NY_E_DEREF:
    return stmt_ownership_root_binding(cg, scopes, depth, e->as.deref.target);
  case NY_E_TRY:
    return stmt_ownership_root_binding(cg, scopes, depth,
                                       e->as.try_expr.target);
  case NY_E_UNARY:
    return stmt_ownership_root_binding(cg, scopes, depth, e->as.unary.right);
  case NY_E_CALL: {
    expr_t *contract_arg = stmt_ownership_return_borrow_arg(cg, e);
    if (!contract_arg)
      contract_arg = stmt_ownership_consumes_arg(cg, e);
    if (!contract_arg)
      contract_arg = stmt_ownership_releases_arg(cg, e);
    if (!contract_arg)
      contract_arg = stmt_ownership_forgets_arg(cg, e);
    if (contract_arg)
      return stmt_ownership_root_binding(cg, scopes, depth, contract_arg);
    const char *helper = stmt_call_tail_name(e);
    if ((helper &&
         (strcmp(helper, "borrow") == 0 || strcmp(helper, "own") == 0 ||
          strcmp(helper, "release") == 0 || strcmp(helper, "forget") == 0)) &&
        e->as.call.args.len == 1)
      return stmt_ownership_root_binding(cg, scopes, depth,
                                         e->as.call.args.data[0].val);
    return NULL;
  }
  case NY_E_MEMCALL:
    if (stmt_expr_is_mutating_name(e->as.memcall.name))
      return stmt_ownership_root_binding(cg, scopes, depth, e->as.memcall.target);
    return NULL;
  default:
    return NULL;
  }
}

static binding *stmt_ownership_returned_borrow_binding(codegen_t *cg,
                                                       scope *scopes,
                                                       size_t depth,
                                                       expr_t *e) {
  if (!e)
    return NULL;
  expr_t *borrow_arg = stmt_ownership_return_borrow_arg(cg, e);
  if (!borrow_arg)
    borrow_arg = stmt_ownership_unary_arg(e, "borrow");
  if (borrow_arg)
    return stmt_ownership_root_binding(cg, scopes, depth, borrow_arg);
  if (e->kind == NY_E_IDENT) {
    binding *b = stmt_ownership_ident_binding(cg, scopes, depth, e);
    if (stmt_ownership_binding_is_immediate(b))
      return NULL;
    if (b && b->ownership_borrow_source && *b->ownership_borrow_source)
      return stmt_lookup_binding(cg, scopes, depth, b->ownership_borrow_source,
                                 strlen(b->ownership_borrow_source),
                                 b->ownership_borrow_source_hash);
  }
  return NULL;
}

static void stmt_ownership_check_returned_borrow(codegen_t *cg, scope *scopes,
                                                 size_t depth, expr_t *e) {
  if (!cg || !cg->ownership_strict || !e)
    return;
  binding *src = stmt_ownership_returned_borrow_binding(cg, scopes, depth, e);
  if (!src || !src->ownership_tracked)
    return;
  const char *allowed = cg->current_fn_returns_borrow;
  if (allowed && *allowed && strcmp(allowed, src->name) == 0)
    return;
  stmt_ownership_diag(
      cg, e->tok, "returning borrow of local owner '%s' would outlive its slot",
      src->name);
  ny_diag_fix(
      "return an owned value, clone(%s), or annotate a parameter borrow with "
      "@returns_borrow(name)",
      src->name);
}

static bool stmt_sig_contract_has(const ny_str_list *list, const char *name) {
  if (!list || !name)
    return false;
  for (size_t i = 0; i < list->len; i++) {
    if (list->data[i] && strcmp(list->data[i], name) == 0)
      return true;
  }
  return false;
}

static bool stmt_ownership_type_is_immediate(const char *type_name) {
  if (!type_name || !*type_name)
    return false;
  const char *leaf = strrchr(type_name, '.');
  leaf = leaf ? leaf + 1 : type_name;
  if (strcmp(leaf, "ptr") == 0 || leaf[0] == '*')
    return false;
  return ny_is_native_abi_type_name(leaf) || strcmp(leaf, "bool") == 0 ||
         strcmp(leaf, "char") == 0;
}

static bool stmt_ownership_binding_is_immediate(binding *b) {
  if (!b)
    return false;
  return b->is_int_slot || b->is_int_direct || b->is_f64_slot ||
         b->is_f64_direct || b->is_f32_slot || b->is_f32_direct ||
         stmt_ownership_type_is_immediate(b->type_name);
}

static bool stmt_ownership_borrow_expr_is_immediate(codegen_t *cg,
                                                    scope *scopes, size_t depth,
                                                    expr_t *e) {
  const char *borrow_type = infer_expr_type(cg, scopes, depth, e);
  if (stmt_ownership_type_is_immediate(borrow_type) ||
      ny_is_proven_int(cg, scopes, depth, e, NULL))
    return true;
  if (e && e->kind == NY_E_INDEX && e->as.index.target &&
      e->as.index.target->kind == NY_E_IDENT) {
    binding *target =
        stmt_ownership_ident_binding(cg, scopes, depth, e->as.index.target);
    expr_t *init = target && !target->is_mut
                       ? ny_binding_var_init_expr(
                             target, e->as.index.target->as.ident.name)
                       : NULL;
    if (stmt_expr_is_int_list_literal(cg, scopes, depth, init))
      return true;
  }
  return false;
}

static const char *stmt_sig_param_name(fun_sig *sig, size_t idx) {
  if (!sig || !sig->stmt_t || sig->stmt_t->kind != NY_S_FUNC)
    return NULL;
  if (idx >= sig->stmt_t->as.fn.params.len)
    return NULL;
  return sig->stmt_t->as.fn.params.data[idx].name;
}

static fun_sig *stmt_ownership_resolve_call_sig(codegen_t *cg,
                                                expr_t *call_expr) {
  if (!cg || !call_expr || call_expr->kind != NY_E_CALL ||
      !call_expr->as.call.callee ||
      call_expr->as.call.callee->kind != NY_E_IDENT)
    return NULL;
  return resolve_overload(cg, call_expr->as.call.callee->as.ident.name,
                          call_expr->as.call.args.len,
                          call_expr->as.call.callee->as.ident.hash);
}

static expr_t *stmt_ownership_arg_for_contract(codegen_t *cg, expr_t *call_expr,
                                               const ny_str_list *contracts) {
  fun_sig *sig = stmt_ownership_resolve_call_sig(cg, call_expr);
  if (!sig || !contracts || contracts->len == 0)
    return NULL;
  for (size_t i = 0; i < call_expr->as.call.args.len; i++) {
    call_arg_t *arg = &call_expr->as.call.args.data[i];
    const char *pname = arg->name ? arg->name : stmt_sig_param_name(sig, i);
    if (stmt_sig_contract_has(contracts, pname))
      return arg->val;
  }
  return NULL;
}

static expr_t *stmt_ownership_return_borrow_arg(codegen_t *cg,
                                                expr_t *call_expr) {
  fun_sig *sig = stmt_ownership_resolve_call_sig(cg, call_expr);
  if (!sig || !sig->returns_borrow)
    return NULL;
  for (size_t i = 0; i < call_expr->as.call.args.len; i++) {
    call_arg_t *arg = &call_expr->as.call.args.data[i];
    const char *pname = arg->name ? arg->name : stmt_sig_param_name(sig, i);
    if (pname && strcmp(pname, sig->returns_borrow) == 0)
      return arg->val;
  }
  return NULL;
}

static expr_t *stmt_ownership_releases_arg(codegen_t *cg, expr_t *call_expr) {
  fun_sig *sig = stmt_ownership_resolve_call_sig(cg, call_expr);
  expr_t *arg =
      sig ? stmt_ownership_arg_for_contract(cg, call_expr, &sig->releases)
          : NULL;
  if (arg)
    return arg;
  return stmt_ownership_unary_arg(call_expr, "release");
}

static expr_t *stmt_ownership_forgets_arg(codegen_t *cg, expr_t *call_expr) {
  fun_sig *sig = stmt_ownership_resolve_call_sig(cg, call_expr);
  expr_t *arg =
      sig ? stmt_ownership_arg_for_contract(cg, call_expr, &sig->forgets)
          : NULL;
  if (arg)
    return arg;
  return stmt_ownership_unary_arg(call_expr, "forget");
}

static expr_t *stmt_ownership_consumes_arg(codegen_t *cg, expr_t *call_expr) {
  fun_sig *sig = stmt_ownership_resolve_call_sig(cg, call_expr);
  return sig ? stmt_ownership_arg_for_contract(cg, call_expr, &sig->consumes)
             : NULL;
}

static void stmt_ownership_apply_call_contracts(codegen_t *cg, scope *scopes,
                                                size_t depth,
                                                expr_t *call_expr) {
  if (!cg || !cg->ownership_enabled || !call_expr ||
      call_expr->kind != NY_E_CALL || !call_expr->as.call.callee ||
      call_expr->as.call.callee->kind != NY_E_IDENT)
    return;
  fun_sig *sig = stmt_ownership_resolve_call_sig(cg, call_expr);
  if (!sig)
    return;
  for (size_t i = 0; i < call_expr->as.call.args.len; i++) {
    call_arg_t *arg = &call_expr->as.call.args.data[i];
    const char *pname = arg->name ? arg->name : stmt_sig_param_name(sig, i);
    if (!pname)
      continue;
    binding *root = stmt_ownership_root_binding(cg, scopes, depth, arg->val);
    if (!root || !root->ownership_tracked)
      continue;
    if (stmt_sig_contract_has(&sig->mutates, pname))
      stmt_ownership_check_live_borrows(cg, scopes, depth, root, arg->val->tok,
                                        "mutate");
    if (stmt_sig_contract_has(&sig->consumes, pname)) {
      stmt_ownership_check_live_borrows(cg, scopes, depth, root, arg->val->tok,
                                        "move");
      root->owner_state = NY_OWNER_MOVED;
    }
  }
}

static bool stmt_ownership_expr_is_fresh_heap(codegen_t *cg, expr_t *e,
                                              bool *raw_ptr) {
  if (raw_ptr)
    *raw_ptr = false;
  if (!e)
    return false;
  if (e->kind == NY_E_LIST || e->kind == NY_E_DICT || e->kind == NY_E_SET) {
    return true;
  }
  if (stmt_expr_is_adt_ctor(cg, e))
    return true;
  if (e->kind != NY_E_CALL)
    return false;
  if (stmt_call_tail_is(e, "own")) {
    if (raw_ptr)
      *raw_ptr = false;
    return true;
  }
  const char *n = stmt_call_tail_name(e);
  if (!n)
    return false;
  if (strcmp(n, "malloc") == 0 || strcmp(n, "zalloc") == 0 ||
      strcmp(n, "realloc") == 0) {
    if (raw_ptr)
      *raw_ptr = true;
    return true;
  }
  if (strcmp(n, "list") == 0 || strcmp(n, "dict") == 0 ||
      strcmp(n, "set") == 0 || strcmp(n, "__list_new") == 0 ||
      strcmp(n, "__str_concat") == 0) {
    return true;
  }
  fun_sig *sig = lookup_fun(cg, e->as.call.callee->as.ident.name,
                            e->as.call.callee->as.ident.hash);
  return sig && sig->returns_owned;
}

static bool stmt_ownership_alloc_size_bytes(codegen_t *cg, scope *scopes,
                                            size_t depth, expr_t *rhs,
                                            int64_t *out_size) {
  if (!rhs || rhs->kind != NY_E_CALL || !rhs->as.call.callee ||
      rhs->as.call.callee->kind != NY_E_IDENT)
    return false;
  const char *n = stmt_call_tail_name(rhs);
  if (!n)
    return false;
  size_t arg_idx = SIZE_MAX;
  if ((strcmp(n, "malloc") == 0 || strcmp(n, "zalloc") == 0) &&
      rhs->as.call.args.len >= 1) {
    arg_idx = 0;
  } else if (strcmp(n, "realloc") == 0 && rhs->as.call.args.len >= 2) {
    arg_idx = 1;
  }
  if (arg_idx == SIZE_MAX)
    return false;
  expr_t *size_expr = rhs->as.call.args.data[arg_idx].val;
  int64_t lo = 0, hi = 0;
  if (!stmt_expr_int_range(cg, scopes, depth, size_expr, &lo, &hi) ||
      lo != hi || lo < 0)
    return false;
  if (out_size)
    *out_size = lo;
  return true;
}

static void stmt_ownership_warn_use_after_move(codegen_t *cg, scope *scopes,
                                               size_t depth, expr_t *e) {
  if (!cg || !cg->ownership_enabled || !e || cg->had_error)
    return;
  switch (e->kind) {
  case NY_E_IDENT: {
    binding *b = stmt_ownership_ident_binding(cg, scopes, depth, e);
    if (b && b->ownership_tracked && b->owner_state == NY_OWNER_MOVED &&
        ny_diag_should_emit("ownership_use_after_move", e->tok, b->name)) {
      stmt_ownership_diag(cg, e->tok, "use after move of owned slot '%s'",
                          b->name);
      ny_diag_fix("use borrow(%s) before the move, clone(%s), or assign a new "
                  "owned value",
                  b->name, b->name);
    }
    break;
  }
  case NY_E_UNARY:
    stmt_ownership_warn_use_after_move(cg, scopes, depth, e->as.unary.right);
    break;
  case NY_E_BINARY:
    stmt_ownership_warn_use_after_move(cg, scopes, depth, e->as.binary.left);
    stmt_ownership_warn_use_after_move(cg, scopes, depth, e->as.binary.right);
    break;
  case NY_E_LOGICAL:
    stmt_ownership_warn_use_after_move(cg, scopes, depth, e->as.logical.left);
    stmt_ownership_warn_use_after_move(cg, scopes, depth, e->as.logical.right);
    break;
  case NY_E_TERNARY:
    stmt_ownership_warn_use_after_move(cg, scopes, depth, e->as.ternary.cond);
    stmt_ownership_warn_use_after_move(cg, scopes, depth,
                                       e->as.ternary.true_expr);
    stmt_ownership_warn_use_after_move(cg, scopes, depth,
                                       e->as.ternary.false_expr);
    break;
  case NY_E_CALL:
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      stmt_ownership_warn_use_after_move(cg, scopes, depth,
                                         e->as.call.args.data[i].val);
    break;
  case NY_E_MEMCALL:
    stmt_ownership_warn_use_after_move(cg, scopes, depth, e->as.memcall.target);
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      stmt_ownership_warn_use_after_move(cg, scopes, depth,
                                         e->as.memcall.args.data[i].val);
    break;
  case NY_E_INDEX:
    stmt_ownership_warn_use_after_move(cg, scopes, depth, e->as.index.target);
    stmt_ownership_warn_use_after_move(cg, scopes, depth, e->as.index.start);
    stmt_ownership_warn_use_after_move(cg, scopes, depth, e->as.index.stop);
    stmt_ownership_warn_use_after_move(cg, scopes, depth, e->as.index.step);
    break;
  case NY_E_MEMBER:
    stmt_ownership_warn_use_after_move(cg, scopes, depth, e->as.member.target);
    break;
  default:
    break;
  }
}

static void stmt_ownership_emit_drop(codegen_t *cg, binding *b, token_t tok) {
  if (!cg || !cg->ownership_enabled || !b || !b->ownership_tracked ||
      b->owner_state != NY_OWNER_OWNED || b->ownership_forgotten)
    return;
  if (!cg->ownership_runtime_cleanup)
    return;
  if (!b->value)
    return;
  fun_sig *drop_sig = lookup_fun(cg, "__drop_owned", 0);
  if (!drop_sig) {
    stmt_ownership_diag(
        cg, tok, "ownership cleanup requires __drop_owned; import std.core");
    return;
  }
  LLVMValueRef v = b->is_slot ? LLVMBuildLoad2(cg->builder, cg->type_i64,
                                               b->value, "own.load")
                              : b->value;
  LLVMBuildCall2(cg->builder, drop_sig->type, drop_sig->value,
                 (LLVMValueRef[]){v}, 1, "own.drop");
  if (b->is_slot)
    ny_store(cg, b->value, ny_c0(cg));
}

static void stmt_ownership_register_slot_defer(codegen_t *cg, scope *scopes,
                                               size_t depth, binding *b) {
  if (!cg || !cg->ownership_enabled || !cg->ownership_runtime_cleanup ||
      !scopes || !b || !b->is_slot ||
      b->ownership_defer_registered)
    return;
  fun_sig *push_sig = lookup_fun(cg, "__push_defer", 0);
  fun_sig *drop_slot_sig = lookup_fun(cg, "__drop_owned_slot", 0);
  if (!push_sig || !drop_slot_sig)
    return;
  LLVMValueRef fn_ptr =
      ny_ptr2i64(cg, drop_slot_sig->value, "own.drop.slot.fn");
  LLVMValueRef env =
      LLVMBuildPtrToInt(cg->builder, b->value, cg->type_i64, "own.slot.env");
  LLVMBuildCall2(cg->builder, push_sig->type, push_sig->value,
                 (LLVMValueRef[]){fn_ptr, env}, 2, "");
  vec_push(&scopes[depth].defers, NULL);
  b->ownership_defer_registered = true;
}

static void stmt_ownership_cleanup_scope(codegen_t *cg, scope *scopes,
                                         size_t depth) {
  if (!cg || !cg->ownership_enabled || !cg->ownership_runtime_cleanup || !scopes)
    return;
  scope *sc = &scopes[depth];
  for (ssize_t i = (ssize_t)sc->vars.len - 1; i >= 0; --i) {
    binding *b = &sc->vars.data[i];
    if (b->ownership_tracked && b->owner_state == NY_OWNER_OWNED) {
      if (b->ownership_defer_registered)
        continue;
      stmt_ownership_emit_drop(cg, b,
                               b->stmt_t ? b->stmt_t->tok : (token_t){0});
    }
  }
}

static void stmt_ownership_clear_borrow(binding *b) {
  if (!b)
    return;
  b->ownership_borrow_source = NULL;
  b->ownership_borrow_source_hash = 0;
}

static bool stmt_ownership_is_live_borrow_of(binding *borrower,
                                             binding *source) {
  if (!borrower || !source || borrower == source ||
      !borrower->ownership_borrow_source || !source->name)
    return false;
  uint64_t source_hash =
      source->name_hash ? source->name_hash : ny_hash64_cstr(source->name);
  if (borrower->ownership_borrow_source_hash &&
      borrower->ownership_borrow_source_hash != source_hash)
    return false;
  return strcmp(borrower->ownership_borrow_source, source->name) == 0;
}

static void stmt_ownership_check_live_borrows(codegen_t *cg, scope *scopes,
                                              size_t depth, binding *source,
                                              token_t tok, const char *action) {
  if (!cg || !cg->ownership_enabled || !scopes || !source || !source->name)
    return;
  for (size_t d = 0; d <= depth; ++d) {
    scope *sc = &scopes[d];
    for (size_t i = 0; i < sc->vars.len; ++i) {
      binding *borrower = &sc->vars.data[i];
      if (!stmt_ownership_is_live_borrow_of(borrower, source))
        continue;
      if (!ny_diag_should_emit("ownership_live_borrow", tok, source->name))
        continue;
      stmt_ownership_diag(
          cg, tok, "cannot %s owned slot '%s' while borrow '%s' is live",
          action ? action : "change", source->name, borrower->name);
      ny_diag_fix(
          "end the borrow scope first, clone(%s), or keep passing borrow(%s)",
          source->name, source->name);
    }
  }
}

static bool stmt_ownership_same_source(binding *dest, expr_t *rhs,
                                       codegen_t *cg, scope *scopes,
                                       size_t depth) {
  expr_t *src = rhs;
  expr_t *borrow_arg = stmt_ownership_return_borrow_arg(cg, rhs);
  expr_t *consumed_arg = stmt_ownership_consumes_arg(cg, rhs);
  if (borrow_arg || consumed_arg)
    src = borrow_arg ? borrow_arg : consumed_arg;
  else if (rhs && rhs->kind == NY_E_MEMCALL &&
           stmt_expr_is_mutating_name(rhs->as.memcall.name))
    src = rhs->as.memcall.target;
  else if (stmt_call_tail_is(rhs, "borrow") || stmt_call_tail_is(rhs, "own"))
    src = stmt_ownership_unary_arg(rhs, stmt_call_tail_name(rhs));
  return dest && src &&
         stmt_ownership_root_binding(cg, scopes, depth, src) == dest;
}

static void stmt_ownership_release_source(codegen_t *cg, scope *scopes,
                                          size_t depth, expr_t *arg,
                                          bool forgotten) {
  binding *b = stmt_ownership_ident_binding(cg, scopes, depth, arg);
  if (!b)
    return;
  if (b->ownership_tracked && b->owner_state == NY_OWNER_RELEASED &&
      ny_diag_should_emit("ownership_double_release", arg->tok, b->name)) {
    stmt_ownership_diag(cg, arg->tok, "double release of owned slot '%s'",
                        b->name);
    ny_diag_fix("remove one release(%s), or use borrow(%s) if the value is "
                "still needed",
                b->name, b->name);
  }
  stmt_ownership_check_live_borrows(cg, scopes, depth, b, arg->tok,
                                    forgotten ? "forget" : "release");
  b->ownership_tracked = true;
  b->ownership_forgotten = forgotten;
  b->ownership_alloc_size_known = false;
  b->ownership_alloc_size_raw = 0;
  b->owner_state = forgotten ? NY_OWNER_MOVED : NY_OWNER_RELEASED;
  stmt_ownership_clear_borrow(b);
  if (b->is_slot && cg->ownership_runtime_cleanup)
    ny_store(cg, b->value, ny_c0(cg));
}

static void stmt_ownership_pre_store(codegen_t *cg, scope *scopes, size_t depth,
                                     binding *dest, expr_t *rhs, token_t tok) {
  if (!cg || !cg->ownership_enabled || !dest || !dest->ownership_tracked ||
      dest->owner_state != NY_OWNER_OWNED)
    return;
  if (stmt_ownership_same_source(dest, rhs, cg, scopes, depth))
    return;
  stmt_ownership_check_live_borrows(cg, scopes, depth, dest, tok, "reassign");
  if (cg->ownership_runtime_cleanup &&
      ny_diag_should_emit("ownership_reassign_drop", tok, dest->name))
    ny_diag_warning(tok,
                    "reassigning owned slot '%s' drops its previous heap value",
                    dest->name);
  stmt_ownership_emit_drop(cg, dest, tok);
  dest->owner_state = NY_OWNER_RELEASED;
  dest->ownership_alloc_size_known = false;
  dest->ownership_alloc_size_raw = 0;
  stmt_ownership_clear_borrow(dest);
}

static void stmt_ownership_post_store(codegen_t *cg, scope *scopes,
                                      size_t depth, binding *dest, expr_t *rhs,
                                      token_t tok, bool target_global) {
  if (!cg || !cg->ownership_enabled || !dest || !rhs)
    return;
  stmt_ownership_warn_use_after_move(cg, scopes, depth, rhs);
  expr_t *rel = stmt_ownership_releases_arg(cg, rhs);
  expr_t *forget = stmt_ownership_forgets_arg(cg, rhs);
  if (rel || forget) {
    stmt_ownership_release_source(cg, scopes, depth, rel ? rel : forget,
                                  forget != NULL);
    dest->ownership_tracked = false;
    dest->ownership_alloc_size_known = false;
    dest->ownership_alloc_size_raw = 0;
    dest->owner_state = NY_OWNER_BORROWED;
    stmt_ownership_clear_borrow(dest);
    return;
  }
  expr_t *borrow_arg = stmt_ownership_return_borrow_arg(cg, rhs);
  if (!borrow_arg)
    borrow_arg = stmt_ownership_unary_arg(rhs, "borrow");
  if (borrow_arg) {
    binding *borrow_src =
        stmt_ownership_root_binding(cg, scopes, depth, borrow_arg);
    dest->ownership_tracked = false;
    dest->ownership_alloc_size_known = false;
    dest->ownership_alloc_size_raw = 0;
    dest->owner_state = NY_OWNER_BORROWED;
    if (stmt_ownership_borrow_expr_is_immediate(cg, scopes, depth,
                                                borrow_arg)) {
      stmt_ownership_clear_borrow(dest);
      return;
    }
    dest->ownership_borrow_source = borrow_src ? borrow_src->name : NULL;
    dest->ownership_borrow_source_hash =
        borrow_src ? (borrow_src->name_hash ? borrow_src->name_hash
                                            : ny_hash64_cstr(borrow_src->name))
                   : 0;
    return;
  }
  if (rhs->kind == NY_E_MEMCALL &&
      stmt_expr_is_mutating_name(rhs->as.memcall.name)) {
    binding *mut_src =
        stmt_ownership_root_binding(cg, scopes, depth, rhs->as.memcall.target);
    if (mut_src == dest && dest->ownership_tracked &&
        dest->owner_state == NY_OWNER_OWNED) {
      stmt_ownership_clear_borrow(dest);
      return;
    }
  }
  bool raw_ptr = false;
  bool fresh = stmt_ownership_expr_is_fresh_heap(cg, rhs, &raw_ptr);
  binding *src = stmt_ownership_ident_binding(cg, scopes, depth, rhs);
  expr_t *own_arg = stmt_ownership_consumes_arg(cg, rhs);
  if (!own_arg)
    own_arg = stmt_ownership_unary_arg(rhs, "own");
  if (!src && own_arg)
    src = stmt_ownership_ident_binding(cg, scopes, depth, own_arg);
  bool move = src && src != dest && src->ownership_tracked &&
              src->owner_state == NY_OWNER_OWNED;
  if (!fresh && !move) {
    dest->ownership_tracked = false;
    dest->ownership_alloc_size_known = false;
    dest->ownership_alloc_size_raw = 0;
    dest->owner_state = NY_OWNER_BORROWED;
    stmt_ownership_clear_borrow(dest);
    return;
  }
  bool alloc_size_known = false;
  int64_t alloc_size_raw = 0;
  if (move) {
    raw_ptr = src->ownership_raw_ptr;
    alloc_size_known = src->ownership_alloc_size_known;
    alloc_size_raw = src->ownership_alloc_size_raw;
  } else if (raw_ptr) {
    alloc_size_known =
        stmt_ownership_alloc_size_bytes(cg, scopes, depth, rhs, &alloc_size_raw);
  }
  bool explicit_own = stmt_call_tail_is(rhs, "own");
  if (target_global && !explicit_own &&
      ny_diag_should_emit("ownership_escape_global", tok, dest->name)) {
    stmt_ownership_diag(
        cg, tok,
        "owned heap value stored in global '%s' may escape ownership cleanup",
        dest->name);
    ny_diag_fix("wrap process-lifetime storage in own(...), or keep it local and "
                "release it");
  }
  dest->ownership_tracked = true;
  dest->ownership_raw_ptr = raw_ptr;
  dest->ownership_alloc_size_known = alloc_size_known;
  dest->ownership_alloc_size_raw = alloc_size_known ? alloc_size_raw : 0;
  dest->ownership_forgotten = false;
  dest->owner_state = NY_OWNER_OWNED;
  stmt_ownership_clear_borrow(dest);
  if (!target_global)
    stmt_ownership_register_slot_defer(cg, scopes, depth, dest);
  if (move) {
    stmt_ownership_check_live_borrows(cg, scopes, depth, src, tok, "move");
    src->owner_state = NY_OWNER_MOVED;
    src->ownership_forgotten = false;
    src->ownership_alloc_size_known = false;
    src->ownership_alloc_size_raw = 0;
    stmt_ownership_clear_borrow(src);
    if (src->is_slot && cg->ownership_runtime_cleanup)
      ny_store(cg, src->value, ny_c0(cg));
  }
}

static binding *
stmt_ownership_begin_return_transfer(codegen_t *cg, scope *scopes, size_t depth,
                                     expr_t *value,
                                     ny_owner_state_t *old_state) {
  if (old_state)
    *old_state = NY_OWNER_BORROWED;
  if (!cg || !cg->ownership_enabled || !value)
    return NULL;
  expr_t *src = value;
  if (stmt_ownership_return_borrow_arg(cg, value) ||
      stmt_call_tail_is(value, "borrow"))
    return NULL;
  expr_t *consumed_arg = stmt_ownership_consumes_arg(cg, value);
  if (consumed_arg)
    src = consumed_arg;
  else if (value->kind == NY_E_MEMCALL &&
           stmt_expr_is_mutating_name(value->as.memcall.name))
    src = value->as.memcall.target;
  else if (stmt_call_tail_is(value, "own"))
    src = stmt_ownership_unary_arg(value, "own");
  binding *b = stmt_ownership_root_binding(cg, scopes, depth, src);
  if (!b || !b->ownership_tracked || b->owner_state != NY_OWNER_OWNED)
    return NULL;
  stmt_ownership_check_live_borrows(cg, scopes, depth, b, value->tok, "return");
  if (cg->ownership_strict && !cg->current_fn_returns_owned) {
    stmt_ownership_diag(cg, value->tok,
                        "returning owned slot '%s' requires @returns_owned",
                        b->name);
    ny_diag_fix("add @returns_owned to the function, return borrow(%s), "
                "clone(%s), or release ownership before returning",
                b->name, b->name);
  }
  if (old_state)
    *old_state = b->owner_state;
  b->owner_state = NY_OWNER_MOVED;
  if (b->is_slot && b->value)
    ny_store(cg, b->value, ny_c0(cg));
  return b;
}

static void stmt_ownership_end_return_transfer(binding *b,
                                               ny_owner_state_t old_state) {
  if (b)
    b->owner_state = old_state;
}

static bool stmt_expr_is_int_list_literal(codegen_t *cg, scope *scopes,
                                          size_t depth, expr_t *e) {
  if (!e || e->kind != NY_E_LIST)
    return false;
  for (size_t i = 0; i < e->as.list_like.len; ++i) {
    if (!ny_is_proven_int(cg, scopes, depth, e->as.list_like.data[i], NULL))
      return false;
  }
  return true;
}

static bool stmt_str_in(const char *s, const char *const *vals, size_t n) {
  if (!s)
    return false;
  for (size_t i = 0; i < n; ++i)
    if (strcmp(s, vals[i]) == 0)
      return true;
  return false;
}

static bool stmt_type_name_is_float_value(const char *type_name) {
  static const char *const names[] = {"f64", "float", "f32"};
  return stmt_str_in(type_name, names, sizeof(names) / sizeof(names[0]));
}

static bool stmt_type_name_is_f64_value(const char *type_name) {
  static const char *const names[] = {"f64", "float"};
  return stmt_str_in(type_name, names, sizeof(names) / sizeof(names[0]));
}

static bool stmt_type_name_is_f32_value(const char *type_name) {
  return type_name && strcmp(type_name, "f32") == 0;
}

static bool stmt_type_name_is_int_value(const char *type_name) {
  return type_name && strcmp(type_name, "int") == 0;
}

static bool stmt_type_name_is_index_int_value(const char *type_name) {
  static const char *const names[] = {"int", "i8",  "i16", "i32", "i64",
                                      "u8",  "u16", "u32", "u64"};
  return stmt_str_in(type_name, names, sizeof(names) / sizeof(names[0]));
}

static bool stmt_type_name_is_narrow_fixed_int_value(const char *type_name) {
  static const char *const names[] = {"u8", "u16", "u32", "u64",
                                      "i8", "i16", "i32", "i64"};
  return stmt_str_in(type_name, names, sizeof(names) / sizeof(names[0]));
}

static bool stmt_expr_is_f64_value(codegen_t *cg, scope *scopes, size_t depth,
                                   expr_t *e) {
  if (!e)
    return false;
  if (e->kind == NY_E_LITERAL)
    return e->as.literal.kind == NY_LIT_FLOAT;
  if (e->kind == NY_E_IDENT && e->as.ident.name) {
    size_t name_len = (size_t)e->tok.len;
    if (name_len == 0)
      name_len = strlen(e->as.ident.name);
    binding *b = stmt_lookup_binding(cg, scopes, depth, e->as.ident.name,
                                     name_len, e->as.ident.hash);
    if (b && (b->is_f64_slot || b->is_f64_direct || b->is_f32_slot ||
              b->is_f32_direct))
      return true;
  }
  const char *t = infer_expr_type(cg, scopes, depth, e);
  return stmt_type_name_is_float_value(t);
}

static bool stmt_expr_is_f64_list_literal(codegen_t *cg, scope *scopes,
                                          size_t depth, expr_t *e) {
  if (!e || e->kind != NY_E_LIST)
    return false;
  for (size_t i = 0; i < e->as.list_like.len; ++i) {
    if (!stmt_expr_is_f64_value(cg, scopes, depth, e->as.list_like.data[i]))
      return false;
  }
  return true;
}

static bool stmt_iterable_yields_int(codegen_t *cg, scope *scopes, size_t depth,
                                     expr_t *iterable) {
  if (!iterable)
    return false;
  if (iterable->kind == NY_E_BINARY && iterable->as.binary.op &&
      strcmp(iterable->as.binary.op, "..") == 0)
    return true;
  if (iterable->kind == NY_E_CALL && iterable->as.call.callee &&
      iterable->as.call.callee->kind == NY_E_IDENT &&
      iterable->as.call.callee->as.ident.name &&
      (ny_name_tail_is(iterable->as.call.callee->as.ident.name, "range") ||
       ny_name_tail_is(iterable->as.call.callee->as.ident.name, "range2")))
    return true;
  const char *iter_type = infer_expr_type(cg, scopes, depth, iterable);
  if (ny_type_is(iter_type, "range"))
    return true;
  if (stmt_expr_is_int_list_literal(cg, scopes, depth, iterable))
    return true;
  if (iterable->kind == NY_E_IDENT && iterable->as.ident.name) {
    size_t name_len = (size_t)iterable->tok.len;
    if (name_len == 0)
      name_len = strlen(iterable->as.ident.name);
    binding *b = stmt_lookup_binding(cg, scopes, depth, iterable->as.ident.name,
                                     name_len, iterable->as.ident.hash);
    return b && b->is_int_list_storage;
  }
  return false;
}

static bool stmt_expr_is_static_list_builtin_target(expr_t *e,
                                                    const char *name) {
  return ny_expr_ident_is_name(e, name);
}

static bool stmt_param_list_shadows_name(const ny_param_list *params,
                                         const char *name) {
  if (!params || !name)
    return false;
  for (size_t i = 0; i < params->len; ++i) {
    if (params->data[i].name && strcmp(params->data[i].name, name) == 0)
      return true;
  }
  return false;
}

static bool stmt_contains_ident_name(stmt_t *s, const char *name);

static bool stmt_expr_is_int_index_expr(codegen_t *cg, scope *scopes,
                                        size_t depth, expr_t *e) {
  if (ny_expr_literal_i64(e, NULL))
    return true;
  const char *t = infer_expr_type(cg, scopes, depth, e);
  return stmt_type_name_is_index_int_value(t);
}

static bool stmt_expr_contains_ident_name(expr_t *e, const char *name) {
  if (!e || !name)
    return false;
  switch (e->kind) {
  case NY_E_IDENT:
    return ny_expr_ident_is_name(e, name);
  case NY_E_UNARY:
    return stmt_expr_contains_ident_name(e->as.unary.right, name);
  case NY_E_BINARY:
    return stmt_expr_contains_ident_name(e->as.binary.left, name) ||
           stmt_expr_contains_ident_name(e->as.binary.right, name);
  case NY_E_LOGICAL:
    return stmt_expr_contains_ident_name(e->as.logical.left, name) ||
           stmt_expr_contains_ident_name(e->as.logical.right, name);
  case NY_E_TERNARY:
    return stmt_expr_contains_ident_name(e->as.ternary.cond, name) ||
           stmt_expr_contains_ident_name(e->as.ternary.true_expr, name) ||
           stmt_expr_contains_ident_name(e->as.ternary.false_expr, name);
  case NY_E_CALL:
    if (stmt_expr_contains_ident_name(e->as.call.callee, name))
      return true;
    for (size_t i = 0; i < e->as.call.args.len; ++i) {
      if (stmt_expr_contains_ident_name(e->as.call.args.data[i].val, name))
        return true;
    }
    return false;
  case NY_E_MEMCALL:
    if (stmt_expr_contains_ident_name(e->as.memcall.target, name))
      return true;
    for (size_t i = 0; i < e->as.memcall.args.len; ++i) {
      if (stmt_expr_contains_ident_name(e->as.memcall.args.data[i].val, name))
        return true;
    }
    return false;
  case NY_E_INDEX:
    return stmt_expr_contains_ident_name(e->as.index.target, name) ||
           stmt_expr_contains_ident_name(e->as.index.start, name) ||
           stmt_expr_contains_ident_name(e->as.index.stop, name) ||
           stmt_expr_contains_ident_name(e->as.index.step, name);
  case NY_E_MEMBER:
    return stmt_expr_contains_ident_name(e->as.member.target, name);
  case NY_E_PTR_TYPE:
    return stmt_expr_contains_ident_name(e->as.ptr_type.target, name);
  case NY_E_DEREF:
    return stmt_expr_contains_ident_name(e->as.deref.target, name);
  case NY_E_SIZEOF:
    return !e->as.szof.is_type &&
           stmt_expr_contains_ident_name(e->as.szof.target, name);
  case NY_E_TRY:
    return stmt_expr_contains_ident_name(e->as.try_expr.target, name);
  case NY_E_LAMBDA:
  case NY_E_FN:
    for (size_t i = 0; i < e->as.lambda.params.len; ++i) {
      if (stmt_expr_contains_ident_name(e->as.lambda.params.data[i].def, name))
        return true;
    }
    if (stmt_param_list_shadows_name(&e->as.lambda.params, name))
      return false;
    return stmt_contains_ident_name(e->as.lambda.body, name);
  case NY_E_ASM:
    for (size_t i = 0; i < e->as.as_asm.args.len; ++i) {
      if (stmt_expr_contains_ident_name(e->as.as_asm.args.data[i], name))
        return true;
    }
    return false;
  case NY_E_COMPTIME:
    return stmt_contains_ident_name(e->as.comptime_expr.body, name);
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; ++i) {
      fstring_part_t *part = &e->as.fstring.parts.data[i];
      if (part->kind == NY_FSP_EXPR &&
          stmt_expr_contains_ident_name(part->as.e, name))
        return true;
    }
    return false;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i) {
      if (stmt_expr_contains_ident_name(e->as.list_like.data[i], name))
        return true;
    }
    return false;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      if (stmt_expr_contains_ident_name(e->as.dict.pairs.data[i].key, name) ||
          stmt_expr_contains_ident_name(e->as.dict.pairs.data[i].value, name))
        return true;
    }
    return false;
  case NY_E_MATCH:
    if (stmt_expr_contains_ident_name(e->as.match.test, name))
      return true;
    for (size_t i = 0; i < e->as.match.arms.len; ++i) {
      for (size_t p = 0; p < e->as.match.arms.data[i].patterns.len; ++p) {
        if (stmt_expr_contains_ident_name(
                e->as.match.arms.data[i].patterns.data[p], name))
          return true;
      }
      if (stmt_expr_contains_ident_name(e->as.match.arms.data[i].guard, name))
        return true;
      if (stmt_contains_ident_name(e->as.match.arms.data[i].conseq, name))
        return true;
    }
    return stmt_contains_ident_name(e->as.match.default_conseq, name);
  default:
    return false;
  }
}

static bool stmt_params_contain_ident_name(const ny_param_list *params,
                                           const char *name) {
  if (!params || !name)
    return false;
  for (size_t i = 0; i < params->len; ++i) {
    if (stmt_expr_contains_ident_name(params->data[i].def, name))
      return true;
  }
  return false;
}

static bool stmt_list_contains_ident_name(ny_stmt_list *list,
                                          const char *name) {
  if (!list || !name)
    return false;
  for (size_t i = 0; i < list->len; ++i) {
    if (stmt_contains_ident_name(list->data[i], name))
      return true;
  }
  return false;
}

static bool stmt_layout_fields_contain_ident_name(ny_layout_field_list *fields,
                                                  const char *name) {
  if (!fields || !name)
    return false;
  for (size_t i = 0; i < fields->len; ++i) {
    if (stmt_expr_contains_ident_name(fields->data[i].default_value, name))
      return true;
  }
  return false;
}

static bool stmt_contains_ident_name(stmt_t *s, const char *name) {
  if (!s || !name)
    return false;
  switch (s->kind) {
  case NY_S_BLOCK:
    return stmt_list_contains_ident_name(&s->as.block.body, name);
  case NY_S_MODULE:
    return stmt_list_contains_ident_name(&s->as.module.body, name);
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; ++i) {
      if (stmt_expr_contains_ident_name(s->as.var.exprs.data[i], name))
        return true;
    }
    return false;
  case NY_S_EXPR:
    return stmt_expr_contains_ident_name(s->as.expr.expr, name);
  case NY_S_IF:
    return stmt_contains_ident_name(s->as.iff.init, name) ||
           stmt_expr_contains_ident_name(s->as.iff.test, name) ||
           stmt_contains_ident_name(s->as.iff.conseq, name) ||
           stmt_contains_ident_name(s->as.iff.alt, name);
  case NY_S_GUARD:
    return stmt_expr_contains_ident_name(s->as.guard.value, name) ||
           stmt_contains_ident_name(s->as.guard.fallback, name);
  case NY_S_WHILE:
    return stmt_contains_ident_name(s->as.whl.init, name) ||
           stmt_expr_contains_ident_name(s->as.whl.test, name) ||
           stmt_contains_ident_name(s->as.whl.body, name) ||
           stmt_contains_ident_name(s->as.whl.update, name);
  case NY_S_FOR:
    return stmt_contains_ident_name(s->as.fr.init, name) ||
           stmt_expr_contains_ident_name(s->as.fr.cond, name) ||
           stmt_expr_contains_ident_name(s->as.fr.iterable, name) ||
           stmt_contains_ident_name(s->as.fr.body, name) ||
           stmt_contains_ident_name(s->as.fr.update, name);
  case NY_S_TRY:
    return stmt_contains_ident_name(s->as.tr.body, name) ||
           stmt_contains_ident_name(s->as.tr.handler, name);
  case NY_S_RETURN:
    return stmt_expr_contains_ident_name(s->as.ret.value, name);
  case NY_S_DEFER:
    return stmt_contains_ident_name(s->as.de.body, name);
  case NY_S_MATCH:
    if (stmt_expr_contains_ident_name(s->as.match.test, name))
      return true;
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      match_arm_t *arm = &s->as.match.arms.data[i];
      for (size_t p = 0; p < arm->patterns.len; ++p) {
        if (stmt_expr_contains_ident_name(arm->patterns.data[p], name))
          return true;
      }
      if (stmt_expr_contains_ident_name(arm->guard, name) ||
          stmt_contains_ident_name(arm->conseq, name))
        return true;
    }
    return stmt_contains_ident_name(s->as.match.default_conseq, name);
  case NY_S_FUNC:
    if (stmt_params_contain_ident_name(&s->as.fn.params, name))
      return true;
    if (stmt_param_list_shadows_name(&s->as.fn.params, name))
      return false;
    return stmt_contains_ident_name(s->as.fn.body, name);
  case NY_S_LAYOUT:
    return stmt_layout_fields_contain_ident_name(&s->as.layout.fields, name) ||
           stmt_list_contains_ident_name(&s->as.layout.methods, name);
  case NY_S_STRUCT:
    return stmt_layout_fields_contain_ident_name(&s->as.struc.fields, name) ||
           stmt_list_contains_ident_name(&s->as.struc.methods, name);
  case NY_S_ENUM:
    for (size_t i = 0; i < s->as.enu.items.len; ++i) {
      if (stmt_expr_contains_ident_name(s->as.enu.items.data[i].value, name))
        return true;
    }
    return false;
  case NY_S_MACRO:
    for (size_t i = 0; i < s->as.macro.args.len; ++i) {
      if (stmt_expr_contains_ident_name(s->as.macro.args.data[i], name))
        return true;
    }
    return stmt_contains_ident_name(s->as.macro.body, name);
  case NY_S_IMPL:
    return stmt_list_contains_ident_name(&s->as.impl.methods, name);
  default:
    return false;
  }
}

static bool stmt_call_name_is_unshadowed_builtin(codegen_t *cg, scope *scopes,
                                                 size_t depth,
                                                 const char *callee,
                                                 const char *tail) {
  if (!callee || !tail)
    return false;
  if (strcmp(callee, tail) == 0)
    return scope_lookup(scopes, depth, callee) == NULL;
  char qname[128];
  snprintf(qname, sizeof(qname), "std.core.%s", tail);
  if (strcmp(callee, qname) == 0)
    return true;
  snprintf(qname, sizeof(qname), "std.core.reflect.%s", tail);
  if (strcmp(callee, qname) == 0)
    return true;
  (void)cg;
  return false;
}

static bool stmt_expr_static_list_only_uses(codegen_t *cg, scope *scopes,
                                            size_t depth, expr_t *e,
                                            const char *name);
static bool stmt_static_list_only_uses(codegen_t *cg, scope *scopes,
                                       size_t depth, stmt_t *s,
                                       stmt_t *decl_stmt, const char *name);
static const char *stmt_expr_list_fastpath_bail_reason(codegen_t *cg,
                                                       scope *scopes,
                                                       size_t depth, expr_t *e,
                                                       const char *name,
                                                       bool allow_set_idx);
static const char *stmt_list_fastpath_bail_reason(codegen_t *cg, scope *scopes,
                                                  size_t depth, stmt_t *s,
                                                  stmt_t *decl_stmt,
                                                  const char *name,
                                                  bool allow_set_idx);
static bool stmt_expr_is_set_idx_to_name(expr_t *e, const char *name,
                                         expr_t **out_value);

static bool stmt_static_list_same_file_or_unknown(stmt_t *decl_stmt,
                                                  stmt_t *s) {
  const char *decl_file =
      decl_stmt && decl_stmt->tok.filename ? decl_stmt->tok.filename : NULL;
  const char *stmt_file = s && s->tok.filename ? s->tok.filename : NULL;
  return !decl_file || !*decl_file || !stmt_file || !*stmt_file ||
         strcmp(decl_file, stmt_file) == 0;
}

static bool stmt_static_list_is_decl_stmt(stmt_t *s, stmt_t *decl_stmt,
                                          const char *name) {
  if (s == decl_stmt)
    return true;
  if (!s || !decl_stmt || s->kind != NY_S_VAR || decl_stmt->kind != NY_S_VAR)
    return false;
  if (!stmt_static_list_same_file_or_unknown(decl_stmt, s))
    return false;
  if (s->tok.line > 0 && decl_stmt->tok.line > 0 &&
      s->tok.line == decl_stmt->tok.line && s->tok.col == decl_stmt->tok.col)
    return true;
  if (!name || s->as.var.names.len != decl_stmt->as.var.names.len ||
      s->as.var.exprs.len != decl_stmt->as.var.exprs.len)
    return false;
  for (size_t i = 0; i < s->as.var.names.len; ++i) {
    if (!s->as.var.names.data[i] || !decl_stmt->as.var.names.data[i] ||
        strcmp(s->as.var.names.data[i], decl_stmt->as.var.names.data[i]) != 0)
      return false;
  }
  return s->as.var.names.len == 1 && strcmp(s->as.var.names.data[0], name) == 0;
}

static bool stmt_static_list_should_scan_stmt(stmt_t *decl_stmt, stmt_t *s) {
  return stmt_static_list_same_file_or_unknown(decl_stmt, s);
}

static bool stmt_call_args_static_list_only_uses(codegen_t *cg, scope *scopes,
                                                 size_t depth,
                                                 ny_call_arg_list *args,
                                                 size_t start,
                                                 const char *name) {
  if (!args)
    return true;
  for (size_t i = start; i < args->len; ++i) {
    if (!stmt_expr_static_list_only_uses(cg, scopes, depth, args->data[i].val,
                                         name))
      return false;
  }
  return true;
}

static bool stmt_params_static_list_only_uses(codegen_t *cg, scope *scopes,
                                              size_t depth,
                                              const ny_param_list *params,
                                              const char *name) {
  if (!params)
    return true;
  for (size_t i = 0; i < params->len; ++i) {
    if (!stmt_expr_static_list_only_uses(cg, scopes, depth,
                                         params->data[i].def, name))
      return false;
  }
  return true;
}

static bool stmt_expr_static_list_only_uses(codegen_t *cg, scope *scopes,
                                            size_t depth, expr_t *e,
                                            const char *name) {
  if (!e)
    return true;
  switch (e->kind) {
  case NY_E_IDENT:
    return !ny_expr_ident_is_name(e, name);
  case NY_E_LITERAL:
  case NY_E_EMBED:
  case NY_E_INFERRED_MEMBER:
    return true;
  case NY_E_UNARY:
    return stmt_expr_static_list_only_uses(cg, scopes, depth, e->as.unary.right,
                                           name);
  case NY_E_BINARY:
    return stmt_expr_static_list_only_uses(cg, scopes, depth, e->as.binary.left,
                                           name) &&
           stmt_expr_static_list_only_uses(cg, scopes, depth,
                                           e->as.binary.right, name);
  case NY_E_LOGICAL:
    return stmt_expr_static_list_only_uses(cg, scopes, depth,
                                           e->as.logical.left, name) &&
           stmt_expr_static_list_only_uses(cg, scopes, depth,
                                           e->as.logical.right, name);
  case NY_E_TERNARY:
    return stmt_expr_static_list_only_uses(cg, scopes, depth,
                                           e->as.ternary.cond, name) &&
           stmt_expr_static_list_only_uses(cg, scopes, depth,
                                           e->as.ternary.true_expr, name) &&
           stmt_expr_static_list_only_uses(cg, scopes, depth,
                                           e->as.ternary.false_expr, name);
  case NY_E_CALL: {
    const char *callee =
        (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT)
            ? e->as.call.callee->as.ident.name
            : NULL;
    if (callee && (e->as.call.args.len == 2 || e->as.call.args.len == 3) &&
        stmt_call_name_is_unshadowed_builtin(cg, scopes, depth, callee,
                                             "get") &&
        stmt_expr_is_static_list_builtin_target(e->as.call.args.data[0].val,
                                                name)) {
      return stmt_call_args_static_list_only_uses(cg, scopes, depth,
                                                  &e->as.call.args, 1, name);
    }
    if (callee && e->as.call.args.len == 1 &&
        stmt_call_name_is_unshadowed_builtin(cg, scopes, depth, callee,
                                             "len") &&
        stmt_expr_is_static_list_builtin_target(e->as.call.args.data[0].val,
                                                name)) {
      return true;
    }
    if (!stmt_expr_static_list_only_uses(cg, scopes, depth, e->as.call.callee,
                                         name))
      return false;
    return stmt_call_args_static_list_only_uses(cg, scopes, depth,
                                                &e->as.call.args, 0, name);
  }
  case NY_E_MEMCALL:
    if (e->as.memcall.name &&
        stmt_expr_is_static_list_builtin_target(e->as.memcall.target, name) &&
        ny_name_tail_is(e->as.memcall.name, "get") &&
        (e->as.memcall.args.len == 1 || e->as.memcall.args.len == 2)) {
      return stmt_call_args_static_list_only_uses(cg, scopes, depth,
                                                  &e->as.memcall.args, 0,
                                                  name);
    }
    return stmt_expr_static_list_only_uses(cg, scopes, depth,
                                           e->as.memcall.target, name) &&
           stmt_call_args_static_list_only_uses(cg, scopes, depth,
                                                &e->as.memcall.args, 0, name);
  case NY_E_INDEX:
    return stmt_expr_static_list_only_uses(cg, scopes, depth,
                                           e->as.index.target, name) &&
           stmt_expr_static_list_only_uses(cg, scopes, depth, e->as.index.start,
                                           name) &&
           stmt_expr_static_list_only_uses(cg, scopes, depth, e->as.index.stop,
                                           name) &&
           stmt_expr_static_list_only_uses(cg, scopes, depth, e->as.index.step,
                                           name);
  case NY_E_MEMBER:
    if (e->as.member.name && strcmp(e->as.member.name, "len") == 0 &&
        stmt_expr_is_static_list_builtin_target(e->as.member.target, name))
      return true;
    return stmt_expr_static_list_only_uses(cg, scopes, depth,
                                           e->as.member.target, name);
  case NY_E_PTR_TYPE:
    return stmt_expr_static_list_only_uses(cg, scopes, depth,
                                           e->as.ptr_type.target, name);
  case NY_E_DEREF:
    return stmt_expr_static_list_only_uses(cg, scopes, depth,
                                           e->as.deref.target, name);
  case NY_E_SIZEOF:
    return e->as.szof.is_type ||
           stmt_expr_static_list_only_uses(cg, scopes, depth, e->as.szof.target,
                                           name);
  case NY_E_TRY:
    return stmt_expr_static_list_only_uses(cg, scopes, depth,
                                           e->as.try_expr.target, name);
  case NY_E_LAMBDA:
  case NY_E_FN:
    if (!stmt_params_static_list_only_uses(cg, scopes, depth,
                                           &e->as.lambda.params, name))
      return false;
    if (stmt_params_contain_ident_name(&e->as.lambda.params, name))
      return false;
    if (stmt_param_list_shadows_name(&e->as.lambda.params, name))
      return true;
    return !stmt_contains_ident_name(e->as.lambda.body, name);
  case NY_E_ASM:
    for (size_t i = 0; i < e->as.as_asm.args.len; ++i) {
      if (!stmt_expr_static_list_only_uses(cg, scopes, depth,
                                           e->as.as_asm.args.data[i], name))
        return false;
    }
    return true;
  case NY_E_COMPTIME:
    return stmt_static_list_only_uses(cg, scopes, depth,
                                      e->as.comptime_expr.body, NULL, name);
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; ++i) {
      fstring_part_t *part = &e->as.fstring.parts.data[i];
      if (part->kind == NY_FSP_EXPR &&
          !stmt_expr_static_list_only_uses(cg, scopes, depth, part->as.e,
                                           name))
        return false;
    }
    return true;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i) {
      if (!stmt_expr_static_list_only_uses(cg, scopes, depth,
                                           e->as.list_like.data[i], name))
        return false;
    }
    return true;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      if (!stmt_expr_static_list_only_uses(
              cg, scopes, depth, e->as.dict.pairs.data[i].key, name) ||
          !stmt_expr_static_list_only_uses(
              cg, scopes, depth, e->as.dict.pairs.data[i].value, name))
        return false;
    }
    return true;
  case NY_E_MATCH:
    if (!stmt_expr_static_list_only_uses(cg, scopes, depth, e->as.match.test,
                                         name))
      return false;
    for (size_t i = 0; i < e->as.match.arms.len; ++i) {
      for (size_t p = 0; p < e->as.match.arms.data[i].patterns.len; ++p) {
        if (!stmt_expr_static_list_only_uses(
                cg, scopes, depth, e->as.match.arms.data[i].patterns.data[p],
                name))
          return false;
      }
      if (!stmt_expr_static_list_only_uses(
              cg, scopes, depth, e->as.match.arms.data[i].guard, name) ||
          !stmt_static_list_only_uses(
              cg, scopes, depth, e->as.match.arms.data[i].conseq, NULL, name))
        return false;
    }
    return stmt_static_list_only_uses(cg, scopes, depth,
                                      e->as.match.default_conseq, NULL, name);
  default:
    return !stmt_expr_contains_ident_name(e, name);
  }
}

static bool stmt_static_list_only_uses(codegen_t *cg, scope *scopes,
                                       size_t depth, stmt_t *s,
                                       stmt_t *decl_stmt, const char *name) {
  if (!s)
    return true;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      if (!stmt_static_list_should_scan_stmt(decl_stmt, s->as.block.body.data[i]))
        continue;
      if (!stmt_static_list_only_uses(
              cg, scopes, depth, s->as.block.body.data[i], decl_stmt, name))
        return false;
    }
    return true;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      if (!stmt_static_list_should_scan_stmt(decl_stmt,
                                             s->as.module.body.data[i]))
        continue;
      if (!stmt_static_list_only_uses(
              cg, scopes, depth, s->as.module.body.data[i], decl_stmt, name))
        return false;
    }
    return true;
  case NY_S_VAR:
    if (!stmt_static_list_is_decl_stmt(s, decl_stmt, name)) {
      for (size_t i = 0; i < s->as.var.names.len; ++i) {
        if (s->as.var.names.data[i] &&
            strcmp(s->as.var.names.data[i], name) == 0)
          return false;
      }
    }
    for (size_t i = 0; i < s->as.var.exprs.len; ++i) {
      if (!stmt_expr_static_list_only_uses(cg, scopes, depth,
                                           s->as.var.exprs.data[i], name))
        return false;
    }
    return true;
  case NY_S_EXPR:
    return stmt_expr_static_list_only_uses(cg, scopes, depth, s->as.expr.expr,
                                           name);
  case NY_S_IF:
    return stmt_expr_static_list_only_uses(cg, scopes, depth, s->as.iff.test,
                                           name) &&
           stmt_static_list_only_uses(cg, scopes, depth, s->as.iff.init,
                                      decl_stmt, name) &&
           stmt_static_list_only_uses(cg, scopes, depth, s->as.iff.conseq,
                                      decl_stmt, name) &&
           stmt_static_list_only_uses(cg, scopes, depth, s->as.iff.alt,
                                      decl_stmt, name);
  case NY_S_GUARD:
    return stmt_expr_static_list_only_uses(cg, scopes, depth, s->as.guard.value,
                                           name) &&
           stmt_static_list_only_uses(cg, scopes, depth, s->as.guard.fallback,
                                      decl_stmt, name);
  case NY_S_WHILE:
    return stmt_static_list_only_uses(cg, scopes, depth, s->as.whl.init,
                                      decl_stmt, name) &&
           stmt_expr_static_list_only_uses(cg, scopes, depth, s->as.whl.test,
                                           name) &&
           stmt_static_list_only_uses(cg, scopes, depth, s->as.whl.body,
                                      decl_stmt, name) &&
           stmt_static_list_only_uses(cg, scopes, depth, s->as.whl.update,
                                      decl_stmt, name);
  case NY_S_FOR:
    if (s->as.fr.iter_var && strcmp(s->as.fr.iter_var, name) == 0)
      return false;
    if (s->as.fr.iter_index_var && strcmp(s->as.fr.iter_index_var, name) == 0)
      return false;
    return stmt_static_list_only_uses(cg, scopes, depth, s->as.fr.init,
                                      decl_stmt, name) &&
           stmt_expr_static_list_only_uses(cg, scopes, depth, s->as.fr.cond,
                                           name) &&
           stmt_expr_static_list_only_uses(cg, scopes, depth, s->as.fr.iterable,
                                           name) &&
           stmt_static_list_only_uses(cg, scopes, depth, s->as.fr.body,
                                      decl_stmt, name) &&
           stmt_static_list_only_uses(cg, scopes, depth, s->as.fr.update,
                                      decl_stmt, name);
  case NY_S_TRY:
    return stmt_static_list_only_uses(cg, scopes, depth, s->as.tr.body,
                                      decl_stmt, name) &&
           stmt_static_list_only_uses(cg, scopes, depth, s->as.tr.handler,
                                      decl_stmt, name);
  case NY_S_RETURN:
    return stmt_expr_static_list_only_uses(cg, scopes, depth, s->as.ret.value,
                                           name);
  case NY_S_DEFER:
    return stmt_static_list_only_uses(cg, scopes, depth, s->as.de.body,
                                      decl_stmt, name);
  case NY_S_MATCH:
    if (!stmt_expr_static_list_only_uses(cg, scopes, depth, s->as.match.test,
                                         name))
      return false;
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      for (size_t p = 0; p < s->as.match.arms.data[i].patterns.len; ++p) {
        if (!stmt_expr_static_list_only_uses(
                cg, scopes, depth, s->as.match.arms.data[i].patterns.data[p],
                name))
          return false;
      }
      if (!stmt_expr_static_list_only_uses(
              cg, scopes, depth, s->as.match.arms.data[i].guard, name) ||
          !stmt_static_list_only_uses(cg, scopes, depth,
                                      s->as.match.arms.data[i].conseq,
                                      decl_stmt, name))
        return false;
    }
    return stmt_static_list_only_uses(
        cg, scopes, depth, s->as.match.default_conseq, decl_stmt, name);
  case NY_S_FUNC:
    if (!stmt_params_static_list_only_uses(cg, scopes, depth, &s->as.fn.params,
                                           name))
      return false;
    if (stmt_params_contain_ident_name(&s->as.fn.params, name))
      return false;
    if (stmt_param_list_shadows_name(&s->as.fn.params, name))
      return true;
    return !stmt_contains_ident_name(s->as.fn.body, name);
  case NY_S_MACRO:
    return stmt_static_list_only_uses(cg, scopes, depth, s->as.macro.body,
                                      decl_stmt, name);
  default:
    return true;
  }
}

static const char *stmt_expr_list_fastpath_bail_reason(codegen_t *cg,
                                                       scope *scopes,
                                                       size_t depth, expr_t *e,
                                                       const char *name,
                                                       bool allow_set_idx);

static const char *stmt_call_args_list_fastpath_bail_reason(
    codegen_t *cg, scope *scopes, size_t depth, ny_call_arg_list *args,
    size_t start, const char *name, bool allow_set_idx) {
  if (!args)
    return NULL;
  for (size_t i = start; i < args->len; ++i) {
    const char *reason = stmt_expr_list_fastpath_bail_reason(
        cg, scopes, depth, args->data[i].val, name, allow_set_idx);
    if (reason)
      return reason;
  }
  return NULL;
}

static const char *stmt_params_list_fastpath_bail_reason(
    codegen_t *cg, scope *scopes, size_t depth, const ny_param_list *params,
    const char *name, bool allow_set_idx) {
  if (!params)
    return NULL;
  for (size_t i = 0; i < params->len; ++i) {
    const char *reason = stmt_expr_list_fastpath_bail_reason(
        cg, scopes, depth, params->data[i].def, name, allow_set_idx);
    if (reason)
      return reason;
  }
  return NULL;
}

static const char *stmt_expr_list_fastpath_bail_reason(codegen_t *cg,
                                                       scope *scopes,
                                                       size_t depth, expr_t *e,
                                                       const char *name,
                                                       bool allow_set_idx) {
  if (!e)
    return NULL;
  switch (e->kind) {
  case NY_E_IDENT:
    return ny_expr_ident_is_name(e, name) ? "value-use" : NULL;
  case NY_E_LITERAL:
  case NY_E_EMBED:
  case NY_E_INFERRED_MEMBER:
    return NULL;
  case NY_E_UNARY:
    return stmt_expr_list_fastpath_bail_reason(
        cg, scopes, depth, e->as.unary.right, name, allow_set_idx);
  case NY_E_BINARY: {
    const char *reason = stmt_expr_list_fastpath_bail_reason(
        cg, scopes, depth, e->as.binary.left, name, allow_set_idx);
    return reason ? reason
                  : stmt_expr_list_fastpath_bail_reason(cg, scopes, depth,
                                                        e->as.binary.right,
                                                        name, allow_set_idx);
  }
  case NY_E_LOGICAL: {
    const char *reason = stmt_expr_list_fastpath_bail_reason(
        cg, scopes, depth, e->as.logical.left, name, allow_set_idx);
    return reason ? reason
                  : stmt_expr_list_fastpath_bail_reason(cg, scopes, depth,
                                                        e->as.logical.right,
                                                        name, allow_set_idx);
  }
  case NY_E_TERNARY: {
    const char *reason = stmt_expr_list_fastpath_bail_reason(
        cg, scopes, depth, e->as.ternary.cond, name, allow_set_idx);
    if (reason)
      return reason;
    reason = stmt_expr_list_fastpath_bail_reason(
        cg, scopes, depth, e->as.ternary.true_expr, name, allow_set_idx);
    return reason
               ? reason
               : stmt_expr_list_fastpath_bail_reason(cg, scopes, depth,
                                                     e->as.ternary.false_expr,
                                                     name, allow_set_idx);
  }
  case NY_E_CALL: {
    const char *callee =
        (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT)
            ? e->as.call.callee->as.ident.name
            : NULL;
    bool target_is_list =
        e->as.call.args.len > 0 && stmt_expr_is_static_list_builtin_target(
                                       e->as.call.args.data[0].val, name);
    if (callee && target_is_list &&
        stmt_call_name_is_unshadowed_builtin(cg, scopes, depth, callee,
                                             "get")) {
      if (e->as.call.args.len != 2 && e->as.call.args.len != 3)
        return "unsupported-get-arity";
      return stmt_call_args_list_fastpath_bail_reason(
          cg, scopes, depth, &e->as.call.args, 1, name, allow_set_idx);
    }
    if (callee && target_is_list &&
        stmt_call_name_is_unshadowed_builtin(cg, scopes, depth, callee,
                                             "len")) {
      return e->as.call.args.len == 1 ? NULL : "unsupported-len-arity";
    }
    if (callee && target_is_list && ny_name_tail_is(callee, "set_idx")) {
      if (!allow_set_idx)
        return "set_idx";
      if (e->as.call.args.len != 3)
        return "unsupported-set-arity";
      return stmt_call_args_list_fastpath_bail_reason(
          cg, scopes, depth, &e->as.call.args, 1, name, allow_set_idx);
    }
    if (callee && target_is_list)
      return "unknown-call";
    if (stmt_expr_contains_ident_name(e, name))
      return "unknown-call";
    return NULL;
  }
  case NY_E_MEMCALL:
    if (e->as.memcall.name &&
        stmt_expr_is_static_list_builtin_target(e->as.memcall.target, name) &&
        ny_name_tail_is(e->as.memcall.name, "get")) {
      if (e->as.memcall.args.len != 1 && e->as.memcall.args.len != 2)
        return "unsupported-get-arity";
      return stmt_call_args_list_fastpath_bail_reason(
          cg, scopes, depth, &e->as.memcall.args, 0, name, allow_set_idx);
    }
    if (stmt_expr_contains_ident_name(e, name))
      return "unknown-call";
    return NULL;
  case NY_E_INDEX:
    if (stmt_expr_contains_ident_name(e->as.index.target, name))
      return "value-use";
    {
      const char *reason = stmt_expr_list_fastpath_bail_reason(
          cg, scopes, depth, e->as.index.start, name, allow_set_idx);
      if (reason)
        return reason;
      reason = stmt_expr_list_fastpath_bail_reason(
          cg, scopes, depth, e->as.index.stop, name, allow_set_idx);
      return reason ? reason
                    : stmt_expr_list_fastpath_bail_reason(cg, scopes, depth,
                                                          e->as.index.step,
                                                          name, allow_set_idx);
    }
  case NY_E_MEMBER:
    if (e->as.member.name && strcmp(e->as.member.name, "len") == 0 &&
        stmt_expr_is_static_list_builtin_target(e->as.member.target, name))
      return NULL;
    return stmt_expr_contains_ident_name(e->as.member.target, name)
               ? "value-use"
               : NULL;
  case NY_E_PTR_TYPE:
    return stmt_expr_list_fastpath_bail_reason(
        cg, scopes, depth, e->as.ptr_type.target, name, allow_set_idx);
  case NY_E_DEREF:
    return stmt_expr_list_fastpath_bail_reason(
        cg, scopes, depth, e->as.deref.target, name, allow_set_idx);
  case NY_E_SIZEOF:
    return e->as.szof.is_type
               ? NULL
               : stmt_expr_list_fastpath_bail_reason(
                     cg, scopes, depth, e->as.szof.target, name, allow_set_idx);
  case NY_E_TRY:
    return stmt_expr_list_fastpath_bail_reason(
        cg, scopes, depth, e->as.try_expr.target, name, allow_set_idx);
  case NY_E_LAMBDA:
  case NY_E_FN: {
    const char *reason = stmt_params_list_fastpath_bail_reason(
        cg, scopes, depth, &e->as.lambda.params, name, allow_set_idx);
    if (reason)
      return reason;
    if (stmt_params_contain_ident_name(&e->as.lambda.params, name))
      return "function-capture";
    if (stmt_param_list_shadows_name(&e->as.lambda.params, name))
      return NULL;
    return stmt_contains_ident_name(e->as.lambda.body, name)
               ? "function-capture"
               : NULL;
  }
  case NY_E_ASM:
    for (size_t i = 0; i < e->as.as_asm.args.len; ++i) {
      const char *reason = stmt_expr_list_fastpath_bail_reason(
          cg, scopes, depth, e->as.as_asm.args.data[i], name, allow_set_idx);
      if (reason)
        return reason;
    }
    return NULL;
  case NY_E_COMPTIME:
    return stmt_list_fastpath_bail_reason(cg, scopes, depth,
                                          e->as.comptime_expr.body, NULL, name,
                                          allow_set_idx);
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; ++i) {
      fstring_part_t *part = &e->as.fstring.parts.data[i];
      if (part->kind != NY_FSP_EXPR)
        continue;
      const char *reason = stmt_expr_list_fastpath_bail_reason(
          cg, scopes, depth, part->as.e, name, allow_set_idx);
      if (reason)
        return reason;
    }
    return NULL;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i) {
      const char *reason = stmt_expr_list_fastpath_bail_reason(
          cg, scopes, depth, e->as.list_like.data[i], name, allow_set_idx);
      if (reason)
        return reason;
    }
    return NULL;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      const char *reason = stmt_expr_list_fastpath_bail_reason(
          cg, scopes, depth, e->as.dict.pairs.data[i].key, name, allow_set_idx);
      if (reason)
        return reason;
      reason = stmt_expr_list_fastpath_bail_reason(
          cg, scopes, depth, e->as.dict.pairs.data[i].value, name,
          allow_set_idx);
      if (reason)
        return reason;
    }
    return NULL;
  default:
    return stmt_expr_contains_ident_name(e, name) ? "unsupported-use" : NULL;
  }
}

static const char *stmt_list_fastpath_bail_reason(codegen_t *cg, scope *scopes,
                                                  size_t depth, stmt_t *s,
                                                  stmt_t *decl_stmt,
                                                  const char *name,
                                                  bool allow_set_idx) {
  if (!s)
    return NULL;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      if (!stmt_static_list_should_scan_stmt(decl_stmt, s->as.block.body.data[i]))
        continue;
      const char *reason = stmt_list_fastpath_bail_reason(
          cg, scopes, depth, s->as.block.body.data[i], decl_stmt, name,
          allow_set_idx);
      if (reason)
        return reason;
    }
    return NULL;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      if (!stmt_static_list_should_scan_stmt(decl_stmt,
                                             s->as.module.body.data[i]))
        continue;
      const char *reason = stmt_list_fastpath_bail_reason(
          cg, scopes, depth, s->as.module.body.data[i], decl_stmt, name,
          allow_set_idx);
      if (reason)
        return reason;
    }
    return NULL;
  case NY_S_VAR:
    if (!stmt_static_list_is_decl_stmt(s, decl_stmt, name)) {
      for (size_t i = 0; i < s->as.var.names.len; ++i) {
        if (s->as.var.names.data[i] &&
            strcmp(s->as.var.names.data[i], name) == 0)
          return allow_set_idx ? "reassigned" : "alias-or-reassign";
      }
    }
    for (size_t i = 0; i < s->as.var.exprs.len; ++i) {
      const char *reason = stmt_expr_list_fastpath_bail_reason(
          cg, scopes, depth, s->as.var.exprs.data[i], name, allow_set_idx);
      if (reason)
        return reason;
    }
    return NULL;
  case NY_S_EXPR:
    return stmt_expr_list_fastpath_bail_reason(
        cg, scopes, depth, s->as.expr.expr, name, allow_set_idx);
  case NY_S_IF: {
    const char *reason = stmt_expr_list_fastpath_bail_reason(
        cg, scopes, depth, s->as.iff.test, name, allow_set_idx);
    if (reason)
      return reason;
    reason = stmt_list_fastpath_bail_reason(cg, scopes, depth, s->as.iff.init,
                                            decl_stmt, name, allow_set_idx);
    if (reason)
      return reason;
    reason = stmt_list_fastpath_bail_reason(cg, scopes, depth, s->as.iff.conseq,
                                            decl_stmt, name, allow_set_idx);
    return reason ? reason
                  : stmt_list_fastpath_bail_reason(cg, scopes, depth,
                                                   s->as.iff.alt, decl_stmt,
                                                   name, allow_set_idx);
  }
  case NY_S_WHILE: {
    const char *reason = stmt_list_fastpath_bail_reason(
        cg, scopes, depth, s->as.whl.init, decl_stmt, name, allow_set_idx);
    if (reason)
      return reason;
    reason = stmt_expr_list_fastpath_bail_reason(
        cg, scopes, depth, s->as.whl.test, name, allow_set_idx);
    if (reason)
      return reason;
    reason = stmt_list_fastpath_bail_reason(cg, scopes, depth, s->as.whl.body,
                                            decl_stmt, name, allow_set_idx);
    return reason ? reason
                  : stmt_list_fastpath_bail_reason(cg, scopes, depth,
                                                   s->as.whl.update, decl_stmt,
                                                   name, allow_set_idx);
  }
  case NY_S_FOR: {
    if (s->as.fr.iter_var && strcmp(s->as.fr.iter_var, name) == 0)
      return "reassigned";
    if (s->as.fr.iter_index_var && strcmp(s->as.fr.iter_index_var, name) == 0)
      return "reassigned";
    const char *reason = stmt_list_fastpath_bail_reason(
        cg, scopes, depth, s->as.fr.init, decl_stmt, name, allow_set_idx);
    if (reason)
      return reason;
    reason = stmt_expr_list_fastpath_bail_reason(
        cg, scopes, depth, s->as.fr.cond, name, allow_set_idx);
    if (reason)
      return reason;
    reason = stmt_expr_list_fastpath_bail_reason(
        cg, scopes, depth, s->as.fr.iterable, name, allow_set_idx);
    if (reason)
      return reason;
    reason = stmt_list_fastpath_bail_reason(cg, scopes, depth, s->as.fr.body,
                                            decl_stmt, name, allow_set_idx);
    return reason ? reason
                  : stmt_list_fastpath_bail_reason(cg, scopes, depth,
                                                   s->as.fr.update, decl_stmt,
                                                   name, allow_set_idx);
  }
  case NY_S_RETURN:
    return stmt_expr_list_fastpath_bail_reason(
        cg, scopes, depth, s->as.ret.value, name, allow_set_idx);
  case NY_S_GUARD: {
    const char *reason = stmt_expr_list_fastpath_bail_reason(
        cg, scopes, depth, s->as.guard.value, name, allow_set_idx);
    return reason
               ? reason
               : stmt_list_fastpath_bail_reason(cg, scopes, depth,
                                                s->as.guard.fallback, decl_stmt,
                                                name, allow_set_idx);
  }
  case NY_S_TRY: {
    const char *reason = stmt_list_fastpath_bail_reason(
        cg, scopes, depth, s->as.tr.body, decl_stmt, name, allow_set_idx);
    return reason ? reason
                  : stmt_list_fastpath_bail_reason(cg, scopes, depth,
                                                   s->as.tr.handler, decl_stmt,
                                                   name, allow_set_idx);
  }
  case NY_S_DEFER:
    return stmt_list_fastpath_bail_reason(cg, scopes, depth, s->as.de.body,
                                          decl_stmt, name, allow_set_idx);
  case NY_S_FUNC: {
    const char *reason = stmt_params_list_fastpath_bail_reason(
        cg, scopes, depth, &s->as.fn.params, name, allow_set_idx);
    if (reason)
      return reason;
    if (stmt_params_contain_ident_name(&s->as.fn.params, name))
      return "function-capture";
    if (stmt_param_list_shadows_name(&s->as.fn.params, name))
      return NULL;
    return stmt_contains_ident_name(s->as.fn.body, name) ? "function-capture"
                                                         : NULL;
  }
  case NY_S_MACRO:
    return stmt_list_fastpath_bail_reason(cg, scopes, depth, s->as.macro.body,
                                          decl_stmt, name, allow_set_idx);
  default:
    return NULL;
  }
}

static bool stmt_can_elide_static_int_list_object(
    codegen_t *cg, scope *scopes, size_t depth, stmt_t *decl_stmt,
    const char *name, bool escapes, expr_t *init, const char **bail_reason) {
  // This optimization was found to be unsound: it can elide a module-level
  // (or otherwise escaping) immutable int-list object even when the list is
  // later indexed from a different function in the same module, since that
  // usage isn't visible to the safety scan below. That mismatch between the
  // elided fast representation and ordinary list access corrupts memory
  // (observed as segfaults/garbage reads). Disabled unconditionally.
  (void)cg;
  (void)scopes;
  (void)depth;
  (void)decl_stmt;
  (void)name;
  (void)escapes;
  (void)init;
  if (bail_reason)
    *bail_reason = NULL;
  return false;
}

static void stmt_update_static_int_list_elide_metadata(
    binding *b, bool candidate, bool lowered, const char *bail_reason) {
  if (!b)
    return;
  b->static_int_list_elide_candidate = candidate;
  b->static_int_list_elide_lowered = lowered;
  b->static_int_list_elide_bail_reason = lowered ? NULL : bail_reason;
  if (candidate && lowered)
    b->static_indexable_invalid = false;
}

static bool stmt_expr_raw_mut_list_only_uses(codegen_t *cg, scope *scopes,
                                             size_t depth, expr_t *e,
                                             const char *name);
static bool stmt_raw_mut_list_only_uses(codegen_t *cg, scope *scopes,
                                        size_t depth, stmt_t *s,
                                        stmt_t *decl_stmt, const char *name);

static size_t stmt_count_mut_int_list_literals(codegen_t *cg, scope *scopes,
                                               size_t depth, stmt_t *s) {
  if (!s)
    return 0;
  switch (s->kind) {
  case NY_S_BLOCK: {
    size_t n = 0;
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      n += stmt_count_mut_int_list_literals(cg, scopes, depth,
                                            s->as.block.body.data[i]);
    return n;
  }
  case NY_S_MODULE: {
    size_t n = 0;
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      n += stmt_count_mut_int_list_literals(cg, scopes, depth,
                                            s->as.module.body.data[i]);
    return n;
  }
  case NY_S_VAR: {
    if (!s->as.var.is_mut)
      return 0;
    size_t n = 0;
    size_t lim = s->as.var.names.len < s->as.var.exprs.len
                     ? s->as.var.names.len
                     : s->as.var.exprs.len;
    for (size_t i = 0; i < lim; ++i) {
      if (stmt_expr_is_int_list_literal(cg, scopes, depth,
                                        s->as.var.exprs.data[i]))
        ++n;
    }
    return n;
  }
  case NY_S_IF:
    return stmt_count_mut_int_list_literals(cg, scopes, depth, s->as.iff.init) +
           stmt_count_mut_int_list_literals(cg, scopes, depth,
                                            s->as.iff.conseq) +
           stmt_count_mut_int_list_literals(cg, scopes, depth, s->as.iff.alt);
  case NY_S_WHILE:
    return stmt_count_mut_int_list_literals(cg, scopes, depth, s->as.whl.init) +
           stmt_count_mut_int_list_literals(cg, scopes, depth, s->as.whl.body) +
           stmt_count_mut_int_list_literals(cg, scopes, depth,
                                            s->as.whl.update);
  case NY_S_FOR:
    return stmt_count_mut_int_list_literals(cg, scopes, depth, s->as.fr.init) +
           stmt_count_mut_int_list_literals(cg, scopes, depth, s->as.fr.body) +
           stmt_count_mut_int_list_literals(cg, scopes, depth, s->as.fr.update);
  case NY_S_GUARD:
    return stmt_count_mut_int_list_literals(cg, scopes, depth,
                                            s->as.guard.fallback);
  case NY_S_TRY:
    return stmt_count_mut_int_list_literals(cg, scopes, depth, s->as.tr.body) +
           stmt_count_mut_int_list_literals(cg, scopes, depth,
                                            s->as.tr.handler);
  case NY_S_DEFER:
    return stmt_count_mut_int_list_literals(cg, scopes, depth, s->as.de.body);
  case NY_S_FUNC:
  case NY_S_MACRO:
    return 0;
  default:
    return 0;
  }
}

static bool stmt_call_args_raw_mut_list_only_uses(codegen_t *cg, scope *scopes,
                                                  size_t depth,
                                                  ny_call_arg_list *args,
                                                  size_t start,
                                                  const char *name) {
  if (!args)
    return true;
  for (size_t i = start; i < args->len; ++i) {
    if (!stmt_expr_raw_mut_list_only_uses(cg, scopes, depth, args->data[i].val,
                                          name))
      return false;
  }
  return true;
}

static bool stmt_expr_raw_mut_list_only_uses(codegen_t *cg, scope *scopes,
                                             size_t depth, expr_t *e,
                                             const char *name) {
  if (!e)
    return true;
  switch (e->kind) {
  case NY_E_IDENT:
    return !ny_expr_ident_is_name(e, name);
  case NY_E_LITERAL:
  case NY_E_EMBED:
  case NY_E_INFERRED_MEMBER:
    return true;
  case NY_E_UNARY:
    return stmt_expr_raw_mut_list_only_uses(cg, scopes, depth,
                                            e->as.unary.right, name);
  case NY_E_BINARY:
    return stmt_expr_raw_mut_list_only_uses(cg, scopes, depth,
                                            e->as.binary.left, name) &&
           stmt_expr_raw_mut_list_only_uses(cg, scopes, depth,
                                            e->as.binary.right, name);
  case NY_E_LOGICAL:
    return stmt_expr_raw_mut_list_only_uses(cg, scopes, depth,
                                            e->as.logical.left, name) &&
           stmt_expr_raw_mut_list_only_uses(cg, scopes, depth,
                                            e->as.logical.right, name);
  case NY_E_TERNARY:
    return stmt_expr_raw_mut_list_only_uses(cg, scopes, depth,
                                            e->as.ternary.cond, name) &&
           stmt_expr_raw_mut_list_only_uses(cg, scopes, depth,
                                            e->as.ternary.true_expr, name) &&
           stmt_expr_raw_mut_list_only_uses(cg, scopes, depth,
                                            e->as.ternary.false_expr, name);
  case NY_E_CALL: {
    const char *callee =
        (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT)
            ? e->as.call.callee->as.ident.name
            : NULL;
    if (callee && (e->as.call.args.len == 2 || e->as.call.args.len == 3) &&
        stmt_call_name_is_unshadowed_builtin(cg, scopes, depth, callee,
                                             "get") &&
        stmt_expr_is_static_list_builtin_target(e->as.call.args.data[0].val,
                                                name)) {
      if (!stmt_expr_is_int_index_expr(cg, scopes, depth,
                                       e->as.call.args.data[1].val))
        return false;
      return stmt_call_args_raw_mut_list_only_uses(cg, scopes, depth,
                                                   &e->as.call.args, 1, name);
    }
    if (callee && e->as.call.args.len == 3 &&
        ny_name_tail_is(callee, "set_idx") &&
        stmt_expr_is_static_list_builtin_target(e->as.call.args.data[0].val,
                                                name)) {
      if (!stmt_expr_is_int_index_expr(cg, scopes, depth,
                                       e->as.call.args.data[1].val))
        return false;
      if (ny_fast_path_enabled(cg, "NYTRIX_UNTAGGED_INT_LIST_STORAGE") &&
          !ny_is_proven_int(cg, scopes, depth, e->as.call.args.data[2].val,
                            NULL))
        return false;
      return stmt_call_args_raw_mut_list_only_uses(cg, scopes, depth,
                                                   &e->as.call.args, 1, name);
    }
    if (callee && e->as.call.args.len == 1 &&
        stmt_call_name_is_unshadowed_builtin(cg, scopes, depth, callee,
                                             "len") &&
        stmt_expr_is_static_list_builtin_target(e->as.call.args.data[0].val,
                                                name)) {
      return true;
    }
    if (!stmt_expr_raw_mut_list_only_uses(cg, scopes, depth, e->as.call.callee,
                                          name))
      return false;
    return stmt_call_args_raw_mut_list_only_uses(cg, scopes, depth,
                                                 &e->as.call.args, 0, name);
  }
  case NY_E_MEMCALL:
    return stmt_expr_raw_mut_list_only_uses(cg, scopes, depth,
                                            e->as.memcall.target, name) &&
           stmt_call_args_raw_mut_list_only_uses(cg, scopes, depth,
                                                 &e->as.memcall.args, 0, name);
  case NY_E_INDEX:
    return stmt_expr_raw_mut_list_only_uses(cg, scopes, depth,
                                            e->as.index.target, name) &&
           stmt_expr_raw_mut_list_only_uses(cg, scopes, depth,
                                            e->as.index.start, name) &&
           stmt_expr_raw_mut_list_only_uses(cg, scopes, depth, e->as.index.stop,
                                            name) &&
           stmt_expr_raw_mut_list_only_uses(cg, scopes, depth, e->as.index.step,
                                            name);
  case NY_E_MEMBER:
    return stmt_expr_raw_mut_list_only_uses(cg, scopes, depth,
                                            e->as.member.target, name);
  case NY_E_PTR_TYPE:
    return stmt_expr_raw_mut_list_only_uses(cg, scopes, depth,
                                            e->as.ptr_type.target, name);
  case NY_E_DEREF:
    return stmt_expr_raw_mut_list_only_uses(cg, scopes, depth,
                                            e->as.deref.target, name);
  case NY_E_SIZEOF:
    return e->as.szof.is_type ||
           stmt_expr_raw_mut_list_only_uses(cg, scopes, depth,
                                            e->as.szof.target, name);
  case NY_E_TRY:
    return stmt_expr_raw_mut_list_only_uses(cg, scopes, depth,
                                            e->as.try_expr.target, name);
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i) {
      if (!stmt_expr_raw_mut_list_only_uses(cg, scopes, depth,
                                            e->as.list_like.data[i], name))
        return false;
    }
    return true;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      if (!stmt_expr_raw_mut_list_only_uses(
              cg, scopes, depth, e->as.dict.pairs.data[i].key, name) ||
          !stmt_expr_raw_mut_list_only_uses(
              cg, scopes, depth, e->as.dict.pairs.data[i].value, name))
        return false;
    }
    return true;
  default:
    return false;
  }
}

static bool stmt_raw_mut_list_only_uses(codegen_t *cg, scope *scopes,
                                        size_t depth, stmt_t *s,
                                        stmt_t *decl_stmt, const char *name) {
  if (!s)
    return true;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      if (!stmt_raw_mut_list_only_uses(
              cg, scopes, depth, s->as.block.body.data[i], decl_stmt, name))
        return false;
    }
    return true;
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; ++i) {
      if (!stmt_raw_mut_list_only_uses(
              cg, scopes, depth, s->as.module.body.data[i], decl_stmt, name))
        return false;
    }
    return true;
  case NY_S_VAR: {
    if (s == decl_stmt)
      return true;
    bool assigns_target = false;
    for (size_t i = 0; i < s->as.var.names.len; ++i) {
      if (s->as.var.names.data[i] && strcmp(s->as.var.names.data[i], name) == 0)
        assigns_target = true;
    }
    if (assigns_target) {
      if (s->as.var.names.len != 1 || s->as.var.exprs.len != 1)
        return false;
      expr_t *set_value = NULL;
      if (!stmt_expr_is_set_idx_to_name(s->as.var.exprs.data[0], name,
                                        &set_value))
        return false;
      return stmt_expr_raw_mut_list_only_uses(cg, scopes, depth,
                                              s->as.var.exprs.data[0], name);
    }
    for (size_t i = 0; i < s->as.var.exprs.len; ++i) {
      if (!stmt_expr_raw_mut_list_only_uses(cg, scopes, depth,
                                            s->as.var.exprs.data[i], name))
        return false;
    }
    return true;
  }
  case NY_S_EXPR:
    return stmt_expr_raw_mut_list_only_uses(cg, scopes, depth, s->as.expr.expr,
                                            name);
  case NY_S_IF:
    return stmt_expr_raw_mut_list_only_uses(cg, scopes, depth, s->as.iff.test,
                                            name) &&
           stmt_raw_mut_list_only_uses(cg, scopes, depth, s->as.iff.init,
                                       decl_stmt, name) &&
           stmt_raw_mut_list_only_uses(cg, scopes, depth, s->as.iff.conseq,
                                       decl_stmt, name) &&
           stmt_raw_mut_list_only_uses(cg, scopes, depth, s->as.iff.alt,
                                       decl_stmt, name);
  case NY_S_WHILE:
    return stmt_raw_mut_list_only_uses(cg, scopes, depth, s->as.whl.init,
                                       decl_stmt, name) &&
           stmt_expr_raw_mut_list_only_uses(cg, scopes, depth, s->as.whl.test,
                                            name) &&
           stmt_raw_mut_list_only_uses(cg, scopes, depth, s->as.whl.body,
                                       decl_stmt, name) &&
           stmt_raw_mut_list_only_uses(cg, scopes, depth, s->as.whl.update,
                                       decl_stmt, name);
  case NY_S_FOR:
    if (s->as.fr.iter_var && strcmp(s->as.fr.iter_var, name) == 0)
      return false;
    if (s->as.fr.iter_index_var && strcmp(s->as.fr.iter_index_var, name) == 0)
      return false;
    return stmt_raw_mut_list_only_uses(cg, scopes, depth, s->as.fr.init,
                                       decl_stmt, name) &&
           stmt_expr_raw_mut_list_only_uses(cg, scopes, depth, s->as.fr.cond,
                                            name) &&
           stmt_expr_raw_mut_list_only_uses(cg, scopes, depth,
                                            s->as.fr.iterable, name) &&
           stmt_raw_mut_list_only_uses(cg, scopes, depth, s->as.fr.body,
                                       decl_stmt, name) &&
           stmt_raw_mut_list_only_uses(cg, scopes, depth, s->as.fr.update,
                                       decl_stmt, name);
  case NY_S_RETURN:
    return stmt_expr_raw_mut_list_only_uses(cg, scopes, depth, s->as.ret.value,
                                            name);
  case NY_S_GUARD:
    return stmt_expr_raw_mut_list_only_uses(cg, scopes, depth,
                                            s->as.guard.value, name) &&
           stmt_raw_mut_list_only_uses(cg, scopes, depth, s->as.guard.fallback,
                                       decl_stmt, name);
  case NY_S_TRY:
    return stmt_raw_mut_list_only_uses(cg, scopes, depth, s->as.tr.body,
                                       decl_stmt, name) &&
           stmt_raw_mut_list_only_uses(cg, scopes, depth, s->as.tr.handler,
                                       decl_stmt, name);
  case NY_S_DEFER:
    return stmt_raw_mut_list_only_uses(cg, scopes, depth, s->as.de.body,
                                       decl_stmt, name);
  case NY_S_FUNC:
  case NY_S_MACRO:
    return false;
  default:
    return true;
  }
}

static bool stmt_can_use_raw_int_list_mutation(codegen_t *cg, scope *scopes,
                                               size_t depth, stmt_t *decl_stmt,
                                               const char *name, bool escapes,
                                               expr_t *init,
                                               const char **bail_reason) {
  if (bail_reason)
    *bail_reason = NULL;
  if (!ny_env_enabled_default_on("NYTRIX_RAW_INT_LIST_MUTATION"))
    return false;
  if (!cg || !name || !decl_stmt) {
    if (bail_reason)
      *bail_reason = "internal";
    return false;
  }
  if (!decl_stmt->as.var.is_mut) {
    if (bail_reason)
      *bail_reason = "immutable";
    return false;
  }
  if (escapes) {
    if (bail_reason)
      *bail_reason = "escapes";
    return false;
  }
  if (!stmt_expr_is_int_list_literal(cg, scopes, depth, init)) {
    if (bail_reason)
      *bail_reason = "mixed-type";
    return false;
  }
  if (ny_expr_is_list_or_tuple_lit(init) && init->as.list_like.len == 0) {
    if (bail_reason)
      *bail_reason = "empty-list";
    return false;
  }
  if (!ny_env_enabled("NYTRIX_RAW_INT_LIST_MUTATION_ALL") &&
      ny_expr_is_list_or_tuple_lit(init)) {
    int max_len =
        ny_env_int_range("NYTRIX_RAW_INT_LIST_MUTATION_MAX_LEN", 256, 1, 4096);
    if (init->as.list_like.len > (size_t)max_len) {
      if (bail_reason)
        *bail_reason = "too-large";
      return false;
    }
  }
  stmt_t *root = cg->current_fn_body ? cg->current_fn_body : decl_stmt;
  if (!ny_env_enabled("NYTRIX_RAW_INT_LIST_MUTATION_ALL")) {
    int max_bindings =
        ny_env_int_range("NYTRIX_RAW_INT_LIST_MUTATION_MAX_BINDINGS", 8, 1, 64);
    size_t candidate_count =
        stmt_count_mut_int_list_literals(cg, scopes, depth, root);
    if (candidate_count > (size_t)max_bindings) {
      if (bail_reason)
        *bail_reason = "too-many-candidates-cap";
      return false;
    }
  }
  if (!stmt_raw_mut_list_only_uses(cg, scopes, depth, root, decl_stmt, name)) {
    if (bail_reason)
      *bail_reason = stmt_list_fastpath_bail_reason(cg, scopes, depth, root,
                                                    decl_stmt, name, true);
    if (bail_reason && !*bail_reason)
      *bail_reason = "unsupported-use";
    return false;
  }
  return true;
}

static void stmt_init_raw_int_list_storage(codegen_t *cg, scope *scopes,
                                           size_t depth, binding *b,
                                           expr_t *init) {
  if (!cg || !b || !init || !ny_expr_is_list_or_tuple_lit(init))
    return;
  size_t len = init->as.list_like.len;
  LLVMTypeRef array_ty = LLVMArrayType(cg->type_i64, (unsigned)(len ? len : 1));
  LLVMValueRef storage = build_alloca(cg, "raw.int.list", array_ty);
  if (!storage)
    return;
  bool untagged =
      ny_fast_path_enabled(cg, "NYTRIX_UNTAGGED_INT_LIST_STORAGE");
  for (size_t i = 0; i < (len ? len : 1); ++i) {
    int64_t raw = 0;
    if (i < len)
      (void)ny_expr_literal_i64(init->as.list_like.data[i], &raw);
    LLVMValueRef idxs[2] = {ny_c0(cg),
                            LLVMConstInt(cg->type_i64, (uint64_t)i, false)};
    LLVMValueRef ptr = LLVMBuildInBoundsGEP2(cg->builder, array_ty, storage,
                                             idxs, 2, "raw_int_list_init_ptr");
    uint64_t v = untagged ? (uint64_t)raw : (((uint64_t)raw) << 1) | 1u;
    ny_store(cg, ptr, LLVMConstInt(cg->type_i64, v, false));
  }
  LLVMValueRef first_idxs[2] = {ny_c0(cg), ny_c0(cg)};
  b->raw_int_list_ptr = LLVMBuildInBoundsGEP2(
      cg->builder, array_ty, storage, first_idxs, 2, "raw_int_list_ptr");
  b->raw_int_list_len = len;
  b->raw_int_list_mutation = true;
  b->raw_int_list_untagged = untagged;
  if (untagged && cg->module) {
    LLVMMetadataRef s =
        LLVMMDStringInContext2(cg->ctx, "raw_int_list_untagged", 21);
    LLVMMetadataRef md = LLVMMDNodeInContext2(cg->ctx, &s, 1);
    LLVMAddNamedMetadataOperand(cg->module, "nytrix.untagged_list",
                                LLVMMetadataAsValue(cg->ctx, md));
  }
  b->raw_int_list_bail_reason = NULL;
  (void)scopes;
  (void)depth;
}

static void stmt_update_list_binding_proof(binding *b, bool is_list_storage,
                                           bool is_int_list_storage,
                                           bool is_f64_list_storage) {
  if (!b)
    return;
  NY_COMPILER_ASSERT(!is_int_list_storage || is_list_storage,
                     "int-list proof requires list storage proof");
  NY_COMPILER_ASSERT(!is_f64_list_storage || is_list_storage,
                     "f64-list proof requires list storage proof");
  b->is_list_storage = is_list_storage;
  b->is_int_list_storage = is_list_storage && is_int_list_storage;
  b->is_f64_list_storage = is_list_storage && is_f64_list_storage;
  if (!b->is_list_storage) {
    b->has_list_len_min = false;
    b->has_list_int_range = false;
    b->is_f64_list_storage = false;
    b->raw_int_list_ptr = NULL;
    b->raw_int_list_len = 0;
    b->raw_int_list_mutation = false;
    b->raw_int_list_bail_reason = "list proof lost";
    return;
  }
  if (!b->is_int_list_storage) {
    b->has_list_int_range = false;
    if (b->raw_int_list_untagged) {
      b->raw_int_list_ptr = NULL;
      b->raw_int_list_len = 0;
      b->raw_int_list_mutation = false;
      b->raw_int_list_bail_reason = "int proof lost";
    }
  }
}

static void stmt_update_dict_binding_proof(binding *b, bool is_dict_storage,
                                           bool is_int_dict_storage) {
  if (!b)
    return;
  NY_COMPILER_ASSERT(!is_int_dict_storage || is_dict_storage,
                     "int-dict proof requires dict storage proof");
  b->is_dict_storage = is_dict_storage;
  b->is_int_dict_storage = is_dict_storage && is_int_dict_storage;
  if (!b->is_int_dict_storage)
    b->has_dict_int_range = false;
}

static void stmt_update_list_binding_range(binding *b, bool has_range,
                                           int64_t min_raw, int64_t max_raw) {
  if (!b || !b->is_int_list_storage) {
    if (b)
      b->has_list_int_range = false;
    return;
  }
  b->has_list_int_range = has_range;
  if (has_range) {
    NY_COMPILER_ASSERTF(min_raw <= max_raw,
                        "list range inverted: min=%lld max=%lld",
                        (long long)min_raw, (long long)max_raw);
    b->list_int_min_raw = min_raw;
    b->list_int_max_raw = max_raw;
  }
}

static void stmt_update_list_binding_len_min(binding *b, bool has_len_min,
                                             int64_t len_min_raw) {
  if (!b || !b->is_list_storage) {
    if (b)
      b->has_list_len_min = false;
    return;
  }
  if (!has_len_min || len_min_raw < 0) {
    b->has_list_len_min = false;
    return;
  }
  b->has_list_len_min = true;
  b->list_len_min_raw = len_min_raw;
}

static void stmt_update_direct_callable_binding(codegen_t *cg, binding *b,
                                                expr_t *rhs) {
  if (!b)
    return;
  b->direct_callable_sig = NULL;
  if (!cg || !rhs || b->is_mut)
    return;
  if ((rhs->kind == NY_E_LAMBDA || rhs->kind == NY_E_FN) &&
      cg->last_lambda_sig && ny_sig_in_current_sigs(cg, cg->last_lambda_sig))
    b->direct_callable_sig = cg->last_lambda_sig;
}

static void stmt_invalidate_static_indexable(binding *b) {
  if (!b)
    return;
  b->static_indexable_invalid = true;
  b->static_indexable_object_elided = false;
  b->static_int_list_elide_lowered = false;
  if (b->static_int_list_elide_candidate)
    b->static_int_list_elide_bail_reason = "invalidated";
  b->static_int_list_global = NULL;
  b->static_int_list_len = 0;
}

static void stmt_invalidate_static_indexable_name(codegen_t *cg, scope *scopes,
                                                  size_t depth,
                                                  const char *name) {
  if (!name)
    return;
  size_t nlen = strlen(name);
  stmt_invalidate_static_indexable(
      stmt_lookup_binding(cg, scopes, depth, name, nlen, 0));
}

static const char *stmt_expr_set_idx_target_name(expr_t *e) {
  if (!e || e->kind != NY_E_CALL || !e->as.call.callee ||
      e->as.call.callee->kind != NY_E_IDENT ||
      !e->as.call.callee->as.ident.name || e->as.call.args.len < 3)
    return NULL;
  if (!ny_name_tail_is(e->as.call.callee->as.ident.name, "set_idx"))
    return NULL;
  expr_t *target = e->as.call.args.data[0].val;
  return (target && target->kind == NY_E_IDENT) ? target->as.ident.name : NULL;
}

static void stmt_preinvalidate_loop_static_indexables(codegen_t *cg,
                                                      scope *scopes,
                                                      size_t depth, stmt_t *s) {
  if (!s)
    return;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      stmt_preinvalidate_loop_static_indexables(cg, scopes, depth,
                                                s->as.block.body.data[i]);
    break;
  case NY_S_VAR:
    if (!s->as.var.is_decl) {
      for (size_t i = 0; i < s->as.var.names.len; ++i)
        stmt_invalidate_static_indexable_name(cg, scopes, depth,
                                              s->as.var.names.data[i]);
      for (size_t i = 0; i < s->as.var.exprs.len; ++i)
        stmt_invalidate_static_indexable_name(
            cg, scopes, depth,
            stmt_expr_set_idx_target_name(s->as.var.exprs.data[i]));
    }
    break;
  case NY_S_EXPR:
    stmt_invalidate_static_indexable_name(
        cg, scopes, depth, stmt_expr_set_idx_target_name(s->as.expr.expr));
    break;
  case NY_S_IF:
    stmt_preinvalidate_loop_static_indexables(cg, scopes, depth,
                                              s->as.iff.conseq);
    stmt_preinvalidate_loop_static_indexables(cg, scopes, depth, s->as.iff.alt);
    break;
  case NY_S_WHILE:
    stmt_preinvalidate_loop_static_indexables(cg, scopes, depth,
                                              s->as.whl.body);
    break;
  case NY_S_FOR:
    stmt_preinvalidate_loop_static_indexables(cg, scopes, depth, s->as.fr.body);
    break;
  default:
    break;
  }
}

static void stmt_update_dict_binding_range(binding *b, bool has_range,
                                           int64_t min_raw, int64_t max_raw) {
  if (!b || !b->is_int_dict_storage) {
    if (b)
      b->has_dict_int_range = false;
    return;
  }
  b->has_dict_int_range = has_range;
  if (has_range) {
    NY_COMPILER_ASSERTF(min_raw <= max_raw,
                        "dict range inverted: min=%lld max=%lld",
                        (long long)min_raw, (long long)max_raw);
    b->dict_int_min_raw = min_raw;
    b->dict_int_max_raw = max_raw;
  }
}

static bool stmt_expr_int_range(codegen_t *cg, scope *scopes, size_t depth,
                                expr_t *e, int64_t *out_min, int64_t *out_max);

typedef struct stmt_binding_int_snapshot_t {
  binding *b;
  bool active;
  bool is_int_slot;
  bool is_int_direct;
  bool is_int_raw_direct;
  bool has_int_range;
  int64_t int_min_raw;
  int64_t int_max_raw;
} stmt_binding_int_snapshot_t;

static stmt_binding_int_snapshot_t stmt_snapshot_binding_int_proof(binding *b) {
  stmt_binding_int_snapshot_t snap = {0};
  if (!b)
    return snap;
  snap.b = b;
  snap.active = true;
  snap.is_int_slot = b->is_int_slot;
  snap.is_int_direct = b->is_int_direct;
  snap.is_int_raw_direct = b->is_int_raw_direct;
  snap.has_int_range = b->has_int_range;
  snap.int_min_raw = b->int_min_raw;
  snap.int_max_raw = b->int_max_raw;
  return snap;
}

static void stmt_restore_binding_int_proof(stmt_binding_int_snapshot_t snap) {
  if (!snap.active || !snap.b)
    return;
  snap.b->is_int_slot = snap.is_int_slot;
  snap.b->is_int_direct = snap.is_int_direct;
  snap.b->is_int_raw_direct = snap.is_int_raw_direct;
  snap.b->has_int_range = snap.has_int_range;
  snap.b->int_min_raw = snap.int_min_raw;
  snap.b->int_max_raw = snap.int_max_raw;
}

static void
stmt_restore_binding_int_proof_if_still_int(stmt_binding_int_snapshot_t snap) {
  if (!snap.active || !snap.b)
    return;
  if ((snap.is_int_slot || snap.is_int_direct) &&
      !(snap.b->is_int_slot || snap.b->is_int_direct))
    return;
  stmt_restore_binding_int_proof(snap);
}

static binding *stmt_while_lhs_binding(codegen_t *cg, scope *scopes,
                                       size_t depth, stmt_t *s) {
  if (!cg || !scopes || !s || s->kind != NY_S_WHILE || !s->as.whl.test)
    return NULL;
  expr_t *test = s->as.whl.test;
  while (test && test->kind == NY_E_LOGICAL && test->as.logical.op &&
         strcmp(test->as.logical.op, "&&") == 0)
    test = test->as.logical.left;
  if (test->kind != NY_E_BINARY || !test->as.binary.op)
    return NULL;
  if (strcmp(test->as.binary.op, "<") != 0 &&
      strcmp(test->as.binary.op, "<=") != 0)
    return NULL;
  expr_t *lhs = test->as.binary.left;
  if (!lhs || lhs->kind != NY_E_IDENT || !lhs->as.ident.name)
    return NULL;
  size_t name_len = (size_t)lhs->tok.len;
  if (name_len == 0)
    name_len = strlen(lhs->as.ident.name);
  binding *b = stmt_lookup_binding(cg, scopes, depth, lhs->as.ident.name,
                                   name_len, lhs->as.ident.hash);
  return b;
}

static bool stmt_expr_is_ident_plus_const(expr_t *e, const char *name,
                                          int64_t *delta_out) {
  if (delta_out)
    *delta_out = 0;
  if (!e || e->kind != NY_E_BINARY || !e->as.binary.op || !name)
    return false;
  if (strcmp(e->as.binary.op, "+") != 0 && strcmp(e->as.binary.op, "-") != 0)
    return false;
  expr_t *lhs = e->as.binary.left;
  expr_t *rhs = e->as.binary.right;
  if (!lhs || lhs->kind != NY_E_IDENT || !lhs->as.ident.name ||
      strcmp(lhs->as.ident.name, name) != 0)
    return false;
  if (!rhs || rhs->kind != NY_E_LITERAL || rhs->as.literal.kind != NY_LIT_INT)
    return false;
  int64_t delta = rhs->as.literal.as.i;
  if (strcmp(e->as.binary.op, "-") == 0)
    delta = -delta;
  if (delta_out)
    *delta_out = delta;
  return true;
}

static bool stmt_try_while_trip_upper_bound(codegen_t *cg, scope *scopes,
                                            size_t depth, stmt_t *s,
                                            int64_t *trip_out);
static void stmt_preseed_loop_accumulator_ranges(codegen_t *cg, scope *scopes,
                                                 size_t depth, stmt_t *body,
                                                 int64_t trip_count,
                                                 const char *skip_name);

static bool stmt_int_list_literal_range(codegen_t *cg, scope *scopes,
                                        size_t depth, expr_t *e,
                                        int64_t *out_min, int64_t *out_max) {
  if (!e || e->kind != NY_E_LIST || e->as.list_like.len == 0)
    return false;
  int64_t min_v = 0;
  int64_t max_v = 0;
  for (size_t i = 0; i < e->as.list_like.len; ++i) {
    int64_t lo = 0, hi = 0;
    if (!stmt_expr_int_range(cg, scopes, depth, e->as.list_like.data[i], &lo,
                             &hi) ||
        lo != hi)
      return false;
    if (i == 0) {
      min_v = lo;
      max_v = hi;
    } else {
      if (lo < min_v)
        min_v = lo;
      if (hi > max_v)
        max_v = hi;
    }
  }
  if (out_min)
    *out_min = min_v;
  if (out_max)
    *out_max = max_v;
  return true;
}

static bool stmt_expr_is_append_to_name(expr_t *e, const char *name,
                                        expr_t **out_value) {
  if (out_value)
    *out_value = NULL;
  if (!e || !name)
    return false;
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT &&
      e->as.call.callee->as.ident.name) {
    const char *callee = e->as.call.callee->as.ident.name;
    if (!ny_name_tail_is(callee, "append") || e->as.call.args.len < 2)
      return false;
    expr_t *target = e->as.call.args.data[0].val;
    if (!target || target->kind != NY_E_IDENT || !target->as.ident.name ||
        strcmp(target->as.ident.name, name) != 0)
      return false;
    if (out_value)
      *out_value = e->as.call.args.data[1].val;
    return true;
  }
  if (e->kind == NY_E_MEMCALL && e->as.memcall.name &&
      ny_name_tail_is(e->as.memcall.name, "append") &&
      e->as.memcall.target && e->as.memcall.target->kind == NY_E_IDENT &&
      e->as.memcall.target->as.ident.name &&
      strcmp(e->as.memcall.target->as.ident.name, name) == 0 &&
      e->as.memcall.args.len >= 1) {
    if (out_value)
      *out_value = e->as.memcall.args.data[0].val;
    return true;
  }
  return false;
}

static bool stmt_expr_is_set_idx_to_name(expr_t *e, const char *name,
                                         expr_t **out_value) {
  if (out_value)
    *out_value = NULL;
  if (!e || !name)
    return false;
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT &&
      e->as.call.callee->as.ident.name) {
    const char *callee = e->as.call.callee->as.ident.name;
    if (!ny_name_tail_is(callee, "set_idx") || e->as.call.args.len < 3)
      return false;
    expr_t *target = e->as.call.args.data[0].val;
    if (!target || target->kind != NY_E_IDENT || !target->as.ident.name ||
        strcmp(target->as.ident.name, name) != 0)
      return false;
    if (out_value)
      *out_value = e->as.call.args.data[2].val;
    return true;
  }
  if (e->kind == NY_E_MEMCALL && e->as.memcall.name &&
      (ny_name_tail_is(e->as.memcall.name, "set_idx") ||
       ny_name_tail_is(e->as.memcall.name, "set")) &&
      e->as.memcall.target && e->as.memcall.target->kind == NY_E_IDENT &&
      e->as.memcall.target->as.ident.name &&
      strcmp(e->as.memcall.target->as.ident.name, name) == 0 &&
      e->as.memcall.args.len >= 2) {
    if (out_value)
      *out_value = e->as.memcall.args.data[1].val;
    return true;
  }
  return false;
}

static bool stmt_find_loop_step_in_stmt(stmt_t *s, const char *name,
                                        int64_t *delta_out) {
  if (delta_out)
    *delta_out = 0;
  if (!s || !name)
    return false;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      if (stmt_find_loop_step_in_stmt(s->as.block.body.data[i], name,
                                      delta_out))
        return true;
    }
    return false;
  case NY_S_VAR:
    if (s->as.var.is_decl)
      return false;
    for (size_t i = 0; i < s->as.var.names.len && i < s->as.var.exprs.len;
         ++i) {
      const char *n = s->as.var.names.data[i];
      if (!n || strcmp(n, name) != 0)
        continue;
      return stmt_expr_is_ident_plus_const(s->as.var.exprs.data[i], name,
                                           delta_out);
    }
    return false;
  default:
    return false;
  }
}

static bool stmt_loop_assigns_name_unproven_int(codegen_t *cg, scope *scopes,
                                                size_t depth, stmt_t *s,
                                                const char *name) {
  if (!s || !name)
    return false;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (stmt_loop_assigns_name_unproven_int(cg, scopes, depth,
                                              s->as.block.body.data[i], name))
        return true;
    return false;
  case NY_S_VAR:
    if (s->as.var.is_decl)
      return false;
    for (size_t i = 0; i < s->as.var.names.len; ++i) {
      const char *n = s->as.var.names.data[i];
      if (!n || strcmp(n, name) != 0)
        continue;
      if (s->as.var.is_del || s->as.var.is_destructure ||
          i >= s->as.var.exprs.len)
        return true;
      int64_t delta = 0;
      if (stmt_find_loop_step_in_stmt(s, name, &delta))
        continue;
      if (!ny_is_proven_int(cg, scopes, depth, s->as.var.exprs.data[i], NULL))
        return true;
    }
    return false;
  case NY_S_IF:
    return stmt_loop_assigns_name_unproven_int(cg, scopes, depth,
                                               s->as.iff.init, name) ||
           stmt_loop_assigns_name_unproven_int(cg, scopes, depth,
                                               s->as.iff.conseq, name) ||
           stmt_loop_assigns_name_unproven_int(cg, scopes, depth,
                                               s->as.iff.alt, name);
  case NY_S_GUARD:
    return stmt_loop_assigns_name_unproven_int(cg, scopes, depth,
                                               s->as.guard.fallback, name);
  case NY_S_WHILE:
    return stmt_loop_assigns_name_unproven_int(cg, scopes, depth,
                                               s->as.whl.init, name) ||
           stmt_loop_assigns_name_unproven_int(cg, scopes, depth,
                                               s->as.whl.body, name) ||
           stmt_loop_assigns_name_unproven_int(cg, scopes, depth,
                                               s->as.whl.update, name);
  case NY_S_FOR:
    return stmt_loop_assigns_name_unproven_int(cg, scopes, depth,
                                               s->as.fr.init, name) ||
           stmt_loop_assigns_name_unproven_int(cg, scopes, depth,
                                               s->as.fr.body, name) ||
           stmt_loop_assigns_name_unproven_int(cg, scopes, depth,
                                               s->as.fr.update, name);
  case NY_S_TRY:
    return stmt_loop_assigns_name_unproven_int(cg, scopes, depth,
                                               s->as.tr.body, name) ||
           stmt_loop_assigns_name_unproven_int(cg, scopes, depth,
                                               s->as.tr.handler, name);
  case NY_S_DEFER:
    return stmt_loop_assigns_name_unproven_int(cg, scopes, depth,
                                               s->as.de.body, name);
  case NY_S_MATCH:
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      if (stmt_loop_assigns_name_unproven_int(
              cg, scopes, depth, s->as.match.arms.data[i].conseq, name))
        return true;
    return stmt_loop_assigns_name_unproven_int(
        cg, scopes, depth, s->as.match.default_conseq, name);
  default:
    return false;
  }
}

static bool emit_active_panic_env_clear(codegen_t *cg, token_t tok) {
  if (!cg || cg->active_panic_envs == 0)
    return true;
  fun_sig *clr_env = lookup_fun(cg, "__clear_panic_env", 0);
  if (!clr_env) {
    ny_diag_error(tok, "return from try requires __clear_panic_env");
    if (cg)
      cg->had_error = 1;
    return false;
  }
  for (size_t i = 0; i < cg->active_panic_envs; ++i) {
    LLVMBuildCall2(cg->builder, clr_env->type, clr_env->value, NULL, 0, "");
  }
  return true;
}

static void scope_enter(scope *scopes, size_t *depth,
                        LLVMBasicBlockRef break_bb,
                        LLVMBasicBlockRef continue_bb) {
  int64_t inherited_loop_trip_hint = 0;
  if (scopes && *depth < SIZE_MAX)
    inherited_loop_trip_hint = scopes[*depth].loop_trip_hint;
  (*depth)++;
  memset(&scopes[*depth], 0, sizeof(scopes[*depth]));
  scopes[*depth].break_bb = break_bb;
  scopes[*depth].continue_bb = continue_bb;
  scopes[*depth].loop_trip_hint = inherited_loop_trip_hint;
}

static void report_reassign_immutable(codegen_t *cg, token_t tok, const char *n,
                                      bool is_global, const binding *decl) {
  ny_diag_error(tok, "cannot reassign immutable %svariable %s'%s'%s",
                is_global ? "global " : "", clr(NY_CLR_BOLD), n,
                clr(NY_CLR_RESET));
  if (decl && decl->stmt_t)
    ny_diag_note_tok(decl->stmt_t->tok,
                     "'%s' declared here (immutable by default)", n);
  ny_diag_fix("declare with %smut %s = ...%s instead", clr(NY_CLR_BOLD), n,
              clr(NY_CLR_RESET));
  cg->had_error = 1;
}

static bool ensure_mutable_binding_for_assign(codegen_t *cg, token_t tok,
                                              const char *name,
                                              const binding *b,
                                              bool is_global) {
  if (!b)
    return false;
  if (b->is_mut)
    return true;
  report_reassign_immutable(cg, tok, name, is_global, b);
  return false;
}

static void stmt_var_setup_local_binding(
    codegen_t *cg, scope *scopes, size_t depth, stmt_t *var_stmt,
    sema_var_t *sema, size_t idx, const char *name, const char *decl_type,
    bool decl_type_explicit, bool prefer_direct_locals, bool *bind_direct,
    LLVMValueRef *slot) {
  if (!cg || !scopes || !var_stmt || !name || !bind_direct || !slot)
    return;
  if (prefer_direct_locals ||
      can_bind_decl_direct(cg, name, var_stmt->as.var.is_mut)) {
    *bind_direct = true;
    return;
  }
  bool use_f64_slot = false;
  bool use_f32_slot = false;
  bool use_int_slot = false;
  LLVMTypeRef var_type = cg->type_i64;

  if (sema && sema->resolved_types.len > idx) {
    var_type = sema->resolved_types.data[idx];
    if (sema->is_int_proven.len > idx && sema->is_int_proven.data[idx])
      use_int_slot = true;
    if (sema->is_f64_proven.len > idx && sema->is_f64_proven.data[idx])
      use_f64_slot = true;
    if (!use_int_slot && !use_f64_slot && decl_type) {
      if (stmt_type_name_is_f64_value(decl_type))
        use_f64_slot = true;
      else if (stmt_type_name_is_f32_value(decl_type))
        use_f32_slot = true;
    }
  } else if (decl_type) {
    if (stmt_type_name_is_f64_value(decl_type)) {
      var_type = cg->type_f64;
      use_f64_slot = true;
    } else if (stmt_type_name_is_f32_value(decl_type)) {
      var_type = cg->type_f32;
      use_f32_slot = true;
    } else if (stmt_type_name_is_int_value(decl_type)) {
      use_int_slot = true;
    } else {
      var_type = resolve_type_name(cg, decl_type, var_stmt->tok);
    }
  }
  if (!use_int_slot && !use_f64_slot && !use_f32_slot && !decl_type &&
      var_stmt->as.var.exprs.len > idx &&
      ny_expr_literal_i64(var_stmt->as.var.exprs.data[idx], NULL))
    use_int_slot = true;
  *slot = build_alloca(cg, name, var_type);
  scope_bind(cg, scopes, depth, name, *slot, var_stmt, var_stmt->as.var.is_mut,
             decl_type, true);
  if (use_f64_slot || use_f32_slot || use_int_slot ||
      (sema && sema->escapes.len > idx)) {
    size_t nlen = strlen(name);
    binding *b = stmt_lookup_binding_no_mark(scopes, depth, name, nlen, 0);
    if (b) {
      if (!decl_type_explicit && !cg->strict_types)
        b->decl_type_name = NULL;
      b->is_f64_slot = use_f64_slot;
      b->is_f32_slot = use_f32_slot;
      b->is_int_slot = use_int_slot;
      if (use_int_slot && !b->raw_int_value)
        b->raw_int_value = build_alloca(cg, "raw.int", cg->type_i64);
      if (sema && sema->escapes.len > idx)
        b->escapes = sema->escapes.data[idx];
    }
  }
}

static binding *stmt_var_lookup_existing(codegen_t *cg, scope *scopes,
                                         size_t depth, const char *name,
                                         bool *is_global) {
  if (is_global)
    *is_global = false;
  if (!name)
    return NULL;
  binding *local = scope_lookup(scopes, depth, name);
  if (local)
    return local;
  if (is_global)
    *is_global = true;
  return lookup_binding_hash(cg, NULL, 0, name, 0, 0);
}

static bool stmt_top_entry_can_hoist_var(codegen_t *cg, const char *name) {
  if (!cg || !cg->top_entry_local_hoist_enabled || !name || !*name)
    return false;
  if (cg->emit_module_decls_only ||
      (cg->emit_module_name && *cg->emit_module_name))
    return false;
  if (cg->source_main_file && *cg->source_main_file) {
    const char *base = strrchr(cg->source_main_file, '/');
    base = base ? base + 1 : cg->source_main_file;
    if (strcmp(base, "std.ny") == 0)
      return false;
  }
  if (cg->current_module_name && *cg->current_module_name)
    return false;
  binding *global = lookup_global(cg, name);
  if (global && global->is_slot && global->stmt_t &&
      global->stmt_t->kind == NY_S_VAR) {
    if (!global->name || strcmp(global->name, name) != 0 ||
        strchr(global->name, '.') || ny_is_stdlib_tok(global->stmt_t->tok) ||
        !ny_codegen_stmt_is_source_file(cg, global->stmt_t))
      return false;
  }
  uint64_t hash = ny_hash64_cstr(name);
  bool blocked = ny_name_set_has_hash(
      cg->top_entry_blocked_names_data, cg->top_entry_blocked_names_len,
      cg->top_entry_blocked_hashes_data, cg->top_entry_blocked_hashes_len,
      cg->top_entry_blocked_bloom, name, hash);
  return !blocked;
}

static bool stmt_type_name_is_top_entry_numeric(const char *decl_type) {
  return stmt_type_name_is_int_value(decl_type) ||
         stmt_type_name_is_float_value(decl_type);
}

static bool stmt_top_entry_numeric_hoist(codegen_t *cg, scope *scopes,
                                         size_t depth, stmt_t *s,
                                         sema_var_t *sema, size_t idx,
                                         const char *decl_type, expr_t *init) {
  bool sema_numeric = false;
  if (sema) {
    bool sema_int =
        sema->is_int_proven.len > idx && sema->is_int_proven.data[idx];
    bool sema_f64 =
        sema->is_f64_proven.len > idx && sema->is_f64_proven.data[idx];
    sema_numeric = sema_int || sema_f64;
  }
  if (!s)
    return false;
  bool init_numeric = false;
  if (init) {
    if (ny_is_proven_int(cg, scopes, depth, init, NULL)) {
      init_numeric = true;
    } else {
      const char *init_type = infer_expr_type(cg, scopes, depth, init);
      init_numeric = stmt_type_name_is_float_value(init_type);
    }
  }
  return sema_numeric || init_numeric ||
         stmt_type_name_is_top_entry_numeric(decl_type);
}

static bool stmt_platform_ident_bool(const char *name, bool *out) {
  if (!name || !out)
    return false;
  const char *os = ny_host_os_name();
  const char *arch = ny_host_arch_name();
  bool is_windows = strcmp(os, "windows") == 0;
  bool is_macos = strcmp(os, "macos") == 0;
  bool is_linux = strcmp(os, "linux") == 0;
  bool is_unix = !is_windows && strcmp(os, "unknown") != 0;
  bool is_x86_64 = strcmp(arch, "x86_64") == 0;
  bool is_x86 = strcmp(arch, "x86") == 0 || is_x86_64;
  bool is_aarch64 = strcmp(arch, "aarch64") == 0 || strcmp(arch, "arm64") == 0;
  bool is_arm = strcmp(arch, "arm") == 0 || is_aarch64;
  bool is_riscv = strcmp(arch, "riscv") == 0;
  const struct {
    const char *symbol;
    bool value;
  } platform_bools[] = {
      {"linux", is_linux},       {"LINUX", is_linux},
      {"IS_LINUX", is_linux},   {"macos", is_macos},
      {"mac", is_macos},         {"MACOS", is_macos},
      {"IS_MACOS", is_macos},   {"windows", is_windows},
      {"IS_WINDOWS", is_windows},
      {"unix", is_unix},         {"posix", is_unix},
      {"UNIX", is_unix},         {"IS_UNIX", is_unix},
      {"x86_64", is_x86_64},     {"x64", is_x86_64},
      {"IS_X86_64", is_x86_64}, {"x86", is_x86},
      {"IS_X86", is_x86},       {"aarch64", is_aarch64},
      {"arm64", is_aarch64},     {"IS_AARCH64", is_aarch64},
      {"arm", is_arm},           {"IS_ARM", is_arm},
      {"riscv", is_riscv},       {"IS_RISCV", is_riscv},
  };
  for (size_t i = 0; i < sizeof(platform_bools) / sizeof(platform_bools[0]);
       i++) {
    if (strcmp(name, platform_bools[i].symbol) == 0) {
      *out = platform_bools[i].value;
      return true;
    }
  }
  return false;
}

static LLVMValueRef stmt_const_string_raw_ptr(codegen_t *cg, const char *s,
                                              size_t len) {
  LLVMValueRef runtime_global = const_string_ptr(cg, s ? s : "", len);
  if (!runtime_global)
    return NULL;
  for (size_t i = 0; i < cg->interns.len; i++) {
    string_intern *si = &cg->interns.data[i];
    if (si->val != runtime_global || !si->gv)
      continue;
    LLVMValueRef indices[] = {LLVMConstInt(cg->type_i64, 64, false)};
    LLVMValueRef data_ptr =
        LLVMConstInBoundsGEP2(ny_i8_ty(cg), si->gv, indices, 1);
    return LLVMConstPtrToInt(data_ptr, cg->type_i64);
  }
  return NULL;
}

static LLVMValueRef stmt_const_top_level_expr_value(codegen_t *cg,
                                                    expr_t *init) {
  if (!cg || !init)
    return NULL;
  if (init->kind == NY_E_LITERAL) {
    if (init->as.literal.kind == NY_LIT_INT) {
      if (!ny_small_int_fits_i64(init->as.literal.as.i))
        return NULL;
      uint64_t raw = (uint64_t)init->as.literal.as.i;
      return LLVMConstInt(cg->type_i64, (raw << 1) | UINT64_C(1), false);
    }
    if (init->as.literal.kind == NY_LIT_BOOL)
      return init->as.literal.as.b ? ny_ctrue(cg) : ny_cfalse(cg);
    if (init->as.literal.kind == NY_LIT_STR)
      return stmt_const_string_raw_ptr(cg, init->as.literal.as.s.data,
                                       init->as.literal.as.s.len);
    return NULL;
  }
  if (init->kind == NY_E_IDENT && init->as.ident.name) {
    bool b = false;
    if (stmt_platform_ident_bool(init->as.ident.name, &b))
      return b ? ny_ctrue(cg) : ny_cfalse(cg);
  }
  if (init->kind == NY_E_CALL && init->as.call.callee &&
      init->as.call.callee->kind == NY_E_IDENT &&
      init->as.call.callee->as.ident.name && init->as.call.args.len == 0) {
    const char *name = init->as.call.callee->as.ident.name;
    if (ny_name_tail_is(name, "__os_name"))
      return stmt_const_string_raw_ptr(cg, ny_host_os_name(),
                                       strlen(ny_host_os_name()));
    if (ny_name_tail_is(name, "__arch_name"))
      return stmt_const_string_raw_ptr(cg, ny_host_arch_name(),
                                       strlen(ny_host_arch_name()));
  }
  return NULL;
}

static bool ensure_store_ready(codegen_t *cg, token_t tok, LLVMValueRef value,
                               LLVMValueRef slot, const char *ctx) {
  if (!cg->builder) {
    ny_diag_error(tok, "internal codegen failure: missing builder in %s", ctx);
    cg->had_error = 1;
    return false;
  }
  if (!value) {
    ny_diag_error(
        tok, "internal codegen failure: missing assignment value in %s", ctx);
    cg->had_error = 1;
    return false;
  }
  if (!slot) {
    ny_diag_error(
        tok, "internal codegen failure: missing assignment destination in %s",
        ctx);
    cg->had_error = 1;
    return false;
  }
  if (LLVMGetTypeKind(LLVMTypeOf(slot)) != LLVMPointerTypeKind) {
    ny_diag_error(tok,
                  "internal codegen failure: assignment destination is not "
                  "addressable in "
                  "%s",
                  ctx);
    cg->had_error = 1;
    return false;
  }
  LLVMBasicBlockRef cur_block = ny_cur_block(cg);
  if (!cur_block) {
    ny_diag_error(tok, "internal codegen failure: missing insert block");
    cg->had_error = 1;
    return false;
  }
  return !LLVMGetBasicBlockTerminator(cur_block);
}

static bool can_bind_decl_direct(const codegen_t *cg, const char *name,
                                 bool is_mut) {
  /* Only RAII cleanup needs an addressable slot to null out on move/drop;
   * advisory-only ownership tracking (the default) keeps the fast direct
   * binding path so enabling diagnostics never changes generated code. */
  if (cg && cg->ownership_enabled && cg->ownership_runtime_cleanup)
    return false;
  if (!is_mut)
    return true;
  if (!cg || !name || !cg->assigned_names_data || cg->assigned_names_len == 0)
    return false;
  uint64_t h = ny_hash64_cstr(name);
  return !ny_name_set_has_hash(
      (const char *const *)cg->assigned_names_data, cg->assigned_names_len,
      cg->assigned_name_hashes_data, cg->assigned_name_hashes_len,
      cg->assigned_names_bloom, name, h);
}

static bool stmt_expr_is_mutating_name(const char *name) {
  static const char *const k_names[] = {"add",    "append", "extend", "sub",
                                        "remove", "sort",   "clear",  "push",
                                        "insert", "set",    "put",    "delete",
                                        "del",    NULL};
  if (!name)
    return false;
  for (size_t i = 0; k_names[i]; ++i) {
    if (ny_name_tail_is(name, k_names[i]))
      return true;
  }
  return false;
}

static bool stmt_expr_is_unassigned_append_call(expr_t *e) {
  if (!e)
    return false;
  if (e->kind == NY_E_MEMCALL)
    return ny_name_tail_is(e->as.memcall.name, "append");
  if (e->kind != NY_E_CALL || !e->as.call.callee)
    return false;
  expr_t *callee = e->as.call.callee;
  if (callee->kind == NY_E_IDENT)
    return ny_name_tail_is(callee->as.ident.name, "append");
  if (callee->kind == NY_E_MEMBER)
    return ny_name_tail_is(callee->as.member.name, "append");
  return false;
}

static bool stmt_expr_is_cond_small_int(codegen_t *cg, scope *scopes,
                                        size_t depth, expr_t *e) {
  const int64_t ny_small_int_min = INT64_C(-4611686018427387904);
  const int64_t ny_small_int_max = INT64_C(4611686018427387903);
  if (!cg || !e)
    return false;
  switch (e->kind) {
  case NY_E_LITERAL:
    return e->as.literal.kind == NY_LIT_INT &&
           e->as.literal.as.i >= ny_small_int_min &&
           e->as.literal.as.i <= ny_small_int_max;
  case NY_E_IDENT: {
    size_t name_len = (size_t)e->tok.len;
    if (name_len == 0)
      name_len = strlen(e->as.ident.name);
    binding *b = stmt_lookup_binding(cg, scopes, depth, e->as.ident.name,
                                     name_len, e->as.ident.hash);
    if (b) {
      if (b->is_int_slot || b->is_int_direct)
        return true;
      expr_t *init =
          !b->is_mut ? ny_binding_var_init_expr(b, e->as.ident.name) : NULL;
      if (init)
        return stmt_expr_is_cond_small_int(cg, scopes, depth, init);
    }
    const char *t = infer_expr_type(cg, scopes, depth, e);
    return stmt_type_name_is_index_int_value(t);
  }
  default:
    return false;
  }
}

static LLVMIntPredicate stmt_cmp_pred_for_op(const char *op) {
  if (strcmp(op, "<") == 0)
    return LLVMIntSLT;
  if (strcmp(op, "<=") == 0)
    return LLVMIntSLE;
  if (strcmp(op, ">") == 0)
    return LLVMIntSGT;
  if (strcmp(op, ">=") == 0)
    return LLVMIntSGE;
  if (strcmp(op, "==") == 0)
    return LLVMIntEQ;
  return LLVMIntNE;
}

static LLVMRealPredicate stmt_fcmp_pred_for_op(const char *op) {
  if (strcmp(op, "<") == 0)
    return LLVMRealOLT;
  if (strcmp(op, "<=") == 0)
    return LLVMRealOLE;
  if (strcmp(op, ">") == 0)
    return LLVMRealOGT;
  if (strcmp(op, ">=") == 0)
    return LLVMRealOGE;
  if (strcmp(op, "==") == 0)
    return LLVMRealOEQ;
  return LLVMRealUNE;
}

static int stmt_expr_numeric_kind(codegen_t *cg, scope *scopes, size_t depth,
                                  expr_t *e) {
  if (!e)
    return 0;
  if (e->kind == NY_E_LITERAL) {
    if (e->as.literal.kind == NY_LIT_FLOAT)
      return 2;
    if (e->as.literal.kind == NY_LIT_INT)
      return 1;
    return 0;
  }
  const char *t = infer_expr_type(cg, scopes, depth, e);
  if (!t)
    return 0;
  if (stmt_type_name_is_float_value(t))
    return 2;
  if (stmt_type_name_is_int_value(t))
    return 1;
  return 0;
}

static bool stmt_exprs_are_f64_cmp(codegen_t *cg, scope *scopes, size_t depth,
                                   expr_t *le, expr_t *re) {
  int lk = stmt_expr_numeric_kind(cg, scopes, depth, le);
  int rk = stmt_expr_numeric_kind(cg, scopes, depth, re);
  return lk > 0 && rk > 0 && (lk == 2 || rk == 2);
}

enum {
  STMT_BRANCH_OP_LT = 1 << 0,
  STMT_BRANCH_OP_LE = 1 << 1,
  STMT_BRANCH_OP_GT = 1 << 2,
  STMT_BRANCH_OP_GE = 1 << 3,
  STMT_BRANCH_OP_EQ = 1 << 4,
  STMT_BRANCH_OP_NE = 1 << 5,
  STMT_BRANCH_OP_ALL = (1 << 6) - 1,
  STMT_BRANCH_OP_ORDER = STMT_BRANCH_OP_LT | STMT_BRANCH_OP_LE |
                         STMT_BRANCH_OP_GT | STMT_BRANCH_OP_GE,
  STMT_BRANCH_OP_EQUALITY = STMT_BRANCH_OP_EQ | STMT_BRANCH_OP_NE,
};

static int stmt_branch_op_bit(const char *op) {
  if (!op)
    return 0;
  if (strcmp(op, "<") == 0)
    return STMT_BRANCH_OP_LT;
  if (strcmp(op, "<=") == 0)
    return STMT_BRANCH_OP_LE;
  if (strcmp(op, ">") == 0)
    return STMT_BRANCH_OP_GT;
  if (strcmp(op, ">=") == 0)
    return STMT_BRANCH_OP_GE;
  if (strcmp(op, "==") == 0)
    return STMT_BRANCH_OP_EQ;
  if (strcmp(op, "!=") == 0)
    return STMT_BRANCH_OP_NE;
  return 0;
}

static int stmt_parse_branch_fast_ops_mask(void) {
  const char *ops = getenv("NYTRIX_PROVEN_INT_BRANCH_FAST_OPS");
  if (!ops || !*ops)
    return STMT_BRANCH_OP_ALL;

  int mask = 0;
  const char *p = ops;
  while (*p) {
    while (*p == ',' || *p == ' ' || *p == '\t')
      p++;
    const char *start = p;
    while (*p && *p != ',' && *p != ' ' && *p != '\t')
      p++;
    size_t len = (size_t)(p - start);
    if (len == 2 && strncmp(start, "lt", 2) == 0)
      mask |= STMT_BRANCH_OP_LT;
    else if (len == 2 && strncmp(start, "le", 2) == 0)
      mask |= STMT_BRANCH_OP_LE;
    else if (len == 2 && strncmp(start, "gt", 2) == 0)
      mask |= STMT_BRANCH_OP_GT;
    else if (len == 2 && strncmp(start, "ge", 2) == 0)
      mask |= STMT_BRANCH_OP_GE;
    else if (len == 2 && strncmp(start, "eq", 2) == 0)
      mask |= STMT_BRANCH_OP_EQ;
    else if (len == 2 && strncmp(start, "ne", 2) == 0)
      mask |= STMT_BRANCH_OP_NE;
    else if (len == 5 && strncmp(start, "order", 5) == 0)
      mask |= STMT_BRANCH_OP_ORDER;
    else if (len == 8 && strncmp(start, "equality", 8) == 0)
      mask |= STMT_BRANCH_OP_EQUALITY;
    else if (len == 3 && strncmp(start, "all", 3) == 0)
      mask |= STMT_BRANCH_OP_ALL;
  }
  return mask;
}

static bool stmt_proven_int_branch_fast_op_enabled(codegen_t *cg,
                                                   const char *op) {
  int bit = stmt_branch_op_bit(op);
  if (!bit)
    return false;
  int encoded = cg ? cg->env_cache.proven_int_branch_ops_mask : 0;
  if (!encoded) {
    int mask = stmt_parse_branch_fast_ops_mask();
    encoded = mask + 1;
    if (cg)
      cg->env_cache.proven_int_branch_ops_mask = encoded;
  }
  return ((encoded - 1) & bit) != 0;
}

static bool stmt_proven_int_branch_eq_default_enabled(codegen_t *cg) {
  if (cg && cg->env_cache.proven_int_branch_eq_fast != 0)
    return cg->env_cache.proven_int_branch_eq_fast == 1;
  bool enabled = ny_env_enabled_default_on("NYTRIX_PROVEN_INT_BRANCH_EQ_FAST");
  if (cg)
    cg->env_cache.proven_int_branch_eq_fast = enabled ? 1 : -1;
  return enabled;
}

static bool stmt_proven_int_branch_fast_enabled(codegen_t *cg, const char *op) {
  if (ny_fast_path_enabled(cg, "NYTRIX_PROVEN_INT_BRANCH_FAST") &&
      stmt_proven_int_branch_fast_op_enabled(cg, op))
    return true;
  int bit = stmt_branch_op_bit(op);
  return (bit & STMT_BRANCH_OP_EQUALITY) &&
         stmt_proven_int_branch_eq_default_enabled(cg);
}

static fun_sig *stmt_cmp_runtime_sig(codegen_t *cg, const char *op) {
  if (!cg || !op)
    return NULL;
  const char *name = NULL;
  if (strcmp(op, "<") == 0)
    name = "__lt";
  else if (strcmp(op, "<=") == 0)
    name = "__le";
  else if (strcmp(op, ">") == 0)
    name = "__gt";
  else if (strcmp(op, ">=") == 0)
    name = "__ge";
  else if (strcmp(op, "==") == 0)
    name = "__eq";
  if (!name)
    return NULL;
  return lookup_fun(cg, name, 0);
}

static binding *stmt_expr_target_binding(codegen_t *cg, scope *scopes,
                                         size_t depth, expr_t *target,
                                         bool *is_global) {
  if (is_global)
    *is_global = false;
  if (!target || target->kind != NY_E_IDENT)
    return NULL;
  return stmt_var_lookup_existing(cg, scopes, depth, target->as.ident.name,
                                  is_global);
}

static void stmt_store_raw_int_shadow(codegen_t *cg, binding *b,
                                      LLVMValueRef tagged_value);
static bool stmt_build_raw_int_expr_const_ok(codegen_t *cg, scope *scopes,
                                             size_t depth, expr_t *expr,
                                             LLVMValueRef *out_raw);

static bool stmt_expr_store_back(codegen_t *cg, scope *scopes, size_t depth,
                                 expr_t *target, LLVMValueRef value,
                                 token_t tok) {
  bool is_global = false;
  binding *b = stmt_expr_target_binding(cg, scopes, depth, target, &is_global);
  if (!b || !b->is_slot)
    return false;
  if (!ensure_mutable_binding_for_assign(cg, tok, target->as.ident.name, b,
                                         is_global))
    return false;
  stmt_ownership_check_live_borrows(cg, scopes, depth, b, tok, "mutate");
  if (!ensure_store_ready(cg, tok, value, b->value,
                          "mutating expression statement"))
    return false;
  ny_store(cg, b->value, value);
  if (b->is_int_slot)
    stmt_store_raw_int_shadow(cg, b, value);
  return true;
}

static LLVMIntPredicate stmt_cmp_pred_for_op(const char *op);

typedef struct stmt_ident_eq_set_t {
  const char *name;
  size_t name_len;
  uint64_t hash;
  int64_t values[64];
  size_t count;
} stmt_ident_eq_set_t;

static bool stmt_expr_ident_lit_eq(expr_t *e, const char **name,
                                   size_t *name_len, uint64_t *hash,
                                   int64_t *value) {
  if (!e || e->kind != NY_E_BINARY || !e->as.binary.op ||
      strcmp(e->as.binary.op, "==") != 0)
    return false;
  expr_t *a = e->as.binary.left;
  expr_t *b = e->as.binary.right;
  if (a && b && a->kind == NY_E_IDENT && b->kind == NY_E_LITERAL &&
      b->as.literal.kind == NY_LIT_INT && a->as.ident.name) {
    if (name)
      *name = a->as.ident.name;
    if (name_len) {
      *name_len = (size_t)a->tok.len;
      if (*name_len == 0)
        *name_len = strlen(a->as.ident.name);
    }
    if (hash)
      *hash = a->as.ident.hash;
    if (value)
      *value = b->as.literal.as.i;
    return true;
  }
  if (a && b && b->kind == NY_E_IDENT && a->kind == NY_E_LITERAL &&
      a->as.literal.kind == NY_LIT_INT && b->as.ident.name) {
    if (name)
      *name = b->as.ident.name;
    if (name_len) {
      *name_len = (size_t)b->tok.len;
      if (*name_len == 0)
        *name_len = strlen(b->as.ident.name);
    }
    if (hash)
      *hash = b->as.ident.hash;
    if (value)
      *value = a->as.literal.as.i;
    return true;
  }
  return false;
}

static bool stmt_ident_eq_set_push(stmt_ident_eq_set_t *set, const char *name,
                                   size_t name_len, uint64_t hash,
                                   int64_t value) {
  if (!set || !name || !*name || set->count >= 64)
    return false;
  if (!set->name) {
    set->name = name;
    set->name_len = name_len;
    set->hash = hash;
  } else if (set->hash != hash || set->name_len != name_len ||
             strncmp(set->name, name, name_len) != 0) {
    return false;
  }
  for (size_t i = 0; i < set->count; ++i) {
    if (set->values[i] == value)
      return true;
  }
  set->values[set->count++] = value;
  return true;
}

static bool stmt_collect_ident_eq_or(expr_t *e, stmt_ident_eq_set_t *set) {
  if (!e || !set)
    return false;
  if (e->kind == NY_E_LOGICAL && e->as.logical.op &&
      strcmp(e->as.logical.op, "||") == 0) {
    return stmt_collect_ident_eq_or(e->as.logical.left, set) &&
           stmt_collect_ident_eq_or(e->as.logical.right, set);
  }
  const char *name = NULL;
  size_t name_len = 0;
  uint64_t hash = 0;
  int64_t value = 0;
  if (!stmt_expr_ident_lit_eq(e, &name, &name_len, &hash, &value))
    return false;
  return stmt_ident_eq_set_push(set, name, name_len, hash, value);
}

static LLVMValueRef stmt_try_ident_eq_set_cond_i1(codegen_t *cg, scope *scopes,
                                                  size_t depth, expr_t *e) {
  stmt_ident_eq_set_t set = {0};
  if (!stmt_collect_ident_eq_or(e, &set) || !set.name || set.count < 3)
    return NULL;
  int64_t min_v = set.values[0];
  int64_t max_v = set.values[0];
  for (size_t i = 1; i < set.count; ++i) {
    if (set.values[i] < min_v)
      min_v = set.values[i];
    if (set.values[i] > max_v)
      max_v = set.values[i];
  }
  if (max_v < min_v || max_v - min_v > 63)
    return NULL;

  expr_t ident = {0};
  ident.kind = NY_E_IDENT;
  ident.tok = e->tok;
  ident.tok.len = (int)set.name_len;
  ident.as.ident.name = set.name;
  ident.as.ident.hash = set.hash;

  LLVMValueRef raw = NULL;
  if (!stmt_build_raw_int_expr_const_ok(cg, scopes, depth, &ident, &raw))
    return NULL;

  uint64_t mask = 0;
  for (size_t i = 0; i < set.count; ++i)
    mask |= UINT64_C(1) << (unsigned)(set.values[i] - min_v);

  LLVMValueRef idx = LLVMBuildSub(
      cg->builder, raw, LLVMConstInt(cg->type_i64, (uint64_t)min_v, true),
      "eqset_idx");
  LLVMValueRef in_range = LLVMBuildICmp(
      cg->builder, LLVMIntULE, idx,
      LLVMConstInt(cg->type_i64, (uint64_t)(max_v - min_v), false),
      "eqset_range");
  LLVMValueRef safe_idx =
      LLVMBuildSelect(cg->builder, in_range, idx,
                      LLVMConstInt(cg->type_i64, 0, false), "eqset_safe_idx");
  LLVMValueRef bits =
      LLVMBuildLShr(cg->builder, LLVMConstInt(cg->type_i64, mask, false),
                    safe_idx, "eqset_bits");
  LLVMValueRef bit = LLVMBuildAnd(
      cg->builder, bits, LLVMConstInt(cg->type_i64, 1, false), "eqset_bit");
  LLVMValueRef hit =
      LLVMBuildICmp(cg->builder, LLVMIntNE, bit,
                    LLVMConstInt(cg->type_i64, 0, false), "eqset_hit");
  return LLVMBuildAnd(cg->builder, in_range, hit, "eqset_cond");
}

static LLVMValueRef stmt_gen_cond_i1(codegen_t *cg, scope *scopes, size_t depth,
                                     expr_t *e) {
  if (!e)
    return LLVMConstInt(ny_i1_ty(cg), 0, false);

  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_BOOL)
    return LLVMConstInt(ny_i1_ty(cg), e->as.literal.as.b ? 1 : 0, false);

  LLVMValueRef eqset_cond = stmt_try_ident_eq_set_cond_i1(cg, scopes, depth, e);
  if (eqset_cond)
    return eqset_cond;

  if (e->kind == NY_E_UNARY && e->as.unary.op &&
      strcmp(e->as.unary.op, "!") == 0) {
    LLVMValueRef inner = stmt_gen_cond_i1(cg, scopes, depth, e->as.unary.right);
    return LLVMBuildNot(cg->builder, inner, "cond_not");
  }

  if (e->kind == NY_E_LOGICAL && e->as.logical.op) {
    bool and_op = strcmp(e->as.logical.op, "&&") == 0;
    bool or_op = strcmp(e->as.logical.op, "||") == 0;
    if (and_op || or_op) {
      ny_null_narrow_list_t rhs_narrow;
      vec_init(&rhs_narrow);
      bool narrow_rhs = ny_null_narrow_collect_logical_rhs(e->as.logical.left,
                                                           and_op, &rhs_narrow);

      LLVMValueRef left =
          stmt_gen_cond_i1(cg, scopes, depth, e->as.logical.left);
      LLVMBasicBlockRef left_bb = ny_cur_block(cg);
      LLVMValueRef f = LLVMGetBasicBlockParent(left_bb);
      LLVMBasicBlockRef rhs_bb =
          ny_bb_fn(f, and_op ? "cond_and_rhs" : "cond_or_rhs");
      LLVMBasicBlockRef end_bb =
          ny_bb_fn(f, and_op ? "cond_and_end" : "cond_or_end");
      if (and_op)
        ny_cond_br(cg, left, rhs_bb, end_bb);
      else
        ny_cond_br(cg, left, end_bb, rhs_bb);

      ny_pos(cg, rhs_bb);
      ny_null_narrow_restore_list_t rhs_applied;
      if (narrow_rhs)
        ny_null_narrow_apply(cg, scopes, depth, &rhs_narrow, true,
                             &rhs_applied);
      LLVMValueRef right =
          stmt_gen_cond_i1(cg, scopes, depth, e->as.logical.right);
      if (narrow_rhs)
        ny_null_narrow_restore(&rhs_applied);
      vec_free(&rhs_narrow);
      LLVMBasicBlockRef rhs_end_bb = ny_cur_block(cg);
      if (!LLVMGetBasicBlockTerminator(rhs_end_bb))
        ny_br(cg, end_bb);

      ny_pos(cg, end_bb);
      LLVMValueRef phi =
          ny_phi(cg, ny_i1_ty(cg), and_op ? "cond_and" : "cond_or");
      LLVMValueRef short_value =
          LLVMConstInt(ny_i1_ty(cg), and_op ? 0 : 1, false);
      LLVMAddIncoming(phi, (LLVMValueRef[]){short_value, right},
                      (LLVMBasicBlockRef[]){left_bb, rhs_end_bb}, 2);
      return phi;
    }
  }

  if (e->kind == NY_E_BINARY && e->as.binary.op) {
    const char *op = e->as.binary.op;
    expr_t *le = e->as.binary.left;
    expr_t *re = e->as.binary.right;
    bool is_cmp = (strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
                   strcmp(op, ">") == 0 || strcmp(op, ">=") == 0 ||
                   strcmp(op, "==") == 0 || strcmp(op, "!=") == 0);
    if (is_cmp) {
      if (stmt_exprs_are_f64_cmp(cg, scopes, depth, le, re)) {
        LLVMValueRef lf = gen_expr_as_f64(cg, scopes, depth, le);
        LLVMValueRef rf = gen_expr_as_f64(cg, scopes, depth, re);
        return LLVMBuildFCmp(cg->builder, stmt_fcmp_pred_for_op(op), lf, rf,
                             "cond_fcmp_fast");
      }
      LLVMValueRef l = gen_expr(cg, scopes, depth, le);
      LLVMValueRef r = gen_expr(cg, scopes, depth, re);
      if (stmt_expr_is_cond_small_int(cg, scopes, depth, le) &&
          stmt_expr_is_cond_small_int(cg, scopes, depth, re)) {
        LLVMIntPredicate pred = LLVMIntEQ;
        if (strcmp(op, "<") == 0)
          pred = LLVMIntSLT;
        else if (strcmp(op, "<=") == 0)
          pred = LLVMIntSLE;
        else if (strcmp(op, ">") == 0)
          pred = LLVMIntSGT;
        else if (strcmp(op, ">=") == 0)
          pred = LLVMIntSGE;
        else if (strcmp(op, "!=") == 0)
          pred = LLVMIntNE;
        return LLVMBuildICmp(cg->builder, pred, l, r, "cond_icmp_fast");
      }
      if (stmt_proven_int_branch_fast_enabled(cg, op) &&
          ny_is_proven_int(cg, scopes, depth, le, l) &&
          ny_is_proven_int(cg, scopes, depth, re, r)) {
        return LLVMBuildICmp(cg->builder, stmt_cmp_pred_for_op(op), l, r,
                             "proven_int_branch_fast");
      }
      return to_bool(cg, gen_binary(cg, scopes, depth, op, l, r, le, re));
    }
  }

  return to_bool(cg, gen_expr(cg, scopes, depth, e));
}

static bool stmt_try_emit_direct_cmp_cond_branch(codegen_t *cg, scope *scopes,
                                                 size_t depth, expr_t *e,
                                                 LLVMBasicBlockRef true_bb,
                                                 LLVMBasicBlockRef false_bb) {
  if (!cg || !e || e->kind != NY_E_BINARY || !e->as.binary.op)
    return false;
  const char *op = e->as.binary.op;
  expr_t *le = e->as.binary.left;
  expr_t *re = e->as.binary.right;
  bool is_cmp =
      (strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 || strcmp(op, ">") == 0 ||
       strcmp(op, ">=") == 0 || strcmp(op, "==") == 0 || strcmp(op, "!=") == 0);
  if (!is_cmp)
    return false;

  if (stmt_exprs_are_f64_cmp(cg, scopes, depth, le, re)) {
    LLVMValueRef lf = gen_expr_as_f64(cg, scopes, depth, le);
    LLVMValueRef rf = gen_expr_as_f64(cg, scopes, depth, re);
    LLVMValueRef fast_cond = LLVMBuildFCmp(
        cg->builder, stmt_fcmp_pred_for_op(op), lf, rf, "cond_fcmp_fast");
    ny_cond_br(cg, fast_cond, true_bb, false_bb);
    return true;
  }

  LLVMValueRef l = gen_expr(cg, scopes, depth, le);
  LLVMValueRef r = gen_expr(cg, scopes, depth, re);
  if (stmt_expr_is_cond_small_int(cg, scopes, depth, le) &&
      stmt_expr_is_cond_small_int(cg, scopes, depth, re)) {
    LLVMValueRef fast_cond = LLVMBuildICmp(
        cg->builder, stmt_cmp_pred_for_op(op), l, r, "cond_icmp_fast");
    ny_cond_br(cg, fast_cond, true_bb, false_bb);
    return true;
  }

  bool prov_l = ny_is_proven_int(cg, scopes, depth, le, l);
  bool prov_r = ny_is_proven_int(cg, scopes, depth, re, r);
  if (stmt_proven_int_branch_fast_enabled(cg, op) && prov_l && prov_r) {
    LLVMValueRef fast_cond = LLVMBuildICmp(
        cg->builder, stmt_cmp_pred_for_op(op), l, r, "proven_int_branch_fast");
    ny_cond_br(cg, fast_cond, true_bb, false_bb);
    return true;
  }

  fun_sig *cmp_sig = stmt_cmp_runtime_sig(cg, op);
  if (!cmp_sig || (!prov_l && !prov_r))
    return false;

  LLVMBasicBlockRef entry_bb = ny_cur_block(cg);
  LLVMValueRef fn = LLVMGetBasicBlockParent(entry_bb);
  LLVMBasicBlockRef fast_bb = ny_bb_fn(fn, "cond.cmp.fast");
  LLVMBasicBlockRef slow_bb = ny_bb_fn(fn, "cond.cmp.slow");
  LLVMBasicBlockRef merge_bb = ny_bb_fn(fn, "cond.cmp.merge");
  LLVMValueRef both_int = prov_l && prov_r
                              ? LLVMConstInt(ny_i1_ty(cg), 1, false)
                          : prov_l ? ny_is_tagged_int(cg, r)
                                   : ny_is_tagged_int(cg, l);
  if (LLVMIsAConstantInt(both_int) && LLVMConstIntGetZExtValue(both_int))
    ny_br(cg, fast_bb);
  else
    ny_cond_br(cg, both_int, fast_bb, slow_bb);

  ny_pos(cg, fast_bb);
  LLVMValueRef fast_cond = LLVMBuildICmp(cg->builder, stmt_cmp_pred_for_op(op),
                                         l, r, "cond_cmp_fast");
  LLVMBasicBlockRef fast_done_bb = ny_cur_block(cg);
  ny_br(cg, merge_bb);

  ny_pos(cg, slow_bb);
  LLVMValueRef slow_tagged =
      LLVMBuildCall2(cg->builder, cmp_sig->type, cmp_sig->value,
                     (LLVMValueRef[]){l, r}, 2, "");
  LLVMValueRef slow_cond = to_bool(cg, slow_tagged);
  LLVMBasicBlockRef slow_done_bb = ny_cur_block(cg);
  ny_br(cg, merge_bb);

  ny_pos(cg, merge_bb);
  LLVMValueRef phi = LLVMBuildPhi(cg->builder, ny_i1_ty(cg), "cond_cmp_phi");
  LLVMValueRef incoming_vals[2] = {fast_cond, slow_cond};
  LLVMBasicBlockRef incoming_bbs[2] = {fast_done_bb, slow_done_bb};
  LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 2);
  ny_cond_br(cg, phi, true_bb, false_bb);
  return true;
}

static void gen_loop_control(codegen_t *cg, scope *scopes, size_t depth,
                             token_t tok, bool is_continue) {
  LLVMBasicBlockRef jump_bb = NULL;
  for (ssize_t i = (ssize_t)depth; i >= 0; i--) {
    emit_defers(cg, scopes, (size_t)i, (size_t)i);
    if (is_continue ? scopes[i].continue_bb : scopes[i].break_bb) {
      jump_bb = is_continue ? scopes[i].continue_bb : scopes[i].break_bb;
      break;
    }
  }
  if (jump_bb) {
    ny_dbg_loc(cg, tok);
    ny_br(cg, jump_bb);
    return;
  }
  ny_diag_error(tok, "%s'%s'%s used outside of a loop", clr(NY_CLR_BOLD),
                is_continue ? "continue" : "break", clr(NY_CLR_RESET));
  ny_diag_hint("'%s' is only valid inside %swhile%s or %sfor%s bodies",
               is_continue ? "continue" : "break", clr(NY_CLR_BOLD),
               clr(NY_CLR_RESET), clr(NY_CLR_BOLD), clr(NY_CLR_RESET));
  cg->had_error = 1;
}

void ny_emit_trace_loc_force(codegen_t *cg, token_t tok) {
  if (!cg || !cg->builder)
    return;
  fun_sig *ts = lookup_fun(cg, "__trace_loc", 0);
  if (!ts)
    return;
  const char *fname = tok.filename ? tok.filename : "<unknown>";
  LLVMValueRef fstr_g = const_string_ptr(cg, fname, strlen(fname));
  LLVMValueRef fstr = ny_load(cg, fstr_g, "");
  int line = tok.line > 0 ? tok.line : 1;
  int col = tok.col > 0 ? tok.col : 1;
  LLVMValueRef line_v =
      LLVMConstInt(cg->type_i64, ((uint64_t)line << 1) | 1, false);
  LLVMValueRef col_v =
      LLVMConstInt(cg->type_i64, ((uint64_t)col << 1) | 1, false);
  LLVMBuildCall2(cg->builder, ts->type, ts->value,
                 (LLVMValueRef[]){fstr, line_v, col_v}, 3, "");
}

static void emit_trace_loc(codegen_t *cg, token_t tok) {
  if (!cg || !cg->trace_exec)
    return;
  ny_emit_trace_loc_force(cg, tok);
}

static bool stmt_is_direct_thread_attr_call(codegen_t *cg, expr_t *e) {
  if (!cg || !e || e->kind != NY_E_CALL || !e->as.call.callee ||
      e->as.call.callee->kind != NY_E_IDENT)
    return false;
  const char *name = e->as.call.callee->as.ident.name;
  if (!name || !*name)
    return false;
  fun_sig *sig = lookup_fun(cg, name, 0);
  if (!sig || sig->is_extern || !sig->stmt_t || sig->stmt_t->kind != NY_S_FUNC)
    return false;
  return sig->stmt_t->as.fn.attr_thread;
}

static label_binding *find_label_binding(codegen_t *cg, const char *name) {
  if (!cg || !name)
    return NULL;
  for (size_t i = 0; i < cg->labels.len; i++) {
    if (strcmp(cg->labels.data[i].name, name) == 0)
      return &cg->labels.data[i];
  }
  return NULL;
}

static bool match_pattern_is_okerr(expr_t *pat, bool *is_ok, bool *is_err) {
  if (!pat || pat->kind != NY_E_CALL || !pat->as.call.callee ||
      pat->as.call.callee->kind != NY_E_IDENT)
    return false;
  const char *callee = pat->as.call.callee->as.ident.name;
  if (!callee)
    return false;
  if (strcmp(callee, "ok") == 0) {
    if (is_ok)
      *is_ok = true;
    return true;
  }
  if (strcmp(callee, "err") == 0) {
    if (is_err)
      *is_err = true;
    return true;
  }
  return false;
}

static enum_member_def_t *match_pattern_enum_member(codegen_t *cg, expr_t *pat,
                                                    enum_def_t **out_enum) {
  if (!pat)
    return NULL;
  if (pat->kind == NY_E_IDENT) {
    return lookup_enum_member_owner(cg, pat->as.ident.name, out_enum);
  }
  if (pat->kind == NY_E_MEMBER) {
    char *full_name = codegen_full_name(cg, pat, cg->arena);
    if (!full_name)
      return NULL;
    return lookup_enum_member_owner(cg, full_name, out_enum);
  }
  if (pat->kind == NY_E_CALL || pat->kind == NY_E_MEMCALL) {
    char *full_name = ny_adt_member_call_full_name(cg, pat);
    if (!full_name)
      return NULL;
    return lookup_enum_member_owner(cg, full_name, out_enum);
  }
  return NULL;
}

static const char *stmt_type_trim_copy(codegen_t *cg, const char *s, size_t n) {
  if (!s)
    return NULL;
  while (n > 0 && (*s == ' ' || *s == '\t')) {
    s++;
    n--;
  }
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'))
    n--;
  return arena_strndup(cg ? cg->arena : NULL, s, n);
}

static bool stmt_type_generic_base_is(const char *type_name, const char *base) {
  if (!type_name || !base)
    return false;
  while (*type_name == '?' || *type_name == '*')
    type_name++;
  const char *leaf = ny_name_leaf(type_name);
  if (!leaf)
    leaf = type_name;
  const char *lt = strchr(leaf, '<');
  size_t leaf_len = lt ? (size_t)(lt - leaf) : strlen(leaf);
  const char *base_leaf = ny_name_leaf(base);
  if (!base_leaf)
    base_leaf = base;
  return strlen(base_leaf) == leaf_len &&
         strncmp(leaf, base_leaf, leaf_len) == 0;
}

static const char *stmt_type_generic_arg(codegen_t *cg, const char *type_name,
                                         size_t index) {
  if (!type_name)
    return NULL;
  while (*type_name == '?' || *type_name == '*')
    type_name++;
  const char *leaf = ny_name_leaf(type_name);
  if (!leaf)
    leaf = type_name;
  const char *lt = strchr(leaf, '<');
  const char *gt = strrchr(leaf, '>');
  if (!lt || !gt || gt <= lt)
    return NULL;
  const char *start = lt + 1;
  size_t current = 0;
  int depth = 0;
  for (const char *p = start; *p; ++p) {
    if (*p == '<') {
      depth++;
      continue;
    }
    if (*p == '>') {
      if (depth == 0) {
        if (current == index)
          return stmt_type_trim_copy(cg, start, (size_t)(p - start));
        return NULL;
      }
      depth--;
      continue;
    }
    if (*p == ',' && depth == 0) {
      if (current == index)
        return stmt_type_trim_copy(cg, start, (size_t)(p - start));
      current++;
      start = p + 1;
    }
  }
  return NULL;
}

static const char *stmt_match_subject_known_type(codegen_t *cg, scope *scopes,
                                                 size_t depth, expr_t *test) {
  if (!test)
    return NULL;
  if (test->kind == NY_E_CALL && test->as.call.callee &&
      test->as.call.callee->kind == NY_E_IDENT) {
    const char *name = test->as.call.callee->as.ident.name;
    fun_sig *sig = lookup_fun(cg, name, test->as.call.callee->as.ident.hash);
    if (sig) {
      if (stmt_type_generic_base_is(sig->return_type, "Result"))
        return sig->return_type;
      if (stmt_type_generic_base_is(sig->inferred_return_type, "Result"))
        return sig->inferred_return_type;
    }
    size_t name_len = name ? strlen(name) : 0;
    size_t argc = test->as.call.args.len;
    for (size_t i = 0; cg && i < cg->fun_sigs.len; ++i) {
      fun_sig *cand = &cg->fun_sigs.data[i];
      if (!cand->name || strlen(cand->name) != name_len ||
          memcmp(cand->name, name, name_len) != 0)
        continue;
      if (!cand->is_variadic && cand->arity != (int)argc)
        continue;
      if (stmt_type_generic_base_is(cand->return_type, "Result"))
        return cand->return_type;
      if (stmt_type_generic_base_is(cand->inferred_return_type, "Result"))
        return cand->inferred_return_type;
    }
    if (sig) {
      if (sig->return_type)
        return sig->return_type;
      if (sig->inferred_return_type)
        return sig->inferred_return_type;
    }
  }
  if (test->kind == NY_E_IDENT && test->as.ident.name) {
    size_t name_len = (size_t)test->tok.len;
    if (name_len == 0)
      name_len = strlen(test->as.ident.name);
    binding *b = stmt_lookup_binding(cg, scopes, depth, test->as.ident.name,
                                     name_len, test->as.ident.hash);
    if (b) {
      if (b->type_name)
        return b->type_name;
      if (!b->is_mut) {
        expr_t *init = ny_binding_var_init_expr(b, test->as.ident.name);
        const char *init_type = infer_expr_type(cg, scopes, depth, init);
        if (init_type)
          return init_type;
      }
    }
  }
  return NULL;
}

static const char *stmt_match_result_payload_type(codegen_t *cg, scope *scopes,
                                                  size_t depth,
                                                  stmt_t *owner_stmt,
                                                  bool want_ok) {
  if (!owner_stmt || owner_stmt->kind != NY_S_MATCH)
    return NULL;
  const char *subject_type =
      infer_expr_type(cg, scopes, depth, owner_stmt->as.match.test);
  if (!stmt_type_generic_base_is(subject_type, "Result")) {
    const char *known = stmt_match_subject_known_type(
        cg, scopes, depth, owner_stmt->as.match.test);
    if (stmt_type_generic_base_is(known, "Result"))
      subject_type = known;
    else
      return NULL;
  }
  return stmt_type_generic_arg(cg, subject_type, want_ok ? 0u : 1u);
}

static const char *
stmt_match_substitute_adt_field_type(codegen_t *cg, scope *scopes, size_t depth,
                                     stmt_t *owner_stmt, enum_def_t *owner,
                                     const char *field_type) {
  if (!cg || !owner_stmt || owner_stmt->kind != NY_S_MATCH || !owner ||
      !field_type)
    return field_type;
  const char *subject_type =
      infer_expr_type(cg, scopes, depth, owner_stmt->as.match.test);
  if (!stmt_type_generic_base_is(subject_type, owner->name))
    return field_type;
  for (size_t i = 0; i < owner->type_params.len; ++i) {
    const char *param = owner->type_params.data[i];
    if (!param || !*param)
      continue;
    const char *actual = stmt_type_generic_arg(cg, subject_type, i);
    if (!actual || !*actual)
      continue;
    if (strcmp(field_type, param) == 0)
      return actual;
    if ((field_type[0] == '*' || field_type[0] == '?') &&
        strcmp(field_type + 1, param) == 0) {
      size_t actual_len = strlen(actual);
      char *out = arena_alloc(cg->arena, actual_len + 2);
      out[0] = field_type[0];
      memcpy(out + 1, actual, actual_len + 1);
      return out;
    }
  }
  return field_type;
}

static LLVMValueRef match_pattern_adt_cond(codegen_t *cg, scope *scopes,
                                           size_t depth, stmt_t *owner_stmt,
                                           LLVMValueRef testv, expr_t *pat) {
  (void)scopes;
  (void)depth;
  (void)owner_stmt;
  if (!pat || (pat->kind != NY_E_CALL && pat->kind != NY_E_MEMCALL))
    return NULL;
  enum_def_t *owner = NULL;
  enum_member_def_t *mem = match_pattern_enum_member(cg, pat, &owner);
  if (!mem || !owner || !mem->has_payload)
    return NULL;

  fun_sig *tag_sig = lookup_fun(cg, "__tagof", 0);
  if (!tag_sig)
    return NULL;
  LLVMValueRef tag = LLVMBuildCall2(cg->builder, tag_sig->type, tag_sig->value,
                                    (LLVMValueRef[]){testv}, 1, "");
  return ny_eq(cg, tag, ny_ci(cg, ((uint64_t)mem->runtime_tag << 1) | 1u),
               NY_LLVM_NAME(cg, "adt_tag_eq"));
}

static void match_bind_adt_pattern(codegen_t *cg, scope *scopes, size_t depth,
                                   stmt_t *owner_stmt, LLVMValueRef testv,
                                   expr_t *pat) {
  if (!pat || (pat->kind != NY_E_CALL && pat->kind != NY_E_MEMCALL))
    return;
  enum_def_t *owner = NULL;
  enum_member_def_t *mem = match_pattern_enum_member(cg, pat, &owner);
  if (!mem || !owner || !mem->has_payload)
    return;

  call_arg_t *args = NULL;
  size_t argc = 0;
  ny_expr_call_args(pat, &args, &argc);
  for (size_t i = 0; i < argc; i++) {
    call_arg_t *arg = &args[i];
    ssize_t idx = -1;
    const char *field_name = NULL;
    if (arg->name) {
      idx = ny_enum_member_field_index(mem, arg->name);
      field_name = arg->name;
      if (idx < 0) {
        ny_diag_error(pat->tok, "unknown field '%s' in ADT match pattern '%s.%s'",
                      arg->name, owner->name, mem->name);
        cg->had_error = 1;
        continue;
      }
    } else {
      if (i >= mem->fields.len) {
        ny_diag_error(pat->tok, "too many positional fields in ADT match pattern '%s.%s'",
                      owner->name, mem->name);
        cg->had_error = 1;
        continue;
      }
      idx = (ssize_t)i;
      field_name = mem->fields.data[idx].name;
    }
    expr_t *bind = arg->val;
    if (!bind || bind->kind != NY_E_IDENT) {
      ny_diag_error(bind ? bind->tok : pat->tok,
                    "ADT match field '%s' must bind to an identifier or '_'",
                    field_name);
      cg->had_error = 1;
      continue;
    }
    if (strcmp(bind->as.ident.name, "_") == 0)
      continue;
    LLVMValueRef slot_addr = LLVMBuildIntToPtr(
        cg->builder, ny_add(cg, testv, ny_ci(cg, (uint64_t)idx * 8), ""),
        ny_ptr_i64_ty(cg), "");
    LLVMValueRef val = ny_load(cg, slot_addr, NY_LLVM_NAME(cg, "adt_field"));
    const char *field_type = stmt_match_substitute_adt_field_type(
        cg, scopes, depth, owner_stmt, owner, mem->fields.data[idx].type_name);
    scope_bind(cg, scopes, depth, bind->as.ident.name, val, owner_stmt, false,
               field_type, false);
  }
}

static LLVMValueRef match_pattern_result_cond(codegen_t *cg, scope *scopes,
                                              size_t depth, stmt_t *owner_stmt,
                                              LLVMValueRef testv, expr_t *pat) {
  if (pat && pat->kind == NY_E_BINARY && pat->as.binary.op &&
      strcmp(pat->as.binary.op, "..") == 0) {
    expr_t *lo_expr = pat->as.binary.left;
    expr_t *hi_expr = pat->as.binary.right;
    if (!lo_expr || !hi_expr)
      return NULL;
    LLVMValueRef lo = gen_expr(cg, scopes, depth, lo_expr);
    LLVMValueRef hi = gen_expr(cg, scopes, depth, hi_expr);
    LLVMValueRef ge =
        gen_binary(cg, scopes, depth, ">=", testv, lo,
                   owner_stmt ? owner_stmt->as.match.test : NULL, lo_expr);
    LLVMValueRef le =
        gen_binary(cg, scopes, depth, "<=", testv, hi,
                   owner_stmt ? owner_stmt->as.match.test : NULL, hi_expr);
    return ny_and(cg, to_bool(cg, ge), to_bool(cg, le),
                  NY_LLVM_NAME(cg, "match_range"));
  }

  LLVMValueRef adt_cond =
      match_pattern_adt_cond(cg, scopes, depth, owner_stmt, testv, pat);
  if (adt_cond)
    return adt_cond;

  if (!pat || pat->kind != NY_E_CALL || !pat->as.call.callee ||
      pat->as.call.callee->kind != NY_E_IDENT)
    return NULL;
  const char *callee = pat->as.call.callee->as.ident.name;
  if (!callee)
    return NULL;
  bool is_ok_pat = (strcmp(callee, "ok") == 0);
  bool is_err_pat = (strcmp(callee, "err") == 0);
  if (!is_ok_pat && !is_err_pat)
    return NULL;
  fun_sig *check_sig = lookup_fun(cg, is_ok_pat ? "__is_ok" : "__is_err", 0);
  if (!check_sig)
    return NULL;
  LLVMValueRef tag_check =
      LLVMBuildCall2(cg->builder, check_sig->type, check_sig->value,
                     (LLVMValueRef[]){testv}, 1, "");
  LLVMValueRef pat_cond = to_bool(cg, tag_check);
  if (pat->as.call.args.len > 0) {
    expr_t *arg0 = pat->as.call.args.data[0].val;
    if (arg0 && arg0->kind == NY_E_IDENT &&
        strcmp(arg0->as.ident.name ? arg0->as.ident.name : "", "_") != 0) {
      fun_sig *unwrap_sig = lookup_fun(cg, "__unwrap", 0);
      if (unwrap_sig) {
        LLVMValueRef val =
            LLVMBuildCall2(cg->builder, unwrap_sig->type, unwrap_sig->value,
                           (LLVMValueRef[]){testv}, 1, "");
        const char *payload_type = stmt_match_result_payload_type(
            cg, scopes, depth, owner_stmt, is_ok_pat);
        scope_bind(cg, scopes, depth, arg0->as.ident.name, val, owner_stmt,
                   false, payload_type, false);
      }
    }
  }
  return pat_cond;
}

static bool stmt_expr_is_constish(expr_t *e) {
  if (!e)
    return false;
  switch (e->kind) {
  case NY_E_LITERAL:
  case NY_E_COMPTIME:
    return true;
  case NY_E_UNARY:
    return stmt_expr_is_constish(e->as.unary.right);
  case NY_E_BINARY:
    if (e->as.binary.op && strcmp(e->as.binary.op, "..") == 0)
      return false;
    return stmt_expr_is_constish(e->as.binary.left) &&
           stmt_expr_is_constish(e->as.binary.right);
  case NY_E_LOGICAL:
    return stmt_expr_is_constish(e->as.logical.left) &&
           stmt_expr_is_constish(e->as.logical.right);
  case NY_E_TERNARY:
    return stmt_expr_is_constish(e->as.ternary.cond) &&
           stmt_expr_is_constish(e->as.ternary.true_expr) &&
           stmt_expr_is_constish(e->as.ternary.false_expr);
  default:
    return false;
  }
}

static bool stmt_const_tagged_i64(LLVMValueRef v, int64_t *out) {
  if (!v || !LLVMIsAConstantInt(v))
    return false;
  if (out)
    *out = LLVMConstIntGetSExtValue(v);
  return true;
}

static bool stmt_tagged_small_int_raw(int64_t tagged, int64_t *out) {
  if ((((uint64_t)tagged) & 1u) == 0)
    return false;
  if (out)
    *out = tagged >> 1;
  return true;
}

static bool stmt_const_pattern_tagged(codegen_t *cg, scope *scopes,
                                      size_t depth, expr_t *pat, int64_t *out) {
  if (!pat)
    return false;
  if (pat->kind == NY_E_LITERAL) {
    if (pat->as.literal.kind == NY_LIT_BOOL) {
      if (out)
        *out = pat->as.literal.as.b ? NY_IMM_TRUE : NY_IMM_FALSE;
      return true;
    }
    if (pat->as.literal.kind == NY_LIT_INT) {
      if (pat->tok.kind == NY_T_NIL) {
        if (out)
          *out = NY_IMM_NIL;
        return true;
      }
      if (!ny_small_int_fits_i64(pat->as.literal.as.i))
        return false;
      if (out)
        *out = (int64_t)((((uint64_t)pat->as.literal.as.i) << 1) | 1u);
      return true;
    }
    return false;
  }
  if (!stmt_expr_is_constish(pat))
    return false;
  LLVMValueRef v = gen_expr(cg, scopes, depth, pat);
  return stmt_const_tagged_i64(v, out);
}

static int stmt_const_match_pattern(codegen_t *cg, scope *scopes, size_t depth,
                                    int64_t test_tagged, expr_t *pat) {
  if (ny_expr_is_wildcard_ident(pat))
    return 1;
  if (pat && pat->kind == NY_E_BINARY && pat->as.binary.op &&
      strcmp(pat->as.binary.op, "..") == 0) {
    int64_t test_raw = 0, lo = 0, hi = 0;
    if (!stmt_tagged_small_int_raw(test_tagged, &test_raw) ||
        !ny_expr_literal_i64(pat->as.binary.left, &lo) ||
        !ny_expr_literal_i64(pat->as.binary.right, &hi))
      return -1;
    return (test_raw >= lo && test_raw <= hi) ? 1 : 0;
  }
  int64_t pat_tagged = 0;
  if (!stmt_const_pattern_tagged(cg, scopes, depth, pat, &pat_tagged))
    return -1;
  return pat_tagged == test_tagged ? 1 : 0;
}

static bool stmt_try_gen_const_match(codegen_t *cg, scope *scopes,
                                     size_t *depth, stmt_t *s, size_t func_root,
                                     bool is_tail) {
  if (!cg || !scopes || !depth || !s || s->kind != NY_S_MATCH ||
      !s->as.match.test || !stmt_expr_is_constish(s->as.match.test))
    return false;

  LLVMValueRef testv = gen_expr(cg, scopes, *depth, s->as.match.test);
  int64_t test_tagged = 0;
  if (!stmt_const_tagged_i64(testv, &test_tagged))
    return false;

  for (size_t i = 0; i < s->as.match.arms.len; ++i) {
    match_arm_t *arm = &s->as.match.arms.data[i];
    bool matched = false;
    for (size_t j = 0; j < arm->patterns.len; ++j) {
      int r = stmt_const_match_pattern(cg, scopes, *depth, test_tagged,
                                       arm->patterns.data[j]);
      if (r < 0)
        return false;
      if (r > 0) {
        matched = true;
        break;
      }
    }
    if (!matched)
      continue;
    if (arm->guard)
      return false;
    scope_enter(scopes, depth, scopes[*depth].break_bb,
                scopes[*depth].continue_bb);
    gen_stmt(cg, scopes, depth, arm->conseq, func_root, is_tail);
    if (!ny_has_terminator(cg))
      emit_defers(cg, scopes, *depth, *depth);
    scope_pop(scopes, depth);
    return true;
  }

  if (s->as.match.default_conseq) {
    gen_stmt(cg, scopes, depth, s->as.match.default_conseq, func_root, is_tail);
  }
  return true;
}

static const char *binding_assign_type(const binding *b) {
  if (!b)
    return NULL;
  if (b->decl_type_name && *b->decl_type_name)
    return b->decl_type_name;
  return NULL;
}

static bool stmt_bindable_inferred_type(const char *type_name) {
  if (!type_name || !*type_name || strcmp(type_name, "any") == 0 ||
      strcmp(type_name, "void") == 0)
    return false;

  if (stmt_type_name_is_narrow_fixed_int_value(type_name))
    return false;
  return true;
}

static bool stmt_bindable_mut_inferred_type(const char *type_name) {
  if (!type_name || !*type_name || strcmp(type_name, "any") == 0 ||
      strcmp(type_name, "void") == 0)
    return false;
  if (stmt_type_name_is_float_value(type_name))
    return true;

  return ny_gencall_type_is_known_obj(type_name);
}

static bool stmt_type_accepts_int_proof(const char *type_name) {
  if (!type_name || !*type_name)
    return true;
  return ny_gencall_type_is_integer_number(type_name) ||
         ny_gencall_type_is(type_name, "integer") ||
         ny_gencall_type_is(type_name, "handle");
}

static bool stmt_binding_accepts_int_proof(const binding *b) {
  if (!b)
    return true;
  const char *assign_type = binding_assign_type(b);
  bool has_assign_type = assign_type && *assign_type;
  bool has_type_name = b->type_name && *b->type_name;
  if (!has_assign_type && !has_type_name)
    return true;
  return (!has_assign_type || stmt_type_accepts_int_proof(assign_type)) &&
         (!has_type_name || stmt_type_accepts_int_proof(b->type_name));
}

static bool stmt_binding_has_float_storage(codegen_t *cg, const binding *b) {
  if (!cg || !b || !b->is_slot || !b->value)
    return false;
  LLVMTypeRef ty = NULL;
  if (LLVMIsAGlobalVariable(b->value)) {
    ty = LLVMGlobalGetValueType(b->value);
  } else if (LLVMIsAAllocaInst(b->value)) {
    ty = LLVMGetAllocatedType(b->value);
  }
  if (!ty)
    return false;
  LLVMTypeKind kind = LLVMGetTypeKind(ty);
  return kind == LLVMFloatTypeKind || kind == LLVMDoubleTypeKind ||
         kind == LLVMFP128TypeKind;
}

static void stmt_update_numeric_binding_proof(codegen_t *cg, binding *b,
                                              bool proven_int,
                                              bool proven_f64) {
  if (!b)
    return;
  bool is_global_binding = ny_binding_is_valid(cg, b);
  if (is_global_binding)
    b->raw_int_value = NULL;

  bool float_layout = stmt_type_name_is_float_value(b->decl_type_name) ||
                      stmt_type_name_is_float_value(b->type_name) ||
                      b->is_f64_slot || b->is_f64_direct;
  bool int_layout = stmt_binding_accepts_int_proof(b);

  b->is_int_slot = false;
  b->is_int_direct = false;
  b->is_int_raw_direct = false;
  b->is_f64_slot = false;
  b->is_f64_direct = false;
  b->has_int_range = false;

  if (proven_f64 && float_layout) {
    if (b->is_slot) {
      if (stmt_binding_has_float_storage(cg, b))
        b->is_f64_slot = true;
    } else {
      b->is_f64_direct = true;
    }
    return;
  }

  if (proven_int && int_layout) {
    if (b->is_slot) {
      b->is_int_slot = true;
      if (cg && !is_global_binding && !b->raw_int_value)
        b->raw_int_value = build_alloca(cg, "raw.int", cg->type_i64);
    } else {
      b->is_int_direct = true;
    }
  }
}

static void stmt_update_int_binding_range(binding *b, bool has_range,
                                          int64_t min_raw, int64_t max_raw) {
  if (!b || !(b->is_int_slot || b->is_int_direct)) {
    if (b)
      b->has_int_range = false;
    return;
  }
  b->has_int_range = has_range;
  if (has_range) {
    NY_COMPILER_ASSERTF(min_raw <= max_raw,
                        "int range inverted: min=%lld max=%lld",
                        (long long)min_raw, (long long)max_raw);
    b->int_min_raw = min_raw;
    b->int_max_raw = max_raw;
  }
}

static void stmt_store_raw_int_shadow(codegen_t *cg, binding *b,
                                      LLVMValueRef tagged_value) {
  if (!cg || !b || !b->raw_int_value || !tagged_value)
    return;
  if (ny_binding_is_valid(cg, b)) {
    b->raw_int_value = NULL;
    return;
  }
  ny_store(cg, b->raw_int_value, ny_untag_int(cg, tagged_value));
}

static bool stmt_build_raw_int_expr_const_ok(codegen_t *cg, scope *scopes,
                                             size_t depth, expr_t *expr,
                                             LLVMValueRef *out_raw) {
  if (out_raw)
    *out_raw = NULL;
  LLVMValueRef raw = NULL;
  LLVMValueRef ok = NULL;
  if (!expr ||
      !ny_build_mono_raw_int_expr(cg, scopes, depth, expr, &raw, &ok) || !raw ||
      !ok || !LLVMIsAConstantInt(ok) || LLVMConstIntGetZExtValue(ok) == 0)
    return false;
  if (out_raw)
    *out_raw = raw;
  return true;
}

static void stmt_store_raw_int_shadow_expr(codegen_t *cg, scope *scopes,
                                           size_t depth, binding *b,
                                           expr_t *expr,
                                           LLVMValueRef tagged_value) {
  if (!cg || !b || !b->raw_int_value || !tagged_value)
    return;
  LLVMValueRef raw = NULL;
  if (stmt_build_raw_int_expr_const_ok(cg, scopes, depth, expr, &raw)) {
    if (ny_binding_is_valid(cg, b)) {
      b->raw_int_value = NULL;
      return;
    }
    ny_store(cg, b->raw_int_value, raw);
    return;
  }
  stmt_store_raw_int_shadow(cg, b, tagged_value);
}

static bool stmt_expr_int_range(codegen_t *cg, scope *scopes, size_t depth,
                                expr_t *e, int64_t *out_min, int64_t *out_max) {
  if (!e)
    return false;
  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->as.literal.kind != NY_LIT_INT)
      return false;
    if (out_min)
      *out_min = e->as.literal.as.i;
    if (out_max)
      *out_max = e->as.literal.as.i;
    return true;
  case NY_E_IDENT: {
    if (!e->as.ident.name)
      return false;
    size_t name_len = (size_t)e->tok.len;
    if (name_len == 0)
      name_len = strlen(e->as.ident.name);
    binding *b = stmt_lookup_binding(cg, scopes, depth, e->as.ident.name,
                                     name_len, e->as.ident.hash);
    if (b && b->has_int_range) {
      if (out_min)
        *out_min = b->int_min_raw;
      if (out_max)
        *out_max = b->int_max_raw;
      return true;
    }
    expr_t *init =
        b && !b->is_mut ? ny_binding_var_init_expr(b, e->as.ident.name) : NULL;
    int64_t lit = 0;
    if (ny_expr_literal_i64(init, &lit)) {
      if (out_min)
        *out_min = lit;
      if (out_max)
        *out_max = lit;
      return true;
    }
    return false;
  }
  case NY_E_MEMBER: {
    if (!e->as.member.name || strcmp(e->as.member.name, "len") != 0)
      return false;
    expr_t *target = e->as.member.target;
    expr_t *init = NULL;
    if (target && (target->kind == NY_E_LITERAL || target->kind == NY_E_LIST ||
                   target->kind == NY_E_TUPLE)) {
      init = target;
    } else if (target && target->kind == NY_E_IDENT && target->as.ident.name) {
      size_t name_len = (size_t)target->tok.len;
      if (name_len == 0)
        name_len = strlen(target->as.ident.name);
      binding *b = stmt_lookup_binding(cg, scopes, depth, target->as.ident.name,
                                       name_len, target->as.ident.hash);
      init = ny_binding_var_init_expr(b, target->as.ident.name);
    }
    int64_t len = -1;
    if (init && init->kind == NY_E_LITERAL &&
        init->as.literal.kind == NY_LIT_STR)
      len = (int64_t)init->as.literal.as.s.len;
    else if (init && (init->kind == NY_E_LIST || init->kind == NY_E_TUPLE))
      len = (int64_t)init->as.list_like.len;
    if (len < 0)
      return false;
    if (out_min)
      *out_min = len;
    if (out_max)
      *out_max = len;
    return true;
  }
  case NY_E_CALL: {
    if (!e->as.call.callee || e->as.call.callee->kind != NY_E_IDENT)
      return false;
    const char *n = e->as.call.callee->as.ident.name;
    bool builtin_shadowed =
        stmt_call_builtin_name_shadowed(cg, scopes, depth, e->as.call.callee);
    if (!builtin_shadowed && n && ny_name_tail_is(n, "len") &&
        e->as.call.args.len == 1) {
      expr_t *target = e->as.call.args.data[0].val;
      expr_t *init = NULL;
      if (target && (target->kind == NY_E_LITERAL ||
                     target->kind == NY_E_LIST || target->kind == NY_E_TUPLE)) {
        init = target;
      } else if (target && target->kind == NY_E_IDENT &&
                 target->as.ident.name) {
        size_t name_len = (size_t)target->tok.len;
        if (name_len == 0)
          name_len = strlen(target->as.ident.name);
        binding *b =
            stmt_lookup_binding(cg, scopes, depth, target->as.ident.name,
                                name_len, target->as.ident.hash);
        init = ny_binding_var_init_expr(b, target->as.ident.name);
      }
      int64_t len = -1;
      if (init && init->kind == NY_E_LITERAL &&
          init->as.literal.kind == NY_LIT_STR)
        len = (int64_t)init->as.literal.as.s.len;
      else if (init && (init->kind == NY_E_LIST || init->kind == NY_E_TUPLE))
        len = (int64_t)init->as.list_like.len;
      if (len >= 0) {
        if (out_min)
          *out_min = len;
        if (out_max)
          *out_max = len;
        return true;
      }
    }
    if (!builtin_shadowed && n && e->as.call.args.len == 2 &&
        (ny_name_tail_is(n, "load8") || strcmp(n, "__load8_idx") == 0)) {
      if (out_min)
        *out_min = 0;
      if (out_max)
        *out_max = 255;
      return true;
    }
    if (!builtin_shadowed && n && e->as.call.args.len == 2 &&
        (ny_name_tail_is(n, "load16") || strcmp(n, "__load16_idx") == 0)) {
      if (out_min)
        *out_min = 0;
      if (out_max)
        *out_max = 65535;
      return true;
    }
    if (!builtin_shadowed && n && e->as.call.args.len == 2 &&
        (ny_name_tail_is(n, "load32") || strcmp(n, "__load32_idx") == 0 ||
         ny_name_tail_is(n, "load32_h"))) {
      if (out_min)
        *out_min = 0;
      if (out_max)
        *out_max = UINT32_MAX;
      return true;
    }
    if (e->as.call.args.len < 2)
      return false;
    if (!builtin_shadowed && n && ny_name_tail_is(n, "band") &&
        e->as.call.args.len == 2) {
      expr_t *rhs = e->as.call.args.data[1].val;
      if (rhs && rhs->kind == NY_E_LITERAL &&
          rhs->as.literal.kind == NY_LIT_INT && rhs->as.literal.as.i >= 0 &&
          ny_is_proven_int(cg, scopes, depth, e->as.call.args.data[0].val,
                           NULL)) {
        if (out_min)
          *out_min = 0;
        if (out_max)
          *out_max = rhs->as.literal.as.i;
        return true;
      }
    }
    bool want_builtin_get =
        !builtin_shadowed && n &&
        (strcmp(n, "get") == 0 || strcmp(n, "std.core.get") == 0 ||
         strcmp(n, "std.core.reflect.get") == 0 || ny_name_tail_is(n, "get"));
    if (!want_builtin_get)
      return false;
    expr_t *target = e->as.call.args.data[0].val;
    if (!target || target->kind != NY_E_IDENT || !target->as.ident.name)
      return false;
    size_t name_len = (size_t)target->tok.len;
    if (name_len == 0)
      name_len = strlen(target->as.ident.name);
    binding *b = stmt_lookup_binding(cg, scopes, depth, target->as.ident.name,
                                     name_len, target->as.ident.hash);
    if (b && b->is_int_list_storage && b->has_list_int_range) {
      if (out_min)
        *out_min = b->list_int_min_raw;
      if (out_max)
        *out_max = b->list_int_max_raw;
      return true;
    }
    if (b && b->is_int_dict_storage && b->has_dict_int_range) {
      if (out_min)
        *out_min = b->dict_int_min_raw;
      if (out_max)
        *out_max = b->dict_int_max_raw;
      return true;
    }
    return false;
  }
  case NY_E_MEMCALL: {
    if (!e->as.memcall.name || !ny_name_tail_is(e->as.memcall.name, "get") ||
        !e->as.memcall.target)
      return false;
    expr_t *target = e->as.memcall.target;
    if (!target || target->kind != NY_E_IDENT || !target->as.ident.name)
      return false;
    size_t name_len = (size_t)target->tok.len;
    if (name_len == 0)
      name_len = strlen(target->as.ident.name);
    binding *b = stmt_lookup_binding(cg, scopes, depth, target->as.ident.name,
                                     name_len, target->as.ident.hash);
    if (b && b->is_int_list_storage && b->has_list_int_range) {
      if (out_min)
        *out_min = b->list_int_min_raw;
      if (out_max)
        *out_max = b->list_int_max_raw;
      return true;
    }
    if (b && b->is_int_dict_storage && b->has_dict_int_range) {
      if (out_min)
        *out_min = b->dict_int_min_raw;
      if (out_max)
        *out_max = b->dict_int_max_raw;
      return true;
    }
    return false;
  }
  case NY_E_BINARY: {
    const char *op = e->as.binary.op;
    int64_t lmin = 0, lmax = 0, rmin = 0, rmax = 0;
    if (!op)
      return false;
    if (strcmp(op, "&") == 0 && e->as.binary.right &&
        e->as.binary.right->kind == NY_E_LITERAL &&
        e->as.binary.right->as.literal.kind == NY_LIT_INT &&
        e->as.binary.right->as.literal.as.i >= 0 &&
        ny_is_proven_int(cg, scopes, depth, e->as.binary.left, NULL)) {
      if (out_min)
        *out_min = 0;
      if (out_max)
        *out_max = e->as.binary.right->as.literal.as.i;
      return true;
    }
    bool lhs_has =
        stmt_expr_int_range(cg, scopes, depth, e->as.binary.left, &lmin, &lmax);
    bool rhs_has = stmt_expr_int_range(cg, scopes, depth, e->as.binary.right,
                                       &rmin, &rmax);
    if (strcmp(op, "%") == 0 && e->as.binary.right &&
        e->as.binary.right->kind == NY_E_LITERAL &&
        e->as.binary.right->as.literal.kind == NY_LIT_INT &&
        e->as.binary.right->as.literal.as.i > 0 &&
        ny_is_proven_int(cg, scopes, depth, e->as.binary.left, NULL)) {
      int64_t m = e->as.binary.right->as.literal.as.i;
      if (lhs_has && lmin >= 0) {
        if (out_min)
          *out_min = 0;
        if (out_max)
          *out_max = m - 1;
      } else {
        if (out_min)
          *out_min = -(m - 1);
        if (out_max)
          *out_max = m - 1;
      }
      return true;
    }
    if (!lhs_has || !rhs_has)
      return false;
    if (strcmp(op, "+") == 0) {
      int64_t lo = 0, hi = 0;
      if (!ny_add_range_ok(lmin, rmin, &lo) ||
          !ny_add_range_ok(lmax, rmax, &hi))
        return false;
      if (out_min)
        *out_min = lo;
      if (out_max)
        *out_max = hi;
      return true;
    }
    if (strcmp(op, "-") == 0) {
      int64_t lo = 0, hi = 0;
      if (!ny_sub_range_ok(lmin, rmax, &lo) ||
          !ny_sub_range_ok(lmax, rmin, &hi))
        return false;
      if (out_min)
        *out_min = lo;
      if (out_max)
        *out_max = hi;
      return true;
    }
    if (strcmp(op, "*") == 0) {
      int64_t c[4];
      if (!ny_mul_range_ok(lmin, rmin, &c[0]) ||
          !ny_mul_range_ok(lmin, rmax, &c[1]) ||
          !ny_mul_range_ok(lmax, rmin, &c[2]) ||
          !ny_mul_range_ok(lmax, rmax, &c[3]))
        return false;
      int64_t lo = c[0], hi = c[0];
      for (int i = 1; i < 4; ++i) {
        if (c[i] < lo)
          lo = c[i];
        if (c[i] > hi)
          hi = c[i];
      }
      if (out_min)
        *out_min = lo;
      if (out_max)
        *out_max = hi;
      return true;
    }
    if (strcmp(op, "/") == 0 && rmin == rmax && rmax > 0) {
      int64_t lo = lmin / rmax;
      int64_t hi = lmax / rmax;
      if (lo > hi) {
        int64_t tmp = lo;
        lo = hi;
        hi = tmp;
      }
      if (out_min)
        *out_min = lo;
      if (out_max)
        *out_max = hi;
      return true;
    }
    if (strcmp(op, "%") == 0 && rmin == rmax && rmax > 0) {
      if (out_min)
        *out_min = 0;
      if (out_max)
        *out_max = rmax - 1;
      return true;
    }
    return false;
  }
  default:
    return false;
  }
}

static bool stmt_try_while_trip_upper_bound(codegen_t *cg, scope *scopes,
                                            size_t depth, stmt_t *s,
                                            int64_t *trip_out) {
  if (trip_out)
    *trip_out = 0;
  if (!s || s->kind != NY_S_WHILE || !s->as.whl.test)
    return false;
  expr_t *test = s->as.whl.test;
  if (test->kind != NY_E_BINARY || !test->as.binary.op)
    return false;
  const char *op = test->as.binary.op;
  bool strict_lt = strcmp(op, "<") == 0;
  bool strict_le = strcmp(op, "<=") == 0;
  if (!strict_lt && !strict_le)
    return false;
  expr_t *lhs = test->as.binary.left;
  expr_t *rhs = test->as.binary.right;
  if (!lhs || lhs->kind != NY_E_IDENT || !lhs->as.ident.name || !rhs)
    return false;

  int64_t start_lo = 0, start_hi = 0, bound_lo = 0, bound_hi = 0, step = 0;
  if (!stmt_expr_int_range(cg, scopes, depth, lhs, &start_lo, &start_hi) ||
      start_lo != start_hi)
    return false;
  if (!stmt_expr_int_range(cg, scopes, depth, rhs, &bound_lo, &bound_hi) ||
      bound_lo != bound_hi)
    return false;
  if (s->as.whl.update)
    (void)stmt_find_loop_step_in_stmt(s->as.whl.update, lhs->as.ident.name,
                                      &step);
  if (step <= 0 &&
      !stmt_find_loop_step_in_stmt(s->as.whl.body, lhs->as.ident.name, &step))
    return false;
  if (step <= 0)
    return false;

  int64_t start = start_lo;
  int64_t bound = bound_lo;
  int64_t trip = 0;
  if (strict_lt) {
    if (start >= bound)
      trip = 0;
    else {
      int64_t span = bound - start;
      trip = (span + step - 1) / step;
    }
  } else {
    if (start > bound)
      trip = 0;
    else {
      int64_t span = bound - start;
      trip = (span / step) + 1;
    }
  }
  if (trip < 0)
    return false;
  if (trip_out)
    *trip_out = trip;
  return true;
}

static bool stmt_expr_is_stable_dynamic_trip_bound(expr_t *e) {
  if (!e)
    return false;
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_INT)
    return true;
  return e->kind == NY_E_IDENT && e->as.ident.name;
}

static LLVMValueRef stmt_try_dynamic_while_trip_count_raw(codegen_t *cg,
                                                          scope *scopes,
                                                          size_t depth,
                                                          stmt_t *s) {
  if (!cg || !s || s->kind != NY_S_WHILE || !s->as.whl.test)
    return NULL;
  expr_t *test = s->as.whl.test;
  if (test->kind != NY_E_BINARY || !test->as.binary.op)
    return NULL;
  bool strict_lt = strcmp(test->as.binary.op, "<") == 0;
  bool strict_le = strcmp(test->as.binary.op, "<=") == 0;
  if (!strict_lt && !strict_le)
    return NULL;
  expr_t *lhs = test->as.binary.left;
  expr_t *rhs = test->as.binary.right;
  if (!lhs || lhs->kind != NY_E_IDENT || !lhs->as.ident.name ||
      !stmt_expr_is_stable_dynamic_trip_bound(rhs))
    return NULL;

  int64_t start_lo = 0, start_hi = 0, step = 0;
  if (!stmt_expr_int_range(cg, scopes, depth, lhs, &start_lo, &start_hi) ||
      start_lo != start_hi)
    return NULL;
  if (s->as.whl.update)
    (void)stmt_find_loop_step_in_stmt(s->as.whl.update, lhs->as.ident.name,
                                      &step);
  if (step <= 0 &&
      !stmt_find_loop_step_in_stmt(s->as.whl.body, lhs->as.ident.name, &step))
    return NULL;
  if (step <= 0)
    return NULL;

  LLVMValueRef rhs_v = gen_expr(cg, scopes, depth, rhs);
  if (!rhs_v)
    return NULL;
  if (LLVMTypeOf(rhs_v) != cg->type_i64)
    rhs_v = ny_ptr2i64(cg, rhs_v, NY_LLVM_NAME(cg, "dynamic_trip_bound"));
  LLVMValueRef rhs_raw = ny_untag_int(cg, rhs_v);
  LLVMValueRef span = ny_sub(
      cg, rhs_raw, LLVMConstInt(cg->type_i64, (uint64_t)start_lo, true),
      NY_LLVM_NAME(cg, "dynamic_trip_span"));
  if (strict_le)
    span = ny_add(cg, span, LLVMConstInt(cg->type_i64, 1, true),
                  NY_LLVM_NAME(cg, "dynamic_trip_span_le"));
  LLVMValueRef positive =
      LLVMBuildICmp(cg->builder, LLVMIntSGT, span, ny_c0(cg),
                    NY_LLVM_NAME(cg, "dynamic_trip_positive"));
  LLVMValueRef nonneg_span =
      ny_select(cg, positive, span, ny_c0(cg), "dynamic_trip_nonneg_span");
  if (step == 1)
    return nonneg_span;
  LLVMValueRef numerator =
      ny_add(cg, nonneg_span, LLVMConstInt(cg->type_i64, (uint64_t)(step - 1), true),
             NY_LLVM_NAME(cg, "dynamic_trip_num"));
  return LLVMBuildSDiv(cg->builder, numerator,
                       LLVMConstInt(cg->type_i64, (uint64_t)step, true),
                       NY_LLVM_NAME(cg, "dynamic_trip_raw"));
}

static void stmt_try_preseed_accumulator_assign(codegen_t *cg, scope *scopes,
                                                size_t depth, stmt_t *s,
                                                int64_t trip_count,
                                                const char *skip_name) {
  if (!s || s->kind != NY_S_VAR || s->as.var.is_decl || trip_count <= 0)
    return;
  for (size_t i = 0; i < s->as.var.names.len && i < s->as.var.exprs.len; ++i) {
    const char *name = s->as.var.names.data[i];
    expr_t *expr = s->as.var.exprs.data[i];
    if (skip_name && name && strcmp(name, skip_name) == 0)
      continue;
    if (!name || !expr || expr->kind != NY_E_BINARY || !expr->as.binary.op)
      continue;
    const char *op = expr->as.binary.op;
    bool is_add = strcmp(op, "+") == 0;
    bool is_sub = strcmp(op, "-") == 0;
    if (!is_add && !is_sub)
      continue;
    expr_t *lhs = expr->as.binary.left;
    expr_t *rhs = expr->as.binary.right;
    if (!lhs || lhs->kind != NY_E_IDENT || !lhs->as.ident.name ||
        strcmp(lhs->as.ident.name, name) != 0)
      continue;

    size_t name_len = strlen(name);
    binding *b = stmt_lookup_binding(cg, scopes, depth, name, name_len, 0);
    if (!b || !(b->is_int_slot || b->is_int_direct) || !b->has_int_range)
      continue;

    int64_t rhs_min = 0, rhs_max = 0;
    if (!stmt_expr_int_range(cg, scopes, depth, rhs, &rhs_min, &rhs_max))
      continue;
    int64_t span_min = 0, span_max = 0;
    if (!ny_mul_range_ok(rhs_min, trip_count, &span_min) ||
        !ny_mul_range_ok(rhs_max, trip_count, &span_max))
      continue;

    int64_t new_min = 0, new_max = 0;
    if (is_add) {
      if (!ny_add_range_ok(b->int_min_raw, span_min, &new_min) ||
          !ny_add_range_ok(b->int_max_raw, span_max, &new_max))
        continue;
    } else {
      if (!ny_sub_range_ok(b->int_min_raw, span_max, &new_min) ||
          !ny_sub_range_ok(b->int_max_raw, span_min, &new_max))
        continue;
    }
    stmt_update_int_binding_range(b, true, new_min, new_max);
  }
}

static void stmt_preseed_loop_accumulator_ranges(codegen_t *cg, scope *scopes,
                                                 size_t depth, stmt_t *body,
                                                 int64_t trip_count,
                                                 const char *skip_name) {
  if (!body || trip_count <= 0)
    return;
  switch (body->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < body->as.block.body.len; ++i)
      stmt_preseed_loop_accumulator_ranges(cg, scopes, depth,
                                           body->as.block.body.data[i],
                                           trip_count, skip_name);
    return;
  case NY_S_VAR:
    stmt_try_preseed_accumulator_assign(cg, scopes, depth, body, trip_count,
                                        skip_name);
    return;
  case NY_S_WHILE: {
    int64_t inner_trip = 0;
    if (stmt_try_while_trip_upper_bound(cg, scopes, depth, body, &inner_trip) &&
        inner_trip > 0) {
      int64_t total_trip = 0;
      if (ny_mul_range_ok(trip_count, inner_trip, &total_trip) &&
          total_trip > 0)
        stmt_preseed_loop_accumulator_ranges(
            cg, scopes, depth, body->as.whl.body, total_trip, skip_name);
    }
    return;
  }
  case NY_S_IF:
    stmt_preseed_loop_accumulator_ranges(cg, scopes, depth, body->as.iff.conseq,
                                         trip_count, skip_name);
    if (body->as.iff.alt)
      stmt_preseed_loop_accumulator_ranges(cg, scopes, depth, body->as.iff.alt,
                                           trip_count, skip_name);
    return;
  default:
    return;
  }
}

static const char *stmt_while_lhs_name(stmt_t *s) {
  if (!s || s->kind != NY_S_WHILE || !s->as.whl.test)
    return NULL;
  expr_t *test = s->as.whl.test;
  while (test && test->kind == NY_E_LOGICAL && test->as.logical.op &&
         strcmp(test->as.logical.op, "&&") == 0)
    test = test->as.logical.left;
  if (test->kind != NY_E_BINARY || !test->as.binary.op)
    return NULL;
  if (strcmp(test->as.binary.op, "<") != 0 &&
      strcmp(test->as.binary.op, "<=") != 0)
    return NULL;
  expr_t *lhs = test->as.binary.left;
  return (lhs && lhs->kind == NY_E_IDENT) ? lhs->as.ident.name : NULL;
}

static void stmt_preseed_loop_index_ranges(codegen_t *cg, scope *scopes,
                                           size_t depth, stmt_t *body,
                                           int64_t trip_count) {
  (void)cg;
  if (!scopes || !body || trip_count <= 0)
    return;
  switch (body->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < body->as.block.body.len; ++i)
      stmt_preseed_loop_index_ranges(cg, scopes, depth,
                                     body->as.block.body.data[i], trip_count);
    return;
  case NY_S_VAR:
    if (!body->as.var.is_decl && !body->as.var.is_del) {
      for (size_t i = 0;
           i < body->as.var.names.len && i < body->as.var.exprs.len; ++i) {
        const char *name = body->as.var.names.data[i];
        expr_t *expr = body->as.var.exprs.data[i];
        int64_t init = 0;
        if (!name || !ny_expr_literal_i64(expr, &init) || init < 0)
          continue;
        int64_t max_v = 0;
        if (!ny_add_range_ok(init, trip_count - 1, &max_v))
          continue;
        size_t name_len = strlen(name);
        binding *b = stmt_lookup_binding(cg, scopes, depth, name, name_len, 0);
        if (b && (b->is_int_direct || b->is_int_slot))
          stmt_update_int_binding_range(b, true, init, max_v);
      }
    }
    return;
  default:
    return;
  }
}

static void stmt_preseed_while_index_range(codegen_t *cg, scope *scopes,
                                           size_t depth, stmt_t *s,
                                           int64_t trip_count) {
  if (!cg || !scopes || !s || s->kind != NY_S_WHILE || !s->as.whl.test ||
      trip_count <= 0)
    return;
  expr_t *test = s->as.whl.test;
  if (test->kind != NY_E_BINARY || !test->as.binary.op)
    return;
  if (strcmp(test->as.binary.op, "<") != 0 &&
      strcmp(test->as.binary.op, "<=") != 0)
    return;
  expr_t *lhs = test->as.binary.left;
  if (!lhs || lhs->kind != NY_E_IDENT || !lhs->as.ident.name)
    return;
  int64_t start_lo = 0, start_hi = 0, step = 0;
  if (!stmt_expr_int_range(cg, scopes, depth, lhs, &start_lo, &start_hi) ||
      start_lo != start_hi)
    return;
  if (s->as.whl.update)
    (void)stmt_find_loop_step_in_stmt(s->as.whl.update, lhs->as.ident.name,
                                      &step);
  if (step <= 0 &&
      !stmt_find_loop_step_in_stmt(s->as.whl.body, lhs->as.ident.name, &step))
    return;
  if (step <= 0)
    return;
  int64_t span = 0, max_v = 0;
  if (!ny_mul_range_ok(step, trip_count - 1, &span) ||
      !ny_add_range_ok(start_lo, span, &max_v))
    return;
  size_t name_len = (size_t)lhs->tok.len;
  if (name_len == 0)
    name_len = strlen(lhs->as.ident.name);
  binding *b = stmt_lookup_binding(cg, scopes, depth, lhs->as.ident.name,
                                   name_len, lhs->as.ident.hash);
  if (b && (b->is_int_direct || b->is_int_slot))
    stmt_update_int_binding_range(b, true, start_lo, max_v);
}

static void stmt_preseed_while_condition_index_range(codegen_t *cg,
                                                     scope *scopes,
                                                     size_t depth, stmt_t *s) {
  if (!cg || !scopes || !s || s->kind != NY_S_WHILE || !s->as.whl.test)
    return;
  expr_t *test = s->as.whl.test;
  if (test->kind != NY_E_BINARY || !test->as.binary.op)
    return;
  bool strict_lt = strcmp(test->as.binary.op, "<") == 0;
  bool strict_le = strcmp(test->as.binary.op, "<=") == 0;
  if (!strict_lt && !strict_le)
    return;
  expr_t *lhs = test->as.binary.left;
  expr_t *rhs = test->as.binary.right;
  if (!lhs || lhs->kind != NY_E_IDENT || !lhs->as.ident.name || !rhs)
    return;
  int64_t lhs_min = 0, lhs_max = 0, rhs_min = 0, rhs_max = 0;
  if (!stmt_expr_int_range(cg, scopes, depth, lhs, &lhs_min, &lhs_max) ||
      lhs_min < 0)
    return;
  if (!stmt_expr_int_range(cg, scopes, depth, rhs, &rhs_min, &rhs_max) ||
      rhs_max < 0)
    return;
  int64_t hi = rhs_max;
  if (strict_lt && !ny_sub_range_ok(rhs_max, 1, &hi))
    return;
  if (hi < lhs_min)
    return;
  size_t name_len = (size_t)lhs->tok.len;
  if (name_len == 0)
    name_len = strlen(lhs->as.ident.name);
  binding *b = stmt_lookup_binding(cg, scopes, depth, lhs->as.ident.name,
                                   name_len, lhs->as.ident.hash);
  if (!b)
    return;
  if (!b->is_int_direct && !b->is_int_slot)
    return;
  stmt_update_int_binding_range(b, true, lhs_min, hi);
}

static bool stmt_loop_body_has_control_exit(stmt_t *s) {
  if (!s)
    return false;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      if (stmt_loop_body_has_control_exit(s->as.block.body.data[i]))
        return true;
    }
    return false;
  case NY_S_IF:
    return stmt_loop_body_has_control_exit(s->as.iff.conseq) ||
           stmt_loop_body_has_control_exit(s->as.iff.alt);
  case NY_S_TRY:
    return stmt_loop_body_has_control_exit(s->as.tr.body) ||
           stmt_loop_body_has_control_exit(s->as.tr.handler);
  case NY_S_MATCH:
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      if (stmt_loop_body_has_control_exit(s->as.match.arms.data[i].conseq))
        return true;
    }
    return stmt_loop_body_has_control_exit(s->as.match.default_conseq);
  case NY_S_WHILE:
  case NY_S_FOR:
  case NY_S_RETURN:
  case NY_S_GOTO:
  case NY_S_BREAK:
  case NY_S_CONTINUE:
    return true;
  default:
    return false;
  }
}

typedef struct stmt_str_append_loop_t {
  const char *name;
  LLVMValueRef builder;
  LLVMValueRef slot;
  binding *binding;
  bool active;
} stmt_str_append_loop_t;

static bool stmt_expr_is_stringish(codegen_t *cg, scope *scopes, size_t depth,
                                   expr_t *e) {
  if (!e)
    return false;
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_STR)
    return true;
  return ny_type_is(infer_expr_type(cg, scopes, depth, e), "str");
}

static bool stmt_expr_is_self_str_concat(codegen_t *cg, scope *scopes,
                                         size_t depth, expr_t *e,
                                         const char *name,
                                         expr_t **piece_out) {
  if (piece_out)
    *piece_out = NULL;
  if (!e || !name || e->kind != NY_E_BINARY || !e->as.binary.op ||
      strcmp(e->as.binary.op, "+") != 0 || !e->as.binary.left ||
      !e->as.binary.right)
    return false;
  if (!ny_expr_ident_is_name(e->as.binary.left, name))
    return false;
  if (stmt_expr_contains_ident_name(e->as.binary.right, name))
    return false;
  if (!stmt_expr_is_stringish(cg, scopes, depth, e->as.binary.right))
    return false;
  if (piece_out)
    *piece_out = e->as.binary.right;
  return true;
}

static void stmt_collect_str_append_candidate(codegen_t *cg, scope *scopes,
                                              size_t depth, stmt_t *s,
                                              const char **name_out,
                                              bool *ambiguous) {
  if (!s || !name_out || !ambiguous || *ambiguous)
    return;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      stmt_collect_str_append_candidate(cg, scopes, depth,
                                        s->as.block.body.data[i], name_out,
                                        ambiguous);
    return;
  case NY_S_VAR:
    if (!s->as.var.is_decl && s->as.var.names.len == 1 &&
        s->as.var.exprs.len == 1 && s->as.var.names.data[0] &&
        stmt_expr_is_self_str_concat(cg, scopes, depth,
                                     s->as.var.exprs.data[0],
                                     s->as.var.names.data[0], NULL)) {
      const char *name = s->as.var.names.data[0];
      if (*name_out && strcmp(*name_out, name) != 0)
        *ambiguous = true;
      else
        *name_out = name;
    }
    return;
  case NY_S_IF:
    stmt_collect_str_append_candidate(cg, scopes, depth, s->as.iff.conseq,
                                      name_out, ambiguous);
    stmt_collect_str_append_candidate(cg, scopes, depth, s->as.iff.alt,
                                      name_out, ambiguous);
    return;
  case NY_S_MATCH:
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      stmt_collect_str_append_candidate(cg, scopes, depth,
                                        s->as.match.arms.data[i].conseq,
                                        name_out, ambiguous);
    stmt_collect_str_append_candidate(cg, scopes, depth,
                                      s->as.match.default_conseq, name_out,
                                      ambiguous);
    return;
  default:
    return;
  }
}

static bool stmt_str_append_body_only_uses(codegen_t *cg, scope *scopes,
                                           size_t depth, stmt_t *s,
                                           const char *name) {
  if (!s || !name)
    return true;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      if (!stmt_str_append_body_only_uses(cg, scopes, depth,
                                          s->as.block.body.data[i], name))
        return false;
    }
    return true;
  case NY_S_VAR:
    if (!s->as.var.is_decl && s->as.var.names.len == 1 &&
        s->as.var.exprs.len == 1 && s->as.var.names.data[0] &&
        strcmp(s->as.var.names.data[0], name) == 0) {
      return stmt_expr_is_self_str_concat(cg, scopes, depth,
                                          s->as.var.exprs.data[0], name, NULL);
    }
    for (size_t i = 0; i < s->as.var.names.len; ++i) {
      if (s->as.var.names.data[i] &&
          strcmp(s->as.var.names.data[i], name) == 0)
        return false;
    }
    return !stmt_contains_ident_name(s, name);
  case NY_S_IF:
    return !stmt_contains_ident_name(s->as.iff.init, name) &&
           !stmt_expr_contains_ident_name(s->as.iff.test, name) &&
           stmt_str_append_body_only_uses(cg, scopes, depth,
                                          s->as.iff.conseq, name) &&
           stmt_str_append_body_only_uses(cg, scopes, depth, s->as.iff.alt,
                                          name);
  case NY_S_MATCH:
    if (stmt_expr_contains_ident_name(s->as.match.test, name))
      return false;
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      match_arm_t *arm = &s->as.match.arms.data[i];
      for (size_t p = 0; p < arm->patterns.len; ++p) {
        if (stmt_expr_contains_ident_name(arm->patterns.data[p], name))
          return false;
      }
      if (stmt_expr_contains_ident_name(arm->guard, name))
        return false;
      if (!stmt_str_append_body_only_uses(cg, scopes, depth, arm->conseq,
                                          name))
        return false;
    }
    return stmt_str_append_body_only_uses(cg, scopes, depth,
                                          s->as.match.default_conseq, name);
  default:
    return !stmt_contains_ident_name(s, name);
  }
}

static bool stmt_prepare_str_append_loop(codegen_t *cg, scope *scopes,
                                         size_t depth, stmt_t *s,
                                         stmt_str_append_loop_t *out) {
  if (out)
    *out = (stmt_str_append_loop_t){0};
  if (!out || !ny_env_enabled_default_on("NYTRIX_STRING_APPEND_LOOP_BUILDER") ||
      !s || s->kind != NY_S_WHILE || stmt_loop_body_has_control_exit(s->as.whl.body))
    return false;
  const char *name = NULL;
  bool ambiguous = false;
  stmt_collect_str_append_candidate(cg, scopes, depth, s->as.whl.body, &name,
                                    &ambiguous);
  if (!name || ambiguous)
    return false;
  if (stmt_expr_contains_ident_name(s->as.whl.test, name) ||
      stmt_contains_ident_name(s->as.whl.init, name) ||
      stmt_contains_ident_name(s->as.whl.update, name))
    return false;
  if (!stmt_str_append_body_only_uses(cg, scopes, depth, s->as.whl.body, name))
    return false;
  binding *b = stmt_lookup_binding_no_mark(scopes, depth, name, strlen(name), 0);
  if (!b || !b->is_mut || !b->is_slot || !b->value)
    return false;
  const char *bt = b->decl_type_name ? b->decl_type_name : b->type_name;
  if (!ny_type_is(bt, "str"))
    return false;
  out->name = name;
  out->slot = b->value;
  out->binding = b;
  return true;
}

static bool stmt_begin_str_append_loop(codegen_t *cg,
                                       stmt_str_append_loop_t *loop) {
  if (!cg || !loop || !loop->name || !loop->slot)
    return false;
  fun_sig *new_sig = lookup_fun(cg, "__str_builder_new", 0);
  fun_sig *append_sig = lookup_fun(cg, "__str_builder_append", 0);
  if (!new_sig || !append_sig || !new_sig->type || !append_sig->type)
    return false;
  LLVMValueRef cap = LLVMConstInt(cg->type_i64, (64u << 1) | 1u, false);
  LLVMValueRef builder =
      LLVMBuildCall2(cg->builder, new_sig->type, new_sig->value,
                     (LLVMValueRef[]){cap}, 1,
                     NY_LLVM_NAME(cg, "str_loop_builder"));
  LLVMValueRef current =
      ny_load(cg, loop->slot, NY_LLVM_NAME(cg, "str_loop_initial"));
  LLVMBuildCall2(cg->builder, append_sig->type, append_sig->value,
                 (LLVMValueRef[]){builder, current}, 2,
                 NY_LLVM_NAME(cg, "str_loop_seed"));
  loop->builder = builder;
  loop->active = true;
  return true;
}

static void stmt_finish_str_append_loop(codegen_t *cg,
                                        stmt_str_append_loop_t *loop) {
  if (!cg || !loop || !loop->active || !loop->builder || !loop->slot)
    return;
  fun_sig *to_str_sig = lookup_fun(cg, "__str_builder_to_str", 0);
  fun_sig *free_sig = lookup_fun(cg, "__str_builder_free", 0);
  if (!to_str_sig || !to_str_sig->type)
    return;
  LLVMValueRef out =
      LLVMBuildCall2(cg->builder, to_str_sig->type, to_str_sig->value,
                     (LLVMValueRef[]){loop->builder}, 1,
                     NY_LLVM_NAME(cg, "str_loop_result"));
  ny_store(cg, loop->slot, out);
  if (loop->binding) {
    loop->binding->type_name = "str";
    if (!loop->binding->decl_type_name)
      loop->binding->decl_type_name = "str";
  }
  if (free_sig && free_sig->type)
    LLVMBuildCall2(cg->builder, free_sig->type, free_sig->value,
                   (LLVMValueRef[]){loop->builder}, 1,
                   NY_LLVM_NAME(cg, "str_loop_free"));
}

static bool stmt_try_emit_active_str_append_assignment(codegen_t *cg,
                                                       scope *scopes,
                                                       size_t depth,
                                                       const char *name,
                                                       expr_t *rhs) {
  if (!cg || !cg->active_str_append_name || !cg->active_str_append_builder ||
      !name || strcmp(cg->active_str_append_name, name) != 0)
    return false;
  expr_t *piece = NULL;
  if (!stmt_expr_is_self_str_concat(cg, scopes, depth, rhs, name, &piece))
    return false;
  fun_sig *append_sig = lookup_fun(cg, "__str_builder_append", 0);
  if (!append_sig || !append_sig->type || !append_sig->value) {
    ny_diag_error(rhs ? rhs->tok : (token_t){0},
                  "internal string append loop lowering requires __str_builder_append");
    cg->had_error = 1;
    return true;
  }
  LLVMValueRef value = gen_expr(cg, scopes, depth, piece);
  LLVMBuildCall2(cg->builder, append_sig->type, append_sig->value,
                 (LLVMValueRef[]){cg->active_str_append_builder, value}, 2,
                 NY_LLVM_NAME(cg, "str_loop_append"));
  cg->active_str_append_used = true;
  return true;
}

typedef struct stmt_loop_append_len_snapshot_t {
  binding *b;
  bool active;
  int64_t before_len_min;
  int64_t appends_per_iter;
} stmt_loop_append_len_snapshot_t;

typedef struct stmt_loop_dict_set_snapshot_t {
  binding *b;
  bool active;
  int64_t sets_per_iter;
} stmt_loop_dict_set_snapshot_t;

static stmt_loop_append_len_snapshot_t *
stmt_find_append_len_snapshot(stmt_loop_append_len_snapshot_t *snaps,
                              size_t count, binding *b) {
  if (!snaps || !b)
    return NULL;
  for (size_t i = 0; i < count; ++i) {
    if (snaps[i].active && snaps[i].b == b)
      return &snaps[i];
  }
  return NULL;
}

static void
stmt_record_loop_append_len_snapshot(stmt_loop_append_len_snapshot_t *snaps,
                                     size_t *count, size_t cap, binding *b) {
  if (!snaps || !count || *count >= cap || !b || !b->is_list_storage ||
      !b->has_list_len_min)
    return;
  stmt_loop_append_len_snapshot_t *snap =
      stmt_find_append_len_snapshot(snaps, *count, b);
  if (snap) {
    if (snap->appends_per_iter < INT64_MAX)
      snap->appends_per_iter++;
    return;
  }
  snaps[*count] = (stmt_loop_append_len_snapshot_t){
      .b = b,
      .active = true,
      .before_len_min = b->list_len_min_raw,
      .appends_per_iter = 1,
  };
  (*count)++;
}

static void stmt_collect_loop_append_len_snapshots(
    codegen_t *cg, scope *scopes, size_t depth, stmt_t *s,
    stmt_loop_append_len_snapshot_t *snaps, size_t *count, size_t cap) {
  if (!s || !snaps || !count || *count >= cap)
    return;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len && *count < cap; ++i)
      stmt_collect_loop_append_len_snapshots(
          cg, scopes, depth, s->as.block.body.data[i], snaps, count, cap);
    return;
  case NY_S_VAR:
    if (s->as.var.is_decl || s->as.var.is_del)
      return;
    for (size_t i = 0;
         i < s->as.var.names.len && i < s->as.var.exprs.len && *count < cap;
         ++i) {
      const char *name = s->as.var.names.data[i];
      if (!name ||
          !stmt_expr_is_append_to_name(s->as.var.exprs.data[i], name, NULL))
        continue;
      binding *b =
          stmt_lookup_binding(cg, scopes, depth, name, strlen(name), 0);
      stmt_record_loop_append_len_snapshot(snaps, count, cap, b);
    }
    return;
  default:
    return;
  }
}

static bool stmt_expr_is_dict_set_to_name(expr_t *e, const char *name) {
  if (!e || !name)
    return false;
  if (e->kind == NY_E_MEMCALL && e->as.memcall.name &&
      ny_name_tail_is(e->as.memcall.name, "set") &&
      e->as.memcall.args.len == 2)
    return ny_expr_ident_is_name(e->as.memcall.target, name);
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT &&
      e->as.call.callee->as.ident.name &&
      ny_name_tail_is(e->as.call.callee->as.ident.name, "set") &&
      e->as.call.args.len == 3)
    return ny_expr_ident_is_name(e->as.call.args.data[0].val, name);
  return false;
}

static stmt_loop_dict_set_snapshot_t *
stmt_find_dict_set_snapshot(stmt_loop_dict_set_snapshot_t *snaps,
                            size_t count, binding *b) {
  if (!snaps || !b)
    return NULL;
  for (size_t i = 0; i < count; ++i) {
    if (snaps[i].active && snaps[i].b == b)
      return &snaps[i];
  }
  return NULL;
}

static void
stmt_record_loop_dict_set_snapshot(stmt_loop_dict_set_snapshot_t *snaps,
                                   size_t *count, size_t cap, binding *b) {
  const char *bt = b ? (b->decl_type_name ? b->decl_type_name : b->type_name) : NULL;
  if (!snaps || !count || *count >= cap || !b ||
      (!b->is_dict_storage && !ny_type_is(bt, "dict")))
    return;
  stmt_loop_dict_set_snapshot_t *snap =
      stmt_find_dict_set_snapshot(snaps, *count, b);
  if (snap) {
    if (snap->sets_per_iter < INT64_MAX)
      snap->sets_per_iter++;
    return;
  }
  snaps[*count] = (stmt_loop_dict_set_snapshot_t){
      .b = b,
      .active = true,
      .sets_per_iter = 1,
  };
  (*count)++;
}

static void stmt_collect_loop_dict_set_snapshots(
    codegen_t *cg, scope *scopes, size_t depth, stmt_t *s,
    stmt_loop_dict_set_snapshot_t *snaps, size_t *count, size_t cap) {
  if (!s || !snaps || !count || *count >= cap)
    return;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len && *count < cap; ++i)
      stmt_collect_loop_dict_set_snapshots(
          cg, scopes, depth, s->as.block.body.data[i], snaps, count, cap);
    return;
  case NY_S_VAR:
    if (s->as.var.is_decl || s->as.var.is_del)
      return;
    for (size_t i = 0;
         i < s->as.var.names.len && i < s->as.var.exprs.len && *count < cap;
         ++i) {
      const char *name = s->as.var.names.data[i];
      if (!name || !stmt_expr_is_dict_set_to_name(s->as.var.exprs.data[i], name))
        continue;
      binding *b =
          stmt_lookup_binding(cg, scopes, depth, name, strlen(name), 0);
      stmt_record_loop_dict_set_snapshot(snaps, count, cap, b);
    }
    return;
  default:
    return;
  }
}

static void
stmt_apply_loop_append_len_snapshots(stmt_loop_append_len_snapshot_t *snaps,
                                     size_t count, int64_t trip_count) {
  if (!snaps || trip_count <= 0)
    return;
  for (size_t i = 0; i < count; ++i) {
    stmt_loop_append_len_snapshot_t *snap = &snaps[i];
    binding *b = snap->b;
    if (!snap->active || !b || !b->is_list_storage || !snap->appends_per_iter)
      continue;
    int64_t total_delta = 0, expected_after_one = 0, desired = 0;
    if (!ny_mul_range_ok(snap->appends_per_iter, trip_count, &total_delta) ||
        !ny_add_range_ok(snap->before_len_min, total_delta, &desired) ||
        !ny_add_range_ok(snap->before_len_min, snap->appends_per_iter,
                         &expected_after_one))
      continue;
    int64_t current =
        b->has_list_len_min ? b->list_len_min_raw : snap->before_len_min;
    if (current >= expected_after_one) {
      int64_t remaining_delta = 0, from_current = 0;
      if (ny_mul_range_ok(snap->appends_per_iter, trip_count - 1,
                          &remaining_delta) &&
          ny_add_range_ok(current, remaining_delta, &from_current))
        desired = from_current;
    }
    if (!b->has_list_len_min || b->list_len_min_raw < desired)
      stmt_update_list_binding_len_min(b, true, desired);
  }
}

static void stmt_reserve_loop_dict_sets(codegen_t *cg,
                                        stmt_loop_dict_set_snapshot_t *snaps,
                                        size_t count, int64_t trip_count) {
  if (!cg || !snaps || trip_count <= 0 ||
      !ny_env_enabled_default_on("NYTRIX_LOOP_DICT_RESERVE"))
    return;
  fun_sig *reserve_sig = lookup_fun(cg, "__dict_reserve", 0);
  if (!reserve_sig || !reserve_sig->type || !reserve_sig->value)
    return;
  for (size_t i = 0; i < count; ++i) {
    stmt_loop_dict_set_snapshot_t *snap = &snaps[i];
    binding *b = snap->b;
    const char *bt = b ? (b->decl_type_name ? b->decl_type_name : b->type_name) : NULL;
    if (!snap->active || !b ||
        (!b->is_dict_storage && !ny_type_is(bt, "dict")) || !b->is_slot ||
        !b->value || !snap->sets_per_iter)
      continue;
    int64_t additional = 0;
    if (!ny_mul_range_ok(snap->sets_per_iter, trip_count, &additional) ||
        additional <= 0 || additional > NY_SMALL_INT_MAX)
      continue;
    LLVMValueRef cur = ny_load(cg, b->value, NY_LLVM_NAME(cg, "dict_reserve_cur"));
    LLVMValueRef add =
        LLVMConstInt(cg->type_i64, ((uint64_t)additional << 1) | 1u, false);
    LLVMValueRef out =
        LLVMBuildCall2(cg->builder, reserve_sig->type, reserve_sig->value,
                       (LLVMValueRef[]){cur, add}, 2,
                       NY_LLVM_NAME(cg, "dict_reserve"));
    ny_store(cg, b->value, out);
  }
}

static void stmt_reserve_loop_dict_sets_dynamic(
    codegen_t *cg, stmt_loop_dict_set_snapshot_t *snaps, size_t count,
    LLVMValueRef trip_count_raw) {
  if (!cg || !snaps || !trip_count_raw ||
      !ny_env_enabled_default_on("NYTRIX_LOOP_DICT_RESERVE"))
    return;
  fun_sig *reserve_sig = lookup_fun(cg, "__dict_reserve", 0);
  if (!reserve_sig || !reserve_sig->type || !reserve_sig->value)
    return;
  for (size_t i = 0; i < count; ++i) {
    stmt_loop_dict_set_snapshot_t *snap = &snaps[i];
    binding *b = snap->b;
    const char *bt = b ? (b->decl_type_name ? b->decl_type_name : b->type_name) : NULL;
    if (!snap->active || !b ||
        (!b->is_dict_storage && !ny_type_is(bt, "dict")) || !b->is_slot ||
        !b->value || !snap->sets_per_iter)
      continue;
    LLVMValueRef additional = trip_count_raw;
    if (snap->sets_per_iter != 1)
      additional =
          LLVMBuildMul(cg->builder, additional,
                       LLVMConstInt(cg->type_i64,
                                    (uint64_t)snap->sets_per_iter, false),
                       NY_LLVM_NAME(cg, "dict_reserve_additional_raw"));
    LLVMValueRef tagged =
        ny_or(cg, ny_shl(cg, additional, ny_c1(cg),
                         NY_LLVM_NAME(cg, "dict_reserve_additional_shl")),
              ny_c1(cg), NY_LLVM_NAME(cg, "dict_reserve_additional"));
    LLVMValueRef cur = ny_load(cg, b->value, NY_LLVM_NAME(cg, "dict_reserve_cur"));
    LLVMValueRef out =
        LLVMBuildCall2(cg->builder, reserve_sig->type, reserve_sig->value,
                       (LLVMValueRef[]){cur, tagged}, 2,
                       NY_LLVM_NAME(cg, "dict_reserve_dynamic"));
    ny_store(cg, b->value, out);
  }
}

static void stmt_reserve_loop_append_lists(codegen_t *cg,
                                           stmt_loop_append_len_snapshot_t *snaps,
                                           size_t count, int64_t trip_count) {
  if (!cg || !snaps || trip_count <= 0 ||
      !ny_env_enabled("NYTRIX_LOOP_LIST_RESERVE"))
    return;
  fun_sig *reserve_sig = lookup_fun(cg, "__list_reserve", 0);
  if (!reserve_sig || !reserve_sig->type || !reserve_sig->value)
    return;
  for (size_t i = 0; i < count; ++i) {
    stmt_loop_append_len_snapshot_t *snap = &snaps[i];
    binding *b = snap->b;
    if (!snap->active || !b || !b->is_list_storage || !b->is_slot ||
        !b->value || !snap->appends_per_iter)
      continue;
    int64_t total_delta = 0, desired = 0;
    if (!ny_mul_range_ok(snap->appends_per_iter, trip_count, &total_delta) ||
        !ny_add_range_ok(snap->before_len_min, total_delta, &desired) ||
        desired <= snap->before_len_min || desired <= 0 ||
        desired > NY_SMALL_INT_MAX)
      continue;
    LLVMValueRef cur = ny_load(cg, b->value, NY_LLVM_NAME(cg, "list_reserve_cur"));
    LLVMValueRef cap =
        LLVMConstInt(cg->type_i64, ((uint64_t)desired << 1) | 1u, false);
    LLVMValueRef out =
        LLVMBuildCall2(cg->builder, reserve_sig->type, reserve_sig->value,
                       (LLVMValueRef[]){cur, cap}, 2,
                       NY_LLVM_NAME(cg, "list_reserve"));
    ny_store(cg, b->value, out);
  }
}

typedef struct stmt_list_sum_loop_t {
  const char *acc_name;
  const char *index_name;
  const char *list_name;
  binding *acc_binding;
  binding *index_binding;
  binding *list_binding;
  bool requires_int_list_proof;
} stmt_list_sum_loop_t;

static bool stmt_expr_is_len_of_ident(codegen_t *cg, scope *scopes,
                                      size_t depth, expr_t *e,
                                      const char **name_out) {
  if (name_out)
    *name_out = NULL;
  if (!e)
    return false;
  expr_t *target = NULL;
  if (e->kind == NY_E_MEMCALL && e->as.memcall.name &&
      ny_name_tail_is(e->as.memcall.name, "len") &&
      e->as.memcall.args.len == 0) {
    target = e->as.memcall.target;
  } else if (e->kind == NY_E_MEMBER && e->as.member.name &&
             ny_name_tail_is(e->as.member.name, "len")) {
    target = e->as.member.target;
  } else if (e->kind == NY_E_CALL && e->as.call.callee &&
             e->as.call.callee->kind == NY_E_IDENT &&
             e->as.call.callee->as.ident.name &&
             e->as.call.args.len == 1) {
    if (stmt_call_builtin_name_shadowed(cg, scopes, depth, e->as.call.callee) ||
        !ny_name_tail_is(e->as.call.callee->as.ident.name, "len"))
      return false;
    target = e->as.call.args.data[0].val;
  }
  if (!target || target->kind != NY_E_IDENT || !target->as.ident.name)
    return false;
  if (name_out)
    *name_out = target->as.ident.name;
  return true;
}

static bool stmt_expr_is_index_of_ident(expr_t *e, const char *list_name,
                                        const char *index_name) {
  if (!e || e->kind != NY_E_INDEX || !list_name || !index_name)
    return false;
  if (e->as.index.stop || e->as.index.step || !e->as.index.target ||
      !e->as.index.start)
    return false;
  expr_t *target = e->as.index.target;
  expr_t *idx = e->as.index.start;
  return target->kind == NY_E_IDENT && target->as.ident.name &&
         strcmp(target->as.ident.name, list_name) == 0 &&
         idx->kind == NY_E_IDENT && idx->as.ident.name &&
         strcmp(idx->as.ident.name, index_name) == 0;
}

static bool stmt_expr_is_int_list_item(codegen_t *cg, scope *scopes,
                                       size_t depth, expr_t *e,
                                       const char *list_name,
                                       const char *index_name,
                                       bool *requires_int_list_proof) {
  if (requires_int_list_proof)
    *requires_int_list_proof = false;
  if (!e)
    return false;
  if (stmt_expr_is_index_of_ident(e, list_name, index_name)) {
    if (requires_int_list_proof)
      *requires_int_list_proof = true;
    return true;
  }
  if (e->kind != NY_E_CALL || !e->as.call.callee ||
      e->as.call.callee->kind != NY_E_IDENT ||
      !e->as.call.callee->as.ident.name || e->as.call.args.len != 1)
    return false;
  const char *name = e->as.call.callee->as.ident.name;
  if ((strcmp(name, "int") != 0 && !ny_name_tail_is(name, "to_int")) ||
      stmt_call_builtin_name_shadowed(cg, scopes, depth, e->as.call.callee))
    return false;
  return stmt_expr_is_index_of_ident(e->as.call.args.data[0].val, list_name,
                                     index_name);
}

static bool stmt_expr_is_sum_update(codegen_t *cg, scope *scopes, size_t depth,
                                    expr_t *e, const char *acc_name,
                                    const char *list_name,
                                    const char *index_name,
                                    bool *requires_int_list_proof) {
  if (requires_int_list_proof)
    *requires_int_list_proof = false;
  if (!e || e->kind != NY_E_BINARY || !e->as.binary.op ||
      strcmp(e->as.binary.op, "+") != 0)
    return false;
  expr_t *lhs = e->as.binary.left;
  expr_t *rhs = e->as.binary.right;
  bool lhs_acc = lhs && lhs->kind == NY_E_IDENT && lhs->as.ident.name &&
                 strcmp(lhs->as.ident.name, acc_name) == 0;
  bool rhs_acc = rhs && rhs->kind == NY_E_IDENT && rhs->as.ident.name &&
                 strcmp(rhs->as.ident.name, acc_name) == 0;
  bool item_needs_int_list = false;
  bool ok = false;
  if (lhs_acc)
    ok = stmt_expr_is_int_list_item(cg, scopes, depth, rhs, list_name,
                                    index_name, &item_needs_int_list);
  if (!ok && rhs_acc)
    ok = stmt_expr_is_int_list_item(cg, scopes, depth, lhs, list_name,
                                    index_name, &item_needs_int_list);
  if (ok && requires_int_list_proof)
    *requires_int_list_proof = item_needs_int_list;
  return ok;
}

static bool stmt_is_simple_step_assignment(stmt_t *s, const char *index_name) {
  if (!s || !index_name || s->kind != NY_S_VAR || s->as.var.is_decl ||
      s->as.var.is_del || s->as.var.is_destructure ||
      s->as.var.names.len != 1 || s->as.var.exprs.len != 1)
    return false;
  const char *name = s->as.var.names.data[0];
  if (!name || strcmp(name, index_name) != 0)
    return false;
  int64_t delta = 0;
  return stmt_expr_is_ident_plus_const(s->as.var.exprs.data[0], index_name,
                                       &delta) &&
         delta == 1;
}

static bool stmt_is_simple_sum_assignment(codegen_t *cg, scope *scopes,
                                          size_t depth, stmt_t *s,
                                          const char *list_name,
                                          const char *index_name,
                                          const char **acc_name_out,
                                          bool *requires_int_list_proof) {
  if (acc_name_out)
    *acc_name_out = NULL;
  if (requires_int_list_proof)
    *requires_int_list_proof = false;
  if (!s || s->kind != NY_S_VAR || s->as.var.is_decl || s->as.var.is_del ||
      s->as.var.is_destructure || s->as.var.names.len != 1 ||
      s->as.var.exprs.len != 1)
    return false;
  const char *acc_name = s->as.var.names.data[0];
  if (!acc_name || strcmp(acc_name, index_name) == 0 ||
      strcmp(acc_name, list_name) == 0)
    return false;
  if (!stmt_expr_is_sum_update(cg, scopes, depth, s->as.var.exprs.data[0],
                               acc_name, list_name, index_name,
                               requires_int_list_proof))
    return false;
  if (acc_name_out)
    *acc_name_out = acc_name;
  return true;
}

static bool stmt_extract_list_sum_body(codegen_t *cg, scope *scopes,
                                       size_t depth, stmt_t *body,
                                       stmt_t *update,
                                       const char *list_name,
                                       const char *index_name,
                                       const char **acc_name_out,
                                       bool *requires_int_list_proof) {
  if (requires_int_list_proof)
    *requires_int_list_proof = false;
  if (!body || !list_name || !index_name)
    return false;
  if (update) {
    if (!stmt_is_simple_step_assignment(update, index_name))
      return false;
    if (body->kind == NY_S_BLOCK) {
      if (body->as.block.body.len != 1)
        return false;
      body = body->as.block.body.data[0];
    }
    return stmt_is_simple_sum_assignment(cg, scopes, depth, body, list_name,
                                         index_name, acc_name_out,
                                         requires_int_list_proof);
  }

  if (body->kind != NY_S_BLOCK || body->as.block.body.len != 2)
    return false;
  stmt_t *a = body->as.block.body.data[0];
  stmt_t *b = body->as.block.body.data[1];
  const char *acc_name = NULL;
  bool needs_int_list = false;
  if (stmt_is_simple_step_assignment(a, index_name) &&
      stmt_is_simple_sum_assignment(cg, scopes, depth, b, list_name, index_name,
                                    &acc_name, &needs_int_list)) {
    if (acc_name_out)
      *acc_name_out = acc_name;
    if (requires_int_list_proof)
      *requires_int_list_proof = needs_int_list;
    return true;
  }
  if (stmt_is_simple_sum_assignment(cg, scopes, depth, a, list_name, index_name,
                                    &acc_name, &needs_int_list) &&
      stmt_is_simple_step_assignment(b, index_name)) {
    if (acc_name_out)
      *acc_name_out = acc_name;
    if (requires_int_list_proof)
      *requires_int_list_proof = needs_int_list;
    return true;
  }
  return false;
}

static bool stmt_match_list_sum_loop(codegen_t *cg, scope *scopes,
                                     size_t depth, stmt_t *s,
                                     stmt_list_sum_loop_t *out) {
  if (out)
    memset(out, 0, sizeof(*out));
  if (!cg || !s || s->kind != NY_S_WHILE || !s->as.whl.test ||
      stmt_loop_body_has_control_exit(s->as.whl.body))
    return false;
  expr_t *test = s->as.whl.test;
  if (test->kind != NY_E_BINARY || !test->as.binary.op ||
      strcmp(test->as.binary.op, "<") != 0)
    return false;
  expr_t *lhs = test->as.binary.left;
  expr_t *rhs = test->as.binary.right;
  if (!lhs || lhs->kind != NY_E_IDENT || !lhs->as.ident.name || !rhs)
    return false;
  const char *list_name = NULL;
  if (!stmt_expr_is_len_of_ident(cg, scopes, depth, rhs, &list_name) ||
      !list_name)
    return false;

  int64_t idx_min = 0, idx_max = 0;
  if (!stmt_expr_int_range(cg, scopes, depth, lhs, &idx_min, &idx_max) ||
      idx_min < 0)
    return false;

  const char *acc_name = NULL;
  bool requires_int_list_proof = false;
  if (!stmt_extract_list_sum_body(cg, scopes, depth, s->as.whl.body,
                                  s->as.whl.update, list_name,
                                  lhs->as.ident.name, &acc_name,
                                  &requires_int_list_proof) ||
      !acc_name)
    return false;

  if (strcmp(acc_name, lhs->as.ident.name) == 0 ||
      strcmp(acc_name, list_name) == 0 ||
      strcmp(lhs->as.ident.name, list_name) == 0)
    return false;

  binding *list_b =
      stmt_lookup_binding(cg, scopes, depth, list_name, strlen(list_name), 0);
  binding *idx_b = stmt_lookup_binding(cg, scopes, depth, lhs->as.ident.name,
                                       strlen(lhs->as.ident.name), 0);
  binding *acc_b =
      stmt_lookup_binding(cg, scopes, depth, acc_name, strlen(acc_name), 0);
  const char *list_type =
      list_b ? (list_b->decl_type_name ? list_b->decl_type_name
                                       : list_b->type_name)
             : NULL;
  bool list_ok =
      list_b && (list_b->is_int_list_storage ||
                 (!requires_int_list_proof &&
                  (list_b->is_list_storage || ny_type_is(list_type, "list") ||
                   ny_type_is(list_type, "tuple"))));
  if (!list_b || !idx_b || !acc_b || !idx_b->is_slot || !acc_b->is_slot ||
      !list_b->value || !idx_b->value || !acc_b->value || !list_ok ||
      !idx_b->is_int_slot ||
      !acc_b->is_int_slot)
    return false;

  if (out) {
    out->acc_name = acc_name;
    out->index_name = lhs->as.ident.name;
    out->list_name = list_name;
    out->acc_binding = acc_b;
    out->index_binding = idx_b;
    out->list_binding = list_b;
    out->requires_int_list_proof = requires_int_list_proof;
  }
  return true;
}

static bool stmt_try_emit_list_sum_loop(codegen_t *cg, scope *scopes,
                                        size_t depth, stmt_t *s) {
  if (!ny_env_enabled_default_on("NYTRIX_FAST_LIST_SUM"))
    return false;
  stmt_list_sum_loop_t loop = {0};
  if (!stmt_match_list_sum_loop(cg, scopes, depth, s, &loop))
    return false;
  fun_sig *sum_sig = lookup_fun(cg, "__list_sum_int_range", 0);
  if (!sum_sig || !sum_sig->type || !sum_sig->value)
    return false;

  ny_dbg_loc(cg, s->tok);
  LLVMValueRef list_v =
      loop.list_binding->is_slot
          ? ny_load(cg, loop.list_binding->value,
                    NY_LLVM_NAME(cg, "list_sum_list"))
          : loop.list_binding->value;
  if (LLVMTypeOf(list_v) != cg->type_i64)
    list_v = ny_ptr2i64(cg, list_v, "list_sum_list_i64");
  LLVMValueRef idx_v =
      ny_load(cg, loop.index_binding->value, NY_LLVM_NAME(cg, "list_sum_idx"));
  LLVMValueRef idx_raw =
      loop.index_binding->raw_int_value
          ? ny_load(cg, loop.index_binding->raw_int_value,
                    NY_LLVM_NAME(cg, "list_sum_idx_raw"))
          : ny_untag_int(cg, idx_v);
  LLVMValueRef list_ptr =
      LLVMBuildIntToPtr(cg->builder, list_v, ny_ptr_i64_ty(cg),
                        NY_LLVM_NAME(cg, "list_sum_ptr"));
  LLVMValueRef len_v =
      ny_load(cg, list_ptr, NY_LLVM_NAME(cg, "list_sum_len"));
  LLVMValueRef len_raw = ny_untag_int(cg, len_v);
  LLVMValueRef should_run =
      ny_slt(cg, idx_raw, len_raw, NY_LLVM_NAME(cg, "list_sum_has_items"));

  LLVMValueRef fn = ny_cur_fn(cg);
  LLVMBasicBlockRef sum_bb = ny_bb_fn(fn, "list.sum");
  LLVMBasicBlockRef done_bb = ny_bb_fn(fn, "list.sum.done");
  ny_cond_br(cg, should_run, sum_bb, done_bb);

  ny_pos(cg, sum_bb);
  LLVMValueRef sum_v =
      LLVMBuildCall2(cg->builder, sum_sig->type, sum_sig->value,
                     (LLVMValueRef[]){list_v, idx_v, len_v}, 3,
                     NY_LLVM_NAME(cg, "list_sum_range"));
  LLVMValueRef acc_v =
      ny_load(cg, loop.acc_binding->value, NY_LLVM_NAME(cg, "list_sum_acc"));
  LLVMValueRef acc_raw =
      loop.acc_binding->raw_int_value
          ? ny_load(cg, loop.acc_binding->raw_int_value,
                    NY_LLVM_NAME(cg, "list_sum_acc_raw"))
          : ny_untag_int(cg, acc_v);
  LLVMValueRef new_raw =
      ny_add(cg, acc_raw, ny_untag_int(cg, sum_v),
             NY_LLVM_NAME(cg, "list_sum_acc_new_raw"));
  LLVMValueRef new_acc = ny_tag_int(cg, new_raw);
  ny_store(cg, loop.acc_binding->value, new_acc);
  if (loop.acc_binding->raw_int_value)
    ny_store(cg, loop.acc_binding->raw_int_value, new_raw);
  ny_store(cg, loop.index_binding->value, len_v);
  if (loop.index_binding->raw_int_value)
    ny_store(cg, loop.index_binding->raw_int_value, len_raw);
  stmt_update_int_binding_range(loop.acc_binding, false, 0, 0);
  stmt_update_int_binding_range(loop.index_binding, false, 0, 0);
  ny_br(cg, done_bb);

  ny_pos(cg, done_bb);
  return true;
}

static void stmt_try_widen_loop_accumulator_binding(codegen_t *cg,
                                                    scope *scopes, size_t depth,
                                                    const char *name,
                                                    expr_t *expr,
                                                    int64_t loop_trip_hint) {
  if (!cg || !scopes || !name || !expr || loop_trip_hint <= 0 ||
      expr->kind != NY_E_BINARY || !expr->as.binary.op ||
      (strcmp(expr->as.binary.op, "+") != 0 &&
       strcmp(expr->as.binary.op, "-") != 0))
    return;
  bool is_add = strcmp(expr->as.binary.op, "+") == 0;
  expr_t *lhs = expr->as.binary.left;
  expr_t *rhs = expr->as.binary.right;
  if (!lhs || lhs->kind != NY_E_IDENT || !lhs->as.ident.name ||
      strcmp(lhs->as.ident.name, name) != 0)
    return;

  size_t name_len = strlen(name);
  binding *b = stmt_lookup_binding(cg, scopes, depth, name, name_len, 0);
  if (!b || !(b->is_int_slot || b->is_int_direct) || !b->has_int_range)
    return;

  int64_t rhs_min = 0, rhs_max = 0;
  if (!stmt_expr_int_range(cg, scopes, depth, rhs, &rhs_min, &rhs_max))
    return;

  int64_t span_min = 0, span_max = 0, new_min = 0, new_max = 0;
  if (!ny_mul_range_ok(rhs_min, loop_trip_hint, &span_min) ||
      !ny_mul_range_ok(rhs_max, loop_trip_hint, &span_max))
    return;
  if (is_add) {
    if (!ny_add_range_ok(b->int_min_raw, span_min, &new_min) ||
        !ny_add_range_ok(b->int_max_raw, span_max, &new_max))
      return;
  } else {
    if (!ny_sub_range_ok(b->int_min_raw, span_max, &new_min) ||
        !ny_sub_range_ok(b->int_max_raw, span_min, &new_max))
      return;
  }

  stmt_update_int_binding_range(b, true, new_min, new_max);
}

static void match_check_exhaustive(codegen_t *cg, stmt_t *s) {
  if (!cg || !s)
    return;
  if (s->as.match.default_conseq)
    return;
  bool has_wild = false;
  bool has_other = false;
  bool has_ok = false;
  bool has_err = false;
  bool has_true = false;
  bool has_false = false;
  enum_def_t *enum_owner = NULL;
  VEC(const char *) matched;
  vec_init(&matched);
  for (size_t i = 0; i < s->as.match.arms.len; ++i) {
    match_arm_t *arm = &s->as.match.arms.data[i];
    bool arm_guarded = arm->guard != NULL;
    for (size_t j = 0; j < arm->patterns.len; ++j) {
      expr_t *pat = arm->patterns.data[j];
      if (ny_expr_is_wildcard_ident(pat)) {
        if (!arm_guarded)
          has_wild = true;
        continue;
      }
      if (match_pattern_is_okerr(pat, arm_guarded ? NULL : &has_ok,
                                 arm_guarded ? NULL : &has_err))
        continue;
      if (pat && pat->kind == NY_E_LITERAL &&
          pat->as.literal.kind == NY_LIT_BOOL) {
        if (!arm_guarded) {
          if (pat->as.literal.as.b)
            has_true = true;
          else
            has_false = true;
        }
        continue;
      }
      enum_def_t *owner = NULL;
      enum_member_def_t *mem = match_pattern_enum_member(cg, pat, &owner);
      if (mem && owner) {
        if (!enum_owner)
          enum_owner = owner;
        else if (enum_owner != owner) {
          enum_owner = NULL;
          has_other = true;
        }
        if (arm_guarded)
          continue;
        bool seen = false;
        for (size_t k = 0; k < matched.len; k++) {
          if (strcmp(matched.data[k], mem->name) == 0) {
            seen = true;
            ny_diag_warning(pat->tok, "duplicate match pattern '%s'",
                            mem->name);
            break;
          }
        }
        if (!seen)
          vec_push(&matched, mem->name);
        continue;
      }
      has_other = true;
    }
  }
  if (has_wild) {
    vec_free(&matched);
    return;
  }
  if (!has_other && (has_ok || has_err)) {
    if (!has_ok || !has_err) {
      ny_diag_error(s->tok, "non-exhaustive match for Result (missing %s arm)",
                    has_ok ? "err(...)" : "ok(...)");
      cg->had_error = 1;
    }
    vec_free(&matched);
    return;
  }
  if (!has_other && !enum_owner && !has_ok && !has_err &&
      (has_true || has_false)) {
    if (!(has_true && has_false)) {
      ny_diag_error(s->tok, "non-exhaustive match for bool (missing %s)",
                    has_true ? "false" : "true");
      cg->had_error = 1;
    }
    vec_free(&matched);
    return;
  }
  if (enum_owner && !has_other && !has_ok && !has_err) {
    size_t missing = 0;
    char buf[256];
    buf[0] = '\0';
    size_t buf_len = 0;
    for (size_t i = 0; i < enum_owner->members.len; i++) {
      const char *name = enum_owner->members.data[i].name;
      bool found = false;
      for (size_t j = 0; j < matched.len; j++) {
        if (strcmp(matched.data[j], name) == 0) {
          found = true;
          break;
        }
      }
      if (!found) {
        missing++;
        size_t name_len = strlen(name);
        if (buf_len + name_len + 2 < sizeof(buf)) {
          if (buf_len)
            buf[buf_len++] = ',';
          if (buf_len)
            buf[buf_len++] = ' ';
          memcpy(buf + buf_len, name, name_len + 1);
          buf_len += name_len;
        }
      }
    }
    if (missing > 0) {
      if (buf_len > 0) {
        ny_diag_error(s->tok,
                      "non-exhaustive match for enum '%s' (missing: %s)",
                      enum_owner->name, buf);
      } else {
        ny_diag_error(s->tok, "non-exhaustive match for enum '%s'",
                      enum_owner->name);
      }
      cg->had_error = 1;
    }
  }
  vec_free(&matched);
}

static void gen_stmt_with_null_narrow(codegen_t *cg, scope *scopes,
                                      size_t *depth,
                                      const ny_null_narrow_list_t *narrow,
                                      bool true_branch, stmt_t *body,
                                      size_t func_root, bool is_tail) {
  if (!body)
    return;
  ny_null_narrow_restore_list_t applied;
  ny_null_narrow_apply(cg, scopes, *depth, narrow, true_branch, &applied);
  gen_stmt(cg, scopes, depth, body, func_root, is_tail);
  ny_null_narrow_restore(&applied);
}

static bool stmt_type_is_native_float(const char *type_name) {
  return ny_type_is(type_name, "f32") || ny_type_is(type_name, "f64") ||
         ny_type_is(type_name, "f128");
}

static bool stmt_type_is_native_abi_value(codegen_t *cg,
                                          const char *type_name) {
  if (!type_name || !*type_name)
    return false;
  if (ny_type_is(type_name, "fnptr"))
    return false;
  if (ny_is_native_abi_type_name(type_name) && !ny_type_is_tagged(type_name))
    return true;
  while (*type_name == '?')
    type_name++;
  if (!*type_name || *type_name == '*')
    return false;
  layout_def_t *layout = lookup_layout(cg, type_name);
  return layout && layout->llvm_type;
}

typedef struct {
  binding *binding;
  bool active;
  int64_t then_min;
  int64_t then_max;
  int64_t else_min;
  int64_t else_max;
  stmt_binding_int_snapshot_t snapshot;
} stmt_int_branch_narrow_t;

static bool stmt_int_bound_add1(int64_t v, int64_t *out) {
  if (v == INT64_MAX)
    return false;
  if (out)
    *out = v + 1;
  return true;
}

static bool stmt_int_bound_sub1(int64_t v, int64_t *out) {
  if (v == INT64_MIN)
    return false;
  if (out)
    *out = v - 1;
  return true;
}

static bool stmt_int_branch_narrow_from_test(codegen_t *cg, scope *scopes,
                                             size_t depth, expr_t *test,
                                             stmt_int_branch_narrow_t *out) {
  if (!cg || !scopes || !test || !out || test->kind != NY_E_BINARY ||
      !test->as.binary.op)
    return false;
  memset(out, 0, sizeof(*out));
  expr_t *ident = NULL;
  expr_t *lit_expr = NULL;
  const char *op = test->as.binary.op;
  bool swapped = false;
  if (test->as.binary.left && test->as.binary.left->kind == NY_E_IDENT) {
    ident = test->as.binary.left;
    lit_expr = test->as.binary.right;
  } else if (test->as.binary.right &&
             test->as.binary.right->kind == NY_E_IDENT) {
    ident = test->as.binary.right;
    lit_expr = test->as.binary.left;
    swapped = true;
  } else {
    return false;
  }
  int64_t c = 0;
  if (!ny_expr_literal_i64(lit_expr, &c) || !ident || !ident->as.ident.name)
    return false;
  if (swapped) {
    if (strcmp(op, ">") == 0)
      op = "<";
    else if (strcmp(op, ">=") == 0)
      op = "<=";
    else if (strcmp(op, "<") == 0)
      op = ">";
    else if (strcmp(op, "<=") == 0)
      op = ">=";
  }
  size_t name_len = (size_t)ident->tok.len;
  if (name_len == 0)
    name_len = strlen(ident->as.ident.name);
  binding *b = stmt_lookup_binding(cg, scopes, depth, ident->as.ident.name,
                                   name_len, ident->as.ident.hash);
  if (!b || !ny_is_proven_int(cg, scopes, depth, ident, NULL))
    return false;
  if (b->is_slot)
    b->is_int_slot = true;
  else
    b->is_int_direct = true;

  const int64_t small_min = INT64_C(-4611686018427387904);
  const int64_t small_max = INT64_C(4611686018427387903);
  int64_t base_min = b->has_int_range ? b->int_min_raw : small_min;
  int64_t base_max = b->has_int_range ? b->int_max_raw : small_max;
  int64_t then_min = base_min, then_max = base_max;
  int64_t else_min = base_min, else_max = base_max;
  if (strcmp(op, ">") == 0) {
    int64_t lo = 0;
    if (!stmt_int_bound_add1(c, &lo))
      return false;
    if (then_min < lo)
      then_min = lo;
    if (else_max > c)
      else_max = c;
  } else if (strcmp(op, ">=") == 0) {
    if (then_min < c)
      then_min = c;
    int64_t hi = 0;
    if (!stmt_int_bound_sub1(c, &hi))
      return false;
    if (else_max > hi)
      else_max = hi;
  } else if (strcmp(op, "<") == 0) {
    int64_t hi = 0;
    if (!stmt_int_bound_sub1(c, &hi))
      return false;
    if (then_max > hi)
      then_max = hi;
    if (else_min < c)
      else_min = c;
  } else if (strcmp(op, "<=") == 0) {
    if (then_max > c)
      then_max = c;
    int64_t lo = 0;
    if (!stmt_int_bound_add1(c, &lo))
      return false;
    if (else_min < lo)
      else_min = lo;
  } else {
    return false;
  }
  if (then_min > then_max && else_min > else_max)
    return false;
  out->binding = b;
  out->active = true;
  out->then_min = then_min;
  out->then_max = then_max;
  out->else_min = else_min;
  out->else_max = else_max;
  out->snapshot = stmt_snapshot_binding_int_proof(b);
  return true;
}

static void stmt_apply_int_branch_range(stmt_int_branch_narrow_t *narrow,
                                        bool then_side) {
  if (!narrow || !narrow->active || !narrow->binding)
    return;
  int64_t lo = then_side ? narrow->then_min : narrow->else_min;
  int64_t hi = then_side ? narrow->then_max : narrow->else_max;
  if (lo <= hi)
    stmt_update_int_binding_range(narrow->binding, true, lo, hi);
}

static bool stmt_expr_is_native_float_tail_safe(expr_t *e) {
  if (!e)
    return false;
  switch (e->kind) {
  case NY_E_LITERAL:
  case NY_E_IDENT:
    return true;
  case NY_E_BINARY:
    if (!e->as.binary.op)
      return false;
    if (strcmp(e->as.binary.op, "+") != 0 &&
        strcmp(e->as.binary.op, "-") != 0 &&
        strcmp(e->as.binary.op, "*") != 0 && strcmp(e->as.binary.op, "/") != 0)
      return false;
    return stmt_expr_is_native_float_tail_safe(e->as.binary.left) &&
           stmt_expr_is_native_float_tail_safe(e->as.binary.right);
  default:
    return false;
  }
}

static LLVMValueRef stmt_gen_return_value(codegen_t *cg, scope *scopes,
                                          size_t depth, expr_t *e,
                                          const char *ret_type) {
  if (!e)
    return ny_c1(cg);
  if (ret_type && stmt_type_is_native_float(ret_type) &&
      !ny_type_is_tagged(ret_type)) {
    LLVMValueRef fv = gen_expr_as_f64(cg, scopes, depth, e);
    if (ny_type_is(ret_type, "f32"))
      return LLVMBuildFPTrunc(cg->builder, fv, cg->type_f32, "ret_f32");
    if (ny_type_is(ret_type, "f128"))
      return LLVMBuildFPExt(cg->builder, fv, cg->type_f128, "ret_f128");
    return fv;
  }
  LLVMValueRef v = gen_expr(cg, scopes, depth, e);
  if (stmt_type_is_native_abi_value(cg, ret_type)) {
    bool proven_int = ny_is_proven_int(cg, scopes, depth, e, v);
    v = ny_coerce_to_abi_proven_int(cg, v, ret_type, proven_int);
  }
  return v;
}

static bool stmt_should_mark_tail_call_expr(codegen_t *cg, expr_t *e) {
  return cg && e && e->kind == NY_E_CALL &&
         (cg->current_fn_attr_tailcall || cg->opt_tail_call);
}

static bool stmt_expr_is_noreturn_call(codegen_t *cg, scope *scopes,
                                       size_t depth, expr_t *e) {
  if (!e || e->kind != NY_E_CALL || !e->as.call.callee ||
      e->as.call.callee->kind != NY_E_IDENT)
    return false;
  const char *name = e->as.call.callee->as.ident.name;
  if (!name)
    return false;
  if (strcmp(name, "__panic") == 0)
    return true;
  return ny_name_tail_is(name, "panic") &&
         !stmt_call_builtin_name_shadowed(cg, scopes, depth, e->as.call.callee);
}

static LLVMValueRef stmt_gen_expr_with_tail_call_hint(codegen_t *cg,
                                                      scope *scopes,
                                                      size_t depth, expr_t *e) {
  bool mark = stmt_should_mark_tail_call_expr(cg, e);
  if (mark)
    cg->tail_call_depth++;
  LLVMValueRef v = gen_expr(cg, scopes, depth, e);
  if (mark && cg->tail_call_depth > 0)
    cg->tail_call_depth--;
  return v;
}

static void gen_stmt_if(codegen_t *cg, scope *scopes, size_t *depth, stmt_t *s,
                        size_t func_root, bool is_tail) {
  if (s->as.iff.init) {
    scope_enter(scopes, depth, NULL, NULL);
    gen_stmt(cg, scopes, depth, s->as.iff.init, func_root, false);
  }
  ny_null_narrow_list_t narrow;
  vec_init(&narrow);
  (void)ny_null_narrow_collect(s->as.iff.test, &narrow);
  LLVMValueRef c = stmt_gen_cond_i1(cg, scopes, *depth, s->as.iff.test);
  bool then_tail = is_tail;
  bool else_tail = is_tail;
  if (LLVMIsAConstantInt(c)) {
    bool truthy = LLVMConstIntGetZExtValue(c) != 0;
    if (truthy) {
      gen_stmt_with_null_narrow(cg, scopes, depth, &narrow, true,
                                s->as.iff.conseq, func_root, then_tail);
      if (!s->as.iff.init && !LLVMGetBasicBlockTerminator(ny_cur_block(cg)))
        ny_null_narrow_apply_persistent(cg, scopes, *depth, &narrow, true);
    } else if (s->as.iff.alt) {
      gen_stmt_with_null_narrow(cg, scopes, depth, &narrow, false,
                                s->as.iff.alt, func_root, else_tail);
      if (!s->as.iff.init && !LLVMGetBasicBlockTerminator(ny_cur_block(cg)))
        ny_null_narrow_apply_persistent(cg, scopes, *depth, &narrow, false);
    } else if (!s->as.iff.init &&
               !LLVMGetBasicBlockTerminator(ny_cur_block(cg))) {
      ny_null_narrow_apply_persistent(cg, scopes, *depth, &narrow, false);
    }
    vec_free(&narrow);
    if (s->as.iff.init)
      scope_pop(scopes, depth);
    return;
  }
  LLVMValueRef f = ny_cur_fn(cg);
  LLVMBasicBlockRef tb = ny_bb_fn(f, "it"),
                    eb = s->as.iff.alt ? ny_bb_fn(f, "ie") : NULL,
                    next = ny_bb_fn(f, "in");
  LLVMBasicBlockRef then_end = NULL;
  LLVMBasicBlockRef else_end = NULL;
  bool then_fallthrough = false;
  bool else_fallthrough = false;
  ny_dbg_loc(cg, s->tok);
  ny_cond_br(cg, c, tb, eb ? eb : next);

  stmt_int_branch_narrow_t int_narrow = {0};
  bool has_int_narrow = stmt_int_branch_narrow_from_test(
      cg, scopes, *depth, s->as.iff.test, &int_narrow);

  ny_pos(cg, tb);

  if (has_int_narrow)
    stmt_apply_int_branch_range(&int_narrow, true);
  gen_stmt_with_null_narrow(cg, scopes, depth, &narrow, true, s->as.iff.conseq,
                            func_root, then_tail);
  then_end = ny_cur_block(cg);
  then_fallthrough = !LLVMGetBasicBlockTerminator(then_end);
  if (then_fallthrough)
    ny_br(cg, next);
  if (has_int_narrow)
    stmt_restore_binding_int_proof(int_narrow.snapshot);
  if (eb) {
    ny_pos(cg, eb);

    if (has_int_narrow)
      stmt_apply_int_branch_range(&int_narrow, false);
    gen_stmt_with_null_narrow(cg, scopes, depth, &narrow, false, s->as.iff.alt,
                              func_root, else_tail);
    else_end = ny_cur_block(cg);
    else_fallthrough = !LLVMGetBasicBlockTerminator(else_end);
    if (else_fallthrough)
      ny_br(cg, next);
    if (has_int_narrow)
      stmt_restore_binding_int_proof(int_narrow.snapshot);
  }
  ny_pos(cg, next);

  bool false_fallthrough = eb ? else_fallthrough : true;
  if (!then_fallthrough && !false_fallthrough && !ny_has_terminator(cg))
    LLVMBuildUnreachable(cg->builder);
  if (!s->as.iff.init && then_fallthrough != false_fallthrough) {
    ny_null_narrow_apply_persistent(cg, scopes, *depth, &narrow,
                                    then_fallthrough);
  }

  vec_free(&narrow);
  if (s->as.iff.init)
    scope_pop(scopes, depth);
}

static char *stmt_guard_from_name(codegen_t *cg, const char *type_name) {
  if (!type_name)
    type_name = "";
  while (*type_name == '?' || *type_name == '*')
    type_name++;
  size_t len = strlen(type_name);
  char *out = arena_alloc(cg ? cg->arena : NULL, len + 6);
  if (!out) {
    fprintf(stderr, "oom\n");
    exit(1);
  }
  memcpy(out, type_name, len);
  memcpy(out + len, "_from", 6);
  return out;
}

static const char *stmt_guard_binding_type(codegen_t *cg,
                                           const char *type_name) {
  if (!type_name || !*type_name)
    return "ptr";
  while (*type_name == '?')
    type_name++;
  if (type_name[0] == '*')
    return type_name;
  size_t len = strlen(type_name);
  char *out = arena_alloc(cg ? cg->arena : NULL, len + 2);
  out[0] = '*';
  memcpy(out + 1, type_name, len + 1);
  return out;
}

static void gen_stmt_guard(codegen_t *cg, scope *scopes, size_t *depth,
                           stmt_t *s, size_t func_root) {
  char *from_name = stmt_guard_from_name(cg, s->as.guard.type_name);
  expr_t *callee = arena_alloc(cg ? cg->arena : NULL, sizeof(*callee));
  callee->kind = NY_E_IDENT;
  callee->tok = s->tok;
  callee->tok.lexeme = from_name;
  callee->tok.len = (int)strlen(from_name);
  callee->as.ident.name = from_name;
  callee->as.ident.hash = ny_hash64_cstr(from_name);

  call_arg_t *arg_data = arena_alloc(cg ? cg->arena : NULL, sizeof(*arg_data));
  arg_data[0] = (call_arg_t){NULL, s->as.guard.value};
  expr_t *call = arena_alloc(cg ? cg->arena : NULL, sizeof(*call));
  call->kind = NY_E_CALL;
  call->tok = s->tok;
  call->as.call.callee = callee;
  call->as.call.args.data = arg_data;
  call->as.call.args.len = 1;
  call->as.call.args.cap = 1;

  const char **name_data = arena_alloc(cg ? cg->arena : NULL, sizeof(*name_data));
  name_data[0] = s->as.guard.name;
  const char *guard_type = stmt_guard_binding_type(cg, s->as.guard.type_name);
  const char **type_data = arena_alloc(cg ? cg->arena : NULL, sizeof(*type_data));
  type_data[0] = guard_type;
  expr_t **expr_data = arena_alloc(cg ? cg->arena : NULL, sizeof(*expr_data));
  expr_data[0] = call;
  stmt_t *decl = arena_alloc(cg ? cg->arena : NULL, sizeof(*decl));
  decl->kind = NY_S_VAR;
  decl->tok = s->tok;
  decl->as.var.names.data = (const char **)name_data;
  decl->as.var.names.len = 1;
  decl->as.var.names.cap = 1;
  decl->as.var.types.data = (const char **)type_data;
  decl->as.var.types.len = 1;
  decl->as.var.types.cap = 1;
  decl->as.var.exprs.data = expr_data;
  decl->as.var.exprs.len = 1;
  decl->as.var.exprs.cap = 1;
  decl->as.var.is_decl = true;
  gen_stmt(cg, scopes, depth, decl, func_root, false);

  expr_t *ident = arena_alloc(cg ? cg->arena : NULL, sizeof(*ident));
  ident->kind = NY_E_IDENT;
  ident->tok = s->tok;
  ident->tok.lexeme = s->as.guard.name;
  ident->tok.len = (int)strlen(s->as.guard.name);
  ident->as.ident.name = s->as.guard.name;
  ident->as.ident.hash = ny_hash64_cstr(s->as.guard.name);
  expr_t *not_ident = arena_alloc(cg ? cg->arena : NULL, sizeof(*not_ident));
  not_ident->kind = NY_E_UNARY;
  not_ident->tok = s->tok;
  not_ident->as.unary.op = "!";
  not_ident->as.unary.right = ident;
  stmt_t *iff = arena_alloc(cg ? cg->arena : NULL, sizeof(*iff));
  iff->kind = NY_S_IF;
  iff->tok = s->tok;
  iff->as.iff.test = not_ident;
  iff->as.iff.conseq = s->as.guard.fallback;
  gen_stmt_if(cg, scopes, depth, iff, func_root, false);
}

static void gen_stmt_block(codegen_t *cg, scope *scopes, size_t *depth,
                           stmt_t *s, size_t func_root, bool is_tail) {
  if (s->as.block.transparent) {
    for (size_t i = 0; i < s->as.block.body.len; i++) {
      if (cg->builder) {
        LLVMBasicBlockRef cur = ny_cur_block(cg);
        if (cur && LLVMGetBasicBlockTerminator(cur)) {
          stmt_t *unreach = s->as.block.body.data[i];
          if (unreach && unreach->kind == NY_S_LABEL) {
            gen_stmt(cg, scopes, depth, unreach, func_root, false);
            continue;
          }
          bool stdlib_quiet = ny_is_stdlib_tok(unreach->tok) &&
                              !ny_strict_error_enabled(cg, unreach->tok) &&
                              verbose_enabled < 2;
          if (!stdlib_quiet)
            ny_diag_warning(unreach->tok, "unreachable code");
          break;
        }
      }
      gen_stmt(cg, scopes, depth, s->as.block.body.data[i], func_root,
               is_tail && (i == s->as.block.body.len - 1));
    }
    return;
  }
  LLVMMetadataRef dbg_scope = codegen_debug_push_block(cg, s->tok);
  scope_enter(scopes, depth, scopes[*depth].break_bb,
              scopes[*depth].continue_bb);
  for (size_t i = 0; i < s->as.block.body.len; i++) {
    if (cg->builder) {
      LLVMBasicBlockRef cur = ny_cur_block(cg);
      if (cur && LLVMGetBasicBlockTerminator(cur)) {
        stmt_t *unreach = s->as.block.body.data[i];
        if (unreach && unreach->kind == NY_S_LABEL) {
          gen_stmt(cg, scopes, depth, unreach, func_root, false);
          continue;
        }
        bool stdlib_quiet = ny_is_stdlib_tok(unreach->tok) &&
                            !ny_strict_error_enabled(cg, unreach->tok) &&
                            verbose_enabled < 2;
        if (!stdlib_quiet)
          ny_diag_warning(unreach->tok, "unreachable code");
        break;
      }
    }
    gen_stmt(cg, scopes, depth, s->as.block.body.data[i], func_root,
             is_tail && (i == s->as.block.body.len - 1));
  }
  if (!ny_has_terminator(cg))
    emit_defers(cg, scopes, *depth, *depth);
  scope_pop(scopes, depth);
  codegen_debug_pop_block(cg, dbg_scope);
}

static bool stmt_auto_simd_expr_safe(codegen_t *cg, scope *scopes, size_t depth,
                                     expr_t *e);

static bool stmt_auto_simd_call_is_typed_buffer(const char *name, size_t argc) {
  if (!name)
    return false;
  bool is_load =
      ny_name_tail_is(name, "load8") || ny_name_tail_is(name, "load16") ||
      ny_name_tail_is(name, "load32") || ny_name_tail_is(name, "load64") ||
      ny_name_tail_is(name, "load32_h") || ny_name_tail_is(name, "load64_h") ||
      ny_name_tail_is(name, "load64_i") || strcmp(name, "__load8_idx") == 0 ||
      strcmp(name, "__load16_idx") == 0 || strcmp(name, "__load32_idx") == 0 ||
      strcmp(name, "__load64_idx") == 0 || strcmp(name, "__load32_h") == 0 ||
      strcmp(name, "__load64_h") == 0;
  bool is_store =
      ny_name_tail_is(name, "store8") || ny_name_tail_is(name, "store16") ||
      ny_name_tail_is(name, "store32") || ny_name_tail_is(name, "store64") ||
      ny_name_tail_is(name, "store32_h") ||
      ny_name_tail_is(name, "store64_h") ||
      ny_name_tail_is(name, "store64_i") || strcmp(name, "__store8_idx") == 0 ||
      strcmp(name, "__store16_idx") == 0 ||
      strcmp(name, "__store32_idx") == 0 ||
      strcmp(name, "__store64_idx") == 0 || strcmp(name, "__store64_h") == 0;
  return (is_load && argc >= 1 && argc <= 2) || (is_store && argc == 3);
}

static bool stmt_auto_simd_call_is_effect_free(codegen_t *cg,
                                               expr_call_t *call) {
  if (!cg || !call || !call->callee || call->callee->kind != NY_E_IDENT)
    return false;
  const char *name = call->callee->as.ident.name;
  if (!name)
    return false;
  if (ny_name_tail_is(name, "len") && call->args.len == 1)
    return true;
  if (stmt_auto_simd_call_is_typed_buffer(name, call->args.len))
    return true;
  uint64_t hash = call->callee->as.ident.hash;
  fun_sig *sig = resolve_overload(cg, name, call->args.len, hash);
  if (!sig)
    sig = lookup_fun(cg, name, hash);
  if (!sig)
    return false;
  if (sig->effects_known)
    return (sig->effects &
            (NY_FX_IO | NY_FX_ALLOC | NY_FX_FFI | NY_FX_THREAD)) == 0;
  return sig->is_pure;
}

static bool stmt_auto_simd_expr_safe(codegen_t *cg, scope *scopes, size_t depth,
                                     expr_t *e) {
  if (!e)
    return true;
  switch (e->kind) {
  case NY_E_LITERAL:
  case NY_E_IDENT:
  case NY_E_INFERRED_MEMBER:
    return true;
  case NY_E_UNARY:
    if (e->as.unary.op && (strcmp(e->as.unary.op, "async") == 0 ||
                           strcmp(e->as.unary.op, "await") == 0))
      return false;
    return stmt_auto_simd_expr_safe(cg, scopes, depth, e->as.unary.right);
  case NY_E_BINARY:
    return stmt_auto_simd_expr_safe(cg, scopes, depth, e->as.binary.left) &&
           stmt_auto_simd_expr_safe(cg, scopes, depth, e->as.binary.right);
  case NY_E_LOGICAL:
    return stmt_auto_simd_expr_safe(cg, scopes, depth, e->as.logical.left) &&
           stmt_auto_simd_expr_safe(cg, scopes, depth, e->as.logical.right);
  case NY_E_TERNARY:
    return stmt_auto_simd_expr_safe(cg, scopes, depth, e->as.ternary.cond) &&
           stmt_auto_simd_expr_safe(cg, scopes, depth,
                                    e->as.ternary.true_expr) &&
           stmt_auto_simd_expr_safe(cg, scopes, depth,
                                    e->as.ternary.false_expr);
  case NY_E_INDEX: {
    if (e->as.index.stop || e->as.index.step || !e->as.index.start ||
        !e->as.index.target || e->as.index.target->kind != NY_E_IDENT ||
        !e->as.index.target->as.ident.name)
      return false;
    expr_t *target = e->as.index.target;
    size_t name_len = (size_t)target->tok.len;
    if (name_len == 0)
      name_len = strlen(target->as.ident.name);
    binding *b = stmt_lookup_binding(cg, scopes, depth, target->as.ident.name,
                                     name_len, target->as.ident.hash);
    if (!b || (!b->is_int_list_storage && !b->raw_int_list_ptr &&
               !b->static_int_list_global))
      return false;
    return stmt_auto_simd_expr_safe(cg, scopes, depth, e->as.index.start);
  }
  case NY_E_CALL:
    if (!stmt_auto_simd_call_is_effect_free(cg, &e->as.call))
      return false;
    for (size_t i = 0; i < e->as.call.args.len; ++i) {
      if (!stmt_auto_simd_expr_safe(cg, scopes, depth,
                                    e->as.call.args.data[i].val))
        return false;
    }
    return true;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
  case NY_E_DICT:
    return false;
  default:
    return false;
  }
}

static bool stmt_auto_simd_body_safe(codegen_t *cg, scope *scopes, size_t depth,
                                     stmt_t *s) {
  if (!s)
    return true;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      if (!stmt_auto_simd_body_safe(cg, scopes, depth,
                                    s->as.block.body.data[i]))
        return false;
    }
    return true;
  case NY_S_VAR:
    if (s->as.var.is_del || s->as.var.is_destructure)
      return false;
    for (size_t i = 0; i < s->as.var.exprs.len; ++i) {
      if (!stmt_auto_simd_expr_safe(cg, scopes, depth, s->as.var.exprs.data[i]))
        return false;
    }
    return true;
  case NY_S_EXPR:
    return stmt_auto_simd_expr_safe(cg, scopes, depth, s->as.expr.expr);
  case NY_S_IF:
    return stmt_auto_simd_expr_safe(cg, scopes, depth, s->as.iff.test) &&
           stmt_auto_simd_body_safe(cg, scopes, depth, s->as.iff.init) &&
           stmt_auto_simd_body_safe(cg, scopes, depth, s->as.iff.conseq) &&
           stmt_auto_simd_body_safe(cg, scopes, depth, s->as.iff.alt);
  default:
    return false;
  }
}

static bool stmt_auto_simd_cond_shape(codegen_t *cg, scope *scopes,
                                      size_t depth, expr_t *cond,
                                      const char **index_name_out) {
  if (index_name_out)
    *index_name_out = NULL;
  if (!cond || cond->kind != NY_E_BINARY || !cond->as.binary.op)
    return false;
  const char *op = cond->as.binary.op;
  if (strcmp(op, "<") != 0 && strcmp(op, "<=") != 0)
    return false;
  expr_t *lhs = cond->as.binary.left;
  expr_t *rhs = cond->as.binary.right;
  if (!lhs || lhs->kind != NY_E_IDENT || !lhs->as.ident.name || !rhs)
    return false;
  if (!stmt_auto_simd_expr_safe(cg, scopes, depth, rhs))
    return false;
  int64_t lhs_min = 0, lhs_max = 0;
  if (stmt_expr_int_range(cg, scopes, depth, lhs, &lhs_min, &lhs_max) &&
      lhs_min < 0)
    return false;
  if (index_name_out)
    *index_name_out = lhs->as.ident.name;
  return true;
}

static bool stmt_auto_simd_while_shape(codegen_t *cg, scope *scopes,
                                       size_t depth, stmt_t *s) {
  const char *index_name = NULL;
  if (!s || s->kind != NY_S_WHILE ||
      !stmt_auto_simd_cond_shape(cg, scopes, depth, s->as.whl.test,
                                 &index_name))
    return false;
  int64_t step = 0;
  if (s->as.whl.update)
    (void)stmt_find_loop_step_in_stmt(s->as.whl.update, index_name, &step);
  if (step <= 0 &&
      !stmt_find_loop_step_in_stmt(s->as.whl.body, index_name, &step))
    return false;
  return step > 0 && !stmt_loop_body_has_control_exit(s->as.whl.body) &&
         stmt_auto_simd_body_safe(cg, scopes, depth, s->as.whl.body) &&
         stmt_auto_simd_body_safe(cg, scopes, depth, s->as.whl.update);
}

static bool stmt_auto_simd_for_shape(codegen_t *cg, scope *scopes, size_t depth,
                                     stmt_t *s) {
  if (!s || s->kind != NY_S_FOR || !s->as.fr.init || !s->as.fr.cond ||
      !s->as.fr.update)
    return false;
  if (!s->as.fr.init || s->as.fr.init->kind != NY_S_VAR ||
      s->as.fr.init->as.var.names.len != 1)
    return false;
  const char *init_name = s->as.fr.init->as.var.names.data[0];
  const char *cond_name = NULL;
  if (!init_name ||
      !stmt_auto_simd_cond_shape(cg, scopes, depth, s->as.fr.cond,
                                 &cond_name) ||
      !cond_name || strcmp(init_name, cond_name) != 0)
    return false;
  int64_t step = 0;
  if (!stmt_find_loop_step_in_stmt(s->as.fr.update, init_name, &step) ||
      step <= 0)
    return false;
  return !stmt_loop_body_has_control_exit(s->as.fr.body) &&
         stmt_auto_simd_body_safe(cg, scopes, depth, s->as.fr.body);
}

static bool stmt_auto_simd_loop_shape(codegen_t *cg, scope *scopes,
                                      size_t depth, stmt_t *s) {
  if (!ny_env_enabled_default_on("NYTRIX_AUTO_SIMD"))
    return false;
  if (!s)
    return false;
  if (s->kind == NY_S_WHILE)
    return stmt_auto_simd_while_shape(cg, scopes, depth, s);
  if (s->kind == NY_S_FOR)
    return stmt_auto_simd_for_shape(cg, scopes, depth, s);
  return false;
}

static void apply_loop_metadata(codegen_t *cg, LLVMValueRef branch,
                                bool attr_unroll, bool attr_nounroll,
                                bool attr_vectorize, bool inferred_vectorize) {
  (void)inferred_vectorize;
  if (attr_unroll)
    ny_loop_unroll_hint(cg, branch);
  else if (attr_nounroll)
    ny_loop_nounroll_hint(cg, branch);
  if (attr_vectorize || ny_env_enabled("NYTRIX_AUTO_VECTORIZE_LOOPS"))
    ny_loop_vectorize_hint(cg, branch);
}

static void gen_stmt_while(codegen_t *cg, scope *scopes, size_t *depth,
                           stmt_t *s, size_t func_root) {
  if (s->as.whl.init) {
    scope_enter(scopes, depth, NULL, NULL);
    gen_stmt(cg, scopes, depth, s->as.whl.init, func_root, false);
  }
  if (stmt_try_emit_list_sum_loop(cg, scopes, *depth, s)) {
    if (s->as.whl.init)
      scope_pop(scopes, depth);
    return;
  }
  stmt_str_append_loop_t str_append_loop = {0};
  bool use_str_append_loop =
      stmt_prepare_str_append_loop(cg, scopes, *depth, s, &str_append_loop) &&
      stmt_begin_str_append_loop(cg, &str_append_loop);
  int64_t trip_count_hint = 0;
  bool has_trip_count_hint =
      stmt_try_while_trip_upper_bound(cg, scopes, *depth, s, &trip_count_hint) &&
      trip_count_hint > 0;
  LLVMValueRef dynamic_trip_count_raw =
      has_trip_count_hint ? NULL
                          : stmt_try_dynamic_while_trip_count_raw(
                                cg, scopes, *depth, s);
  stmt_loop_append_len_snapshot_t append_len_snaps[64] = {0};
  size_t append_len_snap_count = 0;
  stmt_loop_dict_set_snapshot_t dict_set_snaps[64] = {0};
  size_t dict_set_snap_count = 0;
  if ((has_trip_count_hint || dynamic_trip_count_raw) &&
      !stmt_loop_body_has_control_exit(s->as.whl.body)) {
    stmt_collect_loop_append_len_snapshots(
        cg, scopes, *depth, s->as.whl.body, append_len_snaps,
        &append_len_snap_count,
        sizeof(append_len_snaps) / sizeof(append_len_snaps[0]));
    stmt_collect_loop_dict_set_snapshots(
        cg, scopes, *depth, s->as.whl.body, dict_set_snaps,
        &dict_set_snap_count,
        sizeof(dict_set_snaps) / sizeof(dict_set_snaps[0]));
    stmt_reserve_loop_append_lists(cg, append_len_snaps,
                                   append_len_snap_count, trip_count_hint);
    if (has_trip_count_hint)
      stmt_reserve_loop_dict_sets(cg, dict_set_snaps, dict_set_snap_count,
                                  trip_count_hint);
    else
      stmt_reserve_loop_dict_sets_dynamic(cg, dict_set_snaps,
                                          dict_set_snap_count,
                                          dynamic_trip_count_raw);
  }
  LLVMValueRef f = ny_cur_fn(cg);
  LLVMBasicBlockRef cb = ny_bb_fn(f, "wc"), bb = ny_bb_fn(f, "wb"),
                    eb = ny_bb_fn(f, "we");
  LLVMBasicBlockRef ub = NULL;
  if (s->as.whl.update)
    ub = ny_bb_fn(f, "wu");
  LLVMBasicBlockRef cont_bb = ub ? ub : cb;
  ny_dbg_loc(cg, s->tok);
  ny_br(cg, cb);

  ny_pos(cg, cb);
  ny_dbg_loc(cg, s->tok);
  const char *while_lhs_name = stmt_while_lhs_name(s);
  binding *while_lhs_binding =
      stmt_while_lhs_binding(cg, scopes, *depth, s);
  bool suppress_cond_int_proof =
      while_lhs_name && while_lhs_binding &&
      (while_lhs_binding->is_int_slot || while_lhs_binding->is_int_direct) &&
      (stmt_loop_assigns_name_unproven_int(cg, scopes, *depth, s->as.whl.body,
                                           while_lhs_name) ||
       stmt_loop_assigns_name_unproven_int(cg, scopes, *depth,
                                           s->as.whl.update, while_lhs_name));
  if (suppress_cond_int_proof) {
    while_lhs_binding->is_int_slot = false;
    while_lhs_binding->is_int_direct = false;
    while_lhs_binding->is_int_raw_direct = false;
    while_lhs_binding->has_int_range = false;
  }
  bool inferred_vectorize = stmt_auto_simd_loop_shape(cg, scopes, *depth, s);
  if (!stmt_try_emit_direct_cmp_cond_branch(cg, scopes, *depth, s->as.whl.test,
                                            bb, eb)) {
    ny_cond_br(cg, stmt_gen_cond_i1(cg, scopes, *depth, s->as.whl.test), bb,
               eb);
  }

  ny_pos(cg, bb);
  LLVMMetadataRef dbg_scope = codegen_debug_push_block(cg, s->tok);
  scope_enter(scopes, depth, eb, cont_bb);
  stmt_binding_int_snapshot_t loop_index_snapshot =
      stmt_snapshot_binding_int_proof(
          stmt_while_lhs_binding(cg, scopes, *depth, s));
  stmt_preseed_while_condition_index_range(cg, scopes, *depth, s);
  if (has_trip_count_hint) {
    int64_t parent_hint = scopes[*depth].loop_trip_hint;
    if (parent_hint > 0) {
      int64_t combined = 0;
      if (ny_mul_range_ok(parent_hint, trip_count_hint, &combined) &&
          combined > 0)
        scopes[*depth].loop_trip_hint = combined;
    } else {
      scopes[*depth].loop_trip_hint = trip_count_hint;
    }
    stmt_preseed_while_index_range(cg, scopes, *depth, s, trip_count_hint);
    stmt_preseed_loop_accumulator_ranges(cg, scopes, *depth, s->as.whl.body,
                                         trip_count_hint,
                                         stmt_while_lhs_name(s));
    stmt_preseed_loop_index_ranges(cg, scopes, *depth, s->as.whl.body,
                                   trip_count_hint);
    if (!append_len_snap_count && !stmt_loop_body_has_control_exit(s->as.whl.body))
      stmt_collect_loop_append_len_snapshots(
          cg, scopes, *depth, s->as.whl.body, append_len_snaps,
          &append_len_snap_count,
          sizeof(append_len_snaps) / sizeof(append_len_snaps[0]));
  }
  const char *prev_str_append_name = cg->active_str_append_name;
  LLVMValueRef prev_str_append_builder = cg->active_str_append_builder;
  bool prev_str_append_used = cg->active_str_append_used;
  if (use_str_append_loop) {
    cg->active_str_append_name = str_append_loop.name;
    cg->active_str_append_builder = str_append_loop.builder;
    cg->active_str_append_used = false;
  }
  gen_stmt(cg, scopes, depth, s->as.whl.body, func_root, false);
  if (use_str_append_loop) {
    cg->active_str_append_name = prev_str_append_name;
    cg->active_str_append_builder = prev_str_append_builder;
    cg->active_str_append_used = prev_str_append_used;
  }
  emit_defers(cg, scopes, *depth, *depth);
  if (!ny_has_terminator(cg)) {
    ny_dbg_loc(cg, s->tok);
    LLVMValueRef latch_br = ny_br(cg, cont_bb);
    if (!ub)
      apply_loop_metadata(cg, latch_br, s->as.whl.attr_unroll,
                          s->as.whl.attr_nounroll, s->as.whl.attr_vectorize,
                          inferred_vectorize);
  }

  if (ub) {
    ny_pos(cg, ub);
    gen_stmt(cg, scopes, depth, s->as.whl.update, func_root, false);
    if (!ny_has_terminator(cg)) {
      LLVMValueRef latch_br = ny_br(cg, cb);
      apply_loop_metadata(cg, latch_br, s->as.whl.attr_unroll,
                          s->as.whl.attr_nounroll, s->as.whl.attr_vectorize,
                          inferred_vectorize);
    }
  }

  scope_pop(scopes, depth);
  stmt_restore_binding_int_proof_if_still_int(loop_index_snapshot);
  stmt_apply_loop_append_len_snapshots(append_len_snaps, append_len_snap_count,
                                       trip_count_hint);
  codegen_debug_pop_block(cg, dbg_scope);

  ny_pos(cg, eb);
  if (use_str_append_loop)
    stmt_finish_str_append_loop(cg, &str_append_loop);
  if (s->as.whl.init)
    scope_pop(scopes, depth);
}

static bool get_for_iter_helpers(codegen_t *cg, token_t tok, fun_sig **ls,
                                 fun_sig **gs) {
  *ls = lookup_fun(cg, "std.core.len", 0);
  if (!*ls)
    *ls = lookup_fun(cg, "len", 0);
  *gs = lookup_fun(cg, "std.core.get", 0);
  if (!*gs)
    *gs = lookup_fun(cg, "get", 0);
  if (*ls && *gs)
    return true;
  ny_diag_error(tok, "for-loop over iterable requires std.core.len/get");
  if (verbose_enabled >= 1)
    ny_diag_hint("import std.core or ensure std.core.len/get are available");
  cg->had_error = 1;
  return false;
}

static bool stmt_expr_is_minus_one(expr_t *e, expr_t **base_out) {
  if (base_out)
    *base_out = NULL;
  if (!e || e->kind != NY_E_BINARY || !e->as.binary.op ||
      strcmp(e->as.binary.op, "-") != 0 || !e->as.binary.left ||
      !e->as.binary.right)
    return false;
  int64_t rhs = 0;
  if (!ny_expr_literal_i64(e->as.binary.right, &rhs) || rhs != 1)
    return false;
  if (base_out)
    *base_out = e->as.binary.left;
  return true;
}

static LLVMValueRef stmt_emit_raw_int_expr(codegen_t *cg, scope *scopes,
                                           size_t depth, expr_t *e,
                                           const char *name) {
  (void)name;
  if (!ny_is_proven_int(cg, scopes, depth, e, NULL))
    return NULL;
  LLVMValueRef tagged = gen_expr(cg, scopes, depth, e);
  if (!tagged)
    return NULL;
  return ny_untag_int(cg, tagged);
}

static bool stmt_fast_range_bounds_supported(codegen_t *cg, scope *scopes,
                                             size_t depth, expr_t *iterable,
                                             expr_t **start_out,
                                             expr_t **stop_excl_out,
                                             bool *stop_is_inclusive_out) {
  if (start_out)
    *start_out = NULL;
  if (stop_excl_out)
    *stop_excl_out = NULL;
  if (stop_is_inclusive_out)
    *stop_is_inclusive_out = false;
  if (!iterable || iterable->kind != NY_E_BINARY || !iterable->as.binary.op ||
      strcmp(iterable->as.binary.op, "..") != 0 || !iterable->as.binary.left ||
      !iterable->as.binary.right)
    return false;

  expr_t *start = iterable->as.binary.left;
  expr_t *right = iterable->as.binary.right;
  if (!ny_is_proven_int(cg, scopes, depth, start, NULL))
    return false;

  expr_t *minus_one_base = NULL;
  if (stmt_expr_is_minus_one(right, &minus_one_base)) {
    if (!ny_is_proven_int(cg, scopes, depth, minus_one_base, NULL))
      return false;
    if (start_out)
      *start_out = start;
    if (stop_excl_out)
      *stop_excl_out = minus_one_base;
    if (stop_is_inclusive_out)
      *stop_is_inclusive_out = false;
    return true;
  }

  if (!ny_is_proven_int(cg, scopes, depth, right, NULL))
    return false;
  int64_t min_raw = 0, max_raw = 0;
  if (!stmt_expr_int_range(cg, scopes, depth, right, &min_raw, &max_raw) ||
      max_raw == INT64_MAX)
    return false;
  (void)min_raw;

  if (start_out)
    *start_out = start;
  if (stop_excl_out)
    *stop_excl_out = right;
  if (stop_is_inclusive_out)
    *stop_is_inclusive_out = true;
  return true;
}

static bool stmt_try_emit_fast_range_for(codegen_t *cg, scope *scopes,
                                         size_t *depth, stmt_t *s,
                                         size_t func_root) {
  expr_t *start_expr = NULL;
  expr_t *stop_expr = NULL;
  bool stop_is_inclusive = false;
  if (!s || !s->as.fr.iterable ||
      !stmt_fast_range_bounds_supported(cg, scopes, *depth, s->as.fr.iterable,
                                        &start_expr, &stop_expr,
                                        &stop_is_inclusive))
    return false;

  LLVMValueRef start_raw =
      stmt_emit_raw_int_expr(cg, scopes, *depth, start_expr, "for_range_start");
  LLVMValueRef stop_raw =
      stmt_emit_raw_int_expr(cg, scopes, *depth, stop_expr, "for_range_stop");
  if (!start_raw || !stop_raw)
    return false;
  if (stop_is_inclusive) {
    stop_raw = ny_add(cg, stop_raw, LLVMConstInt(cg->type_i64, 1, true),
                      NY_LLVM_NAME(cg, "for_range_stop_excl"));
  }

  stmt_str_append_loop_t str_append_range_loop = {0};
  bool use_str_append_range = false;
  if (ny_env_enabled_default_on("NYTRIX_STRING_APPEND_LOOP_BUILDER") &&
      !stmt_loop_body_has_control_exit(s->as.fr.body)) {
    const char *name = NULL;
    bool ambiguous = false;
    stmt_collect_str_append_candidate(cg, scopes, *depth, s->as.fr.body,
                                      &name, &ambiguous);
    if (name && !ambiguous &&
        stmt_str_append_body_only_uses(cg, scopes, *depth, s->as.fr.body, name)) {
      binding *b = stmt_lookup_binding_no_mark(scopes, *depth, name, strlen(name), 0);
      if (b && b->is_mut && b->is_slot && b->value) {
        const char *bt = b->decl_type_name ? b->decl_type_name : b->type_name;
        if (ny_type_is(bt, "str")) {
          str_append_range_loop.name = name;
          str_append_range_loop.slot = b->value;
          str_append_range_loop.binding = b;
          use_str_append_range = stmt_begin_str_append_loop(cg, &str_append_range_loop);
        }
      }
    }
  }

  LLVMBasicBlockRef pre = ny_cur_block(cg);
  LLVMValueRef f = LLVMGetBasicBlockParent(pre);
  LLVMBasicBlockRef cb = ny_bb_fn(f, "frc"), bb = ny_bb_fn(f, "frb"),
                    lb = ny_bb_fn(f, "frl"), eb = ny_bb_fn(f, "fre");

  ny_dbg_loc(cg, s->tok);
  ny_br(cg, cb);

  ny_pos(cg, cb);
  LLVMValueRef cur_raw = ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "for_range_cur"));
  LLVMValueRef idx_raw = ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "for_range_idx"));
  LLVMAddIncoming(cur_raw, &start_raw, &pre, 1);
  LLVMValueRef zero = LLVMConstInt(cg->type_i64, 0, true);
  LLVMAddIncoming(idx_raw, &zero, &pre, 1);
  ny_dbg_loc(cg, s->tok);
  ny_cond_br(cg, ny_slt(cg, cur_raw, stop_raw, NY_LLVM_NAME(cg, "for_range_cond")), bb,
             eb);

  ny_pos(cg, bb);
  LLVMMetadataRef dbg_scope = codegen_debug_push_block(cg, s->tok);
  scope_enter(scopes, depth, eb, lb);

  LLVMValueRef iter_raw = s->as.fr.iter_by_index ? idx_raw : cur_raw;
  LLVMValueRef iter_binding = ny_tag_int(cg, iter_raw);
  scope_bind(cg, scopes, *depth, s->as.fr.iter_var, iter_binding, s, false,
             "int", false);
  if (scopes[*depth].vars.len > 0) {
    binding *iter_b = &scopes[*depth].vars.data[scopes[*depth].vars.len - 1];
    iter_b->is_int_direct = true;
    iter_b->is_int_raw_direct = true;
    iter_b->raw_int_value = iter_raw;
  }
  if (s->as.fr.iter_index_var) {
    LLVMValueRef index_binding = ny_tag_int(cg, idx_raw);
    scope_bind(cg, scopes, *depth, s->as.fr.iter_index_var, index_binding, s,
               false, "int", false);
    if (scopes[*depth].vars.len > 0) {
      binding *index_b = &scopes[*depth].vars.data[scopes[*depth].vars.len - 1];
      index_b->is_int_direct = true;
      index_b->is_int_raw_direct = true;
      index_b->raw_int_value = idx_raw;
    }
  }
  const char *prev_fast_str_name = cg->active_str_append_name;
  LLVMValueRef prev_fast_str_builder = cg->active_str_append_builder;
  bool prev_fast_str_used = cg->active_str_append_used;
  if (use_str_append_range) {
    cg->active_str_append_name = str_append_range_loop.name;
    cg->active_str_append_builder = str_append_range_loop.builder;
    cg->active_str_append_used = false;
  }
  gen_stmt(cg, scopes, depth, s->as.fr.body, func_root, false);
  if (use_str_append_range) {
    cg->active_str_append_name = prev_fast_str_name;
    cg->active_str_append_builder = prev_fast_str_builder;
    cg->active_str_append_used = prev_fast_str_used;
  }
  if (!ny_has_terminator(cg)) {
    emit_defers(cg, scopes, *depth, *depth);
    if (!ny_has_terminator(cg)) {
      ny_dbg_loc(cg, s->tok);
      ny_br(cg, lb);
    }
  }
  scope_pop(scopes, depth);
  codegen_debug_pop_block(cg, dbg_scope);

  ny_pos(cg, lb);
  LLVMValueRef one = LLVMConstInt(cg->type_i64, 1, true);
  LLVMValueRef next_cur =
      ny_add(cg, cur_raw, one, NY_LLVM_NAME(cg, "for_range_next"));
  LLVMValueRef next_idx =
      ny_add(cg, idx_raw, one, NY_LLVM_NAME(cg, "for_range_next_idx"));
  ny_dbg_loc(cg, s->tok);
  LLVMValueRef latch_br = ny_br(cg, cb);
  apply_loop_metadata(cg, latch_br, s->as.fr.attr_unroll,
                      s->as.fr.attr_nounroll, s->as.fr.attr_vectorize,
                      stmt_auto_simd_body_safe(cg, scopes, *depth, s->as.fr.body));
  LLVMAddIncoming(cur_raw, &next_cur, &lb, 1);
  LLVMAddIncoming(idx_raw, &next_idx, &lb, 1);

  ny_pos(cg, eb);
  if (use_str_append_range)
    stmt_finish_str_append_loop(cg, &str_append_range_loop);
  return true;
}

static void gen_stmt_for(codegen_t *cg, scope *scopes, size_t *depth, stmt_t *s,
                         size_t func_root) {

  if (s->as.fr.init || s->as.fr.cond) {
    LLVMBasicBlockRef pre = ny_cur_block(cg);
    LLVMValueRef f = LLVMGetBasicBlockParent(pre);
    LLVMBasicBlockRef cb = ny_bb_fn(f, "fc"), bb = ny_bb_fn(f, "fb"),
                      lb = ny_bb_fn(f, "fl"), eb = ny_bb_fn(f, "fe");

    if (s->as.fr.init)
      gen_stmt(cg, scopes, depth, s->as.fr.init, func_root, false);

    ny_dbg_loc(cg, s->tok);
    ny_br(cg, cb);

    ny_pos(cg, cb);
    bool inferred_vectorize =
        stmt_auto_simd_loop_shape(cg, scopes, *depth, s);
    if (s->as.fr.cond) {
      ny_dbg_loc(cg, s->tok);
      ny_cond_br(cg, stmt_gen_cond_i1(cg, scopes, *depth, s->as.fr.cond), bb,
                 eb);
    } else {
      ny_br(cg, bb);
    }

    ny_pos(cg, bb);
    LLVMMetadataRef dbg_scope = codegen_debug_push_block(cg, s->tok);
    scope_enter(scopes, depth, eb, lb);
    gen_stmt(cg, scopes, depth, s->as.fr.body, func_root, false);
  if (!ny_has_terminator(cg)) {
      emit_defers(cg, scopes, *depth, *depth);
      if (!ny_has_terminator(cg)) {
        ny_dbg_loc(cg, s->tok);
        ny_br(cg, lb);
      }
    }
    scope_pop(scopes, depth);
    codegen_debug_pop_block(cg, dbg_scope);

    ny_pos(cg, lb);
    if (s->as.fr.update)
      gen_stmt(cg, scopes, depth, s->as.fr.update, func_root, false);
    ny_dbg_loc(cg, s->tok);
    LLVMValueRef latch_br = ny_br(cg, cb);
    apply_loop_metadata(cg, latch_br, s->as.fr.attr_unroll,
                        s->as.fr.attr_nounroll, s->as.fr.attr_vectorize,
                        inferred_vectorize);

    ny_pos(cg, eb);
    return;
  }

  if (stmt_try_emit_fast_range_for(cg, scopes, depth, s, func_root))
    return;

  LLVMValueRef itv = gen_expr(cg, scopes, *depth, s->as.fr.iterable);
  LLVMBasicBlockRef pre = ny_cur_block(cg);
  LLVMValueRef f = LLVMGetBasicBlockParent(pre);
  LLVMBasicBlockRef cb = ny_bb_fn(f, "fc"), bb = ny_bb_fn(f, "fb"),
                    lb = ny_bb_fn(f, "fl"), eb = ny_bb_fn(f, "fe");
  fun_sig *ls = NULL, *gs = NULL;
  if (!get_for_iter_helpers(cg, s->tok, &ls, &gs))
    return;
  LLVMValueRef n_val = LLVMBuildCall2(cg->builder, ls->type, ls->value,
                                      (LLVMValueRef[]){itv}, 1, "");
  if (stmt_type_is_native_abi_value(cg, ls->return_type)) {
    n_val = ny_box_abi_result(cg, n_val, ls->return_type);
  }
  ny_dbg_loc(cg, s->tok);
  ny_br(cg, cb);

  ny_pos(cg, cb);

  LLVMValueRef i_start = ny_c1(cg);
  LLVMValueRef i_val = ny_phi(cg, cg->type_i64, NY_LLVM_NAME(cg, "for_i"));
  LLVMAddIncoming(i_val, &i_start, &pre, 1);
  ny_dbg_loc(cg, s->tok);
  ny_cond_br(cg, ny_slt(cg, i_val, n_val, ""), bb, eb);

  ny_pos(cg, bb);

  unsigned get_param_count = LLVMCountParamTypes(gs->type);
  LLVMValueRef get_args[3] = {
      itv, i_val, ny_c1(cg)};
  LLVMValueRef item = LLVMBuildCall2(cg->builder, gs->type, gs->value, get_args,
                                     get_param_count, "");
  if (stmt_type_is_native_abi_value(cg, gs->return_type)) {
    item = ny_box_abi_result(cg, item, gs->return_type);
  }
  LLVMValueRef iter_binding = s->as.fr.iter_by_index ? i_val : item;
  bool iter_binding_is_int =
      s->as.fr.iter_by_index ||
      stmt_iterable_yields_int(cg, scopes, *depth, s->as.fr.iterable);

  stmt_str_append_loop_t str_append_iter_loop = {0};
  bool use_str_append_iter = false;
  if (ny_env_enabled_default_on("NYTRIX_STRING_APPEND_LOOP_BUILDER") &&
      !stmt_loop_body_has_control_exit(s->as.fr.body)) {
    const char *name = NULL;
    bool ambiguous = false;
    stmt_collect_str_append_candidate(cg, scopes, *depth, s->as.fr.body,
                                      &name, &ambiguous);
    if (name && !ambiguous &&
        stmt_str_append_body_only_uses(cg, scopes, *depth, s->as.fr.body, name)) {
      binding *b = stmt_lookup_binding_no_mark(scopes, *depth, name, strlen(name), 0);
      if (b && b->is_mut && b->is_slot && b->value) {
        const char *bt = b->decl_type_name ? b->decl_type_name : b->type_name;
        if (ny_type_is(bt, "str")) {
          str_append_iter_loop.name = name;
          str_append_iter_loop.slot = b->value;
          str_append_iter_loop.binding = b;
          use_str_append_iter = stmt_begin_str_append_loop(cg, &str_append_iter_loop);
        }
      }
    }
  }

  LLVMMetadataRef dbg_scope = codegen_debug_push_block(cg, s->tok);
  scope_enter(scopes, depth, eb, lb);
  scope_bind(cg, scopes, *depth, s->as.fr.iter_var, iter_binding, s, false,
             iter_binding_is_int ? "int" : NULL, false);
  if (iter_binding_is_int && scopes[*depth].vars.len > 0) {
    binding *iter_b = &scopes[*depth].vars.data[scopes[*depth].vars.len - 1];
    iter_b->is_int_direct = true;
    iter_b->is_int_raw_direct = true;
    iter_b->raw_int_value = ny_untag_int(cg, iter_binding);
  }
  if (s->as.fr.iter_index_var) {
    scope_bind(cg, scopes, *depth, s->as.fr.iter_index_var, i_val, s, false,
               "int", false);
    if (scopes[*depth].vars.len > 0) {
      binding *index_b = &scopes[*depth].vars.data[scopes[*depth].vars.len - 1];
      index_b->is_int_direct = true;
      index_b->is_int_raw_direct = true;
      index_b->raw_int_value = ny_untag_int(cg, i_val);
    }
  }
  const char *prev_iter_str_name = cg->active_str_append_name;
  LLVMValueRef prev_iter_str_builder = cg->active_str_append_builder;
  bool prev_iter_str_used = cg->active_str_append_used;
  if (use_str_append_iter) {
    cg->active_str_append_name = str_append_iter_loop.name;
    cg->active_str_append_builder = str_append_iter_loop.builder;
    cg->active_str_append_used = false;
  }
  gen_stmt(cg, scopes, depth, s->as.fr.body, func_root, false);
  if (use_str_append_iter) {
    cg->active_str_append_name = prev_iter_str_name;
    cg->active_str_append_builder = prev_iter_str_builder;
    cg->active_str_append_used = prev_iter_str_used;
  }
  if (!ny_has_terminator(cg)) {
    emit_defers(cg, scopes, *depth, *depth);
    if (!ny_has_terminator(cg)) {
      ny_dbg_loc(cg, s->tok);

      ny_br(cg, lb);
    }
  }
  scope_pop(scopes, depth);
  codegen_debug_pop_block(cg, dbg_scope);
  ny_pos(cg, lb);

  LLVMValueRef i_next =
      ny_add(cg, i_val, LLVMConstInt(cg->type_i64, 2, false), "");
  ny_dbg_loc(cg, s->tok);

  LLVMValueRef latch_br = ny_br(cg, cb);
  apply_loop_metadata(cg, latch_br, s->as.fr.attr_unroll,
                      s->as.fr.attr_nounroll, s->as.fr.attr_vectorize, false);
  LLVMAddIncoming(i_val, &i_next, &lb, 1);

  ny_pos(cg, eb);
  if (use_str_append_iter)
    stmt_finish_str_append_loop(cg, &str_append_iter_loop);
}

static void gen_stmt_try(codegen_t *cg, scope *scopes, size_t *depth, stmt_t *s,
                         size_t func_root, bool is_tail) {
  fun_sig *set_env = lookup_fun(cg, "__set_panic_env", 0);
  fun_sig *clr_env = lookup_fun(cg, "__clear_panic_env", 0);
  fun_sig *get_err = lookup_fun(cg, "__get_panic_val", 0);
  if (!set_env || !clr_env || !get_err) {
    ny_diag_error(s->tok, "missing runtime support for try/catch");
    if (verbose_enabled >= 1)
      ny_diag_hint("required runtime symbols: "
                   "__set_panic_env/__clear_panic_env/__get_panic_val");
    cg->had_error = 1;
    return;
  }

  LLVMTypeRef arr_type = LLVMArrayType(ny_i8_ty(cg), 1024);
  LLVMValueRef jmpbuf = build_alloca(cg, "jmpbuf", arr_type);
  LLVMSetAlignment(jmpbuf, 16);

  LLVMValueRef func = ny_cur_fn(cg);
  ny_apply_decl_fn_attrs(cg, func, s);

  LLVMValueRef jmpbuf_ptr = ny_ptr2i64(cg, jmpbuf, "");
  LLVMBuildCall2(cg->builder, set_env->type, set_env->value,
                 (LLVMValueRef[]){jmpbuf_ptr}, 1, "");

  LLVMValueRef setjmp_func = NULL;
  LLVMTypeRef ret_t = LLVMInt32TypeInContext(cg->ctx);
  LLVMTypeRef arg_t = ny_llvm_ptr_type(cg->ctx);

#ifdef _WIN32
  setjmp_func = ny_get_named_fn(cg, "_setjmp");
  if (!setjmp_func) {
    setjmp_func = LLVMAddFunction(
        cg->module, "_setjmp",
        LLVMFunctionType(ret_t, (LLVMTypeRef[]){arg_t, arg_t}, 2, 0));
  }
#else

  setjmp_func = ny_get_named_fn(cg, "_setjmp");
  if (!setjmp_func)
    setjmp_func = ny_get_named_fn(cg, "setjmp");
  if (!setjmp_func) {
    setjmp_func =
        LLVMAddFunction(cg->module, "_setjmp",
                        LLVMFunctionType(ret_t, (LLVMTypeRef[]){arg_t}, 1, 0));
  }
#endif

  if (setjmp_func) {
    unsigned rt_kind = LLVMGetEnumAttributeKindForName("returns_twice", 13);
    if (rt_kind != 0) {
      LLVMAttributeRef rt_attr = LLVMCreateEnumAttribute(cg->ctx, rt_kind, 0);
      LLVMAddAttributeAtIndex(setjmp_func, LLVMAttributeFunctionIndex, rt_attr);
    }
    unsigned nuw_kind = LLVMGetEnumAttributeKindForName("nounwind", 8);
    if (nuw_kind != 0) {
      LLVMAttributeRef nuw_attr = LLVMCreateEnumAttribute(cg->ctx, nuw_kind, 0);
      LLVMAddAttributeAtIndex(setjmp_func, LLVMAttributeFunctionIndex,
                              nuw_attr);
    }
  }

#ifdef _WIN32
  LLVMValueRef sj_res = LLVMBuildCall2(
      cg->builder, LLVMGlobalGetValueType(setjmp_func), setjmp_func,
      (LLVMValueRef[]){jmpbuf, LLVMConstNull(arg_t)}, 2, "sj_res");
#else
  LLVMValueRef sj_res =
      LLVMBuildCall2(cg->builder, LLVMGlobalGetValueType(setjmp_func),
                     setjmp_func, (LLVMValueRef[]){jmpbuf}, 1, "sj_res");
#endif
  if (sj_res && LLVMIsAInstruction(sj_res)) {
    unsigned rt_kind = LLVMGetEnumAttributeKindForName("returns_twice", 13);
    if (rt_kind != 0) {
      LLVMAttributeRef rt_attr = LLVMCreateEnumAttribute(cg->ctx, rt_kind, 0);
      LLVMAddCallSiteAttribute(sj_res, LLVMAttributeFunctionIndex, rt_attr);
    }
  }

  LLVMBasicBlockRef try_b =
      LLVMAppendBasicBlockInContext(cg->ctx, func, "try_body");
  LLVMBasicBlockRef catch_b =
      LLVMAppendBasicBlockInContext(cg->ctx, func, "catch_body");
  LLVMBasicBlockRef end_b =
      LLVMAppendBasicBlockInContext(cg->ctx, func, "try_end");
  LLVMBuildCondBr(cg->builder,
                  ny_eq(cg, sj_res,
                        LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, false),
                        ""),
                  try_b, catch_b);
  ny_pos(cg, try_b);

  size_t prev_panic_envs = cg->active_panic_envs;
  cg->active_panic_envs = prev_panic_envs + 1;
  gen_stmt(cg, scopes, depth, s->as.tr.body, func_root, is_tail);
  cg->active_panic_envs = prev_panic_envs;
  if (!ny_has_terminator(cg)) {
    LLVMBuildCall2(cg->builder, clr_env->type, clr_env->value, NULL, 0, "");
    ny_br(cg, end_b);
  }
  ny_pos(cg, catch_b);

  LLVMBuildCall2(cg->builder, clr_env->type, clr_env->value, NULL, 0, "");
  LLVMValueRef err_val = LLVMBuildCall2(cg->builder, get_err->type,
                                        get_err->value, NULL, 0, "err");
  cg->active_panic_envs = prev_panic_envs;
  if (s->as.tr.err) {
    LLVMMetadataRef dbg_scope = codegen_debug_push_block(cg, s->tok);
    scope_enter(scopes, depth, scopes[*depth].break_bb,
                scopes[*depth].continue_bb);
    scope_bind(cg, scopes, *depth, s->as.tr.err, err_val, s, false, NULL,
               false);
    gen_stmt(cg, scopes, depth, s->as.tr.handler, func_root, is_tail);
    scope_pop(scopes, depth);
    codegen_debug_pop_block(cg, dbg_scope);
  } else {
    gen_stmt(cg, scopes, depth, s->as.tr.handler, func_root, is_tail);
  }
  if (!ny_has_terminator(cg)) {

    ny_br(cg, end_b);
  }
  ny_pos(cg, end_b);
}

static void gen_stmt_defer(codegen_t *cg, scope *scopes, size_t depth,
                           stmt_t *s) {
  if (!s->as.de.body)
    return;
  ny_param_list no_params = {0};
  LLVMValueRef cls = gen_closure(cg, scopes, depth, no_params, s->as.de.body,
                                 false, NULL, "__defer");
  LLVMValueRef cls_raw =
      LLVMBuildIntToPtr(cg->builder, cls, ny_ptr_i64_ty(cg), "");
  LLVMValueRef fn_ptr_int = ny_load(cg, cls_raw, NY_LLVM_NAME(cg, "defer_fn"));
  LLVMValueRef env_addr = LLVMBuildGEP2(cg->builder, cg->type_i64, cls_raw,
                                        (LLVMValueRef[]){ny_c1(cg)}, 1, "");
  LLVMValueRef env = ny_load(cg, env_addr, NY_LLVM_NAME(cg, "defer_env"));
  fun_sig *push_sig = lookup_fun(cg, "__push_defer", 0);
  if (push_sig) {
    LLVMBuildCall2(cg->builder, push_sig->type, push_sig->value,
                   (LLVMValueRef[]){fn_ptr_int, env}, 2, "");
  }
  vec_push(&scopes[depth].defers, s->as.de.body);
}

static void gen_stmt_struct(codegen_t *cg, stmt_t *s) {
  if (lookup_layout(cg, s->as.struc.name))
    return;
  const char *name = s->as.struc.name;
  LLVMTypeRef st = LLVMStructCreateNamed(cg->ctx, name);
  layout_def_t *def = malloc(sizeof(layout_def_t));
  memset(def, 0, sizeof(*def));
  def->name = ny_strdup(name);
  def->llvm_type = st;
  def->is_layout = (s->kind == NY_S_LAYOUT);
  def->heap_allocated = true;
  def->stmt = s;
  vec_push(&cg->layouts, def);
  size_t count = s->as.struc.fields.len;
  LLVMTypeRef *element_types =
      malloc(sizeof(LLVMTypeRef) * (count > 0 ? count : 1));
  for (size_t i = 0; i < count; i++) {
    layout_field_t *f = &s->as.struc.fields.data[i];
    type_layout_t f_layout = resolve_raw_layout(cg, f->type_name, s->tok);
    if (f_layout.is_valid && f_layout.llvm_type) {
      element_types[i] = f_layout.llvm_type;
    } else {
      element_types[i] = cg->type_i64;
    }
  }
  LLVMStructSetBody(st, element_types, (unsigned)count,
                    s->as.struc.pack ? true : false);
  free(element_types);
  LLVMTargetDataRef td = LLVMGetModuleDataLayout(cg->module);
  def->size = LLVMStoreSizeOfType(td, st);
  def->align = LLVMABIAlignmentOfType(td, st);
}

static void gen_stmt_module(codegen_t *cg, scope *scopes, size_t *depth,
                            stmt_t *s, size_t func_root, bool is_tail) {
  const char *prev = cg->current_module_name;
  cg->current_module_name = s->as.module.name;
  bool direct_source_module =
      ny_stmt_tree_has_source_file(cg, s) ||
      ny_codegen_module_is_source_file(cg, s->as.module.name);
  for (size_t i = 0; i < s->as.module.body.len; ++i) {
    stmt_t *child = s->as.module.body.data[i];
    if (!child || child->kind == NY_S_FUNC)
      continue;
    if (cg->lazy_emit_stdlib_enabled && ny_is_stdlib_tok(child->tok) &&
        !direct_source_module && !ny_codegen_stmt_is_source_file(cg, child) &&
        child->kind == NY_S_VAR) {
      if (!ny_lazy_emit_stdlib_var_needed(cg, child, cg->current_module_name))
        continue;
    }
    gen_stmt(cg, scopes, depth, child, func_root, is_tail);
  }
  cg->current_module_name = prev;
}

void emit_defers(codegen_t *cg, scope *scopes, size_t depth, size_t func_root) {
  fun_sig *pop_sig = lookup_fun(cg, "__pop_run_defer", 0);
  for (ssize_t d = (ssize_t)depth; d >= (ssize_t)func_root; --d) {
    stmt_ownership_cleanup_scope(cg, scopes, (size_t)d);
    if (!pop_sig)
      continue;
    for (size_t i = 0; i < scopes[d].defers.len; ++i) {
      LLVMBuildCall2(cg->builder, pop_sig->type, pop_sig->value, NULL, 0, "");
    }
  }
}

void collect_labels(codegen_t *cg, LLVMValueRef func, stmt_t *s, size_t depth) {
  if (!s)
    return;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; i++)
      collect_labels(cg, func, s->as.block.body.data[i],
                     s->as.block.transparent ? depth : depth + 1);
    break;
  case NY_S_IF:
    collect_labels(cg, func, s->as.iff.conseq, depth + 1);
    if (s->as.iff.alt)
      collect_labels(cg, func, s->as.iff.alt, depth + 1);
    break;
  case NY_S_WHILE:
    collect_labels(cg, func, s->as.whl.body, depth + 1);
    break;
  case NY_S_MATCH:
    for (size_t i = 0; i < s->as.match.arms.len; i++)
      collect_labels(cg, func, s->as.match.arms.data[i].conseq, depth + 1);
    if (s->as.match.default_conseq)
      collect_labels(cg, func, s->as.match.default_conseq, depth + 1);
    break;
  case NY_S_FOR:
    collect_labels(cg, func, s->as.fr.body, depth + 1);
    break;
  case NY_S_TRY:
    collect_labels(cg, func, s->as.tr.body, depth + 1);
    collect_labels(cg, func, s->as.tr.handler, depth + 1);
    break;
  case NY_S_LABEL: {
    label_binding lb;
    lb.name = ny_strdup(s->as.label.name);
    lb.bb = ny_bb_fn(func, s->as.label.name);
    lb.depth = depth;
    vec_push(&cg->labels, lb);
    break;
  }
  case NY_S_MODULE:
    for (size_t i = 0; i < s->as.module.body.len; i++)
      collect_labels(cg, func, s->as.module.body.data[i], depth);
    break;
  case NY_S_DEFER:
    collect_labels(cg, func, s->as.de.body, depth + 1);
    break;
  case NY_S_MACRO:
    collect_labels(cg, func, s->as.macro.body, depth + 1);
    break;
  default:
    break;
  }
}

static void gen_stmt_inner(codegen_t *cg, scope *scopes, size_t *depth,
                           stmt_t *s, size_t func_root, bool is_tail) {
  if (!s || cg->had_error)
    return;

  ny_dbg_loc(cg, s->tok);
  if (cg->trace_exec)
    emit_trace_loc(cg, s->tok);
  switch (s->kind) {
  case NY_S_VAR: {
    bool dest = s->as.var.is_destructure;
    NY_COMPILER_ASSERTF(s->as.var.names.len > 0,
                        "var stmt has no bindings at line %d", s->tok.line);
    NY_COMPILER_ASSERTF(s->as.var.types.len == 0 || s->as.var.types.len == 1 ||
                            s->as.var.types.len == s->as.var.names.len,
                        "var stmt type arity mismatch names=%zu types=%zu",
                        s->as.var.names.len, s->as.var.types.len);
    if (dest) {
      NY_COMPILER_ASSERTF(
          s->as.var.exprs.len == 1,
          "destructure expects exactly one source expr, got %zu",
          s->as.var.exprs.len);
    } else if (s->as.var.exprs.len > 1) {
      NY_COMPILER_ASSERTF(
          s->as.var.exprs.len == s->as.var.names.len,
          "parallel var assignment mismatch names=%zu exprs=%zu",
          s->as.var.names.len, s->as.var.exprs.len);
    }
    bool parallel = (s->as.var.names.len == s->as.var.exprs.len) && !dest;
    sema_var_t *sema =
        (s->sema_kind == NY_STMT_SEMA_VAR) ? (sema_var_t *)s->sema : NULL;
    bool prefer_direct_locals = false;
    LLVMValueRef first_val = NULL;
    bool first_static_list_object_elided = false;
    bool first_static_list_elide_candidate = false;
    const char *first_static_list_elide_bail_reason = NULL;
    bool first_raw_int_list_mutation = false;
    const char *first_raw_int_list_bail_reason = NULL;
    if (parallel && s->as.var.names.len == 1 && s->as.var.exprs.len == 1) {
      expr_t *e0 = s->as.var.exprs.data[0];
      bool first_escapes =
          sema && sema->escapes.len > 0 && sema->escapes.data[0];
      const char *first_name = s->as.var.names.data[0];
      first_static_list_elide_candidate =
          ny_env_enabled_default_on("NYTRIX_STATIC_INT_LIST_ELIDE") &&
          !s->as.var.is_mut &&
          stmt_expr_is_int_list_literal(cg, scopes, *depth, e0);
      if (stmt_can_elide_static_int_list_object(
              cg, scopes, *depth, s, first_name, first_escapes, e0,
              &first_static_list_elide_bail_reason)) {
        first_val = ny_c0(cg);
        first_static_list_object_elided = true;
        parallel = false;
      } else if (first_static_list_elide_candidate &&
                 !first_static_list_elide_bail_reason) {
        first_static_list_elide_bail_reason = "unsupported-use";
      }
      if (!first_static_list_object_elided &&
          stmt_can_use_raw_int_list_mutation(cg, scopes, *depth, s, first_name,
                                             first_escapes, e0,
                                             &first_raw_int_list_bail_reason)) {
        first_val = ny_c0(cg);
        first_raw_int_list_mutation = true;
        parallel = false;
      }
    }
    if (!parallel && !first_static_list_object_elided &&
        !first_raw_int_list_mutation && s->as.var.exprs.len > 0) {
      expr_t *e0 = s->as.var.exprs.data[0];
      bool first_escapes =
          sema && sema->escapes.len > 0 && sema->escapes.data[0];
      const char *first_name =
          s->as.var.names.len > 0 ? s->as.var.names.data[0] : NULL;
      first_static_list_elide_candidate =
          ny_env_enabled_default_on("NYTRIX_STATIC_INT_LIST_ELIDE") &&
          !s->as.var.is_mut &&
          stmt_expr_is_int_list_literal(cg, scopes, *depth, e0);
      if (!dest && first_name &&
          stmt_can_elide_static_int_list_object(
              cg, scopes, *depth, s, first_name, first_escapes, e0,
              &first_static_list_elide_bail_reason)) {
        first_val = ny_c0(cg);
        first_static_list_object_elided = true;
      } else if (!dest && first_name && first_static_list_elide_candidate &&
                 !first_static_list_elide_bail_reason) {
        first_static_list_elide_bail_reason = "unsupported-use";
      } else if (!dest && first_name &&
                 stmt_can_use_raw_int_list_mutation(
                     cg, scopes, *depth, s, first_name, first_escapes, e0,
                     &first_raw_int_list_bail_reason)) {
        first_val = ny_c0(cg);
        first_raw_int_list_mutation = true;
      } else if (ny_env_enabled("NYTRIX_STACK_INT_LIST_LITERALS") &&
                 !first_escapes &&
                 stmt_expr_is_int_list_literal(cg, scopes, *depth, e0))
        first_val = gen_expr_list_stack_alloc(cg, scopes, *depth, e0);
      else
        first_val = gen_expr(cg, scopes, *depth, e0);
    }
    fun_sig *gs = NULL;
    if (dest) {
      gs = lookup_fun(cg, "get", 0);
      if (!gs) {
        ny_diag_error(s->tok, "destructuring requires 'get' function");
        if (verbose_enabled >= 1)
          ny_diag_hint("import std.core or ensure 'get' is in scope");
        cg->had_error = 1;
        return;
      }
    }
    LLVMValueRef *parallel_assign_values = NULL;
    if (!s->as.var.is_decl && parallel && s->as.var.names.len > 1) {
      parallel_assign_values =
          calloc(s->as.var.names.len, sizeof(*parallel_assign_values));
      if (!parallel_assign_values) {
        ny_diag_error(s->tok,
                      "out of memory while preparing grouped assignment");
        cg->had_error = 1;
        return;
      }
      for (size_t j = 0; j < s->as.var.names.len; j++)
        parallel_assign_values[j] =
            gen_expr(cg, scopes, *depth, s->as.var.exprs.data[j]);
      if (cg->had_error) {
        free(parallel_assign_values);
        return;
      }
    }
    for (size_t i = 0; i < s->as.var.names.len; i++) {
      const char *n = s->as.var.names.data[i];
      NY_COMPILER_ASSERTF(n && *n, "var binding %zu missing name", i);
      LLVMValueRef slot = NULL;
      bool bind_direct = false;
      binding *resolved_local = NULL;
      binding *resolved_global = NULL;
      bool top_level_existing_global = false;
      expr_t *expr_for_check = NULL;
      const char *decl_type = NULL;
      bool decl_type_explicit = false;
      if (s->as.var.types.len > i)
        decl_type = s->as.var.types.data[i];
      decl_type_explicit = decl_type && *decl_type;
      if (parallel) {
        expr_for_check = s->as.var.exprs.data[i];
      } else if (!dest) {
        if (s->as.var.exprs.len > 0)
          expr_for_check = s->as.var.exprs.data[0];
      }
      if (!decl_type && expr_for_check) {
        const char *inf = infer_expr_type(cg, scopes, *depth, expr_for_check);
        if ((cg->strict_types &&
             (stmt_bindable_inferred_type(inf) ||
              stmt_type_name_is_int_value(inf))) ||
            (!s->as.var.is_mut && stmt_bindable_inferred_type(inf)) ||
            (s->as.var.is_mut && stmt_bindable_mut_inferred_type(inf)))
          decl_type = inf;
      }
      bool top_entry_numeric_hoist = stmt_top_entry_numeric_hoist(
          cg, scopes, *depth, s, sema, i, decl_type, expr_for_check);
      bool top_entry_existing_local_assign = false;
      bool top_entry_can_hoist = stmt_top_entry_can_hoist_var(cg, n);
      if (*depth == 0 && top_entry_can_hoist && !s->as.var.is_decl) {
        bool existing_is_global = false;
        binding *existing = stmt_var_lookup_existing(cg, scopes, *depth, n,
                                                     &existing_is_global);
        top_entry_existing_local_assign = existing && !existing_is_global;
      }
      bool top_entry_local_path =
          *depth == 0 && top_entry_can_hoist &&
          (top_entry_numeric_hoist || top_entry_existing_local_assign);
      if (s->as.var.is_del) {
        bool target_is_global = false;
        LLVMValueRef zero = ny_c0(cg);
        binding *eb =
            stmt_var_lookup_existing(cg, scopes, *depth, n, &target_is_global);
        if (!eb) {
          report_undef_symbol(cg, n, s->tok);
          return;
        }
        if (!ensure_mutable_binding_for_assign(cg, s->tok, n, eb,
                                               target_is_global)) {
          continue;
        }
        eb->is_int_slot = false;
        eb->is_int_direct = false;
        eb->raw_int_value = NULL;
        eb->is_f64_slot = false;
        eb->is_f64_direct = false;
        eb->is_f32_slot = false;
        eb->is_f32_direct = false;
        eb->is_list_storage = false;
        eb->is_int_list_storage = false;
        eb->is_f64_list_storage = false;
        eb->is_dict_storage = false;
        eb->is_int_dict_storage = false;
        eb->has_int_range = false;
        eb->has_list_int_range = false;
        eb->has_list_len_min = false;
        eb->has_dict_int_range = false;
        eb->type_name = NULL;
        eb->decl_type_name = NULL;
        if (target_is_global || eb->is_slot) {
          slot = eb->value;
        } else {
          eb->value = zero;

          continue;
        }
        if (ensure_store_ready(cg, s->tok, zero, slot, "NY_S_VAR(del)")) {
          ny_store(cg, slot, zero);
        }
        continue;
      }
      if (*depth == 0 && !top_entry_local_path) {
        binding *gb = lookup_global(cg, n);
        if (gb) {
          resolved_global = gb;
          top_level_existing_global = true;
          slot = gb->value;
        } else {
          const char *type_name = decl_type;
          if (s->as.var.types.len > i)
            type_name = s->as.var.types.data[i];
          bool sema_global_f64 = sema && sema->is_f64_proven.len > i &&
                                 sema->is_f64_proven.data[i];
          if (!type_name && sema_global_f64)
            type_name = "f64";
          LLVMTypeRef global_type = cg->type_i64;
          bool global_is_f64 =
              sema_global_f64 || stmt_type_name_is_f64_value(type_name);
          bool global_is_f32 = stmt_type_name_is_f32_value(type_name);
          if (global_is_f64)
            global_type = cg->type_f64;
          else if (global_is_f32)
            global_type = cg->type_f32;
          slot = LLVMAddGlobal(cg->module, global_type, n);
          if ((!cg->current_module_name || !*cg->current_module_name) &&
              !ny_is_stdlib_tok(s->tok)) {
            LLVMSetLinkage(slot, LLVMPrivateLinkage);
          }
          LLVMSetInitializer(slot, LLVMConstNull(global_type));
          binding b = {0};
          b.name = ny_strdup(n);
          b.value = slot;
          b.stmt_t = s;
          b.is_slot = true;
          b.is_mut = s->as.var.is_mut ? true : false;
          b.owned = true;
          b.type_name = type_name;
          b.decl_type_name =
              (decl_type_explicit || cg->strict_types) ? type_name : NULL;
          b.is_f64_slot = global_is_f64;
          b.is_f32_slot = global_is_f32;
          vec_push(&cg->global_vars, b);
          if (cg->global_vars.len > 0)
            resolved_global = &cg->global_vars.data[cg->global_vars.len - 1];
          if (cg->debug_symbols && cg->di_builder) {
            codegen_debug_global_variable(cg, n, slot, type_name, s->tok);
          }
          if (s->tok.filename) {
            ny_diag_warning(s->tok,
                            "implicit declaration of global variable %s'%s'%s",
                            clr(NY_CLR_BOLD), n, clr(NY_CLR_RESET));
            ny_diag_fix("declare with %sdef %s = ...%s or %smut %s = ...%s at "
                        "the top level",
                        clr(NY_CLR_BOLD), n, clr(NY_CLR_RESET),
                        clr(NY_CLR_BOLD), n, clr(NY_CLR_RESET));
          }
        }
      }
      LLVMValueRef target_val = NULL;

      if (*depth == 0 && !top_entry_local_path) {
        binding *gb = resolved_global ? resolved_global : lookup_global(cg, n);
        if (gb && decl_type && !gb->type_name) {
          gb->type_name = decl_type;
          if (decl_type_explicit || cg->strict_types)
            gb->decl_type_name = decl_type;
        }
      }
      if (*depth == 0 && !top_entry_local_path && s->as.var.is_decl &&
          !s->as.var.is_del && !dest && s->as.var.names.len == 1 &&
          s->as.var.exprs.len == 1 && slot && LLVMIsAGlobalVariable(slot) &&
          !top_level_existing_global) {
        LLVMValueRef const_init =
            stmt_const_top_level_expr_value(cg, expr_for_check);
        if (const_init) {
          LLVMSetInitializer(slot, const_init);
          binding *gb =
              resolved_global ? resolved_global : lookup_global(cg, n);
          if (gb) {
            const char *init_type =
                decl_type ? decl_type
                          : infer_expr_type(cg, scopes, *depth, expr_for_check);
            if (!gb->type_name)
              gb->type_name = init_type;
            if ((decl_type_explicit || cg->strict_types) && !gb->decl_type_name)
              gb->decl_type_name = init_type;
          }
          continue;
        }
      }

      if (*depth > 0 || top_entry_local_path) {
        if (s->as.var.is_decl) {
          stmt_var_setup_local_binding(cg, scopes, *depth, s, sema, i, n,
                                       decl_type, decl_type_explicit,
                                       prefer_direct_locals,
                                       &bind_direct, &slot);
        } else {
          bool target_is_global = false;
          binding *eb = stmt_var_lookup_existing(cg, scopes, *depth, n,
                                                 &target_is_global);
          if (eb) {
            if (!ensure_mutable_binding_for_assign(cg, s->tok, n, eb,
                                                   target_is_global)) {
              continue;
            }
            if (target_is_global) {
              resolved_global = eb;
              if (!eb->is_slot) {
                ny_diag_error(s->tok,
                              "cannot assign to non-addressable value '%s'", n);
                cg->had_error = 1;
                continue;
              }
              slot = eb->value;
            } else {
              resolved_local = eb;
              if (eb->is_slot)
                slot = eb->value;
            }
          } else {
            stmt_var_setup_local_binding(cg, scopes, *depth, s, sema, i, n,
                                         decl_type, decl_type_explicit,
                                         prefer_direct_locals,
                                         &bind_direct, &slot);
          }
        }
      }

      bool target_is_f64_slot = false;
      bool target_is_f32_slot = false;
      if (resolved_local) {
        if (resolved_local->is_f64_slot)
          target_is_f64_slot = true;
        if (resolved_local->is_f32_slot)
          target_is_f32_slot = true;
      } else if (resolved_global) {
        if (resolved_global->is_f64_slot)
          target_is_f64_slot = true;
        if (resolved_global->is_f32_slot)
          target_is_f32_slot = true;
      } else if (!resolved_global && slot) {
        size_t nlen = strlen(n);
        binding *nb = stmt_lookup_binding_no_mark(scopes, *depth, n, nlen, 0);
        if (nb) {
          if (nb->is_f64_slot)
            target_is_f64_slot = true;
          if (nb->is_f32_slot)
            target_is_f32_slot = true;
        }
      }

      if (!s->as.var.is_decl && !dest && s->as.var.names.len == 1 &&
          s->as.var.exprs.len == 1 &&
          stmt_try_emit_active_str_append_assignment(cg, scopes, *depth, n,
                                                     expr_for_check)) {
        continue;
      }

      if (expr_for_check && scopes[*depth].loop_trip_hint > 0) {
        stmt_try_widen_loop_accumulator_binding(cg, scopes, *depth, n,
                                                expr_for_check,
                                                scopes[*depth].loop_trip_hint);
      }

      if ((target_is_f64_slot || target_is_f32_slot) && slot &&
          expr_for_check &&
          (!parallel || s->as.var.names.len == 1 || parallel_assign_values) &&
          !dest) {
        LLVMTypeRef target_type =
            target_is_f32_slot ? cg->type_f32 : cg->type_f64;
        if (parallel_assign_values) {
          LLVMValueRef fv = parallel_assign_values[i];
          if (fv && LLVMTypeOf(fv) != target_type) {
            fv = ny_unbox_float(cg, fv);
            if (target_is_f32_slot)
              fv = LLVMBuildFPTrunc(cg->builder, fv, cg->type_f32, "f2f");
          }
          if (ensure_store_ready(cg, s->tok, fv, slot, "NY_S_VAR_FLT"))
            ny_store(cg, slot, fv);
          continue;
        }
        const char *et = infer_expr_type(cg, scopes, *depth, expr_for_check);
        bool rhs_is_float = stmt_type_name_is_float_value(et);
        if (rhs_is_float) {
          LLVMValueRef fv = gen_expr_as_f64(cg, scopes, *depth, expr_for_check);
          if (target_is_f32_slot)
            fv = LLVMBuildFPTrunc(cg->builder, fv, cg->type_f32, "f2f");
          if (ensure_store_ready(cg, s->tok, fv, slot, "NY_S_VAR_FLT"))
            ny_store(cg, slot, fv);
          continue;
        }
        bool rhs_is_int = stmt_type_name_is_int_value(et);
        if (rhs_is_int) {
          LLVMValueRef iv = gen_expr(cg, scopes, *depth, expr_for_check);
          LLVMValueRef fv = LLVMBuildSIToFP(
              cg->builder, ny_ashr(cg, iv, ny_c1(cg), ""), target_type, "i2f");
          if (ensure_store_ready(cg, s->tok, fv, slot, "NY_S_VAR_FLT"))
            ny_store(cg, slot, fv);
          continue;
        }

        {
          LLVMValueRef v = gen_expr(cg, scopes, *depth, expr_for_check);
          LLVMValueRef fv = ny_unbox_float(cg, v);
          if (target_is_f32_slot)
            fv = LLVMBuildFPTrunc(cg->builder, fv, cg->type_f32, "f2f");
          if (ensure_store_ready(cg, s->tok, fv, slot, "NY_S_VAR_FLT"))
            ny_store(cg, slot, fv);
          continue;
        }
      }

      bool direct_native_float_candidate =
          bind_direct && stmt_type_name_is_f64_value(decl_type) && !dest &&
          expr_for_check &&
          (!parallel || s->as.var.names.len == 1);

      binding *rhs_self_dest = resolved_local ? resolved_local : resolved_global;
      if (!rhs_self_dest && slot) {
        size_t nlen = strlen(n);
        rhs_self_dest =
            stmt_lookup_binding_no_mark(scopes, *depth, n, nlen, 0);
      }
      bool rhs_pre_proven_int =
          expr_for_check && ny_is_proven_int(cg, scopes, *depth,
                                             expr_for_check, NULL);
      bool suppress_self_raw_int =
          rhs_self_dest && rhs_self_dest->is_int_slot && expr_for_check &&
          stmt_expr_contains_ident_name(expr_for_check, n) &&
          !rhs_pre_proven_int;
      stmt_binding_int_snapshot_t self_raw_snap = {0};
      if (suppress_self_raw_int) {
        self_raw_snap = stmt_snapshot_binding_int_proof(rhs_self_dest);
        rhs_self_dest->is_int_slot = false;
        rhs_self_dest->is_int_direct = false;
        rhs_self_dest->is_int_raw_direct = false;
        rhs_self_dest->has_int_range = false;
      }
      if (direct_native_float_candidate) {
        target_val = NULL;
      } else if (parallel) {
        target_val = parallel_assign_values
                         ? parallel_assign_values[i]
                         : gen_expr(cg, scopes, *depth, expr_for_check);
      } else if (dest) {
        uint64_t tagged_idx = ((uint64_t)i << 1) | 1;
        LLVMValueRef idx_val = LLVMConstInt(cg->type_i64, tagged_idx, false);
        target_val =
            LLVMBuildCall2(cg->builder, gs->type, gs->value,
                           (LLVMValueRef[]){first_val, idx_val}, 2, "");
      } else {
        target_val = first_val;
      }
      stmt_restore_binding_int_proof(self_raw_snap);
      if (!s->as.var.is_destructure) {
        const char *want = decl_type;
        if (!s->as.var.is_decl) {
          binding *cur = resolved_local;
          if (cur)
            want = binding_assign_type(cur);
          else if (resolved_global)
            want = binding_assign_type(resolved_global);
        }
        if (want && expr_for_check)
          ensure_expr_type_compatible(cg, scopes, *depth, want, expr_for_check,
                                      expr_for_check->tok, "assignment");
      }
      bool rhs_proven_int = false;
      bool rhs_proven_f64 = false;
      bool rhs_has_int_range = false;
      int64_t rhs_int_min_raw = 0;
      int64_t rhs_int_max_raw = 0;
      bool rhs_list_storage = false;
      bool rhs_int_list_storage = false;
      bool rhs_f64_list_storage = false;
      bool rhs_dict_storage = false;
      bool rhs_int_dict_storage = false;
      bool rhs_has_list_int_range = false;
      int64_t rhs_list_min_raw = 0;
      int64_t rhs_list_max_raw = 0;
      bool rhs_has_list_len_min = false;
      int64_t rhs_list_len_min_raw = 0;
      bool rhs_has_dict_int_range = false;
      int64_t rhs_dict_min_raw = 0;
      int64_t rhs_dict_max_raw = 0;
      if (expr_for_check) {
        rhs_proven_int =
            ny_is_proven_int(cg, scopes, *depth, expr_for_check, target_val);
        const char *rhs_type =
            infer_expr_type(cg, scopes, *depth, expr_for_check);
        rhs_proven_f64 = stmt_type_name_is_float_value(rhs_type);
        if (rhs_proven_int) {
          rhs_has_int_range =
              stmt_expr_int_range(cg, scopes, *depth, expr_for_check,
                                  &rhs_int_min_raw, &rhs_int_max_raw);
        }
        if (ny_expr_is_list_or_tuple_lit(expr_for_check) ||
            stmt_expr_is_list_ctor(expr_for_check)) {
          rhs_list_storage = true;
          rhs_int_list_storage =
              stmt_expr_is_list_ctor(expr_for_check) ||
              stmt_expr_is_int_list_literal(cg, scopes, *depth, expr_for_check);
          rhs_f64_list_storage =
              stmt_expr_is_list_ctor(expr_for_check) ||
              stmt_expr_is_f64_list_literal(cg, scopes, *depth, expr_for_check);
          rhs_has_list_len_min = true;
          rhs_list_len_min_raw = ny_expr_is_list_or_tuple_lit(expr_for_check)
                                     ? (int64_t)expr_for_check->as.list_like.len
                                     : 0;
          if (rhs_int_list_storage) {
            rhs_has_list_int_range = stmt_int_list_literal_range(
                cg, scopes, *depth, expr_for_check, &rhs_list_min_raw,
                &rhs_list_max_raw);
          }
        } else if (stmt_expr_is_dict_ctor(expr_for_check)) {
          rhs_dict_storage = true;
          rhs_int_dict_storage = true;
        } else {
          expr_t *append_value = NULL;
          expr_t *set_value = NULL;
          if (stmt_expr_is_append_to_name(expr_for_check, n, &append_value)) {
            binding *src = resolved_local ? resolved_local : resolved_global;
            if (!src) {
              size_t nlen = strlen(n);
              src = stmt_lookup_binding(cg, scopes, *depth, n, nlen, 0);
            }
            if (src && src->is_list_storage) {
              rhs_list_storage = true;
              rhs_int_list_storage =
                  src->is_int_list_storage &&
                  ny_is_proven_int(cg, scopes, *depth, append_value, NULL);
              rhs_f64_list_storage =
                  src->is_f64_list_storage &&
                  stmt_expr_is_f64_value(cg, scopes, *depth, append_value);
              if (src->has_list_len_min && src->list_len_min_raw < INT64_MAX) {
                rhs_has_list_len_min = true;
                rhs_list_len_min_raw = src->list_len_min_raw + 1;
              }
              if (rhs_int_list_storage) {
                int64_t val_min = 0, val_max = 0;
                bool val_has_range = stmt_expr_int_range(
                    cg, scopes, *depth, append_value, &val_min, &val_max);
                if (src->has_list_int_range && val_has_range) {
                  rhs_has_list_int_range = true;
                  rhs_list_min_raw = src->list_int_min_raw < val_min
                                         ? src->list_int_min_raw
                                         : val_min;
                  rhs_list_max_raw = src->list_int_max_raw > val_max
                                         ? src->list_int_max_raw
                                         : val_max;
                } else if (val_has_range) {
                  rhs_has_list_int_range = true;
                  rhs_list_min_raw = val_min;
                  rhs_list_max_raw = val_max;
                }
              }
            }
          } else if (stmt_expr_is_set_idx_to_name(expr_for_check, n,
                                                  &set_value)) {
            binding *src = resolved_local ? resolved_local : resolved_global;
            if (!src) {
              size_t nlen = strlen(n);
              src = stmt_lookup_binding(cg, scopes, *depth, n, nlen, 0);
            }
            if (src && src->is_list_storage) {
              rhs_list_storage = true;
              rhs_int_list_storage =
                  src->is_int_list_storage &&
                  ny_is_proven_int(cg, scopes, *depth, set_value, NULL);
              rhs_f64_list_storage =
                  src->is_f64_list_storage &&
                  stmt_expr_is_f64_value(cg, scopes, *depth, set_value);
              if (src->has_list_len_min) {
                rhs_has_list_len_min = true;
                rhs_list_len_min_raw = src->list_len_min_raw;
              }
              if (rhs_int_list_storage) {
                int64_t val_min = 0, val_max = 0;
                bool val_has_range = stmt_expr_int_range(
                    cg, scopes, *depth, set_value, &val_min, &val_max);
                if (src->has_list_int_range && val_has_range) {
                  rhs_has_list_int_range = true;
                  rhs_list_min_raw = src->list_int_min_raw < val_min
                                         ? src->list_int_min_raw
                                         : val_min;
                  rhs_list_max_raw = src->list_int_max_raw > val_max
                                         ? src->list_int_max_raw
                                         : val_max;
                } else if (src->has_list_int_range) {
                  rhs_has_list_int_range = true;
                  rhs_list_min_raw = src->list_int_min_raw;
                  rhs_list_max_raw = src->list_int_max_raw;
                } else if (val_has_range) {
                  rhs_has_list_int_range = true;
                  rhs_list_min_raw = val_min;
                  rhs_list_max_raw = val_max;
                }
              }
            }
            if (src && src->is_dict_storage) {
              rhs_dict_storage = true;
              rhs_int_dict_storage =
                  src->is_int_dict_storage &&
                  ny_is_proven_int(cg, scopes, *depth, set_value, NULL);
              if (rhs_int_dict_storage) {
                int64_t val_min = 0, val_max = 0;
                bool val_has_range = stmt_expr_int_range(
                    cg, scopes, *depth, set_value, &val_min, &val_max);
                if (src->has_dict_int_range && val_has_range) {
                  rhs_has_dict_int_range = true;
                  rhs_dict_min_raw = src->dict_int_min_raw < val_min
                                         ? src->dict_int_min_raw
                                         : val_min;
                  rhs_dict_max_raw = src->dict_int_max_raw > val_max
                                         ? src->dict_int_max_raw
                                         : val_max;
                } else if (val_has_range) {
                  rhs_has_dict_int_range = true;
                  rhs_dict_min_raw = val_min;
                  rhs_dict_max_raw = val_max;
                }
              }
            }
          }
        }
      }
      if (bind_direct) {
        bool is_f64_direct = false;
        bool is_int_direct = false;
        if (direct_native_float_candidate) {
          target_val = gen_expr_as_f64(cg, scopes, *depth, expr_for_check);
          is_f64_direct = true;
        } else if ((stmt_type_name_is_int_value(decl_type) && rhs_proven_int) ||
                   rhs_proven_int) {
          is_int_direct = true;
        }

        scope_bind(cg, scopes, *depth, n, target_val, s, s->as.var.is_mut,
                   decl_type, false);
        size_t nlen = strlen(n);
        binding *b = stmt_lookup_binding_no_mark(scopes, *depth, n, nlen, 0);
        if (b) {
          if (!decl_type_explicit)
            b->decl_type_name = NULL;
          stmt_update_numeric_binding_proof(cg, b, is_int_direct,
                                            is_f64_direct);
          stmt_update_int_binding_range(b, rhs_has_int_range, rhs_int_min_raw,
                                        rhs_int_max_raw);
          stmt_update_list_binding_proof(b, rhs_list_storage,
                                         rhs_int_list_storage,
                                         rhs_f64_list_storage);
          stmt_update_dict_binding_proof(b, rhs_dict_storage,
                                         rhs_int_dict_storage);
          stmt_update_list_binding_range(b, rhs_has_list_int_range,
                                         rhs_list_min_raw, rhs_list_max_raw);
          stmt_update_list_binding_len_min(b, rhs_has_list_len_min,
                                           rhs_list_len_min_raw);
          stmt_update_dict_binding_range(b, rhs_has_dict_int_range,
                                         rhs_dict_min_raw, rhs_dict_max_raw);
          stmt_update_direct_callable_binding(cg, b, expr_for_check);
          if (first_static_list_object_elided && i == 0)
            b->static_indexable_object_elided = true;
          if (first_static_list_elide_candidate && i == 0)
            stmt_update_static_int_list_elide_metadata(
                b, true, first_static_list_object_elided,
                first_static_list_elide_bail_reason);
          if (first_raw_int_list_mutation && i == 0)
            stmt_init_raw_int_list_storage(cg, scopes, *depth, b,
                                           expr_for_check);
          else if (first_raw_int_list_bail_reason && i == 0)
            b->raw_int_list_bail_reason = first_raw_int_list_bail_reason;
          stmt_ownership_post_store(cg, scopes, *depth, b, expr_for_check,
                                    s->tok, false);
        }

        continue;
      }
      if (resolved_local && !resolved_local->is_slot) {
        stmt_ownership_pre_store(cg, scopes, *depth, resolved_local,
                                 expr_for_check, s->tok);
        stmt_invalidate_static_indexable(resolved_local);
        stmt_update_numeric_binding_proof(cg, resolved_local, rhs_proven_int,
                                          rhs_proven_f64);
        stmt_update_int_binding_range(resolved_local, rhs_has_int_range,
                                      rhs_int_min_raw, rhs_int_max_raw);
        stmt_update_list_binding_proof(resolved_local, rhs_list_storage,
                                       rhs_int_list_storage,
                                       rhs_f64_list_storage);
        stmt_update_dict_binding_proof(resolved_local, rhs_dict_storage,
                                       rhs_int_dict_storage);
        stmt_update_list_binding_range(resolved_local, rhs_has_list_int_range,
                                       rhs_list_min_raw, rhs_list_max_raw);
        stmt_update_list_binding_len_min(resolved_local, rhs_has_list_len_min,
                                         rhs_list_len_min_raw);
        stmt_update_dict_binding_range(resolved_local, rhs_has_dict_int_range,
                                       rhs_dict_min_raw, rhs_dict_max_raw);
        stmt_update_direct_callable_binding(cg, resolved_local,
                                            expr_for_check);
        if (first_static_list_object_elided && i == 0)
          resolved_local->static_indexable_object_elided = true;
        if (first_static_list_elide_candidate && i == 0)
          stmt_update_static_int_list_elide_metadata(
              resolved_local, true, first_static_list_object_elided,
              first_static_list_elide_bail_reason);
        if (first_raw_int_list_mutation && i == 0)
          stmt_init_raw_int_list_storage(cg, scopes, *depth, resolved_local,
                                         expr_for_check);
        else if (first_raw_int_list_bail_reason && i == 0)
          resolved_local->raw_int_list_bail_reason =
              first_raw_int_list_bail_reason;
        resolved_local->value = target_val;
        stmt_ownership_post_store(cg, scopes, *depth, resolved_local,
                                  expr_for_check, s->tok, false);

        continue;
      }
      if (resolved_local) {
        stmt_invalidate_static_indexable(resolved_local);
        stmt_update_numeric_binding_proof(cg, resolved_local, rhs_proven_int,
                                          rhs_proven_f64);
        stmt_update_int_binding_range(resolved_local, rhs_has_int_range,
                                      rhs_int_min_raw, rhs_int_max_raw);
        stmt_update_list_binding_proof(resolved_local, rhs_list_storage,
                                       rhs_int_list_storage,
                                       rhs_f64_list_storage);
        stmt_update_dict_binding_proof(resolved_local, rhs_dict_storage,
                                       rhs_int_dict_storage);
        stmt_update_list_binding_range(resolved_local, rhs_has_list_int_range,
                                       rhs_list_min_raw, rhs_list_max_raw);
        stmt_update_list_binding_len_min(resolved_local, rhs_has_list_len_min,
                                         rhs_list_len_min_raw);
        stmt_update_dict_binding_range(resolved_local, rhs_has_dict_int_range,
                                       rhs_dict_min_raw, rhs_dict_max_raw);
        stmt_update_direct_callable_binding(cg, resolved_local,
                                            expr_for_check);
        if (first_static_list_object_elided && i == 0)
          resolved_local->static_indexable_object_elided = true;
        if (first_static_list_elide_candidate && i == 0)
          stmt_update_static_int_list_elide_metadata(
              resolved_local, true, first_static_list_object_elided,
              first_static_list_elide_bail_reason);
        if (first_raw_int_list_mutation && i == 0)
          stmt_init_raw_int_list_storage(cg, scopes, *depth, resolved_local,
                                         expr_for_check);
        else if (first_raw_int_list_bail_reason && i == 0)
          resolved_local->raw_int_list_bail_reason =
              first_raw_int_list_bail_reason;
      } else if (resolved_global) {
        stmt_invalidate_static_indexable(resolved_global);
        stmt_update_numeric_binding_proof(cg, resolved_global, rhs_proven_int,
                                          rhs_proven_f64);
        stmt_update_int_binding_range(resolved_global, rhs_has_int_range,
                                      rhs_int_min_raw, rhs_int_max_raw);
        stmt_update_list_binding_proof(resolved_global, rhs_list_storage,
                                       rhs_int_list_storage,
                                       rhs_f64_list_storage);
        stmt_update_dict_binding_proof(resolved_global, rhs_dict_storage,
                                       rhs_int_dict_storage);
        stmt_update_list_binding_range(resolved_global, rhs_has_list_int_range,
                                       rhs_list_min_raw, rhs_list_max_raw);
        stmt_update_list_binding_len_min(resolved_global, rhs_has_list_len_min,
                                         rhs_list_len_min_raw);
        stmt_update_dict_binding_range(resolved_global, rhs_has_dict_int_range,
                                       rhs_dict_min_raw, rhs_dict_max_raw);
        stmt_update_direct_callable_binding(cg, resolved_global,
                                            expr_for_check);
        if (first_static_list_object_elided && i == 0)
          resolved_global->static_indexable_object_elided = true;
        if (first_static_list_elide_candidate && i == 0)
          stmt_update_static_int_list_elide_metadata(
              resolved_global, true, first_static_list_object_elided,
              first_static_list_elide_bail_reason);
        if (first_raw_int_list_mutation && i == 0)
          stmt_init_raw_int_list_storage(cg, scopes, *depth, resolved_global,
                                         expr_for_check);
        else if (first_raw_int_list_bail_reason && i == 0)
          resolved_global->raw_int_list_bail_reason =
              first_raw_int_list_bail_reason;
      } else if (slot) {
        size_t nlen = strlen(n);
        binding *created =
            stmt_lookup_binding_no_mark(scopes, *depth, n, nlen, 0);
        if (created) {
          stmt_update_numeric_binding_proof(cg, created, rhs_proven_int,
                                            rhs_proven_f64);
          stmt_update_int_binding_range(created, rhs_has_int_range,
                                        rhs_int_min_raw, rhs_int_max_raw);
          stmt_update_list_binding_proof(created, rhs_list_storage,
                                         rhs_int_list_storage,
                                         rhs_f64_list_storage);
          stmt_update_dict_binding_proof(created, rhs_dict_storage,
                                         rhs_int_dict_storage);
          stmt_update_list_binding_range(created, rhs_has_list_int_range,
                                         rhs_list_min_raw, rhs_list_max_raw);
          stmt_update_list_binding_len_min(created, rhs_has_list_len_min,
                                           rhs_list_len_min_raw);
          stmt_update_dict_binding_range(created, rhs_has_dict_int_range,
                                         rhs_dict_min_raw, rhs_dict_max_raw);
          stmt_update_direct_callable_binding(cg, created, expr_for_check);
          if (first_static_list_object_elided && i == 0)
            created->static_indexable_object_elided = true;
          if (first_static_list_elide_candidate && i == 0)
            stmt_update_static_int_list_elide_metadata(
                created, true, first_static_list_object_elided,
                first_static_list_elide_bail_reason);
          if (first_raw_int_list_mutation && i == 0)
            stmt_init_raw_int_list_storage(cg, scopes, *depth, created,
                                           expr_for_check);
          else if (first_raw_int_list_bail_reason && i == 0)
            created->raw_int_list_bail_reason = first_raw_int_list_bail_reason;
        }
      }
      binding *own_dest = resolved_local ? resolved_local : resolved_global;
      bool own_dest_global = resolved_global != NULL;
      if (!own_dest && *depth == 0 && !top_entry_local_path) {
        own_dest = lookup_global(cg, n);
        own_dest_global = own_dest != NULL;
      } else if (!own_dest && slot) {
        size_t nlen = strlen(n);
        own_dest = stmt_lookup_binding_no_mark(scopes, *depth, n, nlen, 0);
      }
      stmt_ownership_pre_store(cg, scopes, *depth, own_dest, expr_for_check,
                               s->tok);
      if (ensure_store_ready(cg, s->tok, target_val, slot, "NY_S_VAR")) {
        if (target_is_f64_slot || target_is_f32_slot) {
          LLVMValueRef fv = NULL;
          LLVMTypeRef target_type =
              target_is_f32_slot ? cg->type_f32 : cg->type_f64;
          if (target_val && LLVMTypeOf(target_val) == target_type) {
            fv = target_val;
          } else {
            fv = ny_unbox_float(cg, target_val);
            if (target_is_f32_slot)
              fv = LLVMBuildFPTrunc(cg->builder, fv, cg->type_f32, "f2f");
          }
          ny_store(cg, slot, fv);

        } else {
          ny_store(cg, slot, target_val);
          if (own_dest && own_dest->is_int_slot)
            stmt_store_raw_int_shadow_expr(cg, scopes, *depth, own_dest,
                                           expr_for_check, target_val);
        }
        stmt_ownership_post_store(cg, scopes, *depth, own_dest, expr_for_check,
                                  s->tok, own_dest_global);
      }
    }
    free(parallel_assign_values);
    break;
  }
  case NY_S_EXPR: {
    expr_t *e = s->as.expr.expr;
    if (!e) {
      ny_diag_error(s->tok, "missing expression statement payload");
      cg->had_error = 1;
      return;
    }
    if (stmt_expr_is_unassigned_append_call(e) &&
        ny_diag_should_emit("unused_append_result", s->tok, "append")) {
      ny_diag_warning_code(
          s->tok, 2001,
          "result of 'append' is unused; list append returns a new list");
      ny_diag_hint("assign the returned list, e.g. xs = xs.append(value)");
    }
    if (e->kind == NY_E_CALL && e->as.call.callee &&
        e->as.call.callee->kind == NY_E_IDENT) {
      fun_sig *sig = lookup_fun(cg, e->as.call.callee->as.ident.name,
                                e->as.call.callee->as.ident.hash);
      if (sig && sig->return_type && (size_t)sig->return_type > 0x1000) {
        if (strcmp(sig->return_type, "Result") == 0 ||
            strcmp(sig->return_type, "std.core.error.Result") == 0) {
          if (ny_diag_should_emit("unused_result", s->tok, sig->name)) {
            bool strict_err = ny_strict_error_enabled(cg, s->tok);
            if (strict_err)
              ny_diag_error(s->tok, "unused Result from '%s'", sig->name);
            else
              ny_diag_warning(s->tok, "unused Result from '%s'", sig->name);
            ny_diag_hint("handle the value (e.g. with match/unwrap/propagate)");
            if (strict_err) {
              cg->had_error = 1;
              return;
            }
          }
        }
      }
    }
    if (is_tail && !cg->result_store_val && cg->current_fn_ret_type) {
      ensure_expr_type_compatible(cg, scopes, *depth, cg->current_fn_ret_type,
                                  e, e->tok, "return");
      if (cg->had_error)
        return;
      if (!cg->current_fn_attr_naked && !cg->result_store_val &&
          cg->current_fn_native_abi &&
          stmt_type_is_native_float(cg->current_fn_ret_type) &&
          stmt_expr_is_native_float_tail_safe(e)) {
        LLVMValueRef v = stmt_gen_return_value(cg, scopes, *depth, e,
                                               cg->current_fn_ret_type);
        if (!v) {
          ny_diag_error(s->tok, "failed to generate expression");
          cg->had_error = 1;
          return;
        }
        ny_owner_state_t old_owner_state = NY_OWNER_BORROWED;
        binding *return_owner = stmt_ownership_begin_return_transfer(
            cg, scopes, *depth, e, &old_owner_state);
        emit_defers(cg, scopes, *depth, func_root);
        stmt_ownership_end_return_transfer(return_owner, old_owner_state);
        if (!ny_has_terminator(cg)) {
          if (!emit_active_panic_env_clear(cg, s->tok))
            return;
          ny_cg_emit_trace_return(cg, v, cg->current_fn_ret_type);
          ny_cg_emit_trace_exit(cg);
          LLVMBuildRet(cg->builder, v);
        }
        break;
      }
    }
    bool prev_detach_stmt_call = cg->thread_detach_stmt_call;
    if (!is_tail && stmt_is_direct_thread_attr_call(cg, e))
      cg->thread_detach_stmt_call = true;
    LLVMValueRef v =
        is_tail ? stmt_gen_expr_with_tail_call_hint(cg, scopes, *depth, e)
                : gen_expr(cg, scopes, *depth, e);
    cg->thread_detach_stmt_call = prev_detach_stmt_call;
    if (!v) {
      ny_diag_error(s->tok, "failed to generate expression");
      cg->had_error = 1;
      return;
    }
    if (!is_tail && stmt_expr_is_noreturn_call(cg, scopes, *depth, e)) {
      LLVMBuildUnreachable(cg->builder);
      break;
    }
    if (cg->ownership_enabled) {
      expr_t *rel = stmt_ownership_releases_arg(cg, e);
      expr_t *forget = stmt_ownership_forgets_arg(cg, e);
      if (rel || forget)
        stmt_ownership_release_source(cg, scopes, *depth, rel ? rel : forget,
                                      forget != NULL);
      else {
        stmt_ownership_warn_use_after_move(cg, scopes, *depth, e);
        stmt_ownership_apply_call_contracts(cg, scopes, *depth, e);
      }
    }
    if (e->kind == NY_E_CALL && e->as.call.callee &&
        e->as.call.callee->kind == NY_E_IDENT &&
        stmt_expr_is_mutating_name(e->as.call.callee->as.ident.name) &&
        e->as.call.args.len > 0) {
      (void)stmt_expr_store_back(cg, scopes, *depth,
                                 e->as.call.args.data[0].val, v, s->tok);
    } else if (e->kind == NY_E_MEMCALL &&
               stmt_expr_is_mutating_name(e->as.memcall.name)) {
      (void)stmt_expr_store_back(cg, scopes, *depth, e->as.memcall.target, v,
                                 s->tok);
    }
    if (e->kind == NY_E_CALL && e->as.call.callee &&
        e->as.call.callee->kind == NY_E_IDENT) {
      expr_t *stored_value = NULL;
      expr_t *target = NULL;
      if (stmt_expr_is_set_idx_to_name(
              e,
              e->as.call.args.len > 0 && e->as.call.args.data[0].val &&
                      e->as.call.args.data[0].val->kind == NY_E_IDENT
                  ? e->as.call.args.data[0].val->as.ident.name
                  : NULL,
              &stored_value)) {
        target = e->as.call.args.data[0].val;
      }
      if (target && stored_value && target->kind == NY_E_IDENT &&
          target->as.ident.name) {
        size_t tlen = (size_t)target->tok.len;
        if (tlen == 0)
          tlen = strlen(target->as.ident.name);
        binding *b =
            stmt_lookup_binding(cg, scopes, *depth, target->as.ident.name, tlen,
                                target->as.ident.hash);
        if (b && b->is_slot && b->is_mut) {
          stmt_ownership_check_live_borrows(cg, scopes, *depth, b, s->tok,
                                            "mutate");
          if (ensure_store_ready(cg, s->tok, v, b->value,
                                 "indexed assignment")) {
            ny_store(cg, b->value, v);
            if (b->is_int_slot)
              stmt_store_raw_int_shadow(cg, b, v);
          }
        }
        if (b && b->is_list_storage) {
          stmt_invalidate_static_indexable(b);
          bool val_is_int =
              ny_is_proven_int(cg, scopes, *depth, stored_value, NULL);
          bool val_is_f64 =
              stmt_expr_is_f64_value(cg, scopes, *depth, stored_value);
          stmt_update_list_binding_proof(
              b, true, b->is_int_list_storage && val_is_int,
              b->is_f64_list_storage && val_is_f64);
          if (b->is_int_list_storage) {
            int64_t val_min = 0, val_max = 0;
            bool val_has_range = stmt_expr_int_range(
                cg, scopes, *depth, stored_value, &val_min, &val_max);
            if (b->has_list_int_range && val_has_range) {
              int64_t min_v =
                  b->list_int_min_raw < val_min ? b->list_int_min_raw : val_min;
              int64_t max_v =
                  b->list_int_max_raw > val_max ? b->list_int_max_raw : val_max;
              stmt_update_list_binding_range(b, true, min_v, max_v);
            } else if (val_has_range) {
              stmt_update_list_binding_range(b, true, val_min, val_max);
            } else {
              stmt_update_list_binding_range(b, false, 0, 0);
            }
          }
        }
        if (b && b->is_dict_storage) {
          bool val_is_int =
              ny_is_proven_int(cg, scopes, *depth, stored_value, NULL);
          stmt_update_dict_binding_proof(b, true,
                                         b->is_int_dict_storage && val_is_int);
          if (b->is_int_dict_storage) {
            int64_t val_min = 0, val_max = 0;
            bool val_has_range = stmt_expr_int_range(
                cg, scopes, *depth, stored_value, &val_min, &val_max);
            if (b->has_dict_int_range && val_has_range) {
              int64_t min_v =
                  b->dict_int_min_raw < val_min ? b->dict_int_min_raw : val_min;
              int64_t max_v =
                  b->dict_int_max_raw > val_max ? b->dict_int_max_raw : val_max;
              stmt_update_dict_binding_range(b, true, min_v, max_v);
            } else if (val_has_range) {
              stmt_update_dict_binding_range(b, true, val_min, val_max);
            } else {
              stmt_update_dict_binding_range(b, false, 0, 0);
            }
          }
        }
      }
    }
    if (is_tail && !cg->result_store_val &&
        stmt_type_is_native_abi_value(cg, cg->current_fn_ret_type)) {
      bool proven_int = ny_is_proven_int(cg, scopes, *depth, e, v);
      v = ny_coerce_to_abi_proven_int(cg, v, cg->current_fn_ret_type,
                                      proven_int);
    }
    if (is_tail && !cg->current_fn_attr_naked) {
      if (cg->result_store_val) {
        ny_store(cg, cg->result_store_val, v);
      } else {
        ny_owner_state_t old_owner_state = NY_OWNER_BORROWED;
        binding *return_owner = stmt_ownership_begin_return_transfer(
            cg, scopes, *depth, e, &old_owner_state);
        emit_defers(cg, scopes, *depth, func_root);
        stmt_ownership_end_return_transfer(return_owner, old_owner_state);
        if (!ny_has_terminator(cg)) {
          if (!emit_active_panic_env_clear(cg, s->tok))
            return;
          ny_cg_emit_trace_return(cg, v, cg->current_fn_ret_type);
          ny_cg_emit_trace_exit(cg);
          LLVMBuildRet(cg->builder, v);
        }
      }
    }
    break;
  }
  case NY_S_IF: {
    gen_stmt_if(cg, scopes, depth, s, func_root, is_tail);
    break;
  }
  case NY_S_GUARD: {
    gen_stmt_guard(cg, scopes, depth, s, func_root);
    break;
  }
  case NY_S_MATCH: {
    match_check_exhaustive(cg, s);
    if (cg->had_error)
      return;
    if (stmt_try_gen_const_match(cg, scopes, depth, s, func_root, is_tail))
      break;
    LLVMValueRef testv = gen_expr(cg, scopes, *depth, s->as.match.test);
    LLVMValueRef f = ny_cur_fn(cg);
    LLVMBasicBlockRef end = ny_bb_fn(f, "match_end");
    LLVMTypeRef i1 = ny_i1_ty(cg);
    bool has_end_pred = false;
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      match_arm_t *arm = &s->as.match.arms.data[i];
      LLVMBasicBlockRef arm_bb = ny_bb_fn(f, "match_arm");
      LLVMBasicBlockRef next_bb = ny_bb_fn(f, "match_next");
      LLVMValueRef cond = NULL;
      int has_wild = 0;
      scope_enter(scopes, depth, scopes[*depth].break_bb,
                  scopes[*depth].continue_bb);
      for (size_t j = 0; j < arm->patterns.len; ++j) {
        expr_t *pat = arm->patterns.data[j];
        if (ny_expr_is_wildcard_ident(pat)) {
          has_wild = 1;
          break;
        }
        LLVMValueRef pat_cond =
            match_pattern_result_cond(cg, scopes, *depth, s, testv, pat);
        if (!pat_cond) {
          LLVMValueRef pv = gen_expr(cg, scopes, *depth, pat);
          LLVMValueRef eq = gen_binary(cg, scopes, *depth, "==", testv, pv,
                                       s->as.match.test, pat);
          pat_cond = to_bool(cg, eq);
        }
        cond = cond ? ny_or(cg, cond, pat_cond, "") : pat_cond;
      }
      if (has_wild) {
        cond = LLVMConstInt(i1, 1, false);
      } else if (!cond) {
        cond = LLVMConstInt(i1, 0, false);
      }
      token_t btok = s->tok;
      if (arm->patterns.len > 0 && arm->patterns.data[0])
        btok = arm->patterns.data[0]->tok;
      ny_dbg_loc(cg, btok);
      ny_cond_br(cg, cond, arm_bb, next_bb);

      ny_pos(cg, arm_bb);

      if (arm->patterns.len == 1)
        match_bind_adt_pattern(cg, scopes, *depth, s, testv,
                               arm->patterns.data[0]);

      if (arm->guard) {
        ny_dbg_loc(cg, arm->guard->tok);
        LLVMValueRef guard_b = stmt_gen_cond_i1(cg, scopes, *depth, arm->guard);
        LLVMBasicBlockRef guard_pass_bb = ny_bb_fn(f, "match_guard_pass");
        ny_cond_br(cg, guard_b, guard_pass_bb, next_bb);

        ny_pos(cg, guard_pass_bb);
      }
      gen_stmt(cg, scopes, depth, arm->conseq, func_root, is_tail);
      if (!ny_has_terminator(cg)) {

        emit_defers(cg, scopes, *depth, *depth);
        ny_br(cg, end);
        has_end_pred = true;
      }
      scope_pop(scopes, depth);
      ny_pos(cg, next_bb);
    }
    if (s->as.match.default_conseq) {
      gen_stmt(cg, scopes, depth, s->as.match.default_conseq, func_root,
               is_tail);
      if (!ny_has_terminator(cg)) {

        ny_br(cg, end);
        has_end_pred = true;
      }
    } else {

      if (is_tail)
        LLVMBuildUnreachable(cg->builder);
      else {
        ny_br(cg, end);
        has_end_pred = true;
      }
    }
    if (has_end_pred)
      ny_pos(cg, end);
    else
      LLVMDeleteBasicBlock(end);

    break;
  }
  case NY_S_WHILE: {
    stmt_preinvalidate_loop_static_indexables(cg, scopes, *depth,
                                              s->as.whl.body);
    gen_stmt_while(cg, scopes, depth, s, func_root);
    break;
  }
  case NY_S_FOR: {
    stmt_preinvalidate_loop_static_indexables(cg, scopes, *depth,
                                              s->as.fr.body);
    gen_stmt_for(cg, scopes, depth, s, func_root);
    break;
  }
  case NY_S_RETURN: {
    if (cg->current_fn_ret_type && !s->as.ret.value) {
      const char *rt = cg->current_fn_ret_type;
      const char *dot = strrchr(rt, '.');
      const char *base = dot ? dot + 1 : rt;
      if (base && strcmp(base, "void") != 0) {
        ny_diag_error(s->tok, "missing return value for %s", base);
        cg->had_error = 1;
      }
    }
    if (cg->current_fn_ret_type && s->as.ret.value) {
      ensure_expr_type_compatible(cg, scopes, *depth, cg->current_fn_ret_type,
                                  s->as.ret.value, s->as.ret.value->tok,
                                  "return");
    }
    stmt_ownership_check_returned_borrow(cg, scopes, *depth, s->as.ret.value);
    bool mark_tail = stmt_should_mark_tail_call_expr(cg, s->as.ret.value);
    if (mark_tail)
      cg->tail_call_depth++;
    LLVMValueRef v = stmt_gen_return_value(cg, scopes, *depth, s->as.ret.value,
                                           cg->current_fn_ret_type);
    if (mark_tail && cg->tail_call_depth > 0)
      cg->tail_call_depth--;
    if (!v) {
      ny_diag_error(s->tok, "failed to generate return value");
      cg->had_error = 1;
      return;
    }
    ny_owner_state_t old_owner_state = NY_OWNER_BORROWED;
    binding *return_owner = stmt_ownership_begin_return_transfer(
        cg, scopes, *depth, s->as.ret.value, &old_owner_state);
    emit_defers(cg, scopes, *depth, func_root);
    stmt_ownership_end_return_transfer(return_owner, old_owner_state);
    if (!ny_has_terminator(cg)) {
      if (!emit_active_panic_env_clear(cg, s->tok))
        break;
      ny_cg_emit_trace_return(cg, v, cg->current_fn_ret_type);
      ny_cg_emit_trace_exit(cg);
      LLVMBuildRet(cg->builder, v);
    }
    break;
  }
  case NY_S_USE: {
    break;
  }
  case NY_S_BLOCK: {
    gen_stmt_block(cg, scopes, depth, s, func_root, is_tail);
    break;
  }
  case NY_S_TRY: {
    gen_stmt_try(cg, scopes, depth, s, func_root, is_tail);
    break;
  }
  case NY_S_FUNC:
    gen_func(cg, s, s->as.fn.name, scopes, *depth, NULL);
    break;
  case NY_S_DEFER: {
    gen_stmt_defer(cg, scopes, *depth, s);
    break;
  }
  case NY_S_BREAK: {
    gen_loop_control(cg, scopes, *depth, s->tok, false);
    break;
  }
  case NY_S_CONTINUE: {
    gen_loop_control(cg, scopes, *depth, s->tok, true);
    break;
  }
  case NY_S_STRUCT: {
    gen_stmt_struct(cg, s);
    break;
  }
  case NY_S_LAYOUT: {
    break;
  }
  case NY_S_OPERATOR:
    break;
  case NY_S_IMPL:
    break;
  case NY_S_MODULE: {
    gen_stmt_module(cg, scopes, depth, s, func_root, is_tail);
    break;
  }
  case NY_S_EXPORT:
    break;
  case NY_S_LABEL: {
    label_binding *lb = find_label_binding(cg, s->as.label.name);
    if (lb) {
      if (!ny_has_terminator(cg)) {
        ny_br(cg, lb->bb);
      }
      ny_pos(cg, lb->bb);
      return;
    }
    break;
  }
  case NY_S_GOTO: {
    label_binding *lb = find_label_binding(cg, s->as.go.name);
    if (lb) {
      size_t target_depth = lb->depth;
      if (*depth < target_depth) {
        ny_diag_error(s->tok,
                      "goto cannot enter label \033[1;37m'%s'\033[0m from an outer scope",
                      s->as.go.name);
        ny_diag_hint("move the label to the current scope or use structured control flow");
        cg->had_error = 1;
        break;
      }
      if (*depth > target_depth) {
        emit_defers(cg, scopes, *depth, target_depth + 1);
      }

      ny_br(cg, lb->bb);
      LLVMValueRef f = ny_cur_fn(cg);
      ny_pos(cg, ny_bb_fn(f, "dead"));
      return;
    }
    ny_diag_error(s->tok, "undefined label \033[1;37m'%s'\033[0m",
                  s->as.go.name);
    cg->had_error = 1;
    break;
  }
  case NY_S_MACRO: {
    const char *macro_name = (s->as.macro.name && *s->as.macro.name)
                                 ? s->as.macro.name
                                 : "<unknown>";
    for (size_t i = 0; i < s->as.macro.args.len; i++) {
      expr_t *arg = s->as.macro.args.data[i];
      if (!arg)
        continue;
      (void)gen_expr(cg, scopes, *depth, arg);
    }
    if (s->as.macro.body) {
      ny_diag_warning(s->tok,
                      "macro statement '%s' fell back to direct body execution",
                      macro_name);
      if (verbose_enabled >= 1) {
        ny_diag_hint(
            "register '%s' in std.core.syntax to customize expansion behavior",
            macro_name);
      }
      gen_stmt(cg, scopes, depth, s->as.macro.body, func_root, is_tail);
      break;
    }
    ny_diag_warning(s->tok, "macro statement not implemented: '%s'",
                    macro_name);
    if (verbose_enabled >= 1) {
      ny_diag_hint("register '%s' in std.core.syntax to implement this macro",
                   macro_name);
    }
    break;
  }
  default:
    break;
  }
}

void gen_stmt(codegen_t *cg, scope *scopes, size_t *depth, stmt_t *s,
              size_t func_root, bool is_tail) {
  if (!ny_codegen_stmt_kind_profile_enabled()) {
    gen_stmt_inner(cg, scopes, depth, s, func_root, is_tail);
    return;
  }
  if (!g_stmt_kind_profile_registered) {
    atexit(ny_codegen_stmt_kind_profile_report);
    g_stmt_kind_profile_registered = 1;
  }
  int kind = s ? (int)s->kind : 63;
  int slot = g_stmt_kind_depth;
  if (slot >= 0 && slot < (int)(sizeof(g_stmt_kind_child_ms) /
                                sizeof(g_stmt_kind_child_ms[0])))
    g_stmt_kind_child_ms[slot] = 0.0;
  g_stmt_kind_depth++;
  ny_tick_t start = ny_ticks_now();
  gen_stmt_inner(cg, scopes, depth, s, func_root, is_tail);
  double total_ms = ny_ticks_elapsed_ms(start);
  g_stmt_kind_depth--;
  double child_ms = 0.0;
  if (slot >= 0 && slot < (int)(sizeof(g_stmt_kind_child_ms) /
                                sizeof(g_stmt_kind_child_ms[0])))
    child_ms = g_stmt_kind_child_ms[slot];
  if (g_stmt_kind_depth > 0) {
    int parent = g_stmt_kind_depth - 1;
    if (parent >= 0 && parent < (int)(sizeof(g_stmt_kind_child_ms) /
                                      sizeof(g_stmt_kind_child_ms[0])))
      g_stmt_kind_child_ms[parent] += total_ms;
  }
  ny_codegen_stmt_kind_profile_add(kind, total_ms, child_ms);
}
