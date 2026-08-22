/*
 * Ownership tracking module: handles borrow checking, move semantics,
 * drop deferrals, and lifetime diagnostics for Nytrix codegen.
 */
#include "code/ownership.h"
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

static expr_t *stmt_ownership_return_borrow_arg(codegen_t *cg,
                                                expr_t *call_expr);
expr_t *stmt_ownership_releases_arg(codegen_t *cg, expr_t *call_expr);
expr_t *stmt_ownership_forgets_arg(codegen_t *cg, expr_t *call_expr);
static expr_t *stmt_ownership_consumes_arg(codegen_t *cg, expr_t *call_expr);
static bool stmt_ownership_binding_is_immediate(binding *b);

static binding *stmt_lookup_binding(codegen_t *cg, scope *scopes, size_t depth,
                                    const char *name, size_t name_len,
                                    uint64_t hash) {
  return lookup_binding_hash(cg, scopes, depth, name, name_len, hash);
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

void stmt_ownership_register_slot_defer(codegen_t *cg, scope *scopes,
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

void stmt_ownership_cleanup_scope(codegen_t *cg, scope *scopes,
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

void stmt_ownership_release_source(codegen_t *cg, scope *scopes,
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

void stmt_ownership_post_store(codegen_t *cg, scope *scopes,
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
