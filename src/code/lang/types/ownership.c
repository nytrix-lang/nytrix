/*
 * Canonical ownership implementation for Nytrix codegen. Static rules and
 * runtime ownership hooks are kept together so every backend uses one model.
 */
#include "code/lang/types/ownership.h"
#include "base/util.h"
#include "code/backend/llvm/bridge.h"
#include "code/lang/types/nullnarrow.h"

#include <inttypes.h>
#include <llvm-c/Core.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static expr_t *stmt_ownership_return_borrow_arg(codegen_t *cg,
                                                expr_t *call_expr);
expr_t *stmt_ownership_releases_arg(codegen_t *cg, expr_t *call_expr);
expr_t *stmt_ownership_forgets_arg(codegen_t *cg, expr_t *call_expr);
static expr_t *stmt_ownership_consumes_arg(codegen_t *cg, expr_t *call_expr);
static bool stmt_ownership_binding_is_immediate(binding *b);

bool stmt_expr_is_mutating_name(const char *name) {
  static const char *const k_names[] = {
      "add",  "append", "extend", "sub", "remove", "sort", "clear",
      "push", "insert", "set",    "put", "delete", "del",  NULL};
  if (!name)
    return false;
  for (size_t i = 0; k_names[i]; ++i) {
    if (ny_name_tail_is(name, k_names[i]))
      return true;
  }
  return false;
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

static bool stmt_expr_is_adt_ctor(codegen_t *cg, expr_t *e) {
  char *name = ny_adt_member_call_full_name(cg, e);
  if (!name)
    return false;
  enum_def_t *owner = NULL;
  enum_member_def_t *mem = lookup_enum_member_owner(cg, name, &owner);
  return mem && owner && mem->has_payload;
}

void stmt_ownership_diag(codegen_t *cg, token_t tok, const char *fmt, ...) {
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
  return lookup_binding_hash(cg, scopes, depth, e->as.ident.name, len,
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
      return stmt_ownership_root_binding(cg, scopes, depth,
                                         e->as.memcall.target);
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
      return lookup_binding_hash(cg, scopes, depth, b->ownership_borrow_source,
                                 strlen(b->ownership_borrow_source),
                                 b->ownership_borrow_source_hash);
  }
  return NULL;
}

void stmt_ownership_check_returned_borrow(codegen_t *cg, scope *scopes,
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

expr_t *stmt_ownership_releases_arg(codegen_t *cg, expr_t *call_expr) {
  fun_sig *sig = stmt_ownership_resolve_call_sig(cg, call_expr);
  expr_t *arg =
      sig ? stmt_ownership_arg_for_contract(cg, call_expr, &sig->releases)
          : NULL;
  if (arg)
    return arg;
  return stmt_ownership_unary_arg(call_expr, "release");
}

expr_t *stmt_ownership_forgets_arg(codegen_t *cg, expr_t *call_expr) {
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

void stmt_ownership_apply_call_contracts(codegen_t *cg, scope *scopes,
                                         size_t depth, expr_t *call_expr) {
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

void stmt_ownership_warn_use_after_move(codegen_t *cg, scope *scopes,
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
    stmt_ownership_diag(cg, tok,
                        "ownership cleanup requires __drop_owned; import core");
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

void stmt_ownership_register_slot_defer(codegen_t *cg, scope *scopes,
                                        size_t depth, binding *b) {
  if (!cg || !cg->ownership_enabled || !cg->ownership_runtime_cleanup ||
      !scopes || !b || !b->is_slot || b->ownership_defer_registered)
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

void stmt_ownership_cleanup_scope(codegen_t *cg, scope *scopes, size_t depth) {
  if (!cg || !cg->ownership_enabled || !cg->ownership_runtime_cleanup ||
      !scopes)
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

void stmt_ownership_check_live_borrows(codegen_t *cg, scope *scopes,
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
      /*
       * Strict ownership has a whole-function, last-use check for direct
       * scalar borrow aliases.  Do not let the older scope-wide runtime
       * cleanup guard turn those proven-dead aliases into false positives.
       * Keep its conservative behavior outside strict mode and for non-scalar
       * or indirect aliases, which still need the fallback guard.
       */
      if (cg->ownership_strict && stmt_ownership_binding_is_immediate(source))
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

void stmt_ownership_release_source(codegen_t *cg, scope *scopes, size_t depth,
                                   expr_t *arg, bool forgotten) {
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

void stmt_ownership_pre_store(codegen_t *cg, scope *scopes, size_t depth,
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

void stmt_ownership_post_store(codegen_t *cg, scope *scopes, size_t depth,
                               binding *dest, expr_t *rhs, token_t tok,
                               bool target_global) {
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
    alloc_size_known = stmt_ownership_alloc_size_bytes(cg, scopes, depth, rhs,
                                                       &alloc_size_raw);
  }
  bool explicit_own = stmt_call_tail_is(rhs, "own");
  if (target_global && !explicit_own &&
      ny_diag_should_emit("ownership_escape_global", tok, dest->name)) {
    stmt_ownership_diag(
        cg, tok,
        "owned heap value stored in global '%s' may escape ownership cleanup",
        dest->name);
    ny_diag_fix(
        "wrap process-lifetime storage in own(...), or keep it local and "
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

binding *stmt_ownership_begin_return_transfer(codegen_t *cg, scope *scopes,
                                              size_t depth, expr_t *value,
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

void stmt_ownership_end_return_transfer(binding *b,
                                        ny_owner_state_t old_state) {
  if (b)
    b->owner_state = old_state;
}

static size_t stmt_ownership_count_uses_expr(const expr_t *e,
                                             const char *param_name);
static size_t stmt_ownership_count_uses_stmt_internal(const stmt_t *s,
                                                      const char *param_name,
                                                      bool *out_reassigned);

static size_t stmt_ownership_count_uses_block(const stmt_block_t *blk,
                                              const char *param_name,
                                              bool *out_reassigned) {
  if (!blk)
    return 0;
  size_t total = 0;
  for (size_t i = 0; i < blk->body.len; i++) {
    const stmt_t *st = blk->body.data[i];
    if (!st)
      continue;
    bool st_reassigned = false;
    total +=
        stmt_ownership_count_uses_stmt_internal(st, param_name, &st_reassigned);
    if (st_reassigned) {
      if (out_reassigned)
        *out_reassigned = true;
      break;
    }
  }
  return total;
}

static size_t stmt_ownership_count_uses_expr(const expr_t *e,
                                             const char *param_name) {
  if (!e || !param_name)
    return 0;
  switch (e->kind) {
  case NY_E_IDENT:
    if (e->as.ident.name && strcmp(e->as.ident.name, param_name) == 0)
      return 1;
    return 0;
  case NY_E_LITERAL:
  case NY_E_PTR_TYPE:
  case NY_E_SIZEOF:
    return 0;
  case NY_E_UNARY:
    return stmt_ownership_count_uses_expr(e->as.unary.right, param_name);
  case NY_E_BINARY:
    if (e->as.binary.op && strcmp(e->as.binary.op, "=") == 0)
      return stmt_ownership_count_uses_expr(e->as.binary.right, param_name);
    return stmt_ownership_count_uses_expr(e->as.binary.left, param_name) +
           stmt_ownership_count_uses_expr(e->as.binary.right, param_name);
  case NY_E_LOGICAL:
    return stmt_ownership_count_uses_expr(e->as.logical.left, param_name) +
           stmt_ownership_count_uses_expr(e->as.logical.right, param_name);
  case NY_E_TERNARY: {
    size_t c = stmt_ownership_count_uses_expr(e->as.ternary.cond, param_name);
    size_t t =
        stmt_ownership_count_uses_expr(e->as.ternary.true_expr, param_name);
    size_t f =
        stmt_ownership_count_uses_expr(e->as.ternary.false_expr, param_name);
    return c + (t > f ? t : f);
  }
  case NY_E_CALL: {
    size_t total =
        stmt_ownership_count_uses_expr(e->as.call.callee, param_name);
    for (size_t i = 0; i < e->as.call.args.len; i++)
      total += stmt_ownership_count_uses_expr(e->as.call.args.data[i].val,
                                              param_name);
    return total;
  }
  case NY_E_MEMCALL: {
    size_t total =
        stmt_ownership_count_uses_expr(e->as.memcall.target, param_name);
    for (size_t i = 0; i < e->as.memcall.args.len; i++)
      total += stmt_ownership_count_uses_expr(e->as.memcall.args.data[i].val,
                                              param_name);
    return total;
  }
  case NY_E_INDEX: {
    size_t total =
        stmt_ownership_count_uses_expr(e->as.index.target, param_name);
    total += stmt_ownership_count_uses_expr(e->as.index.start, param_name);
    if (e->as.index.stop)
      total += stmt_ownership_count_uses_expr(e->as.index.stop, param_name);
    if (e->as.index.step)
      total += stmt_ownership_count_uses_expr(e->as.index.step, param_name);
    return total;
  }
  case NY_E_MEMBER:
    return stmt_ownership_count_uses_expr(e->as.member.target, param_name);
  case NY_E_DEREF:
    return stmt_ownership_count_uses_expr(e->as.deref.target, param_name);
  case NY_E_TRY:
    return stmt_ownership_count_uses_expr(e->as.try_expr.target, param_name);
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET: {
    size_t total = 0;
    for (size_t i = 0; i < e->as.list_like.len; i++)
      total +=
          stmt_ownership_count_uses_expr(e->as.list_like.data[i], param_name);
    return total;
  }
  case NY_E_DICT: {
    size_t total = 0;
    for (size_t i = 0; i < e->as.dict.pairs.len; i++) {
      total += stmt_ownership_count_uses_expr(e->as.dict.pairs.data[i].key,
                                              param_name);
      total += stmt_ownership_count_uses_expr(e->as.dict.pairs.data[i].value,
                                              param_name);
    }
    return total;
  }
  case NY_E_FSTRING: {
    size_t total = 0;
    for (size_t i = 0; i < e->as.fstring.parts.len; i++) {
      if (e->as.fstring.parts.data[i].kind == NY_FSP_EXPR)
        total += stmt_ownership_count_uses_expr(
            e->as.fstring.parts.data[i].as.e, param_name);
    }
    return total;
  }
  case NY_E_MATCH: {
    size_t total = stmt_ownership_count_uses_expr(e->as.match.test, param_name);
    size_t max_arm = 0;
    for (size_t i = 0; i < e->as.match.arms.len; i++) {
      const match_arm_t *arm = &e->as.match.arms.data[i];
      size_t arm_uses = 0;
      if (arm->guard)
        arm_uses += stmt_ownership_count_uses_expr(arm->guard, param_name);
      if (arm->conseq)
        arm_uses += stmt_ownership_count_uses_stmt_internal(arm->conseq,
                                                            param_name, NULL);
      if (arm_uses > max_arm)
        max_arm = arm_uses;
    }
    if (e->as.match.default_conseq) {
      size_t def_uses = stmt_ownership_count_uses_stmt_internal(
          e->as.match.default_conseq, param_name, NULL);
      if (def_uses > max_arm)
        max_arm = def_uses;
    }
    return total + max_arm;
  }
  case NY_E_LAMBDA:
  case NY_E_FN: {
    /*
     * Closure construction demand != latent invocation demand:
     * If the closure captures param_name, construction demand is 1.
     */
    bool shadowed = false;
    for (size_t i = 0; i < e->as.lambda.params.len; i++) {
      if (e->as.lambda.params.data[i].name &&
          strcmp(e->as.lambda.params.data[i].name, param_name) == 0) {
        shadowed = true;
        break;
      }
    }
    if (!shadowed && e->as.lambda.body) {
      size_t inner = stmt_ownership_count_uses_stmt_internal(e->as.lambda.body,
                                                             param_name, NULL);
      if (inner > 0)
        return 1;
    }
    return 0;
  }
  case NY_E_COMPTIME:
    if (e->as.comptime_expr.body)
      return stmt_ownership_count_uses_stmt_internal(e->as.comptime_expr.body,
                                                     param_name, NULL);
    return 0;
  case NY_E_QUOTE: {
    size_t total = 0;
    if (e->as.quote.body)
      total += stmt_ownership_count_uses_stmt_internal(e->as.quote.body,
                                                       param_name, NULL);
    if (e->as.quote.expr)
      total += stmt_ownership_count_uses_expr(e->as.quote.expr, param_name);
    return total;
  }
  case NY_E_SPLICE:
    if (e->as.splice.expr)
      return stmt_ownership_count_uses_expr(e->as.splice.expr, param_name);
    return 0;
  case NY_E_ASM: {
    size_t total = 0;
    for (size_t i = 0; i < e->as.as_asm.args.len; i++)
      total +=
          stmt_ownership_count_uses_expr(e->as.as_asm.args.data[i], param_name);
    return total;
  }
  default:
    return 0;
  }
}

static size_t stmt_ownership_count_uses_stmt_internal(const stmt_t *s,
                                                      const char *param_name,
                                                      bool *out_reassigned) {
  if (!s || !param_name)
    return 0;
  switch (s->kind) {
  case NY_S_BLOCK:
    return stmt_ownership_count_uses_block(&s->as.block, param_name,
                                           out_reassigned);
  case NY_S_EXPR:
    if (s->as.expr.expr && s->as.expr.expr->kind == NY_E_BINARY &&
        s->as.expr.expr->as.binary.op &&
        strcmp(s->as.expr.expr->as.binary.op, "=") == 0 &&
        s->as.expr.expr->as.binary.left &&
        s->as.expr.expr->as.binary.left->kind == NY_E_IDENT &&
        s->as.expr.expr->as.binary.left->as.ident.name &&
        strcmp(s->as.expr.expr->as.binary.left->as.ident.name, param_name) ==
            0) {
      if (out_reassigned)
        *out_reassigned = true;
      return stmt_ownership_count_uses_expr(s->as.expr.expr->as.binary.right,
                                            param_name);
    }
    return stmt_ownership_count_uses_expr(s->as.expr.expr, param_name);
  case NY_S_RETURN:
    if (s->as.ret.value)
      return stmt_ownership_count_uses_expr(s->as.ret.value, param_name);
    return 0;
  case NY_S_VAR: {
    bool modifies_param = false;
    for (size_t i = 0; i < s->as.var.names.len; i++) {
      if (s->as.var.names.data[i] &&
          strcmp(s->as.var.names.data[i], param_name) == 0) {
        modifies_param = true;
        break;
      }
    }
    size_t total = 0;
    for (size_t i = 0; i < s->as.var.exprs.len; i++) {
      total +=
          stmt_ownership_count_uses_expr(s->as.var.exprs.data[i], param_name);
    }
    if (modifies_param && out_reassigned)
      *out_reassigned = true;
    return total;
  }
  case NY_S_IF: {
    bool then_reassigned = false;
    bool else_reassigned = false;
    size_t init_u = s->as.iff.init ? stmt_ownership_count_uses_stmt_internal(
                                         s->as.iff.init, param_name, NULL)
                                   : 0;
    size_t test_u = stmt_ownership_count_uses_expr(s->as.iff.test, param_name);
    size_t conseq_u = s->as.iff.conseq
                          ? stmt_ownership_count_uses_stmt_internal(
                                s->as.iff.conseq, param_name, &then_reassigned)
                          : 0;
    size_t alt_u = s->as.iff.alt
                       ? stmt_ownership_count_uses_stmt_internal(
                             s->as.iff.alt, param_name, &else_reassigned)
                       : 0;
    if (then_reassigned && else_reassigned && out_reassigned)
      *out_reassigned = true;
    return init_u + test_u + (conseq_u > alt_u ? conseq_u : alt_u);
  }
  case NY_S_MATCH: {
    size_t total = stmt_ownership_count_uses_expr(s->as.match.test, param_name);
    size_t max_arm = 0;
    for (size_t i = 0; i < s->as.match.arms.len; i++) {
      const match_arm_t *arm = &s->as.match.arms.data[i];
      size_t arm_uses = 0;
      if (arm->guard)
        arm_uses += stmt_ownership_count_uses_expr(arm->guard, param_name);
      if (arm->conseq)
        arm_uses += stmt_ownership_count_uses_stmt_internal(arm->conseq,
                                                            param_name, NULL);
      if (arm_uses > max_arm)
        max_arm = arm_uses;
    }
    if (s->as.match.default_conseq) {
      size_t def_uses = stmt_ownership_count_uses_stmt_internal(
          s->as.match.default_conseq, param_name, NULL);
      if (def_uses > max_arm)
        max_arm = def_uses;
    }
    return total + max_arm;
  }
  case NY_S_WHILE: {
    size_t init_u = s->as.whl.init ? stmt_ownership_count_uses_stmt_internal(
                                         s->as.whl.init, param_name, NULL)
                                   : 0;
    size_t test_u = stmt_ownership_count_uses_expr(s->as.whl.test, param_name);
    size_t body_u = s->as.whl.body ? stmt_ownership_count_uses_stmt_internal(
                                         s->as.whl.body, param_name, NULL)
                                   : 0;
    size_t update_u = s->as.whl.update
                          ? stmt_ownership_count_uses_stmt_internal(
                                s->as.whl.update, param_name, NULL)
                          : 0;
    return init_u + test_u + body_u + update_u;
  }
  case NY_S_FOR: {
    size_t init_u = s->as.fr.init ? stmt_ownership_count_uses_stmt_internal(
                                        s->as.fr.init, param_name, NULL)
                                  : 0;
    size_t cond_u =
        s->as.fr.cond
            ? stmt_ownership_count_uses_expr(s->as.fr.cond, param_name)
            : 0;
    size_t iter_u =
        s->as.fr.iterable
            ? stmt_ownership_count_uses_expr(s->as.fr.iterable, param_name)
            : 0;
    size_t body_u = s->as.fr.body ? stmt_ownership_count_uses_stmt_internal(
                                        s->as.fr.body, param_name, NULL)
                                  : 0;
    size_t update_u = s->as.fr.update ? stmt_ownership_count_uses_stmt_internal(
                                            s->as.fr.update, param_name, NULL)
                                      : 0;
    return init_u + cond_u + iter_u + body_u + update_u;
  }
  case NY_S_GUARD: {
    size_t val_u =
        s->as.guard.value
            ? stmt_ownership_count_uses_expr(s->as.guard.value, param_name)
            : 0;
    size_t fall_u = s->as.guard.fallback
                        ? stmt_ownership_count_uses_stmt_internal(
                              s->as.guard.fallback, param_name, NULL)
                        : 0;
    return val_u + fall_u;
  }
  case NY_S_TRY: {
    size_t body_u = s->as.tr.body ? stmt_ownership_count_uses_stmt_internal(
                                        s->as.tr.body, param_name, NULL)
                                  : 0;
    size_t hand_u = s->as.tr.handler ? stmt_ownership_count_uses_stmt_internal(
                                           s->as.tr.handler, param_name, NULL)
                                     : 0;
    return body_u > hand_u ? body_u : hand_u;
  }
  case NY_S_DEFER:
    if (s->as.de.body)
      return stmt_ownership_count_uses_stmt_internal(s->as.de.body, param_name,
                                                     NULL);
    return 0;
  default:
    return 0;
  }
}

/*
 * Least path count for allowance intervals. The existing use counter is the
 * greatest path count; pairing it with this walk makes branch joins explicit.
 */
static size_t stmt_ownership_min_uses_expr(const expr_t *e,
                                           const char *param_name);

static size_t stmt_ownership_min_uses_stmt_paths(const stmt_t *s,
                                                 const char *param_name);

static size_t stmt_ownership_min_uses_expr(const expr_t *e,
                                           const char *param_name) {
  if (!e || !param_name)
    return 0;
  switch (e->kind) {
  case NY_E_IDENT:
    return e->as.ident.name && strcmp(e->as.ident.name, param_name) == 0;
  case NY_E_UNARY:
    return stmt_ownership_min_uses_expr(e->as.unary.right, param_name);
  case NY_E_BINARY:
    if (e->as.binary.op && strcmp(e->as.binary.op, "=") == 0)
      return stmt_ownership_min_uses_expr(e->as.binary.right, param_name);
    return stmt_ownership_min_uses_expr(e->as.binary.left, param_name) +
           stmt_ownership_min_uses_expr(e->as.binary.right, param_name);
  case NY_E_LOGICAL:
    /*
     * The right operand can be skipped by short-circuit evaluation.
     */
    return stmt_ownership_min_uses_expr(e->as.logical.left, param_name);
  case NY_E_TERNARY: {
    size_t cond = stmt_ownership_min_uses_expr(e->as.ternary.cond, param_name);
    size_t yes =
        stmt_ownership_min_uses_expr(e->as.ternary.true_expr, param_name);
    size_t no =
        stmt_ownership_min_uses_expr(e->as.ternary.false_expr, param_name);
    return cond + (yes < no ? yes : no);
  }
  case NY_E_CALL: {
    size_t total = stmt_ownership_min_uses_expr(e->as.call.callee, param_name);
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      total +=
          stmt_ownership_min_uses_expr(e->as.call.args.data[i].val, param_name);
    return total;
  }
  case NY_E_MEMCALL: {
    size_t total =
        stmt_ownership_min_uses_expr(e->as.memcall.target, param_name);
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      total += stmt_ownership_min_uses_expr(e->as.memcall.args.data[i].val,
                                            param_name);
    return total;
  }
  case NY_E_INDEX:
    return stmt_ownership_min_uses_expr(e->as.index.target, param_name) +
           stmt_ownership_min_uses_expr(e->as.index.start, param_name) +
           stmt_ownership_min_uses_expr(e->as.index.stop, param_name) +
           stmt_ownership_min_uses_expr(e->as.index.step, param_name);
  case NY_E_MEMBER:
    return stmt_ownership_min_uses_expr(e->as.member.target, param_name);
  case NY_E_DEREF:
    return stmt_ownership_min_uses_expr(e->as.deref.target, param_name);
  case NY_E_TRY:
    return stmt_ownership_min_uses_expr(e->as.try_expr.target, param_name);
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET: {
    size_t total = 0;
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      total +=
          stmt_ownership_min_uses_expr(e->as.list_like.data[i], param_name);
    return total;
  }
  case NY_E_DICT: {
    size_t total = 0;
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i)
      total += stmt_ownership_min_uses_expr(e->as.dict.pairs.data[i].key,
                                            param_name) +
               stmt_ownership_min_uses_expr(e->as.dict.pairs.data[i].value,
                                            param_name);
    return total;
  }
  case NY_E_FSTRING: {
    size_t total = 0;
    for (size_t i = 0; i < e->as.fstring.parts.len; ++i)
      if (e->as.fstring.parts.data[i].kind == NY_FSP_EXPR)
        total += stmt_ownership_min_uses_expr(e->as.fstring.parts.data[i].as.e,
                                              param_name);
    return total;
  }
  case NY_E_MATCH: {
    size_t total = stmt_ownership_min_uses_expr(e->as.match.test, param_name);
    size_t min_arm = SIZE_MAX;
    for (size_t i = 0; i < e->as.match.arms.len; ++i) {
      const match_arm_t *arm = &e->as.match.arms.data[i];
      size_t uses =
          arm->guard ? stmt_ownership_min_uses_expr(arm->guard, param_name) : 0;
      uses += stmt_ownership_min_uses_stmt_paths(arm->conseq, param_name);
      if (uses < min_arm)
        min_arm = uses;
    }
    if (e->as.match.default_conseq) {
      size_t uses = stmt_ownership_min_uses_stmt_paths(
          e->as.match.default_conseq, param_name);
      if (uses < min_arm)
        min_arm = uses;
    } else if (min_arm != SIZE_MAX) {
      min_arm = 0;
    }
    return total + (min_arm == SIZE_MAX ? 0 : min_arm);
  }
  case NY_E_LAMBDA:
  case NY_E_FN: {
    bool shadowed = false;
    for (size_t i = 0; i < e->as.lambda.params.len; ++i)
      if (e->as.lambda.params.data[i].name &&
          strcmp(e->as.lambda.params.data[i].name, param_name) == 0) {
        shadowed = true;
        break;
      }
    return !shadowed && stmt_ownership_count_uses_stmt_internal(
                            e->as.lambda.body, param_name, NULL) > 0
               ? 1
               : 0;
  }
  case NY_E_COMPTIME:
    return stmt_ownership_min_uses_stmt_paths(e->as.comptime_expr.body,
                                              param_name);
  case NY_E_QUOTE:
    return stmt_ownership_min_uses_stmt_paths(e->as.quote.body, param_name) +
           stmt_ownership_min_uses_expr(e->as.quote.expr, param_name);
  case NY_E_SPLICE:
    return stmt_ownership_min_uses_expr(e->as.splice.expr, param_name);
  case NY_E_ASM: {
    size_t total = 0;
    for (size_t i = 0; i < e->as.as_asm.args.len; ++i)
      total +=
          stmt_ownership_min_uses_expr(e->as.as_asm.args.data[i], param_name);
    return total;
  }
  default:
    return 0;
  }
}

typedef struct stmt_ownership_min_flow {
  bool has_live;
  size_t live;
  size_t live_hi;
  bool has_done;
  size_t done;
  size_t done_hi;
  bool inconclusive;
} stmt_ownership_min_flow_t;

static stmt_ownership_min_flow_t
stmt_ownership_min_flow_union(stmt_ownership_min_flow_t a,
                              stmt_ownership_min_flow_t b) {
  if (b.has_live && (!a.has_live || b.live < a.live)) {
    a.has_live = true;
    a.live = b.live;
  }
  if (b.has_live && b.live_hi > a.live_hi)
    a.live_hi = b.live_hi;
  if (b.has_done && (!a.has_done || b.done < a.done)) {
    a.has_done = true;
    a.done = b.done;
  }
  if (b.has_done && b.done_hi > a.done_hi)
    a.done_hi = b.done_hi;
  a.inconclusive |= b.inconclusive;
  return a;
}

static stmt_ownership_min_flow_t
stmt_ownership_min_flow_sequence(stmt_ownership_min_flow_t before,
                                 stmt_ownership_min_flow_t after) {
  stmt_ownership_min_flow_t out = before;
  out.has_live = before.has_live && after.has_live;
  if (out.has_live) {
    out.live = before.live + after.live;
    out.live_hi = before.live_hi + after.live_hi;
  }
  if (before.has_live && after.has_done) {
    size_t done = before.live + after.done;
    if (!out.has_done || done < out.done) {
      out.has_done = true;
      out.done = done;
    }
    size_t done_hi = before.live_hi + after.done_hi;
    if (done_hi > out.done_hi)
      out.done_hi = done_hi;
  }
  out.inconclusive |= after.inconclusive;
  return out;
}

static bool stmt_ownership_expr_needs_path_flow(const expr_t *e,
                                                unsigned depth);
static bool stmt_ownership_stmt_needs_path_flow(const stmt_t *s,
                                                unsigned depth);

static bool stmt_ownership_expr_needs_path_flow(const expr_t *e,
                                                unsigned depth) {
  if (!e)
    return false;
  if (depth > 128)
    return true;
  switch (e->kind) {
  case NY_E_TRY:
    return true;
  case NY_E_UNARY:
    return stmt_ownership_expr_needs_path_flow(e->as.unary.right, depth + 1);
  case NY_E_BINARY:
  case NY_E_LOGICAL:
    return stmt_ownership_expr_needs_path_flow(e->as.binary.left, depth + 1) ||
           stmt_ownership_expr_needs_path_flow(e->as.binary.right, depth + 1);
  case NY_E_TERNARY:
    return stmt_ownership_expr_needs_path_flow(e->as.ternary.cond, depth + 1) ||
           stmt_ownership_expr_needs_path_flow(e->as.ternary.true_expr,
                                               depth + 1) ||
           stmt_ownership_expr_needs_path_flow(e->as.ternary.false_expr,
                                               depth + 1);
  case NY_E_CALL:
    if (stmt_ownership_expr_needs_path_flow(e->as.call.callee, depth + 1))
      return true;
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      if (stmt_ownership_expr_needs_path_flow(e->as.call.args.data[i].val,
                                              depth + 1))
        return true;
    return false;
  case NY_E_MEMCALL:
    if (stmt_ownership_expr_needs_path_flow(e->as.memcall.target, depth + 1))
      return true;
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      if (stmt_ownership_expr_needs_path_flow(e->as.memcall.args.data[i].val,
                                              depth + 1))
        return true;
    return false;
  case NY_E_INDEX:
    return stmt_ownership_expr_needs_path_flow(e->as.index.target, depth + 1) ||
           stmt_ownership_expr_needs_path_flow(e->as.index.start, depth + 1) ||
           stmt_ownership_expr_needs_path_flow(e->as.index.stop, depth + 1) ||
           stmt_ownership_expr_needs_path_flow(e->as.index.step, depth + 1);
  case NY_E_MEMBER:
    return stmt_ownership_expr_needs_path_flow(e->as.member.target, depth + 1);
  case NY_E_DEREF:
    return stmt_ownership_expr_needs_path_flow(e->as.deref.target, depth + 1);
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      if (stmt_ownership_expr_needs_path_flow(e->as.list_like.data[i],
                                              depth + 1))
        return true;
    return false;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i)
      if (stmt_ownership_expr_needs_path_flow(e->as.dict.pairs.data[i].key,
                                              depth + 1) ||
          stmt_ownership_expr_needs_path_flow(e->as.dict.pairs.data[i].value,
                                              depth + 1))
        return true;
    return false;
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; ++i)
      if (e->as.fstring.parts.data[i].kind == NY_FSP_EXPR &&
          stmt_ownership_expr_needs_path_flow(e->as.fstring.parts.data[i].as.e,
                                              depth + 1))
        return true;
    return false;
  case NY_E_MATCH:
    return true;
  case NY_E_LAMBDA:
  case NY_E_FN:
    /*
     * Deferred execution cannot return from the enclosing function.
     */
    return false;
  case NY_E_COMPTIME:
    return stmt_ownership_stmt_needs_path_flow(e->as.comptime_expr.body,
                                               depth + 1);
  case NY_E_QUOTE:
    return stmt_ownership_stmt_needs_path_flow(e->as.quote.body, depth + 1) ||
           stmt_ownership_expr_needs_path_flow(e->as.quote.expr, depth + 1);
  case NY_E_SPLICE:
    return stmt_ownership_expr_needs_path_flow(e->as.splice.expr, depth + 1);
  case NY_E_ASM:
    for (size_t i = 0; i < e->as.as_asm.args.len; ++i)
      if (stmt_ownership_expr_needs_path_flow(e->as.as_asm.args.data[i],
                                              depth + 1))
        return true;
    return false;
  default:
    return false;
  }
}

static bool stmt_ownership_stmt_needs_path_flow(const stmt_t *s,
                                                unsigned depth) {
  if (!s)
    return false;
  if (depth > 128)
    return true;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (stmt_ownership_stmt_needs_path_flow(s->as.block.body.data[i],
                                              depth + 1))
        return true;
    return false;
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; ++i)
      if (stmt_ownership_expr_needs_path_flow(s->as.var.exprs.data[i],
                                              depth + 1))
        return true;
    return false;
  case NY_S_EXPR:
    return stmt_ownership_expr_needs_path_flow(s->as.expr.expr, depth + 1);
  case NY_S_RETURN:
    return stmt_ownership_expr_needs_path_flow(s->as.ret.value, depth + 1);
  case NY_S_IF:
    return stmt_ownership_stmt_needs_path_flow(s->as.iff.init, depth + 1) ||
           stmt_ownership_expr_needs_path_flow(s->as.iff.test, depth + 1) ||
           stmt_ownership_stmt_needs_path_flow(s->as.iff.conseq, depth + 1) ||
           stmt_ownership_stmt_needs_path_flow(s->as.iff.alt, depth + 1);
  case NY_S_MATCH:
    if (stmt_ownership_expr_needs_path_flow(s->as.match.test, depth + 1))
      return true;
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      if (stmt_ownership_expr_needs_path_flow(s->as.match.arms.data[i].guard,
                                              depth + 1) ||
          stmt_ownership_stmt_needs_path_flow(s->as.match.arms.data[i].conseq,
                                              depth + 1))
        return true;
    return stmt_ownership_stmt_needs_path_flow(s->as.match.default_conseq,
                                               depth + 1);
  case NY_S_WHILE:
    return stmt_ownership_stmt_needs_path_flow(s->as.whl.init, depth + 1) ||
           stmt_ownership_expr_needs_path_flow(s->as.whl.test, depth + 1) ||
           stmt_ownership_stmt_needs_path_flow(s->as.whl.body, depth + 1) ||
           stmt_ownership_stmt_needs_path_flow(s->as.whl.update, depth + 1);
  case NY_S_FOR:
    return stmt_ownership_stmt_needs_path_flow(s->as.fr.init, depth + 1) ||
           stmt_ownership_expr_needs_path_flow(s->as.fr.cond, depth + 1) ||
           stmt_ownership_expr_needs_path_flow(s->as.fr.iterable, depth + 1) ||
           stmt_ownership_stmt_needs_path_flow(s->as.fr.body, depth + 1) ||
           stmt_ownership_stmt_needs_path_flow(s->as.fr.update, depth + 1);
  case NY_S_TRY:
    return stmt_ownership_stmt_needs_path_flow(s->as.tr.body, depth + 1) ||
           stmt_ownership_stmt_needs_path_flow(s->as.tr.handler, depth + 1);
  case NY_S_GUARD:
    return stmt_ownership_expr_needs_path_flow(s->as.guard.value, depth + 1) ||
           stmt_ownership_stmt_needs_path_flow(s->as.guard.fallback, depth + 1);
  case NY_S_DEFER:
    return stmt_ownership_stmt_needs_path_flow(s->as.de.body, depth + 1);
  default:
    return false;
  }
}

static stmt_ownership_min_flow_t
stmt_ownership_min_flow_stmt(const stmt_t *s, const char *param_name,
                             unsigned depth);

static size_t stmt_ownership_match_upper(const expr_t *test,
                                         const ny_match_arm_list *arms,
                                         const stmt_t *default_conseq,
                                         const char *param_name) {
  size_t probe_uses = stmt_ownership_count_uses_expr(test, param_name);
  size_t body_max = 0;
  for (size_t i = 0; arms && i < arms->len; ++i) {
    const match_arm_t *arm = &arms->data[i];
    for (size_t j = 0; j < arm->patterns.len; ++j)
      probe_uses +=
          stmt_ownership_count_uses_expr(arm->patterns.data[j], param_name);
    probe_uses += stmt_ownership_count_uses_expr(arm->guard, param_name);
    size_t body =
        stmt_ownership_count_uses_stmt_internal(arm->conseq, param_name, NULL);
    if (body > body_max)
      body_max = body;
  }
  size_t default_uses =
      stmt_ownership_count_uses_stmt_internal(default_conseq, param_name, NULL);
  if (default_uses > body_max)
    body_max = default_uses;
  return probe_uses + body_max;
}

static bool
stmt_ownership_match_has_unguarded_wildcard(const ny_match_arm_list *arms) {
  for (size_t i = 0; arms && i < arms->len; ++i) {
    const match_arm_t *arm = &arms->data[i];
    if (arm->guard)
      continue;
    for (size_t j = 0; j < arm->patterns.len; ++j)
      if (ny_expr_is_wildcard_ident(arm->patterns.data[j]))
        return true;
  }
  return false;
}

static stmt_ownership_min_flow_t
stmt_ownership_min_flow_expr_inner(const expr_t *e, const char *param_name,
                                   unsigned depth) {
  const stmt_ownership_min_flow_t empty = {.has_live = true, .live = 0};
  if (!e)
    return empty;
  if (depth > 128)
    return (stmt_ownership_min_flow_t){.inconclusive = true};
  if (e->kind == NY_E_TRY) {
    stmt_ownership_min_flow_t target = stmt_ownership_min_flow_expr_inner(
        e->as.try_expr.target, param_name, depth + 1);
    if (target.has_live && (!target.has_done || target.live < target.done)) {
      target.has_done = true;
      target.done = target.live;
    }
    if (target.has_live && target.live_hi > target.done_hi)
      target.done_hi = target.live_hi;
    return target;
  }
  if (!stmt_ownership_expr_needs_path_flow(e, depth))
    return (stmt_ownership_min_flow_t){
        .has_live = true,
        .live = stmt_ownership_min_uses_expr(e, param_name),
        .live_hi = stmt_ownership_count_uses_expr(e, param_name),
    };

  switch (e->kind) {
  case NY_E_UNARY:
    return stmt_ownership_min_flow_expr_inner(e->as.unary.right, param_name,
                                              depth + 1);
  case NY_E_BINARY:
    if (e->as.binary.op && strcmp(e->as.binary.op, "=") == 0)
      return stmt_ownership_min_flow_expr_inner(e->as.binary.right, param_name,
                                                depth + 1);
    return stmt_ownership_min_flow_sequence(
        stmt_ownership_min_flow_expr_inner(e->as.binary.left, param_name,
                                           depth + 1),
        stmt_ownership_min_flow_expr_inner(e->as.binary.right, param_name,
                                           depth + 1));
  case NY_E_LOGICAL: {
    stmt_ownership_min_flow_t left = stmt_ownership_min_flow_expr_inner(
        e->as.logical.left, param_name, depth + 1);
    stmt_ownership_min_flow_t with_right = stmt_ownership_min_flow_sequence(
        left, stmt_ownership_min_flow_expr_inner(e->as.logical.right,
                                                 param_name, depth + 1));
    return stmt_ownership_min_flow_union(left, with_right);
  }
  case NY_E_TERNARY: {
    stmt_ownership_min_flow_t cond = stmt_ownership_min_flow_expr_inner(
        e->as.ternary.cond, param_name, depth + 1);
    stmt_ownership_min_flow_t branches = stmt_ownership_min_flow_union(
        stmt_ownership_min_flow_expr_inner(e->as.ternary.true_expr, param_name,
                                           depth + 1),
        stmt_ownership_min_flow_expr_inner(e->as.ternary.false_expr, param_name,
                                           depth + 1));
    return stmt_ownership_min_flow_sequence(cond, branches);
  }
  case NY_E_CALL: {
    stmt_ownership_min_flow_t flow = stmt_ownership_min_flow_expr_inner(
        e->as.call.callee, param_name, depth + 1);
    for (size_t i = 0; i < e->as.call.args.len && flow.has_live; ++i)
      flow = stmt_ownership_min_flow_sequence(
          flow, stmt_ownership_min_flow_expr_inner(e->as.call.args.data[i].val,
                                                   param_name, depth + 1));
    return flow;
  }
  case NY_E_MEMCALL: {
    stmt_ownership_min_flow_t flow = stmt_ownership_min_flow_expr_inner(
        e->as.memcall.target, param_name, depth + 1);
    for (size_t i = 0; i < e->as.memcall.args.len && flow.has_live; ++i)
      flow = stmt_ownership_min_flow_sequence(
          flow, stmt_ownership_min_flow_expr_inner(
                    e->as.memcall.args.data[i].val, param_name, depth + 1));
    return flow;
  }
  case NY_E_INDEX: {
    stmt_ownership_min_flow_t flow = stmt_ownership_min_flow_expr_inner(
        e->as.index.target, param_name, depth + 1);
    flow = stmt_ownership_min_flow_sequence(
        flow, stmt_ownership_min_flow_expr_inner(e->as.index.start, param_name,
                                                 depth + 1));
    flow = stmt_ownership_min_flow_sequence(
        flow, stmt_ownership_min_flow_expr_inner(e->as.index.stop, param_name,
                                                 depth + 1));
    return stmt_ownership_min_flow_sequence(
        flow, stmt_ownership_min_flow_expr_inner(e->as.index.step, param_name,
                                                 depth + 1));
  }
  case NY_E_MEMBER:
    return stmt_ownership_min_flow_expr_inner(e->as.member.target, param_name,
                                              depth + 1);
  case NY_E_DEREF:
    return stmt_ownership_min_flow_expr_inner(e->as.deref.target, param_name,
                                              depth + 1);
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET: {
    stmt_ownership_min_flow_t flow = empty;
    for (size_t i = 0; i < e->as.list_like.len && flow.has_live; ++i)
      flow = stmt_ownership_min_flow_sequence(
          flow, stmt_ownership_min_flow_expr_inner(e->as.list_like.data[i],
                                                   param_name, depth + 1));
    return flow;
  }
  case NY_E_DICT: {
    stmt_ownership_min_flow_t flow = empty;
    for (size_t i = 0; i < e->as.dict.pairs.len && flow.has_live; ++i) {
      flow = stmt_ownership_min_flow_sequence(
          flow, stmt_ownership_min_flow_expr_inner(e->as.dict.pairs.data[i].key,
                                                   param_name, depth + 1));
      flow = stmt_ownership_min_flow_sequence(
          flow, stmt_ownership_min_flow_expr_inner(
                    e->as.dict.pairs.data[i].value, param_name, depth + 1));
    }
    return flow;
  }
  case NY_E_FSTRING: {
    stmt_ownership_min_flow_t flow = empty;
    for (size_t i = 0; i < e->as.fstring.parts.len && flow.has_live; ++i)
      if (e->as.fstring.parts.data[i].kind == NY_FSP_EXPR)
        flow = stmt_ownership_min_flow_sequence(
            flow, stmt_ownership_min_flow_expr_inner(
                      e->as.fstring.parts.data[i].as.e, param_name, depth + 1));
    return flow;
  }
  case NY_E_MATCH: {
    stmt_ownership_min_flow_t prefix = stmt_ownership_min_flow_expr_inner(
        e->as.match.test, param_name, depth + 1);
    stmt_ownership_min_flow_t arms = {.has_live = false};
    for (size_t i = 0; i < e->as.match.arms.len; ++i) {
      const match_arm_t *arm = &e->as.match.arms.data[i];
      stmt_ownership_min_flow_t branch =
          stmt_ownership_min_flow_stmt(arm->conseq, param_name, depth + 1);
      branch = stmt_ownership_min_flow_sequence(
          stmt_ownership_min_flow_expr_inner(arm->guard, param_name, depth + 1),
          branch);
      arms = stmt_ownership_min_flow_union(arms, branch);
    }
    if (e->as.match.default_conseq)
      arms = stmt_ownership_min_flow_union(
          arms, stmt_ownership_min_flow_stmt(e->as.match.default_conseq,
                                             param_name, depth + 1));
    else if (!stmt_ownership_match_has_unguarded_wildcard(&e->as.match.arms))
      arms = stmt_ownership_min_flow_union(arms, empty);
    size_t arms_hi = stmt_ownership_match_upper(
        NULL, &e->as.match.arms, e->as.match.default_conseq, param_name);
    if (arms.has_live && arms.live_hi < arms_hi)
      arms.live_hi = arms_hi;
    if (arms.has_done && arms.done_hi < arms_hi)
      arms.done_hi = arms_hi;
    return stmt_ownership_min_flow_sequence(prefix, arms);
  }
  case NY_E_COMPTIME:
    return stmt_ownership_min_flow_stmt(e->as.comptime_expr.body, param_name,
                                        depth + 1);
  case NY_E_QUOTE:
    return stmt_ownership_min_flow_sequence(
        stmt_ownership_min_flow_stmt(e->as.quote.body, param_name, depth + 1),
        stmt_ownership_min_flow_expr_inner(e->as.quote.expr, param_name,
                                           depth + 1));
  case NY_E_SPLICE:
    return stmt_ownership_min_flow_expr_inner(e->as.splice.expr, param_name,
                                              depth + 1);
  case NY_E_ASM: {
    stmt_ownership_min_flow_t flow = empty;
    for (size_t i = 0; i < e->as.as_asm.args.len && flow.has_live; ++i)
      flow = stmt_ownership_min_flow_sequence(
          flow, stmt_ownership_min_flow_expr_inner(e->as.as_asm.args.data[i],
                                                   param_name, depth + 1));
    return flow;
  }
  default:
    /*
     * Preserve soundness for any uncommon expression shape not yet modeled.
     */
    return (stmt_ownership_min_flow_t){
        .has_live = true,
        .live = stmt_ownership_min_uses_expr(e, param_name),
        .live_hi = stmt_ownership_count_uses_expr(e, param_name),
        .has_done = true,
        .done = 0,
        .done_hi = stmt_ownership_count_uses_expr(e, param_name),
    };
  }
}

static stmt_ownership_min_flow_t
stmt_ownership_min_flow_expr(const expr_t *e, const char *param_name) {
  return stmt_ownership_min_flow_expr_inner(e, param_name, 0);
}

static stmt_ownership_min_flow_t
stmt_ownership_min_flow_stmt(const stmt_t *s, const char *param_name,
                             unsigned depth) {
  const stmt_ownership_min_flow_t empty = {.has_live = true, .live = 0};
  if (!s || !param_name)
    return empty;
  if (depth > 128)
    return (stmt_ownership_min_flow_t){.inconclusive = true};
  switch (s->kind) {
  case NY_S_BLOCK: {
    stmt_ownership_min_flow_t flow = empty;
    for (size_t i = 0; i < s->as.block.body.len && flow.has_live; ++i) {
      stmt_ownership_min_flow_t next = stmt_ownership_min_flow_stmt(
          s->as.block.body.data[i], param_name, depth + 1);
      flow = stmt_ownership_min_flow_sequence(flow, next);
    }
    return flow;
  }
  case NY_S_RETURN: {
    stmt_ownership_min_flow_t value =
        stmt_ownership_min_flow_expr(s->as.ret.value, param_name);
    stmt_ownership_min_flow_t flow = {.has_done = value.has_done,
                                      .done = value.done,
                                      .done_hi = value.done_hi};
    if (value.has_live && (!flow.has_done || value.live < flow.done)) {
      flow.has_done = true;
      flow.done = value.live;
    }
    if (value.has_live && value.live_hi > flow.done_hi)
      flow.done_hi = value.live_hi;
    return flow;
  }
  case NY_S_IF: {
    stmt_ownership_min_flow_t flow =
        stmt_ownership_min_flow_stmt(s->as.iff.init, param_name, depth + 1);
    flow = stmt_ownership_min_flow_sequence(
        flow, stmt_ownership_min_flow_expr(s->as.iff.test, param_name));
    stmt_ownership_min_flow_t then_flow =
        stmt_ownership_min_flow_stmt(s->as.iff.conseq, param_name, depth + 1);
    stmt_ownership_min_flow_t else_flow =
        s->as.iff.alt
            ? stmt_ownership_min_flow_stmt(s->as.iff.alt, param_name, depth + 1)
            : empty;
    return stmt_ownership_min_flow_sequence(
        flow, stmt_ownership_min_flow_union(then_flow, else_flow));
  }
  case NY_S_MATCH: {
    stmt_ownership_min_flow_t prefix =
        stmt_ownership_min_flow_expr(s->as.match.test, param_name);
    stmt_ownership_min_flow_t arms = {.has_live = false};
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      const match_arm_t *arm = &s->as.match.arms.data[i];
      stmt_ownership_min_flow_t branch =
          stmt_ownership_min_flow_stmt(arm->conseq, param_name, depth + 1);
      branch = stmt_ownership_min_flow_sequence(
          stmt_ownership_min_flow_expr(arm->guard, param_name), branch);
      arms = stmt_ownership_min_flow_union(arms, branch);
    }
    if (s->as.match.default_conseq) {
      arms = stmt_ownership_min_flow_union(
          arms, stmt_ownership_min_flow_stmt(s->as.match.default_conseq,
                                             param_name, depth + 1));
    } else if (!stmt_ownership_match_has_unguarded_wildcard(
                   &s->as.match.arms)) {
      /*
       * An unhandled value is a conservative zero-use path.
       */
      arms = stmt_ownership_min_flow_union(arms, empty);
    }
    size_t arms_hi = stmt_ownership_match_upper(
        NULL, &s->as.match.arms, s->as.match.default_conseq, param_name);
    if (arms.has_live && arms.live_hi < arms_hi)
      arms.live_hi = arms_hi;
    if (arms.has_done && arms.done_hi < arms_hi)
      arms.done_hi = arms_hi;
    return stmt_ownership_min_flow_sequence(prefix, arms);
  }
  case NY_S_WHILE: {
    stmt_ownership_min_flow_t flow =
        stmt_ownership_min_flow_stmt(s->as.whl.init, param_name, depth + 1);
    /*
     * A while test is evaluated at least once; the body may execute zero
     * times. The upper bound still includes all possible loop-body uses.
     */
    flow = stmt_ownership_min_flow_sequence(
        flow, stmt_ownership_min_flow_expr(s->as.whl.test, param_name));
    flow.live_hi = stmt_ownership_count_uses_stmt_internal(s, param_name, NULL);
    stmt_ownership_min_flow_t body =
        stmt_ownership_min_flow_stmt(s->as.whl.body, param_name, depth + 1);
    if (body.has_done) {
      if (!flow.has_done || flow.live < flow.done)
        flow.done = flow.live;
      flow.has_done = true;
      if (flow.live_hi > flow.done_hi)
        flow.done_hi = flow.live_hi;
    }
    return flow;
  }
  case NY_S_FOR: {
    stmt_ownership_min_flow_t flow =
        stmt_ownership_min_flow_stmt(s->as.fr.init, param_name, depth + 1);
    /*
     * The iterable and loop condition are evaluated even if the body is
     * skipped, so their uses contribute to the lower bound.
     */
    flow = stmt_ownership_min_flow_sequence(
        flow, stmt_ownership_min_flow_expr(s->as.fr.iterable, param_name));
    flow = stmt_ownership_min_flow_sequence(
        flow, stmt_ownership_min_flow_expr(s->as.fr.cond, param_name));
    flow.live_hi = stmt_ownership_count_uses_stmt_internal(s, param_name, NULL);
    stmt_ownership_min_flow_t body =
        stmt_ownership_min_flow_stmt(s->as.fr.body, param_name, depth + 1);
    if (body.has_done) {
      if (!flow.has_done || flow.live < flow.done)
        flow.done = flow.live;
      flow.has_done = true;
      if (flow.live_hi > flow.done_hi)
        flow.done_hi = flow.live_hi;
    }
    return flow;
  }
  case NY_S_TRY: {
    size_t body_hi = stmt_ownership_count_uses_stmt_internal(s->as.tr.body,
                                                             param_name, NULL);
    size_t handler_hi = stmt_ownership_count_uses_stmt_internal(
        s->as.tr.handler, param_name, NULL);
    stmt_ownership_min_flow_t flow = {
        .has_live = true,
        /*
         * An exception may transfer to the handler before any body use.
         */
        .live = 0,
        .live_hi = body_hi > handler_hi ? body_hi : handler_hi,
    };
    stmt_ownership_min_flow_t body =
        stmt_ownership_min_flow_stmt(s->as.tr.body, param_name, depth + 1);
    stmt_ownership_min_flow_t handler =
        stmt_ownership_min_flow_stmt(s->as.tr.handler, param_name, depth + 1);
    if (body.has_done || handler.has_done) {
      flow.has_done = true;
      flow.done =
          0; /* exceptional control flow may exit before a counted use */
      flow.done_hi = body_hi + handler_hi;
    }
    return flow;
  }
  case NY_S_EXPR:
    return stmt_ownership_min_flow_expr(s->as.expr.expr, param_name);
  case NY_S_GUARD:
    return stmt_ownership_min_flow_expr(s->as.guard.value, param_name);
  case NY_S_VAR: {
    stmt_ownership_min_flow_t flow = empty;
    for (size_t i = 0; i < s->as.var.exprs.len; ++i)
      flow = stmt_ownership_min_flow_sequence(
          flow,
          stmt_ownership_min_flow_expr(s->as.var.exprs.data[i], param_name));
    return flow;
  }
  case NY_S_DEFER:
    return (stmt_ownership_min_flow_t){
        .has_live = true,
        /*
         * Deferred execution is accounted for conservatively at scope exit.
         */
        .live = 0,
        .live_hi = stmt_ownership_count_uses_stmt_internal(s, param_name, NULL),
    };
  default:
    return (stmt_ownership_min_flow_t){
        .has_live = true,
        .live = 0,
        .live_hi = stmt_ownership_count_uses_stmt_internal(s, param_name, NULL),
    };
  }
}

static size_t stmt_ownership_min_uses_stmt_paths(const stmt_t *s,
                                                 const char *param_name) {
  stmt_ownership_min_flow_t flow =
      stmt_ownership_min_flow_stmt(s, param_name, 0);
  if (!flow.has_live)
    return flow.done;
  if (!flow.has_done || flow.live < flow.done)
    return flow.live;
  return flow.done;
}

static bool stmt_ownership_uses_unbounded_loop(const stmt_t *s,
                                               const char *param_name) {
  if (!s || !param_name)
    return false;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (stmt_ownership_uses_unbounded_loop(s->as.block.body.data[i],
                                             param_name))
        return true;
    return false;
  case NY_S_IF:
    return stmt_ownership_uses_unbounded_loop(s->as.iff.conseq, param_name) ||
           stmt_ownership_uses_unbounded_loop(s->as.iff.alt, param_name);
  case NY_S_MATCH:
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      if (stmt_ownership_uses_unbounded_loop(s->as.match.arms.data[i].conseq,
                                             param_name))
        return true;
    return stmt_ownership_uses_unbounded_loop(s->as.match.default_conseq,
                                              param_name);
  case NY_S_WHILE:
    if (stmt_ownership_count_uses_expr(s->as.whl.test, param_name) ||
        stmt_ownership_count_uses_stmt_internal(s->as.whl.body, param_name,
                                                NULL) ||
        stmt_ownership_count_uses_stmt_internal(s->as.whl.update, param_name,
                                                NULL))
      return true;
    return stmt_ownership_uses_unbounded_loop(s->as.whl.body, param_name);
  case NY_S_FOR:
    if (stmt_ownership_count_uses_expr(s->as.fr.cond, param_name) ||
        stmt_ownership_count_uses_stmt_internal(s->as.fr.body, param_name,
                                                NULL) ||
        stmt_ownership_count_uses_stmt_internal(s->as.fr.update, param_name,
                                                NULL))
      return true;
    return stmt_ownership_uses_unbounded_loop(s->as.fr.body, param_name);
  case NY_S_TRY:
    return stmt_ownership_uses_unbounded_loop(s->as.tr.body, param_name) ||
           stmt_ownership_uses_unbounded_loop(s->as.tr.handler, param_name);
  default:
    return false;
  }
}

static bool stmt_ownership_is_param_reassignment(const stmt_t *s,
                                                 const char *param_name) {
  if (!s || !param_name)
    return false;
  if (s->kind == NY_S_VAR && !s->as.var.is_decl) {
    for (size_t i = 0; i < s->as.var.names.len; ++i)
      if (s->as.var.names.data[i] &&
          strcmp(s->as.var.names.data[i], param_name) == 0)
        return true;
  }
  /*
   * Assignment statements parsed inside match-arm blocks can remain a
   * statement expression instead of the variable-reassignment node used by
   * ordinary block parsing. Recognize only a direct simple assignment; the
   * allowance body validator admits no compound or throwing assignment.
   */
  if (s->kind == NY_S_EXPR && s->as.expr.expr &&
      s->as.expr.expr->kind == NY_E_BINARY && s->as.expr.expr->as.binary.op &&
      strcmp(s->as.expr.expr->as.binary.op, "=") == 0 &&
      s->as.expr.expr->as.binary.left &&
      s->as.expr.expr->as.binary.left->kind == NY_E_IDENT &&
      s->as.expr.expr->as.binary.left->as.ident.name &&
      strcmp(s->as.expr.expr->as.binary.left->as.ident.name, param_name) == 0)
    return true;
  return false;
}

static bool stmt_ownership_allowance_segment_valid(
    codegen_t *cg, const stmt_t *fn_stmt, const ny_allowance_t *allowance,
    stmt_ownership_min_flow_t flow, bool unbounded) {
  if (!cg || !fn_stmt || !allowance)
    return true;
  if (flow.inconclusive) {
    stmt_ownership_diag(
        cg, fn_stmt->tok,
        "cannot validate ownership allowance for '%s': analysis exceeded its "
        "nesting limit",
        allowance->name);
    return false;
  }
  size_t observed_lo = SIZE_MAX;
  size_t observed_hi = 0;
  if (flow.has_live) {
    observed_lo = flow.live;
    observed_hi = flow.live_hi;
  }
  if (flow.has_done) {
    if (flow.done < observed_lo)
      observed_lo = flow.done;
    if (flow.done_hi > observed_hi)
      observed_hi = flow.done_hi;
  }
  if (observed_lo == SIZE_MAX)
    observed_lo = 0;
  if (unbounded)
    observed_hi = SIZE_MAX;
  bool valid = true;
  if (observed_lo < (size_t)allowance->lo) {
    stmt_ownership_diag(cg, fn_stmt->tok,
                        "ownership allowance below for '%s': at least %zu "
                        "uses are required, but some path has %zu",
                        allowance->name, (size_t)allowance->lo, observed_lo);
    ny_diag_fix("ensure every control-flow path uses '%s' at least %zu times",
                allowance->name, (size_t)allowance->lo);
    valid = false;
  }
  if (allowance->hi >= 0 &&
      (unbounded || observed_hi > (size_t)allowance->hi)) {
    stmt_ownership_diag(cg, fn_stmt->tok,
                        "ownership allowance exceeded for '%s': observed "
                        "%s%zu > allowance %zu",
                        allowance->name, unbounded ? "at least " : "",
                        unbounded ? (size_t)allowance->hi + 1 : observed_hi,
                        (size_t)allowance->hi);
    ny_diag_fix("reduce uses of '%s' to at most %zu on every path",
                allowance->name, (size_t)allowance->hi);
    valid = false;
  }
  return valid;
}

typedef struct stmt_ownership_allowance_path {
  stmt_ownership_min_flow_t flow;
  bool unbounded;
} stmt_ownership_allowance_path_t;

typedef struct stmt_ownership_allowance_paths {
  stmt_ownership_allowance_path_t *data;
  size_t len;
  size_t cap;
  bool inconclusive;
} stmt_ownership_allowance_paths_t;

#define STMT_OWNERSHIP_ALLOWANCE_PATH_LIMIT 128u

static bool stmt_ownership_allowance_paths_append(
    stmt_ownership_allowance_paths_t *to,
    const stmt_ownership_allowance_path_t *items, size_t count) {
  if (!to || (!items && count))
    return false;
  if (count > STMT_OWNERSHIP_ALLOWANCE_PATH_LIMIT - to->len) {
    to->inconclusive = true;
    return false;
  }
  size_t need = to->len + count;
  if (need > to->cap) {
    size_t cap = to->cap ? to->cap : 8;
    while (cap < need && cap < STMT_OWNERSHIP_ALLOWANCE_PATH_LIMIT)
      cap *= 2;
    if (cap > STMT_OWNERSHIP_ALLOWANCE_PATH_LIMIT)
      cap = STMT_OWNERSHIP_ALLOWANCE_PATH_LIMIT;
    void *grown = realloc(to->data, cap * sizeof(*to->data));
    if (!grown) {
      to->inconclusive = true;
      return false;
    }
    to->data = grown;
    to->cap = cap;
  }
  if (count)
    memcpy(to->data + to->len, items, count * sizeof(*items));
  to->len = need;
  return true;
}

static bool stmt_ownership_allowance_paths_clone(
    stmt_ownership_allowance_paths_t *to,
    const stmt_ownership_allowance_paths_t *from) {
  if (!to || !from)
    return false;
  to->inconclusive |= from->inconclusive;
  return stmt_ownership_allowance_paths_append(to, from->data, from->len);
}

static void
stmt_ownership_allowance_paths_flow(stmt_ownership_allowance_paths_t *paths,
                                    stmt_ownership_min_flow_t next,
                                    bool unbounded) {
  if (!paths)
    return;
  for (size_t i = 0; i < paths->len; ++i) {
    stmt_ownership_allowance_path_t *path = &paths->data[i];
    if (path->flow.has_live)
      path->flow = stmt_ownership_min_flow_sequence(path->flow, next);
    path->flow.inconclusive |= next.inconclusive;
    path->unbounded |= unbounded;
  }
  stmt_ownership_allowance_paths_t split = {0};
  split.inconclusive = paths->inconclusive;
  for (size_t i = 0; i < paths->len; ++i) {
    stmt_ownership_allowance_path_t path = paths->data[i];
    if (path.flow.has_live && path.flow.has_done) {
      stmt_ownership_allowance_path_t done = path;
      done.flow.has_live = false;
      done.flow.live = 0;
      done.flow.live_hi = 0;
      stmt_ownership_allowance_path_t live = path;
      live.flow.has_done = false;
      live.flow.done = 0;
      live.flow.done_hi = 0;
      stmt_ownership_allowance_paths_append(&split, &done, 1);
      stmt_ownership_allowance_paths_append(&split, &live, 1);
    } else {
      stmt_ownership_allowance_paths_append(&split, &path, 1);
    }
  }
  free(paths->data);
  *paths = split;
}

static bool stmt_ownership_stmt_contains_param_reassignment(
    const stmt_t *s, const char *param_name, unsigned depth) {
  if (!s || !param_name)
    return false;
  if (depth > 128)
    return true;
  if (stmt_ownership_is_param_reassignment(s, param_name))
    return true;
#define CHECK_STMT_REASSIGN(child)                                             \
  stmt_ownership_stmt_contains_param_reassignment((child), param_name,         \
                                                  depth + 1)
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (CHECK_STMT_REASSIGN(s->as.block.body.data[i]))
        return true;
    return false;
  case NY_S_IF:
    return CHECK_STMT_REASSIGN(s->as.iff.init) ||
           CHECK_STMT_REASSIGN(s->as.iff.conseq) ||
           CHECK_STMT_REASSIGN(s->as.iff.alt);
  case NY_S_MATCH:
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      if (CHECK_STMT_REASSIGN(s->as.match.arms.data[i].conseq))
        return true;
    return CHECK_STMT_REASSIGN(s->as.match.default_conseq);
  case NY_S_WHILE:
    return CHECK_STMT_REASSIGN(s->as.whl.init) ||
           CHECK_STMT_REASSIGN(s->as.whl.body) ||
           CHECK_STMT_REASSIGN(s->as.whl.update);
  case NY_S_FOR:
    return CHECK_STMT_REASSIGN(s->as.fr.init) ||
           CHECK_STMT_REASSIGN(s->as.fr.body) ||
           CHECK_STMT_REASSIGN(s->as.fr.update);
  case NY_S_TRY:
    return CHECK_STMT_REASSIGN(s->as.tr.body) ||
           CHECK_STMT_REASSIGN(s->as.tr.handler);
  case NY_S_GUARD:
    return CHECK_STMT_REASSIGN(s->as.guard.fallback);
  case NY_S_DEFER:
    return CHECK_STMT_REASSIGN(s->as.de.body);
  default:
    return false;
  }
#undef CHECK_STMT_REASSIGN
}

/*
 * A `try` body that contains only local scalar assignments, literals, and
 * identifier reads cannot transfer to its handler. Keep this proof fragment
 * deliberately small: calls, operators, indexing, and other operations may
 * throw or invoke user code, so they must retain the inconclusive path result.
 */
static bool stmt_ownership_allowance_expr_nonthrowing(const expr_t *expr,
                                                      unsigned depth) {
  if (depth > 64)
    return false;
  /*
   * A missing expression is the language's unit/void value.
   */
  if (!expr)
    return true;
  return expr->kind == NY_E_LITERAL || expr->kind == NY_E_IDENT;
}

static bool stmt_ownership_allowance_stmt_nonthrowing(const stmt_t *stmt,
                                                      unsigned depth) {
  if (!stmt || depth > 64)
    return false;
  switch (stmt->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < stmt->as.block.body.len; ++i)
      if (!stmt_ownership_allowance_stmt_nonthrowing(
              stmt->as.block.body.data[i], depth + 1))
        return false;
    return true;
  case NY_S_VAR:
    if (stmt->as.var.is_del)
      return false;
    if (!stmt->as.var.is_decl) {
      for (size_t i = 0; i < stmt->as.var.names.len; ++i)
        if (!stmt->as.var.names.data[i])
          return false;
    }
    for (size_t i = 0; i < stmt->as.var.exprs.len; ++i)
      if (!stmt_ownership_allowance_expr_nonthrowing(stmt->as.var.exprs.data[i],
                                                     depth + 1))
        return false;
    return true;
  case NY_S_EXPR:
    if (!stmt->as.expr.expr || stmt->as.expr.expr->kind != NY_E_BINARY ||
        !stmt->as.expr.expr->as.binary.op ||
        strcmp(stmt->as.expr.expr->as.binary.op, "=") != 0 ||
        !stmt->as.expr.expr->as.binary.left ||
        stmt->as.expr.expr->as.binary.left->kind != NY_E_IDENT)
      return false;
    return stmt_ownership_allowance_expr_nonthrowing(
        stmt->as.expr.expr->as.binary.right, depth + 1);
  case NY_S_IF:
    return stmt_ownership_allowance_expr_nonthrowing(stmt->as.iff.test,
                                                     depth + 1) &&
           stmt_ownership_allowance_stmt_nonthrowing(stmt->as.iff.conseq,
                                                     depth + 1) &&
           (!stmt->as.iff.alt || stmt_ownership_allowance_stmt_nonthrowing(
                                     stmt->as.iff.alt, depth + 1));
  case NY_S_RETURN:
    return stmt_ownership_allowance_expr_nonthrowing(stmt->as.ret.value,
                                                     depth + 1);
  default:
    return false;
  }
}

static bool stmt_ownership_allowance_range_constant(const expr_t *expr,
                                                    int64_t *value_out) {
  if (!expr || !value_out)
    return false;
  if (expr->kind == NY_E_LITERAL && expr->as.literal.kind == NY_LIT_INT &&
      !expr->as.literal.integer_outside_i64) {
    *value_out = expr->as.literal.as.i;
    return true;
  }
  if (expr->kind == NY_E_UNARY && expr->as.unary.op &&
      strcmp(expr->as.unary.op, "-") == 0 && expr->as.unary.right &&
      expr->as.unary.right->kind == NY_E_LITERAL &&
      expr->as.unary.right->as.literal.kind == NY_LIT_INT &&
      !expr->as.unary.right->as.literal.integer_outside_i64 &&
      expr->as.unary.right->as.literal.as.i < INT64_MAX) {
    *value_out = -expr->as.unary.right->as.literal.as.i;
    return true;
  }
  return false;
}

/*
 * The range operator's integer form is inclusive. Bound exact unrolling to 64
 * iterations so allowance analysis has deterministic work and path growth.
 */
static bool
stmt_ownership_allowance_fixed_range_iterations(const expr_t *iterable,
                                                size_t *iterations_out) {
  if (!iterable || !iterations_out || iterable->kind != NY_E_BINARY ||
      !iterable->as.binary.op || strcmp(iterable->as.binary.op, "..") != 0)
    return false;
  int64_t start = 0, stop = 0;
  if (!stmt_ownership_allowance_range_constant(iterable->as.binary.left,
                                               &start) ||
      !stmt_ownership_allowance_range_constant(iterable->as.binary.right,
                                               &stop) ||
      stop == INT64_MAX)
    return false;
  if (start > stop) {
    *iterations_out = 0;
    return true;
  }
  int64_t distance = 0, count = 0;
  if (__builtin_sub_overflow(stop, start, &distance) ||
      __builtin_add_overflow(distance, INT64_C(1), &count) || count > 64)
    return false;
  *iterations_out = (size_t)count;
  return true;
}

static bool stmt_ownership_rules_stmt_writes_name(const stmt_t *stmt,
                                                  const char *name,
                                                  unsigned depth);

static bool stmt_ownership_allowance_fixed_counted_iterations(
    const stmt_t *stmt, const char *param_name, size_t *iterations_out) {
  if (!stmt || !iterations_out)
    return false;
  const stmt_t *init = NULL, *update_stmt = NULL;
  const stmt_t *body = NULL;
  const expr_t *condition = NULL;
  if (stmt->kind == NY_S_FOR) {
    if (stmt->as.fr.iterable)
      return false;
    init = stmt->as.fr.init;
    condition = stmt->as.fr.cond;
    update_stmt = stmt->as.fr.update;
    body = stmt->as.fr.body;
  } else if (stmt->kind == NY_S_WHILE) {
    if (stmt->as.whl.invariant)
      return false;
    init = stmt->as.whl.init;
    condition = stmt->as.whl.test;
    update_stmt = stmt->as.whl.update;
    body = stmt->as.whl.body;
  } else {
    return false;
  }
  if (!init || !condition || !update_stmt || init->kind != NY_S_VAR ||
      !init->as.var.is_decl || init->as.var.names.len != 1 ||
      init->as.var.exprs.len != 1 || !init->as.var.names.data[0] ||
      update_stmt->kind != NY_S_VAR || update_stmt->as.var.is_decl ||
      update_stmt->as.var.names.len != 1 || update_stmt->as.var.exprs.len != 1)
    return false;
  const char *iterator = init->as.var.names.data[0];
  const expr_t *update = update_stmt->as.var.exprs.data[0];
  if (!iterator || !condition || condition->kind != NY_E_BINARY ||
      !condition->as.binary.op || !condition->as.binary.left ||
      !condition->as.binary.right ||
      condition->as.binary.left->kind != NY_E_IDENT ||
      !condition->as.binary.left->as.ident.name ||
      strcmp(condition->as.binary.left->as.ident.name, iterator) != 0 ||
      !update_stmt->as.var.names.data[0] ||
      strcmp(update_stmt->as.var.names.data[0], iterator) != 0 || !update ||
      update->kind != NY_E_BINARY || !update->as.binary.op ||
      !update->as.binary.left || update->as.binary.left->kind != NY_E_IDENT ||
      !update->as.binary.left->as.ident.name ||
      strcmp(update->as.binary.left->as.ident.name, iterator) != 0)
    return false;
  if (stmt_ownership_rules_stmt_writes_name(body, iterator, 0))
    return false;
  const char *comparison = condition->as.binary.op;
  const char *increment = update->as.binary.op;
  bool ascending =
      strcmp(comparison, "<") == 0 || strcmp(comparison, "<=") == 0;
  bool descending =
      strcmp(comparison, ">") == 0 || strcmp(comparison, ">=") == 0;
  bool increasing_step = strcmp(increment, "+") == 0;
  bool decreasing_step = strcmp(increment, "-") == 0;
  if ((!ascending && !descending) || (ascending && !increasing_step) ||
      (descending && !decreasing_step))
    return false;
  int64_t start = 0, stop = 0, step = 0;
  if (!stmt_ownership_allowance_range_constant(init->as.var.exprs.data[0],
                                               &start) ||
      !stmt_ownership_allowance_range_constant(condition->as.binary.right,
                                               &stop) ||
      !stmt_ownership_allowance_range_constant(update->as.binary.right,
                                               &step) ||
      step != 1 || (param_name && strcmp(iterator, param_name) == 0) ||
      stmt_ownership_count_uses_stmt_internal(init, param_name, NULL) ||
      stmt_ownership_count_uses_expr(condition, param_name) ||
      stmt_ownership_count_uses_stmt_internal(update_stmt, param_name, NULL))
    return false;
  bool inclusive =
      strcmp(comparison, "<=") == 0 || strcmp(comparison, ">=") == 0;
  bool empty = ascending ? (start > stop || (!inclusive && start == stop))
                         : (start < stop || (!inclusive && start == stop));
  if (empty) {
    *iterations_out = 0;
    return true;
  }
  if (inclusive &&
      ((ascending && stop == INT64_MAX) || (descending && stop == INT64_MIN)))
    return false;
  int64_t distance = 0, count = 0;
  if ((ascending && __builtin_sub_overflow(stop, start, &distance)) ||
      (descending && __builtin_sub_overflow(start, stop, &distance)) ||
      (inclusive && __builtin_add_overflow(distance, INT64_C(1), &count)))
    return false;
  if (!inclusive)
    count = distance;
  if (count < 0 || count > 64)
    return false;
  *iterations_out = (size_t)count;
  return true;
}

static bool stmt_ownership_allowance_loop_body_supported(
    const stmt_t *stmt, const char *allowance_name, unsigned depth) {
  if (!stmt)
    return true;
  if (depth > 64)
    return false;
  if (stmt->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < stmt->as.block.body.len; ++i)
      if (!stmt_ownership_allowance_loop_body_supported(
              stmt->as.block.body.data[i], allowance_name, depth + 1))
        return false;
    return true;
  }
  /*
   * The allowance walker already forks and rejoins `if` paths. Permit such
   * branches in a statically unrolled range only when every expression and
   * statement is in the same bounded nonthrowing fragment. Path growth is
   * still capped by STMT_OWNERSHIP_ALLOWANCE_PATH_LIMIT and fails closed.
   */
  if (stmt->kind == NY_S_IF)
    return stmt_ownership_allowance_expr_nonthrowing(stmt->as.iff.test,
                                                     depth + 1) &&
           stmt_ownership_allowance_loop_body_supported(
               stmt->as.iff.init, allowance_name, depth + 1) &&
           stmt_ownership_allowance_loop_body_supported(
               stmt->as.iff.conseq, allowance_name, depth + 1) &&
           stmt_ownership_allowance_loop_body_supported(
               stmt->as.iff.alt, allowance_name, depth + 1);
  /*
   * Match paths are already forked and joined by the allowance walker. Keep
   * loop support limited to tests, patterns, and guards in the nonthrowing
   * fragment so a match cannot hide an effect or an inconclusive expression.
   */
  if (stmt->kind == NY_S_MATCH) {
    if (!stmt_ownership_allowance_expr_nonthrowing(stmt->as.match.test,
                                                   depth + 1))
      return false;
    for (size_t i = 0; i < stmt->as.match.arms.len; ++i) {
      const match_arm_t *arm = &stmt->as.match.arms.data[i];
      if (!stmt_ownership_allowance_expr_nonthrowing(arm->guard, depth + 1) ||
          !stmt_ownership_allowance_loop_body_supported(
              arm->conseq, allowance_name, depth + 1))
        return false;
      for (size_t j = 0; j < arm->patterns.len; ++j)
        if (!stmt_ownership_allowance_expr_nonthrowing(arm->patterns.data[j],
                                                       depth + 1))
          return false;
    }
    return stmt_ownership_allowance_loop_body_supported(
        stmt->as.match.default_conseq, allowance_name, depth + 1);
  }
  /*
   * Nested loops are safe to unroll only when each inner loop has a constant
   * bounded iteration count and its body stays in this same nonthrowing
   * fragment. The allowance walker enforces the aggregate path cap.
   */
  if (stmt->kind == NY_S_FOR || stmt->kind == NY_S_WHILE) {
    size_t iterations = 0;
    const stmt_t *body =
        stmt->kind == NY_S_FOR ? stmt->as.fr.body : stmt->as.whl.body;
    if (stmt_ownership_allowance_fixed_counted_iterations(stmt, allowance_name,
                                                          &iterations))
      return stmt_ownership_allowance_loop_body_supported(body, allowance_name,
                                                          depth + 1);
    return stmt->kind == NY_S_FOR && !stmt->as.fr.init && !stmt->as.fr.cond &&
           !stmt->as.fr.update &&
           stmt_ownership_allowance_fixed_range_iterations(stmt->as.fr.iterable,
                                                           &iterations) &&
           stmt_ownership_allowance_loop_body_supported(body, allowance_name,
                                                        depth + 1);
  }
  return (stmt->kind == NY_S_VAR || stmt->kind == NY_S_EXPR) &&
         stmt_ownership_allowance_stmt_nonthrowing(stmt, depth + 1);
}

static void
stmt_ownership_allowance_process_stmt(codegen_t *cg, const stmt_t *fn_stmt,
                                      const ny_allowance_t *allowance,
                                      stmt_ownership_allowance_paths_t *paths,
                                      const stmt_t *stmt, unsigned depth);

static void stmt_ownership_allowance_finish_reassignment(
    codegen_t *cg, const stmt_t *fn_stmt, const ny_allowance_t *allowance,
    stmt_ownership_allowance_paths_t *paths) {
  stmt_ownership_allowance_paths_t next = {0};
  for (size_t i = 0; i < paths->len; ++i) {
    stmt_ownership_allowance_path_t path = paths->data[i];
    if (!path.flow.has_live) {
      stmt_ownership_allowance_paths_append(&next, &path, 1);
      continue;
    }
    (void)stmt_ownership_allowance_segment_valid(cg, fn_stmt, allowance,
                                                 path.flow, path.unbounded);
    path.flow = (stmt_ownership_min_flow_t){.has_live = true};
    path.unbounded = false;
    stmt_ownership_allowance_paths_append(&next, &path, 1);
  }
  free(paths->data);
  *paths = next;
}

static void
stmt_ownership_allowance_process_stmt(codegen_t *cg, const stmt_t *fn_stmt,
                                      const ny_allowance_t *allowance,
                                      stmt_ownership_allowance_paths_t *paths,
                                      const stmt_t *stmt, unsigned depth) {
  if (!paths || !stmt || paths->inconclusive)
    return;
  if (depth > 64) {
    paths->inconclusive = true;
    return;
  }
  if (stmt->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < stmt->as.block.body.len && !paths->inconclusive;
         ++i) {
      const stmt_t *child = stmt->as.block.body.data[i];
      if (child && child->kind == NY_S_DEFER &&
          stmt_ownership_stmt_contains_param_reassignment(child,
                                                          allowance->name, 0)) {
        /*
         * A trailing defer executes as this scope exits, after all ordinary
         * statements in the block. Model it at that exit point only; moving
         * an earlier defer to its registration site would count later uses
         * in the wrong allowance epoch.
         */
        if (i + 1 != stmt->as.block.body.len ||
            !stmt_ownership_allowance_stmt_nonthrowing(child->as.de.body, 0)) {
          paths->inconclusive = true;
          return;
        }
        stmt_ownership_allowance_process_stmt(cg, fn_stmt, allowance, paths,
                                              child->as.de.body, depth + 1);
        continue;
      }
      stmt_ownership_allowance_process_stmt(cg, fn_stmt, allowance, paths,
                                            child, depth + 1);
    }
    return;
  }
  if (stmt->kind == NY_S_IF) {
    stmt_ownership_allowance_process_stmt(cg, fn_stmt, allowance, paths,
                                          stmt->as.iff.init, depth + 1);
    stmt_ownership_allowance_paths_flow(
        paths, stmt_ownership_min_flow_expr(stmt->as.iff.test, allowance->name),
        false);
    stmt_ownership_allowance_paths_t base = {0}, joined = {0};
    if (!stmt_ownership_allowance_paths_clone(&base, paths))
      paths->inconclusive = true;
    stmt_ownership_allowance_paths_t then_paths = {0};
    stmt_ownership_allowance_paths_clone(&then_paths, &base);
    stmt_ownership_allowance_process_stmt(cg, fn_stmt, allowance, &then_paths,
                                          stmt->as.iff.conseq, depth + 1);
    stmt_ownership_allowance_paths_append(&joined, then_paths.data,
                                          then_paths.len);
    joined.inconclusive |= then_paths.inconclusive;
    free(then_paths.data);
    if (stmt->as.iff.alt) {
      stmt_ownership_allowance_paths_t else_paths = {0};
      stmt_ownership_allowance_paths_clone(&else_paths, &base);
      stmt_ownership_allowance_process_stmt(cg, fn_stmt, allowance, &else_paths,
                                            stmt->as.iff.alt, depth + 1);
      stmt_ownership_allowance_paths_append(&joined, else_paths.data,
                                            else_paths.len);
      joined.inconclusive |= else_paths.inconclusive;
      free(else_paths.data);
    } else {
      stmt_ownership_allowance_paths_append(&joined, base.data, base.len);
    }
    joined.inconclusive |= base.inconclusive;
    free(base.data);
    free(paths->data);
    *paths = joined;
    return;
  }
  if (stmt->kind == NY_S_MATCH) {
    stmt_ownership_allowance_paths_flow(
        paths,
        stmt_ownership_min_flow_expr(stmt->as.match.test, allowance->name),
        false);
    stmt_ownership_allowance_paths_t base = {0}, joined = {0};
    stmt_ownership_allowance_paths_clone(&base, paths);
    for (size_t i = 0; i < stmt->as.match.arms.len; ++i) {
      const match_arm_t *arm = &stmt->as.match.arms.data[i];
      stmt_ownership_allowance_paths_t arm_paths = {0};
      stmt_ownership_allowance_paths_clone(&arm_paths, &base);
      for (size_t j = 0; j < arm->patterns.len; ++j)
        stmt_ownership_allowance_paths_flow(
            &arm_paths,
            stmt_ownership_min_flow_expr(arm->patterns.data[j],
                                         allowance->name),
            false);
      stmt_ownership_allowance_paths_flow(
          &arm_paths, stmt_ownership_min_flow_expr(arm->guard, allowance->name),
          false);
      stmt_ownership_allowance_process_stmt(cg, fn_stmt, allowance, &arm_paths,
                                            arm->conseq, depth + 1);
      stmt_ownership_allowance_paths_append(&joined, arm_paths.data,
                                            arm_paths.len);
      joined.inconclusive |= arm_paths.inconclusive;
      free(arm_paths.data);
    }
    if (stmt->as.match.default_conseq) {
      stmt_ownership_allowance_paths_t default_paths = {0};
      stmt_ownership_allowance_paths_clone(&default_paths, &base);
      stmt_ownership_allowance_process_stmt(
          cg, fn_stmt, allowance, &default_paths, stmt->as.match.default_conseq,
          depth + 1);
      stmt_ownership_allowance_paths_append(&joined, default_paths.data,
                                            default_paths.len);
      joined.inconclusive |= default_paths.inconclusive;
      free(default_paths.data);
    } else if (!stmt_ownership_match_has_unguarded_wildcard(
                   &stmt->as.match.arms)) {
      stmt_ownership_allowance_paths_append(&joined, base.data, base.len);
    }
    joined.inconclusive |= base.inconclusive;
    free(base.data);
    free(paths->data);
    *paths = joined;
    return;
  }
  if (stmt->kind == NY_S_GUARD) {
    stmt_ownership_allowance_paths_flow(
        paths,
        stmt_ownership_min_flow_expr(stmt->as.guard.value, allowance->name),
        false);
    stmt_ownership_allowance_paths_t base = {0}, joined = {0};
    stmt_ownership_allowance_paths_clone(&base, paths);
    stmt_ownership_allowance_paths_t success_paths = {0};
    stmt_ownership_allowance_paths_clone(&success_paths, &base);
    stmt_ownership_allowance_paths_append(&joined, success_paths.data,
                                          success_paths.len);
    joined.inconclusive |= success_paths.inconclusive;
    free(success_paths.data);
    stmt_ownership_allowance_paths_t fallback_paths = {0};
    stmt_ownership_allowance_paths_clone(&fallback_paths, &base);
    stmt_ownership_allowance_process_stmt(cg, fn_stmt, allowance,
                                          &fallback_paths,
                                          stmt->as.guard.fallback, depth + 1);
    stmt_ownership_allowance_paths_append(&joined, fallback_paths.data,
                                          fallback_paths.len);
    joined.inconclusive |= fallback_paths.inconclusive;
    free(fallback_paths.data);
    joined.inconclusive |= base.inconclusive;
    free(base.data);
    free(paths->data);
    *paths = joined;
    return;
  }
  if (stmt->kind == NY_S_WHILE || stmt->kind == NY_S_FOR) {
    size_t iterations = 0;
    const stmt_t *counted_init = NULL, *counted_body = NULL;
    const stmt_t *counted_update = NULL;
    if (stmt->kind == NY_S_FOR) {
      counted_init = stmt->as.fr.init;
      counted_body = stmt->as.fr.body;
      counted_update = stmt->as.fr.update;
    } else {
      counted_init = stmt->as.whl.init;
      counted_body = stmt->as.whl.body;
      counted_update = stmt->as.whl.update;
    }
    if (stmt_ownership_allowance_fixed_counted_iterations(stmt, allowance->name,
                                                          &iterations) &&
        stmt_ownership_allowance_loop_body_supported(counted_body,
                                                     allowance->name, 0)) {
      stmt_ownership_allowance_process_stmt(cg, fn_stmt, allowance, paths,
                                            counted_init, depth + 1);
      for (size_t i = 0; i < iterations && !paths->inconclusive; ++i) {
        stmt_ownership_allowance_process_stmt(cg, fn_stmt, allowance, paths,
                                              counted_body, depth + 1);
        if (!paths->inconclusive && paths->len)
          stmt_ownership_allowance_process_stmt(cg, fn_stmt, allowance, paths,
                                                counted_update, depth + 1);
      }
      return;
    }
    if (stmt->kind == NY_S_FOR && !stmt->as.fr.init && !stmt->as.fr.cond &&
        !stmt->as.fr.update &&
        stmt_ownership_allowance_fixed_range_iterations(stmt->as.fr.iterable,
                                                        &iterations) &&
        stmt_ownership_allowance_loop_body_supported(stmt->as.fr.body,
                                                     allowance->name, 0)) {
      stmt_ownership_allowance_paths_flow(
          paths,
          stmt_ownership_min_flow_expr(stmt->as.fr.iterable, allowance->name),
          false);
      for (size_t i = 0; i < iterations && !paths->inconclusive; ++i)
        stmt_ownership_allowance_process_stmt(cg, fn_stmt, allowance, paths,
                                              stmt->as.fr.body, depth + 1);
      return;
    }
    if (stmt_ownership_stmt_contains_param_reassignment(stmt, allowance->name,
                                                        0)) {
      paths->inconclusive = true;
      return;
    }
    stmt_ownership_allowance_paths_flow(
        paths, stmt_ownership_min_flow_stmt(stmt, allowance->name, depth),
        stmt_ownership_uses_unbounded_loop(stmt, allowance->name));
    return;
  }
  if (stmt->kind == NY_S_TRY && stmt_ownership_stmt_contains_param_reassignment(
                                    stmt, allowance->name, 0)) {
    if (!stmt_ownership_allowance_stmt_nonthrowing(stmt->as.tr.body, 0)) {
      paths->inconclusive = true;
      return;
    }
    stmt_ownership_allowance_process_stmt(cg, fn_stmt, allowance, paths,
                                          stmt->as.tr.body, depth + 1);
    return;
  }
  if (stmt->kind == NY_S_DEFER &&
      stmt_ownership_stmt_contains_param_reassignment(stmt, allowance->name,
                                                      0)) {
    paths->inconclusive = true;
    return;
  }
  if (stmt_ownership_is_param_reassignment(stmt, allowance->name)) {
    stmt_ownership_allowance_paths_flow(
        paths, stmt_ownership_min_flow_stmt(stmt, allowance->name, depth),
        false);
    stmt_ownership_allowance_finish_reassignment(cg, fn_stmt, allowance, paths);
    return;
  }
  stmt_ownership_allowance_paths_flow(
      paths, stmt_ownership_min_flow_stmt(stmt, allowance->name, depth),
      stmt_ownership_uses_unbounded_loop(stmt, allowance->name));
  if (stmt->kind == NY_S_RETURN) {
    for (size_t i = 0; i < paths->len; ++i) {
      stmt_ownership_allowance_path_t path = paths->data[i];
      if (path.flow.has_live || path.flow.has_done)
        (void)stmt_ownership_allowance_segment_valid(cg, fn_stmt, allowance,
                                                     path.flow, path.unbounded);
    }
    paths->len = 0;
  }
}

static void
stmt_ownership_check_allowance_epochs(codegen_t *cg, const stmt_t *fn_stmt,
                                      const ny_allowance_t *allowance) {
  stmt_ownership_allowance_paths_t paths = {0};
  stmt_ownership_allowance_path_t initial = {.flow = {.has_live = true},
                                             .unbounded = false};
  stmt_ownership_allowance_paths_append(&paths, &initial, 1);
  stmt_ownership_allowance_process_stmt(cg, fn_stmt, allowance, &paths,
                                        fn_stmt->as.fn.body, 0);
  if (paths.inconclusive) {
    stmt_ownership_diag(
        cg, fn_stmt->tok,
        "cannot validate ownership allowance for '%s': path analysis is "
        "inconclusive (path or nesting limit, or reassignment in a loop)",
        allowance->name);
  } else if (paths.len) {
    stmt_ownership_min_flow_t combined = {.has_live = false};
    bool unbounded = false;
    for (size_t i = 0; i < paths.len; ++i) {
      combined = stmt_ownership_min_flow_union(combined, paths.data[i].flow);
      unbounded |= paths.data[i].unbounded;
    }
    (void)stmt_ownership_allowance_segment_valid(cg, fn_stmt, allowance,
                                                 combined, unbounded);
  }
  free(paths.data);
}

static void stmt_ownership_check_allowances(codegen_t *cg,
                                            const stmt_t *fn_stmt) {
  if (!cg || !fn_stmt || fn_stmt->kind != NY_S_FUNC)
    return;
  const stmt_func_t *fn = &fn_stmt->as.fn;
  if (fn->attr_allowances.len == 0 || !fn->body)
    return;
  for (size_t i = 0; i < fn->attr_allowances.len; i++) {
    const ny_allowance_t *a = &fn->attr_allowances.data[i];
    if (!a->name || a->lo < 0 || (a->hi >= 0 && a->lo > a->hi))
      continue;
    stmt_ownership_check_allowance_epochs(cg, fn_stmt, a);
  }
}

static void stmt_ownership_rules_error(codegen_t *cg, token_t tok,
                                       const char *rule, const char *message);
static void stmt_ownership_check_resource_value(codegen_t *cg, const char *name,
                                                const char *qualifier,
                                                const stmt_t *body,
                                                token_t tok);
static bool stmt_ownership_rules_stmt_mentions_name(const stmt_t *s,
                                                    const char *name,
                                                    unsigned depth);

static const char *stmt_ownership_resource_qualifier_depth(codegen_t *cg,
                                                           const char *type,
                                                           unsigned depth);

static bool stmt_ownership_type_substitute_enum_params(
    const enum_def_t *def, const char *concrete_type, const char *field_type,
    char *out, size_t out_cap) {
  if (!def || !concrete_type || !field_type || !out || out_cap == 0)
    return false;
  out[0] = '\0';
  const char *open = strchr(concrete_type, '<');
  const char *close = open ? strrchr(open + 1, '>') : NULL;
  if (!open || !close || close <= open)
    return false;
  const char *args[16];
  size_t arg_lens[16];
  size_t arg_count = 0;
  const char *part = open + 1;
  unsigned nested = 0;
  for (const char *p = part; p <= close; ++p) {
    if (p == close || (*p == ',' && nested == 0)) {
      if (arg_count >= sizeof(args) / sizeof(args[0]))
        return false;
      args[arg_count] = part;
      arg_lens[arg_count++] = (size_t)(p - part);
      part = p + 1;
    } else if (*p == '<') {
      nested++;
    } else if (*p == '>' && nested > 0) {
      nested--;
    }
  }
  size_t used = 0;
  const char *p = field_type;
  while (*p) {
    if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_') {
      const char *start = p++;
      while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
             (*p >= '0' && *p <= '9') || *p == '_')
        p++;
      size_t token_len = (size_t)(p - start);
      const char *replacement = NULL;
      size_t replacement_len = 0;
      for (size_t i = 0; i < def->type_params.len && i < arg_count; ++i) {
        const char *param = def->type_params.data[i];
        if (param && strlen(param) == token_len &&
            memcmp(param, start, token_len) == 0) {
          replacement = args[i];
          replacement_len = arg_lens[i];
          break;
        }
      }
      size_t append_len = replacement ? replacement_len : token_len;
      if (append_len >= out_cap - used)
        return false;
      memcpy(out + used, replacement ? replacement : start, append_len);
      used += append_len;
    } else {
      if (used + 1 >= out_cap)
        return false;
      out[used++] = *p++;
    }
  }
  out[used] = '\0';
  return true;
}

static const char *stmt_ownership_resource_qualifier_depth(codegen_t *cg,
                                                           const char *type,
                                                           unsigned depth) {
  if (!cg || !type || depth > 16)
    return NULL;
  while (*type == '?' || *type == '*' || *type == '&')
    type++;
  while (*type == ' ')
    type++;
  size_t full_len = strlen(type);
  while (full_len && type[full_len - 1] == ' ')
    full_len--;
  if (full_len == 0 || full_len >= 512)
    return NULL;
  char full_type[512];
  memcpy(full_type, type, full_len);
  full_type[full_len] = '\0';

  unsigned nesting = 0;
  for (size_t i = 0; i < full_len; ++i) {
    if (full_type[i] == '<' || full_type[i] == '[')
      nesting++;
    else if ((full_type[i] == '>' || full_type[i] == ']') && nesting)
      nesting--;
    else if (full_type[i] == '|' && nesting == 0) {
      full_type[i] = '\0';
      const char *left =
          stmt_ownership_resource_qualifier_depth(cg, full_type, depth + 1);
      const char *right = stmt_ownership_resource_qualifier_depth(
          cg, full_type + i + 1, depth + 1);
      if (left && strcmp(left, "linear") == 0)
        return left;
      if (right && strcmp(right, "linear") == 0)
        return right;
      return left ? left : right;
    }
  }

  const char *generic = strchr(full_type, '<');
  size_t base_len = generic ? (size_t)(generic - full_type) : strlen(full_type);
  if (base_len == 0 || base_len >= 256)
    return NULL;
  char base_type[256];
  memcpy(base_type, full_type, base_len);
  base_type[base_len] = '\0';

  /*
   * Generic containers inherit the strongest resource qualifier of their
   * type arguments.  Layout and enum declarations below refine this using
   * their actual fields; this fallback covers list<T>, tuple<T>, and
   * user-defined generic wrappers without a resource-specific declaration.
   */
  const char *generic_qualifier = NULL;
  if (generic) {
    const char *arg_start = generic + 1;
    unsigned angle = 0, bracket = 0, paren = 0;
    for (const char *p = arg_start; p <= full_type + full_len; ++p) {
      bool at_end = p == full_type + full_len;
      if (!at_end && *p == '<')
        ++angle;
      else if (!at_end && *p == '>') {
        if (angle)
          --angle;
        else if (bracket == 0 && paren == 0) {
          size_t arg_len = (size_t)(p - arg_start);
          if (arg_len > 0 && arg_len < sizeof(full_type)) {
            char arg_type[sizeof(full_type)];
            memcpy(arg_type, arg_start, arg_len);
            arg_type[arg_len] = '\0';
            const char *nested = stmt_ownership_resource_qualifier_depth(
                cg, arg_type, depth + 1);
            if (nested && strcmp(nested, "linear") == 0)
              return nested;
            if (nested)
              generic_qualifier = nested;
          }
          break;
        }
      } else if (!at_end && *p == '[')
        ++bracket;
      else if (!at_end && *p == ']' && bracket)
        --bracket;
      else if (!at_end && *p == '(')
        ++paren;
      else if (!at_end && *p == ')' && paren)
        --paren;

      if ((at_end || (*p == ',' && angle == 0 && bracket == 0 && paren == 0))) {
        size_t arg_len = (size_t)(p - arg_start);
        if (arg_len > 0 && arg_len < sizeof(full_type)) {
          char arg_type[sizeof(full_type)];
          memcpy(arg_type, arg_start, arg_len);
          arg_type[arg_len] = '\0';
          const char *nested =
              stmt_ownership_resource_qualifier_depth(cg, arg_type, depth + 1);
          if (nested && strcmp(nested, "linear") == 0)
            return nested;
          if (nested)
            generic_qualifier = nested;
        }
        arg_start = p + 1;
      }
    }
  }

  layout_def_t *layout = lookup_layout(cg, base_type);
  if (layout && layout->stmt && layout->stmt->kind == NY_S_LAYOUT) {
    const char *qualifier = layout->stmt->as.layout.resource_qualifier;
    if (qualifier &&
        (strcmp(qualifier, "linear") == 0 || strcmp(qualifier, "affine") == 0))
      return qualifier;
    for (size_t i = 0; i < layout->fields.len; ++i) {
      const char *nested = stmt_ownership_resource_qualifier_depth(
          cg, layout->fields.data[i].type_name, depth + 1);
      if (nested && strcmp(nested, "linear") == 0)
        return nested;
      if (nested)
        qualifier = nested;
    }
    if (qualifier &&
        (strcmp(qualifier, "linear") == 0 || strcmp(qualifier, "affine") == 0))
      return qualifier;
  }

  const char *base_leaf = strrchr(base_type, '.');
  base_leaf = base_leaf ? base_leaf + 1 : base_type;
  for (size_t i = 0; i < cg->enums.len; ++i) {
    enum_def_t *def = cg->enums.data[i];
    if (!def || !def->name)
      continue;
    const char *enum_leaf = strrchr(def->name, '.');
    enum_leaf = enum_leaf ? enum_leaf + 1 : def->name;
    if (strcmp(def->name, base_type) != 0 &&
        (strchr(base_type, '.') || strcmp(enum_leaf, base_leaf) != 0))
      continue;
    const char *found = NULL;
    for (size_t m = 0; m < def->members.len; ++m) {
      const enum_member_def_t *member = &def->members.data[m];
      for (size_t f = 0; f < member->fields.len; ++f) {
        const char *field_type = member->fields.data[f].type_name;
        char substituted[512];
        if (generic && field_type &&
            stmt_ownership_type_substitute_enum_params(
                def, full_type, field_type, substituted, sizeof(substituted)))
          field_type = substituted;
        const char *nested =
            stmt_ownership_resource_qualifier_depth(cg, field_type, depth + 1);
        if (nested && strcmp(nested, "linear") == 0)
          return nested;
        if (nested)
          found = nested;
      }
    }
    return found;
  }
  return generic_qualifier;
}

static const char *stmt_ownership_resource_qualifier(codegen_t *cg,
                                                     const char *type) {
  return stmt_ownership_resource_qualifier_depth(cg, type, 0);
}

static const char *
stmt_ownership_resource_declared_qualifier(codegen_t *cg, const char *type) {
  if (!cg || !type)
    return NULL;
  while (*type == '?' || *type == '*' || *type == '&' || *type == ' ')
    ++type;
  const char *generic = strchr(type, '<');
  size_t len = generic ? (size_t)(generic - type) : strlen(type);
  if (!len || len >= 256)
    return NULL;
  char base[256];
  memcpy(base, type, len);
  base[len] = '\0';
  layout_def_t *layout = lookup_layout(cg, base);
  if (!layout || !layout->stmt || layout->stmt->kind != NY_S_LAYOUT)
    return NULL;
  const char *qualifier = layout->stmt->as.layout.resource_qualifier;
  return qualifier && (strcmp(qualifier, "linear") == 0 ||
                       strcmp(qualifier, "affine") == 0)
             ? qualifier
             : NULL;
}

static bool
stmt_ownership_check_resource_type_paths(codegen_t *cg, const char *path,
                                         const char *type, const stmt_t *body,
                                         token_t tok, unsigned depth) {
  if (!cg || !path || !type || !body || depth > 16)
    return false;
  while (*type == '?' || *type == '*' || *type == '&' || *type == ' ')
    ++type;
  size_t type_len = strlen(type);
  while (type_len && type[type_len - 1] == ' ')
    --type_len;
  const char *generic = memchr(type, '<', type_len);
  size_t base_len = generic ? (size_t)(generic - type) : type_len;
  if (!base_len || base_len >= 256)
    return false;
  char base[256];
  memcpy(base, type, base_len);
  base[base_len] = '\0';

  layout_def_t *layout = lookup_layout(cg, base);
  bool has_nested_resource = false;
  if (layout && layout->stmt && layout->stmt->kind == NY_S_LAYOUT) {
    const char *declared = stmt_ownership_resource_declared_qualifier(cg, type);
    if (declared)
      stmt_ownership_check_resource_value(cg, path, declared, body, tok);
    for (size_t i = 0; i < layout->fields.len; ++i) {
      const layout_field_info_t *field = &layout->fields.data[i];
      if (!field->name || !field->type_name)
        continue;
      size_t path_len = strlen(path);
      size_t field_len = strlen(field->name);
      if (path_len + field_len + 2 > 512)
        continue;
      char field_path[512];
      memcpy(field_path, path, path_len);
      field_path[path_len] = '.';
      memcpy(field_path + path_len + 1, field->name, field_len + 1);
      if (stmt_ownership_check_resource_type_paths(
              cg, field_path, field->type_name, body, tok, depth + 1))
        has_nested_resource = true;
    }
    if (declared || has_nested_resource)
      return true;
  }

  const char *qualifier = stmt_ownership_resource_qualifier(cg, type);
  if (qualifier)
    stmt_ownership_check_resource_value(cg, path, qualifier, body, tok);
  return qualifier != NULL;
}

static const char *stmt_ownership_resource_prior_local_qualifier(
    codegen_t *cg, const stmt_block_t *block, size_t before, const char *name,
    unsigned depth);
static const char *stmt_ownership_resource_prior_local_type(
    codegen_t *cg, const stmt_block_t *block, size_t before, const char *name,
    unsigned depth);

static const char *
stmt_ownership_resource_initializer_qualifier(codegen_t *cg, const expr_t *init,
                                              const stmt_block_t *block,
                                              size_t before, unsigned depth) {
  if (!cg || !init || depth > 128)
    return NULL;
  const char *aggregate_qualifier = NULL;
  if (init->kind == NY_E_LIST || init->kind == NY_E_TUPLE ||
      init->kind == NY_E_SET) {
    for (size_t i = 0; i < init->as.list_like.len; ++i) {
      const char *nested = stmt_ownership_resource_initializer_qualifier(
          cg, init->as.list_like.data[i], block, before, depth + 1);
      if (nested && strcmp(nested, "linear") == 0)
        return nested;
      if (nested)
        aggregate_qualifier = nested;
    }
    return aggregate_qualifier;
  }
  if (init->kind == NY_E_DICT) {
    for (size_t i = 0; i < init->as.dict.pairs.len; ++i) {
      const char *key = stmt_ownership_resource_initializer_qualifier(
          cg, init->as.dict.pairs.data[i].key, block, before, depth + 1);
      const char *value = stmt_ownership_resource_initializer_qualifier(
          cg, init->as.dict.pairs.data[i].value, block, before, depth + 1);
      if ((key && strcmp(key, "linear") == 0) ||
          (value && strcmp(value, "linear") == 0))
        return "linear";
      if (key)
        aggregate_qualifier = key;
      if (value)
        aggregate_qualifier = value;
    }
    return aggregate_qualifier;
  }
  if (init->kind == NY_E_IDENT && init->as.ident.name)
    return stmt_ownership_resource_prior_local_qualifier(
        cg, block, before, init->as.ident.name, depth + 1);
  const char *type = infer_expr_type(cg, NULL, 0, (expr_t *)init);
  return stmt_ownership_resource_qualifier(cg, type);
}

static bool stmt_ownership_resource_expr_captures(const expr_t *e,
                                                  const char *name,
                                                  unsigned depth);

static bool stmt_ownership_resource_stmt_captures(const stmt_t *s,
                                                  const char *name,
                                                  unsigned depth) {
  if (!s || !name || depth > 128)
    return false;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (stmt_ownership_resource_stmt_captures(s->as.block.body.data[i], name,
                                                depth + 1))
        return true;
    return false;
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; ++i)
      if (stmt_ownership_resource_expr_captures(s->as.var.exprs.data[i], name,
                                                depth + 1))
        return true;
    return false;
  case NY_S_EXPR:
    return stmt_ownership_resource_expr_captures(s->as.expr.expr, name,
                                                 depth + 1);
  case NY_S_RETURN:
    return stmt_ownership_resource_expr_captures(s->as.ret.value, name,
                                                 depth + 1);
  case NY_S_IF:
    return stmt_ownership_resource_expr_captures(s->as.iff.test, name,
                                                 depth + 1) ||
           stmt_ownership_resource_stmt_captures(s->as.iff.init, name,
                                                 depth + 1) ||
           stmt_ownership_resource_stmt_captures(s->as.iff.conseq, name,
                                                 depth + 1) ||
           stmt_ownership_resource_stmt_captures(s->as.iff.alt, name,
                                                 depth + 1);
  case NY_S_GUARD:
    return stmt_ownership_resource_expr_captures(s->as.guard.value, name,
                                                 depth + 1) ||
           stmt_ownership_resource_stmt_captures(s->as.guard.fallback, name,
                                                 depth + 1);
  case NY_S_WHILE:
    return stmt_ownership_resource_expr_captures(s->as.whl.test, name,
                                                 depth + 1) ||
           stmt_ownership_resource_stmt_captures(s->as.whl.init, name,
                                                 depth + 1) ||
           stmt_ownership_resource_stmt_captures(s->as.whl.body, name,
                                                 depth + 1) ||
           stmt_ownership_resource_stmt_captures(s->as.whl.update, name,
                                                 depth + 1);
  case NY_S_FOR:
    return stmt_ownership_resource_expr_captures(s->as.fr.iterable, name,
                                                 depth + 1) ||
           stmt_ownership_resource_expr_captures(s->as.fr.cond, name,
                                                 depth + 1) ||
           stmt_ownership_resource_stmt_captures(s->as.fr.init, name,
                                                 depth + 1) ||
           stmt_ownership_resource_stmt_captures(s->as.fr.body, name,
                                                 depth + 1) ||
           stmt_ownership_resource_stmt_captures(s->as.fr.update, name,
                                                 depth + 1);
  case NY_S_TRY:
    return stmt_ownership_resource_stmt_captures(s->as.tr.body, name,
                                                 depth + 1) ||
           stmt_ownership_resource_stmt_captures(s->as.tr.handler, name,
                                                 depth + 1);
  case NY_S_MATCH:
    if (stmt_ownership_resource_expr_captures(s->as.match.test, name,
                                              depth + 1))
      return true;
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      const match_arm_t *arm = &s->as.match.arms.data[i];
      if (stmt_ownership_resource_expr_captures(arm->guard, name, depth + 1) ||
          stmt_ownership_resource_stmt_captures(arm->conseq, name, depth + 1))
        return true;
    }
    return stmt_ownership_resource_stmt_captures(s->as.match.default_conseq,
                                                 name, depth + 1);
  case NY_S_DEFER:
    return stmt_ownership_resource_stmt_captures(s->as.de.body, name,
                                                 depth + 1);
  default:
    return false;
  }
}

static bool stmt_ownership_resource_expr_captures(const expr_t *e,
                                                  const char *name,
                                                  unsigned depth) {
  if (!e || !name)
    return false;
  if (depth > 128)
    return true;
#define NY_RESOURCE_CAPTURE(child)                                             \
  stmt_ownership_resource_expr_captures((child), name, depth + 1)
  switch (e->kind) {
  case NY_E_UNARY:
    return NY_RESOURCE_CAPTURE(e->as.unary.right);
  case NY_E_BINARY:
  case NY_E_LOGICAL:
    return NY_RESOURCE_CAPTURE(e->as.binary.left) ||
           NY_RESOURCE_CAPTURE(e->as.binary.right);
  case NY_E_TERNARY:
    return NY_RESOURCE_CAPTURE(e->as.ternary.cond) ||
           NY_RESOURCE_CAPTURE(e->as.ternary.true_expr) ||
           NY_RESOURCE_CAPTURE(e->as.ternary.false_expr);
  case NY_E_CALL:
    if (NY_RESOURCE_CAPTURE(e->as.call.callee))
      return true;
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      if (NY_RESOURCE_CAPTURE(e->as.call.args.data[i].val))
        return true;
    return false;
  case NY_E_MEMCALL:
    if (NY_RESOURCE_CAPTURE(e->as.memcall.target))
      return true;
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      if (NY_RESOURCE_CAPTURE(e->as.memcall.args.data[i].val))
        return true;
    return false;
  case NY_E_INDEX:
    return NY_RESOURCE_CAPTURE(e->as.index.target) ||
           NY_RESOURCE_CAPTURE(e->as.index.start) ||
           NY_RESOURCE_CAPTURE(e->as.index.stop) ||
           NY_RESOURCE_CAPTURE(e->as.index.step);
  case NY_E_MEMBER:
    return NY_RESOURCE_CAPTURE(e->as.member.target) ||
           NY_RESOURCE_CAPTURE(e->as.member.computed_name);
  case NY_E_LAMBDA:
  case NY_E_FN:
    for (size_t i = 0; i < e->as.lambda.params.len; ++i)
      if (e->as.lambda.params.data[i].name &&
          strcmp(e->as.lambda.params.data[i].name, name) == 0)
        return false;
    return stmt_ownership_rules_stmt_mentions_name(e->as.lambda.body, name,
                                                   depth + 1);
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      if (NY_RESOURCE_CAPTURE(e->as.list_like.data[i]))
        return true;
    return false;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i)
      if (NY_RESOURCE_CAPTURE(e->as.dict.pairs.data[i].key) ||
          NY_RESOURCE_CAPTURE(e->as.dict.pairs.data[i].value))
        return true;
    return false;
  case NY_E_ASM:
    for (size_t i = 0; i < e->as.as_asm.args.len; ++i)
      if (NY_RESOURCE_CAPTURE(e->as.as_asm.args.data[i]))
        return true;
    return false;
  case NY_E_COMPTIME:
    return stmt_ownership_resource_stmt_captures(e->as.comptime_expr.body, name,
                                                 depth + 1);
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; ++i)
      if (e->as.fstring.parts.data[i].kind == NY_FSP_EXPR &&
          NY_RESOURCE_CAPTURE(e->as.fstring.parts.data[i].as.e))
        return true;
    return false;
  case NY_E_MATCH:
    if (NY_RESOURCE_CAPTURE(e->as.match.test))
      return true;
    for (size_t i = 0; i < e->as.match.arms.len; ++i) {
      const match_arm_t *arm = &e->as.match.arms.data[i];
      if (NY_RESOURCE_CAPTURE(arm->guard) ||
          stmt_ownership_resource_stmt_captures(arm->conseq, name, depth + 1))
        return true;
    }
    return stmt_ownership_resource_stmt_captures(e->as.match.default_conseq,
                                                 name, depth + 1);
  case NY_E_PTR_TYPE:
    return NY_RESOURCE_CAPTURE(e->as.ptr_type.target);
  case NY_E_DEREF:
    return NY_RESOURCE_CAPTURE(e->as.deref.target);
  case NY_E_SIZEOF:
    return NY_RESOURCE_CAPTURE(e->as.szof.target);
  case NY_E_TRY:
    return NY_RESOURCE_CAPTURE(e->as.try_expr.target);
  case NY_E_QUOTE:
    return stmt_ownership_resource_stmt_captures(e->as.quote.body, name,
                                                 depth + 1) ||
           NY_RESOURCE_CAPTURE(e->as.quote.expr);
  case NY_E_SPLICE:
    return NY_RESOURCE_CAPTURE(e->as.splice.expr);
  case NY_E_EXISTS:
    return NY_RESOURCE_CAPTURE(e->as.exists.body);
  default:
    return false;
  }
#undef NY_RESOURCE_CAPTURE
}

enum {
  NY_RESOURCE_LIVE_UNCONSUMED = 1u << 0,
  NY_RESOURCE_LIVE_CONSUMED = 1u << 1,
  NY_RESOURCE_DONE_UNCONSUMED = 1u << 2,
  NY_RESOURCE_DONE_CONSUMED = 1u << 3,
};

static bool stmt_ownership_resource_is_name(const expr_t *e, const char *name) {
  return e && e->kind == NY_E_IDENT && e->as.ident.name && name &&
         strcmp(e->as.ident.name, name) == 0;
}

static bool stmt_ownership_resource_place_text(const expr_t *e, char *out,
                                               size_t cap, size_t *len) {
  if (!e || !out || !len || cap == 0)
    return false;
  if (e->kind == NY_E_IDENT) {
    const char *name = e->as.ident.name;
    size_t n = name ? strlen(name) : 0;
    if (!n || n >= cap)
      return false;
    memcpy(out, name, n + 1);
    *len = n;
    return true;
  }
  if (e->kind == NY_E_INDEX) {
    const expr_t *index = e->as.index.start;
    int64_t index_value = 0;
    if (!index || e->as.index.stop || e->as.index.step)
      return false;
    if (index->kind == NY_E_LITERAL && index->as.literal.kind == NY_LIT_STR &&
        index->as.literal.as.s.data) {
      size_t n = 0;
      if (cap < 4 ||
          !stmt_ownership_resource_place_text(e->as.index.target, out, cap,
                                              &n) ||
          n > cap - 4 || index->as.literal.as.s.len > (cap - n - 4) / 2)
        return false;
      static const char hex[] = "0123456789abcdef";
      out[n++] = '[';
      out[n++] = '@';
      for (size_t i = 0; i < index->as.literal.as.s.len; ++i) {
        unsigned char byte = (unsigned char)index->as.literal.as.s.data[i];
        out[n++] = hex[byte >> 4];
        out[n++] = hex[byte & 0x0f];
      }
      out[n++] = ']';
      out[n] = '\0';
      *len = n;
      return true;
    }
    if (index->kind == NY_E_LITERAL && index->as.literal.kind == NY_LIT_INT &&
        !index->as.literal.integer_outside_i64) {
      index_value = index->as.literal.as.i;
    } else if (index->kind == NY_E_UNARY && index->as.unary.op &&
               strcmp(index->as.unary.op, "-") == 0 && index->as.unary.right &&
               index->as.unary.right->kind == NY_E_LITERAL &&
               index->as.unary.right->as.literal.kind == NY_LIT_INT &&
               !index->as.unary.right->as.literal.integer_outside_i64 &&
               index->as.unary.right->as.literal.as.i >= 0 &&
               index->as.unary.right->as.literal.as.i < INT64_MAX) {
      index_value = -index->as.unary.right->as.literal.as.i;
    } else {
      return false;
    }
    size_t n = 0;
    if (!stmt_ownership_resource_place_text(e->as.index.target, out, cap, &n))
      return false;
    char index_text[32];
    int index_len = snprintf(index_text, sizeof(index_text), "%lld",
                             (long long)index_value);
    if (index_len <= 0 || (size_t)index_len >= sizeof(index_text) ||
        n + (size_t)index_len + 3 > cap)
      return false;
    out[n++] = '[';
    memcpy(out + n, index_text, (size_t)index_len);
    n += (size_t)index_len;
    out[n++] = ']';
    out[n] = '\0';
    *len = n;
    return true;
  }
  if (e->kind != NY_E_MEMBER || !e->as.member.name)
    return false;
  size_t n = 0;
  if (!stmt_ownership_resource_place_text(e->as.member.target, out, cap, &n))
    return false;
  size_t field_len = strlen(e->as.member.name);
  if (field_len == 0 || n + field_len + 2 > cap)
    return false;
  out[n++] = '.';
  memcpy(out + n, e->as.member.name, field_len + 1);
  *len = n + field_len;
  return true;
}

/*
 * Fold only checked literal integer expressions; unknown arithmetic stays
 * conservative in borrow-place analysis.
 */
static bool stmt_ownership_exclusive_index_constant(const expr_t *e,
                                                    int64_t *value,
                                                    unsigned depth) {
  if (!e || !value || depth > 64)
    return false;
  if (e->kind == NY_E_LITERAL) {
    if (e->as.literal.kind != NY_LIT_INT || e->as.literal.integer_outside_i64)
      return false;
    *value = e->as.literal.as.i;
    return true;
  }
  if (e->kind == NY_E_UNARY && e->as.unary.op && e->as.unary.right) {
    int64_t operand = 0;
    if (!stmt_ownership_exclusive_index_constant(e->as.unary.right, &operand,
                                                 depth + 1))
      return false;
    if (strcmp(e->as.unary.op, "+") == 0) {
      *value = operand;
      return true;
    }
    if (strcmp(e->as.unary.op, "-") == 0 && operand != INT64_MIN) {
      *value = -operand;
      return true;
    }
    return false;
  }
  if (e->kind != NY_E_BINARY || !e->as.binary.op || !e->as.binary.left ||
      !e->as.binary.right)
    return false;
  int64_t left = 0, right = 0;
  if (!stmt_ownership_exclusive_index_constant(e->as.binary.left, &left,
                                               depth + 1) ||
      !stmt_ownership_exclusive_index_constant(e->as.binary.right, &right,
                                               depth + 1))
    return false;
  const char *op = e->as.binary.op;
  if (strcmp(op, "+") == 0)
    return !__builtin_add_overflow(left, right, value);
  if (strcmp(op, "-") == 0)
    return !__builtin_sub_overflow(left, right, value);
  if (strcmp(op, "*") == 0)
    return !__builtin_mul_overflow(left, right, value);
  if (strcmp(op, "/") == 0 || strcmp(op, "%") == 0) {
    if (right == 0 || (left == INT64_MIN && right == -1))
      return false;
    *value = strcmp(op, "/") == 0 ? left / right : left % right;
    return true;
  }
  return false;
}

static bool stmt_ownership_rules_is_borrow_expr(const expr_t *e);
static const expr_t *stmt_ownership_rules_borrow_source(const expr_t *e);

static bool stmt_ownership_exclusive_place_text(const expr_t *e, char *out,
                                                size_t cap, size_t *len) {
  if (!e || !out || !len || cap == 0)
    return false;
  if (e->kind == NY_E_IDENT)
    return stmt_ownership_resource_place_text(e, out, cap, len);
  if (e->kind == NY_E_DEREF) {
    const expr_t *address = e->as.deref.target;
    if (!stmt_ownership_rules_is_borrow_expr(address))
      return false;
    const expr_t *source = stmt_ownership_rules_borrow_source(address);
    return source && stmt_ownership_exclusive_place_text(source, out, cap, len);
  }
  if (e->kind == NY_E_UNARY && e->as.unary.op &&
      strcmp(e->as.unary.op, "*") == 0) {
    const expr_t *address = e->as.unary.right;
    if (!stmt_ownership_rules_is_borrow_expr(address))
      return false;
    const expr_t *source = stmt_ownership_rules_borrow_source(address);
    return source && stmt_ownership_exclusive_place_text(source, out, cap, len);
  }
  if (e->kind == NY_E_MEMBER) {
    size_t n = 0;
    if (!e->as.member.name ||
        !stmt_ownership_exclusive_place_text(e->as.member.target, out, cap, &n))
      return false;
    size_t field_len = strlen(e->as.member.name);
    if (field_len == 0 || n + field_len + 2 > cap)
      return false;
    out[n++] = '.';
    memcpy(out + n, e->as.member.name, field_len + 1);
    *len = n + field_len;
    return true;
  }
  if (e->kind != NY_E_INDEX || !e->as.index.start || e->as.index.stop ||
      e->as.index.step)
    return false;
  const expr_t *index = e->as.index.start;
  if (index->kind == NY_E_LITERAL && index->as.literal.kind == NY_LIT_STR &&
      index->as.literal.as.s.data)
    return stmt_ownership_resource_place_text(e, out, cap, len);
  int64_t index_value = 0;
  bool constant_index =
      stmt_ownership_exclusive_index_constant(index, &index_value, 0);
  if (!constant_index && (index->kind != NY_E_IDENT || !index->as.ident.name)) {
    return false;
  }
  size_t n = 0;
  if (!stmt_ownership_exclusive_place_text(e->as.index.target, out, cap, &n))
    return false;
  char index_text[32];
  const char *suffix = "[*]";
  size_t suffix_len = 3;
  if (constant_index) {
    int index_len = snprintf(index_text, sizeof(index_text), "[%lld]",
                             (long long)index_value);
    if (index_len <= 0 || (size_t)index_len >= sizeof(index_text))
      return false;
    suffix = index_text;
    suffix_len = (size_t)index_len;
  }
  if (n + suffix_len + 1 > cap)
    return false;
  memcpy(out + n, suffix, suffix_len + 1);
  *len = n + suffix_len;
  return true;
}

static bool stmt_ownership_resource_place_matches(const expr_t *e,
                                                  const char *path) {
  char place[512];
  size_t len = 0;
  return path &&
         stmt_ownership_resource_place_text(e, place, sizeof(place), &len) &&
         strcmp(place, path) == 0;
}

static bool stmt_ownership_resource_place_covers(const expr_t *e,
                                                 const char *path) {
  char place[512];
  size_t len = 0;
  if (!path ||
      !stmt_ownership_resource_place_text(e, place, sizeof(place), &len) ||
      len > strlen(path) || memcmp(place, path, len) != 0)
    return false;
  return path[len] == '\0' || path[len] == '.' || path[len] == '[';
}

static bool stmt_ownership_resource_paths_overlap(const char *left,
                                                  const char *right) {
  if (!left || !right)
    return false;
  static const char unknown_exclusive_place[] = "\001unknown-exclusive-place";
  if (strcmp(left, unknown_exclusive_place) == 0 ||
      strcmp(right, unknown_exclusive_place) == 0)
    return true;
  const char *l = left;
  const char *r = right;
  while (*l && *r) {
    if (*l == '[' && *r == '[') {
      const char *lend_key = l[1] == '@' ? strchr(l + 2, ']') : NULL;
      const char *rend_key = r[1] == '@' ? strchr(r + 2, ']') : NULL;
      bool lkey = lend_key != NULL;
      bool rkey = rend_key != NULL;
      if (lkey || rkey) {
        if (!lkey || !rkey)
          return true;
        size_t lkey_len = (size_t)(lend_key - (l + 2));
        size_t rkey_len = (size_t)(rend_key - (r + 2));
        if ((lkey_len & 1) != 0 || (rkey_len & 1) != 0)
          return true;
        for (size_t i = 0; i < lkey_len; ++i)
          if (!((l[i + 2] >= '0' && l[i + 2] <= '9') ||
                (l[i + 2] >= 'a' && l[i + 2] <= 'f')))
            return true;
        for (size_t i = 0; i < rkey_len; ++i)
          if (!((r[i + 2] >= '0' && r[i + 2] <= '9') ||
                (r[i + 2] >= 'a' && r[i + 2] <= 'f')))
            return true;
        if (lkey_len != rkey_len || memcmp(l + 2, r + 2, lkey_len) != 0)
          return false;
        l = lend_key + 1;
        r = rend_key + 1;
        continue;
      }
      char *lend = NULL;
      char *rend = NULL;
      errno = 0;
      long long li = strtoll(l + 1, &lend, 10);
      bool lvalid = !errno && lend && lend != l + 1 && *lend == ']';
      errno = 0;
      long long ri = strtoll(r + 1, &rend, 10);
      bool rvalid = !errno && rend && rend != r + 1 && *rend == ']';
      bool lwild = l[1] == '*' && l[2] == ']';
      bool rwild = r[1] == '*' && r[2] == ']';
      if ((!lvalid && !lwild) || (!rvalid && !rwild))
        return true;
      if (lvalid && rvalid && li != ri) {
        /*
         * Same-sign constant indices are distinct even for negative list
         * indices. A negative and nonnegative index may alias depending on
         * the collection length, so retain the conservative overlap.
         */
        return (li < 0) != (ri < 0);
      }
      l = lvalid ? lend + 1 : l + 3;
      r = rvalid ? rend + 1 : r + 3;
      continue;
    }
    if (*l == *r) {
      ++l;
      ++r;
      continue;
    }
    return false;
  }
  if (*l == '\0' && *r == '\0')
    return true;
  if (*l == '\0')
    return *r == '.' || *r == '[';
  return *l == '.' || *l == '[';
}

static bool stmt_ownership_resource_path_mentions_stmt(const stmt_t *s,
                                                       const char *path,
                                                       unsigned depth);

static size_t stmt_ownership_resource_call_consume_count(codegen_t *cg,
                                                         const expr_t *e,
                                                         const char *name) {
  if (!cg || !e || e->kind != NY_E_CALL || !name)
    return 0;
  const char *leaf = stmt_call_tail_name((expr_t *)e);
  if (leaf && (strcmp(leaf, "own") == 0 || strcmp(leaf, "release") == 0 ||
               strcmp(leaf, "forget") == 0)) {
    expr_t *arg = stmt_ownership_unary_arg((expr_t *)e, leaf);
    return stmt_ownership_resource_place_covers(arg, name) ? 1 : 0;
  }
  fun_sig *sig = stmt_ownership_resolve_call_sig(cg, (expr_t *)e);
  if (!sig)
    return 0;
  size_t count = 0;
  for (size_t i = 0; i < e->as.call.args.len; ++i) {
    const call_arg_t *arg = &e->as.call.args.data[i];
    const char *param = arg->name ? arg->name : stmt_sig_param_name(sig, i);
    bool consuming = param && (stmt_sig_contract_has(&sig->consumes, param) ||
                               stmt_sig_contract_has(&sig->releases, param) ||
                               stmt_sig_contract_has(&sig->forgets, param));
    if (consuming && stmt_ownership_resource_place_covers(arg->val, name))
      ++count;
  }
  return count;
}

/*
 * A field read observes a resource; it does not transfer or release it.
 */
static bool stmt_ownership_resource_expr_consumes(codegen_t *cg,
                                                  const expr_t *e,
                                                  const char *name) {
  if (!e || !name)
    return false;
  expr_t *arg = NULL;
  if (e->kind == NY_E_CALL) {
    if (stmt_ownership_resource_call_consume_count(cg, e, name) > 0)
      return true;
    arg = stmt_ownership_consumes_arg(cg, (expr_t *)e);
    if (!arg)
      arg = stmt_ownership_releases_arg(cg, (expr_t *)e);
    if (!arg)
      arg = stmt_ownership_forgets_arg(cg, (expr_t *)e);
    if (!arg)
      arg = stmt_ownership_unary_arg((expr_t *)e, "own");
    if (stmt_ownership_resource_is_name(arg, name))
      return true;
    if (stmt_expr_is_adt_ctor(cg, (expr_t *)e)) {
      for (size_t i = 0; i < e->as.call.args.len; ++i)
        if (stmt_ownership_resource_is_name(e->as.call.args.data[i].val, name))
          return true;
    }
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      if (stmt_ownership_resource_expr_consumes(cg, e->as.call.args.data[i].val,
                                                name))
        return true;
    return false;
  }
  switch (e->kind) {
  case NY_E_UNARY:
    return stmt_ownership_resource_expr_consumes(cg, e->as.unary.right, name);
  case NY_E_BINARY:
    return stmt_ownership_resource_expr_consumes(cg, e->as.binary.left, name) ||
           stmt_ownership_resource_expr_consumes(cg, e->as.binary.right, name);
  case NY_E_LOGICAL:
    /*
     * A consume in the short-circuited operand is not guaranteed.
     */
    return stmt_ownership_resource_expr_consumes(cg, e->as.logical.left, name);
  case NY_E_TERNARY:
    return stmt_ownership_resource_expr_consumes(cg, e->as.ternary.cond,
                                                 name) ||
           (stmt_ownership_resource_expr_consumes(cg, e->as.ternary.true_expr,
                                                  name) &&
            stmt_ownership_resource_expr_consumes(cg, e->as.ternary.false_expr,
                                                  name));
  case NY_E_MEMCALL:
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      if (stmt_ownership_resource_expr_consumes(
              cg, e->as.memcall.args.data[i].val, name))
        return true;
    return stmt_ownership_resource_expr_consumes(cg, e->as.memcall.target,
                                                 name);
  case NY_E_INDEX:
    return stmt_ownership_resource_expr_consumes(cg, e->as.index.target,
                                                 name) ||
           stmt_ownership_resource_expr_consumes(cg, e->as.index.start, name) ||
           stmt_ownership_resource_expr_consumes(cg, e->as.index.stop, name) ||
           stmt_ownership_resource_expr_consumes(cg, e->as.index.step, name);
  case NY_E_MEMBER:
    return stmt_ownership_resource_expr_consumes(cg, e->as.member.target, name);
  case NY_E_DEREF:
    return stmt_ownership_resource_expr_consumes(cg, e->as.deref.target, name);
  case NY_E_TRY:
    return stmt_ownership_resource_expr_consumes(cg, e->as.try_expr.target,
                                                 name);
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    /*
     * Embedding a linear/affine value in an aggregate transfers its ownership
     * to that aggregate.  Keep this separate from member/index reads, which
     * only observe the resource and must not satisfy its obligation.
     */
    for (size_t i = 0; i < e->as.list_like.len; ++i) {
      const expr_t *item = e->as.list_like.data[i];
      if (stmt_ownership_resource_is_name(item, name) ||
          stmt_ownership_resource_expr_consumes(cg, item, name))
        return true;
    }
    return false;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      const expr_t *key = e->as.dict.pairs.data[i].key;
      const expr_t *value = e->as.dict.pairs.data[i].value;
      if (stmt_ownership_resource_is_name(key, name) ||
          stmt_ownership_resource_is_name(value, name) ||
          stmt_ownership_resource_expr_consumes(cg, key, name) ||
          stmt_ownership_resource_expr_consumes(cg, value, name))
        return true;
    }
    return false;
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; ++i)
      if (e->as.fstring.parts.data[i].kind == NY_FSP_EXPR &&
          stmt_ownership_resource_expr_consumes(
              cg, e->as.fstring.parts.data[i].as.e, name))
        return true;
    return false;
  case NY_E_MATCH:
    return stmt_ownership_resource_is_name(e->as.match.test, name) ||
           stmt_ownership_resource_expr_consumes(cg, e->as.match.test, name);
  default:
    return false;
  }
}

static bool stmt_ownership_resource_stmt_event(codegen_t *cg, const stmt_t *s,
                                               const char *name) {
  if (!s || !name)
    return false;
  switch (s->kind) {
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; ++i) {
      const expr_t *e = s->as.var.exprs.data[i];
      if (stmt_ownership_resource_place_matches(e, name) ||
          stmt_ownership_resource_place_covers(e, name) ||
          (s->as.var.is_decl && stmt_ownership_resource_is_name(e, name)) ||
          (!s->as.var.is_decl && stmt_ownership_resource_is_name(e, name)) ||
          stmt_ownership_resource_expr_consumes(cg, e, name))
        return true;
    }
    return false;
  case NY_S_EXPR:
    return stmt_ownership_resource_expr_consumes(cg, s->as.expr.expr, name);
  case NY_S_RETURN:
    return stmt_ownership_resource_is_name(s->as.ret.value, name) ||
           stmt_ownership_resource_expr_consumes(cg, s->as.ret.value, name);
  case NY_S_IF:
    return stmt_ownership_resource_expr_consumes(cg, s->as.iff.test, name);
  case NY_S_MATCH:
    return stmt_ownership_resource_is_name(s->as.match.test, name) ||
           stmt_ownership_resource_expr_consumes(cg, s->as.match.test, name);
  case NY_S_WHILE:
    return stmt_ownership_resource_expr_consumes(cg, s->as.whl.test, name);
  case NY_S_FOR:
    return stmt_ownership_resource_expr_consumes(cg, s->as.fr.iterable, name) ||
           stmt_ownership_resource_expr_consumes(cg, s->as.fr.cond, name);
  case NY_S_GUARD:
    return stmt_ownership_resource_expr_consumes(cg, s->as.guard.value, name);
  default:
    return false;
  }
}

static bool stmt_ownership_resource_stmt_assigns_name(const stmt_t *s,
                                                      const char *name) {
  if (!s || s->kind != NY_S_VAR || s->as.var.is_decl || !name)
    return false;
  for (size_t i = 0; i < s->as.var.names.len; ++i)
    if (s->as.var.names.data[i] && strcmp(s->as.var.names.data[i], name) == 0)
      return true;
  return false;
}

static bool stmt_ownership_resource_stmt_self_assigns_name(const stmt_t *s,
                                                           const char *name) {
  if (!s || s->kind != NY_S_VAR || s->as.var.is_decl || !name)
    return false;
  for (size_t i = 0; i < s->as.var.names.len; ++i)
    if (s->as.var.names.data[i] && strcmp(s->as.var.names.data[i], name) == 0 &&
        i < s->as.var.exprs.len &&
        stmt_ownership_resource_is_name(s->as.var.exprs.data[i], name))
      return true;
  return false;
}

static size_t stmt_ownership_resource_stmt_consume_count(codegen_t *cg,
                                                         const stmt_t *s,
                                                         const char *name,
                                                         unsigned depth);

static size_t stmt_ownership_resource_expr_consume_count(codegen_t *cg,
                                                         const expr_t *e,
                                                         const char *name) {
  if (!e || !name)
    return 0;
  if (e->kind == NY_E_CALL) {
    size_t call_consumes =
        stmt_ownership_resource_call_consume_count(cg, e, name);
    if (call_consumes > 0)
      return call_consumes;
    expr_t *arg = stmt_ownership_consumes_arg(cg, (expr_t *)e);
    if (!arg)
      arg = stmt_ownership_releases_arg(cg, (expr_t *)e);
    if (!arg)
      arg = stmt_ownership_forgets_arg(cg, (expr_t *)e);
    if (!arg)
      arg = stmt_ownership_unary_arg((expr_t *)e, "own");
    if (stmt_ownership_resource_place_covers(arg, name))
      return 1;
    if (stmt_expr_is_adt_ctor(cg, (expr_t *)e)) {
      for (size_t i = 0; i < e->as.call.args.len; ++i)
        if (stmt_ownership_resource_is_name(e->as.call.args.data[i].val, name))
          return 1;
    }
    size_t total = 0;
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      total += stmt_ownership_resource_expr_consume_count(
          cg, e->as.call.args.data[i].val, name);
    return total;
  }
  switch (e->kind) {
  case NY_E_UNARY:
    return stmt_ownership_resource_expr_consume_count(cg, e->as.unary.right,
                                                      name);
  case NY_E_BINARY:
    return stmt_ownership_resource_expr_consume_count(cg, e->as.binary.left,
                                                      name) +
           stmt_ownership_resource_expr_consume_count(cg, e->as.binary.right,
                                                      name);
  case NY_E_LOGICAL:
    return stmt_ownership_resource_expr_consume_count(cg, e->as.logical.left,
                                                      name) +
           stmt_ownership_resource_expr_consume_count(cg, e->as.logical.right,
                                                      name);
  case NY_E_TERNARY: {
    size_t cond = stmt_ownership_resource_expr_consume_count(
        cg, e->as.ternary.cond, name);
    size_t yes = stmt_ownership_resource_expr_consume_count(
        cg, e->as.ternary.true_expr, name);
    size_t no = stmt_ownership_resource_expr_consume_count(
        cg, e->as.ternary.false_expr, name);
    return cond + (yes > no ? yes : no);
  }
  case NY_E_MEMCALL: {
    size_t total = stmt_ownership_resource_expr_consume_count(
        cg, e->as.memcall.target, name);
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      total += stmt_ownership_resource_expr_consume_count(
          cg, e->as.memcall.args.data[i].val, name);
    return total;
  }
  case NY_E_INDEX:
    return stmt_ownership_resource_expr_consume_count(cg, e->as.index.target,
                                                      name) +
           stmt_ownership_resource_expr_consume_count(cg, e->as.index.start,
                                                      name) +
           stmt_ownership_resource_expr_consume_count(cg, e->as.index.stop,
                                                      name) +
           stmt_ownership_resource_expr_consume_count(cg, e->as.index.step,
                                                      name);
  case NY_E_MEMBER:
    return stmt_ownership_resource_expr_consume_count(cg, e->as.member.target,
                                                      name);
  case NY_E_DEREF:
    return stmt_ownership_resource_expr_consume_count(cg, e->as.deref.target,
                                                      name);
  case NY_E_TRY:
    return stmt_ownership_resource_expr_consume_count(cg, e->as.try_expr.target,
                                                      name);
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET: {
    size_t total = 0;
    for (size_t i = 0; i < e->as.list_like.len; ++i) {
      const expr_t *item = e->as.list_like.data[i];
      total += stmt_ownership_resource_is_name(item, name)
                   ? 1
                   : stmt_ownership_resource_expr_consume_count(cg, item, name);
    }
    return total;
  }
  case NY_E_DICT: {
    size_t total = 0;
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      const expr_t *key = e->as.dict.pairs.data[i].key;
      const expr_t *value = e->as.dict.pairs.data[i].value;
      total += stmt_ownership_resource_is_name(key, name)
                   ? 1
                   : stmt_ownership_resource_expr_consume_count(cg, key, name);
      total +=
          stmt_ownership_resource_is_name(value, name)
              ? 1
              : stmt_ownership_resource_expr_consume_count(cg, value, name);
    }
    return total;
  }
  case NY_E_FSTRING: {
    size_t total = 0;
    for (size_t i = 0; i < e->as.fstring.parts.len; ++i)
      if (e->as.fstring.parts.data[i].kind == NY_FSP_EXPR)
        total += stmt_ownership_resource_expr_consume_count(
            cg, e->as.fstring.parts.data[i].as.e, name);
    return total;
  }
  case NY_E_MATCH: {
    size_t test = stmt_ownership_resource_is_name(e->as.match.test, name)
                      ? 1
                      : stmt_ownership_resource_expr_consume_count(
                            cg, e->as.match.test, name);
    size_t max_arm = 0;
    for (size_t i = 0; i < e->as.match.arms.len; ++i) {
      size_t arm = stmt_ownership_resource_expr_consume_count(
          cg, e->as.match.arms.data[i].guard, name);
      size_t body = stmt_ownership_resource_stmt_consume_count(
          cg, e->as.match.arms.data[i].conseq, name, 0);
      arm += body;
      if (arm > max_arm)
        max_arm = arm;
    }
    size_t fallback = stmt_ownership_resource_stmt_consume_count(
        cg, e->as.match.default_conseq, name, 0);
    if (fallback > max_arm)
      max_arm = fallback;
    return test + max_arm;
  }
  default:
    return 0;
  }
}

static size_t stmt_ownership_resource_stmt_consume_count(codegen_t *cg,
                                                         const stmt_t *s,
                                                         const char *name,
                                                         unsigned depth) {
  if (!s || depth > 128)
    return 0;
  switch (s->kind) {
  case NY_S_BLOCK: {
    size_t total = 0;
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      total += stmt_ownership_resource_stmt_consume_count(
          cg, s->as.block.body.data[i], name, depth + 1);
    return total;
  }
  case NY_S_VAR: {
    size_t total = 0;
    for (size_t i = 0; i < s->as.var.exprs.len; ++i) {
      const expr_t *e = s->as.var.exprs.data[i];
      if (stmt_ownership_resource_place_matches(e, name) ||
          stmt_ownership_resource_place_covers(e, name) ||
          stmt_ownership_resource_is_name(e, name))
        ++total;
      else
        total += stmt_ownership_resource_expr_consume_count(cg, e, name);
    }
    return total;
  }
  case NY_S_EXPR:
    if (s->as.expr.expr && s->as.expr.expr->kind == NY_E_BINARY &&
        s->as.expr.expr->as.binary.op &&
        strcmp(s->as.expr.expr->as.binary.op, "=") == 0 &&
        stmt_ownership_resource_is_name(s->as.expr.expr->as.binary.right, name))
      return 1;
    return stmt_ownership_resource_expr_consume_count(cg, s->as.expr.expr,
                                                      name);
  case NY_S_RETURN:
    return stmt_ownership_resource_is_name(s->as.ret.value, name)
               ? 1
               : stmt_ownership_resource_expr_consume_count(cg, s->as.ret.value,
                                                            name);
  case NY_S_IF: {
    size_t test =
        stmt_ownership_resource_expr_consume_count(cg, s->as.iff.test, name);
    size_t yes = stmt_ownership_resource_stmt_consume_count(
        cg, s->as.iff.conseq, name, depth + 1);
    size_t no = stmt_ownership_resource_stmt_consume_count(cg, s->as.iff.alt,
                                                           name, depth + 1);
    return test + (yes > no ? yes : no);
  }
  case NY_S_MATCH: {
    size_t test = stmt_ownership_resource_is_name(s->as.match.test, name)
                      ? 1
                      : stmt_ownership_resource_expr_consume_count(
                            cg, s->as.match.test, name);
    size_t max_arm = 0;
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      size_t arm = stmt_ownership_resource_expr_consume_count(
                       cg, s->as.match.arms.data[i].guard, name) +
                   stmt_ownership_resource_stmt_consume_count(
                       cg, s->as.match.arms.data[i].conseq, name, depth + 1);
      if (arm > max_arm)
        max_arm = arm;
    }
    size_t fallback = stmt_ownership_resource_stmt_consume_count(
        cg, s->as.match.default_conseq, name, depth + 1);
    return test + (fallback > max_arm ? fallback : max_arm);
  }
  case NY_S_WHILE: {
    size_t init = stmt_ownership_resource_stmt_consume_count(cg, s->as.whl.init,
                                                             name, depth + 1);
    return init +
           stmt_ownership_resource_expr_consume_count(cg, s->as.whl.test, name);
  }
  case NY_S_FOR: {
    size_t init = stmt_ownership_resource_stmt_consume_count(cg, s->as.fr.init,
                                                             name, depth + 1);
    return init +
           stmt_ownership_resource_expr_consume_count(cg, s->as.fr.iterable,
                                                      name) +
           stmt_ownership_resource_expr_consume_count(cg, s->as.fr.cond, name);
  }
  case NY_S_TRY: {
    size_t body = stmt_ownership_resource_stmt_consume_count(cg, s->as.tr.body,
                                                             name, depth + 1);
    size_t handler = stmt_ownership_resource_stmt_consume_count(
        cg, s->as.tr.handler, name, depth + 1);
    return body + handler;
  }
  case NY_S_GUARD:
    return stmt_ownership_resource_expr_consume_count(cg, s->as.guard.value,
                                                      name) +
           stmt_ownership_resource_stmt_consume_count(cg, s->as.guard.fallback,
                                                      name, depth + 1);
  default:
    return 0;
  }
}

static bool stmt_ownership_resource_consumes_in_loop(codegen_t *cg,
                                                     const stmt_t *s,
                                                     const char *name,
                                                     unsigned depth) {
  if (!s || depth > 128)
    return false;
  if (s->kind == NY_S_WHILE)
    return stmt_ownership_resource_expr_consume_count(cg, s->as.whl.test,
                                                      name) > 0 ||
           stmt_ownership_resource_stmt_consume_count(cg, s->as.whl.body, name,
                                                      depth + 1) > 0 ||
           stmt_ownership_resource_consumes_in_loop(cg, s->as.whl.body, name,
                                                    depth + 1);
  if (s->kind == NY_S_FOR)
    return stmt_ownership_resource_expr_consume_count(cg, s->as.fr.cond, name) >
               0 ||
           stmt_ownership_resource_stmt_consume_count(cg, s->as.fr.body, name,
                                                      depth + 1) > 0 ||
           stmt_ownership_resource_consumes_in_loop(cg, s->as.fr.body, name,
                                                    depth + 1);
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (stmt_ownership_resource_consumes_in_loop(cg, s->as.block.body.data[i],
                                                   name, depth + 1))
        return true;
  } else if (s->kind == NY_S_IF) {
    return stmt_ownership_resource_consumes_in_loop(cg, s->as.iff.conseq, name,
                                                    depth + 1) ||
           stmt_ownership_resource_consumes_in_loop(cg, s->as.iff.alt, name,
                                                    depth + 1);
  } else if (s->kind == NY_S_MATCH) {
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      if (stmt_ownership_resource_consumes_in_loop(
              cg, s->as.match.arms.data[i].conseq, name, depth + 1))
        return true;
    return stmt_ownership_resource_consumes_in_loop(
        cg, s->as.match.default_conseq, name, depth + 1);
  }
  return false;
}

static unsigned stmt_ownership_resource_apply_event(unsigned flow,
                                                    bool consumes,
                                                    bool *double_consume) {
  if (!consumes)
    return flow;
  if (double_consume && (flow & NY_RESOURCE_LIVE_CONSUMED))
    *double_consume = true;
  if (flow & NY_RESOURCE_LIVE_UNCONSUMED) {
    flow &= ~NY_RESOURCE_LIVE_UNCONSUMED;
    flow |= NY_RESOURCE_LIVE_CONSUMED;
  }
  return flow;
}

static unsigned stmt_ownership_resource_apply_count(unsigned flow, size_t count,
                                                    bool *double_consume) {
  /*
   * The flow bits are idempotent after a second event, so two iterations are
   * sufficient to record both the moved state and a duplicate-consume error.
   */
  for (size_t i = 0; i < count && i < 2; ++i)
    flow = stmt_ownership_resource_apply_event(flow, true, double_consume);
  return flow;
}

static unsigned stmt_ownership_resource_expr_flow(codegen_t *cg,
                                                  const expr_t *e,
                                                  const char *name,
                                                  unsigned flow, unsigned depth,
                                                  bool *double_consume) {
  if (!e || depth > 128)
    return flow;
  if (e->kind == NY_E_LOGICAL) {
    unsigned left = stmt_ownership_resource_expr_flow(
        cg, e->as.logical.left, name, flow, depth + 1, double_consume);
    unsigned right = stmt_ownership_resource_expr_flow(
        cg, e->as.logical.right, name, left, depth + 1, double_consume);
    /*
     * The right operand is conditional; keep both the short-circuit path and
     * the path that evaluates it.
     */
    return left | right;
  }
  if (e->kind == NY_E_TERNARY) {
    unsigned cond = stmt_ownership_resource_expr_flow(
        cg, e->as.ternary.cond, name, flow, depth + 1, double_consume);
    unsigned yes = stmt_ownership_resource_expr_flow(
        cg, e->as.ternary.true_expr, name, cond, depth + 1, double_consume);
    unsigned no = stmt_ownership_resource_expr_flow(
        cg, e->as.ternary.false_expr, name, cond, depth + 1, double_consume);
    return yes | no;
  }
  if (e->kind == NY_E_BINARY) {
    unsigned left = stmt_ownership_resource_expr_flow(
        cg, e->as.binary.left, name, flow, depth + 1, double_consume);
    if (e->as.binary.op && strcmp(e->as.binary.op, "=") == 0 &&
        stmt_ownership_resource_is_name(e->as.binary.right, name))
      return stmt_ownership_resource_apply_event(left, true, double_consume);
    return stmt_ownership_resource_expr_flow(cg, e->as.binary.right, name, left,
                                             depth + 1, double_consume);
  }
  if (e->kind == NY_E_CALL) {
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      flow = stmt_ownership_resource_expr_flow(cg, e->as.call.args.data[i].val,
                                               name, flow, depth + 1,
                                               double_consume);
    size_t consumes = stmt_ownership_resource_call_consume_count(cg, e, name);
    return stmt_ownership_resource_apply_count(flow, consumes, double_consume);
  }
  return stmt_ownership_resource_apply_event(
      flow, stmt_ownership_resource_expr_consumes(cg, e, name), double_consume);
}

static size_t stmt_ownership_resource_stmt_consume_count(codegen_t *cg,
                                                         const stmt_t *s,
                                                         const char *name,
                                                         unsigned depth);

static unsigned stmt_ownership_resource_flow(codegen_t *cg, const stmt_t *s,
                                             const char *name, unsigned flow,
                                             unsigned depth,
                                             bool *use_after_move,
                                             bool *double_consume,
                                             bool *overwrite_live) {
  if (!s || depth > 128)
    return flow;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      flow = stmt_ownership_resource_flow(cg, s->as.block.body.data[i], name,
                                          flow, depth + 1, use_after_move,
                                          double_consume, overwrite_live);
    return flow;
  case NY_S_VAR:
    if (stmt_ownership_resource_stmt_assigns_name(s, name)) {
      if (stmt_ownership_resource_stmt_self_assigns_name(s, name)) {
        if ((flow & NY_RESOURCE_LIVE_CONSUMED) && use_after_move)
          *use_after_move = true;
        return flow;
      }
      if ((flow & NY_RESOURCE_LIVE_UNCONSUMED) && overwrite_live)
        *overwrite_live = true;
      /*
       * Reassignment starts a new value generation.  The source's separate
       * flow walk accounts for the move from the RHS.
       */
      bool has_live_path =
          (flow & (NY_RESOURCE_LIVE_UNCONSUMED | NY_RESOURCE_LIVE_CONSUMED)) !=
          0;
      flow &= ~NY_RESOURCE_LIVE_CONSUMED;
      if (has_live_path)
        flow |= NY_RESOURCE_LIVE_UNCONSUMED;
      return flow;
    }
    if (use_after_move && (flow & NY_RESOURCE_LIVE_CONSUMED)) {
      size_t uses =
          strchr(name, '.')
              ? (size_t)stmt_ownership_resource_path_mentions_stmt(s, name, 0)
              : stmt_ownership_count_uses_stmt_internal(s, name, NULL);
      size_t consumes =
          stmt_ownership_resource_stmt_consume_count(cg, s, name, depth);
      if (uses > consumes)
        *use_after_move = true;
      if (double_consume && consumes > 1)
        *double_consume = true;
    }
    for (size_t i = 0; i < s->as.var.exprs.len; ++i) {
      const expr_t *value = s->as.var.exprs.data[i];
      if (stmt_ownership_resource_place_matches(value, name) ||
          stmt_ownership_resource_place_covers(value, name) ||
          stmt_ownership_resource_is_name(value, name))
        flow = stmt_ownership_resource_apply_event(flow, true, double_consume);
      else
        flow = stmt_ownership_resource_expr_flow(cg, value, name, flow,
                                                 depth + 1, double_consume);
    }
    return flow;
  case NY_S_IF: {
    flow = stmt_ownership_resource_expr_flow(cg, s->as.iff.test, name, flow,
                                             depth + 1, double_consume);
    unsigned yes = stmt_ownership_resource_flow(cg, s->as.iff.conseq, name,
                                                flow, depth + 1, use_after_move,
                                                double_consume, overwrite_live);
    unsigned no = s->as.iff.alt
                      ? stmt_ownership_resource_flow(
                            cg, s->as.iff.alt, name, flow, depth + 1,
                            use_after_move, double_consume, overwrite_live)
                      : flow;
    return yes | no;
  }
  case NY_S_MATCH: {
    if (stmt_ownership_resource_is_name(s->as.match.test, name))
      flow = stmt_ownership_resource_apply_event(flow, true, double_consume);
    else
      flow = stmt_ownership_resource_expr_flow(cg, s->as.match.test, name, flow,
                                               depth + 1, double_consume);
    unsigned branches = 0;
    unsigned fallthrough = flow;
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      const match_arm_t *arm = &s->as.match.arms.data[i];
      unsigned guarded = stmt_ownership_resource_expr_flow(
          cg, arm->guard, name, fallthrough, depth + 1, double_consume);
      branches |= stmt_ownership_resource_flow(cg, arm->conseq, name, guarded,
                                               depth + 1, use_after_move,
                                               double_consume, overwrite_live);
      /*
       * Either this pattern did not match, or its guard failed.  Preserve
       * both possibilities for later arms: the pattern-miss path did not run
       * the guard, while the guard-failure path did.
       */
      fallthrough |= guarded;
    }
    if (s->as.match.default_conseq)
      branches |= stmt_ownership_resource_flow(
          cg, s->as.match.default_conseq, name, fallthrough, depth + 1,
          use_after_move, double_consume, overwrite_live);
    else
      branches |= fallthrough;
    return branches;
  }
  case NY_S_WHILE: {
    flow = stmt_ownership_resource_flow(cg, s->as.whl.init, name, flow,
                                        depth + 1, use_after_move,
                                        double_consume, overwrite_live);
    flow = stmt_ownership_resource_expr_flow(cg, s->as.whl.test, name, flow,
                                             depth + 1, double_consume);
    return flow | stmt_ownership_resource_flow(cg, s->as.whl.body, name, flow,
                                               depth + 1, use_after_move,
                                               double_consume, overwrite_live);
  }
  case NY_S_FOR: {
    flow = stmt_ownership_resource_flow(cg, s->as.fr.init, name, flow,
                                        depth + 1, use_after_move,
                                        double_consume, overwrite_live);
    flow = stmt_ownership_resource_expr_flow(cg, s->as.fr.iterable, name, flow,
                                             depth + 1, double_consume);
    flow = stmt_ownership_resource_expr_flow(cg, s->as.fr.cond, name, flow,
                                             depth + 1, double_consume);
    return flow | stmt_ownership_resource_flow(cg, s->as.fr.body, name, flow,
                                               depth + 1, use_after_move,
                                               double_consume, overwrite_live);
  }
  case NY_S_TRY: {
    unsigned body = stmt_ownership_resource_flow(
        cg, s->as.tr.body, name, flow, depth + 1, use_after_move,
        double_consume, overwrite_live);
    unsigned handler = stmt_ownership_resource_flow(
        cg, s->as.tr.handler, name, flow, depth + 1, use_after_move,
        double_consume, overwrite_live);
    return body | handler;
  }
  case NY_S_GUARD: {
    unsigned condition = stmt_ownership_resource_expr_flow(
        cg, s->as.guard.value, name, flow, depth + 1, double_consume);
    unsigned fallback = stmt_ownership_resource_flow(
        cg, s->as.guard.fallback, name, condition, depth + 1, use_after_move,
        double_consume, overwrite_live);
    return condition | fallback;
  }
  case NY_S_RETURN:
    if (stmt_ownership_resource_is_name(s->as.ret.value, name))
      flow = stmt_ownership_resource_apply_event(flow, true, double_consume);
    else
      flow = stmt_ownership_resource_expr_flow(cg, s->as.ret.value, name, flow,
                                               depth + 1, double_consume);
    if (flow & NY_RESOURCE_LIVE_UNCONSUMED) {
      flow &= ~NY_RESOURCE_LIVE_UNCONSUMED;
      flow |= NY_RESOURCE_DONE_UNCONSUMED;
    }
    if (flow & NY_RESOURCE_LIVE_CONSUMED) {
      flow &= ~NY_RESOURCE_LIVE_CONSUMED;
      flow |= NY_RESOURCE_DONE_CONSUMED;
    }
    return flow;
  default:
    if (use_after_move && (flow & NY_RESOURCE_LIVE_CONSUMED)) {
      size_t uses =
          strchr(name, '.')
              ? (size_t)stmt_ownership_resource_path_mentions_stmt(s, name, 0)
              : stmt_ownership_count_uses_stmt_internal(s, name, NULL);
      size_t consumes =
          stmt_ownership_resource_stmt_consume_count(cg, s, name, depth);
      if (uses > consumes)
        *use_after_move = true;
    }
    if (double_consume &&
        stmt_ownership_resource_stmt_consume_count(cg, s, name, depth) > 1)
      *double_consume = true;
    if (s->kind == NY_S_EXPR)
      return stmt_ownership_resource_expr_flow(cg, s->as.expr.expr, name, flow,
                                               depth + 1, double_consume);
    return stmt_ownership_resource_apply_event(
        flow, stmt_ownership_resource_stmt_event(cg, s, name), double_consume);
  }
}

static void stmt_ownership_check_resource_value(codegen_t *cg, const char *name,
                                                const char *qualifier,
                                                const stmt_t *body,
                                                token_t tok) {
  if (!cg || !name || !qualifier || !body)
    return;
  if (stmt_ownership_resource_stmt_captures(body, name, 0)) {
    char message[256];
    snprintf(message, sizeof(message),
             "resource '%s' cannot be captured by value in a closure", name);
    stmt_ownership_rules_error(cg, tok, "L4", message);
    return;
  }
  bool is_linear = strcmp(qualifier, "linear") == 0;
  bool unbounded = stmt_ownership_resource_consumes_in_loop(cg, body, name, 0);
  bool use_after_move = false;
  bool double_consume = false;
  bool overwrite_live = false;
  unsigned consumption = stmt_ownership_resource_flow(
      cg, body, name, NY_RESOURCE_LIVE_UNCONSUMED, 0, &use_after_move,
      &double_consume, &overwrite_live);
  bool every_path_consumes = !(consumption & (NY_RESOURCE_LIVE_UNCONSUMED |
                                              NY_RESOURCE_DONE_UNCONSUMED));
  if (is_linear && !every_path_consumes) {
    char message[256];
    snprintf(message, sizeof(message),
             "linear resource '%s' must be consumed exactly once", name);
    stmt_ownership_rules_error(cg, tok, "L1", message);
    ny_diag_fix("consume or release linear resource '%s' on every path", name);
  }
  if (unbounded || double_consume) {
    char message[256];
    snprintf(message, sizeof(message),
             "%s resource '%s' may be consumed at most once on every path",
             qualifier, name);
    stmt_ownership_rules_error(cg, tok, is_linear ? "L1" : "L2", message);
    ny_diag_fix("move or consume resource '%s' no more than once per path",
                name);
  }
  if (is_linear && overwrite_live) {
    char message[256];
    snprintf(message, sizeof(message),
             "linear resource '%s' is overwritten before consumption", name);
    stmt_ownership_rules_error(cg, tok, "L4", message);
    ny_diag_fix(
        "consume or move the current value of '%s' before assigning a new one",
        name);
  }
  if (use_after_move) {
    char message[256];
    snprintf(message, sizeof(message),
             "%s resource '%s' is used after it was moved", qualifier, name);
    stmt_ownership_rules_error(cg, tok, "L4", message);
    ny_diag_fix("use the destination of the move, not '%s'", name);
  }
}

static bool stmt_ownership_resource_decl_shadows(const stmt_t *s,
                                                 const char *name,
                                                 unsigned depth) {
  if (!s || !name)
    return false;
  if (depth > 128)
    return true;
  switch (s->kind) {
  case NY_S_VAR:
    if (s->as.var.is_decl)
      for (size_t i = 0; i < s->as.var.names.len; ++i)
        if (s->as.var.names.data[i] &&
            strcmp(s->as.var.names.data[i], name) == 0)
          return true;
    break;
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (stmt_ownership_resource_decl_shadows(s->as.block.body.data[i], name,
                                               depth + 1))
        return true;
    break;
  case NY_S_IF:
    return stmt_ownership_resource_decl_shadows(s->as.iff.init, name,
                                                depth + 1) ||
           stmt_ownership_resource_decl_shadows(s->as.iff.conseq, name,
                                                depth + 1) ||
           stmt_ownership_resource_decl_shadows(s->as.iff.alt, name, depth + 1);
  case NY_S_WHILE:
    return stmt_ownership_resource_decl_shadows(s->as.whl.init, name,
                                                depth + 1) ||
           stmt_ownership_resource_decl_shadows(s->as.whl.body, name,
                                                depth + 1);
  case NY_S_FOR:
    return (s->as.fr.iter_var && strcmp(s->as.fr.iter_var, name) == 0) ||
           (s->as.fr.iter_index_var &&
            strcmp(s->as.fr.iter_index_var, name) == 0) ||
           stmt_ownership_resource_decl_shadows(s->as.fr.init, name,
                                                depth + 1) ||
           stmt_ownership_resource_decl_shadows(s->as.fr.body, name, depth + 1);
  case NY_S_TRY:
    return stmt_ownership_resource_decl_shadows(s->as.tr.body, name,
                                                depth + 1) ||
           stmt_ownership_resource_decl_shadows(s->as.tr.handler, name,
                                                depth + 1);
  case NY_S_MATCH:
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      const match_arm_t *arm = &s->as.match.arms.data[i];
      for (size_t j = 0; j < arm->patterns.len; ++j)
        if (arm->patterns.data[j] &&
            arm->patterns.data[j]->kind == NY_E_IDENT &&
            arm->patterns.data[j]->as.ident.name &&
            strcmp(arm->patterns.data[j]->as.ident.name, name) == 0)
          return true;
      if (stmt_ownership_resource_decl_shadows(arm->conseq, name, depth + 1))
        return true;
    }
    return stmt_ownership_resource_decl_shadows(s->as.match.default_conseq,
                                                name, depth + 1);
  case NY_S_GUARD:
    return (s->as.guard.name && strcmp(s->as.guard.name, name) == 0) ||
           stmt_ownership_resource_decl_shadows(s->as.guard.fallback, name,
                                                depth + 1);
  case NY_S_DEFER:
    return stmt_ownership_resource_decl_shadows(s->as.de.body, name, depth + 1);
  default:
    break;
  }
  return false;
}

static const char *stmt_ownership_resource_prior_local_qualifier(
    codegen_t *cg, const stmt_block_t *block, size_t before, const char *name,
    unsigned depth) {
  if (!cg || !block || !name || depth > 128)
    return NULL;
  for (size_t i = before; i > 0; --i) {
    const stmt_t *stmt = block->body.data[i - 1];
    if (!stmt || stmt->kind != NY_S_VAR || !stmt->as.var.is_decl)
      continue;
    for (size_t j = 0; j < stmt->as.var.names.len; ++j) {
      if (!stmt->as.var.names.data[j] ||
          strcmp(stmt->as.var.names.data[j], name) != 0)
        continue;
      const char *qualifier = j < stmt->as.var.types.len
                                  ? stmt_ownership_resource_qualifier(
                                        cg, stmt->as.var.types.data[j])
                                  : NULL;
      if (qualifier)
        return qualifier;
      if (j >= stmt->as.var.exprs.len)
        return NULL;
      const expr_t *init = stmt->as.var.exprs.data[j];
      if (!init)
        return NULL;
      const char *inferred = stmt_ownership_resource_initializer_qualifier(
          cg, init, block, i - 1, depth + 1);
      if (inferred)
        return inferred;
      if (init->kind == NY_E_CALL && init->as.call.callee &&
          init->as.call.callee->kind == NY_E_IDENT)
        return stmt_ownership_resource_qualifier(
            cg, init->as.call.callee->as.ident.name);
      if (init->kind == NY_E_IDENT && init->as.ident.name)
        return stmt_ownership_resource_prior_local_qualifier(
            cg, block, i - 1, init->as.ident.name, depth + 1);
      return NULL;
    }
  }
  return NULL;
}

static const char *stmt_ownership_resource_prior_local_type(
    codegen_t *cg, const stmt_block_t *block, size_t before, const char *name,
    unsigned depth) {
  if (!cg || !block || !name || depth > 128)
    return NULL;
  for (size_t i = before; i > 0; --i) {
    const stmt_t *stmt = block->body.data[i - 1];
    if (!stmt || stmt->kind != NY_S_VAR || !stmt->as.var.is_decl)
      continue;
    for (size_t j = 0; j < stmt->as.var.names.len; ++j) {
      if (!stmt->as.var.names.data[j] ||
          strcmp(stmt->as.var.names.data[j], name) != 0)
        continue;
      if (j < stmt->as.var.types.len && stmt->as.var.types.data[j] &&
          *stmt->as.var.types.data[j])
        return stmt->as.var.types.data[j];
      if (j >= stmt->as.var.exprs.len || !stmt->as.var.exprs.data[j])
        return NULL;
      expr_t *init = stmt->as.var.exprs.data[j];
      const char *type = infer_expr_type(cg, NULL, 0, init);
      if (type)
        return type;
      if (init->kind == NY_E_IDENT && init->as.ident.name)
        return stmt_ownership_resource_prior_local_type(
            cg, block, i - 1, init->as.ident.name, depth + 1);
      return NULL;
    }
  }
  return NULL;
}

static void stmt_ownership_check_resource_locals_stmt(codegen_t *cg,
                                                      const stmt_t *s,
                                                      unsigned depth) {
  if (!cg || !cg->ownership_strict || !s || depth > 128)
    return;
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const stmt_t *item = s->as.block.body.data[i];
      if (!item)
        continue;
      if (item->kind == NY_S_VAR && item->as.var.is_decl) {
        for (size_t j = 0; j < item->as.var.names.len; ++j) {
          const char *name = item->as.var.names.data[j];
          const char *binding_type =
              j < item->as.var.types.len ? item->as.var.types.data[j] : NULL;
          if (!binding_type && j < item->as.var.exprs.len)
            binding_type = infer_expr_type(
                cg, NULL, 0, (expr_t *)item->as.var.exprs.data[j]);
          if (!binding_type && j < item->as.var.exprs.len) {
            const expr_t *init = item->as.var.exprs.data[j];
            if (init && init->kind == NY_E_IDENT && init->as.ident.name)
              binding_type = stmt_ownership_resource_prior_local_type(
                  cg, &s->as.block, i, init->as.ident.name, 0);
          }
          const char *qualifier =
              j < item->as.var.types.len
                  ? stmt_ownership_resource_qualifier(cg, binding_type)
                  : NULL;
          if (!qualifier && j < item->as.var.exprs.len) {
            const expr_t *init = item->as.var.exprs.data[j];
            qualifier = stmt_ownership_resource_initializer_qualifier(
                cg, init, &s->as.block, i, depth + 1);
            if (init && init->kind == NY_E_CALL && init->as.call.callee &&
                init->as.call.callee->kind == NY_E_IDENT)
              qualifier = qualifier
                              ? qualifier
                              : stmt_ownership_resource_qualifier(
                                    cg, init->as.call.callee->as.ident.name);
            else if (init && init->kind == NY_E_IDENT && init->as.ident.name)
              qualifier =
                  qualifier ? qualifier
                            : stmt_ownership_resource_prior_local_qualifier(
                                  cg, &s->as.block, i, init->as.ident.name, 0);
          }
          if (!name || !qualifier)
            continue;
          stmt_t suffix = {.kind = NY_S_BLOCK};
          suffix.as.block.body = s->as.block.body;
          suffix.as.block.body.data += i + 1;
          suffix.as.block.body.len -= i + 1;
          bool shadowed = false;
          for (size_t k = 0; k < suffix.as.block.body.len; ++k)
            if (stmt_ownership_resource_decl_shadows(
                    suffix.as.block.body.data[k], name, depth + 1)) {
              shadowed = true;
              break;
            }
          if (shadowed) {
            stmt_ownership_diag(cg, item->tok,
                                "resource '%s' cannot be shadowed in its scope",
                                name);
            continue;
          }
          if (!binding_type ||
              !stmt_ownership_check_resource_type_paths(cg, name, binding_type,
                                                        &suffix, item->tok, 0))
            stmt_ownership_check_resource_value(cg, name, qualifier, &suffix,
                                                item->tok);
        }
      }
      stmt_ownership_check_resource_locals_stmt(cg, item, depth + 1);
    }
    return;
  }
  switch (s->kind) {
  case NY_S_IF:
    stmt_ownership_check_resource_locals_stmt(cg, s->as.iff.init, depth + 1);
    stmt_ownership_check_resource_locals_stmt(cg, s->as.iff.conseq, depth + 1);
    stmt_ownership_check_resource_locals_stmt(cg, s->as.iff.alt, depth + 1);
    break;
  case NY_S_WHILE:
    stmt_ownership_check_resource_locals_stmt(cg, s->as.whl.init, depth + 1);
    stmt_ownership_check_resource_locals_stmt(cg, s->as.whl.body, depth + 1);
    break;
  case NY_S_FOR:
    stmt_ownership_check_resource_locals_stmt(cg, s->as.fr.init, depth + 1);
    stmt_ownership_check_resource_locals_stmt(cg, s->as.fr.body, depth + 1);
    break;
  case NY_S_TRY:
    stmt_ownership_check_resource_locals_stmt(cg, s->as.tr.body, depth + 1);
    stmt_ownership_check_resource_locals_stmt(cg, s->as.tr.handler, depth + 1);
    break;
  case NY_S_MATCH:
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      stmt_ownership_check_resource_locals_stmt(
          cg, s->as.match.arms.data[i].conseq, depth + 1);
    stmt_ownership_check_resource_locals_stmt(cg, s->as.match.default_conseq,
                                              depth + 1);
    break;
  default:
    break;
  }
}

static void stmt_ownership_check_resource_params(codegen_t *cg,
                                                 const stmt_t *fn_stmt) {
  if (!cg || !cg->ownership_strict || !fn_stmt || fn_stmt->kind != NY_S_FUNC ||
      !fn_stmt->as.fn.body)
    return;
  const stmt_func_t *fn = &fn_stmt->as.fn;
  for (size_t i = 0; i < fn->params.len; ++i) {
    const param_t *param = &fn->params.data[i];
    const char *qualifier = stmt_ownership_resource_qualifier(cg, param->type);
    if (param->name && qualifier &&
        stmt_ownership_resource_decl_shadows(fn->body, param->name, 0)) {
      stmt_ownership_diag(cg, param->tok,
                          "resource '%s' cannot be shadowed in its scope",
                          param->name);
      continue;
    }
    if (param->name && qualifier)
      stmt_ownership_check_resource_value(cg, param->name, qualifier, fn->body,
                                          param->tok);
  }
  stmt_ownership_check_resource_locals_stmt(cg, fn->body, 0);
}

/*
 * Unified static borrow and escape rules used by strict ownership checking.
 */

static bool stmt_ownership_rules_name_is(const char *actual,
                                         const char *expected) {
  if (!actual || !expected)
    return false;
  const char *dot = strrchr(actual, '.');
  return strcmp(dot ? dot + 1 : actual, expected) == 0;
}

static const char *
stmt_ownership_rules_returns_borrow_param(const stmt_t *function) {
  if (!function || function->kind != NY_S_FUNC)
    return NULL;
  if (function->as.fn.attr_returns_borrow)
    return function->as.fn.attr_returns_borrow;
  for (size_t i = 0; i < function->attributes.len; ++i) {
    const attribute_t *attribute = &function->attributes.data[i];
    if (attribute->name && strcmp(attribute->name, "returns_borrow") == 0 &&
        attribute->args.len == 1 && attribute->args.data[0] &&
        attribute->args.data[0]->kind == NY_E_IDENT)
      return attribute->args.data[0]->as.ident.name;
  }
  return NULL;
}

static bool stmt_ownership_rules_is_borrow_expr(const expr_t *e) {
  if (!e)
    return false;
  if (e->kind == NY_E_CALL && e->as.call.callee &&
      e->as.call.callee->kind == NY_E_IDENT &&
      (stmt_ownership_rules_name_is(e->as.call.callee->as.ident.name,
                                    "borrow") ||
       stmt_ownership_rules_name_is(e->as.call.callee->as.ident.name,
                                    "borrow_mut")))
    return e->as.call.args.len > 0;
  if (e->kind == NY_E_CALL && e->semantic.canonical_callee_stmt &&
      stmt_ownership_rules_returns_borrow_param(
          e->semantic.canonical_callee_stmt))
    return true;
  return false;
}

static const expr_t *stmt_ownership_rules_borrow_source(const expr_t *e) {
  if (!stmt_ownership_rules_is_borrow_expr(e))
    return NULL;
  const expr_t *arg = NULL;
  if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
      (stmt_ownership_rules_name_is(e->as.call.callee->as.ident.name,
                                    "borrow") ||
       stmt_ownership_rules_name_is(e->as.call.callee->as.ident.name,
                                    "borrow_mut"))) {
    arg = e->as.call.args.data[0].val;
  } else {
    const stmt_t *function = e->semantic.canonical_callee_stmt;
    const char *source_param =
        stmt_ownership_rules_returns_borrow_param(function);
    if (!function || function->kind != NY_S_FUNC || !source_param)
      return NULL;
    for (size_t i = 0; i < e->as.call.args.len; ++i) {
      const call_arg_t *actual = &e->as.call.args.data[i];
      const char *param_name = actual->name ? actual->name
                               : i < function->as.fn.params.len
                                   ? function->as.fn.params.data[i].name
                                   : NULL;
      if (param_name && strcmp(param_name, source_param) == 0) {
        arg = actual->val;
        break;
      }
    }
  }
  if (!arg)
    return NULL;
  if (stmt_ownership_rules_is_borrow_expr(arg)) {
    const expr_t *nested = stmt_ownership_rules_borrow_source(arg);
    return nested ? nested : arg;
  }
  return arg;
}

static const char *stmt_ownership_rules_place_root_name(const expr_t *e,
                                                        unsigned depth) {
  if (!e || depth > 128)
    return NULL;
  switch (e->kind) {
  case NY_E_IDENT:
    return e->as.ident.name;
  case NY_E_INDEX:
    return stmt_ownership_rules_place_root_name(e->as.index.target, depth + 1);
  case NY_E_MEMBER:
    return stmt_ownership_rules_place_root_name(e->as.member.target, depth + 1);
  case NY_E_DEREF:
    return NULL;
  case NY_E_TRY:
    return stmt_ownership_rules_place_root_name(e->as.try_expr.target,
                                                depth + 1);
  case NY_E_UNARY:
    if (e->as.unary.op && strcmp(e->as.unary.op, "*") == 0)
      return NULL;
    return stmt_ownership_rules_place_root_name(e->as.unary.right, depth + 1);
  default:
    return NULL;
  }
}

static bool stmt_ownership_rules_has_returns_borrow(const stmt_t *fn_stmt,
                                                    const char *param_name) {
  if (!fn_stmt || fn_stmt->kind != NY_S_FUNC || !param_name)
    return false;
  const stmt_func_t *fn = &fn_stmt->as.fn;
  if (fn->attr_returns_borrow &&
      strcmp(fn->attr_returns_borrow, param_name) == 0)
    return true;
  /*
   * HM runs before backend attribute lowering. Read the source AST contract
   * directly so the ownership rule pass has identical behavior on native and
   * LLVM.
   */
  for (size_t i = 0; i < fn_stmt->attributes.len; ++i) {
    const attribute_t *a = &fn_stmt->attributes.data[i];
    if (!a->name || strcmp(a->name, "returns_borrow") != 0 || a->args.len != 1)
      continue;
    const expr_t *arg = a->args.data[0];
    if (arg && arg->kind == NY_E_IDENT && arg->as.ident.name &&
        strcmp(arg->as.ident.name, param_name) == 0)
      return true;
  }
  return false;
}

static bool stmt_ownership_rules_param_exists(const stmt_func_t *fn,
                                              const char *name) {
  if (!fn || !name)
    return false;
  for (size_t i = 0; i < fn->params.len; ++i)
    if (fn->params.data[i].name && strcmp(fn->params.data[i].name, name) == 0)
      return true;
  return false;
}

static const expr_t *stmt_ownership_rules_find_borrow(const expr_t *e) {
  if (!e)
    return NULL;
  if (stmt_ownership_rules_is_borrow_expr(e))
    return e;
  switch (e->kind) {
  case NY_E_UNARY:
    return stmt_ownership_rules_find_borrow(e->as.unary.right);
  case NY_E_BINARY:
  case NY_E_LOGICAL: {
    const expr_t *found = stmt_ownership_rules_find_borrow(e->as.binary.left);
    return found ? found : stmt_ownership_rules_find_borrow(e->as.binary.right);
  }
  case NY_E_TERNARY: {
    const expr_t *found = stmt_ownership_rules_find_borrow(e->as.ternary.cond);
    if (!found)
      found = stmt_ownership_rules_find_borrow(e->as.ternary.true_expr);
    return found ? found
                 : stmt_ownership_rules_find_borrow(e->as.ternary.false_expr);
  }
  case NY_E_CALL:
    for (size_t i = 0; i < e->as.call.args.len; ++i) {
      const expr_t *found =
          stmt_ownership_rules_find_borrow(e->as.call.args.data[i].val);
      if (found)
        return found;
    }
    return NULL;
  case NY_E_MEMCALL:
    for (size_t i = 0; i < e->as.memcall.args.len; ++i) {
      const expr_t *found =
          stmt_ownership_rules_find_borrow(e->as.memcall.args.data[i].val);
      if (found)
        return found;
    }
    return NULL;
  case NY_E_INDEX: {
    const expr_t *found = stmt_ownership_rules_find_borrow(e->as.index.target);
    if (!found)
      found = stmt_ownership_rules_find_borrow(e->as.index.start);
    if (!found)
      found = stmt_ownership_rules_find_borrow(e->as.index.stop);
    return found ? found : stmt_ownership_rules_find_borrow(e->as.index.step);
  }
  case NY_E_MEMBER:
    return stmt_ownership_rules_find_borrow(e->as.member.target);
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i) {
      const expr_t *found =
          stmt_ownership_rules_find_borrow(e->as.list_like.data[i]);
      if (found)
        return found;
    }
    return NULL;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      const expr_t *found =
          stmt_ownership_rules_find_borrow(e->as.dict.pairs.data[i].value);
      if (found)
        return found;
    }
    return NULL;
  default:
    return NULL;
  }
}

static bool stmt_ownership_rules_is_persistent_store(const expr_t *lhs) {
  return lhs && (lhs->kind == NY_E_MEMBER || lhs->kind == NY_E_INDEX ||
                 (lhs->kind == NY_E_UNARY && lhs->as.unary.op &&
                  strcmp(lhs->as.unary.op, "*") == 0));
}

static void stmt_ownership_rules_error(codegen_t *cg, token_t tok,
                                       const char *rule, const char *message) {
  (void)cg;
  if (ny_is_stdlib_tok(tok))
    return;
  ny_diag_error(tok, "[%s] ownership: %s", rule, message);
  if (cg)
    cg->had_error = 1;
}

static void stmt_ownership_rules_check_stmt(codegen_t *cg,
                                            const stmt_t *fn_stmt,
                                            const stmt_func_t *fn,
                                            const stmt_t *s, unsigned depth);

static void stmt_ownership_rules_check_expr(codegen_t *cg,
                                            const stmt_t *fn_stmt,
                                            const stmt_func_t *fn, token_t tok,
                                            const expr_t *e, unsigned depth) {
  if (!e)
    return;
  if (depth > 128) {
    stmt_ownership_rules_error(
        cg, tok, "B1", "borrow analysis exceeded its expression nesting limit");
    return;
  }
  if (e->kind == NY_E_BINARY && e->as.binary.op &&
      strcmp(e->as.binary.op, "=") == 0 &&
      stmt_ownership_rules_is_persistent_store(e->as.binary.left)) {
    const expr_t *borrow = stmt_ownership_rules_find_borrow(e->as.binary.right);
    if (borrow)
      stmt_ownership_rules_error(cg, borrow->tok, "B1",
                                 "a second-class borrow cannot be stored in a "
                                 "field, collection, or dereferenced place");
  }
  if (e->kind == NY_E_MEMCALL && e->as.memcall.name &&
      (strcmp(e->as.memcall.name, "append") == 0 ||
       strcmp(e->as.memcall.name, "push") == 0 ||
       strcmp(e->as.memcall.name, "insert") == 0 ||
       strcmp(e->as.memcall.name, "set") == 0)) {
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      if (stmt_ownership_rules_find_borrow(e->as.memcall.args.data[i].val)) {
        stmt_ownership_rules_error(
            cg, e->as.memcall.args.data[i].val->tok, "B1",
            "a second-class borrow cannot be stored in a collection");
        break;
      }
  }
  if (e->kind == NY_E_CALL) {
    const char *callee =
        e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT
            ? e->as.call.callee->as.ident.name
            : NULL;
    const char *callee_leaf = callee ? strrchr(callee, '.') : NULL;
    callee_leaf = callee_leaf ? callee_leaf + 1 : callee;
    size_t callee_len = callee_leaf ? strlen(callee_leaf) : 0;
    bool field_store =
        stmt_ownership_rules_name_is(callee_leaf, "store_layout_field") ||
        (callee_len >= sizeof("_store_field") - 1 &&
         strcmp(callee_leaf + callee_len - (sizeof("_store_field") - 1),
                "_store_field") == 0);
    if (field_store) {
      for (size_t i = 0; i < e->as.call.args.len; ++i)
        if (stmt_ownership_rules_find_borrow(e->as.call.args.data[i].val)) {
          stmt_ownership_rules_error(
              cg, e->as.call.args.data[i].val->tok, "B1",
              "a second-class borrow cannot be stored in a field");
          break;
        }
    }
    if (stmt_ownership_rules_name_is(callee, "append") ||
        stmt_ownership_rules_name_is(callee, "dict_set") ||
        stmt_ownership_rules_name_is(callee, "set")) {
      for (size_t i = 1; i < e->as.call.args.len; ++i)
        if (stmt_ownership_rules_find_borrow(e->as.call.args.data[i].val)) {
          stmt_ownership_rules_error(
              cg, e->as.call.args.data[i].val->tok, "B1",
              "a second-class borrow cannot be stored in a collection");
          break;
        }
    }
  }
  switch (e->kind) {
  case NY_E_UNARY:
    stmt_ownership_rules_check_expr(cg, fn_stmt, fn, tok, e->as.unary.right,
                                    depth + 1);
    break;
  case NY_E_BINARY:
  case NY_E_LOGICAL:
    stmt_ownership_rules_check_expr(cg, fn_stmt, fn, tok, e->as.binary.left,
                                    depth + 1);
    stmt_ownership_rules_check_expr(cg, fn_stmt, fn, tok, e->as.binary.right,
                                    depth + 1);
    break;
  case NY_E_TERNARY:
    stmt_ownership_rules_check_expr(cg, fn_stmt, fn, tok, e->as.ternary.cond,
                                    depth + 1);
    stmt_ownership_rules_check_expr(cg, fn_stmt, fn, tok,
                                    e->as.ternary.true_expr, depth + 1);
    stmt_ownership_rules_check_expr(cg, fn_stmt, fn, tok,
                                    e->as.ternary.false_expr, depth + 1);
    break;
  case NY_E_CALL:
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      stmt_ownership_rules_check_expr(cg, fn_stmt, fn, tok,
                                      e->as.call.args.data[i].val, depth + 1);
    break;
  case NY_E_MEMCALL:
    stmt_ownership_rules_check_expr(cg, fn_stmt, fn, tok, e->as.memcall.target,
                                    depth + 1);
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      stmt_ownership_rules_check_expr(
          cg, fn_stmt, fn, tok, e->as.memcall.args.data[i].val, depth + 1);
    break;
  case NY_E_INDEX:
    stmt_ownership_rules_check_expr(cg, fn_stmt, fn, tok, e->as.index.target,
                                    depth + 1);
    stmt_ownership_rules_check_expr(cg, fn_stmt, fn, tok, e->as.index.start,
                                    depth + 1);
    stmt_ownership_rules_check_expr(cg, fn_stmt, fn, tok, e->as.index.stop,
                                    depth + 1);
    stmt_ownership_rules_check_expr(cg, fn_stmt, fn, tok, e->as.index.step,
                                    depth + 1);
    break;
  case NY_E_MEMBER:
    stmt_ownership_rules_check_expr(cg, fn_stmt, fn, tok, e->as.member.target,
                                    depth + 1);
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      stmt_ownership_rules_check_expr(cg, fn_stmt, fn, tok,
                                      e->as.list_like.data[i], depth + 1);
    break;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      stmt_ownership_rules_check_expr(cg, fn_stmt, fn, tok,
                                      e->as.dict.pairs.data[i].key, depth + 1);
      stmt_ownership_rules_check_expr(
          cg, fn_stmt, fn, tok, e->as.dict.pairs.data[i].value, depth + 1);
    }
    break;
  case NY_E_LAMBDA:
    stmt_ownership_rules_check_stmt(cg, fn_stmt, fn, e->as.lambda.body,
                                    depth + 1);
    break;
  default:
    break;
  }
}

static void stmt_ownership_rules_check_stmt(codegen_t *cg,
                                            const stmt_t *fn_stmt,
                                            const stmt_func_t *fn,
                                            const stmt_t *s, unsigned depth) {
  if (!s)
    return;
  if (depth > 128) {
    stmt_ownership_rules_error(
        cg, s->tok, "B1",
        "borrow analysis exceeded its statement nesting limit");
    return;
  }
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      stmt_ownership_rules_check_stmt(cg, fn_stmt, fn, s->as.block.body.data[i],
                                      depth + 1);
    break;
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; ++i)
      stmt_ownership_rules_check_expr(cg, fn_stmt, fn, s->tok,
                                      s->as.var.exprs.data[i], depth + 1);
    break;
  case NY_S_EXPR:
    stmt_ownership_rules_check_expr(cg, fn_stmt, fn, s->tok, s->as.expr.expr,
                                    depth + 1);
    break;
  case NY_S_RETURN: {
    const expr_t *value = s->as.ret.value;
    const expr_t *source = stmt_ownership_rules_borrow_source(value);
    if (source) {
      const char *name =
          source->kind == NY_E_IDENT ? source->as.ident.name : NULL;
      if (!stmt_ownership_rules_param_exists(fn, name) ||
          !stmt_ownership_rules_has_returns_borrow(fn_stmt, name))
        stmt_ownership_rules_error(
            cg, value->tok, "B2",
            "returning a borrow requires @returns_borrow(param) naming its "
            "source parameter");
    }
    stmt_ownership_rules_check_expr(cg, fn_stmt, fn, s->tok, value, depth + 1);
    break;
  }
  case NY_S_IF:
    stmt_ownership_rules_check_expr(cg, fn_stmt, fn, s->tok, s->as.iff.test,
                                    depth + 1);
    stmt_ownership_rules_check_stmt(cg, fn_stmt, fn, s->as.iff.conseq,
                                    depth + 1);
    stmt_ownership_rules_check_stmt(cg, fn_stmt, fn, s->as.iff.alt, depth + 1);
    break;
  case NY_S_WHILE:
    stmt_ownership_rules_check_expr(cg, fn_stmt, fn, s->tok, s->as.whl.test,
                                    depth + 1);
    stmt_ownership_rules_check_stmt(cg, fn_stmt, fn, s->as.whl.body, depth + 1);
    break;
  case NY_S_FOR:
    stmt_ownership_rules_check_expr(cg, fn_stmt, fn, s->tok, s->as.fr.iterable,
                                    depth + 1);
    stmt_ownership_rules_check_stmt(cg, fn_stmt, fn, s->as.fr.body, depth + 1);
    break;
  case NY_S_TRY:
    stmt_ownership_rules_check_stmt(cg, fn_stmt, fn, s->as.tr.body, depth + 1);
    stmt_ownership_rules_check_stmt(cg, fn_stmt, fn, s->as.tr.handler,
                                    depth + 1);
    break;
  default:
    break;
  }
}

typedef struct stmt_ownership_rules_borrow_binding {
  const char *name;
  const char *source;
  bool contains_borrow;
} stmt_ownership_rules_borrow_binding_t;

typedef struct stmt_ownership_rules_borrow_bindings {
  stmt_ownership_rules_borrow_binding_t *data;
  size_t len;
  size_t cap;
} stmt_ownership_rules_borrow_bindings_t;

static const char *stmt_ownership_rules_borrow_source_for_name(
    const stmt_ownership_rules_borrow_bindings_t *bindings, const char *name) {
  if (!bindings || !name)
    return NULL;
  for (size_t i = bindings->len; i > 0; --i)
    if (strcmp(bindings->data[i - 1].name, name) == 0)
      return bindings->data[i - 1].source;
  return NULL;
}

static bool stmt_ownership_rules_name_contains_borrow(
    const stmt_ownership_rules_borrow_bindings_t *bindings, const char *name) {
  if (!bindings || !name)
    return false;
  for (size_t i = bindings->len; i > 0; --i)
    if (strcmp(bindings->data[i - 1].name, name) == 0)
      return bindings->data[i - 1].contains_borrow;
  return false;
}

static void stmt_ownership_rules_bind_borrow(
    stmt_ownership_rules_borrow_bindings_t *bindings, const char *name,
    const char *source) {
  if (!bindings || !name || !*name)
    return;
  for (size_t i = bindings->len; i > 0; --i) {
    if (strcmp(bindings->data[i - 1].name, name) != 0)
      continue;
    if (source) {
      bindings->data[i - 1].source = source;
      bindings->data[i - 1].contains_borrow = true;
    } else {
      bindings->data[i - 1] = bindings->data[--bindings->len];
    }
    return;
  }
  if (!source)
    return;
  if (bindings->len == bindings->cap) {
    size_t next = bindings->cap ? bindings->cap * 2 : 8;
    if (next < bindings->cap || next > SIZE_MAX / sizeof(*bindings->data))
      return;
    void *p = realloc(bindings->data, next * sizeof(*bindings->data));
    if (!p)
      return;
    bindings->data = p;
    bindings->cap = next;
  }
  bindings->data[bindings->len++] =
      (stmt_ownership_rules_borrow_binding_t){name, source, source != NULL};
}

static bool stmt_ownership_rules_bind_borrow_capture(
    stmt_ownership_rules_borrow_bindings_t *bindings, const char *name,
    bool contains_borrow) {
  if (!bindings || !name || !*name)
    return true;
  for (size_t i = bindings->len; i > 0; --i) {
    stmt_ownership_rules_borrow_binding_t *binding = &bindings->data[i - 1];
    if (strcmp(binding->name, name) != 0)
      continue;
    binding->contains_borrow = contains_borrow || binding->source != NULL;
    if (!binding->contains_borrow)
      bindings->data[i - 1] = bindings->data[--bindings->len];
    return true;
  }
  if (!contains_borrow)
    return true;
  if (bindings->len == bindings->cap) {
    size_t next = bindings->cap ? bindings->cap * 2 : 8;
    if (next < bindings->cap || next > SIZE_MAX / sizeof(*bindings->data))
      return false;
    void *p = realloc(bindings->data, next * sizeof(*bindings->data));
    if (!p)
      return false;
    bindings->data = p;
    bindings->cap = next;
  }
  bindings->data[bindings->len++] =
      (stmt_ownership_rules_borrow_binding_t){name, NULL, true};
  return true;
}

static bool stmt_ownership_rules_clone_borrow_bindings(
    stmt_ownership_rules_borrow_bindings_t *dst,
    const stmt_ownership_rules_borrow_bindings_t *src) {
  memset(dst, 0, sizeof(*dst));
  if (!src || !src->len)
    return true;
  if (src->len > SIZE_MAX / sizeof(*dst->data))
    return false;
  dst->data = malloc(src->len * sizeof(*dst->data));
  if (!dst->data)
    return false;
  memcpy(dst->data, src->data, src->len * sizeof(*dst->data));
  dst->len = src->len;
  dst->cap = src->len;
  return true;
}

static bool stmt_ownership_rules_declares_borrow_name(const stmt_t *s,
                                                      const char *name,
                                                      unsigned depth) {
  if (!s || !name || depth > 128)
    return false;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (stmt_ownership_rules_declares_borrow_name(s->as.block.body.data[i],
                                                    name, depth + 1))
        return true;
    return false;
  case NY_S_VAR:
    if (s->as.var.is_decl)
      for (size_t i = 0; i < s->as.var.names.len; ++i)
        if (s->as.var.names.data[i] &&
            strcmp(s->as.var.names.data[i], name) == 0)
          return true;
    return false;
  case NY_S_IF:
    return stmt_ownership_rules_declares_borrow_name(s->as.iff.conseq, name,
                                                     depth + 1) ||
           stmt_ownership_rules_declares_borrow_name(s->as.iff.alt, name,
                                                     depth + 1);
  case NY_S_WHILE:
    return stmt_ownership_rules_declares_borrow_name(s->as.whl.body, name,
                                                     depth + 1);
  case NY_S_FOR:
    return stmt_ownership_rules_declares_borrow_name(s->as.fr.body, name,
                                                     depth + 1);
  case NY_S_TRY:
    return stmt_ownership_rules_declares_borrow_name(s->as.tr.body, name,
                                                     depth + 1) ||
           stmt_ownership_rules_declares_borrow_name(s->as.tr.handler, name,
                                                     depth + 1);
  case NY_S_MATCH:
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      if (stmt_ownership_rules_declares_borrow_name(
              s->as.match.arms.data[i].conseq, name, depth + 1))
        return true;
    return stmt_ownership_rules_declares_borrow_name(s->as.match.default_conseq,
                                                     name, depth + 1);
  default:
    return false;
  }
}

static void stmt_ownership_rules_join_borrow_branches(
    codegen_t *cg, token_t tok, stmt_ownership_rules_borrow_bindings_t *dst,
    const stmt_ownership_rules_borrow_bindings_t *base,
    const stmt_ownership_rules_borrow_bindings_t *left,
    const stmt_ownership_rules_borrow_bindings_t *right,
    const stmt_t *left_stmt, const stmt_t *right_stmt) {
  stmt_ownership_rules_borrow_bindings_t merged = {0};
  if (!stmt_ownership_rules_clone_borrow_bindings(&merged, base)) {
    stmt_ownership_rules_error(
        cg, tok, "B2",
        "cannot validate returned borrow sources because ownership analysis "
        "ran out of memory");
    return;
  }
  static const char ambiguous_source[] = "<ambiguous borrow source>";
  for (size_t i = 0; i < base->len; ++i) {
    const char *name = base->data[i].name;
    const char *left_source =
        stmt_ownership_rules_borrow_source_for_name(left, name);
    const char *right_source =
        stmt_ownership_rules_borrow_source_for_name(right, name);
    if (stmt_ownership_rules_declares_borrow_name(left_stmt, name, 0) ||
        stmt_ownership_rules_declares_borrow_name(right_stmt, name, 0)) {
      stmt_ownership_rules_bind_borrow(&merged, name, ambiguous_source);
    } else if (left_source && right_source &&
               strcmp(left_source, right_source) == 0) {
      stmt_ownership_rules_bind_borrow(&merged, name, left_source);
    } else if (!left_source && !right_source) {
      stmt_ownership_rules_bind_borrow(&merged, name, NULL);
    } else {
      stmt_ownership_rules_bind_borrow(&merged, name, ambiguous_source);
    }
    if (!stmt_ownership_rules_bind_borrow_capture(
            &merged, name,
            stmt_ownership_rules_name_contains_borrow(left, name) ||
                stmt_ownership_rules_name_contains_borrow(right, name))) {
      free(merged.data);
      stmt_ownership_rules_error(
          cg, tok, "B2",
          "cannot validate borrow capture provenance because ownership "
          "analysis ran out of memory");
      return;
    }
  }
  /*
   * Also merge provenance first introduced by assignment in either arm.
   */
  const stmt_ownership_rules_borrow_bindings_t *branches[] = {left, right};
  for (size_t side = 0; side < 2; ++side) {
    const stmt_ownership_rules_borrow_bindings_t *branch = branches[side];
    for (size_t i = 0; branch && i < branch->len; ++i) {
      const char *name = branch->data[i].name;
      bool known = false;
      for (size_t j = 0; j < base->len; ++j)
        known |= strcmp(base->data[j].name, name) == 0;
      for (size_t j = 0; j < i; ++j)
        known |= strcmp(branch->data[j].name, name) == 0;
      if (known ||
          stmt_ownership_rules_declares_borrow_name(left_stmt, name, 0) ||
          stmt_ownership_rules_declares_borrow_name(right_stmt, name, 0))
        continue;
      const char *left_source =
          stmt_ownership_rules_borrow_source_for_name(left, name);
      const char *right_source =
          stmt_ownership_rules_borrow_source_for_name(right, name);
      if (left_source && right_source && strcmp(left_source, right_source) == 0)
        stmt_ownership_rules_bind_borrow(&merged, name, left_source);
      else if (left_source || right_source)
        stmt_ownership_rules_bind_borrow(&merged, name, ambiguous_source);
      if (!stmt_ownership_rules_bind_borrow_capture(
              &merged, name,
              stmt_ownership_rules_name_contains_borrow(left, name) ||
                  stmt_ownership_rules_name_contains_borrow(right, name))) {
        free(merged.data);
        stmt_ownership_rules_error(
            cg, tok, "B2",
            "cannot validate borrow capture provenance because ownership "
            "analysis ran out of memory");
        return;
      }
    }
  }
  free(dst->data);
  *dst = merged;
}

static const char *stmt_ownership_rules_borrow_expr_source(
    const stmt_ownership_rules_borrow_bindings_t *bindings,
    const expr_t *value) {
  const expr_t *source = stmt_ownership_rules_borrow_source(value);
  if (source && source->kind == NY_E_IDENT)
    return source->as.ident.name;
  if (value && value->kind == NY_E_IDENT)
    return stmt_ownership_rules_borrow_source_for_name(bindings,
                                                       value->as.ident.name);
  return NULL;
}

static bool stmt_ownership_rules_expr_mentions_name(const expr_t *e,
                                                    const char *name,
                                                    unsigned depth);

static bool stmt_ownership_rules_expr_contains_borrow(
    const stmt_ownership_rules_borrow_bindings_t *bindings,
    const expr_t *value) {
  if (!value)
    return false;
  if (stmt_ownership_rules_find_borrow(value))
    return true;
  for (size_t i = 0; bindings && i < bindings->len; ++i)
    if (bindings->data[i].contains_borrow &&
        stmt_ownership_rules_expr_mentions_name(value, bindings->data[i].name,
                                                0))
      return true;
  return false;
}

static bool stmt_ownership_rules_stmt_mentions_name(const stmt_t *s,
                                                    const char *name,
                                                    unsigned depth);

static bool stmt_ownership_rules_expr_mentions_name(const expr_t *e,
                                                    const char *name,
                                                    unsigned depth) {
  if (!e || !name || depth > 128)
    return false;
  switch (e->kind) {
  case NY_E_IDENT:
    return e->as.ident.name && strcmp(e->as.ident.name, name) == 0;
  case NY_E_LITERAL:
  case NY_E_INFERRED_MEMBER:
  case NY_E_EMBED:
    return false;
  case NY_E_UNARY:
    return stmt_ownership_rules_expr_mentions_name(e->as.unary.right, name,
                                                   depth + 1);
  case NY_E_BINARY:
  case NY_E_LOGICAL:
    return stmt_ownership_rules_expr_mentions_name(e->as.binary.left, name,
                                                   depth + 1) ||
           stmt_ownership_rules_expr_mentions_name(e->as.binary.right, name,
                                                   depth + 1);
  case NY_E_TERNARY:
    return stmt_ownership_rules_expr_mentions_name(e->as.ternary.cond, name,
                                                   depth + 1) ||
           stmt_ownership_rules_expr_mentions_name(e->as.ternary.true_expr,
                                                   name, depth + 1) ||
           stmt_ownership_rules_expr_mentions_name(e->as.ternary.false_expr,
                                                   name, depth + 1);
  case NY_E_CALL:
    if (stmt_ownership_rules_expr_mentions_name(e->as.call.callee, name,
                                                depth + 1))
      return true;
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      if (stmt_ownership_rules_expr_mentions_name(e->as.call.args.data[i].val,
                                                  name, depth + 1))
        return true;
    return false;
  case NY_E_MEMCALL:
    if (stmt_ownership_rules_expr_mentions_name(e->as.memcall.target, name,
                                                depth + 1))
      return true;
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      if (stmt_ownership_rules_expr_mentions_name(
              e->as.memcall.args.data[i].val, name, depth + 1))
        return true;
    return false;
  case NY_E_INDEX:
    return stmt_ownership_rules_expr_mentions_name(e->as.index.target, name,
                                                   depth + 1) ||
           stmt_ownership_rules_expr_mentions_name(e->as.index.start, name,
                                                   depth + 1) ||
           stmt_ownership_rules_expr_mentions_name(e->as.index.stop, name,
                                                   depth + 1) ||
           stmt_ownership_rules_expr_mentions_name(e->as.index.step, name,
                                                   depth + 1);
  case NY_E_MEMBER:
    return stmt_ownership_rules_expr_mentions_name(e->as.member.target, name,
                                                   depth + 1) ||
           stmt_ownership_rules_expr_mentions_name(e->as.member.computed_name,
                                                   name, depth + 1);
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      if (stmt_ownership_rules_expr_mentions_name(e->as.list_like.data[i], name,
                                                  depth + 1))
        return true;
    return false;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i)
      if (stmt_ownership_rules_expr_mentions_name(e->as.dict.pairs.data[i].key,
                                                  name, depth + 1) ||
          stmt_ownership_rules_expr_mentions_name(
              e->as.dict.pairs.data[i].value, name, depth + 1))
        return true;
    return false;
  case NY_E_LAMBDA:
  case NY_E_FN:
    for (size_t i = 0; i < e->as.lambda.params.len; ++i)
      if (e->as.lambda.params.data[i].name &&
          strcmp(e->as.lambda.params.data[i].name, name) == 0)
        return false;
    return stmt_ownership_rules_stmt_mentions_name(e->as.lambda.body, name,
                                                   depth + 1);
  case NY_E_ASM:
    for (size_t i = 0; i < e->as.as_asm.args.len; ++i)
      if (stmt_ownership_rules_expr_mentions_name(e->as.as_asm.args.data[i],
                                                  name, depth + 1))
        return true;
    return false;
  case NY_E_COMPTIME:
    return stmt_ownership_rules_stmt_mentions_name(e->as.comptime_expr.body,
                                                   name, depth + 1);
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; ++i)
      if (e->as.fstring.parts.data[i].kind == NY_FSP_EXPR &&
          stmt_ownership_rules_expr_mentions_name(
              e->as.fstring.parts.data[i].as.e, name, depth + 1))
        return true;
    return false;
  case NY_E_MATCH:
    if (stmt_ownership_rules_expr_mentions_name(e->as.match.test, name,
                                                depth + 1))
      return true;
    for (size_t i = 0; i < e->as.match.arms.len; ++i) {
      const match_arm_t *arm = &e->as.match.arms.data[i];
      if (stmt_ownership_rules_expr_mentions_name(arm->guard, name,
                                                  depth + 1) ||
          stmt_ownership_rules_stmt_mentions_name(arm->conseq, name, depth + 1))
        return true;
    }
    return stmt_ownership_rules_stmt_mentions_name(e->as.match.default_conseq,
                                                   name, depth + 1);
  case NY_E_PTR_TYPE:
    return stmt_ownership_rules_expr_mentions_name(e->as.ptr_type.target, name,
                                                   depth + 1);
  case NY_E_DEREF:
    return stmt_ownership_rules_expr_mentions_name(e->as.deref.target, name,
                                                   depth + 1);
  case NY_E_SIZEOF:
    return stmt_ownership_rules_expr_mentions_name(e->as.szof.target, name,
                                                   depth + 1);
  case NY_E_TRY:
    return stmt_ownership_rules_expr_mentions_name(e->as.try_expr.target, name,
                                                   depth + 1);
  case NY_E_QUOTE:
    return stmt_ownership_rules_stmt_mentions_name(e->as.quote.body, name,
                                                   depth + 1) ||
           stmt_ownership_rules_expr_mentions_name(e->as.quote.expr, name,
                                                   depth + 1);
  case NY_E_SPLICE:
    return stmt_ownership_rules_expr_mentions_name(e->as.splice.expr, name,
                                                   depth + 1);
  case NY_E_EXISTS:
    return stmt_ownership_rules_expr_mentions_name(e->as.exists.body, name,
                                                   depth + 1);
  default:
    return false;
  }
}

static bool stmt_ownership_resource_path_mentions_expr(const expr_t *e,
                                                       const char *path,
                                                       unsigned depth) {
  if (!e || !path)
    return false;
  if (depth > 128)
    return true;
  char place[512];
  size_t place_len = 0;
  if (stmt_ownership_exclusive_place_text(e, place, sizeof(place),
                                          &place_len)) {
    (void)place_len;
    return stmt_ownership_resource_paths_overlap(place, path);
  }
#define NY_RESOURCE_PATH_EXPR(child)                                           \
  stmt_ownership_resource_path_mentions_expr((child), path, depth + 1)
  switch (e->kind) {
  case NY_E_UNARY:
    return NY_RESOURCE_PATH_EXPR(e->as.unary.right);
  case NY_E_BINARY:
    return NY_RESOURCE_PATH_EXPR(e->as.binary.left) ||
           NY_RESOURCE_PATH_EXPR(e->as.binary.right);
  case NY_E_LOGICAL:
    return NY_RESOURCE_PATH_EXPR(e->as.logical.left) ||
           NY_RESOURCE_PATH_EXPR(e->as.logical.right);
  case NY_E_TERNARY:
    return NY_RESOURCE_PATH_EXPR(e->as.ternary.cond) ||
           NY_RESOURCE_PATH_EXPR(e->as.ternary.true_expr) ||
           NY_RESOURCE_PATH_EXPR(e->as.ternary.false_expr);
  case NY_E_CALL:
    /*
     * Indexed assignment is represented as set_idx(list, index, value).
     * Preserve the element path here: treating the list argument as a whole
     * would make a proven-disjoint store conflict with every live element
     * borrow, while ignoring the call would miss aliases such as -1 and 2.
     */
    if (e->as.call.synthetic_index_assignment && e->as.call.callee &&
        e->as.call.callee->kind == NY_E_IDENT &&
        stmt_ownership_rules_name_is(e->as.call.callee->as.ident.name,
                                     "set_idx") &&
        e->as.call.args.len == 3) {
      const expr_t *target = e->as.call.args.data[0].val;
      const expr_t *index = e->as.call.args.data[1].val;
      const expr_t *value = e->as.call.args.data[2].val;
      char target_place[512];
      size_t target_len = 0;
      int64_t index_value = 0;
      if (stmt_ownership_exclusive_place_text(
              target, target_place, sizeof(target_place), &target_len)) {
        if (index && index->kind == NY_E_LITERAL &&
            index->as.literal.kind == NY_LIT_STR &&
            index->as.literal.as.s.data) {
          if (target_len > sizeof(target_place) - 4 ||
              index->as.literal.as.s.len >
                  (sizeof(target_place) - target_len - 4) / 2)
            return true;
          static const char hex[] = "0123456789abcdef";
          size_t suffix_len = 0;
          target_place[target_len + suffix_len++] = '[';
          target_place[target_len + suffix_len++] = '@';
          for (size_t i = 0; i < index->as.literal.as.s.len; ++i) {
            unsigned char byte = (unsigned char)index->as.literal.as.s.data[i];
            target_place[target_len + suffix_len++] = hex[byte >> 4];
            target_place[target_len + suffix_len++] = hex[byte & 0x0f];
          }
          target_place[target_len + suffix_len++] = ']';
          target_place[target_len + suffix_len] = '\0';
          if (stmt_ownership_resource_paths_overlap(target_place, path) ||
              NY_RESOURCE_PATH_EXPR(index) || NY_RESOURCE_PATH_EXPR(value))
            return true;
          return false;
        }
        char index_suffix[48];
        int suffix_len;
        if (stmt_ownership_exclusive_index_constant(index, &index_value, 0))
          suffix_len = snprintf(index_suffix, sizeof(index_suffix), "[%lld]",
                                (long long)index_value);
        else
          suffix_len = snprintf(index_suffix, sizeof(index_suffix), "[*]");
        if (suffix_len <= 0 || (size_t)suffix_len >= sizeof(index_suffix) ||
            target_len + (size_t)suffix_len + 1 > sizeof(target_place))
          return true;
        memcpy(target_place + target_len, index_suffix, (size_t)suffix_len + 1);
        if (stmt_ownership_resource_paths_overlap(target_place, path) ||
            NY_RESOURCE_PATH_EXPR(index) || NY_RESOURCE_PATH_EXPR(value))
          return true;
      } else if (NY_RESOURCE_PATH_EXPR(target) ||
                 NY_RESOURCE_PATH_EXPR(index) || NY_RESOURCE_PATH_EXPR(value)) {
        return true;
      }
      return false;
    }
    if (NY_RESOURCE_PATH_EXPR(e->as.call.callee))
      return true;
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      if (NY_RESOURCE_PATH_EXPR(e->as.call.args.data[i].val))
        return true;
    return false;
  case NY_E_MEMCALL:
    if (NY_RESOURCE_PATH_EXPR(e->as.memcall.target))
      return true;
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      if (NY_RESOURCE_PATH_EXPR(e->as.memcall.args.data[i].val))
        return true;
    return false;
  case NY_E_MEMBER:
    return NY_RESOURCE_PATH_EXPR(e->as.member.target);
  case NY_E_INDEX:
    return NY_RESOURCE_PATH_EXPR(e->as.index.target) ||
           NY_RESOURCE_PATH_EXPR(e->as.index.start) ||
           NY_RESOURCE_PATH_EXPR(e->as.index.stop) ||
           NY_RESOURCE_PATH_EXPR(e->as.index.step);
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      if (NY_RESOURCE_PATH_EXPR(e->as.list_like.data[i]))
        return true;
    return false;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i)
      if (NY_RESOURCE_PATH_EXPR(e->as.dict.pairs.data[i].key) ||
          NY_RESOURCE_PATH_EXPR(e->as.dict.pairs.data[i].value))
        return true;
    return false;
  case NY_E_DEREF:
    return NY_RESOURCE_PATH_EXPR(e->as.deref.target);
  case NY_E_TRY:
    return NY_RESOURCE_PATH_EXPR(e->as.try_expr.target);
  case NY_E_MATCH:
    if (NY_RESOURCE_PATH_EXPR(e->as.match.test))
      return true;
    for (size_t i = 0; i < e->as.match.arms.len; ++i)
      if (NY_RESOURCE_PATH_EXPR(e->as.match.arms.data[i].guard) ||
          stmt_ownership_resource_path_mentions_stmt(
              e->as.match.arms.data[i].conseq, path, depth + 1))
        return true;
    return stmt_ownership_resource_path_mentions_stmt(
        e->as.match.default_conseq, path, depth + 1);
  default:
    return false;
  }
#undef NY_RESOURCE_PATH_EXPR
}

static bool stmt_ownership_resource_path_mentions_stmt(const stmt_t *s,
                                                       const char *path,
                                                       unsigned depth) {
  if (!s || !path)
    return false;
  if (depth > 128)
    return true;
#define NY_RESOURCE_PATH_STMT(child)                                           \
  stmt_ownership_resource_path_mentions_stmt((child), path, depth + 1)
#define NY_RESOURCE_PATH_EXPR(child)                                           \
  stmt_ownership_resource_path_mentions_expr((child), path, depth + 1)
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (NY_RESOURCE_PATH_STMT(s->as.block.body.data[i]))
        return true;
    return false;
  case NY_S_VAR:
    if (!s->as.var.is_decl)
      for (size_t i = 0; i < s->as.var.names.len; ++i) {
        const char *target = s->as.var.names.data[i];
        if (target && stmt_ownership_resource_paths_overlap(target, path))
          return true;
      }
    for (size_t i = 0; i < s->as.var.exprs.len; ++i)
      if (NY_RESOURCE_PATH_EXPR(s->as.var.exprs.data[i]))
        return true;
    return false;
  case NY_S_EXPR:
    return NY_RESOURCE_PATH_EXPR(s->as.expr.expr);
  case NY_S_RETURN:
    return NY_RESOURCE_PATH_EXPR(s->as.ret.value);
  case NY_S_IF:
    return NY_RESOURCE_PATH_EXPR(s->as.iff.test) ||
           NY_RESOURCE_PATH_STMT(s->as.iff.conseq) ||
           NY_RESOURCE_PATH_STMT(s->as.iff.alt);
  case NY_S_WHILE:
    return NY_RESOURCE_PATH_EXPR(s->as.whl.test) ||
           NY_RESOURCE_PATH_STMT(s->as.whl.body) ||
           NY_RESOURCE_PATH_STMT(s->as.whl.update);
  case NY_S_FOR:
    return NY_RESOURCE_PATH_EXPR(s->as.fr.iterable) ||
           NY_RESOURCE_PATH_EXPR(s->as.fr.cond) ||
           NY_RESOURCE_PATH_STMT(s->as.fr.body) ||
           NY_RESOURCE_PATH_STMT(s->as.fr.update);
  case NY_S_TRY:
    return NY_RESOURCE_PATH_STMT(s->as.tr.body) ||
           NY_RESOURCE_PATH_STMT(s->as.tr.handler);
  case NY_S_MATCH:
    if (NY_RESOURCE_PATH_EXPR(s->as.match.test))
      return true;
    for (size_t i = 0; i < s->as.match.arms.len; ++i)
      if (NY_RESOURCE_PATH_EXPR(s->as.match.arms.data[i].guard) ||
          NY_RESOURCE_PATH_STMT(s->as.match.arms.data[i].conseq))
        return true;
    return NY_RESOURCE_PATH_STMT(s->as.match.default_conseq);
  case NY_S_DEFER:
    return NY_RESOURCE_PATH_STMT(s->as.de.body);
  default:
    return false;
  }
#undef NY_RESOURCE_PATH_EXPR
#undef NY_RESOURCE_PATH_STMT
}

static bool stmt_ownership_rules_stmt_mentions_name(const stmt_t *s,
                                                    const char *name,
                                                    unsigned depth) {
  if (!s || !name || depth > 128)
    return false;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (stmt_ownership_rules_stmt_mentions_name(s->as.block.body.data[i],
                                                  name, depth + 1))
        return true;
    return false;
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; ++i)
      if (stmt_ownership_rules_expr_mentions_name(s->as.var.exprs.data[i], name,
                                                  depth + 1))
        return true;
    return false;
  case NY_S_EXPR:
    return stmt_ownership_rules_expr_mentions_name(s->as.expr.expr, name,
                                                   depth + 1);
  case NY_S_RETURN:
    return stmt_ownership_rules_expr_mentions_name(s->as.ret.value, name,
                                                   depth + 1);
  case NY_S_IF:
    return stmt_ownership_rules_expr_mentions_name(s->as.iff.test, name,
                                                   depth + 1) ||
           stmt_ownership_rules_stmt_mentions_name(s->as.iff.init, name,
                                                   depth + 1) ||
           stmt_ownership_rules_stmt_mentions_name(s->as.iff.conseq, name,
                                                   depth + 1) ||
           stmt_ownership_rules_stmt_mentions_name(s->as.iff.alt, name,
                                                   depth + 1);
  case NY_S_WHILE:
    return stmt_ownership_rules_expr_mentions_name(s->as.whl.test, name,
                                                   depth + 1) ||
           stmt_ownership_rules_stmt_mentions_name(s->as.whl.init, name,
                                                   depth + 1) ||
           stmt_ownership_rules_stmt_mentions_name(s->as.whl.update, name,
                                                   depth + 1) ||
           stmt_ownership_rules_stmt_mentions_name(s->as.whl.body, name,
                                                   depth + 1);
  case NY_S_FOR:
    return stmt_ownership_rules_expr_mentions_name(s->as.fr.iterable, name,
                                                   depth + 1) ||
           stmt_ownership_rules_expr_mentions_name(s->as.fr.cond, name,
                                                   depth + 1) ||
           stmt_ownership_rules_stmt_mentions_name(s->as.fr.init, name,
                                                   depth + 1) ||
           stmt_ownership_rules_stmt_mentions_name(s->as.fr.update, name,
                                                   depth + 1) ||
           stmt_ownership_rules_stmt_mentions_name(s->as.fr.body, name,
                                                   depth + 1);
  case NY_S_GUARD:
    return stmt_ownership_rules_expr_mentions_name(s->as.guard.value, name,
                                                   depth + 1) ||
           stmt_ownership_rules_stmt_mentions_name(s->as.guard.fallback, name,
                                                   depth + 1);
  case NY_S_TRY:
    return stmt_ownership_rules_stmt_mentions_name(s->as.tr.body, name,
                                                   depth + 1) ||
           stmt_ownership_rules_stmt_mentions_name(s->as.tr.handler, name,
                                                   depth + 1);
  case NY_S_MATCH:
    if (stmt_ownership_rules_expr_mentions_name(s->as.match.test, name,
                                                depth + 1))
      return true;
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      const match_arm_t *arm = &s->as.match.arms.data[i];
      if (stmt_ownership_rules_expr_mentions_name(arm->guard, name,
                                                  depth + 1) ||
          stmt_ownership_rules_stmt_mentions_name(arm->conseq, name, depth + 1))
        return true;
    }
    return stmt_ownership_rules_stmt_mentions_name(s->as.match.default_conseq,
                                                   name, depth + 1);
  case NY_S_DEFER:
    return stmt_ownership_rules_stmt_mentions_name(s->as.de.body, name,
                                                   depth + 1);
  default:
    return false;
  }
}

static bool stmt_ownership_rules_return_condition_is_safe(const expr_t *e,
                                                          unsigned depth) {
  if (!e || depth > 64)
    return false;
  if (e->kind == NY_E_IDENT || e->kind == NY_E_LITERAL)
    return true;
  if (e->kind == NY_E_UNARY)
    return e->as.unary.op && strcmp(e->as.unary.op, "!") == 0 &&
           stmt_ownership_rules_return_condition_is_safe(e->as.unary.right,
                                                         depth + 1);
  if (e->kind == NY_E_LOGICAL && e->as.logical.op &&
      (strcmp(e->as.logical.op, "&&") == 0 ||
       strcmp(e->as.logical.op, "||") == 0))
    return stmt_ownership_rules_return_condition_is_safe(e->as.logical.left,
                                                         depth + 1) &&
           stmt_ownership_rules_return_condition_is_safe(e->as.logical.right,
                                                         depth + 1);
  return false;
}

static bool stmt_ownership_rules_returns_borrow_mut_param(
    const stmt_t *stmt, const char *param_name, unsigned depth) {
  if (!stmt || !param_name || depth > 64)
    return false;
  if (stmt->kind == NY_S_BLOCK) {
    if (stmt->as.block.body.len == 1)
      return stmt_ownership_rules_returns_borrow_mut_param(
          stmt->as.block.body.data[0], param_name, depth + 1);
    if (stmt->as.block.body.len >= 2 && stmt->as.block.body.len <= 16) {
      bool early_returns_borrow = true;
      for (size_t i = 0; i + 1 < stmt->as.block.body.len; ++i) {
        const stmt_t *branch = stmt->as.block.body.data[i];
        if (!branch || branch->kind != NY_S_IF || branch->as.iff.init ||
            branch->as.iff.alt ||
            !stmt_ownership_rules_return_condition_is_safe(branch->as.iff.test,
                                                           0) ||
            !stmt_ownership_rules_returns_borrow_mut_param(
                branch->as.iff.conseq, param_name, depth + 1)) {
          early_returns_borrow = false;
          break;
        }
      }
      if (early_returns_borrow &&
          stmt_ownership_rules_returns_borrow_mut_param(
              stmt->as.block.body.data[stmt->as.block.body.len - 1], param_name,
              depth + 1))
        return true;
    }
    if (stmt->as.block.body.len < 2 || stmt->as.block.body.len > 17)
      return false;
    const char *previous_alias = NULL;
    for (size_t i = 0; i + 1 < stmt->as.block.body.len; ++i) {
      const stmt_t *binding = stmt->as.block.body.data[i];
      if (!binding || binding->kind != NY_S_VAR || !binding->as.var.is_decl ||
          binding->as.var.names.len != 1 || binding->as.var.exprs.len != 1 ||
          !binding->as.var.names.data[0] || !binding->as.var.exprs.data[0])
        return false;
      const expr_t *value = binding->as.var.exprs.data[0];
      if (!previous_alias) {
        if (value->kind != NY_E_CALL || !value->as.call.callee ||
            value->as.call.callee->kind != NY_E_IDENT ||
            !stmt_ownership_rules_name_is(value->as.call.callee->as.ident.name,
                                          "borrow_mut") ||
            value->as.call.args.len != 1 || !value->as.call.args.data[0].val ||
            value->as.call.args.data[0].val->kind != NY_E_IDENT ||
            !value->as.call.args.data[0].val->as.ident.name ||
            strcmp(value->as.call.args.data[0].val->as.ident.name,
                   param_name) != 0)
          return false;
      } else if (value->kind != NY_E_IDENT || !value->as.ident.name ||
                 strcmp(value->as.ident.name, previous_alias) != 0) {
        return false;
      }
      previous_alias = binding->as.var.names.data[0];
    }
    const stmt_t *ret = stmt->as.block.body.data[stmt->as.block.body.len - 1];
    return previous_alias && ret && ret->kind == NY_S_RETURN &&
           ret->as.ret.value && ret->as.ret.value->kind == NY_E_IDENT &&
           ret->as.ret.value->as.ident.name &&
           strcmp(ret->as.ret.value->as.ident.name, previous_alias) == 0;
  }
  if (stmt->kind == NY_S_RETURN) {
    const expr_t *value = stmt->as.ret.value;
    if (!value || value->kind != NY_E_CALL || !value->as.call.callee)
      return false;
    if (value->as.call.callee->kind == NY_E_IDENT &&
        stmt_ownership_rules_name_is(value->as.call.callee->as.ident.name,
                                     "borrow_mut") &&
        value->as.call.args.len == 1 && value->as.call.args.data[0].val &&
        value->as.call.args.data[0].val->kind == NY_E_IDENT &&
        value->as.call.args.data[0].val->as.ident.name &&
        strcmp(value->as.call.args.data[0].val->as.ident.name, param_name) == 0)
      return true;
    const stmt_t *callee = value->semantic.canonical_callee_stmt;
    const char *callee_param =
        stmt_ownership_rules_returns_borrow_param(callee);
    if (!callee || callee->kind != NY_S_FUNC || !callee_param ||
        !callee->as.fn.body || depth >= 64)
      return false;
    const expr_t *actual = NULL;
    for (size_t i = 0; i < value->as.call.args.len; ++i) {
      const call_arg_t *arg = &value->as.call.args.data[i];
      const char *name = arg->name ? arg->name
                         : i < callee->as.fn.params.len
                             ? callee->as.fn.params.data[i].name
                             : NULL;
      if (name && strcmp(name, callee_param) == 0) {
        actual = arg->val;
        break;
      }
    }
    if (!actual || actual->kind != NY_E_IDENT || !actual->as.ident.name ||
        strcmp(actual->as.ident.name, param_name) != 0)
      return false;
    return stmt_ownership_rules_returns_borrow_mut_param(
        callee->as.fn.body, callee_param, depth + 1);
  }
  if (stmt->kind == NY_S_TRY)
    return stmt->as.tr.body && stmt->as.tr.handler &&
           stmt_ownership_rules_returns_borrow_mut_param(
               stmt->as.tr.body, param_name, depth + 1) &&
           stmt_ownership_rules_returns_borrow_mut_param(stmt->as.tr.handler,
                                                         param_name, depth + 1);
  if (stmt->kind == NY_S_IF)
    return !stmt->as.iff.init &&
           stmt_ownership_rules_return_condition_is_safe(stmt->as.iff.test,
                                                         0) &&
           stmt_ownership_rules_returns_borrow_mut_param(
               stmt->as.iff.conseq, param_name, depth + 1) &&
           stmt_ownership_rules_returns_borrow_mut_param(stmt->as.iff.alt,
                                                         param_name, depth + 1);
  if (stmt->kind == NY_S_MATCH) {
    const stmt_match_t *match = &stmt->as.match;
    if (!stmt_ownership_rules_return_condition_is_safe(match->test, 0) ||
        match->arms.len == 0 || match->arms.len > 16 ||
        (!match->default_conseq &&
         !stmt_ownership_match_has_unguarded_wildcard(&match->arms)))
      return false;
    for (size_t i = 0; i < match->arms.len; ++i) {
      const match_arm_t *arm = &match->arms.data[i];
      if (!arm->conseq || arm->patterns.len == 0 ||
          (arm->guard &&
           !stmt_ownership_rules_return_condition_is_safe(arm->guard, 0)))
        return false;
      for (size_t j = 0; j < arm->patterns.len; ++j)
        if (!stmt_ownership_rules_return_condition_is_safe(
                arm->patterns.data[j], 0))
          return false;
      if (!stmt_ownership_rules_returns_borrow_mut_param(arm->conseq,
                                                         param_name, depth + 1))
        return false;
    }
    return !match->default_conseq ||
           stmt_ownership_rules_returns_borrow_mut_param(match->default_conseq,
                                                         param_name, depth + 1);
  }
  return false;
}

static bool stmt_ownership_rules_is_exclusive_borrow_expr(const expr_t *e) {
  if (!stmt_ownership_rules_is_borrow_expr(e) || !e->as.call.callee)
    return false;
  if (e->as.call.callee->kind == NY_E_IDENT &&
      stmt_ownership_rules_name_is(e->as.call.callee->as.ident.name,
                                   "borrow_mut"))
    return true;
  const stmt_t *function = e->semantic.canonical_callee_stmt;
  const char *source_param =
      stmt_ownership_rules_returns_borrow_param(function);
  const stmt_t *body =
      function && function->kind == NY_S_FUNC ? function->as.fn.body : NULL;
  if (source_param &&
      stmt_ownership_rules_returns_borrow_mut_param(body, source_param, 0))
    return true;
  const stmt_t *ret = body;
  const expr_t *borrow_value = NULL;
  if (body && body->kind == NY_S_BLOCK) {
    if (body->as.block.body.len == 1) {
      ret = body->as.block.body.data[0];
    } else if (body->as.block.body.len == 2) {
      const stmt_t *alias = body->as.block.body.data[0];
      ret = body->as.block.body.data[1];
      if (!alias || alias->kind != NY_S_VAR || !alias->as.var.is_decl ||
          alias->as.var.names.len != 1 || alias->as.var.exprs.len != 1 ||
          !alias->as.var.names.data[0] || !alias->as.var.exprs.data[0] ||
          !ret || ret->kind != NY_S_RETURN || !ret->as.ret.value ||
          ret->as.ret.value->kind != NY_E_IDENT ||
          !ret->as.ret.value->as.ident.name ||
          strcmp(ret->as.ret.value->as.ident.name,
                 alias->as.var.names.data[0]) != 0)
        return false;
      borrow_value = alias->as.var.exprs.data[0];
    } else {
      return false;
    }
  }
  if (!source_param || !ret || ret->kind != NY_S_RETURN || !ret->as.ret.value)
    return false;
  if (!borrow_value) {
    if (ret->as.ret.value->kind != NY_E_CALL)
      return false;
    borrow_value = ret->as.ret.value;
  }
  if (borrow_value->kind != NY_E_CALL || !borrow_value->as.call.callee ||
      borrow_value->as.call.callee->kind != NY_E_IDENT ||
      !stmt_ownership_rules_name_is(borrow_value->as.call.callee->as.ident.name,
                                    "borrow_mut") ||
      borrow_value->as.call.args.len != 1 ||
      !borrow_value->as.call.args.data[0].val ||
      borrow_value->as.call.args.data[0].val->kind != NY_E_IDENT ||
      !borrow_value->as.call.args.data[0].val->as.ident.name)
    return false;
  return strcmp(borrow_value->as.call.args.data[0].val->as.ident.name,
                source_param) == 0;
}

static bool
stmt_ownership_rules_shared_borrow_of_param(const expr_t *value,
                                            const char *param_name) {
  const expr_t *source = stmt_ownership_rules_borrow_source(value);
  return source && source->kind == NY_E_IDENT && source->as.ident.name &&
         strcmp(source->as.ident.name, param_name) == 0 &&
         !stmt_ownership_rules_is_exclusive_borrow_expr(value);
}

static bool stmt_ownership_rules_shared_borrow_reassignments(
    const stmt_t *stmt, const char *alias, const char *param_name,
    unsigned depth) {
  if (!stmt || depth > 64)
    return false;
  if (stmt->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < stmt->as.block.body.len; ++i)
      if (!stmt_ownership_rules_shared_borrow_reassignments(
              stmt->as.block.body.data[i], alias, param_name, depth + 1))
        return false;
    return true;
  }
  if (stmt->kind == NY_S_IF)
    return !stmt->as.iff.init && stmt->as.iff.alt &&
           stmt_ownership_rules_return_condition_is_safe(stmt->as.iff.test,
                                                         0) &&
           stmt_ownership_rules_shared_borrow_reassignments(
               stmt->as.iff.conseq, alias, param_name, depth + 1) &&
           stmt_ownership_rules_shared_borrow_reassignments(
               stmt->as.iff.alt, alias, param_name, depth + 1);
  if (stmt->kind == NY_S_MATCH) {
    const stmt_match_t *match = &stmt->as.match;
    if (!stmt_ownership_rules_return_condition_is_safe(match->test, 0) ||
        !match->arms.len ||
        (!match->default_conseq &&
         !stmt_ownership_match_has_unguarded_wildcard(&match->arms)))
      return false;
    for (size_t i = 0; i < match->arms.len; ++i) {
      const match_arm_t *arm = &match->arms.data[i];
      if (!arm->conseq || !arm->patterns.len ||
          (arm->guard &&
           !stmt_ownership_rules_return_condition_is_safe(arm->guard, 0)))
        return false;
      for (size_t j = 0; j < arm->patterns.len; ++j)
        if (!stmt_ownership_rules_return_condition_is_safe(
                arm->patterns.data[j], 0))
          return false;
      if (!stmt_ownership_rules_shared_borrow_reassignments(
              arm->conseq, alias, param_name, depth + 1))
        return false;
    }
    return !match->default_conseq ||
           stmt_ownership_rules_shared_borrow_reassignments(
               match->default_conseq, alias, param_name, depth + 1);
  }
  if (stmt->kind != NY_S_VAR || stmt->as.var.is_decl ||
      stmt->as.var.names.len != 1 || stmt->as.var.exprs.len != 1 ||
      !stmt->as.var.names.data[0] ||
      strcmp(stmt->as.var.names.data[0], alias) != 0)
    return false;
  return stmt_ownership_rules_shared_borrow_of_param(stmt->as.var.exprs.data[0],
                                                     param_name);
}

static bool
stmt_ownership_rules_returns_shared_borrow_param(const stmt_t *function,
                                                 const char *param_name) {
  if (!function || function->kind != NY_S_FUNC || !param_name ||
      !function->as.fn.body || function->as.fn.body->kind != NY_S_BLOCK)
    return false;
  const stmt_t *body = function->as.fn.body;
  if (body->as.block.body.len < 2 || body->as.block.body.len > 16)
    return false;
  const stmt_t *binding = body->as.block.body.data[0];
  const stmt_t *ret = body->as.block.body.data[body->as.block.body.len - 1];
  if (!binding || binding->kind != NY_S_VAR || !binding->as.var.is_decl ||
      binding->as.var.names.len != 1 || binding->as.var.exprs.len != 1 ||
      !binding->as.var.names.data[0] || !binding->as.var.exprs.data[0] ||
      !ret || ret->kind != NY_S_RETURN || !ret->as.ret.value ||
      ret->as.ret.value->kind != NY_E_IDENT ||
      !ret->as.ret.value->as.ident.name ||
      strcmp(ret->as.ret.value->as.ident.name, binding->as.var.names.data[0]) !=
          0 ||
      !stmt_ownership_rules_shared_borrow_of_param(
          binding->as.var.exprs.data[0], param_name))
    return false;
  for (size_t i = 1; i + 1 < body->as.block.body.len; ++i)
    if (!stmt_ownership_rules_shared_borrow_reassignments(
            body->as.block.body.data[i], binding->as.var.names.data[0],
            param_name, 0))
      return false;
  return true;
}

static bool stmt_ownership_rules_stmt_writes_name(const stmt_t *s,
                                                  const char *name,
                                                  unsigned depth);

static bool stmt_ownership_rules_expr_writes_name(const expr_t *e,
                                                  const char *name,
                                                  unsigned depth) {
  if (!e || !name)
    return false;
  if (depth > 128)
    return true;
  if (stmt_ownership_rules_is_exclusive_borrow_expr(e) &&
      stmt_ownership_rules_expr_mentions_name(
          stmt_ownership_rules_borrow_source(e), name, depth + 1))
    return true;
  switch (e->kind) {
  case NY_E_BINARY:
    if (e->as.binary.op && strcmp(e->as.binary.op, "=") == 0 &&
        stmt_ownership_rules_expr_mentions_name(e->as.binary.left, name,
                                                depth + 1))
      return true;
    return stmt_ownership_rules_expr_writes_name(e->as.binary.left, name,
                                                 depth + 1) ||
           stmt_ownership_rules_expr_writes_name(e->as.binary.right, name,
                                                 depth + 1);
  case NY_E_UNARY:
    return stmt_ownership_rules_expr_writes_name(e->as.unary.right, name,
                                                 depth + 1);
  case NY_E_LOGICAL:
    return stmt_ownership_rules_expr_writes_name(e->as.logical.left, name,
                                                 depth + 1) ||
           stmt_ownership_rules_expr_writes_name(e->as.logical.right, name,
                                                 depth + 1);
  case NY_E_TERNARY:
    return stmt_ownership_rules_expr_writes_name(e->as.ternary.cond, name,
                                                 depth + 1) ||
           stmt_ownership_rules_expr_writes_name(e->as.ternary.true_expr, name,
                                                 depth + 1) ||
           stmt_ownership_rules_expr_writes_name(e->as.ternary.false_expr, name,
                                                 depth + 1);
  case NY_E_CALL:
    if (stmt_ownership_rules_expr_writes_name(e->as.call.callee, name,
                                              depth + 1))
      return true;
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      if (stmt_ownership_rules_expr_writes_name(e->as.call.args.data[i].val,
                                                name, depth + 1))
        return true;
    return false;
  case NY_E_MEMCALL:
    if (stmt_expr_is_mutating_name(e->as.memcall.name) &&
        stmt_ownership_rules_expr_mentions_name(e->as.memcall.target, name,
                                                depth + 1))
      return true;
    if (stmt_ownership_rules_expr_writes_name(e->as.memcall.target, name,
                                              depth + 1))
      return true;
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      if (stmt_ownership_rules_expr_writes_name(e->as.memcall.args.data[i].val,
                                                name, depth + 1))
        return true;
    return false;
  case NY_E_INDEX:
    return stmt_ownership_rules_expr_writes_name(e->as.index.target, name,
                                                 depth + 1) ||
           stmt_ownership_rules_expr_writes_name(e->as.index.start, name,
                                                 depth + 1) ||
           stmt_ownership_rules_expr_writes_name(e->as.index.stop, name,
                                                 depth + 1) ||
           stmt_ownership_rules_expr_writes_name(e->as.index.step, name,
                                                 depth + 1);
  case NY_E_MEMBER:
    return stmt_ownership_rules_expr_writes_name(e->as.member.target, name,
                                                 depth + 1);
  case NY_E_DEREF:
    return stmt_ownership_rules_expr_writes_name(e->as.deref.target, name,
                                                 depth + 1);
  case NY_E_TRY:
    return stmt_ownership_rules_expr_writes_name(e->as.try_expr.target, name,
                                                 depth + 1);
  case NY_E_LIST:
  case NY_E_TUPLE:
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      if (stmt_ownership_rules_expr_writes_name(e->as.list_like.data[i], name,
                                                depth + 1))
        return true;
    return false;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i)
      if (stmt_ownership_rules_expr_writes_name(e->as.dict.pairs.data[i].key,
                                                name, depth + 1) ||
          stmt_ownership_rules_expr_writes_name(e->as.dict.pairs.data[i].value,
                                                name, depth + 1))
        return true;
    return false;
  case NY_E_LAMBDA:
  case NY_E_FN:
    return stmt_ownership_rules_stmt_writes_name(e->as.lambda.body, name,
                                                 depth + 1);
  default:
    return false;
  }
}

static bool stmt_ownership_rules_stmt_writes_name(const stmt_t *s,
                                                  const char *name,
                                                  unsigned depth) {
  if (!s || !name)
    return false;
  if (depth > 128)
    return true;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      if (stmt_ownership_rules_stmt_writes_name(s->as.block.body.data[i], name,
                                                depth + 1))
        return true;
    return false;
  case NY_S_VAR:
    if (!s->as.var.is_decl) {
      for (size_t i = 0; i < s->as.var.names.len; ++i)
        if (s->as.var.names.data[i] &&
            strcmp(s->as.var.names.data[i], name) == 0)
          return true;
    }
    for (size_t i = 0; i < s->as.var.exprs.len; ++i)
      if (stmt_ownership_rules_expr_writes_name(s->as.var.exprs.data[i], name,
                                                depth + 1))
        return true;
    return false;
  case NY_S_EXPR:
    return stmt_ownership_rules_expr_writes_name(s->as.expr.expr, name,
                                                 depth + 1);
  case NY_S_RETURN:
    return stmt_ownership_rules_expr_writes_name(s->as.ret.value, name,
                                                 depth + 1);
  case NY_S_IF:
    return stmt_ownership_rules_expr_writes_name(s->as.iff.test, name,
                                                 depth + 1) ||
           stmt_ownership_rules_stmt_writes_name(s->as.iff.conseq, name,
                                                 depth + 1) ||
           stmt_ownership_rules_stmt_writes_name(s->as.iff.alt, name,
                                                 depth + 1);
  case NY_S_WHILE:
    return stmt_ownership_rules_expr_writes_name(s->as.whl.test, name,
                                                 depth + 1) ||
           stmt_ownership_rules_stmt_writes_name(s->as.whl.body, name,
                                                 depth + 1);
  case NY_S_FOR:
    return stmt_ownership_rules_stmt_writes_name(s->as.fr.init, name,
                                                 depth + 1) ||
           stmt_ownership_rules_expr_writes_name(s->as.fr.iterable, name,
                                                 depth + 1) ||
           stmt_ownership_rules_stmt_writes_name(s->as.fr.body, name,
                                                 depth + 1) ||
           stmt_ownership_rules_stmt_writes_name(s->as.fr.update, name,
                                                 depth + 1);
  case NY_S_TRY:
    return stmt_ownership_rules_stmt_writes_name(s->as.tr.body, name,
                                                 depth + 1) ||
           stmt_ownership_rules_stmt_writes_name(s->as.tr.handler, name,
                                                 depth + 1);
  case NY_S_DEFER:
    return stmt_ownership_rules_stmt_writes_name(s->as.de.body, name,
                                                 depth + 1);
  default:
    return false;
  }
}

static bool stmt_ownership_rules_path_conflict(const stmt_t *s,
                                               const char *alias,
                                               const char *source,
                                               bool exclusive, bool live_after,
                                               unsigned depth) {
  if (!s)
    return false;
  if (depth > 128)
    return true;
  if (s->kind == NY_S_IF) {
    const stmt_t *then_stmt = s->as.iff.conseq;
    const stmt_t *else_stmt = s->as.iff.alt;
    bool then_uses =
        stmt_ownership_rules_stmt_mentions_name(then_stmt, alias, depth + 1);
    bool else_uses =
        stmt_ownership_rules_stmt_mentions_name(else_stmt, alias, depth + 1);
    bool test_uses = stmt_ownership_rules_expr_mentions_name(s->as.iff.test,
                                                             alias, depth + 1);
    bool live_at_test = live_after || test_uses || then_uses || else_uses;
    if (s->as.iff.init &&
        stmt_ownership_rules_path_conflict(s->as.iff.init, alias, source,
                                           exclusive, live_at_test, depth + 1))
      return true;
    bool test_conflict = exclusive ? stmt_ownership_resource_path_mentions_expr(
                                         s->as.iff.test, source, depth + 1)
                                   : stmt_ownership_rules_expr_writes_name(
                                         s->as.iff.test, source, depth + 1);
    if (live_at_test && test_conflict)
      return true;
    return ((live_after || then_uses) &&
            stmt_ownership_rules_path_conflict(
                then_stmt, alias, source, exclusive, live_after, depth + 1)) ||
           ((live_after || else_uses) &&
            stmt_ownership_rules_path_conflict(
                else_stmt, alias, source, exclusive, live_after, depth + 1));
  }
  if (s->kind == NY_S_MATCH) {
    const expr_t *test = s->as.match.test;
    bool any_arm_uses = false;
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      const match_arm_t *arm = &s->as.match.arms.data[i];
      any_arm_uses |= stmt_ownership_rules_expr_mentions_name(arm->guard, alias,
                                                              depth + 1) ||
                      stmt_ownership_rules_stmt_mentions_name(arm->conseq,
                                                              alias, depth + 1);
    }
    bool default_uses = stmt_ownership_rules_stmt_mentions_name(
        s->as.match.default_conseq, alias, depth + 1);
    any_arm_uses |= default_uses;
    bool test_uses_alias =
        stmt_ownership_rules_expr_mentions_name(test, alias, depth + 1);
    bool live_at_test = live_after || test_uses_alias || any_arm_uses;
    bool test_conflict =
        exclusive
            ? stmt_ownership_resource_path_mentions_expr(test, source,
                                                         depth + 1)
            : stmt_ownership_rules_expr_writes_name(test, source, depth + 1);
    if (live_at_test && test_conflict)
      return true;

    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      const match_arm_t *arm = &s->as.match.arms.data[i];
      bool arm_uses_alias = stmt_ownership_rules_expr_mentions_name(
                                arm->guard, alias, depth + 1) ||
                            stmt_ownership_rules_stmt_mentions_name(
                                arm->conseq, alias, depth + 1);
      bool later_uses_alias = false;
      for (size_t j = i + 1; j < s->as.match.arms.len; ++j) {
        const match_arm_t *later = &s->as.match.arms.data[j];
        later_uses_alias |= stmt_ownership_rules_expr_mentions_name(
                                later->guard, alias, depth + 1) ||
                            stmt_ownership_rules_stmt_mentions_name(
                                later->conseq, alias, depth + 1);
      }
      later_uses_alias |= default_uses;
      if ((live_after || arm_uses_alias || later_uses_alias) && arm->guard) {
        bool guard_conflict = exclusive
                                  ? stmt_ownership_resource_path_mentions_expr(
                                        arm->guard, source, depth + 1)
                                  : stmt_ownership_rules_expr_writes_name(
                                        arm->guard, source, depth + 1);
        if (guard_conflict)
          return true;
      }
      if ((live_after || arm_uses_alias) &&
          stmt_ownership_rules_path_conflict(arm->conseq, alias, source,
                                             exclusive, live_after, depth + 1))
        return true;
    }
    if (s->as.match.default_conseq && (live_after || default_uses) &&
        stmt_ownership_rules_path_conflict(s->as.match.default_conseq, alias,
                                           source, exclusive, live_after,
                                           depth + 1))
      return true;
    return false;
  }
  if (s->kind == NY_S_WHILE) {
    bool test_uses = stmt_ownership_rules_expr_mentions_name(s->as.whl.test,
                                                             alias, depth + 1);
    bool body_uses = stmt_ownership_rules_stmt_mentions_name(s->as.whl.body,
                                                             alias, depth + 1);
    bool update_uses = stmt_ownership_rules_stmt_mentions_name(
        s->as.whl.update, alias, depth + 1);
    /*
     * A loop use can be reached again through the backedge. Keep the borrow
     * live through the test, body, and update whenever a later iteration or
     * statement may use it.
     */
    bool loop_live = live_after || test_uses || body_uses || update_uses;
    bool test_conflict = exclusive ? stmt_ownership_resource_path_mentions_expr(
                                         s->as.whl.test, source, depth + 1)
                                   : stmt_ownership_rules_expr_writes_name(
                                         s->as.whl.test, source, depth + 1);
    if (loop_live && test_conflict)
      return true;
    if (s->as.whl.init &&
        stmt_ownership_rules_path_conflict(s->as.whl.init, alias, source,
                                           exclusive, loop_live, depth + 1))
      return true;
    if (loop_live &&
        stmt_ownership_rules_path_conflict(
            s->as.whl.body, alias, source, exclusive,
            live_after || body_uses || update_uses || test_uses, depth + 1))
      return true;
    return loop_live &&
           stmt_ownership_rules_path_conflict(
               s->as.whl.update, alias, source, exclusive,
               live_after || test_uses || body_uses || update_uses, depth + 1);
  }
  if (s->kind == NY_S_FOR) {
    bool iterable_uses = stmt_ownership_rules_expr_mentions_name(
        s->as.fr.iterable, alias, depth + 1);
    bool cond_uses = stmt_ownership_rules_expr_mentions_name(s->as.fr.cond,
                                                             alias, depth + 1);
    bool body_uses = stmt_ownership_rules_stmt_mentions_name(s->as.fr.body,
                                                             alias, depth + 1);
    bool update_uses = stmt_ownership_rules_stmt_mentions_name(
        s->as.fr.update, alias, depth + 1);
    bool loop_live = live_after || cond_uses || body_uses || update_uses;
    bool iterable_conflict =
        exclusive ? stmt_ownership_resource_path_mentions_expr(
                        s->as.fr.iterable, source, depth + 1)
                  : stmt_ownership_rules_expr_writes_name(s->as.fr.iterable,
                                                          source, depth + 1);
    bool cond_conflict = exclusive ? stmt_ownership_resource_path_mentions_expr(
                                         s->as.fr.cond, source, depth + 1)
                                   : stmt_ownership_rules_expr_writes_name(
                                         s->as.fr.cond, source, depth + 1);
    if (((loop_live || iterable_uses) && iterable_conflict) ||
        (loop_live && cond_conflict))
      return true;
    if (s->as.fr.init && stmt_ownership_rules_path_conflict(
                             s->as.fr.init, alias, source, exclusive,
                             loop_live || iterable_uses, depth + 1))
      return true;
    bool body_live = loop_live;
    if (loop_live &&
        stmt_ownership_rules_path_conflict(s->as.fr.body, alias, source,
                                           exclusive, body_live, depth + 1))
      return true;
    return loop_live &&
           stmt_ownership_rules_path_conflict(
               s->as.fr.update, alias, source, exclusive,
               live_after || cond_uses || body_uses || update_uses, depth + 1);
  }
  if (s->kind == NY_S_TRY) {
    bool body_uses_alias = stmt_ownership_rules_stmt_mentions_name(
        s->as.tr.body, alias, depth + 1);
    bool handler_uses_alias = stmt_ownership_rules_stmt_mentions_name(
        s->as.tr.handler, alias, depth + 1);
    /*
     * The handler is reachable only after the body raises. If it uses the
     * alias, keep that borrow live throughout the body because any earlier
     * operation can transfer control to the handler. Otherwise analyze each
     * block's lexical last use independently.
     */
    bool body_live_after = live_after || handler_uses_alias;
    bool handler_live_after = live_after;
    return ((body_live_after || body_uses_alias) &&
            stmt_ownership_rules_path_conflict(s->as.tr.body, alias, source,
                                               exclusive, body_live_after,
                                               depth + 1)) ||
           ((handler_live_after || handler_uses_alias) &&
            stmt_ownership_rules_path_conflict(s->as.tr.handler, alias, source,
                                               exclusive, handler_live_after,
                                               depth + 1));
  }
  if (s->kind == NY_S_BLOCK) {
    const char *live_aliases[64];
    size_t live_alias_count = 1;
    live_aliases[0] = alias;
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      const stmt_t *item = s->as.block.body.data[i];
      bool later_use = live_after;
      for (size_t j = i + 1; !later_use && j < s->as.block.body.len; ++j) {
        const stmt_t *later = s->as.block.body.data[j];
        for (size_t a = 0; !later_use && a < live_alias_count; ++a)
          later_use = stmt_ownership_rules_stmt_mentions_name(
              later, live_aliases[a], depth + 1);
      }
      if (stmt_ownership_rules_path_conflict(item, alias, source, exclusive,
                                             later_use, depth + 1))
        return true;
      if (item && item->kind == NY_S_VAR && item->as.var.is_decl) {
        for (size_t k = 0;
             k < item->as.var.names.len && k < item->as.var.exprs.len; ++k) {
          const expr_t *value = item->as.var.exprs.data[k];
          if (!value || value->kind != NY_E_IDENT ||
              !item->as.var.names.data[k])
            continue;
          bool copies_live_borrow = false;
          for (size_t a = 0; a < live_alias_count; ++a)
            if (strcmp(value->as.ident.name, live_aliases[a]) == 0) {
              copies_live_borrow = true;
              break;
            }
          if (!copies_live_borrow)
            continue;
          if (live_alias_count ==
              sizeof(live_aliases) / sizeof(live_aliases[0]))
            return true;
          live_aliases[live_alias_count++] = item->as.var.names.data[k];
        }
      }
    }
    return false;
  }
  if (!live_after &&
      !stmt_ownership_rules_stmt_mentions_name(s, alias, depth + 1))
    return false;
  return exclusive
             ? stmt_ownership_resource_path_mentions_stmt(s, source, depth + 1)
             : stmt_ownership_rules_stmt_writes_name(s, source, depth + 1);
}

/*
 * Check lexical exclusivity for direct local aliases. Bindings end at their
 * last use in the containing block. If that use is inside an if arm, visit the
 * arms independently; other nested control flow remains conservative.
 */
static void stmt_ownership_rules_check_exclusive_borrow_block(
    codegen_t *cg, const stmt_t *block, unsigned depth) {
  if (!block)
    return;
  if (depth > 128) {
    stmt_ownership_rules_error(
        cg, block->tok, "B3",
        "exclusive-borrow analysis exceeded its nesting limit");
    return;
  }
  if (block->kind != NY_S_BLOCK) {
    switch (block->kind) {
    case NY_S_IF:
      stmt_ownership_rules_check_exclusive_borrow_block(
          cg, block->as.iff.conseq, depth + 1);
      stmt_ownership_rules_check_exclusive_borrow_block(cg, block->as.iff.alt,
                                                        depth + 1);
      break;
    case NY_S_WHILE:
      stmt_ownership_rules_check_exclusive_borrow_block(cg, block->as.whl.body,
                                                        depth + 1);
      break;
    case NY_S_FOR:
      stmt_ownership_rules_check_exclusive_borrow_block(cg, block->as.fr.body,
                                                        depth + 1);
      break;
    case NY_S_TRY:
      stmt_ownership_rules_check_exclusive_borrow_block(cg, block->as.tr.body,
                                                        depth + 1);
      stmt_ownership_rules_check_exclusive_borrow_block(
          cg, block->as.tr.handler, depth + 1);
      break;
    case NY_S_MATCH:
      for (size_t i = 0; i < block->as.match.arms.len; ++i)
        stmt_ownership_rules_check_exclusive_borrow_block(
            cg, block->as.match.arms.data[i].conseq, depth + 1);
      stmt_ownership_rules_check_exclusive_borrow_block(
          cg, block->as.match.default_conseq, depth + 1);
      break;
    default:
      break;
    }
    return;
  }

  for (size_t i = 0; i < block->as.block.body.len; ++i) {
    const stmt_t *decl = block->as.block.body.data[i];
    if (!decl || decl->kind != NY_S_VAR)
      continue;
    for (size_t binding_index = 0; binding_index < decl->as.var.names.len;
         ++binding_index) {
      expr_t *init = binding_index < decl->as.var.exprs.len
                         ? decl->as.var.exprs.data[binding_index]
                         : NULL;
      if (!stmt_ownership_rules_is_borrow_expr(init))
        continue;
      bool exclusive = stmt_ownership_rules_is_exclusive_borrow_expr(init);
      const stmt_t *callee =
          init->kind == NY_E_CALL ? init->semantic.canonical_callee_stmt : NULL;
      const char *borrow_param =
          stmt_ownership_rules_returns_borrow_param(callee);
      bool direct_borrow_builtin =
          init->as.call.callee && init->as.call.callee->kind == NY_E_IDENT &&
          (stmt_ownership_rules_name_is(init->as.call.callee->as.ident.name,
                                        "borrow") ||
           stmt_ownership_rules_name_is(init->as.call.callee->as.ident.name,
                                        "borrow_mut"));
      bool unverified_returned_borrow =
          !exclusive && callee && callee->kind == NY_S_FUNC && borrow_param &&
          !direct_borrow_builtin &&
          !stmt_ownership_rules_returns_shared_borrow_param(callee,
                                                            borrow_param);
      bool conflict_as_exclusive = exclusive || unverified_returned_borrow;
      const expr_t *source_expr = stmt_ownership_rules_borrow_source(init);
      char source_place[512];
      size_t source_place_len = 0;
      const char *source =
          unverified_returned_borrow ? "\001unknown-exclusive-place"
          : conflict_as_exclusive &&
                  stmt_ownership_exclusive_place_text(source_expr, source_place,
                                                      sizeof(source_place),
                                                      &source_place_len)
              ? source_place
              : stmt_ownership_rules_place_root_name(source_expr, 0);
      if (!source && conflict_as_exclusive)
        source = "\001unknown-exclusive-place";
      const char *alias = decl->as.var.names.data[binding_index];
      if (!source || !alias || strcmp(source, alias) == 0)
        continue;

      bool conflict_found = false;
      const char *borrow_aliases[64];
      size_t alias_starts[64];
      size_t alias_count = 1;
      borrow_aliases[0] = alias;
      alias_starts[0] = i;
      /*
       * Follow direct local copies of a borrow so its liveness moves with it.
       */
      for (size_t j = i + 1; j < block->as.block.body.len; ++j) {
        const stmt_t *copy = block->as.block.body.data[j];
        if (!copy || copy->kind != NY_S_VAR || !copy->as.var.is_decl)
          continue;
        for (size_t k = 0;
             k < copy->as.var.names.len && k < copy->as.var.exprs.len; ++k) {
          expr_t *value = copy->as.var.exprs.data[k];
          if (!value || value->kind != NY_E_IDENT ||
              !copy->as.var.names.data[k])
            continue;
          bool copies_borrow = false;
          for (size_t a = 0; a < alias_count; ++a)
            if (strcmp(value->as.ident.name, borrow_aliases[a]) == 0) {
              copies_borrow = true;
              break;
            }
          if (!copies_borrow)
            continue;
          if (alias_count ==
              sizeof(borrow_aliases) / sizeof(borrow_aliases[0])) {
            stmt_ownership_rules_error(
                cg, copy->tok, "B3",
                "borrow alias chain exceeds the ownership analysis limit");
            conflict_found = true;
            break;
          }
          borrow_aliases[alias_count] = copy->as.var.names.data[k];
          alias_starts[alias_count++] = j;
        }
        if (conflict_found)
          break;
      }
      for (size_t a = 0; a < alias_count && !conflict_found; ++a) {
        size_t last_use = alias_starts[a];
        for (size_t j = alias_starts[a] + 1; j < block->as.block.body.len; ++j)
          if (stmt_ownership_rules_stmt_mentions_name(
                  block->as.block.body.data[j], borrow_aliases[a], depth + 1))
            last_use = j;
        for (size_t j = alias_starts[a] + 1; j <= last_use; ++j) {
          const stmt_t *candidate = block->as.block.body.data[j];
          bool conflict = stmt_ownership_rules_path_conflict(
              candidate, borrow_aliases[a], source, conflict_as_exclusive,
              j < last_use, depth + 1);
          if (!conflict)
            continue;
          conflict_found = true;
          if (!exclusive && candidate->kind == NY_S_VAR &&
              !candidate->as.var.is_decl) {
            bool reassigns_source = false;
            for (size_t k = 0; k < candidate->as.var.names.len; ++k)
              if (candidate->as.var.names.data[k] &&
                  strcmp(candidate->as.var.names.data[k], source) == 0) {
                reassigns_source = true;
                break;
              }
            if (reassigns_source) {
              char message[256];
              snprintf(message, sizeof(message),
                       "cannot reassign owned slot '%s' while borrow '%s' is "
                       "live",
                       source, borrow_aliases[a]);
              stmt_ownership_rules_error(cg, candidate->tok, "B3", message);
              break;
            }
          }
          stmt_ownership_rules_error(cg, candidate->tok, "B3",
                                     conflict_as_exclusive
                                         ? "cannot access a place while an "
                                           "exclusive or unverified borrow "
                                           "is live"
                                         : "cannot mutate a place while a "
                                           "shared borrow of it is live");
          break;
        }
      }
      if (exclusive && !conflict_found)
        init->semantic.exclusive_borrow_proven = true;
    }
  }

  for (size_t i = 0; i < block->as.block.body.len; ++i)
    stmt_ownership_rules_check_exclusive_borrow_block(
        cg, block->as.block.body.data[i], depth + 1);
}

static void stmt_ownership_rules_check_lambda_borrow_capture(
    codegen_t *cg, const expr_t *e,
    const stmt_ownership_rules_borrow_bindings_t *bindings, unsigned depth);

static void stmt_ownership_rules_check_lambda_borrow_capture_stmt(
    codegen_t *cg, const stmt_t *s,
    const stmt_ownership_rules_borrow_bindings_t *bindings, unsigned depth) {
  if (!s)
    return;
  if (depth > 128) {
    for (size_t i = 0; bindings && i < bindings->len; ++i) {
      if (!bindings->data[i].contains_borrow)
        continue;
      stmt_ownership_rules_error(
          cg, s->tok, "B1",
          "closure-capture analysis exceeded its nesting limit while a "
          "borrow was in scope");
      break;
    }
    return;
  }
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      stmt_ownership_rules_check_lambda_borrow_capture_stmt(
          cg, s->as.block.body.data[i], bindings, depth + 1);
    break;
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; ++i)
      stmt_ownership_rules_check_lambda_borrow_capture(
          cg, s->as.var.exprs.data[i], bindings, depth + 1);
    break;
  case NY_S_EXPR:
    stmt_ownership_rules_check_lambda_borrow_capture(cg, s->as.expr.expr,
                                                     bindings, depth + 1);
    break;
  case NY_S_IF:
    stmt_ownership_rules_check_lambda_borrow_capture(cg, s->as.iff.test,
                                                     bindings, depth + 1);
    stmt_ownership_rules_check_lambda_borrow_capture_stmt(cg, s->as.iff.init,
                                                          bindings, depth + 1);
    stmt_ownership_rules_check_lambda_borrow_capture_stmt(cg, s->as.iff.conseq,
                                                          bindings, depth + 1);
    stmt_ownership_rules_check_lambda_borrow_capture_stmt(cg, s->as.iff.alt,
                                                          bindings, depth + 1);
    break;
  case NY_S_GUARD:
    stmt_ownership_rules_check_lambda_borrow_capture(cg, s->as.guard.value,
                                                     bindings, depth + 1);
    stmt_ownership_rules_check_lambda_borrow_capture_stmt(
        cg, s->as.guard.fallback, bindings, depth + 1);
    break;
  case NY_S_WHILE:
    stmt_ownership_rules_check_lambda_borrow_capture(cg, s->as.whl.test,
                                                     bindings, depth + 1);
    stmt_ownership_rules_check_lambda_borrow_capture(cg, s->as.whl.invariant,
                                                     bindings, depth + 1);
    stmt_ownership_rules_check_lambda_borrow_capture_stmt(cg, s->as.whl.init,
                                                          bindings, depth + 1);
    stmt_ownership_rules_check_lambda_borrow_capture_stmt(cg, s->as.whl.body,
                                                          bindings, depth + 1);
    stmt_ownership_rules_check_lambda_borrow_capture_stmt(cg, s->as.whl.update,
                                                          bindings, depth + 1);
    break;
  case NY_S_FOR:
    stmt_ownership_rules_check_lambda_borrow_capture(cg, s->as.fr.iterable,
                                                     bindings, depth + 1);
    stmt_ownership_rules_check_lambda_borrow_capture(cg, s->as.fr.cond,
                                                     bindings, depth + 1);
    stmt_ownership_rules_check_lambda_borrow_capture_stmt(cg, s->as.fr.init,
                                                          bindings, depth + 1);
    stmt_ownership_rules_check_lambda_borrow_capture_stmt(cg, s->as.fr.body,
                                                          bindings, depth + 1);
    stmt_ownership_rules_check_lambda_borrow_capture_stmt(cg, s->as.fr.update,
                                                          bindings, depth + 1);
    break;
  case NY_S_TRY:
    stmt_ownership_rules_check_lambda_borrow_capture_stmt(cg, s->as.tr.body,
                                                          bindings, depth + 1);
    stmt_ownership_rules_check_lambda_borrow_capture_stmt(cg, s->as.tr.handler,
                                                          bindings, depth + 1);
    break;
  case NY_S_FUNC:
    for (size_t i = 0; bindings && i < bindings->len; ++i) {
      const char *name = bindings->data[i].name;
      if (!stmt_ownership_rules_name_contains_borrow(bindings, name))
        continue;
      bool shadowed = false;
      for (size_t j = 0; j < s->as.fn.params.len; ++j)
        if (s->as.fn.params.data[j].name &&
            strcmp(s->as.fn.params.data[j].name, name) == 0) {
          shadowed = true;
          break;
        }
      if (!shadowed && stmt_ownership_rules_stmt_mentions_name(
                           s->as.fn.body, name, depth + 1)) {
        stmt_ownership_rules_error(
            cg, s->tok, "B1",
            "a second-class borrow cannot be captured by a closure");
        break;
      }
    }
    stmt_ownership_rules_check_lambda_borrow_capture_stmt(cg, s->as.fn.body,
                                                          bindings, depth + 1);
    break;
  case NY_S_RETURN:
    stmt_ownership_rules_check_lambda_borrow_capture(cg, s->as.ret.value,
                                                     bindings, depth + 1);
    break;
  case NY_S_DEFER:
    stmt_ownership_rules_check_lambda_borrow_capture_stmt(cg, s->as.de.body,
                                                          bindings, depth + 1);
    break;
  case NY_S_MATCH:
    stmt_ownership_rules_check_lambda_borrow_capture(cg, s->as.match.test,
                                                     bindings, depth + 1);
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      const match_arm_t *arm = &s->as.match.arms.data[i];
      for (size_t j = 0; j < arm->patterns.len; ++j)
        stmt_ownership_rules_check_lambda_borrow_capture(
            cg, arm->patterns.data[j], bindings, depth + 1);
      stmt_ownership_rules_check_lambda_borrow_capture(cg, arm->guard, bindings,
                                                       depth + 1);
      stmt_ownership_rules_check_lambda_borrow_capture_stmt(
          cg, arm->conseq, bindings, depth + 1);
    }
    stmt_ownership_rules_check_lambda_borrow_capture_stmt(
        cg, s->as.match.default_conseq, bindings, depth + 1);
    break;
  case NY_S_AUG:
    stmt_ownership_rules_check_lambda_borrow_capture(cg, s->as.aug.target,
                                                     bindings, depth + 1);
    stmt_ownership_rules_check_lambda_borrow_capture(cg, s->as.aug.rhs,
                                                     bindings, depth + 1);
    break;
  default:
    break;
  }
}

static void stmt_ownership_rules_check_lambda_borrow_capture(
    codegen_t *cg, const expr_t *e,
    const stmt_ownership_rules_borrow_bindings_t *bindings, unsigned depth) {
  if (!e)
    return;
  if (depth > 128) {
    for (size_t i = 0; bindings && i < bindings->len; ++i) {
      if (!bindings->data[i].contains_borrow)
        continue;
      stmt_ownership_rules_error(
          cg, e->tok, "B1",
          "closure-capture analysis exceeded its nesting limit while a "
          "borrow was in scope");
      break;
    }
    return;
  }
  if (e->kind == NY_E_LAMBDA || e->kind == NY_E_FN) {
    for (size_t i = 0; bindings && i < bindings->len; ++i) {
      const char *name = bindings->data[i].name;
      if (!stmt_ownership_rules_name_contains_borrow(bindings, name))
        continue;
      bool shadowed = false;
      for (size_t j = 0; j < e->as.lambda.params.len; ++j)
        if (e->as.lambda.params.data[j].name &&
            strcmp(e->as.lambda.params.data[j].name, name) == 0) {
          shadowed = true;
          break;
        }
      bool mentions = stmt_ownership_rules_stmt_mentions_name(e->as.lambda.body,
                                                              name, depth + 1);
      if (!shadowed && mentions) {
        stmt_ownership_rules_error(
            cg, e->tok, "B1",
            "a second-class borrow cannot be captured by a closure");
        break;
      }
    }
    return;
  }
  switch (e->kind) {
  case NY_E_UNARY:
    stmt_ownership_rules_check_lambda_borrow_capture(cg, e->as.unary.right,
                                                     bindings, depth + 1);
    break;
  case NY_E_BINARY:
  case NY_E_LOGICAL:
    stmt_ownership_rules_check_lambda_borrow_capture(cg, e->as.binary.left,
                                                     bindings, depth + 1);
    stmt_ownership_rules_check_lambda_borrow_capture(cg, e->as.binary.right,
                                                     bindings, depth + 1);
    break;
  case NY_E_TERNARY:
    stmt_ownership_rules_check_lambda_borrow_capture(cg, e->as.ternary.cond,
                                                     bindings, depth + 1);
    stmt_ownership_rules_check_lambda_borrow_capture(
        cg, e->as.ternary.true_expr, bindings, depth + 1);
    stmt_ownership_rules_check_lambda_borrow_capture(
        cg, e->as.ternary.false_expr, bindings, depth + 1);
    break;
  case NY_E_CALL:
    stmt_ownership_rules_check_lambda_borrow_capture(cg, e->as.call.callee,
                                                     bindings, depth + 1);
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      stmt_ownership_rules_check_lambda_borrow_capture(
          cg, e->as.call.args.data[i].val, bindings, depth + 1);
    break;
  case NY_E_MEMCALL:
    stmt_ownership_rules_check_lambda_borrow_capture(cg, e->as.memcall.target,
                                                     bindings, depth + 1);
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      stmt_ownership_rules_check_lambda_borrow_capture(
          cg, e->as.memcall.args.data[i].val, bindings, depth + 1);
    break;
  case NY_E_INDEX:
    stmt_ownership_rules_check_lambda_borrow_capture(cg, e->as.index.target,
                                                     bindings, depth + 1);
    stmt_ownership_rules_check_lambda_borrow_capture(cg, e->as.index.start,
                                                     bindings, depth + 1);
    stmt_ownership_rules_check_lambda_borrow_capture(cg, e->as.index.stop,
                                                     bindings, depth + 1);
    stmt_ownership_rules_check_lambda_borrow_capture(cg, e->as.index.step,
                                                     bindings, depth + 1);
    break;
  case NY_E_MEMBER:
    stmt_ownership_rules_check_lambda_borrow_capture(cg, e->as.member.target,
                                                     bindings, depth + 1);
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      stmt_ownership_rules_check_lambda_borrow_capture(
          cg, e->as.list_like.data[i], bindings, depth + 1);
    break;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      stmt_ownership_rules_check_lambda_borrow_capture(
          cg, e->as.dict.pairs.data[i].key, bindings, depth + 1);
      stmt_ownership_rules_check_lambda_borrow_capture(
          cg, e->as.dict.pairs.data[i].value, bindings, depth + 1);
    }
    break;
  case NY_E_ASM:
    for (size_t i = 0; i < e->as.as_asm.args.len; ++i)
      stmt_ownership_rules_check_lambda_borrow_capture(
          cg, e->as.as_asm.args.data[i], bindings, depth + 1);
    break;
  case NY_E_COMPTIME:
    stmt_ownership_rules_check_lambda_borrow_capture_stmt(
        cg, e->as.comptime_expr.body, bindings, depth + 1);
    break;
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; ++i)
      if (e->as.fstring.parts.data[i].kind == NY_FSP_EXPR)
        stmt_ownership_rules_check_lambda_borrow_capture(
            cg, e->as.fstring.parts.data[i].as.e, bindings, depth + 1);
    break;
  case NY_E_MATCH:
    stmt_ownership_rules_check_lambda_borrow_capture(cg, e->as.match.test,
                                                     bindings, depth + 1);
    for (size_t i = 0; i < e->as.match.arms.len; ++i) {
      const match_arm_t *arm = &e->as.match.arms.data[i];
      for (size_t j = 0; j < arm->patterns.len; ++j)
        stmt_ownership_rules_check_lambda_borrow_capture(
            cg, arm->patterns.data[j], bindings, depth + 1);
      stmt_ownership_rules_check_lambda_borrow_capture(cg, arm->guard, bindings,
                                                       depth + 1);
      stmt_ownership_rules_check_lambda_borrow_capture_stmt(
          cg, arm->conseq, bindings, depth + 1);
    }
    stmt_ownership_rules_check_lambda_borrow_capture_stmt(
        cg, e->as.match.default_conseq, bindings, depth + 1);
    break;
  case NY_E_PTR_TYPE:
    stmt_ownership_rules_check_lambda_borrow_capture(cg, e->as.ptr_type.target,
                                                     bindings, depth + 1);
    break;
  case NY_E_DEREF:
    stmt_ownership_rules_check_lambda_borrow_capture(cg, e->as.deref.target,
                                                     bindings, depth + 1);
    break;
  case NY_E_SIZEOF:
    stmt_ownership_rules_check_lambda_borrow_capture(cg, e->as.szof.target,
                                                     bindings, depth + 1);
    break;
  case NY_E_TRY:
    stmt_ownership_rules_check_lambda_borrow_capture(cg, e->as.try_expr.target,
                                                     bindings, depth + 1);
    break;
  case NY_E_QUOTE:
    stmt_ownership_rules_check_lambda_borrow_capture_stmt(cg, e->as.quote.body,
                                                          bindings, depth + 1);
    stmt_ownership_rules_check_lambda_borrow_capture(cg, e->as.quote.expr,
                                                     bindings, depth + 1);
    break;
  case NY_E_SPLICE:
    stmt_ownership_rules_check_lambda_borrow_capture(cg, e->as.splice.expr,
                                                     bindings, depth + 1);
    break;
  case NY_E_EXISTS:
    stmt_ownership_rules_check_lambda_borrow_capture(cg, e->as.exists.body,
                                                     bindings, depth + 1);
    break;
  default:
    break;
  }
}

static void stmt_ownership_rules_check_alias_returns(
    codegen_t *cg, const stmt_t *fn_stmt, const stmt_func_t *fn,
    const stmt_t *s, stmt_ownership_rules_borrow_bindings_t *bindings,
    unsigned depth) {
  if (!s)
    return;
  if (depth > 128) {
    if (bindings && bindings->len)
      stmt_ownership_rules_error(
          cg, s->tok, "B2",
          "cannot validate returned borrow sources because analysis exceeded "
          "its nesting limit");
    return;
  }
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      stmt_ownership_rules_check_alias_returns(
          cg, fn_stmt, fn, s->as.block.body.data[i], bindings, depth + 1);
    break;
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.names.len; ++i) {
      const char *name = s->as.var.names.data[i];
      const expr_t *value =
          i < s->as.var.exprs.len ? s->as.var.exprs.data[i] : NULL;
      const char *source =
          stmt_ownership_rules_borrow_expr_source(bindings, value);
      bool contains_borrow =
          stmt_ownership_rules_expr_contains_borrow(bindings, value);
      stmt_ownership_rules_check_lambda_borrow_capture(cg, value, bindings,
                                                       depth + 1);
      stmt_ownership_rules_bind_borrow(bindings, name, source);
      if (!stmt_ownership_rules_bind_borrow_capture(bindings, name,
                                                    contains_borrow))
        stmt_ownership_rules_error(
            cg, s->tok, "B2",
            "cannot validate borrow capture provenance because ownership "
            "analysis ran out of memory");
    }
    for (size_t i = 0; i < s->as.var.exprs.len; ++i)
      if (i >= s->as.var.names.len)
        stmt_ownership_rules_check_expr(cg, fn_stmt, fn, s->tok,
                                        s->as.var.exprs.data[i], depth + 1);
    break;
  case NY_S_EXPR: {
    const expr_t *e = s->as.expr.expr;
    if (e && e->kind == NY_E_BINARY && e->as.binary.op &&
        strcmp(e->as.binary.op, "=") == 0 &&
        stmt_ownership_rules_is_persistent_store(e->as.binary.left) &&
        stmt_ownership_rules_expr_contains_borrow(bindings, e->as.binary.right))
      stmt_ownership_rules_error(
          cg, e->as.binary.right->tok, "B1",
          "a second-class borrow cannot be stored in a field, collection, "
          "or dereferenced place");
    if (e && e->kind == NY_E_BINARY && e->as.binary.op &&
        strcmp(e->as.binary.op, "=") == 0 && e->as.binary.left &&
        e->as.binary.left->kind == NY_E_IDENT) {
      const char *name = e->as.binary.left->as.ident.name;
      const char *source =
          stmt_ownership_rules_borrow_expr_source(bindings, e->as.binary.right);
      bool contains_borrow = stmt_ownership_rules_expr_contains_borrow(
          bindings, e->as.binary.right);
      stmt_ownership_rules_check_lambda_borrow_capture(cg, e->as.binary.right,
                                                       bindings, depth + 1);
      stmt_ownership_rules_bind_borrow(bindings, name, source);
      if (!stmt_ownership_rules_bind_borrow_capture(bindings, name,
                                                    contains_borrow))
        stmt_ownership_rules_error(
            cg, s->tok, "B2",
            "cannot validate borrow capture provenance because ownership "
            "analysis ran out of memory");
    } else {
      stmt_ownership_rules_check_lambda_borrow_capture(cg, e, bindings,
                                                       depth + 1);
    }
    stmt_ownership_rules_check_expr(cg, fn_stmt, fn, s->tok, e, depth + 1);
    break;
  }
  case NY_S_RETURN: {
    const expr_t *value = s->as.ret.value;
    stmt_ownership_rules_check_lambda_borrow_capture(cg, value, bindings,
                                                     depth + 1);
    const char *source =
        stmt_ownership_rules_borrow_expr_source(bindings, value);
    if (source && !stmt_ownership_rules_borrow_source(value) &&
        (!stmt_ownership_rules_param_exists(fn, source) ||
         !stmt_ownership_rules_has_returns_borrow(fn_stmt, source)))
      stmt_ownership_rules_error(
          cg, value->tok, "B2",
          "returning a borrow requires @returns_borrow(param) naming its "
          "source parameter");
    stmt_ownership_rules_check_expr(cg, fn_stmt, fn, s->tok, value, depth + 1);
    break;
  }
  case NY_S_IF: {
    stmt_ownership_rules_borrow_bindings_t base = {0}, yes = {0}, no = {0};
    if (!stmt_ownership_rules_clone_borrow_bindings(&base, bindings) ||
        !stmt_ownership_rules_clone_borrow_bindings(&yes, bindings) ||
        !stmt_ownership_rules_clone_borrow_bindings(&no, bindings)) {
      free(base.data);
      free(yes.data);
      free(no.data);
      stmt_ownership_rules_error(
          cg, s->tok, "B2",
          "cannot validate returned borrow sources because ownership "
          "analysis ran out of memory");
      break;
    }
    stmt_ownership_rules_check_alias_returns(cg, fn_stmt, fn, s->as.iff.conseq,
                                             &yes, depth + 1);
    stmt_ownership_rules_check_alias_returns(cg, fn_stmt, fn, s->as.iff.alt,
                                             &no, depth + 1);
    stmt_ownership_rules_join_borrow_branches(cg, s->tok, bindings, &base, &yes,
                                              &no, s->as.iff.conseq,
                                              s->as.iff.alt);
    free(base.data);
    free(yes.data);
    free(no.data);
    break;
  }
  case NY_S_WHILE:
  case NY_S_FOR: {
    stmt_ownership_rules_borrow_bindings_t base = {0}, body = {0};
    if (!stmt_ownership_rules_clone_borrow_bindings(&base, bindings) ||
        !stmt_ownership_rules_clone_borrow_bindings(&body, bindings)) {
      free(base.data);
      free(body.data);
      stmt_ownership_rules_error(
          cg, s->tok, "B2",
          "cannot validate returned borrow sources because ownership "
          "analysis ran out of memory");
      break;
    }
    const stmt_t *loop_body =
        s->kind == NY_S_WHILE ? s->as.whl.body : s->as.fr.body;
    stmt_ownership_rules_check_alias_returns(cg, fn_stmt, fn, loop_body, &body,
                                             depth + 1);
    stmt_ownership_rules_join_borrow_branches(cg, s->tok, bindings, &base,
                                              &base, &body, NULL, loop_body);
    free(base.data);
    free(body.data);
    break;
  }
  case NY_S_TRY: {
    stmt_ownership_rules_borrow_bindings_t base = {0}, body = {0},
                                           handler = {0};
    if (!stmt_ownership_rules_clone_borrow_bindings(&base, bindings) ||
        !stmt_ownership_rules_clone_borrow_bindings(&body, bindings) ||
        !stmt_ownership_rules_clone_borrow_bindings(&handler, bindings)) {
      free(base.data);
      free(body.data);
      free(handler.data);
      stmt_ownership_rules_error(
          cg, s->tok, "B2",
          "cannot validate returned borrow sources because ownership "
          "analysis ran out of memory");
      break;
    }
    stmt_ownership_rules_check_alias_returns(cg, fn_stmt, fn, s->as.tr.body,
                                             &body, depth + 1);
    stmt_ownership_rules_check_alias_returns(cg, fn_stmt, fn, s->as.tr.handler,
                                             &handler, depth + 1);
    stmt_ownership_rules_join_borrow_branches(cg, s->tok, bindings, &base,
                                              &body, &handler, s->as.tr.body,
                                              s->as.tr.handler);
    free(base.data);
    free(body.data);
    free(handler.data);
    break;
  }
  case NY_S_FUNC:
    stmt_ownership_rules_check_lambda_borrow_capture_stmt(cg, s, bindings,
                                                          depth + 1);
    break;
  case NY_S_MATCH: {
    stmt_ownership_rules_borrow_bindings_t base = {0}, merged = {0};
    if (!stmt_ownership_rules_clone_borrow_bindings(&base, bindings)) {
      stmt_ownership_rules_error(
          cg, s->tok, "B2",
          "cannot validate returned borrow sources because ownership "
          "analysis ran out of memory");
      break;
    }
    bool have_branch = false;
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      const stmt_t *branch = s->as.match.arms.data[i].conseq;
      stmt_ownership_rules_borrow_bindings_t state = {0};
      if (!stmt_ownership_rules_clone_borrow_bindings(&state, &base)) {
        stmt_ownership_rules_error(
            cg, s->tok, "B2",
            "cannot validate returned borrow sources because ownership "
            "analysis ran out of memory");
        free(state.data);
        break;
      }
      stmt_ownership_rules_check_alias_returns(cg, fn_stmt, fn, branch, &state,
                                               depth + 1);
      if (!have_branch) {
        merged = state;
        have_branch = true;
      } else {
        stmt_ownership_rules_join_borrow_branches(
            cg, s->tok, &merged, &base, &merged, &state,
            s->as.match.arms.data[i - 1].conseq, branch);
        free(state.data);
      }
    }
    if (s->as.match.default_conseq) {
      stmt_ownership_rules_borrow_bindings_t state = {0};
      if (stmt_ownership_rules_clone_borrow_bindings(&state, &base)) {
        stmt_ownership_rules_check_alias_returns(
            cg, fn_stmt, fn, s->as.match.default_conseq, &state, depth + 1);
        if (!have_branch) {
          merged = state;
          have_branch = true;
        } else {
          stmt_ownership_rules_join_borrow_branches(
              cg, s->tok, &merged, &base, &merged, &state,
              s->as.match.arms.len
                  ? s->as.match.arms.data[s->as.match.arms.len - 1].conseq
                  : NULL,
              s->as.match.default_conseq);
          free(state.data);
        }
      } else {
        stmt_ownership_rules_error(
            cg, s->tok, "B2",
            "cannot validate returned borrow sources because ownership "
            "analysis ran out of memory");
      }
    } else if (have_branch) {
      stmt_ownership_rules_join_borrow_branches(
          cg, s->tok, &merged, &base, &merged, &base,
          s->as.match.arms.len
              ? s->as.match.arms.data[s->as.match.arms.len - 1].conseq
              : NULL,
          NULL);
    }
    if (have_branch) {
      free(bindings->data);
      *bindings = merged;
    } else {
      free(merged.data);
    }
    free(base.data);
    break;
  }
  default:
    break;
  }
}

static void stmt_ownership_check_rules(codegen_t *cg, const stmt_t *fn_stmt) {
  if (!cg || !cg->ownership_strict || !fn_stmt || fn_stmt->kind != NY_S_FUNC ||
      ny_is_stdlib_tok(fn_stmt->tok))
    return;
  stmt_ownership_rules_check_stmt(cg, fn_stmt, &fn_stmt->as.fn,
                                  fn_stmt->as.fn.body, 0);
  stmt_ownership_rules_check_exclusive_borrow_block(cg, fn_stmt->as.fn.body, 0);
  stmt_ownership_rules_borrow_bindings_t bindings = {0};
  stmt_ownership_rules_check_alias_returns(cg, fn_stmt, &fn_stmt->as.fn,
                                           fn_stmt->as.fn.body, &bindings, 0);
  free(bindings.data);
}

void stmt_ownership_check_function(codegen_t *cg, const stmt_t *fn_stmt) {
  if (!cg || !fn_stmt || fn_stmt->kind != NY_S_FUNC)
    return;
  if (fn_stmt->as.fn.attr_allowances.len != 0)
    stmt_ownership_check_allowances(cg, fn_stmt);
  stmt_ownership_check_resource_params(cg, fn_stmt);
  stmt_ownership_check_rules(cg, fn_stmt);
}
