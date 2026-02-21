#include "base/util.h"
#include "priv.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Function purity/effects inference and policy diagnostics.
 * Split from func.c for better locality and maintainability.
 */

typedef VEC(const char *) assigned_name_list;
typedef VEC(uint64_t) assigned_hash_list;

static bool assigned_name_has(const assigned_name_list *names,
                              const assigned_hash_list *hashes,
                              const char *name, uint64_t hash,
                              const uint64_t bloom[4]) {
  if (!names || !hashes || !name)
    return false;
  return ny_name_set_has_hash(names->data, names->len, hashes->data,
                              hashes->len, bloom, name, hash);
}

static inline bool assigned_name_contains(const assigned_name_list *names,
                                          const assigned_hash_list *hashes,
                                          const uint64_t bloom[4],
                                          const char *name) {
  if (!name || !*name)
    return false;
  uint64_t hash = ny_hash64_cstr(name);
  return assigned_name_has(names, hashes, name, hash, bloom);
}

static void assigned_name_add(assigned_name_list *names,
                              assigned_hash_list *hashes, uint64_t bloom[4],
                              const char *name) {
  if (!names || !name || !*name)
    return;
  if (!hashes)
    return;
  uint64_t hash = ny_hash64_cstr(name);
  if (assigned_name_has(names, hashes, name, hash, bloom))
    return;
  vec_push(names, name);
  vec_push(hashes, hash);
  ny_name_bloom_add(bloom, hash);
}

static bool ny_builtin_name_is_pure(const char *name) {
  static const char *pure_builtins[] = {
      "__tag",          "__untag",     "__is_int",
      "__is_ptr",       "__is_ny_obj", "__is_str_obj",
      "__is_float_obj", "__tagof",     "__add",
      "__sub",          "__mul",       "__div",
      "__mod",          "__and",       "__or",
      "__xor",          "__shl",       "__shr",
      "__not",          "__eq",        "__lt",
      "__le",           "__gt",        "__ge",
      "__flt_lt",       "__flt_gt",    "__flt_eq",
      "__flt_to_int",   "__flt_trunc", NULL};
  if (!name || !*name)
    return false;
  for (size_t i = 0; pure_builtins[i]; i++) {
    if (strcmp(name, pure_builtins[i]) == 0)
      return true;
  }
  return false;
}

static bool ny_is_std_qname(const char *name) {
  if (!name || !*name)
    return false;
  return strncmp(name, "std.", 4) == 0 || strncmp(name, "lib.", 4) == 0 ||
         strncmp(name, "src.std.", 8) == 0 || strncmp(name, "src.lib.", 8) == 0;
}

static bool ny_sig_is_pure(fun_sig *sig) {
  if (!sig)
    return false;
  if (sig->is_extern)
    return false;
  if (!sig->stmt_t)
    return ny_builtin_name_is_pure(sig->name);
  if (sig->stmt_t->kind != NY_S_FUNC)
    return false;
  if (sig->stmt_t->as.fn.attr_thread || sig->stmt_t->as.fn.attr_naked)
    return false;
  sema_func_t *sema = (sema_func_t *)sig->stmt_t->sema;
  if (!sema || !sema->purity_known)
    return false;
  return sema->is_pure;
}

static bool ny_sig_is_memo_safe(fun_sig *sig) {
  if (!sig)
    return false;
  if (ny_sig_is_pure(sig))
    return true;
  if (sig->is_extern)
    return false;
  if (!sig->stmt_t || sig->stmt_t->kind != NY_S_FUNC)
    return false;
  if (sig->stmt_t->as.fn.attr_thread || sig->stmt_t->as.fn.attr_naked)
    return false;
  sema_func_t *sema = (sema_func_t *)sig->stmt_t->sema;
  if (!sema || !sema->memo_known)
    return false;
  return sema->is_memo_safe;
}

static uint32_t ny_builtin_name_effects(const char *name) {
  if (!name || !*name)
    return NY_FX_ALL;
  if (ny_builtin_name_is_pure(name))
    return NY_FX_NONE;

  if (strcmp(name, "__malloc") == 0 || strcmp(name, "__free") == 0 ||
      strcmp(name, "__realloc") == 0 || strcmp(name, "__zalloc") == 0 ||
      strcmp(name, "__set_args") == 0 || strcmp(name, "__cleanup_args") == 0 ||
      strcmp(name, "__flt_box") == 0) {
    return NY_FX_ALLOC;
  }

  if (strncmp(name, "__thread_", 9) == 0 ||
      strcmp(name, "__thread_spawn") == 0 ||
      strcmp(name, "__thread_join") == 0 ||
      strcmp(name, "__thread_launch") == 0 ||
      strcmp(name, "__thread_detach") == 0 ||
      strcmp(name, "__thread_sleep_ms") == 0) {
    return NY_FX_THREAD;
  }

  if (strncmp(name, "__sys", 5) == 0 || strcmp(name, "__open") == 0 ||
      strcmp(name, "__close") == 0 || strcmp(name, "__ioctl") == 0 ||
      strcmp(name, "__print") == 0 || strcmp(name, "__panic") == 0 ||
      strcmp(name, "__trace_func") == 0 || strcmp(name, "__stats") == 0) {
    return NY_FX_IO;
  }

  return NY_FX_ALL;
}

static uint32_t ny_sig_effects(fun_sig *sig) {
  if (!sig)
    return NY_FX_ALL;
  if (sig->is_extern)
    return NY_FX_FFI | NY_FX_IO;
  if (!sig->stmt_t)
    return ny_builtin_name_effects(sig->name);
  if (sig->stmt_t->kind != NY_S_FUNC)
    return NY_FX_ALL;
  sema_func_t *sema = (sema_func_t *)sig->stmt_t->sema;
  if (!sema || !sema->effects_known)
    return NY_FX_ALL;
  return sema->effects;
}

static void ny_effect_mask_to_buf(uint32_t mask, char *buf, size_t cap) {
  if (!buf || cap == 0)
    return;
  buf[0] = '\0';
  if (mask == NY_FX_NONE) {
    snprintf(buf, cap, "none");
    return;
  }
  size_t used = 0;
  struct {
    uint32_t bit;
    const char *name;
  } parts[] = {{NY_FX_IO, "io"},
               {NY_FX_ALLOC, "alloc"},
               {NY_FX_FFI, "ffi"},
               {NY_FX_THREAD, "thread"}};
  for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
    if ((mask & parts[i].bit) == 0)
      continue;
    int n = snprintf(buf + used, cap - used, "%s%s", used ? "|" : "",
                     parts[i].name);
    if (n < 0)
      break;
    size_t wrote = (size_t)n;
    if (wrote >= cap - used) {
      used = cap - 1;
      break;
    }
    used += wrote;
  }
  uint32_t unknown = mask & ~NY_FX_ALL;
  if (unknown != 0 && used + 16 < cap) {
    snprintf(buf + used, cap - used, "%sunknown(0x%x)", used ? "|" : "",
             (unsigned)unknown);
  }
}

static uint32_t ny_effect_mask_for_token(const char *token, bool *recognized) {
  if (recognized)
    *recognized = true;
  if (!token || !*token) {
    if (recognized)
      *recognized = false;
    return NY_FX_NONE;
  }
  if (strcmp(token, "io") == 0)
    return NY_FX_IO;
  if (strcmp(token, "alloc") == 0)
    return NY_FX_ALLOC;
  if (strcmp(token, "ffi") == 0)
    return NY_FX_FFI;
  if (strcmp(token, "thread") == 0)
    return NY_FX_THREAD;
  if (strcmp(token, "all") == 0)
    return NY_FX_ALL;
  if (strcmp(token, "none") == 0 || strcmp(token, "pure") == 0)
    return NY_FX_NONE;
  if (recognized)
    *recognized = false;
  return NY_FX_NONE;
}

static uint32_t ny_effect_mask_parse_env(codegen_t *cg, const char *raw,
                                         bool *has_any_tokens) {
  if (has_any_tokens)
    *has_any_tokens = false;
  if (!raw || !*raw)
    return NY_FX_NONE;

  uint32_t mask = NY_FX_NONE;
  char *copy = ny_strdup(raw);
  if (!copy)
    return NY_FX_NONE;

  char *tok = strtok(copy, ",|; \t\r\n");
  while (tok) {
    if (has_any_tokens)
      *has_any_tokens = true;
    for (char *p = tok; *p; p++)
      *p = (char)tolower((unsigned char)*p);
    bool recognized = false;
    uint32_t bits = ny_effect_mask_for_token(tok, &recognized);
    if (!recognized) {
      ny_diag_warning((token_t){0},
                      "unknown effect token '%s' in NYTRIX_EFFECT_FORBID "
                      "(expected: io, alloc, ffi, thread, all)",
                      tok);
      if (cg && cg->strict_diagnostics)
        cg->had_error = 1;
    } else {
      mask |= bits;
    }
    tok = strtok(NULL, ",|; \t\r\n");
  }

  free(copy);
  return mask;
}

static fun_sig *ny_purity_resolve_call_sig(codegen_t *cg, expr_call_t *call,
                                           assigned_name_list *local_names,
                                           assigned_hash_list *local_hashes,
                                           uint64_t local_bloom[4]);

static fun_sig *ny_purity_resolve_memcall_sig(codegen_t *cg, expr_memcall_t *mc,
                                              assigned_name_list *local_names,
                                              assigned_hash_list *local_hashes,
                                              uint64_t local_bloom[4]);

static uint32_t ny_expr_effects(codegen_t *cg, expr_t *e,
                                assigned_name_list *local_names,
                                assigned_hash_list *local_hashes,
                                uint64_t local_bloom[4]);

static uint32_t ny_stmt_effects(codegen_t *cg, stmt_t *s,
                                assigned_name_list *local_names,
                                assigned_hash_list *local_hashes,
                                uint64_t local_bloom[4]);

static uint32_t ny_expr_effects(codegen_t *cg, expr_t *e,
                                assigned_name_list *local_names,
                                assigned_hash_list *local_hashes,
                                uint64_t local_bloom[4]) {
  if (!e)
    return NY_FX_NONE;
  switch (e->kind) {
  case NY_E_IDENT:
  case NY_E_LITERAL:
  case NY_E_INFERRED_MEMBER:
  case NY_E_EMBED:
    return NY_FX_NONE;
  case NY_E_UNARY:
    return ny_expr_effects(cg, e->as.unary.right, local_names, local_hashes,
                           local_bloom);
  case NY_E_BINARY:
  case NY_E_LOGICAL:
    return ny_expr_effects(cg, e->as.binary.left, local_names, local_hashes,
                           local_bloom) |
           ny_expr_effects(cg, e->as.binary.right, local_names, local_hashes,
                           local_bloom);
  case NY_E_TERNARY:
    return ny_expr_effects(cg, e->as.ternary.cond, local_names, local_hashes,
                           local_bloom) |
           ny_expr_effects(cg, e->as.ternary.true_expr, local_names,
                           local_hashes, local_bloom) |
           ny_expr_effects(cg, e->as.ternary.false_expr, local_names,
                           local_hashes, local_bloom);
  case NY_E_CALL: {
    uint32_t fx = ny_expr_effects(cg, e->as.call.callee, local_names,
                                  local_hashes, local_bloom);
    for (size_t i = 0; i < e->as.call.args.len; i++) {
      fx |= ny_expr_effects(cg, e->as.call.args.data[i].val, local_names,
                            local_hashes, local_bloom);
    }
    fun_sig *sig = ny_purity_resolve_call_sig(cg, &e->as.call, local_names,
                                              local_hashes, local_bloom);
    fx |= ny_sig_effects(sig);
    if (!sig)
      fx |= NY_FX_FFI;
    return fx;
  }
  case NY_E_MEMCALL: {
    uint32_t fx = ny_expr_effects(cg, e->as.memcall.target, local_names,
                                  local_hashes, local_bloom);
    for (size_t i = 0; i < e->as.memcall.args.len; i++) {
      fx |= ny_expr_effects(cg, e->as.memcall.args.data[i].val, local_names,
                            local_hashes, local_bloom);
    }
    fun_sig *sig = ny_purity_resolve_memcall_sig(
        cg, &e->as.memcall, local_names, local_hashes, local_bloom);
    fx |= ny_sig_effects(sig);
    if (!sig)
      fx |= NY_FX_FFI;
    return fx;
  }
  case NY_E_INDEX:
    return ny_expr_effects(cg, e->as.index.target, local_names, local_hashes,
                           local_bloom) |
           ny_expr_effects(cg, e->as.index.start, local_names, local_hashes,
                           local_bloom) |
           ny_expr_effects(cg, e->as.index.stop, local_names, local_hashes,
                           local_bloom) |
           ny_expr_effects(cg, e->as.index.step, local_names, local_hashes,
                           local_bloom);
  case NY_E_MEMBER:
    return ny_expr_effects(cg, e->as.member.target, local_names, local_hashes,
                           local_bloom);
  case NY_E_PTR_TYPE:
    return ny_expr_effects(cg, e->as.ptr_type.target, local_names, local_hashes,
                           local_bloom);
  case NY_E_DEREF:
    return ny_expr_effects(cg, e->as.deref.target, local_names, local_hashes,
                           local_bloom);
  case NY_E_SIZEOF:
    if (e->as.szof.is_type)
      return NY_FX_NONE;
    return ny_expr_effects(cg, e->as.szof.target, local_names, local_hashes,
                           local_bloom);
  case NY_E_TRY:
    return ny_expr_effects(cg, e->as.try_expr.target, local_names, local_hashes,
                           local_bloom);
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET: {
    uint32_t fx = NY_FX_ALLOC;
    for (size_t i = 0; i < e->as.list_like.len; i++)
      fx |= ny_expr_effects(cg, e->as.list_like.data[i], local_names,
                            local_hashes, local_bloom);
    return fx;
  }
  case NY_E_DICT: {
    uint32_t fx = NY_FX_ALLOC;
    for (size_t i = 0; i < e->as.dict.pairs.len; i++) {
      fx |= ny_expr_effects(cg, e->as.dict.pairs.data[i].key, local_names,
                            local_hashes, local_bloom);
      fx |= ny_expr_effects(cg, e->as.dict.pairs.data[i].value, local_names,
                            local_hashes, local_bloom);
    }
    return fx;
  }
  case NY_E_COMPTIME:
    return ny_stmt_effects(cg, e->as.comptime_expr.body, local_names,
                           local_hashes, local_bloom);
  case NY_E_FSTRING: {
    uint32_t fx = NY_FX_ALLOC;
    for (size_t i = 0; i < e->as.fstring.parts.len; i++) {
      fstring_part_t *part = &e->as.fstring.parts.data[i];
      if (part->kind == NY_FSP_EXPR)
        fx |= ny_expr_effects(cg, part->as.e, local_names, local_hashes,
                              local_bloom);
    }
    return fx;
  }
  case NY_E_MATCH: {
    uint32_t fx = ny_expr_effects(cg, e->as.match.test, local_names,
                                  local_hashes, local_bloom);
    for (size_t i = 0; i < e->as.match.arms.len; i++) {
      match_arm_t *arm = &e->as.match.arms.data[i];
      for (size_t j = 0; j < arm->patterns.len; j++) {
        fx |= ny_expr_effects(cg, arm->patterns.data[j], local_names,
                              local_hashes, local_bloom);
      }
      fx |= ny_expr_effects(cg, arm->guard, local_names, local_hashes,
                            local_bloom);
      fx |= ny_stmt_effects(cg, arm->conseq, local_names, local_hashes,
                            local_bloom);
    }
    fx |= ny_stmt_effects(cg, e->as.match.default_conseq, local_names,
                          local_hashes, local_bloom);
    return fx;
  }
  case NY_E_ASM:
    return NY_FX_FFI;
  case NY_E_LAMBDA:
  case NY_E_FN:
    return NY_FX_ALLOC;
  }
  return NY_FX_ALL;
}

static uint32_t ny_stmt_effects(codegen_t *cg, stmt_t *s,
                                assigned_name_list *local_names,
                                assigned_hash_list *local_hashes,
                                uint64_t local_bloom[4]) {
  if (!s)
    return NY_FX_NONE;
  switch (s->kind) {
  case NY_S_BLOCK: {
    uint32_t fx = NY_FX_NONE;
    size_t old_name_len = local_names->len;
    size_t old_hash_len = local_hashes->len;
    uint64_t old_bloom[4] = {local_bloom[0], local_bloom[1], local_bloom[2],
                             local_bloom[3]};
    for (size_t i = 0; i < s->as.block.body.len; i++)
      fx |= ny_stmt_effects(cg, s->as.block.body.data[i], local_names,
                            local_hashes, local_bloom);
    local_names->len = old_name_len;
    local_hashes->len = old_hash_len;
    local_bloom[0] = old_bloom[0];
    local_bloom[1] = old_bloom[1];
    local_bloom[2] = old_bloom[2];
    local_bloom[3] = old_bloom[3];
    return fx;
  }
  case NY_S_VAR: {
    uint32_t fx = NY_FX_NONE;
    for (size_t i = 0; i < s->as.var.exprs.len; i++)
      fx |= ny_expr_effects(cg, s->as.var.exprs.data[i], local_names,
                            local_hashes, local_bloom);
    if (s->as.var.is_decl) {
      for (size_t i = 0; i < s->as.var.names.len; i++) {
        assigned_name_add(local_names, local_hashes, local_bloom,
                          s->as.var.names.data[i]);
      }
    }
    return fx;
  }
  case NY_S_EXPR:
    return ny_expr_effects(cg, s->as.expr.expr, local_names, local_hashes,
                           local_bloom);
  case NY_S_RETURN:
    return ny_expr_effects(cg, s->as.ret.value, local_names, local_hashes,
                           local_bloom);
  case NY_S_IF:
    return ny_expr_effects(cg, s->as.iff.test, local_names, local_hashes,
                           local_bloom) |
           ny_stmt_effects(cg, s->as.iff.conseq, local_names, local_hashes,
                           local_bloom) |
           ny_stmt_effects(cg, s->as.iff.alt, local_names, local_hashes,
                           local_bloom);
  case NY_S_WHILE:
    return ny_expr_effects(cg, s->as.whl.test, local_names, local_hashes,
                           local_bloom) |
           ny_stmt_effects(cg, s->as.whl.body, local_names, local_hashes,
                           local_bloom);
  case NY_S_FOR: {
    uint32_t fx = ny_expr_effects(cg, s->as.fr.iterable, local_names,
                                  local_hashes, local_bloom);
    size_t old_name_len = local_names->len;
    size_t old_hash_len = local_hashes->len;
    uint64_t old_bloom[4] = {local_bloom[0], local_bloom[1], local_bloom[2],
                             local_bloom[3]};
    assigned_name_add(local_names, local_hashes, local_bloom,
                      s->as.fr.iter_var);
    fx |= ny_stmt_effects(cg, s->as.fr.body, local_names, local_hashes,
                          local_bloom);
    local_names->len = old_name_len;
    local_hashes->len = old_hash_len;
    local_bloom[0] = old_bloom[0];
    local_bloom[1] = old_bloom[1];
    local_bloom[2] = old_bloom[2];
    local_bloom[3] = old_bloom[3];
    return fx;
  }
  case NY_S_MATCH: {
    uint32_t fx = ny_expr_effects(cg, s->as.match.test, local_names,
                                  local_hashes, local_bloom);
    for (size_t i = 0; i < s->as.match.arms.len; i++) {
      match_arm_t *arm = &s->as.match.arms.data[i];
      for (size_t j = 0; j < arm->patterns.len; j++) {
        fx |= ny_expr_effects(cg, arm->patterns.data[j], local_names,
                              local_hashes, local_bloom);
      }
      fx |= ny_expr_effects(cg, arm->guard, local_names, local_hashes,
                            local_bloom);
      fx |= ny_stmt_effects(cg, arm->conseq, local_names, local_hashes,
                            local_bloom);
    }
    fx |= ny_stmt_effects(cg, s->as.match.default_conseq, local_names,
                          local_hashes, local_bloom);
    return fx;
  }
  case NY_S_TRY:
    return ny_stmt_effects(cg, s->as.tr.body, local_names, local_hashes,
                           local_bloom) |
           ny_stmt_effects(cg, s->as.tr.handler, local_names, local_hashes,
                           local_bloom);
  case NY_S_DEFER:
    return ny_stmt_effects(cg, s->as.de.body, local_names, local_hashes,
                           local_bloom);
  case NY_S_MACRO: {
    uint32_t fx = NY_FX_ALL;
    for (size_t i = 0; i < s->as.macro.args.len; i++) {
      fx |= ny_expr_effects(cg, s->as.macro.args.data[i], local_names,
                            local_hashes, local_bloom);
    }
    fx |= ny_stmt_effects(cg, s->as.macro.body, local_names, local_hashes,
                          local_bloom);
    return fx;
  }
  default:
    return NY_FX_NONE;
  }
}

static uint32_t ny_func_decl_effects(codegen_t *cg, stmt_t *fn_stmt) {
  if (!cg || !fn_stmt || fn_stmt->kind != NY_S_FUNC || !fn_stmt->as.fn.body)
    return NY_FX_ALL;
  uint32_t fx = NY_FX_NONE;
  if (fn_stmt->as.fn.attr_thread)
    fx |= NY_FX_THREAD;
  if (fn_stmt->as.fn.attr_naked)
    fx |= NY_FX_FFI;

  assigned_name_list local_names = {0};
  assigned_hash_list local_hashes = {0};
  uint64_t local_bloom[4] = {0, 0, 0, 0};
  for (size_t i = 0; i < fn_stmt->as.fn.params.len; i++) {
    assigned_name_add(&local_names, &local_hashes, local_bloom,
                      fn_stmt->as.fn.params.data[i].name);
  }
  fx |= ny_stmt_effects(cg, fn_stmt->as.fn.body, &local_names, &local_hashes,
                        local_bloom);
  vec_free(&local_names);
  vec_free(&local_hashes);
  return fx;
}

static fun_sig *ny_purity_resolve_call_sig(codegen_t *cg, expr_call_t *call,
                                           assigned_name_list *local_names,
                                           assigned_hash_list *local_hashes,
                                           uint64_t local_bloom[4]) {
  if (!cg || !call || !call->callee || call->callee->kind != NY_E_IDENT)
    return NULL;
  const char *name = call->callee->as.ident.name;
  if (!name || !*name)
    return NULL;
  if (assigned_name_contains(local_names, local_hashes, local_bloom, name))
    return NULL;
  fun_sig *sig = resolve_overload(cg, name, call->args.len);
  if (!sig)
    sig = lookup_use_module_fun(cg, name, call->args.len);
  if (!sig)
    sig = lookup_fun(cg, name);
  return sig;
}

static fun_sig *ny_purity_resolve_memcall_sig(codegen_t *cg, expr_memcall_t *mc,
                                              assigned_name_list *local_names,
                                              assigned_hash_list *local_hashes,
                                              uint64_t local_bloom[4]) {
  if (!cg || !mc || !mc->name || !mc->target ||
      mc->target->kind != NY_E_IDENT) {
    return NULL;
  }
  const char *target_name = mc->target->as.ident.name;
  if (!target_name || !*target_name)
    return NULL;
  if (assigned_name_contains(local_names, local_hashes, local_bloom,
                             target_name)) {
    return NULL;
  }

  const char *module_name = resolve_import_alias(cg, target_name);
  bool module_like = module_name != NULL;
  if (!module_name)
    module_name = target_name;

  if (!module_like) {
    if (lookup_global(cg, target_name) || lookup_fun(cg, target_name))
      return NULL;
    module_like = true;
  }
  if (!module_like)
    return NULL;

  char dotted[512];
  int written =
      snprintf(dotted, sizeof(dotted), "%s.%s", module_name, mc->name);
  if (written <= 0 || (size_t)written >= sizeof(dotted))
    return NULL;
  return lookup_fun(cg, dotted);
}

static bool ny_expr_is_pure(codegen_t *cg, expr_t *e,
                            assigned_name_list *local_names,
                            assigned_hash_list *local_hashes,
                            uint64_t local_bloom[4]);

static bool ny_stmt_is_pure(codegen_t *cg, stmt_t *s,
                            assigned_name_list *local_names,
                            assigned_hash_list *local_hashes,
                            uint64_t local_bloom[4]);

static bool ny_expr_is_memo_safe(codegen_t *cg, expr_t *e,
                                 assigned_name_list *local_names,
                                 assigned_hash_list *local_hashes,
                                 uint64_t local_bloom[4]);

static bool ny_stmt_is_memo_safe(codegen_t *cg, stmt_t *s,
                                 assigned_name_list *local_names,
                                 assigned_hash_list *local_hashes,
                                 uint64_t local_bloom[4]);

static bool ny_expr_is_pure(codegen_t *cg, expr_t *e,
                            assigned_name_list *local_names,
                            assigned_hash_list *local_hashes,
                            uint64_t local_bloom[4]) {
  if (!e)
    return true;
  switch (e->kind) {
  case NY_E_LITERAL:
    return true;
  case NY_E_IDENT: {
    const char *name = e->as.ident.name;
    if (!name || !*name)
      return false;
    if (assigned_name_contains(local_names, local_hashes, local_bloom, name))
      return true;
    if (lookup_enum_member(cg, name))
      return true;
    if (lookup_fun(cg, name))
      return true;
    return false;
  }
  case NY_E_UNARY:
    return ny_expr_is_pure(cg, e->as.unary.right, local_names, local_hashes,
                           local_bloom);
  case NY_E_BINARY:
  case NY_E_LOGICAL:
    return ny_expr_is_pure(cg, e->as.binary.left, local_names, local_hashes,
                           local_bloom) &&
           ny_expr_is_pure(cg, e->as.binary.right, local_names, local_hashes,
                           local_bloom);
  case NY_E_TERNARY:
    return ny_expr_is_pure(cg, e->as.ternary.cond, local_names, local_hashes,
                           local_bloom) &&
           ny_expr_is_pure(cg, e->as.ternary.true_expr, local_names,
                           local_hashes, local_bloom) &&
           ny_expr_is_pure(cg, e->as.ternary.false_expr, local_names,
                           local_hashes, local_bloom);
  case NY_E_CALL: {
    for (size_t i = 0; i < e->as.call.args.len; i++) {
      if (!ny_expr_is_pure(cg, e->as.call.args.data[i].val, local_names,
                           local_hashes, local_bloom))
        return false;
    }
    fun_sig *sig = ny_purity_resolve_call_sig(cg, &e->as.call, local_names,
                                              local_hashes, local_bloom);
    return ny_sig_is_pure(sig);
  }
  case NY_E_MEMCALL: {
    for (size_t i = 0; i < e->as.memcall.args.len; i++) {
      if (!ny_expr_is_pure(cg, e->as.memcall.args.data[i].val, local_names,
                           local_hashes, local_bloom))
        return false;
    }
    fun_sig *sig = ny_purity_resolve_memcall_sig(
        cg, &e->as.memcall, local_names, local_hashes, local_bloom);
    return ny_sig_is_pure(sig);
  }
  case NY_E_INDEX:
  case NY_E_MEMBER:
  case NY_E_DEREF:
  case NY_E_TRY:
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_DICT:
  case NY_E_SET:
  case NY_E_ASM:
  case NY_E_COMPTIME:
  case NY_E_FSTRING:
  case NY_E_INFERRED_MEMBER:
  case NY_E_EMBED:
  case NY_E_LAMBDA:
  case NY_E_FN:
    return false;
  case NY_E_PTR_TYPE:
    return ny_expr_is_pure(cg, e->as.ptr_type.target, local_names, local_hashes,
                           local_bloom);
  case NY_E_SIZEOF:
    if (e->as.szof.is_type)
      return true;
    return ny_expr_is_pure(cg, e->as.szof.target, local_names, local_hashes,
                           local_bloom);
  case NY_E_MATCH:
    if (!ny_expr_is_pure(cg, e->as.match.test, local_names, local_hashes,
                         local_bloom))
      return false;
    for (size_t i = 0; i < e->as.match.arms.len; i++) {
      match_arm_t *arm = &e->as.match.arms.data[i];
      for (size_t j = 0; j < arm->patterns.len; j++) {
        if (!ny_expr_is_pure(cg, arm->patterns.data[j], local_names,
                             local_hashes, local_bloom))
          return false;
      }
      if (!ny_expr_is_pure(cg, arm->guard, local_names, local_hashes,
                           local_bloom))
        return false;
      if (!ny_stmt_is_pure(cg, arm->conseq, local_names, local_hashes,
                           local_bloom))
        return false;
    }
    return ny_stmt_is_pure(cg, e->as.match.default_conseq, local_names,
                           local_hashes, local_bloom);
  }
  return false;
}

static bool ny_stmt_is_pure(codegen_t *cg, stmt_t *s,
                            assigned_name_list *local_names,
                            assigned_hash_list *local_hashes,
                            uint64_t local_bloom[4]) {
  if (!s)
    return true;
  switch (s->kind) {
  case NY_S_BLOCK: {
    size_t old_name_len = local_names->len;
    size_t old_hash_len = local_hashes->len;
    uint64_t old_bloom[4] = {local_bloom[0], local_bloom[1], local_bloom[2],
                             local_bloom[3]};
    bool pure = true;
    for (size_t i = 0; i < s->as.block.body.len; i++) {
      if (!ny_stmt_is_pure(cg, s->as.block.body.data[i], local_names,
                           local_hashes, local_bloom)) {
        pure = false;
        break;
      }
    }
    local_names->len = old_name_len;
    local_hashes->len = old_hash_len;
    local_bloom[0] = old_bloom[0];
    local_bloom[1] = old_bloom[1];
    local_bloom[2] = old_bloom[2];
    local_bloom[3] = old_bloom[3];
    return pure;
  }
  case NY_S_VAR:
    if (!s->as.var.is_decl) {
      for (size_t i = 0; i < s->as.var.names.len; i++) {
        if (!assigned_name_contains(local_names, local_hashes, local_bloom,
                                    s->as.var.names.data[i])) {
          return false;
        }
      }
    }
    for (size_t i = 0; i < s->as.var.exprs.len; i++) {
      if (!ny_expr_is_pure(cg, s->as.var.exprs.data[i], local_names,
                           local_hashes, local_bloom))
        return false;
    }
    if (s->as.var.is_decl) {
      for (size_t i = 0; i < s->as.var.names.len; i++) {
        assigned_name_add(local_names, local_hashes, local_bloom,
                          s->as.var.names.data[i]);
      }
    }
    return true;
  case NY_S_EXPR:
    return ny_expr_is_pure(cg, s->as.expr.expr, local_names, local_hashes,
                           local_bloom);
  case NY_S_RETURN:
    return ny_expr_is_pure(cg, s->as.ret.value, local_names, local_hashes,
                           local_bloom);
  case NY_S_IF:
    return ny_expr_is_pure(cg, s->as.iff.test, local_names, local_hashes,
                           local_bloom) &&
           ny_stmt_is_pure(cg, s->as.iff.conseq, local_names, local_hashes,
                           local_bloom) &&
           ny_stmt_is_pure(cg, s->as.iff.alt, local_names, local_hashes,
                           local_bloom);
  case NY_S_WHILE:
    return ny_expr_is_pure(cg, s->as.whl.test, local_names, local_hashes,
                           local_bloom) &&
           ny_stmt_is_pure(cg, s->as.whl.body, local_names, local_hashes,
                           local_bloom);
  case NY_S_FOR: {
    if (!ny_expr_is_pure(cg, s->as.fr.iterable, local_names, local_hashes,
                         local_bloom))
      return false;
    size_t old_name_len = local_names->len;
    size_t old_hash_len = local_hashes->len;
    uint64_t old_bloom[4] = {local_bloom[0], local_bloom[1], local_bloom[2],
                             local_bloom[3]};
    assigned_name_add(local_names, local_hashes, local_bloom,
                      s->as.fr.iter_var);
    bool pure = ny_stmt_is_pure(cg, s->as.fr.body, local_names, local_hashes,
                                local_bloom);
    local_names->len = old_name_len;
    local_hashes->len = old_hash_len;
    local_bloom[0] = old_bloom[0];
    local_bloom[1] = old_bloom[1];
    local_bloom[2] = old_bloom[2];
    local_bloom[3] = old_bloom[3];
    return pure;
  }
  case NY_S_MATCH:
    if (!ny_expr_is_pure(cg, s->as.match.test, local_names, local_hashes,
                         local_bloom))
      return false;
    for (size_t i = 0; i < s->as.match.arms.len; i++) {
      match_arm_t *arm = &s->as.match.arms.data[i];
      for (size_t j = 0; j < arm->patterns.len; j++) {
        if (!ny_expr_is_pure(cg, arm->patterns.data[j], local_names,
                             local_hashes, local_bloom))
          return false;
      }
      if (!ny_expr_is_pure(cg, arm->guard, local_names, local_hashes,
                           local_bloom))
        return false;
      if (!ny_stmt_is_pure(cg, arm->conseq, local_names, local_hashes,
                           local_bloom))
        return false;
    }
    return ny_stmt_is_pure(cg, s->as.match.default_conseq, local_names,
                           local_hashes, local_bloom);
  case NY_S_TRY:
  case NY_S_DEFER:
  case NY_S_GOTO:
  case NY_S_LABEL:
  case NY_S_FUNC:
  case NY_S_EXTERN:
  case NY_S_LAYOUT:
  case NY_S_MODULE:
  case NY_S_USE:
  case NY_S_EXPORT:
  case NY_S_STRUCT:
  case NY_S_ENUM:
  case NY_S_MACRO:
    return false;
  case NY_S_BREAK:
  case NY_S_CONTINUE:
    return true;
  }
  return false;
}

static bool ny_expr_is_memo_safe(codegen_t *cg, expr_t *e,
                                 assigned_name_list *local_names,
                                 assigned_hash_list *local_hashes,
                                 uint64_t local_bloom[4]) {
  if (!e)
    return true;
  switch (e->kind) {
  case NY_E_LITERAL:
    return true;
  case NY_E_IDENT: {
    const char *name = e->as.ident.name;
    if (!name || !*name)
      return false;
    if (assigned_name_contains(local_names, local_hashes, local_bloom, name))
      return true;
    if (lookup_enum_member(cg, name))
      return true;
    if (lookup_fun(cg, name))
      return true;
    return false;
  }
  case NY_E_UNARY:
    return ny_expr_is_memo_safe(cg, e->as.unary.right, local_names,
                                local_hashes, local_bloom);
  case NY_E_BINARY:
  case NY_E_LOGICAL:
    return ny_expr_is_memo_safe(cg, e->as.binary.left, local_names,
                                local_hashes, local_bloom) &&
           ny_expr_is_memo_safe(cg, e->as.binary.right, local_names,
                                local_hashes, local_bloom);
  case NY_E_TERNARY:
    return ny_expr_is_memo_safe(cg, e->as.ternary.cond, local_names,
                                local_hashes, local_bloom) &&
           ny_expr_is_memo_safe(cg, e->as.ternary.true_expr, local_names,
                                local_hashes, local_bloom) &&
           ny_expr_is_memo_safe(cg, e->as.ternary.false_expr, local_names,
                                local_hashes, local_bloom);
  case NY_E_CALL: {
    for (size_t i = 0; i < e->as.call.args.len; i++) {
      if (!ny_expr_is_memo_safe(cg, e->as.call.args.data[i].val, local_names,
                                local_hashes, local_bloom))
        return false;
    }
    fun_sig *sig = ny_purity_resolve_call_sig(cg, &e->as.call, local_names,
                                              local_hashes, local_bloom);
    return ny_sig_is_memo_safe(sig);
  }
  case NY_E_MEMCALL: {
    for (size_t i = 0; i < e->as.memcall.args.len; i++) {
      if (!ny_expr_is_memo_safe(cg, e->as.memcall.args.data[i].val, local_names,
                                local_hashes, local_bloom))
        return false;
    }
    fun_sig *sig = ny_purity_resolve_memcall_sig(
        cg, &e->as.memcall, local_names, local_hashes, local_bloom);
    return ny_sig_is_memo_safe(sig);
  }
  case NY_E_INDEX:
    return ny_expr_is_memo_safe(cg, e->as.index.target, local_names,
                                local_hashes, local_bloom) &&
           ny_expr_is_memo_safe(cg, e->as.index.start, local_names,
                                local_hashes, local_bloom) &&
           ny_expr_is_memo_safe(cg, e->as.index.stop, local_names, local_hashes,
                                local_bloom) &&
           ny_expr_is_memo_safe(cg, e->as.index.step, local_names, local_hashes,
                                local_bloom);
  case NY_E_MEMBER:
    return ny_expr_is_memo_safe(cg, e->as.member.target, local_names,
                                local_hashes, local_bloom);
  case NY_E_DEREF:
    return ny_expr_is_memo_safe(cg, e->as.deref.target, local_names,
                                local_hashes, local_bloom);
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; i++) {
      if (!ny_expr_is_memo_safe(cg, e->as.list_like.data[i], local_names,
                                local_hashes, local_bloom))
        return false;
    }
    return true;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; i++) {
      if (!ny_expr_is_memo_safe(cg, e->as.dict.pairs.data[i].key, local_names,
                                local_hashes, local_bloom))
        return false;
      if (!ny_expr_is_memo_safe(cg, e->as.dict.pairs.data[i].value, local_names,
                                local_hashes, local_bloom))
        return false;
    }
    return true;
  case NY_E_PTR_TYPE:
    return ny_expr_is_memo_safe(cg, e->as.ptr_type.target, local_names,
                                local_hashes, local_bloom);
  case NY_E_SIZEOF:
    if (e->as.szof.is_type)
      return true;
    return ny_expr_is_memo_safe(cg, e->as.szof.target, local_names,
                                local_hashes, local_bloom);
  case NY_E_MATCH:
    if (!ny_expr_is_memo_safe(cg, e->as.match.test, local_names, local_hashes,
                              local_bloom))
      return false;
    for (size_t i = 0; i < e->as.match.arms.len; i++) {
      match_arm_t *arm = &e->as.match.arms.data[i];
      for (size_t j = 0; j < arm->patterns.len; j++) {
        if (!ny_expr_is_memo_safe(cg, arm->patterns.data[j], local_names,
                                  local_hashes, local_bloom))
          return false;
      }
      if (!ny_expr_is_memo_safe(cg, arm->guard, local_names, local_hashes,
                                local_bloom))
        return false;
      if (!ny_stmt_is_memo_safe(cg, arm->conseq, local_names, local_hashes,
                                local_bloom))
        return false;
    }
    return ny_stmt_is_memo_safe(cg, e->as.match.default_conseq, local_names,
                                local_hashes, local_bloom);
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; i++) {
      fstring_part_t *part = &e->as.fstring.parts.data[i];
      if (part->kind == NY_FSP_EXPR &&
          !ny_expr_is_memo_safe(cg, part->as.e, local_names, local_hashes,
                                local_bloom)) {
        return false;
      }
    }
    return true;
  case NY_E_TRY:
  case NY_E_ASM:
  case NY_E_COMPTIME:
  case NY_E_INFERRED_MEMBER:
  case NY_E_EMBED:
  case NY_E_LAMBDA:
  case NY_E_FN:
    return false;
  }
  return false;
}

static bool ny_stmt_is_memo_safe(codegen_t *cg, stmt_t *s,
                                 assigned_name_list *local_names,
                                 assigned_hash_list *local_hashes,
                                 uint64_t local_bloom[4]) {
  if (!s)
    return true;
  switch (s->kind) {
  case NY_S_BLOCK: {
    size_t old_name_len = local_names->len;
    size_t old_hash_len = local_hashes->len;
    uint64_t old_bloom[4] = {local_bloom[0], local_bloom[1], local_bloom[2],
                             local_bloom[3]};
    bool safe = true;
    for (size_t i = 0; i < s->as.block.body.len; i++) {
      if (!ny_stmt_is_memo_safe(cg, s->as.block.body.data[i], local_names,
                                local_hashes, local_bloom)) {
        safe = false;
        break;
      }
    }
    local_names->len = old_name_len;
    local_hashes->len = old_hash_len;
    local_bloom[0] = old_bloom[0];
    local_bloom[1] = old_bloom[1];
    local_bloom[2] = old_bloom[2];
    local_bloom[3] = old_bloom[3];
    return safe;
  }
  case NY_S_VAR:
    if (!s->as.var.is_decl) {
      for (size_t i = 0; i < s->as.var.names.len; i++) {
        if (!assigned_name_contains(local_names, local_hashes, local_bloom,
                                    s->as.var.names.data[i])) {
          return false;
        }
      }
    }
    for (size_t i = 0; i < s->as.var.exprs.len; i++) {
      if (!ny_expr_is_memo_safe(cg, s->as.var.exprs.data[i], local_names,
                                local_hashes, local_bloom))
        return false;
    }
    if (s->as.var.is_decl) {
      for (size_t i = 0; i < s->as.var.names.len; i++) {
        assigned_name_add(local_names, local_hashes, local_bloom,
                          s->as.var.names.data[i]);
      }
    }
    return true;
  case NY_S_EXPR:
    return ny_expr_is_memo_safe(cg, s->as.expr.expr, local_names, local_hashes,
                                local_bloom);
  case NY_S_RETURN:
    return ny_expr_is_memo_safe(cg, s->as.ret.value, local_names, local_hashes,
                                local_bloom);
  case NY_S_IF:
    return ny_expr_is_memo_safe(cg, s->as.iff.test, local_names, local_hashes,
                                local_bloom) &&
           ny_stmt_is_memo_safe(cg, s->as.iff.conseq, local_names, local_hashes,
                                local_bloom) &&
           ny_stmt_is_memo_safe(cg, s->as.iff.alt, local_names, local_hashes,
                                local_bloom);
  case NY_S_WHILE:
    return ny_expr_is_memo_safe(cg, s->as.whl.test, local_names, local_hashes,
                                local_bloom) &&
           ny_stmt_is_memo_safe(cg, s->as.whl.body, local_names, local_hashes,
                                local_bloom);
  case NY_S_FOR: {
    if (!ny_expr_is_memo_safe(cg, s->as.fr.iterable, local_names, local_hashes,
                              local_bloom))
      return false;
    size_t old_name_len = local_names->len;
    size_t old_hash_len = local_hashes->len;
    uint64_t old_bloom[4] = {local_bloom[0], local_bloom[1], local_bloom[2],
                             local_bloom[3]};
    assigned_name_add(local_names, local_hashes, local_bloom,
                      s->as.fr.iter_var);
    bool safe = ny_stmt_is_memo_safe(cg, s->as.fr.body, local_names,
                                     local_hashes, local_bloom);
    local_names->len = old_name_len;
    local_hashes->len = old_hash_len;
    local_bloom[0] = old_bloom[0];
    local_bloom[1] = old_bloom[1];
    local_bloom[2] = old_bloom[2];
    local_bloom[3] = old_bloom[3];
    return safe;
  }
  case NY_S_MATCH:
    if (!ny_expr_is_memo_safe(cg, s->as.match.test, local_names, local_hashes,
                              local_bloom))
      return false;
    for (size_t i = 0; i < s->as.match.arms.len; i++) {
      match_arm_t *arm = &s->as.match.arms.data[i];
      for (size_t j = 0; j < arm->patterns.len; j++) {
        if (!ny_expr_is_memo_safe(cg, arm->patterns.data[j], local_names,
                                  local_hashes, local_bloom))
          return false;
      }
      if (!ny_expr_is_memo_safe(cg, arm->guard, local_names, local_hashes,
                                local_bloom))
        return false;
      if (!ny_stmt_is_memo_safe(cg, arm->conseq, local_names, local_hashes,
                                local_bloom))
        return false;
    }
    return ny_stmt_is_memo_safe(cg, s->as.match.default_conseq, local_names,
                                local_hashes, local_bloom);
  case NY_S_TRY:
  case NY_S_DEFER:
  case NY_S_GOTO:
  case NY_S_LABEL:
  case NY_S_FUNC:
  case NY_S_EXTERN:
  case NY_S_LAYOUT:
  case NY_S_MODULE:
  case NY_S_USE:
  case NY_S_EXPORT:
  case NY_S_STRUCT:
  case NY_S_ENUM:
  case NY_S_MACRO:
    return false;
  case NY_S_BREAK:
  case NY_S_CONTINUE:
    return true;
  }
  return false;
}

typedef struct ny_escape_summary_t {
  bool args_escape;
  bool args_mutated;
  bool returns_alias;
} ny_escape_summary_t;

static bool ny_stmt_refs_params(stmt_t *s,
                                const assigned_name_list *param_names,
                                const assigned_hash_list *param_hashes,
                                const uint64_t param_bloom[4]);

static bool ny_expr_refs_params(expr_t *e,
                                const assigned_name_list *param_names,
                                const assigned_hash_list *param_hashes,
                                const uint64_t param_bloom[4]) {
  if (!e)
    return false;
  switch (e->kind) {
  case NY_E_IDENT:
    if (!e->as.ident.name || !*e->as.ident.name)
      return false;
    return assigned_name_contains(param_names, param_hashes, param_bloom,
                                  e->as.ident.name);
  case NY_E_UNARY:
    return ny_expr_refs_params(e->as.unary.right, param_names, param_hashes,
                               param_bloom);
  case NY_E_BINARY:
  case NY_E_LOGICAL:
    return ny_expr_refs_params(e->as.binary.left, param_names, param_hashes,
                               param_bloom) ||
           ny_expr_refs_params(e->as.binary.right, param_names, param_hashes,
                               param_bloom);
  case NY_E_TERNARY:
    return ny_expr_refs_params(e->as.ternary.cond, param_names, param_hashes,
                               param_bloom) ||
           ny_expr_refs_params(e->as.ternary.true_expr, param_names,
                               param_hashes, param_bloom) ||
           ny_expr_refs_params(e->as.ternary.false_expr, param_names,
                               param_hashes, param_bloom);
  case NY_E_CALL:
    if (ny_expr_refs_params(e->as.call.callee, param_names, param_hashes,
                            param_bloom))
      return true;
    for (size_t i = 0; i < e->as.call.args.len; i++) {
      if (ny_expr_refs_params(e->as.call.args.data[i].val, param_names,
                              param_hashes, param_bloom))
        return true;
    }
    return false;
  case NY_E_MEMCALL:
    if (ny_expr_refs_params(e->as.memcall.target, param_names, param_hashes,
                            param_bloom))
      return true;
    for (size_t i = 0; i < e->as.memcall.args.len; i++) {
      if (ny_expr_refs_params(e->as.memcall.args.data[i].val, param_names,
                              param_hashes, param_bloom))
        return true;
    }
    return false;
  case NY_E_INDEX:
    return ny_expr_refs_params(e->as.index.target, param_names, param_hashes,
                               param_bloom) ||
           ny_expr_refs_params(e->as.index.start, param_names, param_hashes,
                               param_bloom) ||
           ny_expr_refs_params(e->as.index.stop, param_names, param_hashes,
                               param_bloom) ||
           ny_expr_refs_params(e->as.index.step, param_names, param_hashes,
                               param_bloom);
  case NY_E_MEMBER:
    return ny_expr_refs_params(e->as.member.target, param_names, param_hashes,
                               param_bloom);
  case NY_E_PTR_TYPE:
    return ny_expr_refs_params(e->as.ptr_type.target, param_names, param_hashes,
                               param_bloom);
  case NY_E_DEREF:
    return ny_expr_refs_params(e->as.deref.target, param_names, param_hashes,
                               param_bloom);
  case NY_E_SIZEOF:
    if (e->as.szof.is_type)
      return false;
    return ny_expr_refs_params(e->as.szof.target, param_names, param_hashes,
                               param_bloom);
  case NY_E_TRY:
    return ny_expr_refs_params(e->as.try_expr.target, param_names, param_hashes,
                               param_bloom);
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; i++) {
      if (ny_expr_refs_params(e->as.list_like.data[i], param_names,
                              param_hashes, param_bloom))
        return true;
    }
    return false;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; i++) {
      if (ny_expr_refs_params(e->as.dict.pairs.data[i].key, param_names,
                              param_hashes, param_bloom))
        return true;
      if (ny_expr_refs_params(e->as.dict.pairs.data[i].value, param_names,
                              param_hashes, param_bloom))
        return true;
    }
    return false;
  case NY_E_COMPTIME:
    return ny_stmt_refs_params(e->as.comptime_expr.body, param_names,
                               param_hashes, param_bloom);
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; i++) {
      fstring_part_t *part = &e->as.fstring.parts.data[i];
      if (part->kind == NY_FSP_EXPR &&
          ny_expr_refs_params(part->as.e, param_names, param_hashes,
                              param_bloom)) {
        return true;
      }
    }
    return false;
  case NY_E_MATCH:
    if (ny_expr_refs_params(e->as.match.test, param_names, param_hashes,
                            param_bloom))
      return true;
    for (size_t i = 0; i < e->as.match.arms.len; i++) {
      match_arm_t *arm = &e->as.match.arms.data[i];
      for (size_t j = 0; j < arm->patterns.len; j++) {
        if (ny_expr_refs_params(arm->patterns.data[j], param_names,
                                param_hashes, param_bloom))
          return true;
      }
      if (ny_expr_refs_params(arm->guard, param_names, param_hashes,
                              param_bloom))
        return true;
      if (ny_stmt_refs_params(arm->conseq, param_names, param_hashes,
                              param_bloom))
        return true;
    }
    return ny_stmt_refs_params(e->as.match.default_conseq, param_names,
                               param_hashes, param_bloom);
  default:
    return false;
  }
}

static bool ny_stmt_refs_params(stmt_t *s,
                                const assigned_name_list *param_names,
                                const assigned_hash_list *param_hashes,
                                const uint64_t param_bloom[4]) {
  if (!s)
    return false;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; i++) {
      if (ny_stmt_refs_params(s->as.block.body.data[i], param_names,
                              param_hashes, param_bloom))
        return true;
    }
    return false;
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; i++) {
      if (ny_expr_refs_params(s->as.var.exprs.data[i], param_names,
                              param_hashes, param_bloom))
        return true;
    }
    return false;
  case NY_S_EXPR:
    return ny_expr_refs_params(s->as.expr.expr, param_names, param_hashes,
                               param_bloom);
  case NY_S_RETURN:
    return ny_expr_refs_params(s->as.ret.value, param_names, param_hashes,
                               param_bloom);
  case NY_S_IF:
    return ny_expr_refs_params(s->as.iff.test, param_names, param_hashes,
                               param_bloom) ||
           ny_stmt_refs_params(s->as.iff.conseq, param_names, param_hashes,
                               param_bloom) ||
           ny_stmt_refs_params(s->as.iff.alt, param_names, param_hashes,
                               param_bloom);
  case NY_S_WHILE:
    return ny_expr_refs_params(s->as.whl.test, param_names, param_hashes,
                               param_bloom) ||
           ny_stmt_refs_params(s->as.whl.body, param_names, param_hashes,
                               param_bloom);
  case NY_S_FOR:
    return ny_expr_refs_params(s->as.fr.iterable, param_names, param_hashes,
                               param_bloom) ||
           ny_stmt_refs_params(s->as.fr.body, param_names, param_hashes,
                               param_bloom);
  case NY_S_MATCH:
    if (ny_expr_refs_params(s->as.match.test, param_names, param_hashes,
                            param_bloom))
      return true;
    for (size_t i = 0; i < s->as.match.arms.len; i++) {
      match_arm_t *arm = &s->as.match.arms.data[i];
      for (size_t j = 0; j < arm->patterns.len; j++) {
        if (ny_expr_refs_params(arm->patterns.data[j], param_names,
                                param_hashes, param_bloom))
          return true;
      }
      if (ny_expr_refs_params(arm->guard, param_names, param_hashes,
                              param_bloom))
        return true;
      if (ny_stmt_refs_params(arm->conseq, param_names, param_hashes,
                              param_bloom))
        return true;
    }
    return ny_stmt_refs_params(s->as.match.default_conseq, param_names,
                               param_hashes, param_bloom);
  case NY_S_TRY:
    return ny_stmt_refs_params(s->as.tr.body, param_names, param_hashes,
                               param_bloom) ||
           ny_stmt_refs_params(s->as.tr.handler, param_names, param_hashes,
                               param_bloom);
  case NY_S_DEFER:
    return ny_stmt_refs_params(s->as.de.body, param_names, param_hashes,
                               param_bloom);
  case NY_S_MACRO:
    for (size_t i = 0; i < s->as.macro.args.len; i++) {
      if (ny_expr_refs_params(s->as.macro.args.data[i], param_names,
                              param_hashes, param_bloom)) {
        return true;
      }
    }
    return ny_stmt_refs_params(s->as.macro.body, param_names, param_hashes,
                               param_bloom);
  default:
    return false;
  }
}

typedef enum ny_callee_escape_query {
  NY_CALLEE_Q_ARGS_ESCAPE = 0,
  NY_CALLEE_Q_ARGS_MUTATED,
  NY_CALLEE_Q_RETURNS_ALIAS,
} ny_callee_escape_query_t;

static bool ny_callee_escape_query(fun_sig *sig,
                                   ny_callee_escape_query_t query) {
  if (!sig)
    return true;
  if (sig->is_extern)
    return true;
  if (!sig->stmt_t)
    return !ny_builtin_name_is_pure(sig->name);
  if (sig->stmt_t->kind != NY_S_FUNC)
    return true;
  sema_func_t *sema = (sema_func_t *)sig->stmt_t->sema;
  if (!sema || !sema->escape_known)
    return true;
  switch (query) {
  case NY_CALLEE_Q_ARGS_ESCAPE:
    return sema->args_escape;
  case NY_CALLEE_Q_ARGS_MUTATED:
    return sema->args_mutated;
  case NY_CALLEE_Q_RETURNS_ALIAS:
    return sema->returns_alias;
  default:
    return true;
  }
}

static void ny_collect_escape_expr(
    codegen_t *cg, expr_t *e, const assigned_name_list *param_names,
    const assigned_hash_list *param_hashes, const uint64_t param_bloom[4],
    assigned_name_list *local_names, assigned_hash_list *local_hashes,
    uint64_t local_bloom[4], ny_escape_summary_t *out);

static void ny_collect_escape_stmt(
    codegen_t *cg, stmt_t *s, const assigned_name_list *param_names,
    const assigned_hash_list *param_hashes, const uint64_t param_bloom[4],
    assigned_name_list *local_names, assigned_hash_list *local_hashes,
    uint64_t local_bloom[4], ny_escape_summary_t *out);

static void ny_collect_escape_expr(
    codegen_t *cg, expr_t *e, const assigned_name_list *param_names,
    const assigned_hash_list *param_hashes, const uint64_t param_bloom[4],
    assigned_name_list *local_names, assigned_hash_list *local_hashes,
    uint64_t local_bloom[4], ny_escape_summary_t *out) {
  if (!e || !out)
    return;
  switch (e->kind) {
  case NY_E_CALL: {
    ny_collect_escape_expr(cg, e->as.call.callee, param_names, param_hashes,
                           param_bloom, local_names, local_hashes, local_bloom,
                           out);
    fun_sig *sig = ny_purity_resolve_call_sig(cg, &e->as.call, local_names,
                                              local_hashes, local_bloom);
    bool callee_escape = ny_callee_escape_query(sig, NY_CALLEE_Q_ARGS_ESCAPE);
    bool callee_mutates = ny_callee_escape_query(sig, NY_CALLEE_Q_ARGS_MUTATED);
    bool callee_returns_alias =
        ny_callee_escape_query(sig, NY_CALLEE_Q_RETURNS_ALIAS);
    for (size_t i = 0; i < e->as.call.args.len; i++) {
      expr_t *arg = e->as.call.args.data[i].val;
      bool arg_refs_param =
          ny_expr_refs_params(arg, param_names, param_hashes, param_bloom);
      if (callee_escape && arg_refs_param) {
        out->args_escape = true;
      }
      if (callee_mutates && arg_refs_param)
        out->args_mutated = true;
      if (callee_returns_alias && arg_refs_param)
        out->returns_alias = true;
      ny_collect_escape_expr(cg, arg, param_names, param_hashes, param_bloom,
                             local_names, local_hashes, local_bloom, out);
    }
    break;
  }
  case NY_E_MEMCALL: {
    ny_collect_escape_expr(cg, e->as.memcall.target, param_names, param_hashes,
                           param_bloom, local_names, local_hashes, local_bloom,
                           out);
    fun_sig *sig = ny_purity_resolve_memcall_sig(
        cg, &e->as.memcall, local_names, local_hashes, local_bloom);
    bool callee_escape = ny_callee_escape_query(sig, NY_CALLEE_Q_ARGS_ESCAPE);
    bool callee_mutates = ny_callee_escape_query(sig, NY_CALLEE_Q_ARGS_MUTATED);
    bool callee_returns_alias =
        ny_callee_escape_query(sig, NY_CALLEE_Q_RETURNS_ALIAS);
    for (size_t i = 0; i < e->as.memcall.args.len; i++) {
      expr_t *arg = e->as.memcall.args.data[i].val;
      bool arg_refs_param =
          ny_expr_refs_params(arg, param_names, param_hashes, param_bloom);
      if (callee_escape && arg_refs_param) {
        out->args_escape = true;
      }
      if (callee_mutates && arg_refs_param)
        out->args_mutated = true;
      if (callee_returns_alias && arg_refs_param)
        out->returns_alias = true;
      ny_collect_escape_expr(cg, arg, param_names, param_hashes, param_bloom,
                             local_names, local_hashes, local_bloom, out);
    }
    break;
  }
  case NY_E_UNARY:
    ny_collect_escape_expr(cg, e->as.unary.right, param_names, param_hashes,
                           param_bloom, local_names, local_hashes, local_bloom,
                           out);
    break;
  case NY_E_BINARY:
  case NY_E_LOGICAL:
    ny_collect_escape_expr(cg, e->as.binary.left, param_names, param_hashes,
                           param_bloom, local_names, local_hashes, local_bloom,
                           out);
    ny_collect_escape_expr(cg, e->as.binary.right, param_names, param_hashes,
                           param_bloom, local_names, local_hashes, local_bloom,
                           out);
    break;
  case NY_E_TERNARY:
    ny_collect_escape_expr(cg, e->as.ternary.cond, param_names, param_hashes,
                           param_bloom, local_names, local_hashes, local_bloom,
                           out);
    ny_collect_escape_expr(cg, e->as.ternary.true_expr, param_names,
                           param_hashes, param_bloom, local_names, local_hashes,
                           local_bloom, out);
    ny_collect_escape_expr(cg, e->as.ternary.false_expr, param_names,
                           param_hashes, param_bloom, local_names, local_hashes,
                           local_bloom, out);
    break;
  case NY_E_INDEX:
    ny_collect_escape_expr(cg, e->as.index.target, param_names, param_hashes,
                           param_bloom, local_names, local_hashes, local_bloom,
                           out);
    ny_collect_escape_expr(cg, e->as.index.start, param_names, param_hashes,
                           param_bloom, local_names, local_hashes, local_bloom,
                           out);
    ny_collect_escape_expr(cg, e->as.index.stop, param_names, param_hashes,
                           param_bloom, local_names, local_hashes, local_bloom,
                           out);
    ny_collect_escape_expr(cg, e->as.index.step, param_names, param_hashes,
                           param_bloom, local_names, local_hashes, local_bloom,
                           out);
    if (ny_expr_refs_params(e, param_names, param_hashes, param_bloom))
      out->args_escape = true;
    break;
  case NY_E_MEMBER:
    ny_collect_escape_expr(cg, e->as.member.target, param_names, param_hashes,
                           param_bloom, local_names, local_hashes, local_bloom,
                           out);
    if (ny_expr_refs_params(e, param_names, param_hashes, param_bloom))
      out->args_escape = true;
    break;
  case NY_E_PTR_TYPE:
  case NY_E_DEREF:
  case NY_E_TRY:
    if (e->kind == NY_E_PTR_TYPE) {
      ny_collect_escape_expr(cg, e->as.ptr_type.target, param_names,
                             param_hashes, param_bloom, local_names,
                             local_hashes, local_bloom, out);
    } else if (e->kind == NY_E_DEREF) {
      ny_collect_escape_expr(cg, e->as.deref.target, param_names, param_hashes,
                             param_bloom, local_names, local_hashes,
                             local_bloom, out);
    } else {
      ny_collect_escape_expr(cg, e->as.try_expr.target, param_names,
                             param_hashes, param_bloom, local_names,
                             local_hashes, local_bloom, out);
    }
    if (ny_expr_refs_params(e, param_names, param_hashes, param_bloom))
      out->args_escape = true;
    break;
  case NY_E_SIZEOF:
    if (!e->as.szof.is_type) {
      ny_collect_escape_expr(cg, e->as.szof.target, param_names, param_hashes,
                             param_bloom, local_names, local_hashes,
                             local_bloom, out);
    }
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; i++) {
      ny_collect_escape_expr(cg, e->as.list_like.data[i], param_names,
                             param_hashes, param_bloom, local_names,
                             local_hashes, local_bloom, out);
    }
    break;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; i++) {
      ny_collect_escape_expr(cg, e->as.dict.pairs.data[i].key, param_names,
                             param_hashes, param_bloom, local_names,
                             local_hashes, local_bloom, out);
      ny_collect_escape_expr(cg, e->as.dict.pairs.data[i].value, param_names,
                             param_hashes, param_bloom, local_names,
                             local_hashes, local_bloom, out);
    }
    break;
  case NY_E_MATCH:
    ny_collect_escape_expr(cg, e->as.match.test, param_names, param_hashes,
                           param_bloom, local_names, local_hashes, local_bloom,
                           out);
    for (size_t i = 0; i < e->as.match.arms.len; i++) {
      match_arm_t *arm = &e->as.match.arms.data[i];
      for (size_t j = 0; j < arm->patterns.len; j++) {
        ny_collect_escape_expr(cg, arm->patterns.data[j], param_names,
                               param_hashes, param_bloom, local_names,
                               local_hashes, local_bloom, out);
      }
      ny_collect_escape_expr(cg, arm->guard, param_names, param_hashes,
                             param_bloom, local_names, local_hashes,
                             local_bloom, out);
      ny_collect_escape_stmt(cg, arm->conseq, param_names, param_hashes,
                             param_bloom, local_names, local_hashes,
                             local_bloom, out);
    }
    ny_collect_escape_stmt(cg, e->as.match.default_conseq, param_names,
                           param_hashes, param_bloom, local_names, local_hashes,
                           local_bloom, out);
    break;
  case NY_E_COMPTIME:
    ny_collect_escape_stmt(cg, e->as.comptime_expr.body, param_names,
                           param_hashes, param_bloom, local_names, local_hashes,
                           local_bloom, out);
    if (ny_expr_refs_params(e, param_names, param_hashes, param_bloom))
      out->args_escape = true;
    break;
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; i++) {
      fstring_part_t *part = &e->as.fstring.parts.data[i];
      if (part->kind == NY_FSP_EXPR) {
        ny_collect_escape_expr(cg, part->as.e, param_names, param_hashes,
                               param_bloom, local_names, local_hashes,
                               local_bloom, out);
      }
    }
    break;
  case NY_E_ASM:
  case NY_E_EMBED:
  case NY_E_LAMBDA:
  case NY_E_FN:
    if (ny_expr_refs_params(e, param_names, param_hashes, param_bloom))
      out->args_escape = true;
    break;
  default:
    break;
  }
}

static void ny_collect_escape_stmt(
    codegen_t *cg, stmt_t *s, const assigned_name_list *param_names,
    const assigned_hash_list *param_hashes, const uint64_t param_bloom[4],
    assigned_name_list *local_names, assigned_hash_list *local_hashes,
    uint64_t local_bloom[4], ny_escape_summary_t *out) {
  if (!s || !out)
    return;
  switch (s->kind) {
  case NY_S_BLOCK: {
    size_t old_name_len = local_names->len;
    size_t old_hash_len = local_hashes->len;
    uint64_t old_bloom[4] = {local_bloom[0], local_bloom[1], local_bloom[2],
                             local_bloom[3]};
    for (size_t i = 0; i < s->as.block.body.len; i++) {
      ny_collect_escape_stmt(cg, s->as.block.body.data[i], param_names,
                             param_hashes, param_bloom, local_names,
                             local_hashes, local_bloom, out);
    }
    local_names->len = old_name_len;
    local_hashes->len = old_hash_len;
    local_bloom[0] = old_bloom[0];
    local_bloom[1] = old_bloom[1];
    local_bloom[2] = old_bloom[2];
    local_bloom[3] = old_bloom[3];
    break;
  }
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; i++) {
      expr_t *rhs = s->as.var.exprs.data[i];
      if (!s->as.var.is_decl) {
        bool rhs_refs_params =
            ny_expr_refs_params(rhs, param_names, param_hashes, param_bloom);
        bool nonlocal_target = false;
        for (size_t n = 0; n < s->as.var.names.len; n++) {
          const char *name = s->as.var.names.data[n];
          if (assigned_name_contains(param_names, param_hashes, param_bloom,
                                     name)) {
            out->args_mutated = true;
          }
          if (!assigned_name_contains(local_names, local_hashes, local_bloom,
                                      name)) {
            nonlocal_target = true;
          }
        }
        if (rhs_refs_params && nonlocal_target)
          out->args_escape = true;
      }
      ny_collect_escape_expr(cg, rhs, param_names, param_hashes, param_bloom,
                             local_names, local_hashes, local_bloom, out);
    }
    if (s->as.var.is_decl) {
      for (size_t i = 0; i < s->as.var.names.len; i++) {
        assigned_name_add(local_names, local_hashes, local_bloom,
                          s->as.var.names.data[i]);
      }
    }
    break;
  case NY_S_EXPR:
    ny_collect_escape_expr(cg, s->as.expr.expr, param_names, param_hashes,
                           param_bloom, local_names, local_hashes, local_bloom,
                           out);
    break;
  case NY_S_RETURN:
    if (ny_expr_refs_params(s->as.ret.value, param_names, param_hashes,
                            param_bloom)) {
      out->returns_alias = true;
    }
    ny_collect_escape_expr(cg, s->as.ret.value, param_names, param_hashes,
                           param_bloom, local_names, local_hashes, local_bloom,
                           out);
    break;
  case NY_S_IF:
    ny_collect_escape_expr(cg, s->as.iff.test, param_names, param_hashes,
                           param_bloom, local_names, local_hashes, local_bloom,
                           out);
    ny_collect_escape_stmt(cg, s->as.iff.conseq, param_names, param_hashes,
                           param_bloom, local_names, local_hashes, local_bloom,
                           out);
    ny_collect_escape_stmt(cg, s->as.iff.alt, param_names, param_hashes,
                           param_bloom, local_names, local_hashes, local_bloom,
                           out);
    break;
  case NY_S_WHILE:
    ny_collect_escape_expr(cg, s->as.whl.test, param_names, param_hashes,
                           param_bloom, local_names, local_hashes, local_bloom,
                           out);
    ny_collect_escape_stmt(cg, s->as.whl.body, param_names, param_hashes,
                           param_bloom, local_names, local_hashes, local_bloom,
                           out);
    break;
  case NY_S_FOR: {
    ny_collect_escape_expr(cg, s->as.fr.iterable, param_names, param_hashes,
                           param_bloom, local_names, local_hashes, local_bloom,
                           out);
    size_t old_name_len = local_names->len;
    size_t old_hash_len = local_hashes->len;
    uint64_t old_bloom[4] = {local_bloom[0], local_bloom[1], local_bloom[2],
                             local_bloom[3]};
    assigned_name_add(local_names, local_hashes, local_bloom,
                      s->as.fr.iter_var);
    ny_collect_escape_stmt(cg, s->as.fr.body, param_names, param_hashes,
                           param_bloom, local_names, local_hashes, local_bloom,
                           out);
    local_names->len = old_name_len;
    local_hashes->len = old_hash_len;
    local_bloom[0] = old_bloom[0];
    local_bloom[1] = old_bloom[1];
    local_bloom[2] = old_bloom[2];
    local_bloom[3] = old_bloom[3];
    break;
  }
  case NY_S_MATCH:
    ny_collect_escape_expr(cg, s->as.match.test, param_names, param_hashes,
                           param_bloom, local_names, local_hashes, local_bloom,
                           out);
    for (size_t i = 0; i < s->as.match.arms.len; i++) {
      match_arm_t *arm = &s->as.match.arms.data[i];
      for (size_t j = 0; j < arm->patterns.len; j++) {
        ny_collect_escape_expr(cg, arm->patterns.data[j], param_names,
                               param_hashes, param_bloom, local_names,
                               local_hashes, local_bloom, out);
      }
      ny_collect_escape_expr(cg, arm->guard, param_names, param_hashes,
                             param_bloom, local_names, local_hashes,
                             local_bloom, out);
      ny_collect_escape_stmt(cg, arm->conseq, param_names, param_hashes,
                             param_bloom, local_names, local_hashes,
                             local_bloom, out);
    }
    ny_collect_escape_stmt(cg, s->as.match.default_conseq, param_names,
                           param_hashes, param_bloom, local_names, local_hashes,
                           local_bloom, out);
    break;
  case NY_S_TRY:
    ny_collect_escape_stmt(cg, s->as.tr.body, param_names, param_hashes,
                           param_bloom, local_names, local_hashes, local_bloom,
                           out);
    ny_collect_escape_stmt(cg, s->as.tr.handler, param_names, param_hashes,
                           param_bloom, local_names, local_hashes, local_bloom,
                           out);
    break;
  case NY_S_DEFER:
    ny_collect_escape_stmt(cg, s->as.de.body, param_names, param_hashes,
                           param_bloom, local_names, local_hashes, local_bloom,
                           out);
    break;
  case NY_S_MACRO:
    for (size_t i = 0; i < s->as.macro.args.len; i++) {
      expr_t *arg = s->as.macro.args.data[i];
      ny_collect_escape_expr(cg, arg, param_names, param_hashes, param_bloom,
                             local_names, local_hashes, local_bloom, out);
      if (ny_expr_refs_params(arg, param_names, param_hashes, param_bloom))
        out->args_escape = true;
    }
    ny_collect_escape_stmt(cg, s->as.macro.body, param_names, param_hashes,
                           param_bloom, local_names, local_hashes, local_bloom,
                           out);
    break;
  default:
    break;
  }
}

static ny_escape_summary_t ny_func_decl_escape_summary(codegen_t *cg,
                                                       stmt_t *fn_stmt) {
  ny_escape_summary_t out = {0, 0, 0};
  if (!cg || !fn_stmt || fn_stmt->kind != NY_S_FUNC || !fn_stmt->as.fn.body) {
    out.args_escape = true;
    out.args_mutated = true;
    out.returns_alias = true;
    return out;
  }
  if (fn_stmt->as.fn.attr_thread || fn_stmt->as.fn.attr_naked) {
    out.args_escape = true;
    out.args_mutated = true;
    out.returns_alias = true;
    return out;
  }

  assigned_name_list param_names = {0};
  assigned_hash_list param_hashes = {0};
  uint64_t param_bloom[4] = {0, 0, 0, 0};

  assigned_name_list local_names = {0};
  assigned_hash_list local_hashes = {0};
  uint64_t local_bloom[4] = {0, 0, 0, 0};

  for (size_t i = 0; i < fn_stmt->as.fn.params.len; i++) {
    const char *pname = fn_stmt->as.fn.params.data[i].name;
    assigned_name_add(&param_names, &param_hashes, param_bloom, pname);
    assigned_name_add(&local_names, &local_hashes, local_bloom, pname);
  }

  ny_collect_escape_stmt(cg, fn_stmt->as.fn.body, &param_names, &param_hashes,
                         param_bloom, &local_names, &local_hashes, local_bloom,
                         &out);

  vec_free(&param_names);
  vec_free(&param_hashes);
  vec_free(&local_names);
  vec_free(&local_hashes);
  return out;
}

static const char *ny_module_prefix_stable(codegen_t *cg, const char *qname) {
  if (!cg || !qname || !*qname)
    return NULL;
  const char *dot = strrchr(qname, '.');
  if (!dot)
    return NULL;
  size_t len = (size_t)(dot - qname);
  return arena_strndup(cg->arena, qname, len);
}

typedef VEC(fun_sig *) ny_sig_ptr_list;
typedef VEC(size_t) ny_idx_list;

static int ny_sig_ptr_cmp(const void *a, const void *b) {
  const uintptr_t pa = (uintptr_t)*(fun_sig *const *)a;
  const uintptr_t pb = (uintptr_t)*(fun_sig *const *)b;
  if (pa < pb)
    return -1;
  if (pa > pb)
    return 1;
  return 0;
}

static void ny_sig_ptr_list_sort_unique(ny_sig_ptr_list *list) {
  if (!list || list->len <= 1)
    return;
  qsort(list->data, list->len, sizeof(list->data[0]), ny_sig_ptr_cmp);
  size_t out = 1;
  for (size_t i = 1; i < list->len; i++) {
    if (list->data[i] != list->data[out - 1])
      list->data[out++] = list->data[i];
  }
  list->len = out;
}

static int ny_size_t_cmp(const void *a, const void *b) {
  const size_t aa = *(const size_t *)a;
  const size_t bb = *(const size_t *)b;
  if (aa < bb)
    return -1;
  if (aa > bb)
    return 1;
  return 0;
}

static void ny_idx_list_sort_unique(ny_idx_list *list) {
  if (!list || list->len <= 1)
    return;
  qsort(list->data, list->len, sizeof(list->data[0]), ny_size_t_cmp);
  size_t out = 1;
  for (size_t i = 1; i < list->len; i++) {
    if (list->data[i] != list->data[out - 1])
      list->data[out++] = list->data[i];
  }
  list->len = out;
}

typedef struct ny_sig_idx_map_entry {
  const fun_sig *sig;
  size_t idx;
} ny_sig_idx_map_entry;

static int ny_sig_idx_map_cmp(const void *a, const void *b) {
  const uintptr_t pa = (uintptr_t)((const ny_sig_idx_map_entry *)a)->sig;
  const uintptr_t pb = (uintptr_t)((const ny_sig_idx_map_entry *)b)->sig;
  if (pa < pb)
    return -1;
  if (pa > pb)
    return 1;
  return 0;
}

static ny_sig_idx_map_entry *ny_build_fun_sig_index_map(fun_sig *sigs,
                                                        size_t len) {
  if (!sigs || len == 0)
    return NULL;
  ny_sig_idx_map_entry *map = malloc(sizeof(*map) * len);
  if (!map)
    return NULL;
  for (size_t i = 0; i < len; i++) {
    map[i].sig = &sigs[i];
    map[i].idx = i;
  }
  qsort(map, len, sizeof(map[0]), ny_sig_idx_map_cmp);
  return map;
}

static long ny_lookup_fun_sig_index_map(const ny_sig_idx_map_entry *map,
                                        size_t len, const fun_sig *needle) {
  if (!map || len == 0 || !needle)
    return -1;
  const uintptr_t target = (uintptr_t)needle;
  size_t lo = 0;
  size_t hi = len;
  while (lo < hi) {
    size_t mid = lo + ((hi - lo) >> 1);
    uintptr_t cur = (uintptr_t)map[mid].sig;
    if (cur < target)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo < len && map[lo].sig == needle)
    return (long)map[lo].idx;
  return -1;
}

static long ny_fun_sig_index_linear(fun_sig *sigs, size_t len,
                                    const fun_sig *needle) {
  if (!sigs || !needle)
    return -1;
  for (size_t i = 0; i < len; i++) {
    if (&sigs[i] == needle)
      return (long)i;
  }
  return -1;
}

static void ny_collect_calls_expr(codegen_t *cg, expr_t *e,
                                  assigned_name_list *local_names,
                                  assigned_hash_list *local_hashes,
                                  uint64_t local_bloom[4],
                                  ny_sig_ptr_list *out_calls);

static void ny_collect_calls_stmt(codegen_t *cg, stmt_t *s,
                                  assigned_name_list *local_names,
                                  assigned_hash_list *local_hashes,
                                  uint64_t local_bloom[4],
                                  ny_sig_ptr_list *out_calls);

static void ny_collect_calls_expr(codegen_t *cg, expr_t *e,
                                  assigned_name_list *local_names,
                                  assigned_hash_list *local_hashes,
                                  uint64_t local_bloom[4],
                                  ny_sig_ptr_list *out_calls) {
  if (!e || !out_calls)
    return;
  switch (e->kind) {
  case NY_E_UNARY:
    ny_collect_calls_expr(cg, e->as.unary.right, local_names, local_hashes,
                          local_bloom, out_calls);
    break;
  case NY_E_BINARY:
  case NY_E_LOGICAL:
    ny_collect_calls_expr(cg, e->as.binary.left, local_names, local_hashes,
                          local_bloom, out_calls);
    ny_collect_calls_expr(cg, e->as.binary.right, local_names, local_hashes,
                          local_bloom, out_calls);
    break;
  case NY_E_TERNARY:
    ny_collect_calls_expr(cg, e->as.ternary.cond, local_names, local_hashes,
                          local_bloom, out_calls);
    ny_collect_calls_expr(cg, e->as.ternary.true_expr, local_names,
                          local_hashes, local_bloom, out_calls);
    ny_collect_calls_expr(cg, e->as.ternary.false_expr, local_names,
                          local_hashes, local_bloom, out_calls);
    break;
  case NY_E_CALL: {
    ny_collect_calls_expr(cg, e->as.call.callee, local_names, local_hashes,
                          local_bloom, out_calls);
    for (size_t i = 0; i < e->as.call.args.len; i++) {
      ny_collect_calls_expr(cg, e->as.call.args.data[i].val, local_names,
                            local_hashes, local_bloom, out_calls);
    }
    fun_sig *sig = ny_purity_resolve_call_sig(cg, &e->as.call, local_names,
                                              local_hashes, local_bloom);
    if (sig && sig->stmt_t && sig->stmt_t->kind == NY_S_FUNC && !sig->is_extern)
      vec_push(out_calls, sig);
    break;
  }
  case NY_E_MEMCALL: {
    ny_collect_calls_expr(cg, e->as.memcall.target, local_names, local_hashes,
                          local_bloom, out_calls);
    for (size_t i = 0; i < e->as.memcall.args.len; i++) {
      ny_collect_calls_expr(cg, e->as.memcall.args.data[i].val, local_names,
                            local_hashes, local_bloom, out_calls);
    }
    fun_sig *sig = ny_purity_resolve_memcall_sig(
        cg, &e->as.memcall, local_names, local_hashes, local_bloom);
    if (sig && sig->stmt_t && sig->stmt_t->kind == NY_S_FUNC && !sig->is_extern)
      vec_push(out_calls, sig);
    break;
  }
  case NY_E_INDEX:
    ny_collect_calls_expr(cg, e->as.index.target, local_names, local_hashes,
                          local_bloom, out_calls);
    ny_collect_calls_expr(cg, e->as.index.start, local_names, local_hashes,
                          local_bloom, out_calls);
    ny_collect_calls_expr(cg, e->as.index.stop, local_names, local_hashes,
                          local_bloom, out_calls);
    ny_collect_calls_expr(cg, e->as.index.step, local_names, local_hashes,
                          local_bloom, out_calls);
    break;
  case NY_E_MEMBER:
    ny_collect_calls_expr(cg, e->as.member.target, local_names, local_hashes,
                          local_bloom, out_calls);
    break;
  case NY_E_PTR_TYPE:
    ny_collect_calls_expr(cg, e->as.ptr_type.target, local_names, local_hashes,
                          local_bloom, out_calls);
    break;
  case NY_E_DEREF:
    ny_collect_calls_expr(cg, e->as.deref.target, local_names, local_hashes,
                          local_bloom, out_calls);
    break;
  case NY_E_SIZEOF:
    if (!e->as.szof.is_type)
      ny_collect_calls_expr(cg, e->as.szof.target, local_names, local_hashes,
                            local_bloom, out_calls);
    break;
  case NY_E_TRY:
    ny_collect_calls_expr(cg, e->as.try_expr.target, local_names, local_hashes,
                          local_bloom, out_calls);
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; i++) {
      ny_collect_calls_expr(cg, e->as.list_like.data[i], local_names,
                            local_hashes, local_bloom, out_calls);
    }
    break;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; i++) {
      ny_collect_calls_expr(cg, e->as.dict.pairs.data[i].key, local_names,
                            local_hashes, local_bloom, out_calls);
      ny_collect_calls_expr(cg, e->as.dict.pairs.data[i].value, local_names,
                            local_hashes, local_bloom, out_calls);
    }
    break;
  case NY_E_MATCH:
    ny_collect_calls_expr(cg, e->as.match.test, local_names, local_hashes,
                          local_bloom, out_calls);
    for (size_t i = 0; i < e->as.match.arms.len; i++) {
      match_arm_t *arm = &e->as.match.arms.data[i];
      for (size_t j = 0; j < arm->patterns.len; j++) {
        ny_collect_calls_expr(cg, arm->patterns.data[j], local_names,
                              local_hashes, local_bloom, out_calls);
      }
      ny_collect_calls_expr(cg, arm->guard, local_names, local_hashes,
                            local_bloom, out_calls);
      ny_collect_calls_stmt(cg, arm->conseq, local_names, local_hashes,
                            local_bloom, out_calls);
    }
    ny_collect_calls_stmt(cg, e->as.match.default_conseq, local_names,
                          local_hashes, local_bloom, out_calls);
    break;
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; i++) {
      fstring_part_t *part = &e->as.fstring.parts.data[i];
      if (part->kind == NY_FSP_EXPR) {
        ny_collect_calls_expr(cg, part->as.e, local_names, local_hashes,
                              local_bloom, out_calls);
      }
    }
    break;
  default:
    break;
  }
}

static void ny_collect_calls_stmt(codegen_t *cg, stmt_t *s,
                                  assigned_name_list *local_names,
                                  assigned_hash_list *local_hashes,
                                  uint64_t local_bloom[4],
                                  ny_sig_ptr_list *out_calls) {
  if (!s || !out_calls)
    return;
  switch (s->kind) {
  case NY_S_BLOCK: {
    size_t old_name_len = local_names->len;
    size_t old_hash_len = local_hashes->len;
    uint64_t old_bloom[4] = {local_bloom[0], local_bloom[1], local_bloom[2],
                             local_bloom[3]};
    for (size_t i = 0; i < s->as.block.body.len; i++) {
      ny_collect_calls_stmt(cg, s->as.block.body.data[i], local_names,
                            local_hashes, local_bloom, out_calls);
    }
    local_names->len = old_name_len;
    local_hashes->len = old_hash_len;
    local_bloom[0] = old_bloom[0];
    local_bloom[1] = old_bloom[1];
    local_bloom[2] = old_bloom[2];
    local_bloom[3] = old_bloom[3];
    break;
  }
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; i++) {
      ny_collect_calls_expr(cg, s->as.var.exprs.data[i], local_names,
                            local_hashes, local_bloom, out_calls);
    }
    if (s->as.var.is_decl) {
      for (size_t i = 0; i < s->as.var.names.len; i++) {
        assigned_name_add(local_names, local_hashes, local_bloom,
                          s->as.var.names.data[i]);
      }
    }
    break;
  case NY_S_EXPR:
    ny_collect_calls_expr(cg, s->as.expr.expr, local_names, local_hashes,
                          local_bloom, out_calls);
    break;
  case NY_S_RETURN:
    ny_collect_calls_expr(cg, s->as.ret.value, local_names, local_hashes,
                          local_bloom, out_calls);
    break;
  case NY_S_IF:
    ny_collect_calls_expr(cg, s->as.iff.test, local_names, local_hashes,
                          local_bloom, out_calls);
    ny_collect_calls_stmt(cg, s->as.iff.conseq, local_names, local_hashes,
                          local_bloom, out_calls);
    ny_collect_calls_stmt(cg, s->as.iff.alt, local_names, local_hashes,
                          local_bloom, out_calls);
    break;
  case NY_S_WHILE:
    ny_collect_calls_expr(cg, s->as.whl.test, local_names, local_hashes,
                          local_bloom, out_calls);
    ny_collect_calls_stmt(cg, s->as.whl.body, local_names, local_hashes,
                          local_bloom, out_calls);
    break;
  case NY_S_FOR: {
    ny_collect_calls_expr(cg, s->as.fr.iterable, local_names, local_hashes,
                          local_bloom, out_calls);
    size_t old_name_len = local_names->len;
    size_t old_hash_len = local_hashes->len;
    uint64_t old_bloom[4] = {local_bloom[0], local_bloom[1], local_bloom[2],
                             local_bloom[3]};
    assigned_name_add(local_names, local_hashes, local_bloom,
                      s->as.fr.iter_var);
    ny_collect_calls_stmt(cg, s->as.fr.body, local_names, local_hashes,
                          local_bloom, out_calls);
    local_names->len = old_name_len;
    local_hashes->len = old_hash_len;
    local_bloom[0] = old_bloom[0];
    local_bloom[1] = old_bloom[1];
    local_bloom[2] = old_bloom[2];
    local_bloom[3] = old_bloom[3];
    break;
  }
  case NY_S_MATCH:
    ny_collect_calls_expr(cg, s->as.match.test, local_names, local_hashes,
                          local_bloom, out_calls);
    for (size_t i = 0; i < s->as.match.arms.len; i++) {
      match_arm_t *arm = &s->as.match.arms.data[i];
      for (size_t j = 0; j < arm->patterns.len; j++) {
        ny_collect_calls_expr(cg, arm->patterns.data[j], local_names,
                              local_hashes, local_bloom, out_calls);
      }
      ny_collect_calls_expr(cg, arm->guard, local_names, local_hashes,
                            local_bloom, out_calls);
      ny_collect_calls_stmt(cg, arm->conseq, local_names, local_hashes,
                            local_bloom, out_calls);
    }
    ny_collect_calls_stmt(cg, s->as.match.default_conseq, local_names,
                          local_hashes, local_bloom, out_calls);
    break;
  case NY_S_MACRO:
    for (size_t i = 0; i < s->as.macro.args.len; i++) {
      ny_collect_calls_expr(cg, s->as.macro.args.data[i], local_names,
                            local_hashes, local_bloom, out_calls);
    }
    ny_collect_calls_stmt(cg, s->as.macro.body, local_names, local_hashes,
                          local_bloom, out_calls);
    break;
  default:
    break;
  }
}

static void ny_collect_direct_calls_for_sig(codegen_t *cg, fun_sig *sig,
                                            ny_sig_ptr_list *out_calls) {
  if (!cg || !sig || !out_calls || !sig->stmt_t ||
      sig->stmt_t->kind != NY_S_FUNC || !sig->stmt_t->as.fn.body) {
    return;
  }
  assigned_name_list local_names = {0};
  assigned_hash_list local_hashes = {0};
  uint64_t local_bloom[4] = {0, 0, 0, 0};
  for (size_t i = 0; i < sig->stmt_t->as.fn.params.len; i++) {
    assigned_name_add(&local_names, &local_hashes, local_bloom,
                      sig->stmt_t->as.fn.params.data[i].name);
  }
  ny_collect_calls_stmt(cg, sig->stmt_t->as.fn.body, &local_names,
                        &local_hashes, local_bloom, out_calls);
  ny_sig_ptr_list_sort_unique(out_calls);
  vec_free(&local_names);
  vec_free(&local_hashes);
}

static bool ny_idx_list_contains(const ny_idx_list *list, size_t needle) {
  if (!list || list->len == 0)
    return false;
  size_t lo = 0;
  size_t hi = list->len;
  while (lo < hi) {
    size_t mid = lo + ((hi - lo) >> 1);
    size_t cur = list->data[mid];
    if (cur < needle)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo < list->len && list->data[lo] == needle;
}

typedef struct ny_recursion_scc_ctx {
  ny_idx_list *edges;
  const bool *enabled;
  size_t node_count;
  size_t next_index;
  size_t *indices;
  size_t *lowlink;
  size_t *stack;
  size_t *component;
  size_t stack_len;
  bool *on_stack;
  bool *recursive_flags;
  size_t *recursive_count;
} ny_recursion_scc_ctx;

static void ny_mark_recursive_components_dfs(ny_recursion_scc_ctx *ctx,
                                             size_t node_idx) {
  ctx->indices[node_idx] = ctx->next_index;
  ctx->lowlink[node_idx] = ctx->next_index;
  ctx->next_index++;

  ctx->stack[ctx->stack_len++] = node_idx;
  ctx->on_stack[node_idx] = true;

  ny_idx_list *adj = &ctx->edges[node_idx];
  for (size_t i = 0; i < adj->len; i++) {
    size_t next = adj->data[i];
    if (next >= ctx->node_count || !ctx->enabled[next])
      continue;
    if (ctx->indices[next] == SIZE_MAX) {
      ny_mark_recursive_components_dfs(ctx, next);
      if (ctx->lowlink[next] < ctx->lowlink[node_idx])
        ctx->lowlink[node_idx] = ctx->lowlink[next];
    } else if (ctx->on_stack[next] &&
               ctx->indices[next] < ctx->lowlink[node_idx]) {
      ctx->lowlink[node_idx] = ctx->indices[next];
    }
  }

  if (ctx->lowlink[node_idx] != ctx->indices[node_idx])
    return;

  size_t component_len = 0;
  while (ctx->stack_len > 0) {
    size_t member = ctx->stack[--ctx->stack_len];
    ctx->on_stack[member] = false;
    ctx->component[component_len++] = member;
    if (member == node_idx)
      break;
  }

  bool recursive_component =
      component_len > 1 ||
      ny_idx_list_contains(&ctx->edges[node_idx], node_idx);
  if (!recursive_component)
    return;

  for (size_t i = 0; i < component_len; i++) {
    size_t member = ctx->component[i];
    if (!ctx->recursive_flags[member]) {
      ctx->recursive_flags[member] = true;
      (*ctx->recursive_count)++;
    }
  }
}

static bool ny_mark_recursive_components(ny_idx_list *edges,
                                         const bool *enabled, size_t node_count,
                                         bool *recursive_flags,
                                         size_t *recursive_count) {
  if (!edges || !enabled || !recursive_flags || !recursive_count)
    return false;
  size_t *indices = malloc(sizeof(*indices) * node_count);
  size_t *lowlink = malloc(sizeof(*lowlink) * node_count);
  size_t *stack = malloc(sizeof(*stack) * node_count);
  size_t *component = malloc(sizeof(*component) * node_count);
  bool *on_stack = calloc(node_count, sizeof(*on_stack));
  if (!indices || !lowlink || !stack || !component || !on_stack) {
    free(indices);
    free(lowlink);
    free(stack);
    free(component);
    free(on_stack);
    return false;
  }
  for (size_t i = 0; i < node_count; i++)
    indices[i] = SIZE_MAX;

  ny_recursion_scc_ctx ctx = {
      .edges = edges,
      .enabled = enabled,
      .node_count = node_count,
      .next_index = 0,
      .indices = indices,
      .lowlink = lowlink,
      .stack = stack,
      .component = component,
      .stack_len = 0,
      .on_stack = on_stack,
      .recursive_flags = recursive_flags,
      .recursive_count = recursive_count,
  };
  for (size_t i = 0; i < node_count; i++) {
    if (!enabled[i] || indices[i] != SIZE_MAX)
      continue;
    ny_mark_recursive_components_dfs(&ctx, i);
  }

  free(indices);
  free(lowlink);
  free(stack);
  free(component);
  free(on_stack);
  return true;
}

static bool ny_func_decl_is_pure(codegen_t *cg, stmt_t *fn_stmt) {
  if (!cg || !fn_stmt || fn_stmt->kind != NY_S_FUNC || !fn_stmt->as.fn.body)
    return false;
  if (fn_stmt->as.fn.attr_thread || fn_stmt->as.fn.attr_naked)
    return false;

  assigned_name_list local_names = {0};
  assigned_hash_list local_hashes = {0};
  uint64_t local_bloom[4] = {0, 0, 0, 0};
  for (size_t i = 0; i < fn_stmt->as.fn.params.len; i++) {
    assigned_name_add(&local_names, &local_hashes, local_bloom,
                      fn_stmt->as.fn.params.data[i].name);
  }
  bool pure = ny_stmt_is_pure(cg, fn_stmt->as.fn.body, &local_names,
                              &local_hashes, local_bloom);
  vec_free(&local_names);
  vec_free(&local_hashes);
  return pure;
}

static bool ny_func_decl_is_memo_safe(codegen_t *cg, stmt_t *fn_stmt) {
  if (!cg || !fn_stmt || fn_stmt->kind != NY_S_FUNC || !fn_stmt->as.fn.body)
    return false;
  if (fn_stmt->as.fn.attr_thread || fn_stmt->as.fn.attr_naked)
    return false;

  assigned_name_list local_names = {0};
  assigned_hash_list local_hashes = {0};
  uint64_t local_bloom[4] = {0, 0, 0, 0};
  for (size_t i = 0; i < fn_stmt->as.fn.params.len; i++) {
    assigned_name_add(&local_names, &local_hashes, local_bloom,
                      fn_stmt->as.fn.params.data[i].name);
  }
  bool safe = ny_stmt_is_memo_safe(cg, fn_stmt->as.fn.body, &local_names,
                                   &local_hashes, local_bloom);
  vec_free(&local_names);
  vec_free(&local_hashes);
  return safe;
}

typedef enum ny_infer_pass_kind_t {
  NY_INFER_PASS_PURE = 0,
  NY_INFER_PASS_EFFECTS = 1,
  NY_INFER_PASS_MEMO_SAFE = 2,
  NY_INFER_PASS_ESCAPE = 3,
} ny_infer_pass_kind_t;

#define NY_FOREACH_FUNC_SIG(cg, sig_var)                                       \
  for (size_t _ny_i = 0; _ny_i < (cg)->fun_sigs.len; _ny_i++)                  \
    for (fun_sig *sig_var = &(cg)->fun_sigs.data[_ny_i]; sig_var != NULL;      \
         sig_var = NULL)                                                       \
      if (!sig_var->stmt_t || sig_var->stmt_t->kind != NY_S_FUNC) {            \
      } else

#define NY_FOREACH_NON_STD_FUNC_SIG(cg, sig_var)                               \
  NY_FOREACH_FUNC_SIG(cg, sig_var)                                             \
  if (ny_is_std_qname(sig_var->name)) {                                        \
  } else

#define NY_FOREACH_POLICY_SIG(cg, include_std_flag, sig_var)                   \
  for (size_t _ny_i = 0; _ny_i < (cg)->fun_sigs.len; _ny_i++)                  \
    for (fun_sig *sig_var = &(cg)->fun_sigs.data[_ny_i]; sig_var != NULL;      \
         sig_var = NULL)                                                       \
      if (!sig_var->stmt_t || sig_var->stmt_t->kind != NY_S_FUNC ||            \
          (!(include_std_flag) && ny_is_std_qname(sig_var->name))) {           \
      } else

static bool ny_apply_infer_pass_to_sig(codegen_t *cg, fun_sig *sig,
                                       sema_func_t *sema,
                                       ny_infer_pass_kind_t pass) {
  if (!cg || !sig || !sema || !sig->stmt_t || sig->stmt_t->kind != NY_S_FUNC)
    return false;

  bool changed = false;
  const char *saved_mod = cg->current_module_name;
  cg->current_module_name = ny_module_prefix_stable(cg, sig->name);

  switch (pass) {
  case NY_INFER_PASS_PURE: {
    bool pure = ny_func_decl_is_pure(cg, sig->stmt_t);
    if (sema->is_pure != pure) {
      sema->is_pure = pure;
      changed = true;
    }
    break;
  }
  case NY_INFER_PASS_EFFECTS: {
    uint32_t effects = ny_func_decl_effects(cg, sig->stmt_t);
    if (!sema->effects_known || sema->effects != effects) {
      sema->effects_known = true;
      sema->effects = effects;
      changed = true;
    }
    break;
  }
  case NY_INFER_PASS_MEMO_SAFE: {
    bool memo_safe = ny_func_decl_is_memo_safe(cg, sig->stmt_t);
    if (sema->is_memo_safe != memo_safe) {
      sema->is_memo_safe = memo_safe;
      changed = true;
    }
    break;
  }
  case NY_INFER_PASS_ESCAPE: {
    ny_escape_summary_t esc = ny_func_decl_escape_summary(cg, sig->stmt_t);
    if (sema->args_escape != esc.args_escape) {
      sema->args_escape = esc.args_escape;
      changed = true;
    }
    if (sema->args_mutated != esc.args_mutated) {
      sema->args_mutated = esc.args_mutated;
      changed = true;
    }
    if (sema->returns_alias != esc.returns_alias) {
      sema->returns_alias = esc.returns_alias;
      changed = true;
    }
    if (!sema->escape_known) {
      sema->escape_known = true;
      changed = true;
    }
    break;
  }
  default:
    break;
  }

  cg->current_module_name = saved_mod;
  return changed;
}

static void ny_run_infer_fixed_point(codegen_t *cg, int max_iters,
                                     ny_infer_pass_kind_t pass) {
  if (!cg || max_iters <= 0)
    return;
  for (int iter = 0; iter < max_iters; iter++) {
    bool changed = false;
    NY_FOREACH_NON_STD_FUNC_SIG(cg, sig) {
      sema_func_t *sema = (sema_func_t *)sig->stmt_t->sema;
      if (!sema)
        continue;
      if (ny_apply_infer_pass_to_sig(cg, sig, sema, pass))
        changed = true;
    }
    if (!changed)
      break;
  }
}

void infer_pure_functions(codegen_t *cg) {
  if (!cg)
    return;

  bool run_effect_infer = cg->auto_purity_infer;
  if (!run_effect_infer) {
    NY_FOREACH_FUNC_SIG(cg, sig) {
      if (sig->stmt_t->as.fn.effect_contract_known) {
        run_effect_infer = true;
        break;
      }
    }
  }
  if (!run_effect_infer)
    return;

  bool has_functions = false;
  NY_FOREACH_FUNC_SIG(cg, sig) {
    sig->is_recursive = false;
    sig->is_pure = false;
    sig->is_memo_safe = false;
    sig->args_escape = true;
    sig->args_mutated = true;
    sig->returns_alias = true;
    sig->effects = NY_FX_ALL;
    sig->effects_known = false;
    if (ny_is_std_qname(sig->name))
      continue;
    has_functions = true;
    sema_func_t *sema = (sema_func_t *)sig->stmt_t->sema;
    if (!sema)
      continue;
    sema->purity_known = true;
    sema->is_pure = true;
    sema->memo_known = true;
    sema->is_memo_safe = true;
    sema->escape_known = true;
    sema->args_escape = false;
    sema->args_mutated = false;
    sema->returns_alias = false;
    sema->effects_known = true;
    sema->effects = NY_FX_NONE;
    sema->is_recursive = false;
    sig->is_pure = true;
    sig->is_memo_safe = true;
    sig->args_escape = false;
    sig->args_mutated = false;
    sig->returns_alias = false;
    sig->effects = NY_FX_NONE;
    sig->effects_known = true;
  }
  if (!has_functions)
    return;

  const int max_iters = 64;
  ny_run_infer_fixed_point(cg, max_iters, NY_INFER_PASS_PURE);
  ny_run_infer_fixed_point(cg, max_iters, NY_INFER_PASS_EFFECTS);

  if (cg->auto_memoize_impure) {
    ny_run_infer_fixed_point(cg, max_iters, NY_INFER_PASS_MEMO_SAFE);
  }

  ny_run_infer_fixed_point(cg, max_iters, NY_INFER_PASS_ESCAPE);

  size_t pure_count = 0;
  size_t memo_safe_count = 0;
  size_t args_escape_count = 0;
  size_t args_mutated_count = 0;
  size_t returns_alias_count = 0;
  size_t effects_none_count = 0;
  size_t effects_io_count = 0;
  size_t effects_alloc_count = 0;
  size_t effects_ffi_count = 0;
  size_t effects_thread_count = 0;
  size_t effects_unknown_count = 0;
  size_t effect_contract_violations = 0;
  size_t effect_contract_unknown = 0;
  size_t total_count = 0;
  size_t recursive_count = 0;
  size_t effect_policy_violations = 0;
  size_t effect_policy_unknown = 0;
  size_t alias_policy_violations = 0;
  size_t alias_policy_unknown = 0;
  NY_FOREACH_FUNC_SIG(cg, sig) {
    if (ny_is_std_qname(sig->name)) {
      sig->is_pure = false;
      sig->is_memo_safe = false;
      sig->args_escape = true;
      sig->args_mutated = true;
      sig->returns_alias = true;
      sig->effects = NY_FX_ALL;
      sig->effects_known = true;
      continue;
    }
    sema_func_t *sema = (sema_func_t *)sig->stmt_t->sema;
    bool is_pure = sema && sema->purity_known && sema->is_pure;
    bool is_memo_safe = sema && sema->memo_known && sema->is_memo_safe;
    bool args_escape = !sema || !sema->escape_known || sema->args_escape;
    bool args_mutated = !sema || !sema->escape_known || sema->args_mutated;
    bool returns_alias = !sema || !sema->escape_known || sema->returns_alias;
    bool effects_known = sema && sema->effects_known;
    uint32_t effects = effects_known ? sema->effects : NY_FX_ALL;
    sig->is_pure = is_pure;
    sig->is_memo_safe = is_memo_safe;
    sig->args_escape = args_escape;
    sig->args_mutated = args_mutated;
    sig->returns_alias = returns_alias;
    sig->effects = effects;
    sig->effects_known = effects_known;
    if (sig->stmt_t->as.fn.effect_contract_known) {
      uint32_t declared_mask = sig->stmt_t->as.fn.effect_contract_mask;
      if (!effects_known) {
        ny_diag_error(
            sig->stmt_t->tok,
            "effect contract violation in '%s': inferred effects are unknown",
            sig->name ? sig->name : "<anon>");
        cg->had_error = 1;
        effect_contract_unknown++;
      } else {
        uint32_t undeclared = effects & ~declared_mask;
        if (undeclared != 0) {
          char declared_buf[96];
          char inferred_buf[96];
          ny_effect_mask_to_buf(declared_mask, declared_buf,
                                sizeof(declared_buf));
          ny_effect_mask_to_buf(effects, inferred_buf, sizeof(inferred_buf));
          if (sig->stmt_t->as.fn.attr_pure) {
            ny_diag_error(sig->stmt_t->tok,
                          "effect contract violation in '%s': declared @pure "
                          "but inferred effects=%s",
                          sig->name ? sig->name : "<anon>", inferred_buf);
          } else {
            ny_diag_error(sig->stmt_t->tok,
                          "effect contract violation in '%s': declared "
                          "@effects(%s) but inferred effects=%s",
                          sig->name ? sig->name : "<anon>", declared_buf,
                          inferred_buf);
          }
          cg->had_error = 1;
          effect_contract_violations++;
        }
      }
    }
    total_count++;
    if (is_pure)
      pure_count++;
    if (is_memo_safe)
      memo_safe_count++;
    if (args_escape)
      args_escape_count++;
    if (args_mutated)
      args_mutated_count++;
    if (returns_alias)
      returns_alias_count++;
    if (!effects_known) {
      effects_unknown_count++;
    } else {
      if (effects == NY_FX_NONE)
        effects_none_count++;
      if ((effects & NY_FX_IO) != 0)
        effects_io_count++;
      if ((effects & NY_FX_ALLOC) != 0)
        effects_alloc_count++;
      if ((effects & NY_FX_FFI) != 0)
        effects_ffi_count++;
      if ((effects & NY_FX_THREAD) != 0)
        effects_thread_count++;
    }
  }

  bool need_recursion = cg->auto_memoize || cg->auto_memoize_impure ||
                        ny_env_enabled("NYTRIX_PURITY_RECURSION");
  if (need_recursion) {
    ny_idx_list *edges = calloc(cg->fun_sigs.len, sizeof(*edges));
    bool *is_codegen_fn = calloc(cg->fun_sigs.len, sizeof(*is_codegen_fn));
    bool *recursive_flags = calloc(cg->fun_sigs.len, sizeof(*recursive_flags));
    ny_sig_idx_map_entry *sig_idx_map =
        ny_build_fun_sig_index_map(cg->fun_sigs.data, cg->fun_sigs.len);
    if (edges && is_codegen_fn && recursive_flags) {
      const char *saved_mod_for_calls = cg->current_module_name;
      for (size_t i = 0; i < cg->fun_sigs.len; i++) {
        fun_sig *sig = &cg->fun_sigs.data[i];
        if (!sig->stmt_t || sig->stmt_t->kind != NY_S_FUNC || sig->is_extern ||
            ny_is_std_qname(sig->name))
          continue;
        is_codegen_fn[i] = true;
        const char *module_name = ny_module_prefix_stable(cg, sig->name);
        cg->current_module_name = module_name;
        ny_sig_ptr_list direct_calls = {0};
        ny_collect_direct_calls_for_sig(cg, sig, &direct_calls);
        cg->current_module_name = saved_mod_for_calls;
        for (size_t j = 0; j < direct_calls.len; j++) {
          long callee_idx =
              sig_idx_map
                  ? ny_lookup_fun_sig_index_map(sig_idx_map, cg->fun_sigs.len,
                                                direct_calls.data[j])
                  : ny_fun_sig_index_linear(cg->fun_sigs.data, cg->fun_sigs.len,
                                            direct_calls.data[j]);
          if (callee_idx >= 0)
            vec_push(&edges[i], (size_t)callee_idx);
        }
        ny_idx_list_sort_unique(&edges[i]);
        vec_free(&direct_calls);
      }
      cg->current_module_name = saved_mod_for_calls;
      ny_mark_recursive_components(edges, is_codegen_fn, cg->fun_sigs.len,
                                   recursive_flags, &recursive_count);

      for (size_t i = 0; i < cg->fun_sigs.len; i++) {
        fun_sig *sig = &cg->fun_sigs.data[i];
        if (!is_codegen_fn[i]) {
          if (sig)
            sig->is_recursive = false;
          sema_func_t *sema =
              sig && sig->stmt_t ? (sema_func_t *)sig->stmt_t->sema : NULL;
          if (sema)
            sema->is_recursive = false;
          continue;
        }
        bool is_recursive = recursive_flags[i];
        sig->is_recursive = is_recursive;
        sema_func_t *sema =
            sig->stmt_t ? (sema_func_t *)sig->stmt_t->sema : NULL;
        if (sema)
          sema->is_recursive = is_recursive;
      }
    }
    if (edges) {
      for (size_t i = 0; i < cg->fun_sigs.len; i++)
        vec_free(&edges[i]);
    }
    free(edges);
    free(is_codegen_fn);
    free(recursive_flags);
    free(sig_idx_map);
  }

  bool require_pure_effects = ny_env_enabled("NYTRIX_EFFECT_REQUIRE_PURE");
  bool require_known_effects = ny_env_enabled("NYTRIX_EFFECT_REQUIRE_KNOWN");
  bool include_std_in_policy =
      ny_env_enabled("NYTRIX_EFFECT_POLICY_INCLUDE_STD");
  const char *forbid_raw = getenv("NYTRIX_EFFECT_FORBID");
  bool forbid_has_tokens = false;
  uint32_t forbid_effects = NY_FX_NONE;
  if (forbid_raw && *forbid_raw)
    forbid_effects =
        ny_effect_mask_parse_env(cg, forbid_raw, &forbid_has_tokens);
  if (require_pure_effects)
    forbid_effects |= NY_FX_ALL;

  bool effect_policy_active =
      require_pure_effects || require_known_effects || forbid_has_tokens;
  if (effect_policy_active) {
    char forbid_buf[96];
    ny_effect_mask_to_buf(forbid_effects, forbid_buf, sizeof(forbid_buf));
    NY_FOREACH_POLICY_SIG(cg, include_std_in_policy, sig) {
      if (!sig->effects_known) {
        if (require_known_effects || forbid_effects != NY_FX_NONE) {
          ny_diag_error(sig->stmt_t->tok,
                        "effect policy violation in '%s': effects are unknown",
                        sig->name ? sig->name : "<anon>");
          cg->had_error = 1;
          effect_policy_unknown++;
        }
        continue;
      }

      if (forbid_effects != NY_FX_NONE &&
          (sig->effects & forbid_effects) != 0) {
        char fx_buf[96];
        ny_effect_mask_to_buf(sig->effects, fx_buf, sizeof(fx_buf));
        ny_diag_error(
            sig->stmt_t->tok,
            "effect policy violation in '%s': effects=%s forbidden=%s",
            sig->name ? sig->name : "<anon>", fx_buf, forbid_buf);
        cg->had_error = 1;
        effect_policy_violations++;
      }
    }
  }

  bool require_known_alias = ny_env_enabled("NYTRIX_ALIAS_REQUIRE_KNOWN");
  bool require_no_alias_escape =
      ny_env_enabled("NYTRIX_ALIAS_REQUIRE_NO_ESCAPE");
  bool include_std_in_alias_policy =
      ny_env_enabled("NYTRIX_ALIAS_POLICY_INCLUDE_STD");
  bool alias_policy_active = require_known_alias || require_no_alias_escape;
  if (alias_policy_active) {
    NY_FOREACH_POLICY_SIG(cg, include_std_in_alias_policy, sig) {
      sema_func_t *sema = (sema_func_t *)sig->stmt_t->sema;
      bool escape_known = sema && sema->escape_known;
      if (!escape_known) {
        if (require_known_alias || require_no_alias_escape) {
          ny_diag_error(
              sig->stmt_t->tok,
              "alias policy violation in '%s': escape/alias facts are unknown",
              sig->name ? sig->name : "<anon>");
          cg->had_error = 1;
          alias_policy_unknown++;
        }
        continue;
      }

      if (require_no_alias_escape && (sig->args_escape || sig->returns_alias)) {
        ny_diag_error(
            sig->stmt_t->tok,
            "alias policy violation in '%s': args_escape=%s returns_alias=%s",
            sig->name ? sig->name : "<anon>", sig->args_escape ? "yes" : "no",
            sig->returns_alias ? "yes" : "no");
        cg->had_error = 1;
        alias_policy_violations++;
      }
    }
  }

  if (ny_env_enabled("NYTRIX_PURITY_DIAG")) {
    fprintf(
        stderr,
        "[purity] functions=%zu pure=%zu memo_safe=%zu "
        "args_escape=%zu args_mutated=%zu returns_alias=%zu recursive=%zu\n",
        total_count, pure_count, memo_safe_count, args_escape_count,
        args_mutated_count, returns_alias_count, recursive_count);
  }
  if (ny_env_enabled("NYTRIX_EFFECT_DIAG")) {
    fprintf(stderr,
            "[effects] functions=%zu none=%zu io=%zu alloc=%zu ffi=%zu "
            "thread=%zu unknown=%zu\n",
            total_count, effects_none_count, effects_io_count,
            effects_alloc_count, effects_ffi_count, effects_thread_count,
            effects_unknown_count);
  }
  if (ny_env_enabled("NYTRIX_EFFECT_DIAG_VERBOSE")) {
    NY_FOREACH_POLICY_SIG(cg, include_std_in_policy, sig) {
      char fx_buf[96];
      ny_effect_mask_to_buf(sig->effects_known ? sig->effects : NY_FX_ALL,
                            fx_buf, sizeof(fx_buf));
      fprintf(stderr,
              "[effects.fn] %s effects=%s known=%s pure=%s memo_safe=%s "
              "args_escape=%s args_mutated=%s returns_alias=%s recursive=%s\n",
              sig->name ? sig->name : "<anon>", fx_buf,
              sig->effects_known ? "yes" : "no", sig->is_pure ? "yes" : "no",
              sig->is_memo_safe ? "yes" : "no", sig->args_escape ? "yes" : "no",
              sig->args_mutated ? "yes" : "no",
              sig->returns_alias ? "yes" : "no",
              sig->is_recursive ? "yes" : "no");
    }
  }
  if (ny_env_enabled("NYTRIX_EFFECT_DIAG")) {
    char forbid_buf[96];
    ny_effect_mask_to_buf(forbid_effects, forbid_buf, sizeof(forbid_buf));
    fprintf(stderr,
            "[effects.policy] active=%s forbid=%s include_std=%s "
            "violations=%zu unknown=%zu\n",
            effect_policy_active ? "yes" : "no", forbid_buf,
            include_std_in_policy ? "yes" : "no", effect_policy_violations,
            effect_policy_unknown);
  }
  if (ny_env_enabled("NYTRIX_ALIAS_DIAG")) {
    fprintf(stderr,
            "[alias] functions=%zu args_escape=%zu args_mutated=%zu "
            "returns_alias=%zu\n",
            total_count, args_escape_count, args_mutated_count,
            returns_alias_count);
  }
  if (ny_env_enabled("NYTRIX_ALIAS_DIAG")) {
    fprintf(stderr,
            "[alias.policy] active=%s require_known=%s require_no_escape=%s "
            "include_std=%s violations=%zu unknown=%zu\n",
            alias_policy_active ? "yes" : "no",
            require_known_alias ? "yes" : "no",
            require_no_alias_escape ? "yes" : "no",
            include_std_in_alias_policy ? "yes" : "no", alias_policy_violations,
            alias_policy_unknown);
  }
  if (ny_env_enabled("NYTRIX_EFFECT_DIAG")) {
    fprintf(stderr, "[effects.contract] violations=%zu unknown=%zu\n",
            effect_contract_violations, effect_contract_unknown);
  }
}

#undef NY_FOREACH_POLICY_SIG
#undef NY_FOREACH_NON_STD_FUNC_SIG
#undef NY_FOREACH_FUNC_SIG
