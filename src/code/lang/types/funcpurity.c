/*
 * Function purity analysis: determines whether a function is pure,
 * impure, or effectful for optimization and safety-check gating.
 */
#include "base/common.h"
#include "base/debug.h"
#include "base/loader.h"
#include "base/util.h"
#include "code/lang/parse/frontend/visitor.h"
#include "code/lang/priv.h"
#include "code/lang/types/pipeline.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool ny_effect_diag_verbose_enabled(void);
static bool ny_effect_is_assignment_operator(const char *op);
static bool ny_sig_is_std_for_effects(const fun_sig *sig);
static const char *ny_module_prefix_stable(codegen_t *cg, const char *qname);

static void ny_effect_diag_log(const char *event, const char *fmt, ...) {
  char detail[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(detail, sizeof(detail), fmt, ap);
  va_end(ap);
  ny_log_emit_unchecked(ny_ui_mark("hint"), NY_CLR_GRAY, "[effects.%s] %s\n",
                        event ? event : "trace", detail);
}

typedef struct {
  const char *name;
  uint32_t effects;
  enum {
    NY_BUILTIN_EFFECT_DIRECT,
    NY_BUILTIN_EFFECT_CALLABLE_FACTORY,
    NY_BUILTIN_EFFECT_QUERY_FROM,
    NY_BUILTIN_EFFECT_QUERY_STEP,
    NY_BUILTIN_EFFECT_QUERY_PREDICATE_STEP,
    NY_BUILTIN_EFFECT_QUERY_PIPE,
    NY_BUILTIN_EFFECT_QUERY_RUN,
    NY_BUILTIN_EFFECT_OPTIONAL_HOF,
  } kind;
} ny_builtin_effect_contract_t;

static const ny_builtin_effect_contract_t ny_builtin_effect_contracts[] = {
#define NY_BUILTIN_PURE(name) {name, NY_FX_NONE, NY_BUILTIN_EFFECT_DIRECT},
#define NY_BUILTIN_EFFECT(name, effects)                                       \
  {name, effects, NY_BUILTIN_EFFECT_DIRECT},
#define NY_BUILTIN_CALLABLE_FACTORY(name, effects)                             \
  {name, effects, NY_BUILTIN_EFFECT_CALLABLE_FACTORY},
#define NY_BUILTIN_QUERY_FROM(name, effects)                                   \
  {name, effects, NY_BUILTIN_EFFECT_QUERY_FROM},
#define NY_BUILTIN_QUERY_STEP(name, effects)                                   \
  {name, effects, NY_BUILTIN_EFFECT_QUERY_STEP},
#define NY_BUILTIN_QUERY_PREDICATE_STEP(name, effects)                         \
  {name, effects, NY_BUILTIN_EFFECT_QUERY_PREDICATE_STEP},
#define NY_BUILTIN_QUERY_PIPE(name, effects)                                   \
  {name, effects, NY_BUILTIN_EFFECT_QUERY_PIPE},
#define NY_BUILTIN_QUERY_RUN(name, effects)                                    \
  {name, effects, NY_BUILTIN_EFFECT_QUERY_RUN},
#define NY_BUILTIN_OPTIONAL_HOF(name, effects)                                 \
  {name, effects, NY_BUILTIN_EFFECT_OPTIONAL_HOF},
#include "code/lang/runtime/contracts.c"
#undef NY_BUILTIN_OPTIONAL_HOF
#undef NY_BUILTIN_QUERY_RUN
#undef NY_BUILTIN_QUERY_PIPE
#undef NY_BUILTIN_QUERY_PREDICATE_STEP
#undef NY_BUILTIN_QUERY_STEP
#undef NY_BUILTIN_QUERY_FROM
#undef NY_BUILTIN_CALLABLE_FACTORY
#undef NY_BUILTIN_EFFECT
#undef NY_BUILTIN_PURE
};

static bool ny_builtin_contract(const char *name, uint32_t *effects) {
  if (!name || !*name)
    return false;
  for (size_t i = 0; i < sizeof(ny_builtin_effect_contracts) /
                             sizeof(ny_builtin_effect_contracts[0]);
       ++i) {
    if (strcmp(ny_builtin_effect_contracts[i].name, name) == 0) {
      if (effects)
        *effects = ny_builtin_effect_contracts[i].effects;
      return true;
    }
  }
  return false;
}

static int ny_builtin_contract_kind(const char *name) {
  if (!name || !*name)
    return -1;
  for (size_t i = 0; i < sizeof(ny_builtin_effect_contracts) /
                             sizeof(ny_builtin_effect_contracts[0]);
       ++i)
    if (strcmp(ny_builtin_effect_contracts[i].name, name) == 0)
      return ny_builtin_effect_contracts[i].kind;
  return -1;
}

/*
 * Function signatures can retain a module-local spelling such as
 * `list.map`. Resolve its registered owner from the signature's module
 * identity before consulting the shared builtin contract table.
 */
static bool ny_sig_builtin_contract(const fun_sig *sig, uint32_t *effects) {
  if (!sig)
    return false;
  /*
   * A user declaration can deliberately occupy a std.* spelling.  Contracts
   * for stdlib declarations must follow the registered source identity rather
   * than inheriting authority from their canonical-looking name.
   */
  bool standard_origin = ny_sig_is_std_for_effects(sig);
  if (sig->stmt_t && !standard_origin)
    return false;
  if (ny_builtin_contract(sig->name, effects))
    return true;
  if (!sig->name || !*sig->name)
    return false;
  const char *leaf = strrchr(sig->name, '.');
  leaf = leaf ? leaf + 1 : sig->name;
  char qualified[256];
  if (standard_origin && sig->module_name) {
    int n =
        snprintf(qualified, sizeof(qualified), "%s.%s", sig->module_name, leaf);
    if (n > 0 && (size_t)n < sizeof(qualified) &&
        ny_builtin_contract(qualified, effects))
      return true;
  }
  if (ny_sig_is_std_for_effects(sig)) {
    int n = snprintf(qualified, sizeof(qualified), "core.%s", leaf);
    if (n > 0 && (size_t)n < sizeof(qualified) &&
        ny_builtin_contract(qualified, effects))
      return true;
  }
  return false;
}

static bool ny_builtin_name_is_pure(const char *name) {
  uint32_t effects = NY_FX_ALL;
  return ny_builtin_contract(name, &effects) && effects == NY_FX_NONE;
}

/*
 * Effect classification follows source origin carried by the declaration.
 * Qualified names are not provenance: users may declare modules such as
 * `std.project`. Module ids preserve origin through generated declarations.
 */
static bool ny_sig_is_std_for_effects(const fun_sig *sig) {
  if (!sig)
    return false;
  if (!sig->stmt_t || sig->stmt_t->kind != NY_S_FUNC)
    return false;
  token_t tok = sig->stmt_t->tok;
  if (ny_is_stdlib_tok(tok))
    return true;
  if (tok.module_features & NY_MODULE_FEATURE_USER)
    return false;
  if (tok.module_id == NY_MODULE_ID_UNKNOWN)
    return false;
  for (size_t i = 0; i < ny_std_module_count(); ++i)
    if (ny_std_module_id(i) == tok.module_id)
      return (ny_std_module_features(i) & NY_MODULE_FEATURE_STDLIB) != 0;
  return false;
}

static const char *ny_effect_func_leaf_name(const char *name) {
  if (!name || !*name)
    return name;
  const char *dot = strrchr(name, '.');
  return dot ? dot + 1 : name;
}

static bool ny_effect_async_return_is_thread_safe(const char *type_name) {
  if (!type_name || !*type_name)
    return true;
  while (*type_name == '?')
    type_name++;
  const char *leaf = ny_effect_func_leaf_name(type_name);
  if (!leaf || !*leaf)
    return true;
  if (strcmp(leaf, "f32") == 0 || strcmp(leaf, "f64") == 0 ||
      strcmp(leaf, "f128") == 0 || strcmp(leaf, "float") == 0 ||
      strcmp(leaf, "ptr") == 0 || strcmp(leaf, "handle") == 0 ||
      strcmp(leaf, "c64") == 0 || strcmp(leaf, "c128") == 0)
    return false;
  return true;
}

static bool ny_sig_can_effect_async_lower(fun_sig *sig, bool global_lowering) {
  if (!sig || sig->is_extern || !sig->stmt_t || sig->stmt_t->kind != NY_S_FUNC)
    return false;
  stmt_func_t *fn = &sig->stmt_t->as.fn;
  if (fn->is_extern || fn->attr_thread || fn->attr_naked || fn->attr_jit ||
      fn->attr_cache)
    return false;
  if (!fn->attr_async_effects && !global_lowering)
    return false;
  const char *leaf = ny_effect_func_leaf_name(sig->name);
  if (leaf && strcmp(leaf, "main") == 0)
    return false;
  return ny_effect_async_return_is_thread_safe(fn->return_type);
}

static sema_func_t *ny_sig_func_sema(fun_sig *sig) {
  if (!sig || !sig->stmt_t || sig->stmt_t->kind != NY_S_FUNC ||
      sig->stmt_t->sema_kind != NY_STMT_SEMA_FUNC)
    return NULL;
  return (sema_func_t *)sig->stmt_t->sema;
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
  sema_func_t *sema = ny_sig_func_sema(sig);
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
  sema_func_t *sema = ny_sig_func_sema(sig);
  if (!sema || !sema->memo_known)
    return false;
  return sema->is_memo_safe;
}

static uint32_t ny_builtin_name_effects(const char *name) {
  uint32_t effects = NY_FX_ALL;
  return ny_builtin_contract(name, &effects) ? effects : NY_FX_ALL;
}

static uint32_t ny_sig_effects(fun_sig *sig) {
  if (!sig)
    return NY_FX_ALL;
  uint32_t contract_effects = NY_FX_ALL;
  if (ny_sig_builtin_contract(sig, &contract_effects))
    return contract_effects;
  /*
   * Standard-library bodies are intentionally summarized conservatively in
   * the final signature pass below. Make that same known-all contract visible
   * during fixed-point inference, so callers do not retain an unknown fact
   * merely because the stdlib summary is installed later.
   */
  if (ny_sig_is_std_for_effects(sig))
    return NY_FX_ALL;
  if (sig->is_extern) {
    if (sig->effects_known)
      return sig->effects;
    if (sig->stmt_t && sig->stmt_t->kind == NY_S_FUNC &&
        sig->stmt_t->as.fn.effect_contract_known)
      return sig->stmt_t->as.fn.effect_contract_mask;
    return NY_FX_FFI;
  }
  if (!sig->stmt_t)
    return ny_builtin_name_effects(sig->name);
  if (sig->stmt_t->kind != NY_S_FUNC)
    return NY_FX_ALL;
  if (sig->stmt_t->as.fn.effect_contract_known &&
      !sig->stmt_t->as.fn.attr_pure &&
      sig->stmt_t->as.fn.effect_contract_mask == NY_FX_ALL)
    return NY_FX_ALL;
  sema_func_t *sema = ny_sig_func_sema(sig);
  if (!sema || !sema->effects_known)
    return NY_FX_ALL;
  return sema->effects;
}

static bool ny_sig_effects_known(fun_sig *sig) {
  if (!sig)
    return false;
  if (ny_sig_builtin_contract(sig, NULL))
    return true;
  if (ny_sig_is_std_for_effects(sig))
    return true;
  if (sig->is_extern)
    return sig->effects_known ||
           (sig->stmt_t && sig->stmt_t->kind == NY_S_FUNC &&
            sig->stmt_t->as.fn.effect_contract_known);
  if (!sig->stmt_t)
    return ny_builtin_contract(sig->name, NULL);
  if (sig->stmt_t->kind != NY_S_FUNC)
    return false;
  if (sig->stmt_t->as.fn.effect_contract_known &&
      !sig->stmt_t->as.fn.attr_pure &&
      sig->stmt_t->as.fn.effect_contract_mask == NY_FX_ALL)
    return true;
  sema_func_t *sema = ny_sig_func_sema(sig);
  return sema && sema->effects_known;
}

/*
 * Fixed named effects are closed contracts; only a row tail is polymorphic.
 */
static bool ny_effect_type_is_open(const ny_type_t *type) {
  ny_type_t *current = ny_type_find((ny_type_t *)type);
  for (unsigned depth = 0; current && depth < 128; ++depth) {
    if (current->kind == NY_TYPE_EFFECT_VAR || current->kind == NY_TYPE_VAR)
      return true;
    if (current->kind != NY_TYPE_ROW)
      return false;
    current = ny_type_find(current->as.row.tail);
  }
  return false;
}

static const ny_type_t *ny_effect_sig_row(const fun_sig *sig) {
  if (!sig)
    return NULL;
  if (sig->effect_type)
    return sig->effect_type;
  if (sig->stmt_t && sig->stmt_t->kind == NY_S_FUNC)
    return sig->stmt_t->as.fn.effect_type;
  return NULL;
}

static bool ny_effect_row_has_nonbuiltin_content(const ny_type_t *row) {
  ny_type_t *current = ny_type_find((ny_type_t *)row);
  for (unsigned depth = 0; current && depth < 128; ++depth) {
    /*
     * Open callback rows have their own call-site substitution path.
     */
    if (current->kind == NY_TYPE_EFFECT_VAR || current->kind == NY_TYPE_VAR)
      return false;
    if (current->kind != NY_TYPE_ROW)
      return true;
    if (current->as.row.label &&
        strncmp(current->as.row.label, "std.effects.", 12) != 0)
      return true;
    current = ny_type_find(current->as.row.tail);
  }
  return false;
}

static bool ny_effect_row_covers(const ny_type_t *required,
                                 const ny_type_t *allowed) {
  bool allowed_open = ny_effect_type_is_open(allowed);
  ny_type_t *current = ny_type_find((ny_type_t *)required);
  for (unsigned depth = 0; current && depth < 128; ++depth) {
    if (current->kind == NY_TYPE_EFFECT_VAR || current->kind == NY_TYPE_VAR)
      return allowed_open;
    if (current->kind != NY_TYPE_ROW)
      return false;
    const char *label = current->as.row.label;
    if (label && strncmp(label, "std.effects.", 12) != 0) {
      ny_type_t *allowed_field =
          ny_type_row_get_label((ny_type_t *)allowed, label);
      if (!allowed_field) {
        if (!allowed_open)
          return false;
      } else if (!ny_type_structurally_equal(current->as.row.field_type,
                                             allowed_field))
        return false;
    }
    current = ny_type_find(current->as.row.tail);
  }
  return current == NULL;
}

static bool ny_sig_has_polymorphic_effect_contract(const fun_sig *sig) {
  if (!sig)
    return false;
  const ny_type_t *eff = sig->effect_type
                             ? sig->effect_type
                             : (sig->stmt_t && sig->stmt_t->kind == NY_S_FUNC
                                    ? sig->stmt_t->as.fn.effect_type
                                    : NULL);
  bool declared = sig->stmt_t && sig->stmt_t->kind == NY_S_FUNC &&
                  sig->stmt_t->as.fn.effect_contract_known;
  return ny_effect_type_is_open(eff) && (declared || sig->effects_known);
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
               {NY_FX_THREAD, "thread"},
               {NY_FX_READ_MEMORY, "read"},
               {NY_FX_WRITE_MEMORY, "write"}};
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
  if (strcmp(token, "read") == 0 || strcmp(token, "read_memory") == 0)
    return NY_FX_READ_MEMORY;
  if (strcmp(token, "write") == 0 || strcmp(token, "write_memory") == 0)
    return NY_FX_WRITE_MEMORY;
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

/*
 * An impl-less nominal type (declared only through `impl Name { ... }`) has
 * no constructor signature: its calls synthesize a record at lowering time.
 * The registered impl methods (`Name.method`) identify the owner, and the
 * constructor's effect contract is a plain allocation.
 */
static bool ny_sig_is_impl_owner(codegen_t *cg, const char *name) {
  if (!cg || !name || !*name)
    return false;
  size_t nlen = strlen(name);
  for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
    const fun_sig *sig = &cg->fun_sigs.data[i];
    if (!sig->name || strncmp(sig->name, name, nlen) != 0 ||
        sig->name[nlen] != '.')
      continue;
    return true;
  }
  return false;
}

static fun_sig *ny_purity_resolve_call_sig(codegen_t *cg, expr_call_t *call,
                                           assigned_name_list *local_names,
                                           assigned_hash_list *local_hashes,
                                           uint64_t local_bloom[4]);

static fun_sig *ny_purity_resolve_memcall_sig(codegen_t *cg, expr_memcall_t *mc,
                                              assigned_name_list *local_names,
                                              assigned_hash_list *local_hashes,
                                              uint64_t local_bloom[4]);

static uint32_t
ny_stmt_effects(codegen_t *cg, stmt_t *s, assigned_name_list *local_names,
                assigned_hash_list *local_hashes, uint64_t local_bloom[4],
                const ny_param_list *params, const stmt_func_t *owner_fn,
                const ny_type_t *owner_effect_row,
                bool owner_effect_contract_known, bool *known);

typedef struct ny_purity_scope_checkpoint_t {
  size_t name_len;
  size_t hash_len;
  uint64_t bloom[4];
} ny_purity_scope_checkpoint_t;

static void ny_purity_scope_save(const assigned_name_list *local_names,
                                 const assigned_hash_list *local_hashes,
                                 const uint64_t local_bloom[4],
                                 ny_purity_scope_checkpoint_t *cp) {
  if (!cp)
    return;
  cp->name_len = local_names ? local_names->len : 0;
  cp->hash_len = local_hashes ? local_hashes->len : 0;
  if (local_bloom) {
    cp->bloom[0] = local_bloom[0];
    cp->bloom[1] = local_bloom[1];
    cp->bloom[2] = local_bloom[2];
    cp->bloom[3] = local_bloom[3];
  } else {
    cp->bloom[0] = 0;
    cp->bloom[1] = 0;
    cp->bloom[2] = 0;
    cp->bloom[3] = 0;
  }
}

static void ny_purity_scope_restore(assigned_name_list *local_names,
                                    assigned_hash_list *local_hashes,
                                    uint64_t local_bloom[4],
                                    const ny_purity_scope_checkpoint_t *cp) {
  if (!cp)
    return;
  if (local_names)
    local_names->len = cp->name_len;
  if (local_hashes)
    local_hashes->len = cp->hash_len;
  if (local_bloom) {
    local_bloom[0] = cp->bloom[0];
    local_bloom[1] = cp->bloom[1];
    local_bloom[2] = cp->bloom[2];
    local_bloom[3] = cp->bloom[3];
  }
}

typedef struct {
  const char *name;
  const char *container_type;
  const char *container_alias_group;
  const char *element_container_type;
  const char *value_type;
  uint32_t call_effects;
  bool call_effects_known;
  const ny_type_t *call_effect_row;
  bool call_effect_row_known;
  bool call_effect_custom_row_unknown;
  const expr_t *callable_expr;
  bool is_callable;
  bool is_query;
  uint32_t query_effects;
  bool query_effects_known;
  bool is_query_step;
  uint32_t query_step_effects;
  bool query_step_effects_known;
} ny_effect_local_fact_t;

typedef struct ny_comptime_io_scan_t {
  const stmt_t *seen[128];
  size_t len;
  size_t steps;
  bool incomplete;
} ny_comptime_io_scan_t;

typedef struct ny_effects_ctx {
  codegen_t *cg;
  ny_type_arena_t *fact_arena;
  const stmt_func_t *owner_fn;
  const ny_type_t *owner_effect_row;
  bool owner_effect_contract_known;
  bool owner_effect_contract_override;
  assigned_name_list *local_names;
  assigned_hash_list *local_hashes;
  uint64_t *local_bloom;
  uint32_t result;
  bool known;
  bool has_known_io;
  ny_comptime_io_scan_t *comptime_io_scan;
  size_t callback_control_flow_depth;
  size_t callback_try_body_depth;
  bool callback_alias_state_ambiguous;
  const expr_t *callback_refresh_expr;
  const expr_t *callback_nonmutating_list_append;
  ny_effect_local_fact_t *local_containers;
  size_t local_container_len;
  size_t local_container_cap;
  ny_purity_scope_checkpoint_t cp;
} ny_effects_ctx;

static bool effects_visit_expr(ny_visitor_t *v, expr_t *e);
static void effects_visit_expr_post(ny_visitor_t *v, expr_t *e);
static expr_t *ny_effect_direct_returned_container(const ny_effects_ctx *ctx,
                                                   const expr_t *value);
static bool ny_effect_builtin_list_append(ny_effects_ctx *ctx,
                                          const expr_t *expr,
                                          const char **source,
                                          expr_t **appended);
static bool effects_visit_stmt(ny_visitor_t *v, stmt_t *s);
static void effects_visit_stmt_post(ny_visitor_t *v, stmt_t *s);
static bool ny_effect_comptime_callee_io(ny_effects_ctx *ctx, fun_sig *sig);
static void ny_effect_store_call_custom_row(ny_effects_ctx *ctx, expr_t *expr,
                                            const ny_type_t *row, bool known,
                                            fun_sig *sig,
                                            ny_call_arg_list *args,
                                            size_t receiver_skip);
static const char *ny_effect_row_persist(ny_effects_ctx *ctx, ny_type_t *row);
static void ny_effect_record_indexed_callback_store(ny_effects_ctx *ctx,
                                                    expr_t *lhs, expr_t *rhs);
static void ny_effect_record_container_callback_store(ny_effects_ctx *ctx,
                                                      expr_t *target,
                                                      expr_t *key, expr_t *rhs);
static void ny_effect_record_mutated_container_callback_summary(
    ny_effects_ctx *ctx, const char *container,
    const ny_effect_local_fact_t *prior_summary, expr_t *rhs);
static bool ny_effect_callback_store_value_is_stable(const expr_t *value);
static void ny_effect_refresh_local_lambda_summaries(ny_effects_ctx *ctx,
                                                     const char *changed_name);
static bool ny_stmt_refs_params(stmt_t *s,
                                const assigned_name_list *param_names,
                                const assigned_hash_list *param_hashes,
                                const uint64_t param_bloom[4]);

static const ny_type_t *
ny_effect_owner_contract_row(const ny_effects_ctx *ctx) {
  if (!ctx)
    return NULL;
  if (ctx->owner_effect_contract_override)
    return ctx->owner_effect_row;
  return ctx->owner_fn ? ctx->owner_fn->effect_type : NULL;
}

static bool ny_effect_owner_contract_known(const ny_effects_ctx *ctx) {
  return ctx && (ctx->owner_effect_contract_override
                     ? ctx->owner_effect_contract_known
                     : (ctx->owner_fn && ctx->owner_fn->effect_contract_known));
}

static uint32_t ny_stmt_effects_in_context(ny_effects_ctx *ctx, stmt_t *s,
                                           const ny_param_list *params,
                                           const stmt_func_t *owner_fn);

static const char *
ny_effect_local_container_alias_group(const ny_effects_ctx *ctx,
                                      const char *container) {
  if (!ctx || !container)
    return NULL;
  for (size_t i = ctx->local_container_len; i > 0; --i) {
    const ny_effect_local_fact_t *fact = &ctx->local_containers[i - 1];
    if (fact->name && strcmp(fact->name, container) == 0 &&
        fact->container_type &&
        (strcmp(fact->container_type, "list") == 0 ||
         strcmp(fact->container_type, "dict") == 0))
      return fact->container_alias_group ? fact->container_alias_group
                                         : fact->name;
  }
  return NULL;
}

static bool ny_effect_callback_fact_container_name(const char *fact_name,
                                                   char *out, size_t cap) {
  if (!fact_name || !out || cap == 0)
    return false;
  static const char *const prefixes[] = {
      "\001dict-callback-summary:", "\001list-callback-summary:",
      "\001dict-callback:", "\001list-callback:"};
  const char *length_text = NULL;
  for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
    size_t prefix_len = strlen(prefixes[i]);
    if (strncmp(fact_name, prefixes[i], prefix_len) == 0) {
      length_text = fact_name + prefix_len;
      break;
    }
  }
  if (!length_text)
    return false;
  char *length_end = NULL;
  unsigned long name_len = strtoul(length_text, &length_end, 10);
  if (!length_end || length_end == length_text || *length_end != ':' ||
      name_len == 0 || name_len >= cap)
    return false;
  const char *name = length_end + 1;
  if (strlen(name) < name_len)
    return false;
  memcpy(out, name, name_len);
  out[name_len] = '\0';
  return true;
}

static void ny_effect_set_local_container_alias_group(ny_effects_ctx *ctx,
                                                      const char *container,
                                                      const char *group) {
  if (!ctx || !container || !group)
    return;
  for (size_t i = ctx->local_container_len; i > 0; --i) {
    ny_effect_local_fact_t *fact = &ctx->local_containers[i - 1];
    if (fact->name && strcmp(fact->name, container) == 0 &&
        fact->container_type &&
        (strcmp(fact->container_type, "list") == 0 ||
         strcmp(fact->container_type, "dict") == 0)) {
      fact->container_alias_group = group;
      return;
    }
  }
}

static void ny_effect_set_callback_facts_alias_group(ny_effects_ctx *ctx,
                                                     const char *container,
                                                     const char *group) {
  if (!ctx || !container || !group)
    return;
  char fact_container[512];
  for (size_t i = 0; i < ctx->local_container_len; ++i) {
    ny_effect_local_fact_t *fact = &ctx->local_containers[i];
    if (!fact->name ||
        !ny_effect_callback_fact_container_name(fact->name, fact_container,
                                                sizeof(fact_container)) ||
        strcmp(fact_container, container) != 0)
      continue;
    fact->container_alias_group = group;
  }
}

static void ny_effect_record_local_container(
    ny_effects_ctx *ctx, const char *name, const char *container_type,
    const char *element_container_type, const char *value_type,
    uint32_t call_effects, bool call_effects_known, bool is_callable,
    bool is_query, uint32_t query_effects, bool query_effects_known,
    bool is_query_step, uint32_t query_step_effects,
    bool query_step_effects_known) {
  if (!ctx || !name)
    return;
  if (ctx->local_container_len == ctx->local_container_cap) {
    size_t cap = ctx->local_container_cap ? ctx->local_container_cap * 2 : 8;
    void *grown = ny_realloc_array(ctx->local_containers, cap,
                                   sizeof(*ctx->local_containers));
    if (!grown)
      return;
    ctx->local_containers = grown;
    ctx->local_container_cap = cap;
  }
  ctx->local_containers[ctx->local_container_len].name = name;
  ctx->local_containers[ctx->local_container_len].container_type =
      container_type;
  char fact_container[512];
  const char *alias_group = NULL;
  if (container_type && (strcmp(container_type, "list") == 0 ||
                         strcmp(container_type, "dict") == 0)) {
    alias_group = name;
  } else if (ny_effect_callback_fact_container_name(name, fact_container,
                                                    sizeof(fact_container))) {
    alias_group = ny_effect_local_container_alias_group(ctx, fact_container);
  }
  ctx->local_containers[ctx->local_container_len].container_alias_group =
      alias_group;
  ctx->local_containers[ctx->local_container_len].element_container_type =
      element_container_type;
  ctx->local_containers[ctx->local_container_len].value_type = value_type;
  ctx->local_containers[ctx->local_container_len].call_effects = call_effects;
  ctx->local_containers[ctx->local_container_len].call_effects_known =
      call_effects_known;
  ctx->local_containers[ctx->local_container_len].call_effect_row = NULL;
  ctx->local_containers[ctx->local_container_len].call_effect_row_known = false;
  ctx->local_containers[ctx->local_container_len]
      .call_effect_custom_row_unknown = false;
  ctx->local_containers[ctx->local_container_len].callable_expr = NULL;
  ctx->local_containers[ctx->local_container_len].is_callable = is_callable;
  ctx->local_containers[ctx->local_container_len].is_query = is_query;
  ctx->local_containers[ctx->local_container_len].query_effects = query_effects;
  ctx->local_containers[ctx->local_container_len].query_effects_known =
      query_effects_known;
  ctx->local_containers[ctx->local_container_len].is_query_step = is_query_step;
  ctx->local_containers[ctx->local_container_len].query_step_effects =
      query_step_effects;
  ctx->local_containers[ctx->local_container_len].query_step_effects_known =
      query_step_effects_known;
  ctx->local_container_len++;
}

static const expr_t *ny_effect_local_callable_expr(const ny_effects_ctx *ctx,
                                                   const char *name) {
  if (!ctx || !name)
    return NULL;
  for (size_t i = ctx->local_container_len; i > 0; --i) {
    const ny_effect_local_fact_t *fact = &ctx->local_containers[i - 1];
    if (fact->name && strcmp(fact->name, name) == 0)
      return fact->callable_expr;
  }
  return NULL;
}

static void ny_effect_record_local_callable_expr(ny_effects_ctx *ctx,
                                                 const char *name,
                                                 const expr_t *value) {
  if (!ctx || !name)
    return;
  for (size_t i = ctx->local_container_len; i > 0; --i) {
    ny_effect_local_fact_t *fact = &ctx->local_containers[i - 1];
    if (fact->name && strcmp(fact->name, name) == 0) {
      fact->callable_expr = value;
      return;
    }
  }
}

static void ny_effect_record_local_call_row(ny_effects_ctx *ctx,
                                            const char *name,
                                            const ny_type_t *row, bool known) {
  if (!ctx || !name)
    return;
  for (size_t i = ctx->local_container_len; i > 0; --i) {
    ny_effect_local_fact_t *fact = &ctx->local_containers[i - 1];
    if (fact->name && strcmp(fact->name, name) == 0) {
      fact->call_effect_row = row;
      fact->call_effect_row_known = known;
      return;
    }
  }
}

static const ny_type_t *ny_effect_local_call_row(const ny_effects_ctx *ctx,
                                                 const char *name,
                                                 bool *known) {
  if (known)
    *known = false;
  if (!ctx || !name)
    return NULL;
  for (size_t i = ctx->local_container_len; i > 0; --i) {
    const ny_effect_local_fact_t *fact = &ctx->local_containers[i - 1];
    if (!fact->name || strcmp(fact->name, name) != 0)
      continue;
    if (known)
      *known = fact->call_effect_row_known;
    return fact->call_effect_row;
  }
  return NULL;
}

static bool ny_effect_local_custom_row_unknown(const ny_effects_ctx *ctx,
                                               const char *name) {
  if (!ctx || !name)
    return false;
  for (size_t i = ctx->local_container_len; i > 0; --i) {
    const ny_effect_local_fact_t *fact = &ctx->local_containers[i - 1];
    if (fact->name && strcmp(fact->name, name) == 0)
      return fact->call_effect_custom_row_unknown;
  }
  return false;
}

static const char *ny_effect_local_container_type(const ny_effects_ctx *ctx,
                                                  const char *name) {
  if (!ctx || !name)
    return NULL;
  for (size_t i = ctx->local_container_len; i > 0; --i)
    if (ctx->local_containers[i - 1].name &&
        strcmp(ctx->local_containers[i - 1].name, name) == 0)
      return ctx->local_containers[i - 1].container_type;
  return NULL;
}

static const char *
ny_effect_local_element_container_type(const ny_effects_ctx *ctx,
                                       const char *name) {
  if (!ctx || !name)
    return NULL;
  for (size_t i = ctx->local_container_len; i > 0; --i)
    if (ctx->local_containers[i - 1].name &&
        strcmp(ctx->local_containers[i - 1].name, name) == 0)
      return ctx->local_containers[i - 1].element_container_type;
  return NULL;
}

static const char *ny_effect_local_value_type(const ny_effects_ctx *ctx,
                                              const char *name) {
  if (!ctx || !name)
    return NULL;
  for (size_t i = ctx->local_container_len; i > 0; --i)
    if (ctx->local_containers[i - 1].name &&
        strcmp(ctx->local_containers[i - 1].name, name) == 0)
      return ctx->local_containers[i - 1].value_type;
  return NULL;
}

static bool ny_effect_local_callable(const ny_effects_ctx *ctx,
                                     const char *name, uint32_t *effects) {
  if (!ctx || !name)
    return false;
  for (size_t i = ctx->local_container_len; i > 0; --i) {
    if (!ctx->local_containers[i - 1].name ||
        strcmp(ctx->local_containers[i - 1].name, name) != 0)
      continue;
    if (!ctx->local_containers[i - 1].is_callable ||
        !ctx->local_containers[i - 1].call_effects_known)
      return false;
    if (effects)
      *effects = ctx->local_containers[i - 1].call_effects;
    return true;
  }
  return false;
}

static bool ny_effect_local_is_callable(const ny_effects_ctx *ctx,
                                        const char *name) {
  if (!ctx || !name)
    return false;
  for (size_t i = ctx->local_container_len; i > 0; --i) {
    const ny_effect_local_fact_t *fact = &ctx->local_containers[i - 1];
    if (fact->name && strcmp(fact->name, name) == 0)
      return fact->is_callable;
  }
  return false;
}

static ny_effect_local_fact_t *
ny_effect_fact_snapshot(const ny_effects_ctx *ctx, size_t *len) {
  *len = ctx->local_container_len;
  if (!*len)
    return NULL;
  ny_effect_local_fact_t *copy = malloc(*len * sizeof(*copy));
  if (copy)
    memcpy(copy, ctx->local_containers, *len * sizeof(*copy));
  return copy;
}

static bool ny_effect_fact_restore(ny_effects_ctx *ctx,
                                   const ny_effect_local_fact_t *facts,
                                   size_t len) {
  if (len > ctx->local_container_cap) {
    void *grown = ny_realloc_array(ctx->local_containers, len,
                                   sizeof(*ctx->local_containers));
    if (!grown)
      return false;
    ctx->local_containers = grown;
    ctx->local_container_cap = len;
  }
  if (len)
    memcpy(ctx->local_containers, facts, len * sizeof(*facts));
  ctx->local_container_len = len;
  return true;
}

static const char *
ny_effect_list_callback_summary_fact_name(ny_effects_ctx *ctx,
                                          const char *container);
static bool ny_effect_merge_appended_list_callback_summary(
    ny_effects_ctx *ctx, const char *source, const char *destination,
    expr_t *appended_value);

static void
ny_effect_mark_list_callback_summary_unknown(ny_effects_ctx *ctx,
                                             const char *container) {
  const char *summary =
      ny_effect_list_callback_summary_fact_name(ctx, container);
  if (!summary)
    return;
  size_t before = ctx->local_container_len;
  ny_effect_record_local_container(ctx, summary, NULL, NULL, NULL, NY_FX_ALL,
                                   false, true, false, 0, false, false, 0,
                                   false);
  if (ctx->local_container_len > before) {
    ny_effect_local_fact_t *fact =
        &ctx->local_containers[ctx->local_container_len - 1];
    fact->call_effect_custom_row_unknown = true;
    fact->call_effect_row = NULL;
    fact->call_effect_row_known = false;
  }
}

static const ny_effect_local_fact_t *
ny_effect_fact_latest(const ny_effect_local_fact_t *facts, size_t len,
                      const char *name) {
  for (size_t i = len; i > 0; --i)
    if (facts[i - 1].name && strcmp(facts[i - 1].name, name) == 0)
      return &facts[i - 1];
  return NULL;
}

static bool
ny_effect_fact_merge_branches(ny_effects_ctx *ctx,
                              const ny_effect_local_fact_t *const *sets,
                              const size_t *lens, size_t set_count) {
  for (size_t set = 0; set < set_count; ++set) {
    for (size_t i = 0; i < lens[set]; ++i) {
      const char *name = sets[set][i].name;
      if (!name)
        continue;
      bool seen = false;
      for (size_t j = 0; j < i; ++j)
        if (sets[set][j].name && strcmp(sets[set][j].name, name) == 0) {
          seen = true;
          break;
        }
      if (seen)
        continue;
      const ny_effect_local_fact_t *merged = NULL;
      uint32_t effect_union = 0;
      bool all_callable_known = true;
      bool any_callable = false;
      bool all_call_rows_known = true;
      bool call_rows_composable = true;
      bool any_custom_call_row = false;
      const ny_type_t *custom_row = NULL;
      const expr_t *callable_expr = NULL;
      bool callable_expr_initialized = false;
      bool callable_expr_agrees = true;
      bool any_callable_expr = false;
      for (size_t path = 0; path < set_count; ++path) {
        const ny_effect_local_fact_t *fact =
            ny_effect_fact_latest(sets[path], lens[path], name);
        if (!fact)
          return false;
        if (!callable_expr_initialized) {
          callable_expr = fact->callable_expr;
          callable_expr_initialized = true;
        } else if (callable_expr != fact->callable_expr) {
          callable_expr_agrees = false;
        }
        any_callable_expr |= fact->callable_expr != NULL;
        if (!merged)
          merged = fact;
        if (fact->is_callable) {
          any_callable = true;
          all_callable_known = all_callable_known && fact->call_effects_known;
          effect_union |= fact->call_effects;
          all_call_rows_known =
              all_call_rows_known && fact->call_effect_row_known;
          any_custom_call_row |=
              ny_effect_row_has_nonbuiltin_content(fact->call_effect_row) ||
              fact->call_effect_custom_row_unknown;
          if (fact->call_effect_custom_row_unknown)
            call_rows_composable = false;
          if (path == 0)
            custom_row = fact->call_effect_row;
          else if (call_rows_composable &&
                   custom_row != fact->call_effect_row) {
            ny_type_t *composed = ny_type_row_compose(
                ctx->fact_arena, NULL, (ny_type_t *)custom_row,
                (ny_type_t *)fact->call_effect_row);
            if (composed)
              custom_row = composed;
            else
              call_rows_composable = false;
          }
        } else {
          all_callable_known = false;
          all_call_rows_known = false;
        }
      }
      const ny_effect_local_fact_t *prior = ny_effect_fact_latest(
          ctx->local_containers, ctx->local_container_len, name);
      if (!merged || (!any_callable && (!prior || !prior->is_callable)))
        continue;
      if (ctx->local_container_len == ctx->local_container_cap) {
        size_t cap =
            ctx->local_container_cap ? ctx->local_container_cap * 2 : 8;
        void *grown = ny_realloc_array(ctx->local_containers, cap,
                                       sizeof(*ctx->local_containers));
        if (!grown)
          return false;
        ctx->local_containers = grown;
        ctx->local_container_cap = cap;
      }
      ny_effect_local_fact_t joined = merged ? *merged : *prior;
      joined.is_callable = any_callable || (prior && prior->is_callable);
      joined.call_effects_known =
          joined.is_callable && all_callable_known && any_callable;
      joined.call_effects =
          joined.call_effects_known ? effect_union : NY_FX_ALL;
      joined.call_effect_row_known =
          joined.is_callable && all_call_rows_known && call_rows_composable;
      joined.call_effect_row =
          joined.call_effect_row_known &&
                  ny_effect_row_has_nonbuiltin_content(custom_row)
              ? custom_row
              : NULL;
      joined.call_effect_custom_row_unknown =
          any_custom_call_row && !call_rows_composable;
      joined.callable_expr = callable_expr_agrees ? callable_expr : NULL;
      if (!callable_expr_agrees && any_callable_expr) {
        joined.call_effects_known = false;
        joined.call_effect_row_known = false;
        joined.call_effect_custom_row_unknown = true;
      }
      ctx->local_containers[ctx->local_container_len++] = joined;
    }
  }
  return true;
}

static bool ny_effect_visit_loop_body(ny_visitor_t *v, ny_effects_ctx *ctx,
                                      stmt_t *body, stmt_t *update) {
  size_t base_len = 0, body_len = 0;
  ny_effect_local_fact_t *base = ny_effect_fact_snapshot(ctx, &base_len);
  if (base_len && !base) {
    ctx->callback_alias_state_ambiguous = true;
    ctx->callback_control_flow_depth++;
    ny_visit_stmt(v, body);
    ny_visit_stmt(v, update);
    ctx->callback_control_flow_depth--;
    return false;
  }
  bool base_ambiguous = ctx->callback_alias_state_ambiguous;
  ny_visit_stmt(v, body);
  ny_visit_stmt(v, update);
  ny_effect_local_fact_t *body_facts = ny_effect_fact_snapshot(ctx, &body_len);
  bool body_ambiguous = ctx->callback_alias_state_ambiguous;
  bool ok =
      (!body_len || body_facts) && ny_effect_fact_restore(ctx, base, base_len);
  if (ok) {
    const ny_effect_local_fact_t *sets[] = {base, body_facts};
    size_t lens[] = {base_len, body_len};
    ok = ny_effect_fact_merge_branches(ctx, sets, lens, 2);
  }
  ctx->callback_alias_state_ambiguous =
      ok ? base_ambiguous || body_ambiguous : true;
  free(base);
  free(body_facts);
  return ok;
}

static const char *ny_effect_expr_value_type(const ny_effects_ctx *ctx,
                                             const expr_t *expr) {
  if (!expr)
    return NULL;
  if (expr->kind == NY_E_LITERAL) {
    switch (expr->as.literal.kind) {
    case NY_LIT_INT:
      return "int";
    case NY_LIT_FLOAT:
      return expr->as.literal.hint == NY_LIT_HINT_F32 ? "f32" : "f64";
    case NY_LIT_STR:
      return "str";
    case NY_LIT_BOOL:
      return "bool";
    default:
      break;
    }
  }
  if (expr->kind == NY_E_IDENT && expr->as.ident.name)
    return ny_effect_local_value_type(ctx, expr->as.ident.name);
  if (expr->semantic.resolved) {
    switch (expr->semantic.rep) {
    case NY_SEM_REP_RAW_INT:
      return "int";
    case NY_SEM_REP_F32:
      return "f32";
    case NY_SEM_REP_F64:
      return "f64";
    case NY_SEM_REP_STRING:
      return "str";
    case NY_SEM_REP_TYPED_BUFFER:
      return "list";
    default:
      break;
    }
  }
  if (expr->kind == NY_E_CALL) {
    stmt_t *callee = expr->semantic.canonical_callee_stmt;
    if (callee && callee->kind == NY_S_FUNC)
      return callee->as.fn.return_type;
    fun_sig *sig = ny_purity_resolve_call_sig(
        ctx->cg, (expr_call_t *)&expr->as.call, ctx->local_names,
        ctx->local_hashes,
        ctx->local_bloom ? ctx->local_bloom : (uint64_t[4]){0});
    return sig ? sig->return_type : NULL;
  }
  return NULL;
}

static fun_sig *ny_effect_attached_sig_for_owner(codegen_t *cg,
                                                 const char *owner_type,
                                                 const char *method) {
  if (!cg || !owner_type || !method)
    return NULL;
  fun_sig *hit = NULL;
  for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
    fun_sig *candidate = &cg->fun_sigs.data[i];
    if (!candidate->is_attached_method || !candidate->owner_type ||
        strcmp(candidate->owner_type, owner_type) != 0 || !candidate->name)
      continue;
    const char *leaf = strrchr(candidate->name, '.');
    leaf = leaf ? leaf + 1 : candidate->name;
    if (strcmp(leaf, method) != 0)
      continue;
    if (hit && (ny_sig_effects(hit) != ny_sig_effects(candidate) ||
                ny_sig_effects_known(hit) != ny_sig_effects_known(candidate)))
      return NULL;
    hit = candidate;
  }
  return hit;
}

static const char *ny_effect_container_literal_type(const expr_t *expr) {
  if (!expr)
    return NULL;
  switch (expr->kind) {
  case NY_E_LIST:
  case NY_E_TUPLE:
    return "list";
  case NY_E_DICT:
    return "dict";
  case NY_E_SET:
    return "set";
  default:
    return NULL;
  }
}

static const char *ny_effect_container_type_from_name(const char *type_name) {
  if (!type_name)
    return NULL;
  while (*type_name == '?' || *type_name == '*')
    ++type_name;
  if (strcmp(type_name, "list") == 0 || strncmp(type_name, "list<", 5) == 0 ||
      strcmp(type_name, "seq") == 0 || strcmp(type_name, "tuple") == 0)
    return "list";
  if (strcmp(type_name, "dict") == 0)
    return "dict";
  if (strcmp(type_name, "set") == 0)
    return "set";
  return NULL;
}

static const char *
ny_effect_global_container_type_depth(const ny_effects_ctx *ctx,
                                      const char *name, unsigned depth);

static const char *
ny_effect_initializer_container_type(const ny_effects_ctx *ctx,
                                     const expr_t *expr, unsigned depth) {
  if (!ctx || !expr || depth > 16)
    return NULL;
  const char *literal = ny_effect_container_literal_type(expr);
  if (literal)
    return literal;
  if (expr->semantic.resolved && expr->semantic.rep == NY_SEM_REP_TYPED_BUFFER)
    return "list";
  if (expr->kind == NY_E_IDENT && expr->as.ident.name)
    return ny_effect_global_container_type_depth(ctx, expr->as.ident.name,
                                                 depth + 1);
  if (expr->kind != NY_E_CALL)
    return NULL;
  fun_sig *sig = ny_purity_resolve_call_sig(
      ctx->cg, (expr_call_t *)&expr->as.call, NULL, NULL, NULL);
  if (sig && ny_sig_is_std_for_effects(sig) && sig->name) {
    const char *leaf = strrchr(sig->name, '.');
    leaf = leaf ? leaf + 1 : sig->name;
    if (strcmp(leaf, "own") == 0 && expr->as.call.args.len == 1 &&
        expr->as.call.args.data[0].val)
      return ny_effect_initializer_container_type(
          ctx, expr->as.call.args.data[0].val, depth + 1);
  }
  if (sig) {
    const char *container =
        ny_effect_container_type_from_name(sig->return_type);
    if (container)
      return container;
  }
  stmt_t *callee = expr->semantic.canonical_callee_stmt;
  if (callee && callee->kind == NY_S_FUNC)
    return ny_effect_container_type_from_name(callee->as.fn.return_type);
  return NULL;
}

static const char *
ny_effect_global_container_type_depth(const ny_effects_ctx *ctx,
                                      const char *name, unsigned depth) {
  if (!ctx || !ctx->cg || !name || depth > 16)
    return NULL;
  binding *global = lookup_global(ctx->cg, name);
  if (!global || !global->stmt_t || global->stmt_t->kind != NY_S_VAR)
    return NULL;
  stmt_var_t *var = &global->stmt_t->as.var;
  for (size_t i = 0; i < var->names.len; ++i) {
    if (!var->names.data[i] || strcmp(var->names.data[i], name) != 0)
      continue;
    if (i < var->types.len) {
      const char *declared =
          ny_effect_container_type_from_name(var->types.data[i]);
      if (declared)
        return declared;
    }
    return i < var->exprs.len ? ny_effect_initializer_container_type(
                                    ctx, var->exprs.data[i], depth + 1)
                              : NULL;
  }
  return NULL;
}

static const char *ny_effect_value_container_type(const ny_effects_ctx *ctx,
                                                  const expr_t *expr) {
  if (!expr)
    return NULL;
  const char *literal = ny_effect_container_literal_type(expr);
  if (literal)
    return literal;
  if (expr->kind == NY_E_IDENT && expr->as.ident.name)
    return ny_effect_local_container_type(ctx, expr->as.ident.name)
               ? ny_effect_local_container_type(ctx, expr->as.ident.name)
               : ny_effect_global_container_type_depth(ctx, expr->as.ident.name,
                                                       0);
  if (expr->kind == NY_E_CALL) {
    fun_sig *sig = ny_purity_resolve_call_sig(
        ctx->cg, (expr_call_t *)&expr->as.call, ctx->local_names,
        ctx->local_hashes,
        ctx->local_bloom ? ctx->local_bloom : (uint64_t[4]){0});
    if (sig) {
      const char *container =
          ny_effect_container_type_from_name(sig->return_type);
      if (container)
        return container;
    }
    stmt_t *callee = expr->semantic.canonical_callee_stmt;
    if (callee && callee->kind == NY_S_FUNC)
      return ny_effect_container_type_from_name(callee->as.fn.return_type);
  }
  if (expr->kind == NY_E_MEMCALL && expr->as.memcall.target &&
      expr->as.memcall.target->kind == NY_E_IDENT && expr->as.memcall.name &&
      strcmp(expr->as.memcall.name, "get") == 0)
    return ny_effect_local_element_container_type(
        ctx, expr->as.memcall.target->as.ident.name);
  return NULL;
}

static const char *
ny_effect_literal_element_container_type(const ny_effects_ctx *ctx,
                                         const expr_t *expr) {
  if (!expr)
    return NULL;
  if (expr->kind == NY_E_LIST || expr->kind == NY_E_TUPLE) {
    for (size_t i = 0; i < expr->as.list_like.len; ++i) {
      const char *type =
          ny_effect_value_container_type(ctx, expr->as.list_like.data[i]);
      if (type)
        return type;
    }
  } else if (expr->kind == NY_E_IDENT && expr->as.ident.name) {
    return ny_effect_local_element_container_type(ctx, expr->as.ident.name);
  }
  return NULL;
}

static fun_sig *ny_effect_sig_for_stmt(codegen_t *cg, stmt_t *stmt) {
  if (!cg || !stmt)
    return NULL;
  for (size_t i = 0; i < cg->fun_sigs.len; ++i)
    if (cg->fun_sigs.data[i].stmt_t == stmt)
      return &cg->fun_sigs.data[i];
  return NULL;
}

static bool ny_effect_expr_qualified_name(const expr_t *expr, char *out,
                                          size_t out_cap, size_t *len) {
  if (!expr || !out || !out_cap || !len)
    return false;
  if (expr->kind == NY_E_IDENT && expr->as.ident.name) {
    size_t n = strlen(expr->as.ident.name);
    if (n >= out_cap - *len)
      return false;
    memcpy(out + *len, expr->as.ident.name, n);
    *len += n;
    out[*len] = '\0';
    return true;
  }
  if (expr->kind == NY_E_MEMBER && expr->as.member.target &&
      expr->as.member.name) {
    if (!ny_effect_expr_qualified_name(expr->as.member.target, out, out_cap,
                                       len))
      return false;
    size_t n = strlen(expr->as.member.name);
    if (*len + 1 >= out_cap || n >= out_cap - *len - 1)
      return false;
    out[(*len)++] = '.';
    memcpy(out + *len, expr->as.member.name, n);
    *len += n;
    out[*len] = '\0';
    return true;
  }
  return false;
}

static bool ny_effect_resolve_module_function_value(codegen_t *cg,
                                                    const expr_t *member,
                                                    char *out, size_t out_cap) {
  if (!cg || !member || member->kind != NY_E_MEMBER ||
      !member->as.member.target || !member->as.member.name || !out ||
      out_cap == 0)
    return false;
  char module_path[1024];
  if (ny_resolve_module_expr_path(cg, NULL, 0, member->as.member.target,
                                  module_path, sizeof(module_path)) &&
      ny_resolve_module_function_path(cg, module_path, member->as.member.name,
                                      out, out_cap))
    return true;

  char qualified[1280] = {0};
  size_t qualified_len = 0;
  if (!ny_effect_expr_qualified_name(member, qualified, sizeof(qualified),
                                     &qualified_len))
    return false;
  char *leaf = strrchr(qualified, '.');
  if (!leaf || leaf == qualified)
    return false;
  size_t module_len = (size_t)(leaf - qualified);
  if (module_len >= sizeof(module_path))
    return false;
  memcpy(module_path, qualified, module_len);
  module_path[module_len] = '\0';
  return ny_module_path_exists(cg, module_path) &&
         ny_resolve_module_function_path(cg, module_path, leaf + 1, out,
                                         out_cap);
}

static bool ny_effect_lambda_summary(ny_effects_ctx *ctx, const expr_t *lambda,
                                     uint32_t *effects);
static bool ny_effect_call_result_callable_summary(ny_effects_ctx *ctx,
                                                   expr_t *call,
                                                   uint32_t *effects,
                                                   const ny_type_t **row);
static const ny_type_t *ny_effect_callback_row(ny_effects_ctx *ctx,
                                               const expr_t *callback,
                                               bool *known);
static ny_type_t *ny_effect_custom_row_only(ny_type_arena_t *arena,
                                            const ny_type_t *effect_row,
                                            bool *known);
static bool ny_effect_owner_row_callback(const ny_effects_ctx *ctx,
                                         const char *name);

/*
 * Use impossible source identifier bytes so aggregate callback facts share the
 * existing snapshot/branch-join machinery without colliding with locals.
 */
static const char *ny_effect_dict_callback_fact_name(ny_effects_ctx *ctx,
                                                     const char *container,
                                                     const char *key) {
  if (!ctx || !ctx->cg || !ctx->cg->arena || !container || !key)
    return NULL;
  size_t container_len = strlen(container), key_len = strlen(key);
  size_t cap = container_len + key_len + 48;
  char *tmp = malloc(cap);
  if (!tmp)
    return NULL;
  int n = snprintf(tmp, cap, "\001dict-callback:%zu:%s:%s", container_len,
                   container, key);
  const char *name = n > 0 && (size_t)n < cap
                         ? arena_strndup(ctx->cg->arena, tmp, (size_t)n)
                         : NULL;
  free(tmp);
  return name;
}

static bool ny_effect_dict_get_callback_expr(const expr_t *expr,
                                             const char **container,
                                             const char **key) {
  if (!expr || expr->kind != NY_E_MEMCALL || !container || !key ||
      !expr->as.memcall.name || strcmp(expr->as.memcall.name, "get") != 0 ||
      !expr->as.memcall.target || expr->as.memcall.target->kind != NY_E_IDENT ||
      !expr->as.memcall.target->as.ident.name ||
      expr->as.memcall.args.len < 1 || expr->as.memcall.args.len > 2 ||
      expr->as.memcall.args.data[0].name ||
      (expr->as.memcall.args.len == 2 && expr->as.memcall.args.data[1].name &&
       strcmp(expr->as.memcall.args.data[1].name, "default") != 0))
    return false;
  const expr_t *arg = expr->as.memcall.args.data[0].val;
  if (!arg || arg->kind != NY_E_LITERAL || arg->as.literal.kind != NY_LIT_STR ||
      !arg->as.literal.as.s.data)
    return false;
  *container = expr->as.memcall.target->as.ident.name;
  *key = arg->as.literal.as.s.data;
  return true;
}

static const char *
ny_effect_dict_callback_summary_fact_name(ny_effects_ctx *ctx,
                                          const char *container) {
  if (!ctx || !ctx->cg || !ctx->cg->arena || !container)
    return NULL;
  size_t container_len = strlen(container);
  size_t cap = container_len + 64;
  char *tmp = malloc(cap);
  if (!tmp)
    return NULL;
  int n = snprintf(tmp, cap, "\001dict-callback-summary:%zu:%s", container_len,
                   container);
  const char *name = n > 0 && (size_t)n < cap
                         ? arena_strndup(ctx->cg->arena, tmp, (size_t)n)
                         : NULL;
  free(tmp);
  return name;
}

static const char *ny_effect_dict_get_callback_fact(ny_effects_ctx *ctx,
                                                    const expr_t *expr) {
  if (!ctx || !expr || expr->kind != NY_E_MEMCALL || !expr->as.memcall.name ||
      strcmp(expr->as.memcall.name, "get") != 0 || !expr->as.memcall.target ||
      expr->as.memcall.target->kind != NY_E_IDENT ||
      !expr->as.memcall.target->as.ident.name ||
      expr->as.memcall.args.len < 1 || expr->as.memcall.args.len > 2 ||
      expr->as.memcall.args.data[0].name ||
      (expr->as.memcall.args.len == 2 && expr->as.memcall.args.data[1].name &&
       strcmp(expr->as.memcall.args.data[1].name, "default") != 0))
    return NULL;
  const char *container = expr->as.memcall.target->as.ident.name;
  const char *type = ny_effect_local_container_type(ctx, container);
  if (!type || strcmp(type, "dict") != 0)
    return NULL;
  const char *key = NULL;
  if (ny_effect_dict_get_callback_expr(expr, &container, &key))
    return ny_effect_dict_callback_fact_name(ctx, container, key);
  return ny_effect_dict_callback_summary_fact_name(ctx, container);
}

static const char *ny_effect_dict_index_callback_fact(ny_effects_ctx *ctx,
                                                      const expr_t *expr) {
  if (!ctx || !expr || expr->kind != NY_E_INDEX || !expr->as.index.target ||
      expr->as.index.target->kind != NY_E_IDENT ||
      !expr->as.index.target->as.ident.name || !expr->as.index.start ||
      expr->as.index.stop || expr->as.index.step)
    return NULL;
  const char *container = expr->as.index.target->as.ident.name;
  const char *type = ny_effect_local_container_type(ctx, container);
  if (!type || strcmp(type, "dict") != 0)
    return NULL;
  const expr_t *key = expr->as.index.start;
  if (key->kind == NY_E_LITERAL && key->as.literal.kind == NY_LIT_STR &&
      key->as.literal.as.s.data)
    return ny_effect_dict_callback_fact_name(ctx, container,
                                             key->as.literal.as.s.data);
  return ny_effect_dict_callback_summary_fact_name(ctx, container);
}

static const char *ny_effect_list_callback_fact_name(ny_effects_ctx *ctx,
                                                     const char *container,
                                                     int64_t index) {
  if (!ctx || !ctx->cg || !ctx->cg->arena || !container || index < 0)
    return NULL;
  size_t container_len = strlen(container);
  size_t cap = container_len + 64;
  char *tmp = malloc(cap);
  if (!tmp)
    return NULL;
  int n = snprintf(tmp, cap, "\001list-callback:%zu:%s:%lld", container_len,
                   container, (long long)index);
  const char *name = n > 0 && (size_t)n < cap
                         ? arena_strndup(ctx->cg->arena, tmp, (size_t)n)
                         : NULL;
  free(tmp);
  return name;
}

static const char *
ny_effect_list_callback_summary_fact_name(ny_effects_ctx *ctx,
                                          const char *container) {
  if (!ctx || !ctx->cg || !ctx->cg->arena || !container)
    return NULL;
  size_t container_len = strlen(container);
  size_t cap = container_len + 64;
  char *tmp = malloc(cap);
  if (!tmp)
    return NULL;
  int n = snprintf(tmp, cap, "\001list-callback-summary:%zu:%s", container_len,
                   container);
  const char *name = n > 0 && (size_t)n < cap
                         ? arena_strndup(ctx->cg->arena, tmp, (size_t)n)
                         : NULL;
  free(tmp);
  return name;
}

static const char *ny_effect_list_index_callback_fact(ny_effects_ctx *ctx,
                                                      const expr_t *expr) {
  if (!ctx || !expr || expr->kind != NY_E_INDEX || !expr->as.index.target ||
      expr->as.index.target->kind != NY_E_IDENT ||
      !expr->as.index.target->as.ident.name || !expr->as.index.start ||
      expr->as.index.stop || expr->as.index.step)
    return NULL;
  const char *container = expr->as.index.target->as.ident.name;
  const char *type = ny_effect_local_container_type(ctx, container);
  if (!type || strcmp(type, "list") != 0)
    return NULL;
  if (expr->as.index.start->kind != NY_E_LITERAL ||
      !ny_literal_is_signed_i64(&expr->as.index.start->as.literal) ||
      expr->as.index.start->as.literal.as.i < 0)
    return ny_effect_list_callback_summary_fact_name(ctx, container);
  return ny_effect_list_callback_fact_name(
      ctx, container, expr->as.index.start->as.literal.as.i);
}

static const char *ny_effect_list_get_callback_fact(ny_effects_ctx *ctx,
                                                    const expr_t *expr) {
  if (!ctx || !expr || expr->kind != NY_E_MEMCALL || !expr->as.memcall.name ||
      strcmp(expr->as.memcall.name, "get") != 0 || !expr->as.memcall.target ||
      expr->as.memcall.target->kind != NY_E_IDENT ||
      !expr->as.memcall.target->as.ident.name ||
      expr->as.memcall.args.len < 1 || expr->as.memcall.args.len > 2 ||
      expr->as.memcall.args.data[0].name ||
      (expr->as.memcall.args.len == 2 && expr->as.memcall.args.data[1].name &&
       strcmp(expr->as.memcall.args.data[1].name, "default") != 0))
    return NULL;
  const expr_t *arg = expr->as.memcall.args.data[0].val;
  const char *container = expr->as.memcall.target->as.ident.name;
  const char *type = ny_effect_local_container_type(ctx, container);
  if (!type || strcmp(type, "list") != 0)
    return NULL;
  if (!arg || arg->kind != NY_E_LITERAL ||
      !ny_literal_is_signed_i64(&arg->as.literal) || arg->as.literal.as.i < 0)
    return ny_effect_list_callback_summary_fact_name(ctx, container);
  return ny_effect_list_callback_fact_name(ctx, container,
                                           arg->as.literal.as.i);
}

static expr_t *ny_effect_container_get_callback_fallback(const expr_t *expr) {
  if (expr && expr->kind == NY_E_CALL && expr->as.call.callee &&
      expr->as.call.callee->kind == NY_E_IDENT &&
      expr->as.call.callee->as.ident.name && expr->semantic.canonical_callee &&
      (strcmp(expr->semantic.canonical_callee, "core.get") == 0 ||
       strcmp(expr->semantic.canonical_callee, "core.reflect.get") == 0) &&
      expr->as.call.args.len == 3 && !expr->as.call.args.data[0].name &&
      !expr->as.call.args.data[1].name &&
      (!expr->as.call.args.data[2].name ||
       strcmp(expr->as.call.args.data[2].name, "default") == 0))
    return expr->as.call.args.data[2].val;
  if (!expr || expr->kind != NY_E_MEMCALL || !expr->as.memcall.name ||
      strcmp(expr->as.memcall.name, "get") != 0 ||
      expr->as.memcall.args.len != 2 || expr->as.memcall.args.data[0].name ||
      (expr->as.memcall.args.data[1].name &&
       strcmp(expr->as.memcall.args.data[1].name, "default") != 0))
    return NULL;
  return expr->as.memcall.args.data[1].val;
}

static const char *ny_effect_container_callback_fact(ny_effects_ctx *ctx,
                                                     const expr_t *expr) {
  if (!expr)
    return NULL;
  if (expr->kind == NY_E_CALL && expr->as.call.callee &&
      expr->as.call.callee->kind == NY_E_IDENT &&
      expr->as.call.callee->as.ident.name && expr->semantic.canonical_callee &&
      (strcmp(expr->semantic.canonical_callee, "core.get") == 0 ||
       strcmp(expr->semantic.canonical_callee, "core.reflect.get") == 0) &&
      expr->as.call.args.len >= 2 && expr->as.call.args.len <= 3 &&
      !expr->as.call.args.data[0].name && !expr->as.call.args.data[1].name &&
      (expr->as.call.args.len == 2 || !expr->as.call.args.data[2].name ||
       strcmp(expr->as.call.args.data[2].name, "default") == 0)) {
    const expr_t *container_expr = expr->as.call.args.data[0].val;
    const expr_t *key = expr->as.call.args.data[1].val;
    if (!container_expr || container_expr->kind != NY_E_IDENT ||
        !container_expr->as.ident.name)
      return NULL;
    const char *container = container_expr->as.ident.name;
    const char *type = ny_effect_local_container_type(ctx, container);
    if (type && strcmp(type, "dict") == 0) {
      if (key && key->kind == NY_E_LITERAL &&
          key->as.literal.kind == NY_LIT_STR && key->as.literal.as.s.data)
        return ny_effect_dict_callback_fact_name(ctx, container,
                                                 key->as.literal.as.s.data);
      return ny_effect_dict_callback_summary_fact_name(ctx, container);
    }
    if (type && strcmp(type, "list") == 0) {
      if (key && key->kind == NY_E_LITERAL &&
          ny_literal_is_signed_i64(&key->as.literal) &&
          key->as.literal.as.i >= 0)
        return ny_effect_list_callback_fact_name(ctx, container,
                                                 key->as.literal.as.i);
      return ny_effect_list_callback_summary_fact_name(ctx, container);
    }
    return NULL;
  }
  if (expr->kind == NY_E_MEMCALL) {
    const char *dict_fact = ny_effect_dict_get_callback_fact(ctx, expr);
    return dict_fact ? dict_fact : ny_effect_list_get_callback_fact(ctx, expr);
  }
  if (expr->kind == NY_E_INDEX) {
    const char *dict_fact = ny_effect_dict_index_callback_fact(ctx, expr);
    return dict_fact ? dict_fact
                     : ny_effect_list_index_callback_fact(ctx, expr);
  }
  return NULL;
}

static void
ny_effect_invalidate_container_callback_facts(ny_effects_ctx *ctx,
                                              const char *container) {
  if (!ctx || !container)
    return;
  const char *container_type = ny_effect_local_container_type(ctx, container);
  bool invalidate_dict = !container_type ||
                         strcmp(container_type, "dict") == 0 ||
                         strcmp(container_type, "any") == 0;
  bool invalidate_list = !container_type ||
                         strcmp(container_type, "list") == 0 ||
                         strcmp(container_type, "any") == 0;
  if (!invalidate_dict && !invalidate_list)
    return;
  /*
   * A local may be aliased before mutation, and the effect visitor does not
   * maintain points-to sets for dynamic containers. Invalidate all callback
   * facts for that container kind instead of trusting that the syntactic
   * receiver is the only alias. A statically distinct container kind cannot
   * alias this value and keeps its independent facts.
   */
  static const char *const prefixes[] = {
      "\001dict-callback:", "\001dict-callback-summary:", "\001list-callback:",
      "\001list-callback-summary:"};
  for (size_t i = 0; i < ctx->local_container_len; ++i) {
    ny_effect_local_fact_t *fact = &ctx->local_containers[i];
    if (!fact->name)
      continue;
    bool is_callback_fact = false;
    for (size_t p = 0; p < sizeof(prefixes) / sizeof(prefixes[0]); ++p) {
      if ((p < 2 && !invalidate_dict) || (p >= 2 && !invalidate_list))
        continue;
      if (strncmp(fact->name, prefixes[p], strlen(prefixes[p])) == 0) {
        is_callback_fact = true;
        break;
      }
    }
    if (!is_callback_fact)
      continue;
    fact->call_effects_known = false;
    fact->call_effects = NY_FX_ALL;
    fact->call_effect_row = NULL;
    fact->call_effect_row_known = false;
    fact->call_effect_custom_row_unknown = true;
  }
}

static void ny_effect_invalidate_container_alias_group_callback_facts(
    ny_effects_ctx *ctx, const char *container) {
  if (!ctx || !container)
    return;
  const char *group = ny_effect_local_container_alias_group(ctx, container);
  if (!group) {
    ny_effect_invalidate_container_callback_facts(ctx, container);
    return;
  }
  char fact_container[512];
  bool found_container_fact = false;
  for (size_t i = 0; i < ctx->local_container_len; ++i) {
    ny_effect_local_fact_t *fact = &ctx->local_containers[i];
    if (!fact->name ||
        !ny_effect_callback_fact_container_name(fact->name, fact_container,
                                                sizeof(fact_container)) ||
        strcmp(fact_container, container) != 0)
      continue;
    found_container_fact = true;
    if (!fact->container_alias_group ||
        strcmp(fact->container_alias_group, group) != 0) {
      ny_effect_invalidate_container_callback_facts(ctx, container);
      return;
    }
  }
  if (!found_container_fact)
    return;
  for (size_t i = 0; i < ctx->local_container_len; ++i) {
    ny_effect_local_fact_t *fact = &ctx->local_containers[i];
    if (!fact->name || !fact->container_alias_group ||
        strcmp(fact->container_alias_group, group) != 0)
      continue;
    fact->call_effects_known = false;
    fact->call_effects = NY_FX_ALL;
    fact->call_effect_row = NULL;
    fact->call_effect_row_known = false;
    fact->call_effect_custom_row_unknown = true;
  }
}

/*
 * `core.set_idx` detaches a shared local list before its exact indexed write.
 * Its other COW aliases therefore retain their callback facts.
 */
static void
ny_effect_invalidate_named_list_callback_facts(ny_effects_ctx *ctx,
                                               const char *container) {
  if (!ctx || !container)
    return;
  char index_prefix[1400], summary_prefix[1400];
  int index_len =
      snprintf(index_prefix, sizeof(index_prefix),
               "\001list-callback:%zu:%s:", strlen(container), container);
  int summary_len = snprintf(summary_prefix, sizeof(summary_prefix),
                             "\001list-callback-summary:%zu:%s",
                             strlen(container), container);
  if (index_len <= 0 || (size_t)index_len >= sizeof(index_prefix) ||
      summary_len <= 0 || (size_t)summary_len >= sizeof(summary_prefix))
    return;
  for (size_t i = 0; i < ctx->local_container_len; ++i) {
    ny_effect_local_fact_t *fact = &ctx->local_containers[i];
    if (!fact->name ||
        (strncmp(fact->name, index_prefix, (size_t)index_len) != 0 &&
         strncmp(fact->name, summary_prefix, (size_t)summary_len) != 0))
      continue;
    fact->call_effects_known = false;
    fact->call_effects = NY_FX_ALL;
    fact->call_effect_row = NULL;
    fact->call_effect_row_known = false;
    fact->call_effect_custom_row_unknown = true;
  }
}

static void ny_effect_copy_container_callback_facts_from(
    ny_effects_ctx *ctx, const ny_effects_ctx *source_ctx, const char *source,
    const char *destination);

static void ny_effect_copy_container_callback_facts(ny_effects_ctx *ctx,
                                                    const char *source,
                                                    const char *destination) {
  ny_effect_copy_container_callback_facts_from(ctx, ctx, source, destination);
}

static void ny_effect_copy_container_callback_facts_from(
    ny_effects_ctx *ctx, const ny_effects_ctx *source_ctx, const char *source,
    const char *destination) {
  if (!ctx || !source_ctx || !source || !destination)
    return;
  const char *alias_group =
      ny_effect_local_container_alias_group(source_ctx, source);
  if (!alias_group)
    alias_group = source;
  ny_effect_set_local_container_alias_group(ctx, destination, alias_group);
  if (ctx == source_ctx)
    ny_effect_set_callback_facts_alias_group(ctx, source, alias_group);
  char dict_prefix[1400], dict_summary_prefix[1400], list_prefix[1400];
  char list_summary_prefix[1400];
  int dict_len = snprintf(dict_prefix, sizeof(dict_prefix),
                          "\001dict-callback:%zu:%s:", strlen(source), source);
  int list_len = snprintf(list_prefix, sizeof(list_prefix),
                          "\001list-callback:%zu:%s:", strlen(source), source);
  int list_summary_len =
      snprintf(list_summary_prefix, sizeof(list_summary_prefix),
               "\001list-callback-summary:%zu:%s", strlen(source), source);
  int dict_summary_len =
      snprintf(dict_summary_prefix, sizeof(dict_summary_prefix),
               "\001dict-callback-summary:%zu:%s", strlen(source), source);
  if (dict_len <= 0 || (size_t)dict_len >= sizeof(dict_prefix) ||
      list_len <= 0 || (size_t)list_len >= sizeof(list_prefix) ||
      list_summary_len <= 0 ||
      (size_t)list_summary_len >= sizeof(list_summary_prefix) ||
      dict_summary_len <= 0 ||
      (size_t)dict_summary_len >= sizeof(dict_summary_prefix))
    return;
  size_t original_len = source_ctx->local_container_len;
  for (size_t i = 0; i < original_len; ++i) {
    ny_effect_local_fact_t original = source_ctx->local_containers[i];
    if (!original.name)
      continue;
    const char *suffix = NULL;
    const char *renamed = NULL;
    if (strncmp(original.name, dict_summary_prefix, (size_t)dict_summary_len) ==
        0) {
      renamed = ny_effect_dict_callback_summary_fact_name(ctx, destination);
    } else if (strncmp(original.name, list_summary_prefix,
                       (size_t)list_summary_len) == 0) {
      renamed = ny_effect_list_callback_summary_fact_name(ctx, destination);
    } else if (strncmp(original.name, dict_prefix, (size_t)dict_len) == 0) {
      suffix = original.name + dict_len;
      renamed = ny_effect_dict_callback_fact_name(ctx, destination, suffix);
    } else if (strncmp(original.name, list_prefix, (size_t)list_len) == 0) {
      suffix = original.name + list_len;
      char *end = NULL;
      long long index = strtoll(suffix, &end, 10);
      if (end && *suffix && !*end && index >= 0)
        renamed =
            ny_effect_list_callback_fact_name(ctx, destination, (int64_t)index);
    }
    if (!renamed)
      continue;
    size_t before = ctx->local_container_len;
    ny_effect_record_local_container(
        ctx, renamed, NULL, NULL, NULL, original.call_effects,
        original.call_effects_known, original.is_callable, false, 0, false,
        false, 0, false);
    if (ctx->local_container_len == before)
      continue;
    ny_effect_local_fact_t *copy =
        &ctx->local_containers[ctx->local_container_len - 1];
    copy->container_alias_group = alias_group;
    copy->call_effect_row = original.call_effect_row;
    copy->call_effect_row_known = original.call_effect_row_known;
    copy->call_effect_custom_row_unknown =
        original.call_effect_custom_row_unknown;
    copy->callable_expr = original.callable_expr;
  }
}

static bool ny_effect_local_is_dict(const ny_effects_ctx *ctx,
                                    const char *name) {
  const char *type = ny_effect_local_container_type(ctx, name);
  return type && strcmp(type, "dict") == 0;
}

static bool ny_effect_local_is_list(const ny_effects_ctx *ctx,
                                    const char *name) {
  const char *type = ny_effect_local_container_type(ctx, name);
  return type && strcmp(type, "list") == 0;
}

/*
 * Only recognize the canonical functional list append. User methods with the
 * same spelling may mutate their receiver and must keep the conservative path.
 */
static bool ny_effect_builtin_list_append(ny_effects_ctx *ctx,
                                          const expr_t *expr,
                                          const char **source,
                                          expr_t **appended) {
  if (source)
    *source = NULL;
  if (appended)
    *appended = NULL;
  if (!ctx || !expr || expr->kind != NY_E_MEMCALL || !expr->as.memcall.name ||
      strcmp(expr->as.memcall.name, "append") != 0 ||
      !expr->as.memcall.target || expr->as.memcall.target->kind != NY_E_IDENT ||
      !expr->as.memcall.target->as.ident.name ||
      expr->as.memcall.args.len != 1 || expr->as.memcall.args.data[0].name ||
      !expr->as.memcall.args.data[0].val ||
      !ny_effect_local_is_list(ctx, expr->as.memcall.target->as.ident.name))
    return false;
  fun_sig *sig = ny_purity_resolve_memcall_sig(
      ctx->cg, (expr_memcall_t *)&expr->as.memcall, ctx->local_names,
      ctx->local_hashes,
      ctx->local_bloom ? ctx->local_bloom : (uint64_t[4]){0});
  if (!sig || !ny_sig_is_std_for_effects(sig) || !sig->name ||
      (strcmp(sig->name, "core.append") != 0 &&
       strcmp(sig->name, "core.list.append") != 0))
    return false;
  if (source)
    *source = expr->as.memcall.target->as.ident.name;
  if (appended)
    *appended = expr->as.memcall.args.data[0].val;
  return true;
}

static bool ny_effect_callback_expr_summary(ny_effects_ctx *ctx,
                                            const expr_t *callback,
                                            uint32_t *effects) {
  if (!ctx || !callback)
    return false;
  if (callback->kind == NY_E_TERNARY) {
    uint32_t true_effects = NY_FX_ALL;
    uint32_t false_effects = NY_FX_ALL;
    bool true_known = ny_effect_callback_expr_summary(
        ctx, callback->as.ternary.true_expr, &true_effects);
    bool false_known = ny_effect_callback_expr_summary(
        ctx, callback->as.ternary.false_expr, &false_effects);
    if (!true_known || !false_known) {
      if (effects)
        *effects = NY_FX_ALL;
      return false;
    }
    if (effects)
      *effects = true_effects | false_effects;
    return true;
  }
  if (callback->kind == NY_E_LAMBDA || callback->kind == NY_E_FN)
    return ny_effect_lambda_summary(ctx, callback, effects);
  if (callback->kind == NY_E_MEMCALL) {
    const char *fact = ny_effect_dict_get_callback_fact(ctx, callback);
    if (!fact)
      fact = ny_effect_list_get_callback_fact(ctx, callback);

    if (fact)
      return ny_effect_local_callable(ctx, fact, effects);
  }
  if (callback->kind == NY_E_INDEX) {
    const char *fact = ny_effect_dict_index_callback_fact(ctx, callback);
    if (!fact)
      fact = ny_effect_list_index_callback_fact(ctx, callback);
    if (fact) {
      bool known = ny_effect_local_callable(ctx, fact, effects);
      return known;
    }
  }
  if (callback->kind == NY_E_CALL || callback->kind == NY_E_MEMCALL)
    return ny_effect_call_result_callable_summary(ctx, (expr_t *)callback,
                                                  effects, NULL);
  const char *name = NULL;
  char qualified[1280] = {0};
  if (callback->kind == NY_E_IDENT && callback->as.ident.name) {
    name = callback->as.ident.name;
    if (assigned_name_contains(
            ctx->local_names, ctx->local_hashes,
            ctx->local_bloom ? ctx->local_bloom : (uint64_t[4]){0}, name)) {
      if (ctx->callback_alias_state_ambiguous)
        return false;
      if (ny_effect_owner_row_callback(ctx, name)) {
        if (effects)
          *effects = NY_FX_NONE;
        return true;
      }
      return ny_effect_local_callable(ctx, name, effects);
    }
  } else if (callback->kind == NY_E_MEMBER && callback->as.member.target &&
             callback->as.member.name) {
    char function_path[1280];
    if (ny_effect_resolve_module_function_value(
            ctx->cg, callback, function_path, sizeof(function_path)))
      name = function_path;
    else if (callback->semantic.canonical_callee)
      name = callback->semantic.canonical_callee;
    else {
      const expr_t *root = callback;
      while (root && root->kind == NY_E_MEMBER)
        root = root->as.member.target;
      if (root && root->kind == NY_E_IDENT && root->as.ident.name &&
          !assigned_name_contains(ctx->local_names, ctx->local_hashes,
                                  ctx->local_bloom ? ctx->local_bloom
                                                   : (uint64_t[4]){0},
                                  root->as.ident.name) &&
          (strcmp(root->as.ident.name, "std") == 0 ||
           resolve_import_alias(ctx->cg, root->as.ident.name))) {
        size_t qualified_len = 0;
        if (ny_effect_expr_qualified_name(callback, qualified,
                                          sizeof(qualified), &qualified_len))
          name = qualified;
      }
    }
  } else {
    return false;
  }
  fun_sig *sig = callback->semantic.canonical_callee_stmt
                     ? ny_effect_sig_for_stmt(
                           ctx->cg, callback->semantic.canonical_callee_stmt)
                     : (name ? lookup_fun(ctx->cg, name, 0) : NULL);
  if (!sig || !ny_sig_effects_known(sig))
    return false;
  if (effects)
    *effects = ny_sig_effects(sig);
  return true;
}

static const char *ny_effect_sig_param_type(const fun_sig *sig, size_t index) {
  if (!sig)
    return NULL;
  if (index < sig->param_types.len && sig->param_types.data[index])
    return sig->param_types.data[index];
  if (sig->stmt_t && sig->stmt_t->kind == NY_S_FUNC &&
      index < sig->stmt_t->as.fn.params.len)
    return sig->stmt_t->as.fn.params.data[index].type;
  return NULL;
}

static bool ny_effect_param_is_optional(const char *type) {
  return type && type[0] == '?';
}

static bool ny_effect_param_is_callable(const char *type) {
  if (ny_effect_param_is_optional(type))
    type++;
  return type && (strcmp(type, "fnptr") == 0 || strcmp(type, "closure") == 0);
}

static bool ny_effect_callback_value_summary(ny_effects_ctx *ctx, expr_t *value,
                                             uint32_t *effects,
                                             bool *summary_known) {
  if (effects)
    *effects = NY_FX_ALL;
  if (summary_known)
    *summary_known = false;
  if (!ctx || !value)
    return false;
  bool callable = value->kind == NY_E_LAMBDA || value->kind == NY_E_FN;
  bool known = false;
  if (callable)
    known = ny_effect_lambda_summary(ctx, value, effects);
  else if (value->kind == NY_E_CALL) {
    known = ny_effect_call_result_callable_summary(ctx, value, effects, NULL);
    callable = known;
  } else if (value->kind == NY_E_MEMCALL) {
    known = ny_effect_callback_expr_summary(ctx, value, effects);
    callable = known;
  } else if (value->kind == NY_E_INDEX) {
    known = ny_effect_callback_expr_summary(ctx, value, effects);
    callable = known;
  } else if (value->kind == NY_E_TERNARY) {
    uint32_t true_effects = NY_FX_ALL;
    uint32_t false_effects = NY_FX_ALL;
    bool true_known = false;
    bool false_known = false;
    bool true_callable = ny_effect_callback_value_summary(
        ctx, value->as.ternary.true_expr, &true_effects, &true_known);
    bool false_callable = ny_effect_callback_value_summary(
        ctx, value->as.ternary.false_expr, &false_effects, &false_known);
    callable = true_callable && false_callable;
    known = callable && true_known && false_known;
    if (effects)
      *effects = known ? true_effects | false_effects : NY_FX_ALL;
  } else if (value->kind == NY_E_IDENT || value->kind == NY_E_MEMBER) {
    known = ny_effect_callback_expr_summary(ctx, value, effects);
    callable = known;
  }
  if (summary_known)
    *summary_known = known;
  return callable;
}

static bool ny_effect_merge_appended_list_callback_summary(
    ny_effects_ctx *ctx, const char *source, const char *destination,
    expr_t *appended_value) {
  if (!ctx || !source || !destination || !appended_value)
    return false;
  const char *source_name =
      ny_effect_list_callback_summary_fact_name(ctx, source);
  const ny_effect_local_fact_t *source_fact =
      source_name ? ny_effect_fact_latest(ctx->local_containers,
                                          ctx->local_container_len, source_name)
                  : NULL;
  if (!source_fact || !source_fact->is_callable ||
      !source_fact->call_effects_known || !source_fact->call_effect_row_known ||
      source_fact->call_effect_custom_row_unknown)
    return false;

  uint32_t appended_effects = NY_FX_ALL;
  bool appended_effects_known = false;
  if (!ny_effect_callback_value_summary(ctx, appended_value, &appended_effects,
                                        &appended_effects_known) ||
      !appended_effects_known)
    return false;

  bool appended_row_known = false;
  const ny_type_t *appended_row =
      ny_effect_callback_row(ctx, appended_value, &appended_row_known);
  if (!appended_row_known)
    return false;

  const ny_type_t *joined_row = source_fact->call_effect_row;
  if (appended_row) {
    if (!joined_row) {
      joined_row = appended_row;
    } else {
      joined_row =
          ny_type_row_compose(ctx->fact_arena, NULL, (ny_type_t *)joined_row,
                              (ny_type_t *)appended_row);
      if (!joined_row)
        return false;
    }
  }

  const char *destination_name =
      ny_effect_list_callback_summary_fact_name(ctx, destination);
  if (!destination_name)
    return false;
  ny_effect_record_local_container(ctx, destination_name, NULL, NULL, NULL,
                                   source_fact->call_effects | appended_effects,
                                   true, true, false, 0, false, false, 0,
                                   false);
  ny_effect_record_local_call_row(ctx, destination_name, joined_row, true);
  return true;
}

static bool ny_effect_owner_row_callback(const ny_effects_ctx *ctx,
                                         const char *name) {
  if (!ctx || !ctx->owner_fn || !name || !ny_effect_owner_contract_known(ctx) ||
      !ny_effect_type_is_open(ny_effect_owner_contract_row(ctx)))
    return false;
  for (size_t i = 0; i < ctx->owner_fn->params.len; ++i) {
    const param_t *param = &ctx->owner_fn->params.data[i];
    if (param->name && strcmp(param->name, name) == 0 &&
        ny_effect_param_is_callable(param->type))
      return true;
  }
  return false;
}

static const char *ny_effect_sig_param_name(const fun_sig *sig, size_t index) {
  if (!sig || !sig->stmt_t || sig->stmt_t->kind != NY_S_FUNC ||
      index >= sig->stmt_t->as.fn.params.len)
    return NULL;
  return sig->stmt_t->as.fn.params.data[index].name;
}

static expr_t *ny_effect_hof_actual(const fun_sig *sig,
                                    const ny_call_arg_list *args,
                                    size_t param_index, size_t receiver_skip) {
  if (!args || !sig || param_index < receiver_skip)
    return NULL;
  const char *param_name = ny_effect_sig_param_name(sig, param_index);
  size_t positional = 0;
  for (size_t i = 0; i < args->len; ++i) {
    const call_arg_t *arg = &args->data[i];
    if (arg->name) {
      if (param_name && strcmp(param_name, arg->name) == 0)
        return arg->val;
      continue;
    }
    if (positional == param_index - receiver_skip)
      return arg->val;
    positional++;
  }
  return NULL;
}

/*
 * Returns whether the signature has a builtin or open-row callback contract,
 * and separately reports whether every supplied callback has a known summary.
 */
static bool ny_effect_stmt_contains_return(const stmt_t *stmt, unsigned depth) {
  if (!stmt || depth > 64)
    return depth > 64;
  if (stmt->kind == NY_S_RETURN)
    return true;
  switch (stmt->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < stmt->as.block.body.len; ++i)
      if (ny_effect_stmt_contains_return(stmt->as.block.body.data[i],
                                         depth + 1))
        return true;
    return false;
  case NY_S_IF:
    return ny_effect_stmt_contains_return(stmt->as.iff.init, depth + 1) ||
           ny_effect_stmt_contains_return(stmt->as.iff.conseq, depth + 1) ||
           ny_effect_stmt_contains_return(stmt->as.iff.alt, depth + 1);
  case NY_S_GUARD:
    return ny_effect_stmt_contains_return(stmt->as.guard.fallback, depth + 1);
  case NY_S_WHILE:
    return ny_effect_stmt_contains_return(stmt->as.whl.init, depth + 1) ||
           ny_effect_stmt_contains_return(stmt->as.whl.body, depth + 1) ||
           ny_effect_stmt_contains_return(stmt->as.whl.update, depth + 1);
  case NY_S_FOR:
    return ny_effect_stmt_contains_return(stmt->as.fr.init, depth + 1) ||
           ny_effect_stmt_contains_return(stmt->as.fr.body, depth + 1) ||
           ny_effect_stmt_contains_return(stmt->as.fr.update, depth + 1);
  case NY_S_TRY:
    return ny_effect_stmt_contains_return(stmt->as.tr.body, depth + 1) ||
           ny_effect_stmt_contains_return(stmt->as.tr.handler, depth + 1);
  case NY_S_MATCH:
    for (size_t i = 0; i < stmt->as.match.arms.len; ++i)
      if (ny_effect_stmt_contains_return(stmt->as.match.arms.data[i].conseq,
                                         depth + 1))
        return true;
    return ny_effect_stmt_contains_return(stmt->as.match.default_conseq,
                                          depth + 1);
  case NY_S_DEFER:
    return ny_effect_stmt_contains_return(stmt->as.de.body, depth + 1);
  default:
    /*
     * A nested function or lambda has its own return boundary.
     */
    return false;
  }
}

static bool ny_effect_simple_callback_return(const stmt_t *stmt,
                                             expr_t **value) {
  const stmt_t *ret = stmt;
  if (ret && ret->kind == NY_S_BLOCK) {
    if (ret->as.block.body.len != 1)
      return false;
    ret = ret->as.block.body.data[0];
  }
  if (!ret || ret->kind != NY_S_RETURN || !ret->as.ret.value)
    return false;
  expr_t *expr = ret->as.ret.value;
  if (expr->kind != NY_E_LAMBDA && expr->kind != NY_E_FN &&
      expr->kind != NY_E_IDENT && expr->kind != NY_E_TERNARY)
    return false;
  if (value)
    *value = expr;
  return true;
}

static bool ny_effect_callback_store_value_is_stable(const expr_t *value) {
  return value && (value->kind == NY_E_IDENT || value->kind == NY_E_LITERAL);
}

static bool ny_effect_callback_return_test_supported(const expr_t *test,
                                                     unsigned depth) {
  if (!test || depth > 32)
    return false;
  if (test->kind == NY_E_IDENT || test->kind == NY_E_LITERAL)
    return true;
  if (test->kind == NY_E_UNARY && test->as.unary.op &&
      strcmp(test->as.unary.op, "!") == 0)
    return ny_effect_callback_return_test_supported(test->as.unary.right,
                                                    depth + 1);
  return test->kind == NY_E_LOGICAL && test->as.logical.op &&
         (strcmp(test->as.logical.op, "&&") == 0 ||
          strcmp(test->as.logical.op, "||") == 0) &&
         ny_effect_callback_return_test_supported(test->as.logical.left,
                                                  depth + 1) &&
         ny_effect_callback_return_test_supported(test->as.logical.right,
                                                  depth + 1);
}

static bool ny_effect_callback_return_same(const expr_t *left,
                                           const expr_t *right) {
  if (left == right)
    return true;
  return left && right && left->kind == NY_E_IDENT &&
         right->kind == NY_E_IDENT && left->as.ident.name &&
         right->as.ident.name &&
         strcmp(left->as.ident.name, right->as.ident.name) == 0;
}

/*
 * Bound inference to straight-line chains of pure boolean tests whose arms
 * immediately return callbacks. Leading local callback bindings are visited
 * separately so returned closures retain their capture summaries.
 */
#define NY_EFFECT_BOUNDED_CALLBACK_RETURN_LIMIT 16u

static bool ny_effect_callback_return_prefix_supported(const stmt_t *stmt) {
  if (!stmt || stmt->kind != NY_S_VAR || !stmt->as.var.is_decl ||
      stmt->as.var.names.len == 0 ||
      stmt->as.var.exprs.len < stmt->as.var.names.len)
    return false;
  for (size_t i = 0; i < stmt->as.var.names.len; ++i) {
    const expr_t *value = stmt->as.var.exprs.data[i];
    if (!value || (value->kind != NY_E_LAMBDA && value->kind != NY_E_FN &&
                   value->kind != NY_E_IDENT))
      return false;
  }
  return true;
}

static bool ny_effect_collect_callback_return_branch(const stmt_t *stmt,
                                                     expr_t **values,
                                                     size_t capacity,
                                                     size_t *count,
                                                     unsigned depth) {
  if (!stmt || !values || !count || depth > 32)
    return false;
  if (stmt->kind == NY_S_BLOCK) {
    if (stmt->as.block.body.len != 1)
      return false;
    return ny_effect_collect_callback_return_branch(
        stmt->as.block.body.data[0], values, capacity, count, depth + 1);
  }
  expr_t *value = NULL;
  if (ny_effect_simple_callback_return(stmt, &value)) {
    if (*count >= capacity)
      return false;
    values[(*count)++] = value;
    return true;
  }
  if (stmt->kind != NY_S_IF || stmt->as.iff.init || !stmt->as.iff.alt ||
      !ny_effect_callback_return_test_supported(stmt->as.iff.test, 0))
    return false;
  return ny_effect_collect_callback_return_branch(stmt->as.iff.conseq, values,
                                                  capacity, count, depth + 1) &&
         ny_effect_collect_callback_return_branch(stmt->as.iff.alt, values,
                                                  capacity, count, depth + 1);
}

static bool ny_effect_collect_bounded_callback_returns(const stmt_t *function,
                                                       expr_t **values,
                                                       size_t capacity,
                                                       size_t *count,
                                                       size_t *prefix_count) {
  if (count)
    *count = 0;
  if (prefix_count)
    *prefix_count = 0;
  if (!function || function->kind != NY_S_FUNC || !function->as.fn.body ||
      function->as.fn.body->kind != NY_S_BLOCK || !values || !count ||
      !prefix_count)
    return false;
  const stmt_t *body = function->as.fn.body;
  size_t len = body->as.block.body.len;
  size_t start = 0;
  while (start < len && body->as.block.body.data[start] &&
         body->as.block.body.data[start]->kind != NY_S_IF &&
         body->as.block.body.data[start]->kind != NY_S_MATCH) {
    if (!ny_effect_callback_return_prefix_supported(
            body->as.block.body.data[start]))
      return false;
    start++;
  }
  if (start >= len)
    return false;
  if (body->as.block.body.data[start]->kind == NY_S_MATCH) {
    const stmt_t *match = body->as.block.body.data[start];
    size_t tail_len = len - start;
    bool has_default = match->as.match.default_conseq != NULL;
    if (match->as.match.arms.len == 0 || (has_default && tail_len != 1) ||
        (!has_default && tail_len != 2) || match->as.match.arms.len >= capacity)
      return false;
    for (size_t i = 0; i < match->as.match.arms.len; ++i)
      if (!ny_effect_simple_callback_return(match->as.match.arms.data[i].conseq,
                                            &values[i]))
        return false;
    const stmt_t *fallback = has_default ? match->as.match.default_conseq
                                         : body->as.block.body.data[start + 1];
    if (!ny_effect_simple_callback_return(fallback,
                                          &values[match->as.match.arms.len]))
      return false;
    *count = match->as.match.arms.len + 1;
    *prefix_count = start;
    return true;
  }
  size_t return_count = len - start;
  if (return_count == 1 && body->as.block.body.data[start]->kind == NY_S_IF &&
      body->as.block.body.data[start]->as.iff.alt) {
    size_t branch_count = 0;
    if (!ny_effect_collect_callback_return_branch(
            body->as.block.body.data[start], values, capacity, &branch_count,
            0))
      return false;
    *count = branch_count;
    *prefix_count = start;
    return true;
  }
  if (return_count < 2 || return_count > capacity)
    return false;
  for (size_t i = 0; i + 1 < return_count; ++i) {
    const stmt_t *branch = body->as.block.body.data[start + i];
    const expr_t *test =
        branch && branch->kind == NY_S_IF ? branch->as.iff.test : NULL;
    if (!branch || branch->kind != NY_S_IF || branch->as.iff.init ||
        branch->as.iff.alt ||
        !ny_effect_callback_return_test_supported(test, 0) ||
        !ny_effect_simple_callback_return(branch->as.iff.conseq, &values[i]))
      return false;
  }
  if (!ny_effect_simple_callback_return(body->as.block.body.data[len - 1],
                                        &values[return_count - 1]))
    return false;
  *count = return_count;
  *prefix_count = start;
  return true;
}

static bool ny_effect_stmt_returns_callback(ny_effects_ctx *ctx,
                                            const stmt_t *function,
                                            unsigned depth) {
  if (!function || function->kind != NY_S_FUNC || !function->as.fn.body ||
      !ny_effect_param_is_callable(function->as.fn.return_type) || depth > 16)
    return false;
  expr_t *bounded_returns[NY_EFFECT_BOUNDED_CALLBACK_RETURN_LIMIT] = {NULL};
  size_t bounded_return_count = 0, bounded_prefix_count = 0;
  if (ny_effect_collect_bounded_callback_returns(
          function, bounded_returns, NY_EFFECT_BOUNDED_CALLBACK_RETURN_LIMIT,
          &bounded_return_count, &bounded_prefix_count))
    return true;
  const stmt_t *body = function->as.fn.body;
  bool direct_return_only = true;
  if (body->kind == NY_S_BLOCK) {
    if (body->as.block.body.len == 0)
      return false;
    size_t return_index = body->as.block.body.len - 1;
    for (size_t i = 0; i < return_index; ++i)
      if (ny_effect_stmt_contains_return(body->as.block.body.data[i], 0))
        return false;
    direct_return_only = return_index == 0;
    body = body->as.block.body.data[return_index];
  }
  if (!body || body->kind != NY_S_RETURN || !body->as.ret.value)
    return false;
  const expr_t *value = body->as.ret.value;
  if (value->kind == NY_E_LAMBDA || value->kind == NY_E_FN ||
      value->kind == NY_E_IDENT || value->kind == NY_E_TERNARY)
    return true;
  if (value->kind != NY_E_CALL || !direct_return_only || !ctx || !ctx->cg)
    return false;
  uint64_t zero_bloom[4] = {0};
  fun_sig *inner_sig = ny_purity_resolve_call_sig(
      ctx->cg, (expr_call_t *)&value->as.call, ctx->local_names,
      ctx->local_hashes, ctx->local_bloom ? ctx->local_bloom : zero_bloom);
  return inner_sig && ny_effect_param_is_callable(inner_sig->return_type) &&
         ny_effect_stmt_returns_callback(ctx, inner_sig->stmt_t, depth + 1);
}

static bool ny_effect_sig_returns_callback(ny_effects_ctx *ctx,
                                           const fun_sig *sig) {
  return sig && ny_effect_param_is_callable(sig->return_type) &&
         ny_effect_stmt_returns_callback(ctx, sig->stmt_t, 0);
}

static bool ny_effect_hof_callback_summary(
    ny_effects_ctx *ctx, const fun_sig *sig, const ny_call_arg_list *args,
    size_t receiver_skip, uint32_t *effects, bool *callbacks_known) {
  if (effects)
    *effects = 0;
  if (callbacks_known)
    *callbacks_known = true;
  if (!ctx || !sig)
    return false;
  bool has_contract = ny_sig_builtin_contract(sig, NULL) ||
                      ny_sig_has_polymorphic_effect_contract(sig);
  if (ny_sig_is_std_for_effects(sig)) {
    int contract_kind = ny_builtin_contract_kind(sig->name);
    if (contract_kind == NY_BUILTIN_EFFECT_CALLABLE_FACTORY ||
        contract_kind == NY_BUILTIN_EFFECT_QUERY_PREDICATE_STEP)
      return false;
  }
  bool has_callback_param = false;
  for (size_t i = 0; i < (size_t)(sig->arity < 0 ? 0 : sig->arity); ++i) {
    const char *type = ny_effect_sig_param_type(sig, i);
    has_callback_param |= ny_effect_param_is_callable(type);
  }
  if (!has_contract && has_callback_param && ny_effect_diag_verbose_enabled())
    ny_effect_diag_log(
        "hof.resolve",
        "sig=%s module=%s callback_contract=missing callback_row=unresolved",
        sig->name ? sig->name : "<unnamed>",
        sig->module_name ? sig->module_name : "<unregistered>");
  /*
   * A user factory returning the lambda defers its callback parameter. Its
   * argument row belongs to the returned callable summary, not this call.
   */
  if (!has_contract && has_callback_param &&
      ny_effect_sig_returns_callback(ctx, sig))
    return false;
  /*
   * A stdlib function can have a conservative known-all base summary while
   * its higher-order callback row remains unknown. Preserve that distinction
   * even when no audited callback contract exists for this overload.
   */
  if (!has_contract && has_callback_param) {
    if (effects)
      *effects = NY_FX_ALL;
    if (callbacks_known)
      *callbacks_known = false;
    return true;
  }
  if (!has_contract)
    return false;
  bool found = false;
  bool known = true;
  uint32_t combined = 0;
  for (size_t i = 0; i < (size_t)(sig->arity < 0 ? 0 : sig->arity); ++i) {
    const char *type = ny_effect_sig_param_type(sig, i);
    if (ny_effect_diag_verbose_enabled())
      ny_effect_diag_log("hof.formal", "sig=%s index=%zu name=%s type=%s",
                         sig->name ? sig->name : "<unnamed>", i,
                         ny_effect_sig_param_name(sig, i)
                             ? ny_effect_sig_param_name(sig, i)
                             : "<unnamed>",
                         type ? type : "<none>");
    if (!ny_effect_param_is_callable(type))
      continue;
    found = true;
    expr_t *actual = ny_effect_hof_actual(sig, args, i, receiver_skip);
    /*
     * Some desugared module calls keep their namespace target in the member
     * node but resolve to a free-function signature.  In that shape there is
     * no synthetic receiver argument in `args`; retry the formal-to-actual
     * mapping without the receiver offset when the first mapping has no
     * source argument.
     */
    if (!actual && receiver_skip)
      actual = ny_effect_hof_actual(sig, args, i, 0);
    if (!actual && ny_effect_param_is_optional(type) &&
        ny_sig_is_std_for_effects(sig) &&
        ny_builtin_contract_kind(sig->name) == NY_BUILTIN_EFFECT_OPTIONAL_HOF)
      continue;
    bool supplied_by_open_row =
        actual && actual->kind == NY_E_IDENT && actual->as.ident.name &&
        ny_effect_owner_row_callback(ctx, actual->as.ident.name);
    uint32_t callback_effects = supplied_by_open_row ? NY_FX_NONE : NY_FX_ALL;
    bool callback_known =
        supplied_by_open_row ||
        ny_effect_callback_expr_summary(ctx, actual, &callback_effects);
    if (ny_effect_diag_verbose_enabled())
      ny_effect_diag_log(
          "hof.actual",
          "sig=%s formal=%s index=%zu receiver_skip=%zu actual_kind=%d "
          "actual=%s known=%d effects=0x%x",
          sig->name ? sig->name : "<unnamed>",
          ny_effect_sig_param_name(sig, i) ? ny_effect_sig_param_name(sig, i)
                                           : "<unnamed>",
          i, receiver_skip, actual ? (int)actual->kind : -1,
          actual && actual->kind == NY_E_IDENT && actual->as.ident.name
              ? actual->as.ident.name
              : "<non-ident>",
          callback_known, callback_effects);
    if (!callback_known) {
      known = false;
      combined |= NY_FX_ALL;
    } else {
      combined |= callback_effects;
    }
  }
  if (effects)
    *effects = combined;
  if (callbacks_known)
    *callbacks_known = known;
  return found;
}

static bool ny_effect_append_member_path(const expr_t *expr, char *out,
                                         size_t out_cap, size_t *out_len,
                                         unsigned depth) {
  if (!expr || !out || !out_len || depth >= 64)
    return false;
  if (expr->kind == NY_E_IDENT && expr->as.ident.name) {
    size_t len = strlen(expr->as.ident.name);
    if (len >= out_cap - *out_len)
      return false;
    memcpy(out + *out_len, expr->as.ident.name, len);
    *out_len += len;
    out[*out_len] = '\0';
    return true;
  }
  if (expr->kind != NY_E_MEMBER || !expr->as.member.target ||
      !expr->as.member.name ||
      !ny_effect_append_member_path(expr->as.member.target, out, out_cap,
                                    out_len, depth + 1))
    return false;
  size_t name_len = strlen(expr->as.member.name);
  if (*out_len + 1 >= out_cap || name_len >= out_cap - *out_len - 1)
    return false;
  out[(*out_len)++] = '.';
  memcpy(out + *out_len, expr->as.member.name, name_len);
  *out_len += name_len;
  out[*out_len] = '\0';
  return true;
}

static fun_sig *ny_effect_callback_sig(ny_effects_ctx *ctx,
                                       const expr_t *callback) {
  if (!ctx || !callback)
    return NULL;
  if (callback->semantic.canonical_callee_stmt)
    return ny_effect_sig_for_stmt(ctx->cg,
                                  callback->semantic.canonical_callee_stmt);

  const char *name = NULL;
  char function_path[1280];
  if (callback->kind == NY_E_IDENT && callback->as.ident.name) {
    if (assigned_name_contains(ctx->local_names, ctx->local_hashes,
                               ctx->local_bloom ? ctx->local_bloom
                                                : (uint64_t[4]){0},
                               callback->as.ident.name))
      return NULL;
    name = callback->as.ident.name;
  } else if (callback->kind == NY_E_MEMBER && callback->as.member.target &&
             callback->as.member.name) {
    if (ny_effect_resolve_module_function_value(
            ctx->cg, callback, function_path, sizeof(function_path)))
      name = function_path;
    if (!name && callback->semantic.canonical_callee)
      name = callback->semantic.canonical_callee;
  }
  return name ? lookup_fun(ctx->cg, name, 0) : NULL;
}

static const ny_type_t *ny_effect_callback_row(ny_effects_ctx *ctx,
                                               const expr_t *callback,
                                               bool *known) {
  if (known)
    *known = false;
  if (!ctx || !callback)
    return NULL;
  if (callback->kind == NY_E_TERNARY) {
    bool true_known = false;
    bool false_known = false;
    const ny_type_t *true_row = ny_effect_callback_row(
        ctx, callback->as.ternary.true_expr, &true_known);
    const ny_type_t *false_row = ny_effect_callback_row(
        ctx, callback->as.ternary.false_expr, &false_known);
    if (!true_known || !false_known)
      return NULL;
    if (!true_row || !false_row) {
      if (known)
        *known = true;
      return true_row ? true_row : false_row;
    }
    ny_type_t *joined = ny_type_row_compose(
        ctx->fact_arena, NULL, (ny_type_t *)true_row, (ny_type_t *)false_row);
    if (known)
      *known = joined != NULL;
    return joined;
  }
  if (callback->kind == NY_E_CALL) {
    uint32_t effects = NY_FX_ALL;
    const ny_type_t *row = NULL;
    bool summary_known = ny_effect_call_result_callable_summary(
        ctx, (expr_t *)callback, &effects, &row);
    if (summary_known) {
      if (known)
        *known = true;
      return row;
    }
  }
  if (callback->semantic.canonical_callee_stmt) {
    fun_sig *sig = ny_effect_sig_for_stmt(
        ctx->cg, callback->semantic.canonical_callee_stmt);
    if (known)
      *known = ny_sig_effects_known(sig);
    return ny_effect_sig_row(sig);
  }
  if (callback->kind == NY_E_IDENT && callback->as.ident.name &&
      assigned_name_contains(ctx->local_names, ctx->local_hashes,
                             ctx->local_bloom ? ctx->local_bloom
                                              : (uint64_t[4]){0},
                             callback->as.ident.name)) {
    if (ny_effect_local_custom_row_unknown(ctx, callback->as.ident.name))
      return NULL;
    const ny_type_t *row =
        ny_effect_local_call_row(ctx, callback->as.ident.name, known);
    if ((!known || !*known) &&
        ny_effect_owner_row_callback(ctx, callback->as.ident.name)) {
      bool custom_known = false;
      row = ny_effect_custom_row_only(
          ctx->fact_arena, ny_effect_owner_contract_row(ctx), &custom_known);
      if (known)
        *known = custom_known;
    }
    return row;
  }
  if (callback->kind == NY_E_MEMCALL) {
    const char *fact = ny_effect_dict_get_callback_fact(ctx, callback);
    if (!fact)
      fact = ny_effect_list_get_callback_fact(ctx, callback);
    if (fact) {
      if (ny_effect_local_custom_row_unknown(ctx, fact))
        return NULL;
      return ny_effect_local_call_row(ctx, fact, known);
    }
  }
  if (callback->kind == NY_E_INDEX) {
    const char *fact = ny_effect_dict_index_callback_fact(ctx, callback);
    if (!fact)
      fact = ny_effect_list_index_callback_fact(ctx, callback);
    if (fact) {
      if (ny_effect_local_custom_row_unknown(ctx, fact))
        return NULL;
      return ny_effect_local_call_row(ctx, fact, known);
    }
  }
  if (callback->kind == NY_E_LAMBDA || callback->kind == NY_E_FN) {
    uint32_t effects = NY_FX_ALL;
    if (known)
      *known = ny_effect_lambda_summary(ctx, callback, &effects);
    return NULL;
  }
  fun_sig *sig = ny_effect_callback_sig(ctx, callback);
  if (!sig)
    return NULL;
  if (known)
    *known = ny_sig_effects_known(sig);
  return ny_effect_sig_row(sig);
}

static void ny_effect_record_callback_value_row(ny_effects_ctx *ctx,
                                                const char *name,
                                                const expr_t *value,
                                                bool callable) {
  if (!callable)
    return;
  bool known = false;
  const ny_type_t *row = ny_effect_callback_row(ctx, value, &known);
  if (!known && value && value->kind == NY_E_IDENT && value->as.ident.name &&
      ny_effect_owner_row_callback(ctx, value->as.ident.name)) {
    row = ny_effect_custom_row_only(ctx->fact_arena,
                                    ny_effect_owner_contract_row(ctx), &known);
  }
  ny_effect_record_local_call_row(ctx, name, row, known);
  const expr_t *callable_expr =
      value && (value->kind == NY_E_LAMBDA || value->kind == NY_E_FN) ? value
      : value && value->kind == NY_E_IDENT && value->as.ident.name
          ? ny_effect_local_callable_expr(ctx, value->as.ident.name)
          : NULL;
  ny_effect_record_local_callable_expr(ctx, name, callable_expr);
}

/*
 * The mask summary accounts for builtin callback effects. Named rows have no
 * builtin mask bit, so check them separately at generic callback call sites.
 */
static bool ny_effect_hof_custom_rows_covered(ny_effects_ctx *ctx,
                                              const fun_sig *sig,
                                              const ny_call_arg_list *args,
                                              size_t receiver_skip) {
  if (!ctx || !sig || !ny_sig_has_polymorphic_effect_contract(sig))
    return true;
  for (size_t i = 0; i < (size_t)(sig->arity < 0 ? 0 : sig->arity); ++i) {
    if (!ny_effect_param_is_callable(ny_effect_sig_param_type(sig, i)))
      continue;
    expr_t *actual = ny_effect_hof_actual(sig, args, i, receiver_skip);
    if (!actual && receiver_skip)
      actual = ny_effect_hof_actual(sig, args, i, 0);
    if (actual && actual->kind == NY_E_IDENT && actual->as.ident.name &&
        ny_effect_local_custom_row_unknown(ctx, actual->as.ident.name))
      return false;
    bool callback_row_known = false;
    const ny_type_t *callback_row =
        ny_effect_callback_row(ctx, actual, &callback_row_known);
    if (!callback_row_known)
      continue;
    if (!ny_effect_row_has_nonbuiltin_content(callback_row))
      continue;
    if (!ny_effect_owner_contract_known(ctx) ||
        !ny_effect_row_covers(callback_row, ny_effect_owner_contract_row(ctx)))
      return false;
  }
  return true;
}

static uint32_t
ny_stmt_effects(codegen_t *cg, stmt_t *s, assigned_name_list *local_names,
                assigned_hash_list *local_hashes, uint64_t local_bloom[4],
                const ny_param_list *params, const stmt_func_t *owner_fn,
                const ny_type_t *owner_effect_row,
                bool owner_effect_contract_known, bool *known);

static bool ny_effect_nested_attr_text(const expr_t *arg, const char **text,
                                       size_t *len) {
  if (!arg || !text || !len)
    return false;
  if (arg->kind == NY_E_IDENT && arg->as.ident.name) {
    *text = arg->as.ident.name;
    *len = strlen(arg->as.ident.name);
    return true;
  }
  if (arg->kind == NY_E_LITERAL && arg->as.literal.kind == NY_LIT_STR &&
      arg->as.literal.as.s.data) {
    *text = arg->as.literal.as.s.data;
    *len = arg->as.literal.as.s.len;
    return true;
  }
  return false;
}

static bool ny_effect_nested_builtin_mask(const char *name, size_t len,
                                          uint32_t *mask) {
  if (!name || !len || !mask)
    return false;
#define NY_EFFECT_NESTED_NAME(spelling, bit)                                   \
  if (len == sizeof(spelling) - 1 &&                                           \
      memcmp(name, spelling, sizeof(spelling) - 1) == 0) {                     \
    *mask = bit;                                                               \
    return true;                                                               \
  }
  NY_EFFECT_NESTED_NAME("io", NY_FX_IO)
  NY_EFFECT_NESTED_NAME("alloc", NY_FX_ALLOC)
  NY_EFFECT_NESTED_NAME("ffi", NY_FX_FFI)
  NY_EFFECT_NESTED_NAME("thread", NY_FX_THREAD)
  NY_EFFECT_NESTED_NAME("read", NY_FX_READ_MEMORY)
  NY_EFFECT_NESTED_NAME("read_memory", NY_FX_READ_MEMORY)
  NY_EFFECT_NESTED_NAME("write", NY_FX_WRITE_MEMORY)
  NY_EFFECT_NESTED_NAME("write_memory", NY_FX_WRITE_MEMORY)
  NY_EFFECT_NESTED_NAME("all", NY_FX_ALL)
  NY_EFFECT_NESTED_NAME("none", NY_FX_NONE)
  NY_EFFECT_NESTED_NAME("pure", NY_FX_NONE)
#undef NY_EFFECT_NESTED_NAME
  return false;
}

/*
 * Nested declarations are summarized before
 * legacy codegen resolves their attributes.
 * Resolve the closed effect-row subset here so
 * a nested callable does not become an
 * unknown-call barrier merely because its
 * signature has not yet been registered.
 */
static const char *ny_effect_owner_module(const ny_effects_ctx *ctx) {
  if (!ctx || !ctx->cg)
    return NULL;
  for (size_t i = 0; ctx->owner_fn && i < ctx->cg->fun_sigs.len; ++i) {
    const fun_sig *sig = &ctx->cg->fun_sigs.data[i];
    if (sig->stmt_t && sig->stmt_t->kind == NY_S_FUNC &&
        &sig->stmt_t->as.fn == ctx->owner_fn && sig->module_name)
      return sig->module_name;
  }
  return ctx->cg->current_module_name;
}

static bool ny_effect_nested_declared_row(ny_effects_ctx *ctx,
                                          const stmt_t *function,
                                          const ny_type_t **row_out) {
  if (!ctx || !ctx->fact_arena || !function || function->kind != NY_S_FUNC ||
      !row_out)
    return false;
  const attribute_t *effects_attr = NULL;
  for (size_t i = 0; i < function->attributes.len; ++i) {
    const attribute_t *attr = &function->attributes.data[i];
    if (attr->name && strcmp(attr->name, "effects") == 0) {
      effects_attr = attr;
      break;
    }
  }
  if (!effects_attr)
    return false;

  uint32_t builtin_mask = NY_FX_NONE;
  ny_type_t *row = NULL;
  bool saw_effect = false;
  for (size_t i = 0; i < effects_attr->args.len; ++i) {
    const expr_t *arg = effects_attr->args.data[i];
    const char *callee = NULL;
    if (arg && arg->kind == NY_E_CALL && arg->as.call.callee &&
        arg->as.call.callee->kind == NY_E_IDENT)
      callee = arg->as.call.callee->as.ident.name;
    if (callee &&
        (strcmp(callee, "cap") == 0 || strcmp(callee, "capability") == 0)) {
      if (!arg->as.call.args.len)
        return false;
      for (size_t cap_i = 0; cap_i < arg->as.call.args.len; ++cap_i) {
        const char *name = NULL;
        size_t len = 0;
        if (!ny_effect_nested_attr_text(arg->as.call.args.data[cap_i].val,
                                        &name, &len) ||
            !len || len > NY_TYPE_CAPABILITY_NAME_MAX)
          return false;
        char cap_name[NY_TYPE_CAPABILITY_NAME_MAX + 1];
        memcpy(cap_name, name, len);
        cap_name[len] = '\0';
        char identity[NY_TYPE_CAPABILITY_NAME_MAX + 1];
        const char *module = ny_effect_owner_module(ctx);
        if (!ny_type_capability_identity(module, cap_name, identity))
          return false;
        row = ny_type_capability_effect(ctx->fact_arena, identity, row);
        if (!row)
          return false;
        saw_effect = true;
      }
      continue;
    }
    const char *name = NULL;
    size_t len = 0;
    uint32_t bit = NY_FX_NONE;
    if (!ny_effect_nested_attr_text(arg, &name, &len) ||
        !ny_effect_nested_builtin_mask(name, len, &bit))
      return false;
    builtin_mask |= bit;
    saw_effect = true;
  }
  if (!saw_effect)
    return false;
  *row_out =
      ny_type_effect_row_with_builtin_mask(ctx->fact_arena, builtin_mask, row);
  return *row_out != NULL;
}

static bool ny_effect_nested_function_summary(ny_effects_ctx *ctx,
                                              stmt_t *function,
                                              const ny_type_t *declared_row,
                                              uint32_t *effects) {
  if (!ctx || !function || function->kind != NY_S_FUNC || !function->as.fn.body)
    return false;
  ny_purity_scope_checkpoint_t cp = {0};
  uint64_t zero_bloom[4] = {0};
  uint64_t *bloom = ctx->local_bloom ? ctx->local_bloom : zero_bloom;
  size_t container_checkpoint = ctx->local_container_len;
  const char *saved_module = ctx->cg ? ctx->cg->current_module_name : NULL;
  if (ctx->cg)
    ctx->cg->current_module_name = ny_effect_owner_module(ctx);
  ny_purity_scope_save(ctx->local_names, ctx->local_hashes, bloom, &cp);
  for (size_t i = 0; i < function->as.fn.params.len; ++i)
    assigned_name_add(ctx->local_names, ctx->local_hashes, bloom,
                      function->as.fn.params.data[i].name);
  bool body_known = false;
  uint32_t body_effects = ny_stmt_effects(
      ctx->cg, function->as.fn.body, ctx->local_names, ctx->local_hashes, bloom,
      &function->as.fn.params, &function->as.fn, declared_row,
      declared_row != NULL, &body_known);
  ny_purity_scope_restore(ctx->local_names, ctx->local_hashes, bloom, &cp);
  ctx->local_container_len = container_checkpoint;
  if (ctx->cg)
    ctx->cg->current_module_name = saved_module;
  if (effects)
    *effects = body_effects;
  return body_known;
}

static bool ny_effect_lambda_summary(ny_effects_ctx *ctx, const expr_t *lambda,
                                     uint32_t *effects) {
  if (!ctx || !lambda ||
      (lambda->kind != NY_E_LAMBDA && lambda->kind != NY_E_FN) ||
      !lambda->as.lambda.body)
    return false;
  ny_purity_scope_checkpoint_t cp = {0};
  uint64_t zero_bloom[4] = {0};
  uint64_t *bloom = ctx->local_bloom ? ctx->local_bloom : zero_bloom;
  size_t container_checkpoint = ctx->local_container_len;
  ny_purity_scope_save(ctx->local_names, ctx->local_hashes, bloom, &cp);
  for (size_t i = 0; i < lambda->as.lambda.params.len; ++i) {
    const param_t *param = &lambda->as.lambda.params.data[i];
    assigned_name_add(ctx->local_names, ctx->local_hashes, bloom, param->name);
  }
  ny_effects_ctx nested = *ctx;
  nested.owner_effect_row = ny_effect_owner_contract_row(ctx);
  nested.owner_effect_contract_known = ny_effect_owner_contract_known(ctx);
  nested.owner_effect_contract_override = true;
  nested.result = NY_FX_NONE;
  nested.known = true;
  nested.has_known_io = false;
  nested.local_containers = NULL;
  nested.local_container_len = 0;
  nested.local_container_cap = 0;
  if (ctx->local_container_len) {
    nested.local_containers =
        malloc(ctx->local_container_len * sizeof(*nested.local_containers));
    if (!nested.local_containers) {
      ny_purity_scope_restore(ctx->local_names, ctx->local_hashes, bloom, &cp);
      ctx->local_container_len = container_checkpoint;
      return false;
    }
    memcpy(nested.local_containers, ctx->local_containers,
           ctx->local_container_len * sizeof(*nested.local_containers));
    nested.local_container_len = ctx->local_container_len;
    nested.local_container_cap = ctx->local_container_len;
  }
  uint32_t body_effects =
      ny_stmt_effects_in_context(&nested, lambda->as.lambda.body,
                                 &lambda->as.lambda.params, ctx->owner_fn);
  bool body_known = nested.known;
  free(nested.local_containers);
  ny_purity_scope_restore(ctx->local_names, ctx->local_hashes, bloom, &cp);
  ctx->local_container_len = container_checkpoint;
  if (effects)
    *effects = body_effects;
  return body_known;
}

static void
ny_effect_invalidate_container_callback_summaries(ny_effects_ctx *ctx) {
  static const char *const prefixes[] = {"\001dict-callback-summary:",
                                         "\001list-callback-summary:"};
  if (!ctx)
    return;
  for (size_t i = 0; i < ctx->local_container_len; ++i) {
    ny_effect_local_fact_t *fact = &ctx->local_containers[i];
    if (!fact->name)
      continue;
    for (size_t p = 0; p < sizeof(prefixes) / sizeof(prefixes[0]); ++p) {
      if (strncmp(fact->name, prefixes[p], strlen(prefixes[p])) != 0)
        continue;
      fact->call_effects = NY_FX_ALL;
      fact->call_effects_known = false;
      fact->call_effect_row = NULL;
      fact->call_effect_row_known = false;
      fact->call_effect_custom_row_unknown = true;
      break;
    }
  }
}

static void ny_effect_refresh_local_lambda_summaries(ny_effects_ctx *ctx,
                                                     const char *changed_name) {
  if (!ctx || !ctx->local_container_len || !changed_name)
    return;
  const char *pending_names[64] = {changed_name};
  size_t pending_len = 1;
  for (size_t pending = 0; pending < pending_len; ++pending) {
    const char *dependency_name = pending_names[pending];
    size_t checkpoint = ctx->local_container_len;
    for (size_t i = 0; i < checkpoint; ++i) {
      ny_effect_local_fact_t refreshed = ctx->local_containers[i];
      if (!refreshed.name || !refreshed.is_callable ||
          !refreshed.callable_expr ||
          refreshed.callable_expr == ctx->callback_refresh_expr ||
          (refreshed.callable_expr->kind != NY_E_LAMBDA &&
           refreshed.callable_expr->kind != NY_E_FN))
        continue;
      bool latest = true;
      for (size_t j = i + 1; j < checkpoint; ++j)
        if (ctx->local_containers[j].name &&
            strcmp(ctx->local_containers[j].name, refreshed.name) == 0) {
          latest = false;
          break;
        }
      if (!latest)
        continue;
      bool shadows_dependency = false;
      for (size_t p = 0; p < refreshed.callable_expr->as.lambda.params.len; ++p)
        if (refreshed.callable_expr->as.lambda.params.data[p].name &&
            strcmp(refreshed.callable_expr->as.lambda.params.data[p].name,
                   dependency_name) == 0) {
          shadows_dependency = true;
          break;
        }
      if (shadows_dependency || !refreshed.callable_expr->as.lambda.body)
        continue;
      assigned_name_list dependency_names = {0};
      assigned_hash_list dependency_hashes = {0};
      uint64_t dependency_bloom[4] = {0};
      assigned_name_add(&dependency_names, &dependency_hashes, dependency_bloom,
                        dependency_name);
      bool depends_on_name = ny_stmt_refs_params(
          refreshed.callable_expr->as.lambda.body, &dependency_names,
          &dependency_hashes, dependency_bloom);
      free(dependency_names.data);
      free(dependency_hashes.data);
      if (!depends_on_name)
        continue;
      if (strcmp(refreshed.name, dependency_name) == 0) {
        ctx->callback_alias_state_ambiguous = true;
        continue;
      }
      uint32_t updated_effects = NY_FX_ALL;
      const expr_t *saved_refresh_expr = ctx->callback_refresh_expr;
      ctx->callback_refresh_expr = refreshed.callable_expr;
      bool effects_known = ny_effect_lambda_summary(
          ctx, refreshed.callable_expr, &updated_effects);
      ctx->callback_refresh_expr = saved_refresh_expr;
      bool dependency_row_known = false;
      const ny_type_t *dependency_row =
          ny_effect_local_call_row(ctx, dependency_name, &dependency_row_known);
      bool dependency_custom_unknown =
          ny_effect_local_custom_row_unknown(ctx, dependency_name);
      refreshed.call_effects = effects_known ? updated_effects : NY_FX_ALL;
      refreshed.call_effects_known = effects_known;
      if (dependency_row_known && !dependency_custom_unknown &&
          !ny_effect_row_has_nonbuiltin_content(dependency_row)) {
        refreshed.call_effect_row = NULL;
        refreshed.call_effect_row_known = true;
        refreshed.call_effect_custom_row_unknown = false;
      } else {
        refreshed.call_effect_row = NULL;
        refreshed.call_effect_row_known = false;
        refreshed.call_effect_custom_row_unknown = true;
      }
      if (ctx->local_container_len == ctx->local_container_cap) {
        size_t cap =
            ctx->local_container_cap ? ctx->local_container_cap * 2 : 8;
        void *grown = ny_realloc_array(ctx->local_containers, cap,
                                       sizeof(*ctx->local_containers));
        if (!grown) {
          ctx->callback_alias_state_ambiguous = true;
          return;
        }
        ctx->local_containers = grown;
        ctx->local_container_cap = cap;
      }
      ctx->local_containers[ctx->local_container_len++] = refreshed;
      if (strncmp(refreshed.name, "\001dict-callback:", 15) == 0 ||
          strncmp(refreshed.name, "\001list-callback:", 15) == 0)
        ny_effect_invalidate_container_callback_summaries(ctx);
      bool already_pending = false;
      for (size_t p = 0; p < pending_len; ++p)
        if (strcmp(pending_names[p], refreshed.name) == 0) {
          already_pending = true;
          break;
        }
      if (!already_pending) {
        if (pending_len == sizeof(pending_names) / sizeof(pending_names[0])) {
          ctx->callback_alias_state_ambiguous = true;
          return;
        }
        pending_names[pending_len++] = refreshed.name;
      }
    }
  }
}

/*
 * Resolve latent callback rows for explicitly
 * contracted callable factories. The contract
 * identifies constructors that defer
 * invocation; their fnptr actuals contribute to
 * the returned callable, not to the factory
 * call.
 */
static bool ny_effect_call_result_callable_summary(ny_effects_ctx *ctx,
                                                   expr_t *call,
                                                   uint32_t *effects,
                                                   const ny_type_t **row_out) {
  if (row_out)
    *row_out = NULL;
  if (!ctx || !call || (call->kind != NY_E_CALL && call->kind != NY_E_MEMCALL))
    return false;
  if (call->kind == NY_E_CALL && call->as.call.callee &&
      (call->as.call.callee->kind == NY_E_MEMCALL ||
       call->as.call.callee->kind == NY_E_INDEX))
    return ny_effect_callback_expr_summary(ctx, call->as.call.callee, effects);
  uint64_t zero_bloom[4] = {0};
  uint64_t *bloom = ctx->local_bloom ? ctx->local_bloom : zero_bloom;
  fun_sig *sig = call->kind == NY_E_CALL
                     ? ny_purity_resolve_call_sig(ctx->cg, &call->as.call,
                                                  ctx->local_names,
                                                  ctx->local_hashes, bloom)
                     : ny_purity_resolve_memcall_sig(ctx->cg, &call->as.memcall,
                                                     ctx->local_names,
                                                     ctx->local_hashes, bloom);
  ny_call_arg_list *args =
      call->kind == NY_E_CALL ? &call->as.call.args : &call->as.memcall.args;
  if (ny_effect_diag_verbose_enabled())
    ny_effect_diag_log("callable.factory.resolve",
                       "sig=%s std_origin=%d contract_kind=%d "
                       "arity=%d stmt=%d",
                       sig && sig->name ? sig->name : "<unresolved>",
                       ny_sig_is_std_for_effects(sig),
                       sig ? ny_builtin_contract_kind(sig->name) : -1,
                       sig ? sig->arity : -1,
                       sig && sig->stmt_t ? (int)sig->stmt_t->kind : -1);
  if (sig && ny_sig_is_std_for_effects(sig) &&
      ny_builtin_contract_kind(sig->name) ==
          NY_BUILTIN_EFFECT_CALLABLE_FACTORY) {
    uint32_t combined = 0;
    bool known = true;
    ny_type_t *combined_custom =
        row_out ? ny_type_row(ctx->fact_arena, NULL, NULL, NULL) : NULL;
    if (row_out && !combined_custom)
      return false;
    size_t arity = (size_t)(sig->arity < 0 ? 0 : sig->arity);
    for (size_t i = 0; i < arity; ++i) {
      const char *param_type = ny_effect_sig_param_type(sig, i);
      if (!ny_effect_param_is_callable(param_type))
        continue;
      expr_t *actual = ny_effect_hof_actual(sig, args, i, 0);
      if (!actual)
        continue;
      uint32_t callback_effects = NY_FX_ALL;
      bool callback_known =
          ny_effect_callback_expr_summary(ctx, actual, &callback_effects);
      if (ny_effect_diag_verbose_enabled())
        ny_effect_diag_log("callable.factory.input",
                           "sig=%s index=%zu expr_kind=%d "
                           "known=%d effects=0x%x",
                           sig->name, i, (int)actual->kind, callback_known,
                           callback_effects);
      if (callback_known)
        combined |= callback_effects;
      else {
        known = false;
        combined |= NY_FX_ALL;
      }
      if (row_out) {
        bool callback_row_known = false;
        const ny_type_t *callback_row =
            ny_effect_callback_row(ctx, actual, &callback_row_known);
        bool callback_custom_known = false;
        ny_type_t *callback_custom = ny_effect_custom_row_only(
            ctx->fact_arena, callback_row, &callback_custom_known);
        if (!callback_row_known || !callback_custom_known || !callback_custom) {
          known = false;
        } else {
          combined_custom = ny_type_row_compose(
              ctx->fact_arena, NULL, combined_custom, callback_custom);
          if (!combined_custom)
            known = false;
        }
      }
    }
    if (effects)
      *effects = combined;
    if (row_out && known) {
      *row_out = ny_type_effect_row_with_builtin_mask(ctx->fact_arena, combined,
                                                      combined_custom);
      known = *row_out != NULL;
    }
    return known;
  }
  stmt_t *function = call->semantic.canonical_callee_stmt;
  if ((!function || function->kind != NY_S_FUNC) && sig && sig->stmt_t &&
      sig->stmt_t->kind == NY_S_FUNC)
    function = sig->stmt_t;
  if (!sig || !function || function->kind != NY_S_FUNC)
    return false;
  if (!ny_effect_param_is_callable(sig->return_type))
    return false;
  stmt_t *body = function->as.fn.body;
  stmt_t *return_stmt = body;
  size_t prefix_len = 0;
  expr_t *return_values[NY_EFFECT_BOUNDED_CALLBACK_RETURN_LIMIT] = {0};
  size_t return_count = 0;
  if (body && body->kind == NY_S_BLOCK) {
    prefix_len = body->as.block.body.len;
    if (prefix_len == 0)
      return false;
    return_stmt = body->as.block.body.data[prefix_len - 1];
    --prefix_len;
    size_t bounded_prefix_len = 0;
    bool bounded_early_return = ny_effect_collect_bounded_callback_returns(
        function, return_values, NY_EFFECT_BOUNDED_CALLBACK_RETURN_LIMIT,
        &return_count, &bounded_prefix_len);
    if (bounded_early_return)
      prefix_len = bounded_prefix_len;
    if (!bounded_early_return)
      for (size_t i = 0; i < prefix_len; ++i)
        if (ny_effect_stmt_contains_return(body->as.block.body.data[i], 0))
          return false;
  }
  if (!return_count) {
    if (!return_stmt || return_stmt->kind != NY_S_RETURN ||
        !return_stmt->as.ret.value)
      return false;
    return_values[0] = return_stmt->as.ret.value;
    return_count = 1;
  }
  assigned_name_list isolated_names = {0};
  assigned_hash_list isolated_hashes = {0};
  uint64_t isolated_bloom[4] = {0};
  for (size_t i = 0; i < function->as.fn.params.len; ++i) {
    const param_t *param = &function->as.fn.params.data[i];
    if (param->name)
      assigned_name_add(&isolated_names, &isolated_hashes, isolated_bloom,
                        param->name);
  }
  ny_effects_ctx isolated = {.cg = ctx->cg,
                             .fact_arena = ctx->fact_arena,
                             .local_names = &isolated_names,
                             .local_hashes = &isolated_hashes,
                             .local_bloom = isolated_bloom,
                             .owner_fn = &function->as.fn,
                             .owner_effect_row = ny_effect_sig_row(sig),
                             .owner_effect_contract_known =
                                 function->as.fn.effect_contract_known,
                             .owner_effect_contract_override = true};
  for (size_t i = 0; i < function->as.fn.params.len; ++i) {
    const param_t *param = &function->as.fn.params.data[i];
    if (!param->name || !ny_effect_param_is_callable(param->type))
      continue;
    expr_t *actual = ny_effect_hof_actual(sig, args, i, 0);
    uint32_t callback_effects = NY_FX_ALL;
    bool callback_known = actual && ny_effect_callback_expr_summary(
                                        ctx, actual, &callback_effects);
    ny_effect_record_local_container(
        &isolated, param->name, NULL, NULL, param->type,
        callback_known ? callback_effects : NY_FX_ALL, callback_known, true,
        false, 0, false, false, 0, false);
    if (actual) {
      bool callback_row_known = false;
      const ny_type_t *callback_row =
          ny_effect_callback_row(ctx, actual, &callback_row_known);
      ny_effect_record_local_call_row(&isolated, param->name, callback_row,
                                      callback_row_known);
    }
  }
  /*
   * Use the ordinary effect visitor for the
   * factory prefix so local callback aliases
   * inherit its branch/loop joins before the
   * returned closure is summarized.
   * Hand-walking only declaration statements
   * lost those facts.
   */
  ny_visitor_t prefix_visitor = {.ctx = &isolated,
                                 .visit_expr_pre = effects_visit_expr,
                                 .visit_expr_post = effects_visit_expr_post,
                                 .visit_stmt_pre = effects_visit_stmt,
                                 .visit_stmt_post = effects_visit_stmt_post};
  for (size_t si = 0; si < prefix_len; ++si)
    ny_visit_stmt(&prefix_visitor, body->as.block.body.data[si]);
  uint32_t joined_effects = NY_FX_NONE;
  bool known = true;
  const ny_type_t *joined_row = NULL;
  bool joined_row_known = true;
  for (size_t i = 0; i < return_count; ++i) {
    bool duplicate = false;
    for (size_t j = 0; j < i; ++j)
      if (ny_effect_callback_return_same(return_values[i], return_values[j])) {
        duplicate = true;
        break;
      }
    if (duplicate)
      continue;
    uint32_t branch_effects = NY_FX_ALL;
    if (!ny_effect_callback_expr_summary(&isolated, return_values[i],
                                         &branch_effects)) {
      known = false;
      joined_effects |= NY_FX_ALL;
    } else {
      joined_effects |= branch_effects;
    }
    if (row_out) {
      bool branch_row_known = false;
      const ny_type_t *branch_row = ny_effect_callback_row(
          &isolated, return_values[i], &branch_row_known);
      if (!branch_row_known) {
        joined_row_known = false;
        continue;
      }
      bool branch_custom_known = false;
      ny_type_t *branch_custom = ny_effect_custom_row_only(
          ctx->fact_arena, branch_row, &branch_custom_known);
      if (!branch_custom_known || !branch_custom) {
        joined_row_known = false;
        continue;
      }
      if (!joined_row)
        joined_row = branch_custom;
      else {
        ny_type_t *composed = ny_type_row_compose(
            ctx->fact_arena, NULL, (ny_type_t *)joined_row, branch_custom);
        if (composed)
          joined_row = composed;
        else
          joined_row_known = false;
      }
    }
  }
  if (effects)
    *effects = joined_effects;
  if (row_out && known) {
    bool returned_row_known = joined_row_known;
    bool custom_row_known = false;
    ny_type_t *custom_row =
        returned_row_known && joined_row
            ? ny_effect_custom_row_only(ctx->fact_arena, joined_row,
                                        &custom_row_known)
            : NULL;
    if (!custom_row_known || !custom_row) {
      known = false;
    } else {
      uint32_t mask = effects ? *effects : NY_FX_NONE;
      *row_out = ny_type_effect_row_with_builtin_mask(ctx->fact_arena, mask,
                                                      custom_row);
      known = *row_out != NULL;
    }
  }
  if (ny_effect_diag_verbose_enabled())
    ny_effect_diag_log("callable.factory.result",
                       "sig=%s known=%d mask=0x%x custom_known=%d row=%s",
                       sig->name ? sig->name : "<unnamed>", known,
                       effects ? *effects : NY_FX_ALL, joined_row_known,
                       joined_row ? "<joined>" : "<none>");
  free(isolated.local_containers);
  free(isolated_names.data);
  free(isolated_hashes.data);
  return known;
}

static bool ny_effect_expr_call_parts(ny_effects_ctx *ctx, expr_t *expr,
                                      fun_sig **sig_out,
                                      ny_call_arg_list **args_out) {
  if (!ctx || !expr || !sig_out || !args_out)
    return false;
  uint64_t zero_bloom[4] = {0};
  uint64_t *bloom = ctx->local_bloom ? ctx->local_bloom : zero_bloom;
  if (expr->kind == NY_E_CALL) {
    *sig_out = ny_purity_resolve_call_sig(
        ctx->cg, &expr->as.call, ctx->local_names, ctx->local_hashes, bloom);
    *args_out = &expr->as.call.args;
    return *sig_out != NULL;
  }
  if (expr->kind == NY_E_MEMCALL) {
    *sig_out = ny_purity_resolve_memcall_sig(
        ctx->cg, &expr->as.memcall, ctx->local_names, ctx->local_hashes, bloom);
    *args_out = &expr->as.memcall.args;
    return *sig_out != NULL;
  }
  return false;
}

static bool ny_effect_local_query_summary(const ny_effects_ctx *ctx,
                                          const char *name, uint32_t *effects,
                                          bool *known) {
  if (!ctx || !name)
    return false;
  for (size_t i = ctx->local_container_len; i > 0; --i) {
    const ny_effect_local_fact_t *fact = &ctx->local_containers[i - 1];
    if (!fact->name || strcmp(fact->name, name) != 0)
      continue;
    if (!fact->is_query)
      return false;
    if (effects)
      *effects = fact->query_effects;
    if (known)
      *known = fact->query_effects_known;
    return true;
  }
  return false;
}

static bool ny_effect_local_query_step_summary(const ny_effects_ctx *ctx,
                                               const char *name,
                                               uint32_t *effects, bool *known) {
  if (!ctx || !name)
    return false;
  for (size_t i = ctx->local_container_len; i > 0; --i) {
    const ny_effect_local_fact_t *fact = &ctx->local_containers[i - 1];
    if (!fact->name || strcmp(fact->name, name) != 0)
      continue;
    if (!fact->is_query_step)
      return false;
    if (effects)
      *effects = fact->query_step_effects;
    if (known)
      *known = fact->query_step_effects_known;
    return true;
  }
  return false;
}

static bool ny_effect_query_step_summary(ny_effects_ctx *ctx, expr_t *expr,
                                         uint32_t *effects, bool *known) {
  if (effects)
    *effects = 0;
  if (known)
    *known = false;
  if (!ctx || !expr)
    return false;
  if (expr->kind == NY_E_IDENT && expr->as.ident.name)
    return ny_effect_local_query_step_summary(ctx, expr->as.ident.name, effects,
                                              known);
  fun_sig *sig = NULL;
  ny_call_arg_list *args = NULL;
  if (!ny_effect_expr_call_parts(ctx, expr, &sig, &args) ||
      !ny_sig_is_std_for_effects(sig))
    return false;
  int kind = ny_builtin_contract_kind(sig->name);
  if (kind == NY_BUILTIN_EFFECT_QUERY_STEP) {
    if (known)
      *known = true;
    return true;
  }
  if (kind != NY_BUILTIN_EFFECT_QUERY_PREDICATE_STEP)
    return false;
  for (size_t i = 0; i < (size_t)(sig->arity < 0 ? 0 : sig->arity); ++i) {
    const char *type = ny_effect_sig_param_type(sig, i);
    if (!ny_effect_param_is_callable(type))
      continue;
    expr_t *actual = ny_effect_hof_actual(sig, args, i, 0);
    if (!actual)
      return false;
    uint32_t callback_effects = NY_FX_ALL;
    bool callback_known =
        ny_effect_callback_expr_summary(ctx, actual, &callback_effects);
    if (effects)
      *effects = callback_effects;
    if (known)
      *known = callback_known;
    return true;
  }
  return false;
}

static bool ny_effect_query_value_summary(ny_effects_ctx *ctx, expr_t *expr,
                                          uint32_t *effects, bool *known) {
  if (effects)
    *effects = 0;
  if (known)
    *known = false;
  if (!ctx || !expr)
    return false;
  if (expr->kind == NY_E_IDENT && expr->as.ident.name)
    return ny_effect_local_query_summary(ctx, expr->as.ident.name, effects,
                                         known);
  fun_sig *sig = NULL;
  ny_call_arg_list *args = NULL;
  if (!ny_effect_expr_call_parts(ctx, expr, &sig, &args) ||
      !ny_sig_is_std_for_effects(sig))
    return false;
  int kind = ny_builtin_contract_kind(sig->name);
  if (kind == NY_BUILTIN_EFFECT_QUERY_FROM) {
    if (known)
      *known = true;
    return true;
  }
  if (kind != NY_BUILTIN_EFFECT_QUERY_PIPE)
    return false;

  expr_t *query_arg = ny_effect_hof_actual(sig, args, 0, 0);
  expr_t *step_arg = ny_effect_hof_actual(sig, args, 1, 0);
  uint32_t query_effects = NY_FX_ALL;
  uint32_t step_effects = NY_FX_ALL;
  bool query_known = false;
  bool step_known = false;
  bool is_query = ny_effect_query_value_summary(ctx, query_arg, &query_effects,
                                                &query_known);
  bool is_step =
      ny_effect_query_step_summary(ctx, step_arg, &step_effects, &step_known);
  if (!is_query || !is_step)
    return false;
  if (effects)
    *effects = query_effects | step_effects;
  if (known)
    *known = query_known && step_known;
  return true;
}

static bool ny_effect_query_run_summary(ny_effects_ctx *ctx, const fun_sig *sig,
                                        const ny_call_arg_list *args,
                                        size_t receiver_skip, uint32_t *effects,
                                        bool *known) {
  if (effects)
    *effects = 0;
  if (known)
    *known = false;
  if (!ctx || !sig || !ny_sig_is_std_for_effects(sig) ||
      ny_builtin_contract_kind(sig->name) != NY_BUILTIN_EFFECT_QUERY_RUN)
    return false;
  expr_t *query_arg = ny_effect_hof_actual(sig, args, 0, receiver_skip);
  if (!query_arg && receiver_skip)
    query_arg = ny_effect_hof_actual(sig, args, 0, 0);
  uint32_t query_effects = NY_FX_ALL;
  bool query_known = false;
  bool is_query = ny_effect_query_value_summary(ctx, query_arg, &query_effects,
                                                &query_known);
  if (effects)
    *effects = query_effects;
  if (known)
    *known = is_query && query_known;
  return true;
}

static enum_member_def_t *ny_effect_enum_constructor(ny_effects_ctx *ctx,
                                                     expr_t *expr) {
  if (!ctx || !ctx->cg || !expr)
    return NULL;
  expr_t *callee = expr->kind == NY_E_CALL      ? expr->as.call.callee
                   : expr->kind == NY_E_MEMCALL ? expr->as.memcall.target
                                                : NULL;
  expr_t *root = callee;
  while (root && root->kind == NY_E_MEMBER)
    root = root->as.member.target;
  if (!root || root->kind != NY_E_IDENT || !root->as.ident.name ||
      assigned_name_contains(ctx->local_names, ctx->local_hashes,
                             ctx->local_bloom ? ctx->local_bloom
                                              : (uint64_t[4]){0},
                             root->as.ident.name))
    return NULL;
  char name[1024];
  size_t name_len = 0;
  if (!ny_effect_expr_qualified_name(callee, name, sizeof(name), &name_len))
    return NULL;
  if (expr->kind == NY_E_MEMCALL) {
    size_t used = strlen(name);
    int n = snprintf(name + used, sizeof(name) - used, ".%s",
                     expr->as.memcall.name ? expr->as.memcall.name : "");
    if (n < 0 || (size_t)n >= sizeof(name) - used)
      return NULL;
  }
  if (ctx->cg->current_module_name) {
    char owned[1280];
    int n = snprintf(owned, sizeof(owned), "%s.%s",
                     ctx->cg->current_module_name, name);
    if (n > 0 && (size_t)n < sizeof(owned)) {
      enum_member_def_t *member =
          lookup_enum_member_owner(ctx->cg, owned, NULL);
      if (member)
        return member;
    }
  }
  return lookup_enum_member_owner(ctx->cg, name, NULL);
}

static bool effects_visit_expr(ny_visitor_t *v, expr_t *e) {
  ny_effects_ctx *ctx = (ny_effects_ctx *)v->ctx;
  if (!e)
    return false;

  enum_member_def_t *constructor = ny_effect_enum_constructor(ctx, e);
  if (constructor) {
    ny_call_arg_list *args =
        e->kind == NY_E_CALL ? &e->as.call.args : &e->as.memcall.args;
    for (size_t i = 0; i < args->len; ++i)
      ny_visit_expr(v, args->data[i].val);
    if (constructor->has_payload)
      ctx->result |= NY_FX_ALLOC;
    return false;
  }

  switch (e->kind) {
  case NY_E_BINARY:
    if (ny_effect_is_assignment_operator(e->as.binary.op) &&
        e->as.binary.left && e->as.binary.left->kind == NY_E_INDEX &&
        e->as.binary.left->as.index.target &&
        e->as.binary.left->as.index.target->kind == NY_E_IDENT &&
        e->as.binary.left->as.index.target->as.ident.name) {
      const char *name = e->as.binary.left->as.index.target->as.ident.name;
      if (ny_effect_local_is_dict(ctx, name) ||
          ny_effect_local_is_list(ctx, name)) {
        ny_effect_local_fact_t prior_summary = {0};
        const bool list_store = ny_effect_local_is_list(ctx, name);
        const bool dict_store = ny_effect_local_is_dict(ctx, name);
        if (e->as.binary.op && strcmp(e->as.binary.op, "=") == 0) {
          const char *summary_name =
              list_store ? ny_effect_list_callback_summary_fact_name(ctx, name)
              : dict_store
                  ? ny_effect_dict_callback_summary_fact_name(ctx, name)
                  : NULL;
          const ny_effect_local_fact_t *summary =
              summary_name ? ny_effect_fact_latest(ctx->local_containers,
                                                   ctx->local_container_len,
                                                   summary_name)
                           : NULL;
          if (summary && summary->is_callable && summary->call_effects_known &&
              summary->call_effect_row_known &&
              !summary->call_effect_custom_row_unknown)
            prior_summary = *summary;
        }
        if (list_store)
          ny_effect_invalidate_named_list_callback_facts(ctx, name);
        else
          ny_effect_invalidate_container_alias_group_callback_facts(ctx, name);
        if (e->as.binary.op && strcmp(e->as.binary.op, "=") == 0) {
          if (prior_summary.is_callable &&
              ny_effect_callback_store_value_is_stable(
                  e->as.binary.left->as.index.start) &&
              ny_effect_callback_store_value_is_stable(e->as.binary.right))
            ny_effect_record_mutated_container_callback_summary(
                ctx, name, &prior_summary, e->as.binary.right);
          ny_effect_record_indexed_callback_store(ctx, e->as.binary.left,
                                                  e->as.binary.right);
        }
      }
    }
    return true;
  case NY_E_UNARY:
    if (e->as.unary.op && (strcmp(e->as.unary.op, "async") == 0 ||
                           strcmp(e->as.unary.op, "await") == 0))
      ctx->result |= NY_FX_THREAD;
    return true;
  case NY_E_CALL: {
    if (ctx->callback_try_body_depth) {
      for (size_t i = ctx->local_container_len; i > 0; --i) {
        if (ctx->local_containers[i - 1].is_callable) {
          /*
           * A call can throw after changing a
           * captured callback alias. The
           * handler may see an intermediate row
           * absent from the final body
           * snapshot, so the state must be
           * treated as ambiguous.
           */
          ctx->callback_alias_state_ambiguous = true;
          break;
        }
      }
    }
    ny_visit_expr(v, e->as.call.callee);
    for (size_t i = 0; i < e->as.call.args.len; i++)
      ny_visit_expr(v, e->as.call.args.data[i].val);
    fun_sig *sig = ny_purity_resolve_call_sig(
        ctx->cg, &e->as.call, ctx->local_names, ctx->local_hashes,
        ctx->local_bloom ? ctx->local_bloom : (uint64_t[4]){0});
    bool is_builtin_set_idx =
        (e->semantic.canonical_callee &&
         strcmp(e->semantic.canonical_callee, "core.set_idx") == 0) ||
        (sig && ny_sig_is_std_for_effects(sig) && sig->name &&
         strcmp(sig->name, "core.set_idx") == 0);
    ny_effect_local_fact_t prior_set_idx_summary = {0};
    const char *set_idx_container = NULL;
    if (is_builtin_set_idx && e->as.call.args.len == 3 &&
        e->as.call.args.data[0].val &&
        e->as.call.args.data[0].val->kind == NY_E_IDENT &&
        e->as.call.args.data[0].val->as.ident.name) {
      set_idx_container = e->as.call.args.data[0].val->as.ident.name;
      const bool is_list = ny_effect_local_is_list(ctx, set_idx_container);
      const bool is_dict = ny_effect_local_is_dict(ctx, set_idx_container);
      const char *summary_name =
          is_list ? ny_effect_list_callback_summary_fact_name(ctx,
                                                              set_idx_container)
          : is_dict ? ny_effect_dict_callback_summary_fact_name(
                          ctx, set_idx_container)
                    : NULL;
      const ny_effect_local_fact_t *summary =
          summary_name
              ? ny_effect_fact_latest(ctx->local_containers,
                                      ctx->local_container_len, summary_name)
              : NULL;
      if (summary && summary->is_callable && summary->call_effects_known &&
          summary->call_effect_row_known &&
          !summary->call_effect_custom_row_unknown)
        prior_set_idx_summary = *summary;
    }
    bool transparent_container_forward = false;
    if (sig && sig->stmt_t && sig->stmt_t->kind == NY_S_FUNC &&
        sig->stmt_t->as.fn.body &&
        sig->stmt_t->as.fn.body->kind == NY_S_BLOCK) {
      expr_t *returned_container = ny_effect_direct_returned_container(ctx, e);
      transparent_container_forward =
          returned_container && returned_container->kind == NY_E_IDENT;
    }
    for (size_t i = 0; i < e->as.call.args.len; i++) {
      expr_t *arg = e->as.call.args.data[i].val;
      if (arg && arg->kind == NY_E_IDENT && arg->as.ident.name &&
          (ny_effect_local_is_dict(ctx, arg->as.ident.name) ||
           ny_effect_local_is_list(ctx, arg->as.ident.name))) {
        if (transparent_container_forward)
          continue;
        if (is_builtin_set_idx && i == 0 &&
            ny_effect_local_is_list(ctx, arg->as.ident.name))
          ny_effect_invalidate_named_list_callback_facts(ctx,
                                                         arg->as.ident.name);
        else
          ny_effect_invalidate_container_callback_facts(ctx,
                                                        arg->as.ident.name);
      }
    }
    if (is_builtin_set_idx && e->as.call.args.len == 3 &&
        !e->as.call.args.data[0].name && !e->as.call.args.data[1].name &&
        !e->as.call.args.data[2].name) {
      if (set_idx_container && prior_set_idx_summary.is_callable &&
          ny_effect_callback_store_value_is_stable(
              e->as.call.args.data[1].val) &&
          ny_effect_callback_store_value_is_stable(e->as.call.args.data[2].val))
        ny_effect_record_mutated_container_callback_summary(
            ctx, set_idx_container, &prior_set_idx_summary,
            e->as.call.args.data[2].val);
      ny_effect_record_container_callback_store(
          ctx, e->as.call.args.data[0].val, e->as.call.args.data[1].val,
          e->as.call.args.data[2].val);
    }
    uint32_t effects = ny_sig_effects(sig);
    bool effects_known = ny_sig_effects_known(sig);
    const ny_type_t *callee_row = ny_effect_sig_row(sig);
    if (ny_effect_row_has_nonbuiltin_content(callee_row)) {
      bool covered =
          ny_effect_owner_contract_known(ctx) &&
          ny_effect_row_covers(callee_row, ny_effect_owner_contract_row(ctx));
      if (!covered)
        ctx->known = false;
    }
    /*
     * Standard-library signatures intentionally
     * start conservative. A more specific
     * audited runtime contract is stronger
     * evidence for the known builtin entry and
     * must take precedence over that blanket
     * summary.
     */
    if (sig && ny_sig_is_std_for_effects(sig) && e->as.call.callee &&
        e->as.call.callee->kind == NY_E_IDENT) {
      uint32_t builtin_effects = NY_FX_ALL;
      bool has_builtin_contract = ny_builtin_contract(
          e->as.call.callee->as.ident.name, &builtin_effects);
      if (!has_builtin_contract)
        has_builtin_contract = ny_sig_builtin_contract(sig, &builtin_effects);
      if (has_builtin_contract) {
        effects = builtin_effects;
        effects_known = true;
      }
    }
    bool callee_is_local =
        e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
        e->as.call.callee->as.ident.name &&
        assigned_name_contains(ctx->local_names, ctx->local_hashes,
                               ctx->local_bloom ? ctx->local_bloom
                                                : (uint64_t[4]){0},
                               e->as.call.callee->as.ident.name);
    bool callee_is_resolved_closure =
        e->as.call.callee && e->as.call.callee->semantic.resolved &&
        e->as.call.callee->semantic.rep == NY_SEM_REP_CLOSURE;
    const char *container_callback_fact =
        ny_effect_container_callback_fact(ctx, e->as.call.callee);
    if (!sig && e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT &&
        e->as.call.callee->as.ident.name && !callee_is_local) {
      const char *callee_name = e->as.call.callee->as.ident.name;
      if (!effects_known && (lookup_layout(ctx->cg, callee_name) ||
                             ny_lookup_tagged_type(ctx->cg, callee_name))) {
        /*
         * A resolved record/nominal constructor
         * allocates its value.
         */
        effects = NY_FX_ALLOC;
        effects_known = true;
      }
      if (!effects_known && ny_sig_is_impl_owner(ctx->cg, callee_name)) {
        effects = NY_FX_ALLOC;
        effects_known = true;
      }
      /*
       * Refinement membership evaluates a compile-time predicate and has no
       * runtime effects. Keep it distinct from an unresolved function call so
       * direct refinement guards remain usable inside effect-checked code.
       */
      if (!effects_known && ny_lookup_refine(ctx->cg, callee_name)) {
        effects = NY_FX_NONE;
        effects_known = true;
      }
      if (!effects_known)
        effects_known = ny_builtin_contract(callee_name, &effects);
    }
    /*
     * Keep a missing local callback summary
     * unknown. The all-effects mask is only a
     * conservative value; it does not make the
     * effect fact known.
     */
    if (!sig && callee_is_local) {
      bool row_callback =
          ny_effect_owner_row_callback(ctx, e->as.call.callee->as.ident.name);
      bool local_effects_known =
          row_callback || ny_effect_local_callable(
                              ctx, e->as.call.callee->as.ident.name, &effects);
      if (row_callback)
        effects = NY_FX_NONE;
      else if (!local_effects_known)
        effects = NY_FX_ALL;
      effects_known = local_effects_known;
      if (ny_effect_diag_verbose_enabled())
        ny_effect_diag_log("local-call",
                           "callee=%s summary_known=%d "
                           "resolved_closure=%d mask=0x%x",
                           e->as.call.callee->as.ident.name,
                           local_effects_known, callee_is_resolved_closure,
                           effects);
    }
    if (!sig && callee_is_local) {
      bool row_known = false;
      const ny_type_t *local_row = NULL;
      if (ny_effect_owner_row_callback(ctx, e->as.call.callee->as.ident.name)) {
        local_row = ny_effect_owner_contract_row(ctx);
        row_known = true;
      } else {
        local_row = ny_effect_local_call_row(
            ctx, e->as.call.callee->as.ident.name, &row_known);
      }
      if (ny_effect_local_custom_row_unknown(
              ctx, e->as.call.callee->as.ident.name) ||
          (row_known && ny_effect_row_has_nonbuiltin_content(local_row) &&
           (!ny_effect_owner_contract_known(ctx) ||
            !ny_effect_row_covers(local_row,
                                  ny_effect_owner_contract_row(ctx)))))
        effects_known = false;
    }
    if (!sig && container_callback_fact) {
      effects_known =
          ny_effect_local_callable(ctx, container_callback_fact, &effects);
      if (!effects_known)
        effects = NY_FX_ALL;
      bool row_known = false;
      const ny_type_t *local_row =
          ny_effect_local_call_row(ctx, container_callback_fact, &row_known);
      if (ny_effect_local_custom_row_unknown(ctx, container_callback_fact) ||
          (row_known && ny_effect_row_has_nonbuiltin_content(local_row) &&
           (!ny_effect_owner_contract_known(ctx) ||
            !ny_effect_row_covers(local_row,
                                  ny_effect_owner_contract_row(ctx)))))
        effects_known = false;
    }
    expr_t *container_fallback =
        ny_effect_container_get_callback_fallback(e->as.call.callee);
    if (!sig && container_callback_fact && container_fallback) {
      uint32_t fallback_effects = NY_FX_ALL;
      bool fallback_known = ny_effect_callback_expr_summary(
          ctx, container_fallback, &fallback_effects);
      if (fallback_known)
        effects |= fallback_effects;
      else
        effects_known = false;
      bool fallback_row_known = false;
      const ny_type_t *fallback_row =
          ny_effect_callback_row(ctx, container_fallback, &fallback_row_known);
      if (!fallback_row_known ||
          (ny_effect_row_has_nonbuiltin_content(fallback_row) &&
           (!ny_effect_owner_contract_known(ctx) ||
            !ny_effect_row_covers(fallback_row,
                                  ny_effect_owner_contract_row(ctx)))))
        effects_known = false;
    }
    if (!sig && e->as.call.callee &&
        (e->as.call.callee->kind == NY_E_LAMBDA ||
         e->as.call.callee->kind == NY_E_FN)) {
      effects_known =
          ny_effect_lambda_summary(ctx, e->as.call.callee, &effects);
      if (!effects_known)
        effects = NY_FX_ALL;
    }
    uint32_t callback_effects = 0;
    bool callbacks_known = true;
    if (sig &&
        ny_effect_hof_callback_summary(ctx, sig, &e->as.call.args, 0,
                                       &callback_effects, &callbacks_known)) {
      uint32_t base_effects = effects;
      effects |= callback_effects;
      if (!callbacks_known)
        effects_known = false;
      if (ny_effect_diag_verbose_enabled())
        ny_effect_diag_log("hof",
                           "call=%s base=0x%x callback=0x%x "
                           "callback_known=%d "
                           "result=0x%x result_known=%d",
                           sig->name ? sig->name : "<unnamed>", base_effects,
                           callback_effects, callbacks_known, effects,
                           effects_known);
    }
    if (sig &&
        !ny_effect_hof_custom_rows_covered(ctx, sig, &e->as.call.args, 0))
      effects_known = false;
    uint32_t query_effects = 0;
    bool query_known = true;
    if (ny_effect_query_run_summary(ctx, sig, &e->as.call.args, 0,
                                    &query_effects, &query_known)) {
      effects |= query_effects;
      if (!query_known)
        effects_known = false;
      if (ny_effect_diag_verbose_enabled())
        ny_effect_diag_log("query.run",
                           "sig=%s callbacks=0x%x "
                           "callbacks_known=%d "
                           "result=0x%x result_known=%d",
                           sig && sig->name ? sig->name : "<unresolved>",
                           query_effects, query_known, effects, effects_known);
    }
    /*
     * An unresolved indirect call may perform
     * every effect, but that conservative mask
     * is not a resolved effect contract.
     */
    bool callee_is_lambda =
        e->as.call.callee && (e->as.call.callee->kind == NY_E_LAMBDA ||
                              e->as.call.callee->kind == NY_E_FN);
    ctx->has_known_io |=
        effects_known && effects != NY_FX_ALL && (effects & NY_FX_IO) != 0;
    ctx->has_known_io |= ny_effect_comptime_callee_io(ctx, sig);
    if (!sig && !callee_is_local && !callee_is_lambda && !effects_known) {
      effects = NY_FX_ALL;
    }
    e->semantic.call_effect_mask = effects;
    e->semantic.call_effect_known = effects_known;
    e->semantic.call_effect_resolved = true;
    ny_effect_store_call_custom_row(ctx, e, ny_effect_sig_row(sig),
                                    effects_known, sig, &e->as.call.args, 0);
    ctx->result |= effects;
    if (!effects_known) {
      if (ctx->comptime_io_scan)
        ctx->comptime_io_scan->incomplete = true;
      ctx->known = false;
      const char *callee =
          e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT
              ? e->as.call.callee->as.ident.name
              : "<indirect>";
      if (verbose_enabled >= 2)
        ny_dbg_print(NY_DBG_EFFECT, __FILE__, __LINE__, __func__,
                     "UNKNOWN_CALL callee=%s reason=%s",
                     callee ? callee : "<unnamed>",
                     sig ? "callee-contract" : "unresolved-call");
      if (ny_effect_diag_verbose_enabled())
        ny_effect_diag_log(
            "unknown.detail",
            "call=%s semantic=%d canonical=%s "
            "sig=%s sig_known=%d",
            callee ? callee : "<unnamed>", (int)e->semantic.member_call_kind,
            e->semantic.canonical_callee ? e->semantic.canonical_callee
                                         : "<none>",
            sig && sig->name ? sig->name : "<unresolved>",
            ny_sig_effects_known(sig));
    }
    return false;
  }
  case NY_E_MEMCALL: {
    bool is_list_get =
        e->as.memcall.name && strcmp(e->as.memcall.name, "get") == 0;
    ny_effect_local_fact_t prior_dict_set_summary = {0};
    const char *dict_set_container = NULL;
    if (e->as.memcall.target && e->as.memcall.target->kind == NY_E_IDENT &&
        e->as.memcall.target->as.ident.name &&
        ny_effect_local_is_dict(ctx, e->as.memcall.target->as.ident.name) &&
        e->as.memcall.name && strcmp(e->as.memcall.name, "set") == 0) {
      dict_set_container = e->as.memcall.target->as.ident.name;
      const char *summary_name =
          ny_effect_dict_callback_summary_fact_name(ctx, dict_set_container);
      const ny_effect_local_fact_t *summary =
          summary_name
              ? ny_effect_fact_latest(ctx->local_containers,
                                      ctx->local_container_len, summary_name)
              : NULL;
      if (summary && summary->is_callable && summary->call_effects_known &&
          summary->call_effect_row_known &&
          !summary->call_effect_custom_row_unknown)
        prior_dict_set_summary = *summary;
    }
    if (e->as.memcall.target && e->as.memcall.target->kind == NY_E_IDENT &&
        e->as.memcall.target->as.ident.name &&
        ((ny_effect_local_is_dict(ctx, e->as.memcall.target->as.ident.name) &&
          e->as.memcall.name &&
          (strcmp(e->as.memcall.name, "set") == 0 ||
           strcmp(e->as.memcall.name, "remove") == 0 ||
           strcmp(e->as.memcall.name, "pop") == 0 ||
           strcmp(e->as.memcall.name, "clear") == 0 ||
           strcmp(e->as.memcall.name, "update") == 0)) ||
         (ny_effect_local_is_list(ctx, e->as.memcall.target->as.ident.name) &&
          !is_list_get && e != ctx->callback_nonmutating_list_append)))
      ny_effect_invalidate_container_alias_group_callback_facts(
          ctx, e->as.memcall.target->as.ident.name);
    if (ctx->callback_try_body_depth) {
      for (size_t i = ctx->local_container_len; i > 0; --i) {
        if (ctx->local_containers[i - 1].is_callable) {
          ctx->callback_alias_state_ambiguous = true;
          break;
        }
      }
    }
    ny_visit_expr(v, e->as.memcall.target);
    for (size_t i = 0; i < e->as.memcall.args.len; i++) {
      ny_visit_expr(v, e->as.memcall.args.data[i].val);
    }
    fun_sig *sig = ny_purity_resolve_memcall_sig(
        ctx->cg, &e->as.memcall, ctx->local_names, ctx->local_hashes,
        ctx->local_bloom ? ctx->local_bloom : (uint64_t[4]){0});
    if (!sig && e->semantic.member_call_kind == NY_SEM_CALL_DIRECT_ATTACHED)
      sig = ny_effect_sig_for_stmt(ctx->cg, e->semantic.canonical_callee_stmt);
    /*
     * Dictionary aliases are conservatively invalidated at a mutation.
     * Restore an exact local fact only for the standard dictionary setter
     * with a literal key; dynamic keys keep the conservative invalidation.
     */
    bool is_builtin_dict_set =
        e->as.memcall.target && e->as.memcall.target->kind == NY_E_IDENT &&
        e->as.memcall.target->as.ident.name &&
        ny_effect_local_is_dict(ctx, e->as.memcall.target->as.ident.name) &&
        e->as.memcall.name && strcmp(e->as.memcall.name, "set") == 0 && sig &&
        ny_sig_is_std_for_effects(sig) && sig->name &&
        strcmp(sig->name, "core.set") == 0;
    if (is_builtin_dict_set && e->as.memcall.args.len == 2 &&
        !e->as.memcall.args.data[0].name && !e->as.memcall.args.data[1].name) {
      ny_effect_record_container_callback_store(ctx, e->as.memcall.target,
                                                e->as.memcall.args.data[0].val,
                                                e->as.memcall.args.data[1].val);
      if (dict_set_container && prior_dict_set_summary.is_callable &&
          ny_effect_callback_store_value_is_stable(
              e->as.memcall.args.data[0].val) &&
          ny_effect_callback_store_value_is_stable(
              e->as.memcall.args.data[1].val))
        ny_effect_record_mutated_container_callback_summary(
            ctx, dict_set_container, &prior_dict_set_summary,
            e->as.memcall.args.data[1].val);
    }
    uint32_t contract_effects = NY_FX_ALL;
    bool has_contract = false;
    if (!sig && e->as.memcall.target && e->as.memcall.name) {
      const char *container =
          ny_effect_value_container_type(ctx, e->as.memcall.target);
      if (container &&
          (strcmp(container, "list") == 0 || strcmp(container, "dict") == 0 ||
           strcmp(container, "set") == 0)) {
        char contract_name[128];
        int n = snprintf(contract_name, sizeof(contract_name), "core.%s.%s",
                         container, e->as.memcall.name);
        if (n > 0 && (size_t)n < sizeof(contract_name)) {
          sig = lookup_fun(ctx->cg, contract_name, 0);
          if (!sig)
            has_contract =
                ny_builtin_contract(contract_name, &contract_effects);
        }
      }
    }
    /*
     * The fallback above deliberately protects
     * list/dict/set contracts from method-name
     * guessing. For other receivers, resolve
     * the method against the receiver's known
     * owner type before the cross-owner
     * compatibility scan below. This covers
     * literals (`21.twice()`) and typed locals
     * (`f32_value.twice()`) while preserving
     * ambiguity when no owner fact is
     * available.
     */
    if (!sig && !has_contract && e->as.memcall.target && e->as.memcall.name &&
        !ny_effect_value_container_type(ctx, e->as.memcall.target)) {
      const char *receiver_type =
          ny_effect_expr_value_type(ctx, e->as.memcall.target);
      sig = ny_effect_attached_sig_for_owner(ctx->cg, receiver_type,
                                             e->as.memcall.name);
    }
    /*
     * Attached-method sugar on a non-container
     * receiver (`SelfBox.maybe`): resolve
     * through the registered impl spellings
     * (`Owner.method`) when every definition
     * agrees on an effect contract.  Container
     * receivers keep their container-specific
     * contracts above — a generic suffix match
     * would steal them with a wrong-type
     * contract.
     */
    if (!sig && !has_contract && e->as.memcall.target &&
        e->as.memcall.target->kind == NY_E_IDENT &&
        e->as.memcall.target->as.ident.name &&
        !ny_effect_value_container_type(ctx, e->as.memcall.target)) {
      const char *mname = e->as.memcall.name;
      size_t mlen = mname ? strlen(mname) : 0;
      if (mlen) {
        fun_sig *hit = NULL;
        bool ambiguous = false;
        for (size_t i2 = 0; i2 < ctx->cg->fun_sigs.len; ++i2) {
          fun_sig *cand = &ctx->cg->fun_sigs.data[i2];
          if (!cand->name)
            continue;
          const char *dleaf = strrchr(cand->name, '.');
          if (!dleaf || strncmp(dleaf + 1, mname, mlen) != 0 ||
              dleaf[1 + mlen] != '\0')
            continue;
          if (!hit) {
            hit = cand;
            continue;
          }
          if (ny_sig_effects(hit) != ny_sig_effects(cand) ||
              ny_sig_effects_known(hit) != ny_sig_effects_known(cand)) {
            ambiguous = true;
            break;
          }
        }
        if (!ambiguous && hit)
          sig = hit;
      }
    }
    const ny_type_t *callee_row = ny_effect_sig_row(sig);
    if (ny_effect_row_has_nonbuiltin_content(callee_row)) {
      bool covered =
          ny_effect_owner_contract_known(ctx) &&
          ny_effect_row_covers(callee_row, ny_effect_owner_contract_row(ctx));
      if (!covered)
        ctx->known = false;
    }
    uint32_t effects = sig ? ny_sig_effects(sig) : contract_effects;
    bool effects_known = sig ? ny_sig_effects_known(sig) : has_contract;
    /*
     * A standard container method can resolve
     * to a generic stdlib signature whose body
     * summary is unknown because it delegates
     * to a raw helper. Prefer an audited
     * receiver contract in that case;
     * unresolved standard methods without such
     * a contract remain explicitly unknown.
     */
    if (sig && ny_sig_is_std_for_effects(sig)) {
      uint32_t audited_effects = NY_FX_ALL;
      bool audited = ny_builtin_contract(sig->name, &audited_effects);
      if (!audited && e->as.memcall.target && e->as.memcall.name) {
        const char *container =
            ny_effect_value_container_type(ctx, e->as.memcall.target);
        if (container &&
            (strcmp(container, "list") == 0 || strcmp(container, "dict") == 0 ||
             strcmp(container, "set") == 0)) {
          char contract_name[128];
          int n = snprintf(contract_name, sizeof(contract_name), "core.%s.%s",
                           container, e->as.memcall.name);
          if (n > 0 && (size_t)n < sizeof(contract_name))
            audited = ny_builtin_contract(contract_name, &audited_effects);
        }
      }
      if (audited) {
        effects = audited_effects;
        effects_known = true;
      }
    }
    uint32_t callback_effects = 0;
    bool callbacks_known = true;
    /*
     * NY_E_MEMCALL stores only explicit
     * arguments; the receiver occupies formal
     * parameter zero whether resolution found a
     * true impl method or the desugared
     * `core.iter.map` function.
     */
    size_t receiver_skip = 1u;
    /*
     * `it.mapcat(callback, xs)` is
     * module-qualified syntax; `xs.mapcat(fn)`
     * is receiver syntax.  Both are represented
     * as member calls, but only the latter has
     * a synthetic receiver occupying formal
     * zero.  Skipping one argument for a module
     * alias hid the callback contract and made
     * recursive callback summaries appear
     * unknown.
     */
    if (e->as.memcall.target && e->as.memcall.target->kind == NY_E_IDENT &&
        e->as.memcall.target->as.ident.name) {
      char module_fn[1280];
      char module_path[1024];
      bool module_target =
          ny_resolve_module_expr_path(ctx->cg, NULL, 0, e->as.memcall.target,
                                      module_path, sizeof(module_path));
      if (module_target && ny_resolve_module_function_path(
                               ctx->cg, module_path, e->as.memcall.name,
                               module_fn, sizeof(module_fn)))
        receiver_skip = 0;
    }
    if (sig && ny_effect_hof_callback_summary(ctx, sig, &e->as.memcall.args,
                                              receiver_skip, &callback_effects,
                                              &callbacks_known)) {
      uint32_t base_effects = effects;
      effects |= callback_effects;
      if (!callbacks_known)
        effects_known = false;
      if (ny_effect_diag_verbose_enabled())
        ny_effect_diag_log("hof",
                           "method=%s base=0x%x callback=0x%x "
                           "callback_known=%d result=0x%x "
                           "result_known=%d",
                           sig->name ? sig->name : "<unnamed>", base_effects,
                           callback_effects, callbacks_known, effects,
                           effects_known);
    }
    if (sig && !ny_effect_hof_custom_rows_covered(ctx, sig, &e->as.memcall.args,
                                                  receiver_skip))
      effects_known = false;
    uint32_t query_effects = 0;
    bool query_known = true;
    if (ny_effect_query_run_summary(ctx, sig, &e->as.memcall.args,
                                    receiver_skip, &query_effects,
                                    &query_known)) {
      effects |= query_effects;
      if (!query_known)
        effects_known = false;
      if (ny_effect_diag_verbose_enabled())
        ny_effect_diag_log("query.run",
                           "sig=%s callbacks=0x%x "
                           "callbacks_known=%d "
                           "result=0x%x result_known=%d",
                           sig && sig->name ? sig->name : "<unresolved>",
                           query_effects, query_known, effects, effects_known);
    }
    /*
     * An unresolved method may perform every
     * effect, but its conservative mask is not
     * a resolved effect contract.
     */
    if (!sig && !has_contract) {
      effects = NY_FX_ALL;
    }
    ctx->has_known_io |=
        effects_known && effects != NY_FX_ALL && (effects & NY_FX_IO) != 0;
    ctx->has_known_io |= ny_effect_comptime_callee_io(ctx, sig);
    e->semantic.call_effect_mask = effects;
    e->semantic.call_effect_known = effects_known;
    e->semantic.call_effect_resolved = true;
    ny_effect_store_call_custom_row(ctx, e, ny_effect_sig_row(sig),
                                    effects_known, sig, &e->as.memcall.args,
                                    receiver_skip);
    ctx->result |= effects;
    if (!effects_known) {
      if (ctx->comptime_io_scan)
        ctx->comptime_io_scan->incomplete = true;
      ctx->known = false;
      if (verbose_enabled >= 2)
        ny_dbg_print(NY_DBG_EFFECT, __FILE__, __LINE__, __func__,
                     "UNKNOWN_METHOD method=%s "
                     "reason=%s",
                     e->as.memcall.name ? e->as.memcall.name : "<unnamed>",
                     sig ? "callee-contract" : "unresolved-method");
      if (ny_effect_diag_verbose_enabled())
        ny_effect_diag_log(
            "unknown.detail",
            "method=%s semantic=%d "
            "canonical=%s stmt=%s sig=%s "
            "sig_known=%d",
            e->as.memcall.name ? e->as.memcall.name : "<unnamed>",
            (int)e->semantic.member_call_kind,
            e->semantic.canonical_callee ? e->semantic.canonical_callee
                                         : "<none>",
            e->semantic.canonical_callee_stmt ? "yes" : "no",
            sig && sig->name ? sig->name : "<unresolved>",
            ny_sig_effects_known(sig));
    }
    return false;
  }
  case NY_E_MATCH:
    /*
     * Match patterns are syntax, not calls.
     * Variant patterns such as
     * `Shape.Circle(x)` are represented by the
     * parser as member calls, so visiting
     * pattern nodes invents unresolved runtime
     * method effects. Only the scrutinee,
     * guards, and selected-arm bodies execute.
     */
    ny_visit_expr(v, e->as.match.test);
    ctx->callback_control_flow_depth++;
    for (size_t i = 0; i < e->as.match.arms.len; ++i) {
      match_arm_t *arm = &e->as.match.arms.data[i];
      ny_visit_expr(v, arm->guard);
      ny_visit_stmt(v, arm->conseq);
    }
    ny_visit_stmt(v, e->as.match.default_conseq);
    ctx->callback_control_flow_depth--;
    return false;
  case NY_E_COMPTIME:
    /*
     * Comptime bodies are evaluated during
     * compilation and are not runtime effects
     * of the enclosing function.
     */
    e->semantic.expression_effect_mask = NY_FX_NONE;
    e->semantic.expression_effect_known = true;
    e->semantic.expression_effect_resolved = true;
    return false;
  case NY_E_SIZEOF:
    /*
     * sizeof inspects a type; its operand is
     * not evaluated at runtime.
     */
    e->semantic.expression_effect_mask = NY_FX_NONE;
    e->semantic.expression_effect_known = true;
    e->semantic.expression_effect_resolved = true;
    return false;
  case NY_E_QUOTE:
  case NY_E_SPLICE:
  case NY_E_EXISTS:
    /*
     * These nodes are syntax/proof values at
     * runtime. Keep the comptime IO scan
     * conservative while excluding their
     * bodies from ordinary runtime effects.
     */
    if (ctx && ctx->comptime_io_scan)
      return true;
    e->semantic.expression_effect_mask = NY_FX_NONE;
    e->semantic.expression_effect_known = true;
    e->semantic.expression_effect_resolved = true;
    return false;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
  case NY_E_DICT:
  case NY_E_FSTRING:
  case NY_E_LAMBDA:
  case NY_E_FN:
    ctx->result |= NY_FX_ALLOC;
    return true;
  case NY_E_ASM:
    ctx->result |= NY_FX_FFI;
    return true;
  default:
    return true;
  }
}

typedef struct ny_expr_effect_accum_t {
  uint32_t mask;
  bool known;
  ny_type_t *custom_row;
  bool custom_row_known;
  ny_effects_ctx *ctx;
} ny_expr_effect_accum_t;

static ny_type_t *ny_effect_custom_row_only(ny_type_arena_t *arena,
                                            const ny_type_t *effect_row,
                                            bool *known) {
  if (known)
    *known = false;
  if (!arena)
    return NULL;
  ny_type_t *result = ny_type_row(arena, NULL, NULL, NULL);
  ny_type_t *current = ny_type_find((ny_type_t *)effect_row);
  for (unsigned depth = 0; current && depth < 128; ++depth) {
    if (current->kind == NY_TYPE_EFFECT_VAR || current->kind == NY_TYPE_VAR) {
      ny_type_t *open_tail = ny_type_row(arena, NULL, NULL, current);
      result = open_tail ? ny_type_row_compose(arena, NULL, result, open_tail)
                         : NULL;
      if (known)
        *known = result != NULL;
      return result;
    }
    if (current->kind != NY_TYPE_ROW)
      return NULL;
    if (!current->as.row.label) {
      if (current->as.row.field_type || current->as.row.tail)
        return NULL;
      if (known)
        *known = true;
      return result;
    }
    if (strncmp(current->as.row.label, "std.effects.", 12) != 0) {
      ny_type_t *field = current->as.row.field_type;
      if (!field)
        return NULL;
      result = ny_type_row(arena, current->as.row.label, field, result);
      if (!result)
        return NULL;
    }
    current = ny_type_find(current->as.row.tail);
  }
  if (!current && known)
    *known = true;
  return current ? NULL : result;
}

static ny_type_t *ny_effect_row_replace_open_tail(ny_type_arena_t *arena,
                                                  const ny_type_t *row,
                                                  ny_type_t *replacement) {
  if (!arena || !row || !replacement || ny_effect_type_is_open(replacement))
    return NULL;
  ny_type_t *empty = ny_type_row(arena, NULL, NULL, NULL);
  if (!empty)
    return NULL;
  ny_type_t *head = NULL;
  ny_type_t *prev = NULL;
  ny_type_t *current = ny_type_find((ny_type_t *)row);
  for (unsigned depth = 0; current && depth < 128; ++depth) {
    if (current->kind == NY_TYPE_EFFECT_VAR || current->kind == NY_TYPE_VAR) {
      if (prev)
        prev->as.row.tail = replacement;
      else
        head = replacement;
      return head;
    }
    if (current->kind != NY_TYPE_ROW)
      return NULL;
    if (!current->as.row.label) {
      if (current->as.row.field_type || current->as.row.tail)
        return NULL;
      if (prev)
        prev->as.row.tail = empty;
      return head ? head : empty;
    }
    ny_type_t *copy = ny_type_row(arena, current->as.row.label,
                                  current->as.row.field_type, NULL);
    if (!copy)
      return NULL;
    if (!head)
      head = copy;
    else
      prev->as.row.tail = copy;
    prev = copy;
    current = ny_type_find(current->as.row.tail);
  }
  return current ? NULL : (head ? head : empty);
}

static void ny_effect_store_call_custom_row(ny_effects_ctx *ctx, expr_t *expr,
                                            const ny_type_t *effect_row,
                                            bool known, fun_sig *sig,
                                            ny_call_arg_list *args,
                                            size_t receiver_skip) {
  if (!ctx || !expr || !ctx->fact_arena) {
    if (expr)
      expr->semantic.call_custom_effect_row_known = false;
    return;
  }
  bool row_known = known;
  bool local_callback_row = false;
  const ny_type_t *row = effect_row;
  if (!sig && expr->kind == NY_E_CALL && expr->as.call.callee &&
      expr->as.call.callee->kind == NY_E_IDENT) {
    const char *name = expr->as.call.callee->as.ident.name;
    if (ny_effect_owner_row_callback(ctx, name)) {
      row = ny_effect_owner_contract_row(ctx);
      local_callback_row = true;
    } else {
      bool local_known = false;
      const ny_type_t *local_row =
          ny_effect_local_call_row(ctx, name, &local_known);
      if (local_known) {
        row = local_row;
        local_callback_row = true;
      } else if (ny_effect_local_custom_row_unknown(ctx, name)) {
        row_known = false;
      }
    }
  }
  if (!sig && expr->kind == NY_E_CALL && expr->as.call.callee) {
    const char *fact =
        ny_effect_container_callback_fact(ctx, expr->as.call.callee);
    if (fact) {
      bool local_known = false;
      const ny_type_t *local_row =
          ny_effect_local_call_row(ctx, fact, &local_known);
      if (local_known) {
        row = local_row;
        local_callback_row = true;
      } else if (ny_effect_local_custom_row_unknown(ctx, fact)) {
        row_known = false;
      }
    }
  }
  if (!sig && expr->kind == NY_E_CALL && expr->as.call.callee &&
      ny_effect_container_callback_fact(ctx, expr->as.call.callee)) {
    expr_t *fallback =
        ny_effect_container_get_callback_fallback(expr->as.call.callee);
    if (fallback) {
      bool fallback_known = false;
      const ny_type_t *fallback_row =
          ny_effect_callback_row(ctx, fallback, &fallback_known);
      if (!fallback_known) {
        row_known = false;
      } else {
        row = ny_type_row_compose(ctx->fact_arena, NULL, (ny_type_t *)row,
                                  (ny_type_t *)fallback_row);
        if (!row)
          row_known = false;
        else
          local_callback_row = true;
      }
    }
  }
  bool base_known = false;
  ny_type_t *custom =
      ny_effect_custom_row_only(ctx->fact_arena, row, &base_known);
  bool base_open = ny_effect_type_is_open(row);
  row_known =
      row_known && (base_known || ((sig || local_callback_row) && base_open));
  bool saw_callback = false;
  ny_type_t *callback_effects = ny_type_row(ctx->fact_arena, NULL, NULL, NULL);
  if (row_known && sig && args) {
    size_t arity = (size_t)(sig->arity < 0 ? 0 : sig->arity);
    for (size_t i = 0; i < arity; ++i) {
      if (!ny_effect_param_is_callable(ny_effect_sig_param_type(sig, i)))
        continue;
      expr_t *actual = ny_effect_hof_actual(sig, args, i, receiver_skip);
      if (!actual) {
        row_known = false;
        break;
      }
      saw_callback = true;
      bool callback_known = false;
      const ny_type_t *callback_row =
          ny_effect_callback_row(ctx, actual, &callback_known);
      bool custom_known = false;
      ny_type_t *callback_custom = ny_effect_custom_row_only(
          ctx->fact_arena, callback_row, &custom_known);
      if (!callback_known || !custom_known || !callback_custom) {
        row_known = false;
        break;
      }
      if (ny_effect_type_is_open(callback_custom)) {
        row_known = false;
        break;
      }
      callback_effects = ny_type_row_compose(ctx->fact_arena, NULL,
                                             callback_effects, callback_custom);
      if (!callback_effects) {
        row_known = false;
        break;
      }
    }
  }
  if (row_known && saw_callback) {
    if (sig && base_open) {
      custom = ny_effect_row_replace_open_tail(ctx->fact_arena, custom,
                                               callback_effects);
    } else {
      custom =
          ny_type_row_compose(ctx->fact_arena, NULL, custom, callback_effects);
    }
    if (!custom)
      row_known = false;
  }
  if (base_open && !saw_callback && !local_callback_row)
    row_known = false;
  expr->semantic.call_custom_effect_row =
      row_known ? ny_effect_row_persist(ctx, custom) : NULL;
  expr->semantic.call_custom_effect_row_known =
      row_known && expr->semantic.call_custom_effect_row;
}

static const char *ny_effect_row_persist(ny_effects_ctx *ctx, ny_type_t *row) {
  if (!ctx || !ctx->cg || !ctx->cg->arena || !row)
    return NULL;
  char *serialized = ny_type_effect_row_serialize(row);
  if (!serialized)
    return NULL;
  size_t len = strlen(serialized);
  char *stable = arena_alloc(ctx->cg->arena, len + 1);
  if (stable)
    memcpy(stable, serialized, len + 1);
  free(serialized);
  return stable;
}

static bool ny_effect_accum_custom_row(ny_expr_effect_accum_t *acc,
                                       const char *serialized, bool known) {
  if (!acc || !acc->ctx || !acc->ctx->fact_arena)
    return false;
  if (!known || !serialized) {
    acc->custom_row_known = false;
    return false;
  }
  ny_type_t *row =
      ny_type_effect_row_deserialize(acc->ctx->fact_arena, serialized);
  if (!row) {
    acc->custom_row_known = false;
    return false;
  }
  if (!acc->custom_row)
    acc->custom_row = row;
  else {
    acc->custom_row =
        ny_type_row_compose(acc->ctx->fact_arena, NULL, acc->custom_row, row);
    if (!acc->custom_row) {
      acc->custom_row_known = false;
      return false;
    }
  }
  return true;
}

static void
ny_effect_accum_child_custom_row(ny_expr_effect_accum_t *acc,
                                 const ny_expr_semantic_t *semantic) {
  if (!acc || !semantic)
    return;
  (void)ny_effect_accum_custom_row(
      acc, semantic->expression_custom_effect_row,
      semantic->expression_custom_effect_row_known);
}

static void ny_effect_accum_stmt_custom_row(ny_expr_effect_accum_t *acc,
                                            const stmt_t *stmt) {
  if (!acc || !stmt)
    return;
  (void)ny_effect_accum_custom_row(acc, stmt->semantic_custom_effect_row,
                                   stmt->semantic_custom_effect_row_known);
}

static bool ny_effect_is_assignment_operator(const char *op) {
  if (!op)
    return false;
  return strcmp(op, "=") == 0 || strcmp(op, "+=") == 0 ||
         strcmp(op, "-=") == 0 || strcmp(op, "*=") == 0 ||
         strcmp(op, "/=") == 0 || strcmp(op, "%=") == 0 ||
         strcmp(op, "&=") == 0 || strcmp(op, "|=") == 0 ||
         strcmp(op, "^=") == 0 || strcmp(op, "<<=") == 0 ||
         strcmp(op, ">>=") == 0 || strcmp(op, ">>>=") == 0;
}

static void ny_expr_effect_accum_child(ny_expr_effect_accum_t *acc,
                                       const expr_t *child) {
  if (!acc || !child)
    return;
  ny_effect_accum_child_custom_row(acc, &child->semantic);
  if (!child->semantic.expression_effect_resolved ||
      !child->semantic.expression_effect_known) {
    acc->known = false;
    if (child->semantic.expression_effect_resolved)
      acc->mask |= child->semantic.expression_effect_mask;
    return;
  }
  acc->mask |= child->semantic.expression_effect_mask;
}

static void ny_expr_effect_accum_stmt(ny_expr_effect_accum_t *acc,
                                      const stmt_t *child);

static void effects_visit_expr_post(ny_visitor_t *v, expr_t *e) {
  (void)v;
  if (!e)
    return;
  ny_expr_effect_accum_t acc = {.mask = NY_FX_NONE,
                                .known = true,
                                .custom_row_known = true,
                                .ctx = v ? (ny_effects_ctx *)v->ctx : NULL};
  switch (e->kind) {
  case NY_E_CALL:
  case NY_E_MEMCALL:
    if (e->semantic.call_effect_resolved) {
      acc.mask |= e->semantic.call_effect_mask;
      acc.known = e->semantic.call_effect_known;
      (void)ny_effect_accum_custom_row(
          &acc, e->semantic.call_custom_effect_row,
          e->semantic.call_custom_effect_row_known);
    } else {
      acc.known = false;
      acc.custom_row_known = false;
    }
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
  case NY_E_DICT:
  case NY_E_FSTRING:
  case NY_E_LAMBDA:
  case NY_E_FN:
    acc.mask |= NY_FX_ALLOC;
    break;
  case NY_E_ASM:
    acc.mask |= NY_FX_FFI;
    break;
  case NY_E_UNARY:
    if (e->as.unary.op && (strcmp(e->as.unary.op, "async") == 0 ||
                           strcmp(e->as.unary.op, "await") == 0))
      acc.mask |= NY_FX_THREAD;
    else if (e->as.unary.op && (strcmp(e->as.unary.op, "++") == 0 ||
                                strcmp(e->as.unary.op, "--") == 0))
      acc.mask |= NY_FX_WRITE_MEMORY;
    break;
  case NY_E_BINARY:
    if (ny_effect_is_assignment_operator(e->as.binary.op))
      acc.mask |= NY_FX_WRITE_MEMORY;
    break;
  case NY_E_MATCH:
    /*
     * Every arm is analyzed conservatively by
     * the pre-visit and accumulated below. A
     * known union row is sound even when some
     * arms are unreachable.
     */
    break;
  case NY_E_LOGICAL:
  case NY_E_TERNARY:
  case NY_E_INDEX:
  case NY_E_MEMBER:
  case NY_E_DEREF:
  case NY_E_TRY:
    /*
     * Their runtime work is represented by
     * the evaluated child expressions below.
     */
    break;
  case NY_E_SIZEOF:
    /*
     * The operand is unevaluated.
     */
    e->semantic.expression_effect_mask = NY_FX_NONE;
    e->semantic.expression_effect_known = true;
    e->semantic.expression_effect_resolved = true;
    e->semantic.expression_custom_effect_row = ny_effect_row_persist(
        acc.ctx, acc.custom_row
                     ? acc.custom_row
                     : ny_type_row(acc.ctx->fact_arena, NULL, NULL, NULL));
    e->semantic.expression_custom_effect_row_known =
        acc.custom_row_known &&
        e->semantic.expression_custom_effect_row != NULL;
    return;
  case NY_E_COMPTIME:
  case NY_E_QUOTE:
  case NY_E_SPLICE:
  case NY_E_EXISTS:
    /*
     * These nodes carry compile-time/proof
     * syntax, not runtime effects.
     */
    e->semantic.expression_effect_mask = NY_FX_NONE;
    e->semantic.expression_effect_known = true;
    e->semantic.expression_effect_resolved = true;
    e->semantic.expression_custom_effect_row = ny_effect_row_persist(
        acc.ctx, ny_type_row(acc.ctx->fact_arena, NULL, NULL, NULL));
    e->semantic.expression_custom_effect_row_known =
        e->semantic.expression_custom_effect_row != NULL;
    return;
  case NY_E_IDENT:
  case NY_E_LITERAL:
  case NY_E_INFERRED_MEMBER:
  case NY_E_EMBED:
  case NY_E_PTR_TYPE:
    /*
     * These forms carry a value or type name,
     * not runtime work.
     */
    break;
  default:
    /*
     * New expression forms must not silently
     * acquire a pure effect row.
     */
    acc.known = false;
    break;
  }

#define NY_EFFECT_ADD_CHILD(child) ny_expr_effect_accum_child(&acc, (child))
  switch (e->kind) {
  case NY_E_UNARY:
    NY_EFFECT_ADD_CHILD(e->as.unary.right);
    break;
  case NY_E_BINARY:
  case NY_E_LOGICAL:
    NY_EFFECT_ADD_CHILD(e->as.binary.left);
    NY_EFFECT_ADD_CHILD(e->as.binary.right);
    break;
  case NY_E_TERNARY:
    NY_EFFECT_ADD_CHILD(e->as.ternary.cond);
    NY_EFFECT_ADD_CHILD(e->as.ternary.true_expr);
    NY_EFFECT_ADD_CHILD(e->as.ternary.false_expr);
    break;
  case NY_E_CALL:
    NY_EFFECT_ADD_CHILD(e->as.call.callee);
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      NY_EFFECT_ADD_CHILD(e->as.call.args.data[i].val);
    break;
  case NY_E_MEMCALL:
    NY_EFFECT_ADD_CHILD(e->as.memcall.target);
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      NY_EFFECT_ADD_CHILD(e->as.memcall.args.data[i].val);
    break;
  case NY_E_INDEX:
    NY_EFFECT_ADD_CHILD(e->as.index.target);
    NY_EFFECT_ADD_CHILD(e->as.index.start);
    NY_EFFECT_ADD_CHILD(e->as.index.stop);
    NY_EFFECT_ADD_CHILD(e->as.index.step);
    break;
  case NY_E_MEMBER:
    NY_EFFECT_ADD_CHILD(e->as.member.target);
    NY_EFFECT_ADD_CHILD(e->as.member.computed_name);
    break;
  case NY_E_DEREF:
    NY_EFFECT_ADD_CHILD(e->as.deref.target);
    break;
  case NY_E_SIZEOF:
    if (!e->as.szof.is_type)
      NY_EFFECT_ADD_CHILD(e->as.szof.target);
    break;
  case NY_E_TRY:
    NY_EFFECT_ADD_CHILD(e->as.try_expr.target);
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      NY_EFFECT_ADD_CHILD(e->as.list_like.data[i]);
    break;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      NY_EFFECT_ADD_CHILD(e->as.dict.pairs.data[i].key);
      NY_EFFECT_ADD_CHILD(e->as.dict.pairs.data[i].value);
    }
    break;
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; ++i)
      if (e->as.fstring.parts.data[i].kind == NY_FSP_EXPR)
        NY_EFFECT_ADD_CHILD(e->as.fstring.parts.data[i].as.e);
    break;
  case NY_E_ASM:
    for (size_t i = 0; i < e->as.as_asm.args.len; ++i)
      NY_EFFECT_ADD_CHILD(e->as.as_asm.args.data[i]);
    break;
  case NY_E_MATCH:
    NY_EFFECT_ADD_CHILD(e->as.match.test);
    for (size_t i = 0; i < e->as.match.arms.len; ++i) {
      const match_arm_t *arm = &e->as.match.arms.data[i];
      NY_EFFECT_ADD_CHILD(arm->guard);
      ny_expr_effect_accum_stmt(&acc, arm->conseq);
    }
    ny_expr_effect_accum_stmt(&acc, e->as.match.default_conseq);
    break;
  default:
    break;
  }
#undef NY_EFFECT_ADD_CHILD

  e->semantic.expression_effect_mask = acc.mask;
  e->semantic.expression_effect_known = acc.known;
  e->semantic.expression_effect_resolved = true;
  if (acc.custom_row_known) {
    ny_type_t *custom_row =
        acc.custom_row ? acc.custom_row
                       : ny_type_row(acc.ctx->fact_arena, NULL, NULL, NULL);
    e->semantic.expression_custom_effect_row =
        ny_effect_row_persist(acc.ctx, custom_row);
    e->semantic.expression_custom_effect_row_known =
        e->semantic.expression_custom_effect_row != NULL;
  } else {
    e->semantic.expression_custom_effect_row = NULL;
    e->semantic.expression_custom_effect_row_known = false;
  }
}

static void ny_expr_effect_accum_stmt(ny_expr_effect_accum_t *acc,
                                      const stmt_t *child) {
  if (!acc || !child)
    return;
  ny_effect_accum_stmt_custom_row(acc, child);
  if (!child->semantic_effect_resolved || !child->semantic_effect_known) {
    acc->known = false;
    if (child->semantic_effect_resolved)
      acc->mask |= child->semantic_effect_mask;
    return;
  }
  acc->mask |= child->semantic_effect_mask;
}

static void effects_visit_stmt_post(ny_visitor_t *v, stmt_t *s) {
  ny_effects_ctx *ctx = v ? (ny_effects_ctx *)v->ctx : NULL;
  if (!s)
    return;
  ny_expr_effect_accum_t acc = {
      .mask = NY_FX_NONE, .known = true, .custom_row_known = true, .ctx = ctx};
#define NY_EFFECT_ADD_EXPR(child) ny_expr_effect_accum_child(&acc, (child))
#define NY_EFFECT_ADD_STMT(child) ny_expr_effect_accum_stmt(&acc, (child))
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      NY_EFFECT_ADD_STMT(s->as.block.body.data[i]);
    break;
  case NY_S_VAR:
    for (size_t i = 0; i < s->as.var.exprs.len; ++i)
      NY_EFFECT_ADD_EXPR(s->as.var.exprs.data[i]);
    break;
  case NY_S_EXPR:
    NY_EFFECT_ADD_EXPR(s->as.expr.expr);
    break;
  case NY_S_RETURN:
    NY_EFFECT_ADD_EXPR(s->as.ret.value);
    break;
  case NY_S_IF: {
    NY_EFFECT_ADD_STMT(s->as.iff.init);
    NY_EFFECT_ADD_EXPR(s->as.iff.test);
    bool selected_comptime_branch = false;
    if (ctx && s->as.iff.test && s->as.iff.test->kind == NY_E_COMPTIME) {
      ny_ct_fast_val_t value = ny_ct_fast_none();
      bool truthy = false;
      if (ny_try_eval_comptime_expr_fast(ctx->cg, s->as.iff.test, &value, 0) &&
          ny_ct_fast_truthy(&value, &truthy)) {
        NY_EFFECT_ADD_STMT(truthy ? s->as.iff.conseq : s->as.iff.alt);
        selected_comptime_branch = true;
      }
    }
    if (!selected_comptime_branch) {
      NY_EFFECT_ADD_STMT(s->as.iff.conseq);
      NY_EFFECT_ADD_STMT(s->as.iff.alt);
    }
    break;
  }
  case NY_S_GUARD:
    NY_EFFECT_ADD_EXPR(s->as.guard.value);
    NY_EFFECT_ADD_STMT(s->as.guard.fallback);
    break;
  case NY_S_WHILE:
    NY_EFFECT_ADD_EXPR(s->as.whl.test);
    NY_EFFECT_ADD_STMT(s->as.whl.init);
    NY_EFFECT_ADD_STMT(s->as.whl.body);
    NY_EFFECT_ADD_STMT(s->as.whl.update);
    break;
  case NY_S_FOR:
    NY_EFFECT_ADD_STMT(s->as.fr.init);
    NY_EFFECT_ADD_EXPR(s->as.fr.cond);
    NY_EFFECT_ADD_EXPR(s->as.fr.iterable);
    NY_EFFECT_ADD_STMT(s->as.fr.body);
    NY_EFFECT_ADD_STMT(s->as.fr.update);
    break;
  case NY_S_MATCH:
    NY_EFFECT_ADD_EXPR(s->as.match.test);
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      const match_arm_t *arm = &s->as.match.arms.data[i];
      NY_EFFECT_ADD_EXPR(arm->guard);
      NY_EFFECT_ADD_STMT(arm->conseq);
    }
    NY_EFFECT_ADD_STMT(s->as.match.default_conseq);
    break;
  case NY_S_TRY:
    NY_EFFECT_ADD_STMT(s->as.tr.body);
    NY_EFFECT_ADD_STMT(s->as.tr.handler);
    break;
  case NY_S_DEFER:
    NY_EFFECT_ADD_STMT(s->as.de.body);
    break;
  case NY_S_FUNC:
  case NY_S_LEMMA:
  case NY_S_MACRO:
  case NY_S_USE:
  case NY_S_EXTERN:
  case NY_S_LINK:
  case NY_S_LABEL:
  case NY_S_GOTO:
  case NY_S_BREAK:
  case NY_S_CONTINUE:
  case NY_S_LAYOUT:
  case NY_S_MODULE:
  case NY_S_EXPORT:
  case NY_S_STRUCT:
  case NY_S_ENUM:
  case NY_S_INCLUDE:
  case NY_S_DEFINE:
  case NY_S_OPERATOR:
  case NY_S_IMPL:
  case NY_S_REFINE:
  case NY_S_TRAIT:
    /*
     * Declarations and proof/macro bodies are
     * not runtime execution here.
     */
    break;
  default:
    /*
     * New or unsupported statement forms must
     * not claim a complete row.
     */
    acc.known = false;
    break;
  }
#undef NY_EFFECT_ADD_EXPR
#undef NY_EFFECT_ADD_STMT
  s->semantic_effect_mask = acc.mask;
  s->semantic_effect_known = acc.known;
  s->semantic_effect_resolved = true;
  if (acc.custom_row_known) {
    ny_type_t *custom_row =
        acc.custom_row
            ? acc.custom_row
            : ny_type_row(ctx ? ctx->fact_arena : NULL, NULL, NULL, NULL);
    s->semantic_custom_effect_row = ny_effect_row_persist(ctx, custom_row);
    s->semantic_custom_effect_row_known = s->semantic_custom_effect_row != NULL;
  } else {
    s->semantic_custom_effect_row = NULL;
    s->semantic_custom_effect_row_known = false;
  }
}

static bool ny_effect_comptime_match_arm(codegen_t *cg, expr_t *test,
                                         const ny_match_arm_list *arms,
                                         size_t *selected, bool *has_arm) {
  if (!cg || !test || !arms || !selected || !has_arm)
    return false;
  ny_ct_fast_val_t value = ny_ct_fast_none();
  if (!ny_try_eval_comptime_expr_fast(cg, test, &value, 0))
    return false;
  int64_t value_i = 0;
  if (value.kind == NY_CT_FAST_INT)
    value_i = value.i;
  else if (value.kind == NY_CT_FAST_BOOL)
    value_i = value.b ? 1 : 0;
  else
    return false;

  for (size_t i = 0; i < arms->len; ++i) {
    const match_arm_t *arm = &arms->data[i];
    for (size_t j = 0; j < arm->patterns.len; ++j) {
      ny_ct_fast_val_t pattern = ny_ct_fast_none();
      expr_t *pattern_expr = arm->patterns.data[j];
      bool pattern_evaluated =
          ny_try_eval_comptime_expr_fast(cg, pattern_expr, &pattern, 0);
      if (!pattern_evaluated && pattern_expr &&
          pattern_expr->kind == NY_E_BINARY && pattern_expr->as.binary.op &&
          strcmp(pattern_expr->as.binary.op, "..") == 0) {
        ny_ct_fast_val_t lo = ny_ct_fast_none();
        ny_ct_fast_val_t hi = ny_ct_fast_none();
        if (!ny_try_eval_comptime_expr_fast(cg, pattern_expr->as.binary.left,
                                            &lo, 0) ||
            !ny_try_eval_comptime_expr_fast(cg, pattern_expr->as.binary.right,
                                            &hi, 0) ||
            lo.kind != NY_CT_FAST_INT || hi.kind != NY_CT_FAST_INT)
          return false;
        if (value_i >= lo.i && value_i < hi.i) {
          *selected = i;
          *has_arm = true;
          return true;
        }
        continue;
      }
      if (!pattern_evaluated)
        return false;
      if (pattern.kind == NY_CT_FAST_INT && pattern.i == value_i) {
        *selected = i;
        *has_arm = true;
        return true;
      }
      if (pattern.kind == NY_CT_FAST_BOOL && (pattern.b ? 1 : 0) == value_i) {
        *selected = i;
        *has_arm = true;
        return true;
      }
      if (pattern.kind == NY_CT_FAST_RANGE) {
        int64_t lo = pattern.range_start;
        int64_t hi = pattern.range_stop;
        if ((pattern.range_step >= 0 && value_i >= lo && value_i < hi) ||
            (pattern.range_step < 0 && value_i <= lo && value_i > hi)) {
          *selected = i;
          *has_arm = true;
          return true;
        }
      } else if (pattern.kind != NY_CT_FAST_INT &&
                 pattern.kind != NY_CT_FAST_BOOL) {
        return false;
      }
    }
  }
  *has_arm = false;
  return true;
}

static void ny_effect_record_dict_callback_facts(ny_effects_ctx *ctx,
                                                 const char *container,
                                                 const expr_t *dict) {
  if (!ctx || !container || !dict || dict->kind != NY_E_DICT)
    return;
  bool summary_complete = dict->as.dict.pairs.len > 0;
  bool summary_rows_known = true;
  uint32_t summary_effects = NY_FX_NONE;
  ny_type_t *summary_row = ny_type_row(ctx->fact_arena, NULL, NULL, NULL);
  if (!summary_row)
    summary_complete = false;
  for (size_t i = 0; i < dict->as.dict.pairs.len; ++i) {
    const dict_pair_t *pair = &dict->as.dict.pairs.data[i];
    if (!pair->key || pair->key->kind != NY_E_LITERAL ||
        pair->key->as.literal.kind != NY_LIT_STR ||
        !pair->key->as.literal.as.s.data || !pair->value) {
      summary_complete = false;
      continue;
    }
    bool syntactically_callable =
        pair->value->kind == NY_E_LAMBDA || pair->value->kind == NY_E_FN ||
        pair->value->kind == NY_E_CALL || pair->value->kind == NY_E_MEMCALL ||
        pair->value->kind == NY_E_IDENT || pair->value->kind == NY_E_MEMBER ||
        pair->value->kind == NY_E_INDEX;
    if (!syntactically_callable) {
      summary_complete = false;
      continue;
    }
    const char *fact_name = ny_effect_dict_callback_fact_name(
        ctx, container, pair->key->as.literal.as.s.data);
    if (!fact_name) {
      summary_complete = false;
      continue;
    }
    uint32_t effects = NY_FX_ALL;
    bool known = false;
    bool callable =
        ny_effect_callback_value_summary(ctx, pair->value, &effects, &known);
    bool callback_row_known = false;
    const ny_type_t *callback_row =
        ny_effect_callback_row(ctx, pair->value, &callback_row_known);
    if (ny_effect_diag_verbose_enabled())
      ny_effect_diag_log("container.dict.callback",
                         "container=%s key=%s kind=%d callable=%d known=%d "
                         "row_known=%d row=%p",
                         container, pair->key->as.literal.as.s.data,
                         (int)pair->value->kind, callable, known,
                         callback_row_known, (const void *)callback_row);
    if (!callable && pair->value->kind == NY_E_IDENT &&
        pair->value->as.ident.name &&
        ny_effect_owner_row_callback(ctx, pair->value->as.ident.name)) {
      callable = true;
      known = true;
      effects = NY_FX_NONE;
    }
    if (!callable || !known) {
      summary_complete = false;
      continue;
    }
    ny_effect_record_local_container(ctx, fact_name, NULL, NULL, NULL, effects,
                                     known, true, false, 0, false, false, 0,
                                     false);
    ny_effect_record_callback_value_row(ctx, fact_name, pair->value, true);
    summary_effects |= effects;
    bool row_known = false;
    const ny_type_t *row = callback_row;
    row_known = callback_row_known;
    if (!row_known) {
      summary_rows_known = false;
    } else if (row) {
      ny_type_t *composed = ny_type_row_compose(ctx->fact_arena, NULL,
                                                summary_row, (ny_type_t *)row);
      if (composed)
        summary_row = composed;
      else
        summary_rows_known = false;
    }
  }
  if (summary_complete && summary_rows_known) {
    const char *summary_name =
        ny_effect_dict_callback_summary_fact_name(ctx, container);
    if (summary_name) {
      ny_effect_record_local_container(ctx, summary_name, NULL, NULL, NULL,
                                       summary_effects, true, true, false, 0,
                                       false, false, 0, false);
      ny_effect_record_local_call_row(ctx, summary_name, summary_row, true);
    }
  }
}

static void ny_effect_record_list_callback_facts(ny_effects_ctx *ctx,
                                                 const char *container,
                                                 const expr_t *list) {
  if (!ctx || !container || !list ||
      (list->kind != NY_E_LIST && list->kind != NY_E_TUPLE))
    return;
  bool summary_complete = list->as.list_like.len > 0;
  bool summary_saw_callable = false;
  bool summary_rows_known = true;
  uint32_t summary_effects = NY_FX_NONE;
  ny_type_t *summary_row = ny_type_row(ctx->fact_arena, NULL, NULL, NULL);
  if (!summary_row)
    summary_complete = false;
  for (size_t i = 0; i < list->as.list_like.len; ++i) {
    expr_t *value = list->as.list_like.data[i];
    if (!value) {
      summary_complete = false;
      continue;
    }
    bool syntactically_callable =
        value->kind == NY_E_LAMBDA || value->kind == NY_E_FN ||
        value->kind == NY_E_CALL || value->kind == NY_E_MEMCALL ||
        value->kind == NY_E_IDENT || value->kind == NY_E_MEMBER;
    if (!syntactically_callable)
      continue;
    const char *fact_name =
        ny_effect_list_callback_fact_name(ctx, container, (int64_t)i);
    if (!fact_name)
      continue;
    uint32_t effects = NY_FX_ALL;
    bool known = false;
    bool callable =
        ny_effect_callback_value_summary(ctx, value, &effects, &known);
    if (!callable && value->kind == NY_E_IDENT && value->as.ident.name &&
        ny_effect_owner_row_callback(ctx, value->as.ident.name)) {
      callable = true;
      known = true;
      effects = NY_FX_NONE;
    }
    if (!callable || !known) {
      summary_complete = false;
      continue;
    }
    summary_saw_callable = true;
    summary_effects |= effects;
    bool row_known = false;
    const ny_type_t *row = ny_effect_callback_row(ctx, value, &row_known);
    if (!row_known) {
      summary_rows_known = false;
    } else if (row) {
      ny_type_t *composed = ny_type_row_compose(ctx->fact_arena, NULL,
                                                summary_row, (ny_type_t *)row);
      if (composed)
        summary_row = composed;
      else
        summary_rows_known = false;
    }
    ny_effect_record_local_container(ctx, fact_name, NULL, NULL, NULL, effects,
                                     known, true, false, 0, false, false, 0,
                                     false);
    ny_effect_record_callback_value_row(ctx, fact_name, value, true);
  }
  if (summary_complete && summary_saw_callable && summary_rows_known) {
    const char *summary_name =
        ny_effect_list_callback_summary_fact_name(ctx, container);
    if (summary_name) {
      ny_effect_record_local_container(ctx, summary_name, NULL, NULL, NULL,
                                       summary_effects, true, true, false, 0,
                                       false, false, 0, false);
      ny_effect_record_local_call_row(ctx, summary_name, summary_row, true);
    }
  }
}

/*
 * A direct constant-index assignment replaces exactly one list callback slot.
 * The caller invalidates the mutated list's snapshot, leaving its COW aliases
 * unchanged, then this function installs fresh provenance for the written
 * slot only.
 */
static void ny_effect_record_indexed_callback_store(ny_effects_ctx *ctx,
                                                    expr_t *lhs, expr_t *rhs) {
  if (!ctx || !lhs || lhs->kind != NY_E_INDEX || !lhs->as.index.target ||
      lhs->as.index.stop || lhs->as.index.step)
    return;
  ny_effect_record_container_callback_store(ctx, lhs->as.index.target,
                                            lhs->as.index.start, rhs);
}

static void ny_effect_record_container_callback_store(ny_effects_ctx *ctx,
                                                      expr_t *target,
                                                      expr_t *key,
                                                      expr_t *rhs) {
  if (!ctx || !target || target->kind != NY_E_IDENT || !target->as.ident.name ||
      !key || !rhs)
    return;
  const char *container = target->as.ident.name;
  const char *type = ny_effect_local_container_type(ctx, container);
  const char *fact_name = NULL;
  if (type && strcmp(type, "list") == 0 && key->kind == NY_E_LITERAL &&
      ny_literal_is_signed_i64(&key->as.literal) && key->as.literal.as.i >= 0) {
    fact_name =
        ny_effect_list_callback_fact_name(ctx, container, key->as.literal.as.i);
  } else if (type && strcmp(type, "dict") == 0 && key->kind == NY_E_LITERAL &&
             key->as.literal.kind == NY_LIT_STR && key->as.literal.as.s.data) {
    fact_name = ny_effect_dict_callback_fact_name(ctx, container,
                                                  key->as.literal.as.s.data);
  }
  if (!fact_name)
    return;

  uint32_t effects = NY_FX_ALL;
  bool known = false;
  if (!ny_effect_callback_value_summary(ctx, rhs, &effects, &known) || !known)
    return;
  ny_effect_record_local_container(ctx, fact_name, NULL, NULL, NULL, effects,
                                   true, true, false, 0, false, false, 0,
                                   false);
  ny_effect_record_callback_value_row(ctx, fact_name, rhs, true);
}

static void ny_effect_record_mutated_container_callback_summary(
    ny_effects_ctx *ctx, const char *container,
    const ny_effect_local_fact_t *prior_summary, expr_t *rhs) {
  if (!ctx || !container || !prior_summary || !prior_summary->is_callable ||
      !prior_summary->call_effects_known ||
      !prior_summary->call_effect_row_known ||
      prior_summary->call_effect_custom_row_unknown || !rhs)
    return;
  uint32_t rhs_effects = NY_FX_ALL;
  bool rhs_effects_known = false;
  if (!ny_effect_callback_value_summary(ctx, rhs, &rhs_effects,
                                        &rhs_effects_known) ||
      !rhs_effects_known)
    return;
  bool rhs_row_known = false;
  const ny_type_t *rhs_row = ny_effect_callback_row(ctx, rhs, &rhs_row_known);
  if (!rhs_row_known)
    return;
  const ny_type_t *joined_row = prior_summary->call_effect_row;
  if (rhs_row) {
    if (!joined_row) {
      joined_row = rhs_row;
    } else {
      joined_row = ny_type_row_compose(
          ctx->fact_arena, NULL, (ny_type_t *)joined_row, (ny_type_t *)rhs_row);
      if (!joined_row)
        return;
    }
  }
  const char *container_type = ny_effect_local_container_type(ctx, container);
  const char *summary_name =
      container_type && strcmp(container_type, "dict") == 0
          ? ny_effect_dict_callback_summary_fact_name(ctx, container)
      : container_type && strcmp(container_type, "list") == 0
          ? ny_effect_list_callback_summary_fact_name(ctx, container)
          : NULL;
  if (!summary_name)
    return;
  ny_effect_record_local_container(ctx, summary_name, NULL, NULL, NULL,
                                   prior_summary->call_effects | rhs_effects,
                                   true, true, false, 0, false, false, 0,
                                   false);
  ny_effect_record_local_call_row(ctx, summary_name, joined_row, true);
}

static bool ny_effect_returned_container_store_flow_supported(
    const stmt_t *stmt, const char *const *local_names,
    const bool *carries_container, size_t local_len, bool list_container,
    unsigned depth) {
  if (!stmt)
    return true;
  if (depth > 32)
    return false;
  if (stmt->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < stmt->as.block.body.len; ++i)
      if (!ny_effect_returned_container_store_flow_supported(
              stmt->as.block.body.data[i], local_names, carries_container,
              local_len, list_container, depth + 1))
        return false;
    return true;
  }
  if (stmt->kind == NY_S_IF)
    return !stmt->as.iff.init &&
           ny_effect_callback_return_test_supported(stmt->as.iff.test, 0) &&
           ny_effect_returned_container_store_flow_supported(
               stmt->as.iff.conseq, local_names, carries_container, local_len,
               list_container, depth + 1) &&
           ny_effect_returned_container_store_flow_supported(
               stmt->as.iff.alt, local_names, carries_container, local_len,
               list_container, depth + 1);
  if (stmt->kind == NY_S_MATCH) {
    if (!ny_effect_callback_return_test_supported(stmt->as.match.test, 0))
      return false;
    for (size_t i = 0; i < stmt->as.match.arms.len; ++i) {
      const match_arm_t *arm = &stmt->as.match.arms.data[i];
      if (!arm->patterns.len ||
          (arm->guard &&
           !ny_effect_callback_return_test_supported(arm->guard, 0)) ||
          !ny_effect_returned_container_store_flow_supported(
              arm->conseq, local_names, carries_container, local_len,
              list_container, depth + 1))
        return false;
      for (size_t j = 0; j < arm->patterns.len; ++j)
        if (!ny_effect_callback_return_test_supported(arm->patterns.data[j], 0))
          return false;
    }
    return ny_effect_returned_container_store_flow_supported(
        stmt->as.match.default_conseq, local_names, carries_container,
        local_len, list_container, depth + 1);
  }
  if (stmt->kind != NY_S_EXPR || !stmt->as.expr.expr)
    return false;
  const expr_t *expr = stmt->as.expr.expr;
  const char *target = NULL;
  if (!list_container && expr->kind == NY_E_MEMCALL && expr->as.memcall.name &&
      strcmp(expr->as.memcall.name, "set") == 0 && expr->as.memcall.target &&
      expr->as.memcall.target->kind == NY_E_IDENT &&
      expr->as.memcall.target->as.ident.name &&
      expr->as.memcall.args.len == 2 &&
      ny_effect_callback_store_value_is_stable(
          expr->as.memcall.args.data[0].val) &&
      ny_effect_callback_store_value_is_stable(
          expr->as.memcall.args.data[1].val))
    target = expr->as.memcall.target->as.ident.name;
  if (list_container && expr->kind == NY_E_BINARY && expr->as.binary.op &&
      strcmp(expr->as.binary.op, "=") == 0 && expr->as.binary.left &&
      expr->as.binary.left->kind == NY_E_INDEX &&
      !expr->as.binary.left->as.index.stop &&
      !expr->as.binary.left->as.index.step &&
      expr->as.binary.left->as.index.target &&
      expr->as.binary.left->as.index.target->kind == NY_E_IDENT &&
      expr->as.binary.left->as.index.target->as.ident.name &&
      ny_effect_callback_store_value_is_stable(
          expr->as.binary.left->as.index.start) &&
      ny_effect_callback_store_value_is_stable(expr->as.binary.right))
    target = expr->as.binary.left->as.index.target->as.ident.name;
  if (list_container && expr->kind == NY_E_CALL &&
      expr->as.call.args.len == 3 && expr->as.call.args.data[0].val &&
      expr->as.call.args.data[0].val->kind == NY_E_IDENT &&
      expr->as.call.args.data[0].val->as.ident.name &&
      ny_effect_callback_store_value_is_stable(
          expr->as.call.args.data[1].val) &&
      ny_effect_callback_store_value_is_stable(
          expr->as.call.args.data[2].val)) {
    const expr_t *callee = expr->as.call.callee;
    bool set_idx =
        (expr->semantic.canonical_callee &&
         strcmp(expr->semantic.canonical_callee, "core.set_idx") == 0) ||
        (callee && callee->kind == NY_E_IDENT && callee->as.ident.name &&
         strcmp(callee->as.ident.name, "set_idx") == 0);
    if (set_idx)
      target = expr->as.call.args.data[0].val->as.ident.name;
  }
  if (!target)
    return false;
  for (size_t i = 0; i < local_len; ++i)
    if (carries_container[i] && strcmp(target, local_names[i]) == 0)
      return true;
  return false;
}

static bool ny_effect_returned_container_branch_return(const stmt_t *stmt,
                                                       const expr_t **returned,
                                                       unsigned depth) {
  if (!stmt || !returned || depth > 32)
    return false;
  if (stmt->kind == NY_S_BLOCK) {
    if (stmt->as.block.body.len != 1)
      return false;
    return ny_effect_returned_container_branch_return(
        stmt->as.block.body.data[0], returned, depth + 1);
  }
  if (stmt->kind == NY_S_RETURN) {
    const expr_t *value = stmt->as.ret.value;
    if (!value || value->kind != NY_E_IDENT || !value->as.ident.name)
      return false;
    *returned = value;
    return true;
  }
  if (stmt->kind == NY_S_IF) {
    if (stmt->as.iff.init ||
        !ny_effect_callback_return_test_supported(stmt->as.iff.test, 0) ||
        !stmt->as.iff.alt)
      return false;
    const expr_t *then_value = NULL, *else_value = NULL;
    if (!ny_effect_returned_container_branch_return(stmt->as.iff.conseq,
                                                    &then_value, depth + 1) ||
        !ny_effect_returned_container_branch_return(stmt->as.iff.alt,
                                                    &else_value, depth + 1) ||
        !then_value || !else_value || !then_value->as.ident.name ||
        !else_value->as.ident.name ||
        strcmp(then_value->as.ident.name, else_value->as.ident.name) != 0 ||
        then_value->as.ident.syntax_ctx != else_value->as.ident.syntax_ctx)
      return false;
    *returned = then_value;
    return true;
  }
  if (stmt->kind == NY_S_MATCH) {
    if (!ny_effect_callback_return_test_supported(stmt->as.match.test, 0) ||
        !stmt->as.match.default_conseq || stmt->as.match.arms.len == 0)
      return false;
    const expr_t *same_value = NULL;
    for (size_t i = 0; i < stmt->as.match.arms.len; ++i) {
      const match_arm_t *arm = &stmt->as.match.arms.data[i];
      if (!arm->patterns.len ||
          (arm->guard &&
           !ny_effect_callback_return_test_supported(arm->guard, 0)))
        return false;
      for (size_t j = 0; j < arm->patterns.len; ++j)
        if (!ny_effect_callback_return_test_supported(arm->patterns.data[j], 0))
          return false;
      const expr_t *arm_value = NULL;
      if (!ny_effect_returned_container_branch_return(arm->conseq, &arm_value,
                                                      depth + 1) ||
          !arm_value || !arm_value->as.ident.name)
        return false;
      if (!same_value)
        same_value = arm_value;
      else if (strcmp(same_value->as.ident.name, arm_value->as.ident.name) !=
                   0 ||
               same_value->as.ident.syntax_ctx !=
                   arm_value->as.ident.syntax_ctx)
        return false;
    }
    const expr_t *default_value = NULL;
    if (!ny_effect_returned_container_branch_return(
            stmt->as.match.default_conseq, &default_value, depth + 1) ||
        !default_value || !default_value->as.ident.name ||
        strcmp(same_value->as.ident.name, default_value->as.ident.name) != 0 ||
        same_value->as.ident.syntax_ctx != default_value->as.ident.syntax_ctx)
      return false;
    *returned = same_value;
    return true;
  }
  return false;
}

static bool ny_effect_returned_container_forward_param(
    const ny_effects_ctx *ctx, const stmt_t *function, unsigned depth,
    size_t *param_index, const expr_t **source) {
  if (!ctx || !function || function->kind != NY_S_FUNC ||
      !function->as.fn.body || depth > 16)
    return false;
  const stmt_t *body = function->as.fn.body;
  if (body->kind != NY_S_BLOCK || body->as.block.body.len == 0 ||
      body->as.block.body.len > 17)
    return false;
  const char *alias_names[16] = {0};
  uint32_t alias_contexts[16] = {0};
  size_t alias_params[16] = {0};
  size_t alias_count = body->as.block.body.len - 1;
  for (size_t j = 0; j < alias_count; ++j) {
    const stmt_t *decl = body->as.block.body.data[j];
    const expr_t *rhs =
        decl && decl->kind == NY_S_VAR && decl->as.var.exprs.len == 1
            ? decl->as.var.exprs.data[0]
            : NULL;
    if (!decl || decl->kind != NY_S_VAR || !decl->as.var.is_decl ||
        decl->as.var.is_mut || decl->as.var.is_del ||
        decl->as.var.names.len != 1 || !decl->as.var.names.data[0] || !rhs ||
        rhs->kind != NY_E_IDENT || !rhs->as.ident.name)
      return false;
    for (size_t k = 0; k < alias_count; ++k) {
      if (k < j && strcmp(alias_names[k], decl->as.var.names.data[0]) == 0 &&
          alias_contexts[k] == decl->syntax_ctx)
        return false;
    }
    size_t source_param = SIZE_MAX;
    for (size_t i = 0; i < j; ++i)
      if (alias_names[i] && strcmp(alias_names[i], rhs->as.ident.name) == 0 &&
          alias_contexts[i] == rhs->as.ident.syntax_ctx) {
        source_param = alias_params[i];
        break;
      }
    if (source_param == SIZE_MAX)
      for (size_t i = 0; i < function->as.fn.params.len; ++i) {
        const param_t *param = &function->as.fn.params.data[i];
        if (param->name && strcmp(param->name, rhs->as.ident.name) == 0 &&
            param->syntax_ctx == rhs->as.ident.syntax_ctx) {
          source_param = i;
          break;
        }
      }
    if (source_param == SIZE_MAX)
      return false;
    for (size_t i = 0; i < function->as.fn.params.len; ++i) {
      const param_t *param = &function->as.fn.params.data[i];
      if (param->name && strcmp(param->name, decl->as.var.names.data[0]) == 0 &&
          param->syntax_ctx == decl->syntax_ctx)
        return false;
    }
    alias_names[j] = decl->as.var.names.data[0];
    alias_contexts[j] = decl->syntax_ctx;
    alias_params[j] = source_param;
  }
  const stmt_t *ret = body->as.block.body.data[alias_count];
  if (!ret || ret->kind != NY_S_RETURN || !ret->as.ret.value)
    return false;
  const expr_t *value = ret->as.ret.value;
  if (value->kind == NY_E_IDENT && value->as.ident.name) {
    for (size_t i = 0; i < alias_count; ++i)
      if (strcmp(alias_names[i], value->as.ident.name) == 0 &&
          alias_contexts[i] == value->as.ident.syntax_ctx) {
        *param_index = alias_params[i];
        *source = value;
        return true;
      }
    for (size_t i = 0; i < function->as.fn.params.len; ++i) {
      const param_t *param = &function->as.fn.params.data[i];
      if (param->name && strcmp(param->name, value->as.ident.name) == 0 &&
          param->syntax_ctx == value->as.ident.syntax_ctx) {
        *param_index = i;
        *source = value;
        return true;
      }
    }
    return false;
  }
  if (value->kind != NY_E_CALL)
    return false;
  if (!ctx->cg)
    return false;
  /*
   * Argument evaluation belongs to the forwarding body too. A nested call
   * here may mutate or escape the container before the inner return.
   */
  for (size_t i = 0; i < value->as.call.args.len; ++i) {
    const expr_t *arg = value->as.call.args.data[i].val;
    if (!arg || (arg->kind != NY_E_IDENT && arg->kind != NY_E_LITERAL))
      return false;
  }
  stmt_t *inner_function = value->semantic.canonical_callee_stmt;
  fun_sig *inner_sig = ny_purity_resolve_call_sig(
      ctx->cg, (expr_call_t *)&value->as.call, ctx->local_names,
      ctx->local_hashes,
      ctx->local_bloom ? ctx->local_bloom : (uint64_t[4]){0});
  if ((!inner_function || inner_function->kind != NY_S_FUNC) && inner_sig)
    inner_function = inner_sig->stmt_t;
  size_t inner_param_index = 0;
  const expr_t *inner_source = NULL;
  if (!inner_sig || !inner_function ||
      !ny_effect_returned_container_forward_param(
          ctx, inner_function, depth + 1, &inner_param_index, &inner_source) ||
      !inner_source)
    return false;
  expr_t *actual = ny_effect_hof_actual(inner_sig, &value->as.call.args,
                                        inner_param_index, 0);
  if (!actual || actual->kind != NY_E_IDENT || !actual->as.ident.name)
    return false;
  for (size_t i = 0; i < alias_count; ++i)
    if (strcmp(alias_names[i], actual->as.ident.name) == 0 &&
        alias_contexts[i] == actual->as.ident.syntax_ctx) {
      *param_index = alias_params[i];
      *source = actual;
      return true;
    }
  for (size_t i = 0; i < function->as.fn.params.len; ++i) {
    const param_t *param = &function->as.fn.params.data[i];
    if (param->name && strcmp(param->name, actual->as.ident.name) == 0 &&
        param->syntax_ctx == actual->as.ident.syntax_ctx) {
      *param_index = i;
      *source = actual;
      return true;
    }
  }
  return false;
}

static expr_t *ny_effect_direct_returned_container(const ny_effects_ctx *ctx,
                                                   const expr_t *value) {
  if (!ctx || !value || value->kind != NY_E_CALL)
    return NULL;
  stmt_t *function = value->semantic.canonical_callee_stmt;
  fun_sig *sig = NULL;
  if ((!function || function->kind != NY_S_FUNC) && ctx->cg) {
    sig = ny_purity_resolve_call_sig(ctx->cg, (expr_call_t *)&value->as.call,
                                     ctx->local_names, ctx->local_hashes,
                                     ctx->local_bloom ? ctx->local_bloom
                                                      : (uint64_t[4]){0});
    if (sig && sig->stmt_t && sig->stmt_t->kind == NY_S_FUNC)
      function = sig->stmt_t;
  }
  if (!sig && ctx->cg)
    sig = ny_purity_resolve_call_sig(ctx->cg, (expr_call_t *)&value->as.call,
                                     ctx->local_names, ctx->local_hashes,
                                     ctx->local_bloom ? ctx->local_bloom
                                                      : (uint64_t[4]){0});
  if (!function || function->kind != NY_S_FUNC || !function->as.fn.body)
    return NULL;
  stmt_t *body = function->as.fn.body;
  if (sig) {
    size_t forwarded_param = 0;
    const expr_t *forwarded_source = NULL;
    if (ny_effect_returned_container_forward_param(
            ctx, function, 0, &forwarded_param, &forwarded_source)) {
      expr_t *actual =
          ny_effect_hof_actual(sig, &value->as.call.args, forwarded_param, 0);
      const char *container_type = ny_effect_value_container_type(ctx, actual);
      if (forwarded_source && container_type &&
          (strcmp(container_type, "dict") == 0 ||
           strcmp(container_type, "list") == 0 ||
           strcmp(container_type, "tuple") == 0))
        return (expr_t *)forwarded_source;
      return NULL;
    }
  }
  stmt_t *branch_body = body;
  const char *branch_alias_name = NULL;
  uint32_t branch_alias_ctx = 0;
  const char *branch_alias_origin = NULL;
  uint32_t branch_alias_origin_ctx = 0;
  if (body->kind == NY_S_BLOCK && body->as.block.body.len >= 2 &&
      body->as.block.body.len <= 17) {
    const char *alias_name = NULL;
    uint32_t alias_ctx = 0;
    const char *origin_name = NULL;
    uint32_t origin_ctx = 0;
    bool valid_alias_chain = true;
    for (size_t j = 0; j + 1 < body->as.block.body.len; ++j) {
      const stmt_t *decl = body->as.block.body.data[j];
      const expr_t *source =
          decl && decl->kind == NY_S_VAR && decl->as.var.exprs.len == 1
              ? decl->as.var.exprs.data[0]
              : NULL;
      if (!decl || decl->kind != NY_S_VAR || !decl->as.var.is_decl ||
          decl->as.var.is_mut || decl->as.var.is_del ||
          decl->as.var.names.len != 1 || !decl->as.var.names.data[0] ||
          !source || source->kind != NY_E_IDENT || !source->as.ident.name) {
        valid_alias_chain = false;
        break;
      }
      if (!alias_name) {
        for (size_t i = 0; i < function->as.fn.params.len; ++i) {
          const param_t *param = &function->as.fn.params.data[i];
          if (param->name && strcmp(param->name, source->as.ident.name) == 0 &&
              param->syntax_ctx == source->as.ident.syntax_ctx) {
            origin_name = param->name;
            origin_ctx = param->syntax_ctx;
            break;
          }
        }
        if (!origin_name) {
          valid_alias_chain = false;
          break;
        }
      } else if (strcmp(alias_name, source->as.ident.name) != 0 ||
                 alias_ctx != source->as.ident.syntax_ctx) {
        valid_alias_chain = false;
        break;
      }
      alias_name = decl->as.var.names.data[0];
      alias_ctx = decl->syntax_ctx;
    }
    if (valid_alias_chain && alias_name && origin_name) {
      branch_body = body->as.block.body.data[body->as.block.body.len - 1];
      branch_alias_name = alias_name;
      branch_alias_ctx = alias_ctx;
      branch_alias_origin = origin_name;
      branch_alias_origin_ctx = origin_ctx;
    }
  }
  const expr_t *branch_return = NULL;
  if (sig && ny_effect_returned_container_branch_return(branch_body,
                                                        &branch_return, 0)) {
    for (size_t i = 0; i < function->as.fn.params.len; ++i) {
      const param_t *param = &function->as.fn.params.data[i];
      const char *origin = branch_return->as.ident.name;
      uint32_t origin_ctx = branch_return->as.ident.syntax_ctx;
      if (branch_alias_name && origin &&
          strcmp(branch_alias_name, origin) == 0 &&
          branch_alias_ctx == origin_ctx) {
        origin = branch_alias_origin;
        origin_ctx = branch_alias_origin_ctx;
      }
      if (!param->name || !origin || strcmp(param->name, origin) != 0 ||
          param->syntax_ctx != origin_ctx)
        continue;
      expr_t *actual = ny_effect_hof_actual(sig, &value->as.call.args, i, 0);
      const char *container_type = ny_effect_value_container_type(ctx, actual);
      if (container_type && (strcmp(container_type, "dict") == 0 ||
                             strcmp(container_type, "list") == 0 ||
                             strcmp(container_type, "tuple") == 0))
        return (expr_t *)branch_return;
      return NULL;
    }
    return NULL;
  }
  stmt_t *ret = body;
  expr_t *local_literal = NULL;
  if (body->kind == NY_S_BLOCK) {
    size_t body_len = body->as.block.body.len;
    if (body_len == 0 || body_len > 33)
      return NULL;
    ret = body->as.block.body.data[body_len - 1];
    const char *local_names[32] = {0};
    bool carries_container[32] = {0};
    size_t local_len = 0;
    for (size_t i = 0; i + 1 < body_len; ++i) {
      stmt_t *decl = body->as.block.body.data[i];
      if (decl && decl->kind == NY_S_EXPR && decl->as.expr.expr &&
          decl->as.expr.expr->kind == NY_E_MEMCALL &&
          decl->as.expr.expr->as.memcall.name &&
          strcmp(decl->as.expr.expr->as.memcall.name, "set") == 0 &&
          decl->as.expr.expr->as.memcall.target &&
          decl->as.expr.expr->as.memcall.target->kind == NY_E_IDENT &&
          decl->as.expr.expr->as.memcall.target->as.ident.name &&
          local_literal && local_literal->kind == NY_E_DICT) {
        bool known_container = false;
        for (size_t j = 0; j < local_len; ++j)
          if (carries_container[j] &&
              strcmp(decl->as.expr.expr->as.memcall.target->as.ident.name,
                     local_names[j]) == 0)
            known_container = true;
        if (known_container)
          continue;
      }
      if (decl && decl->kind == NY_S_EXPR && decl->as.expr.expr &&
          decl->as.expr.expr->kind == NY_E_BINARY &&
          decl->as.expr.expr->as.binary.op &&
          strcmp(decl->as.expr.expr->as.binary.op, "=") == 0 &&
          decl->as.expr.expr->as.binary.left &&
          decl->as.expr.expr->as.binary.left->kind == NY_E_INDEX &&
          decl->as.expr.expr->as.binary.left->as.index.target &&
          decl->as.expr.expr->as.binary.left->as.index.target->kind ==
              NY_E_IDENT &&
          decl->as.expr.expr->as.binary.left->as.index.target->as.ident.name &&
          local_literal && local_literal->kind == NY_E_LIST) {
        bool known_container = false;
        const char *target =
            decl->as.expr.expr->as.binary.left->as.index.target->as.ident.name;
        for (size_t j = 0; j < local_len; ++j)
          if (carries_container[j] && strcmp(target, local_names[j]) == 0)
            known_container = true;
        if (known_container)
          continue;
      }
      if (decl && decl->kind == NY_S_EXPR && decl->as.expr.expr &&
          decl->as.expr.expr->kind == NY_E_CALL &&
          decl->as.expr.expr->as.call.args.len == 3 &&
          decl->as.expr.expr->as.call.args.data[0].val &&
          decl->as.expr.expr->as.call.args.data[0].val->kind == NY_E_IDENT &&
          decl->as.expr.expr->as.call.args.data[0].val->as.ident.name &&
          local_literal && local_literal->kind == NY_E_LIST) {
        const expr_t *callee = decl->as.expr.expr->as.call.callee;
        const bool set_idx =
            (decl->as.expr.expr->semantic.canonical_callee &&
             strcmp(decl->as.expr.expr->semantic.canonical_callee,
                    "core.set_idx") == 0) ||
            (callee && callee->kind == NY_E_IDENT && callee->as.ident.name &&
             strcmp(callee->as.ident.name, "set_idx") == 0);
        bool known_container = false;
        const char *target =
            decl->as.expr.expr->as.call.args.data[0].val->as.ident.name;
        for (size_t j = 0; j < local_len; ++j)
          if (carries_container[j] && strcmp(target, local_names[j]) == 0)
            known_container = true;
        if (set_idx && known_container)
          continue;
      }
      if (decl && (decl->kind == NY_S_IF || decl->kind == NY_S_MATCH) &&
          local_literal &&
          (local_literal->kind == NY_E_DICT ||
           local_literal->kind == NY_E_LIST)) {
        bool supported = ny_effect_returned_container_store_flow_supported(
            decl, local_names, carries_container, local_len,
            local_literal->kind == NY_E_LIST, 0);
        if (ny_effect_diag_verbose_enabled())
          ny_effect_diag_log("returned.container.branch",
                             "stmt_kind=%d supported=%d", (int)decl->kind,
                             supported);
        if (supported)
          continue;
      }
      if (!decl || decl->kind != NY_S_VAR || !decl->as.var.is_decl ||
          decl->as.var.is_del || decl->as.var.names.len != 1 ||
          decl->as.var.exprs.len != 1 || !decl->as.var.names.data[0])
        return NULL;
      expr_t *initializer = decl->as.var.exprs.data[0];
      bool carries = false;
      if (initializer &&
          (initializer->kind == NY_E_DICT || initializer->kind == NY_E_LIST ||
           initializer->kind == NY_E_TUPLE)) {
        if (local_literal)
          return NULL;
        local_literal = initializer;
        carries = true;
      } else if (initializer && initializer->kind == NY_E_IDENT &&
                 initializer->as.ident.name) {
        bool known_source = false;
        for (size_t j = 0; j < local_len; ++j) {
          if (strcmp(initializer->as.ident.name, local_names[j]) == 0) {
            carries = carries_container[j];
            known_source = true;
            break;
          }
        }
        for (size_t j = 0; !known_source && j < function->as.fn.params.len;
             ++j) {
          const param_t *param = &function->as.fn.params.data[j];
          if (param->name &&
              strcmp(initializer->as.ident.name, param->name) == 0) {
            known_source = true;
            expr_t *actual =
                sig ? ny_effect_hof_actual(sig, &value->as.call.args, j, 0)
                    : NULL;
            const char *container_type =
                ny_effect_value_container_type(ctx, actual);
            carries = container_type && (strcmp(container_type, "dict") == 0 ||
                                         strcmp(container_type, "list") == 0 ||
                                         strcmp(container_type, "tuple") == 0);
          }
        }
        if (!known_source)
          return NULL;
      } else {
        return NULL;
      }
      if (decl->as.var.is_mut && !carries)
        return NULL;
      local_names[local_len] = decl->as.var.names.data[0];
      carries_container[local_len] = carries;
      local_len++;
    }
    if (body_len > 1) {
      if (!ret || ret->kind != NY_S_RETURN || !ret->as.ret.value ||
          ret->as.ret.value->kind != NY_E_IDENT ||
          !ret->as.ret.value->as.ident.name)
        return NULL;
      bool returned_container = false;
      for (size_t i = 0; i < local_len; ++i)
        if (strcmp(ret->as.ret.value->as.ident.name, local_names[i]) == 0)
          returned_container = carries_container[i];
      if (!returned_container)
        return NULL;
      return ret->as.ret.value;
    }
  }
  if (!ret || ret->kind != NY_S_RETURN || !ret->as.ret.value)
    return NULL;
  expr_t *returned = local_literal ? local_literal : ret->as.ret.value;
  if (returned->kind == NY_E_IDENT && returned->as.ident.name && sig) {
    for (size_t i = 0; i < function->as.fn.params.len; ++i) {
      const param_t *param = &function->as.fn.params.data[i];
      if (!param->name || strcmp(param->name, returned->as.ident.name) != 0)
        continue;
      expr_t *actual = ny_effect_hof_actual(sig, &value->as.call.args, i, 0);
      const char *container_type = ny_effect_value_container_type(ctx, actual);
      if (container_type && (strcmp(container_type, "dict") == 0 ||
                             strcmp(container_type, "list") == 0 ||
                             strcmp(container_type, "tuple") == 0))
        return returned;
      return NULL;
    }
  }
  if (returned->kind != NY_E_DICT && returned->kind != NY_E_LIST &&
      returned->kind != NY_E_TUPLE)
    return NULL;
  return returned;
}

/*
 * Recognize bounded branches returning complete callback containers with the
 * same shape. The branch effect facts can then be joined by key/index;
 * mismatched or incomplete alternatives stay unknown.
 */
static bool ny_effect_direct_branches_returned_containers(
    const ny_effects_ctx *ctx, const expr_t *value, expr_t **containers,
    size_t capacity, size_t *container_count) {
  if (container_count)
    *container_count = 0;
  if (containers)
    for (size_t i = 0; i < capacity; ++i)
      containers[i] = NULL;
  if (!ctx || !value || value->kind != NY_E_CALL || !containers ||
      capacity < 2 || !container_count)
    return false;
  stmt_t *function = value->semantic.canonical_callee_stmt;
  if ((!function || function->kind != NY_S_FUNC) && ctx->cg) {
    fun_sig *sig = ny_purity_resolve_call_sig(
        ctx->cg, (expr_call_t *)&value->as.call, ctx->local_names,
        ctx->local_hashes,
        ctx->local_bloom ? ctx->local_bloom : (uint64_t[4]){0});
    if (sig && sig->stmt_t && sig->stmt_t->kind == NY_S_FUNC)
      function = sig->stmt_t;
  }
  if (!function || function->kind != NY_S_FUNC || !function->as.fn.body)
    return false;
  stmt_t *body = function->as.fn.body;
  stmt_t *ret = body;
  expr_t *branches[33] = {NULL};
  size_t branch_count = 0;
  if (ret->kind == NY_S_BLOCK) {
    if (ret->as.block.body.len == 1) {
      ret = ret->as.block.body.data[0];
    } else if (ret->as.block.body.len >= 2 && ret->as.block.body.len <= 34) {
      size_t branch_index = ret->as.block.body.len - 2;
      const char *local_names[32] = {0};
      size_t local_len = 0;
      for (size_t i = 0; i < branch_index; ++i) {
        stmt_t *decl = ret->as.block.body.data[i];
        if (!decl || decl->kind != NY_S_VAR || !decl->as.var.is_decl ||
            decl->as.var.is_mut || decl->as.var.names.len != 1 ||
            decl->as.var.exprs.len != 1 || !decl->as.var.names.data[0])
          return false;
        expr_t *initializer = decl->as.var.exprs.data[0];
        if (initializer && initializer->kind == NY_E_IDENT &&
            initializer->as.ident.name) {
          bool known_source = false;
          for (size_t j = 0; j < local_len; ++j)
            if (strcmp(initializer->as.ident.name, local_names[j]) == 0) {
              known_source = true;
              break;
            }
          for (size_t j = 0; !known_source && j < function->as.fn.params.len;
               ++j) {
            const param_t *param = &function->as.fn.params.data[j];
            if (param->name &&
                strcmp(initializer->as.ident.name, param->name) == 0)
              known_source = true;
          }
          if (!known_source)
            return false;
        } else if (!initializer || (initializer->kind != NY_E_DICT &&
                                    initializer->kind != NY_E_LIST &&
                                    initializer->kind != NY_E_TUPLE)) {
          return false;
        }
        local_names[local_len++] = decl->as.var.names.data[0];
      }
      stmt_t *branch = ret->as.block.body.data[branch_index];
      stmt_t *fallback = ret->as.block.body.data[branch_index + 1];
      if (!branch || branch->kind != NY_S_IF || branch->as.iff.init ||
          branch->as.iff.alt || !fallback || fallback->kind != NY_S_RETURN ||
          !fallback->as.ret.value)
        return false;
      stmt_t *consequence = branch->as.iff.conseq;
      if (consequence && consequence->kind == NY_S_BLOCK) {
        if (consequence->as.block.body.len != 1)
          return false;
        consequence = consequence->as.block.body.data[0];
      }
      if (!consequence || consequence->kind != NY_S_RETURN ||
          !consequence->as.ret.value)
        return false;
      branches[0] = consequence->as.ret.value;
      branches[1] = fallback->as.ret.value;
      branch_count = 2;
      ret = NULL;
    } else {
      return false;
    }
  }
  if (ret && ret->kind == NY_S_MATCH) {
    stmt_t *match = ret;
    if (match->as.match.arms.len == 0 || match->as.match.arms.len > 32) {
      return false;
    }
    bool exhaustive = match->as.match.default_conseq != NULL;
    for (size_t i = 0; i < match->as.match.arms.len; ++i) {
      const match_arm_t *arm = &match->as.match.arms.data[i];
      if (!arm->guard)
        for (size_t p = 0; p < arm->patterns.len; ++p)
          exhaustive |= ny_expr_is_wildcard_ident(arm->patterns.data[p]);
      stmt_t *arm_return = arm->conseq;
      if (arm_return && arm_return->kind == NY_S_BLOCK) {
        if (arm_return->as.block.body.len != 1) {
          return false;
        }
        arm_return = arm_return->as.block.body.data[0];
      }
      if (!arm_return || arm_return->kind != NY_S_RETURN ||
          !arm_return->as.ret.value) {
        return false;
      }
      branches[branch_count++] = arm_return->as.ret.value;
    }
    if (match->as.match.default_conseq) {
      stmt_t *default_return = match->as.match.default_conseq;
      if (default_return->kind == NY_S_BLOCK) {
        if (default_return->as.block.body.len != 1) {
          return false;
        }
        default_return = default_return->as.block.body.data[0];
      }
      if (!default_return || default_return->kind != NY_S_RETURN ||
          !default_return->as.ret.value) {
        return false;
      }
      branches[branch_count++] = default_return->as.ret.value;
    }
    if (!exhaustive) {
      return false;
    }
    ret = NULL;
  }
  if (ret) {
    if (ret->kind != NY_S_RETURN || !ret->as.ret.value ||
        ret->as.ret.value->kind != NY_E_TERNARY)
      return false;
    branches[0] = ret->as.ret.value->as.ternary.true_expr;
    branches[1] = ret->as.ret.value->as.ternary.false_expr;
    branch_count = 2;
  }
  if (branch_count < 2 || branch_count > capacity || branch_count > 33 ||
      !branches[0] ||
      (branches[0]->kind != NY_E_DICT && branches[0]->kind != NY_E_LIST &&
       branches[0]->kind != NY_E_TUPLE))
    return false;
  expr_t *first = branches[0];
  size_t first_len = first->kind == NY_E_DICT ? first->as.dict.pairs.len
                                              : first->as.list_like.len;
  if (first_len == 0)
    return false;
  for (size_t branch = 1; branch < branch_count; ++branch) {
    expr_t *other = branches[branch];
    if (!other || other->kind != first->kind)
      return false;
    size_t other_len = other->kind == NY_E_DICT ? other->as.dict.pairs.len
                                                : other->as.list_like.len;
    if (first_len != other_len)
      return false;
    if (first->kind == NY_E_DICT) {
      for (size_t i = 0; i < first_len; ++i) {
        const dict_pair_t *fp = &first->as.dict.pairs.data[i];
        const dict_pair_t *op = &other->as.dict.pairs.data[i];
        if (!fp->key || !op->key || fp->key->kind != NY_E_LITERAL ||
            op->key->kind != NY_E_LITERAL ||
            fp->key->as.literal.kind != NY_LIT_STR ||
            op->key->as.literal.kind != NY_LIT_STR ||
            !fp->key->as.literal.as.s.data || !op->key->as.literal.as.s.data ||
            strcmp(fp->key->as.literal.as.s.data,
                   op->key->as.literal.as.s.data) != 0)
          return false;
      }
    }
  }
  for (size_t i = 0; i < branch_count; ++i)
    containers[i] = branches[i];
  *container_count = branch_count;
  return true;
}

static void ny_effect_record_returned_container_callback_facts_depth(
    ny_effects_ctx *ctx, const char *destination, const expr_t *call,
    const expr_t *container, unsigned depth);

static void ny_effect_record_returned_container_callback_facts(
    ny_effects_ctx *ctx, const char *destination, const expr_t *call,
    const expr_t *container) {
  ny_effect_record_returned_container_callback_facts_depth(ctx, destination,
                                                           call, container, 0);
}

static void ny_effect_record_returned_container_callback_facts_depth(
    ny_effects_ctx *ctx, const char *destination, const expr_t *call,
    const expr_t *container, unsigned depth) {
  if (!ctx || !destination || !call || call->kind != NY_E_CALL || !container)
    return;
  if (depth > 16)
    return;
  stmt_t *function = call->semantic.canonical_callee_stmt;
  fun_sig *sig = NULL;
  if (ctx->cg) {
    sig = ny_purity_resolve_call_sig(ctx->cg, (expr_call_t *)&call->as.call,
                                     ctx->local_names, ctx->local_hashes,
                                     ctx->local_bloom ? ctx->local_bloom
                                                      : (uint64_t[4]){0});
    if ((!function || function->kind != NY_S_FUNC) && sig && sig->stmt_t &&
        sig->stmt_t->kind == NY_S_FUNC)
      function = sig->stmt_t;
  }
  if (!function || function->kind != NY_S_FUNC || !sig)
    return;

  assigned_name_list names = {0};
  assigned_hash_list hashes = {0};
  uint64_t bloom[4] = {0};
  ny_effects_ctx isolated = {
      .cg = ctx->cg,
      .fact_arena = ctx->fact_arena,
      .owner_fn = &function->as.fn,
      .owner_effect_row = ny_effect_sig_row(sig),
      .owner_effect_contract_known = function->as.fn.effect_contract_known,
      .owner_effect_contract_override = true,
      .local_names = &names,
      .local_hashes = &hashes,
      .local_bloom = bloom,
  };
  for (size_t i = 0; i < function->as.fn.params.len; ++i) {
    const param_t *param = &function->as.fn.params.data[i];
    if (!param->name)
      continue;
    assigned_name_add(&names, &hashes, bloom, param->name);
    expr_t *actual = ny_effect_hof_actual(sig, &call->as.call.args, i, 0);
    if (!ny_effect_param_is_callable(param->type)) {
      const char *container_type = ny_effect_value_container_type(ctx, actual);
      if (container_type && (strcmp(container_type, "dict") == 0 ||
                             strcmp(container_type, "list") == 0 ||
                             strcmp(container_type, "tuple") == 0)) {
        if (actual && actual->kind == NY_E_CALL) {
          expr_t *returned_container =
              ny_effect_direct_returned_container(ctx, actual);
          if (returned_container) {
            ny_effect_record_returned_container_callback_facts_depth(
                &isolated, param->name, actual, returned_container, depth + 1);
            continue;
          }
        }
        ny_effect_record_local_container(&isolated, param->name, container_type,
                                         NULL, NULL, NY_FX_ALL, false, false,
                                         false, 0, false, false, 0, false);
        if (actual && actual->kind == NY_E_IDENT && actual->as.ident.name)
          ny_effect_copy_container_callback_facts_from(
              &isolated, ctx, actual->as.ident.name, param->name);
        else if (actual && actual->kind == NY_E_DICT)
          ny_effect_record_dict_callback_facts(&isolated, param->name, actual);
        else if (actual &&
                 (actual->kind == NY_E_LIST || actual->kind == NY_E_TUPLE))
          ny_effect_record_list_callback_facts(&isolated, param->name, actual);
      }
      continue;
    }
    uint32_t effects = NY_FX_ALL;
    bool effects_known =
        actual && ny_effect_callback_expr_summary(ctx, actual, &effects);
    ny_effect_record_local_container(&isolated, param->name, NULL, NULL,
                                     param->type, effects, effects_known, true,
                                     false, 0, false, false, 0, false);
    const expr_t *callable_expr =
        actual && (actual->kind == NY_E_LAMBDA || actual->kind == NY_E_FN)
            ? actual
        : actual && actual->kind == NY_E_IDENT && actual->as.ident.name
            ? ny_effect_local_callable_expr(ctx, actual->as.ident.name)
            : NULL;
    ny_effect_record_local_callable_expr(&isolated, param->name, callable_expr);
    bool row_known = false;
    const ny_type_t *row = ny_effect_callback_row(ctx, actual, &row_known);
    ny_effect_record_local_call_row(&isolated, param->name, row, row_known);
    if (actual && actual->kind == NY_E_IDENT && actual->as.ident.name &&
        ny_effect_local_custom_row_unknown(ctx, actual->as.ident.name)) {
      for (size_t fi = isolated.local_container_len; fi > 0; --fi) {
        ny_effect_local_fact_t *fact = &isolated.local_containers[fi - 1];
        if (fact->name && strcmp(fact->name, param->name) == 0) {
          fact->call_effect_custom_row_unknown = true;
          break;
        }
      }
    }
  }

  stmt_t *body = function->as.fn.body;
  if (body && body->kind == NY_S_BLOCK) {
    ny_visitor_t prefix_visitor = {.ctx = &isolated,
                                   .visit_expr_pre = effects_visit_expr,
                                   .visit_expr_post = effects_visit_expr_post,
                                   .visit_stmt_pre = effects_visit_stmt,
                                   .visit_stmt_post = effects_visit_stmt_post};
    size_t prefix_len =
        body->as.block.body.len ? body->as.block.body.len - 1 : 0;
    for (size_t i = 0; i < prefix_len; ++i) {
      stmt_t *stmt = body->as.block.body.data[i];
      if (stmt && stmt->kind == NY_S_VAR && stmt->as.var.exprs.len == 1 &&
          stmt->as.var.exprs.data[0] == container)
        break;
      ny_visit_stmt(&prefix_visitor, stmt);
    }
  }

  size_t fact_start = isolated.local_container_len;
  if (container->kind == NY_E_IDENT && container->as.ident.name)
    ny_effect_copy_container_callback_facts(&isolated, container->as.ident.name,
                                            destination);
  else if (container->kind == NY_E_DICT)
    ny_effect_record_dict_callback_facts(&isolated, destination, container);
  else
    ny_effect_record_list_callback_facts(&isolated, destination, container);
  const char *container_type =
      container->kind == NY_E_IDENT && container->as.ident.name
          ? ny_effect_local_container_type(&isolated, container->as.ident.name)
          : ny_effect_container_literal_type(container);
  if (container_type) {
    /*
     * The returned call is initially recorded without a literal kind; publish
     * the branch-joined tuple/list/dict kind so indexed reads can find slots.
     */
    for (size_t i = ctx->local_container_len; i > 0; --i) {
      ny_effect_local_fact_t *fact = &ctx->local_containers[i - 1];
      if (fact->name && strcmp(fact->name, destination) == 0) {
        fact->container_type = container_type;
        break;
      }
    }
  }
  for (size_t i = fact_start; i < isolated.local_container_len; ++i) {
    const ny_effect_local_fact_t *fact = &isolated.local_containers[i];
    if (!fact->name)
      continue;
    bool callback_fact =
        strncmp(fact->name,
                "\001dict-callback:", sizeof("\001dict-callback:") - 1) == 0 ||
        strncmp(fact->name, "\001dict-callback-summary:",
                sizeof("\001dict-callback-summary:") - 1) == 0 ||
        strncmp(fact->name,
                "\001list-callback:", sizeof("\001list-callback:") - 1) == 0 ||
        strncmp(fact->name, "\001list-callback-summary:",
                sizeof("\001list-callback-summary:") - 1) == 0;
    if (!callback_fact)
      continue;
    ny_effect_record_local_container(
        ctx, fact->name, NULL, NULL, NULL, fact->call_effects,
        fact->call_effects_known, fact->is_callable, false, 0, false, false, 0,
        false);
    ny_effect_record_local_call_row(ctx, fact->name, fact->call_effect_row,
                                    fact->call_effect_row_known);
    if (fact->call_effect_custom_row_unknown) {
      for (size_t oi = ctx->local_container_len; oi > 0; --oi) {
        ny_effect_local_fact_t *outer = &ctx->local_containers[oi - 1];
        if (outer->name && strcmp(outer->name, fact->name) == 0) {
          outer->call_effect_custom_row_unknown = true;
          break;
        }
      }
    }
  }
  free(isolated.local_containers);
  free(names.data);
  free(hashes.data);
}

static void ny_effect_record_branch_returned_container_callback_facts(
    ny_effects_ctx *ctx, const char *destination, const expr_t *call,
    const expr_t *const *containers, size_t container_count) {
  if (!ctx || !destination || !call || call->kind != NY_E_CALL || !containers ||
      container_count < 2 || container_count > 33)
    return;
  size_t base = ctx->local_container_len;
  const ny_effect_local_fact_t *sets[33] = {NULL};
  size_t lengths[33] = {0};
  bool complete = true;
  for (size_t i = 0; i < container_count; ++i) {
    ny_effect_record_returned_container_callback_facts(ctx, destination, call,
                                                       containers[i]);
    lengths[i] = ctx->local_container_len - base;
    if (lengths[i]) {
      ny_effect_local_fact_t *copy = malloc(lengths[i] * sizeof(*copy));
      if (!copy) {
        complete = false;
        break;
      }
      memcpy(copy, ctx->local_containers + base, lengths[i] * sizeof(*copy));
      sets[i] = copy;
    } else {
      complete = false;
    }
    ctx->local_container_len = base;
  }
  bool merged = complete && ny_effect_fact_merge_branches(ctx, sets, lengths,
                                                          container_count);
  if (!merged)
    ctx->local_container_len = base;
  for (size_t i = 0; i < container_count; ++i)
    free((void *)sets[i]);
}

static bool effects_visit_stmt(ny_visitor_t *v, stmt_t *s) {
  ny_effects_ctx *ctx = (ny_effects_ctx *)v->ctx;
  if (!s)
    return false;

  switch (s->kind) {
  case NY_S_IF: {
    /*
     * Header bindings and callback aliases are
     * visible to the condition and both
     * branches, just as they are during
     * ordinary statement execution.
     */
    ny_visit_stmt(v, s->as.iff.init);
    if (s->as.iff.test && s->as.iff.test->kind == NY_E_COMPTIME) {
      ny_ct_fast_val_t value = ny_ct_fast_none();
      bool truthy = false;
      if (ny_try_eval_comptime_expr_fast(ctx->cg, s->as.iff.test, &value, 0) &&
          ny_ct_fast_truthy(&value, &truthy)) {
        ny_visit_stmt(v, truthy ? s->as.iff.conseq : s->as.iff.alt);
        return false;
      }
    }
    ny_visit_expr(v, s->as.iff.test);
    size_t base_len = 0, then_len = 0, else_len = 0;
    ny_effect_local_fact_t *base_facts =
        ny_effect_fact_snapshot(ctx, &base_len);
    if (base_len && !base_facts) {
      ctx->callback_alias_state_ambiguous = true;
      ctx->callback_control_flow_depth++;
      ny_visit_stmt(v, s->as.iff.conseq);
      ny_visit_stmt(v, s->as.iff.alt);
      ctx->callback_control_flow_depth--;
      return false;
    }
    bool before_ambiguous = ctx->callback_alias_state_ambiguous;
    ny_visit_stmt(v, s->as.iff.conseq);
    ny_effect_local_fact_t *then_facts =
        ny_effect_fact_snapshot(ctx, &then_len);
    bool then_ambiguous = ctx->callback_alias_state_ambiguous;
    bool restored = (!then_len || then_facts) &&
                    ny_effect_fact_restore(ctx, base_facts, base_len);
    ctx->callback_alias_state_ambiguous = before_ambiguous;
    if (restored) {
      ny_visit_stmt(v, s->as.iff.alt);
      ny_effect_local_fact_t *else_facts =
          ny_effect_fact_snapshot(ctx, &else_len);
      bool else_ambiguous = ctx->callback_alias_state_ambiguous;
      restored = (!else_len || else_facts) &&
                 ny_effect_fact_restore(ctx, base_facts, base_len);
      const ny_effect_local_fact_t *sets[] = {then_facts, else_facts};
      size_t lens[] = {then_len, else_len};
      if (restored && ny_effect_fact_merge_branches(ctx, sets, lens, 2))
        ctx->callback_alias_state_ambiguous = then_ambiguous || else_ambiguous;
      else
        ctx->callback_alias_state_ambiguous = true;
      free(else_facts);
    } else {
      ctx->callback_alias_state_ambiguous = true;
      ny_visit_stmt(v, s->as.iff.alt);
    }
    free(base_facts);
    free(then_facts);
    return false;
  }
  case NY_S_BLOCK: {
    ny_purity_scope_checkpoint_t cp = {0};
    size_t container_checkpoint = ctx->local_container_len;
    ny_purity_scope_save(ctx->local_names, ctx->local_hashes,
                         ctx->local_bloom ? ctx->local_bloom : (uint64_t[4]){0},
                         &cp);
    for (size_t i = 0; i < s->as.block.body.len; i++) {
      ny_visit_stmt(v, s->as.block.body.data[i]);
    }
    ny_effect_local_fact_t *outer_updates =
        container_checkpoint
            ? malloc(container_checkpoint * sizeof(*outer_updates))
            : NULL;
    size_t outer_update_len = 0;
    bool update_snapshot_ok = !container_checkpoint || outer_updates;
    for (size_t i = 0; update_snapshot_ok && i < container_checkpoint; ++i) {
      const char *name = ctx->local_containers[i].name;
      if (!name)
        continue;
      bool seen = false;
      for (size_t j = 0; j < i; ++j)
        if (ctx->local_containers[j].name &&
            strcmp(ctx->local_containers[j].name, name) == 0) {
          seen = true;
          break;
        }
      if (seen)
        continue;
      const ny_effect_local_fact_t *latest = ny_effect_fact_latest(
          ctx->local_containers, ctx->local_container_len, name);
      if (latest && latest >= ctx->local_containers + container_checkpoint)
        outer_updates[outer_update_len++] = *latest;
    }
    ny_purity_scope_restore(
        ctx->local_names, ctx->local_hashes,
        ctx->local_bloom ? ctx->local_bloom : (uint64_t[4]){0}, &cp);
    ctx->local_container_len = container_checkpoint;
    if (update_snapshot_ok) {
      for (size_t i = 0; i < outer_update_len; ++i) {
        ny_effect_local_fact_t *fact = &outer_updates[i];
        size_t fact_count = ctx->local_container_len;
        ny_effect_record_local_container(
            ctx, fact->name, fact->container_type, fact->element_container_type,
            fact->value_type, fact->call_effects, fact->call_effects_known,
            fact->is_callable, fact->is_query, fact->query_effects,
            fact->query_effects_known, fact->is_query_step,
            fact->query_step_effects, fact->query_step_effects_known);
        if (ctx->local_container_len == fact_count) {
          ctx->callback_alias_state_ambiguous = true;
          continue;
        }
        ny_effect_record_local_call_row(ctx, fact->name, fact->call_effect_row,
                                        fact->call_effect_row_known);
        ny_effect_local_fact_t *restored =
            &ctx->local_containers[ctx->local_container_len - 1];
        restored->call_effect_custom_row_unknown =
            fact->call_effect_custom_row_unknown;
        restored->callable_expr = fact->callable_expr;
      }
    } else {
      ctx->callback_alias_state_ambiguous = true;
    }
    free(outer_updates);
    return false;
  }
  case NY_S_VAR: {
    for (size_t i = 0; i < s->as.var.exprs.len; i++) {
      expr_t *rhs = s->as.var.exprs.data[i];
      const char *append_source = NULL;
      if (ny_effect_builtin_list_append(ctx, rhs, &append_source, NULL)) {
        const expr_t *saved_append = ctx->callback_nonmutating_list_append;
        ctx->callback_nonmutating_list_append = rhs;
        ny_visit_expr(v, rhs);
        ctx->callback_nonmutating_list_append = saved_append;
      } else {
        ny_visit_expr(v, rhs);
      }
    }
    if (s->as.var.is_decl) {
      for (size_t i = 0; i < s->as.var.names.len; i++) {
        assigned_name_add(ctx->local_names, ctx->local_hashes,
                          ctx->local_bloom ? ctx->local_bloom
                                           : (uint64_t[4]){0},
                          s->as.var.names.data[i]);
        expr_t *init = i < s->as.var.exprs.len ? s->as.var.exprs.data[i] : NULL;
        if (!init && s->as.var.exprs.len == 1)
          init = s->as.var.exprs.data[0];
        const char *append_source = NULL;
        expr_t *append_item = NULL;
        bool appended_list = ny_effect_builtin_list_append(
            ctx, init, &append_source, &append_item);
        const char *container =
            appended_list ? "list" : ny_effect_value_container_type(ctx, init);
        const char *element_container =
            appended_list
                ? ny_effect_local_element_container_type(ctx, append_source)
                : ny_effect_literal_element_container_type(ctx, init);
        const char *value_type =
            i < s->as.var.types.len && s->as.var.types.data[i]
                ? s->as.var.types.data[i]
                : ny_effect_expr_value_type(ctx, init);
        uint32_t call_effects = NY_FX_ALL;
        bool call_effects_known = false;
        bool is_callable = ny_effect_callback_value_summary(
            ctx, init, &call_effects, &call_effects_known);
        if (!is_callable && init && init->kind == NY_E_IDENT &&
            init->as.ident.name &&
            ny_effect_owner_row_callback(ctx, init->as.ident.name)) {
          /*
           * Preserve an open callback row
           * through a straight-line local
           * alias. The type-level row variable
           * is unified at each caller.
           */
          is_callable = true;
          call_effects_known = true;
          call_effects = NY_FX_NONE;
        }
        if (ctx->callback_try_body_depth && is_callable)
          ctx->callback_alias_state_ambiguous = true;
        if (ny_effect_diag_verbose_enabled() &&
            ny_dbg_function_selected(ctx->owner_fn && ctx->owner_fn->name
                                         ? ctx->owner_fn->name
                                         : NULL))
          ny_effect_diag_log("lambda",
                             "name=%s expr_kind=%d "
                             "callable=%d known=%d mask=0x%x",
                             s->as.var.names.data[i],
                             init ? (int)init->kind : -1, is_callable,
                             call_effects_known, call_effects);
        uint32_t query_effects = 0;
        bool query_effects_known = false;
        bool is_query = ny_effect_query_value_summary(ctx, init, &query_effects,
                                                      &query_effects_known);
        uint32_t query_step_effects = 0;
        bool query_step_effects_known = false;
        bool is_query_step = ny_effect_query_step_summary(
            ctx, init, &query_step_effects, &query_step_effects_known);
        ny_effect_record_local_container(
            ctx, s->as.var.names.data[i], container, element_container,
            value_type, call_effects, call_effects_known, is_callable, is_query,
            query_effects, query_effects_known, is_query_step,
            query_step_effects, query_step_effects_known);
        ny_effect_record_callback_value_row(ctx, s->as.var.names.data[i], init,
                                            is_callable);
        const expr_t *callable_expr =
            init && (init->kind == NY_E_LAMBDA || init->kind == NY_E_FN) ? init
            : init && init->kind == NY_E_IDENT && init->as.ident.name
                ? ny_effect_local_callable_expr(ctx, init->as.ident.name)
                : NULL;
        ny_effect_record_local_callable_expr(
            ctx, s->as.var.names.data[i], is_callable ? callable_expr : NULL);
        expr_t *branch_containers[33] = {NULL};
        size_t branch_container_count = 0;
        bool branch_container =
            !s->as.var.is_mut && init && init->kind == NY_E_CALL &&
            ny_effect_direct_branches_returned_containers(
                ctx, init, branch_containers,
                sizeof(branch_containers) / sizeof(branch_containers[0]),
                &branch_container_count);
        if (ny_effect_diag_verbose_enabled() && init && init->kind == NY_E_CALL)
          ny_effect_diag_log("callback.factory.branches",
                             "name=%s matched=%d count=%zu",
                             s->as.var.names.data[i], branch_container,
                             branch_container_count);
        expr_t *container_init = ny_effect_direct_returned_container(ctx, init);
        if (branch_container) {
          ny_effect_record_branch_returned_container_callback_facts(
              ctx, s->as.var.names.data[i], init,
              (const expr_t *const *)branch_containers, branch_container_count);
        } else if (!s->as.var.is_mut && container_init && init &&
                   init->kind == NY_E_CALL) {
          ny_effect_record_returned_container_callback_facts(
              ctx, s->as.var.names.data[i], init, container_init);
        } else if (!s->as.var.is_mut) {
          expr_t *literal = container_init ? container_init : init;
          ny_effect_record_dict_callback_facts(ctx, s->as.var.names.data[i],
                                               literal);
          ny_effect_record_list_callback_facts(ctx, s->as.var.names.data[i],
                                               literal);
        } else if (init &&
                   (init->kind == NY_E_DICT || init->kind == NY_E_LIST ||
                    init->kind == NY_E_TUPLE)) {
          /*
           * A mutable local starts with these exact entries. Subsequent
           * mutations invalidate the snapshot before installing any narrowly
           * proven replacement facts.
           */
          ny_effect_record_dict_callback_facts(ctx, s->as.var.names.data[i],
                                               init);
          ny_effect_record_list_callback_facts(ctx, s->as.var.names.data[i],
                                               init);
        }
        if (appended_list) {
          ny_effect_copy_container_callback_facts(ctx, append_source,
                                                  s->as.var.names.data[i]);
          if (!ny_effect_merge_appended_list_callback_summary(
                  ctx, append_source, s->as.var.names.data[i], append_item))
            ny_effect_mark_list_callback_summary_unknown(
                ctx, s->as.var.names.data[i]);
        } else if (init && init->kind == NY_E_IDENT && init->as.ident.name)
          ny_effect_copy_container_callback_facts(ctx, init->as.ident.name,
                                                  s->as.var.names.data[i]);
      }
    } else {
      for (size_t i = 0; i < s->as.var.names.len; ++i) {
        expr_t *rhs = i < s->as.var.exprs.len ? s->as.var.exprs.data[i] : NULL;
        if (!rhs && s->as.var.exprs.len == 1)
          rhs = s->as.var.exprs.data[0];
        uint32_t call_effects = NY_FX_ALL;
        bool call_effects_known = false;
        bool is_callable = ny_effect_callback_value_summary(
            ctx, rhs, &call_effects, &call_effects_known);
        const expr_t *callable_expr =
            rhs && (rhs->kind == NY_E_LAMBDA || rhs->kind == NY_E_FN) ? rhs
            : rhs && rhs->kind == NY_E_IDENT && rhs->as.ident.name
                ? ny_effect_local_callable_expr(ctx, rhs->as.ident.name)
                : NULL;
        if (!is_callable && rhs && rhs->kind == NY_E_IDENT &&
            rhs->as.ident.name &&
            ny_effect_owner_row_callback(ctx, rhs->as.ident.name)) {
          is_callable = true;
          call_effects_known = true;
          call_effects = NY_FX_NONE;
        }
        if (ctx->callback_try_body_depth &&
            (is_callable ||
             ny_effect_local_is_callable(ctx, s->as.var.names.data[i])))
          ctx->callback_alias_state_ambiguous = true;
        uint32_t query_effects = 0;
        bool query_effects_known = false;
        bool is_query = ny_effect_query_value_summary(ctx, rhs, &query_effects,
                                                      &query_effects_known);
        uint32_t query_step_effects = 0;
        bool query_step_effects_known = false;
        bool is_query_step = ny_effect_query_step_summary(
            ctx, rhs, &query_step_effects, &query_step_effects_known);
        const char *append_source = NULL;
        expr_t *append_item = NULL;
        bool appended_list = ny_effect_builtin_list_append(
            ctx, rhs, &append_source, &append_item);
        const char *append_element_container =
            appended_list
                ? ny_effect_local_element_container_type(ctx, append_source)
                : NULL;
        ny_effect_record_local_container(
            ctx, s->as.var.names.data[i], appended_list ? "list" : NULL,
            append_element_container, NULL, call_effects, call_effects_known,
            is_callable, is_query, query_effects, query_effects_known,
            is_query_step, query_step_effects, query_step_effects_known);
        ny_effect_record_callback_value_row(ctx, s->as.var.names.data[i], rhs,
                                            is_callable);
        ny_effect_record_local_callable_expr(
            ctx, s->as.var.names.data[i], is_callable ? callable_expr : NULL);
        ny_effect_refresh_local_lambda_summaries(ctx, s->as.var.names.data[i]);
        if (appended_list) {
          ny_effect_copy_container_callback_facts(ctx, append_source,
                                                  s->as.var.names.data[i]);
          if (!ny_effect_merge_appended_list_callback_summary(
                  ctx, append_source, s->as.var.names.data[i], append_item))
            ny_effect_mark_list_callback_summary_unknown(
                ctx, s->as.var.names.data[i]);
        }
      }
    }
    return false;
  }
  case NY_S_FUNC: {
    /*
     * Nested function declarations are local
     * callables.  Record their statically
     * computed body effects so a call such as
     * `inner(x)` does not become an
     * unresolved-indirect-call barrier in its
     * enclosing function's effect summary.
     */
    const char *name = s->as.fn.name;
    const char *local_name = ny_name_leaf(name);
    if (local_name)
      name = local_name;
    if (name && *name) {
      assigned_name_add(ctx->local_names, ctx->local_hashes,
                        ctx->local_bloom ? ctx->local_bloom : (uint64_t[4]){0},
                        name);
      bool unresolved_effect_attr = false;
      const ny_type_t *resolved_nested_row = NULL;
      for (size_t i = 0; i < s->attributes.len; ++i) {
        const attribute_t *attr = &s->attributes.data[i];
        if (attr->name && strcmp(attr->name, "effects") == 0 &&
            !s->as.fn.effect_contract_known && !s->as.fn.effect_type) {
          unresolved_effect_attr = true;
          break;
        }
      }
      bool nested_row_resolved =
          unresolved_effect_attr &&
          ny_effect_nested_declared_row(ctx, s, &resolved_nested_row);
      uint32_t effects = NY_FX_ALL;
      bool known = ny_effect_nested_function_summary(
          ctx, s, nested_row_resolved ? resolved_nested_row : NULL, &effects);
      if (unresolved_effect_attr) {
        if (!nested_row_resolved) {
          known = false;
          effects = NY_FX_ALL;
        }
      }
      ny_effect_record_local_container(ctx, name, NULL, NULL, NULL, effects,
                                       known, true, false, 0, false, false, 0,
                                       false);
      fun_sig *nested_sig = ny_effect_sig_for_stmt(ctx->cg, s);
      const ny_type_t *nested_row =
          ny_effect_sig_row(nested_sig)
              ? ny_effect_sig_row(nested_sig)
              : (resolved_nested_row ? resolved_nested_row
                                     : s->as.fn.effect_type);
      bool nested_row_known =
          (nested_sig && ny_sig_effects_known(nested_sig)) ||
          resolved_nested_row || s->as.fn.effect_contract_known ||
          (!nested_sig && known);
      ny_effect_record_local_call_row(ctx, name, nested_row, nested_row_known);
      if (ny_effect_diag_verbose_enabled())
        ny_effect_diag_log("nested-function",
                           "name=%s summary_known=%d "
                           "effects=0x%x",
                           name, known, effects);
    }
    return false;
  }
  case NY_S_FOR: {
    if (s->as.fr.init)
      ny_visit_stmt(v, s->as.fr.init);
    if (s->as.fr.cond)
      ny_visit_expr(v, s->as.fr.cond);
    if (s->as.fr.iterable)
      ny_visit_expr(v, s->as.fr.iterable);
    ny_purity_scope_checkpoint_t cp = {0};
    ny_purity_scope_save(ctx->local_names, ctx->local_hashes,
                         ctx->local_bloom ? ctx->local_bloom : (uint64_t[4]){0},
                         &cp);
    if (s->as.fr.iter_var)
      assigned_name_add(ctx->local_names, ctx->local_hashes,
                        ctx->local_bloom ? ctx->local_bloom : (uint64_t[4]){0},
                        s->as.fr.iter_var);
    if (s->as.fr.iter_index_var)
      assigned_name_add(ctx->local_names, ctx->local_hashes,
                        ctx->local_bloom ? ctx->local_bloom : (uint64_t[4]){0},
                        s->as.fr.iter_index_var);
    ny_effect_visit_loop_body(v, ctx, s->as.fr.body, s->as.fr.update);
    ny_purity_scope_restore(
        ctx->local_names, ctx->local_hashes,
        ctx->local_bloom ? ctx->local_bloom : (uint64_t[4]){0}, &cp);
    return false;
  }
  case NY_S_WHILE: {
    if (s->as.whl.init)
      ny_visit_stmt(v, s->as.whl.init);
    ny_visit_expr(v, s->as.whl.test);
    ny_visit_expr(v, s->as.whl.invariant);
    ny_effect_visit_loop_body(v, ctx, s->as.whl.body, s->as.whl.update);
    return false;
  }
  case NY_S_LEMMA:
    ny_visit_expr(v, s->as.lemma.proposition);
    return false;
  case NY_S_MATCH:
    /*
     * Pattern variant names are not executable
     * member calls.
     */
    if (s->as.match.test && s->as.match.test->kind == NY_E_COMPTIME) {
      size_t selected = 0;
      bool has_arm = false;
      if (ny_effect_comptime_match_arm(ctx->cg, s->as.match.test,
                                       &s->as.match.arms, &selected,
                                       &has_arm)) {
        if (has_arm) {
          match_arm_t *arm = &s->as.match.arms.data[selected];
          ny_visit_expr(v, arm->guard);
          ny_visit_stmt(v, arm->conseq);
        } else {
          ny_visit_stmt(v, s->as.match.default_conseq);
        }
        return false;
      }
    }
    ny_visit_expr(v, s->as.match.test);
    size_t branch_count = s->as.match.arms.len + 1;
    const ny_effect_local_fact_t **branch_facts =
        calloc(branch_count, sizeof(*branch_facts));
    size_t *branch_lens = calloc(branch_count, sizeof(*branch_lens));
    bool *branch_ambiguous = calloc(branch_count, sizeof(*branch_ambiguous));
    size_t base_len = 0;
    ny_effect_local_fact_t *base_facts =
        ny_effect_fact_snapshot(ctx, &base_len);
    bool snapshots_ok = branch_facts && branch_lens && branch_ambiguous &&
                        (!base_len || base_facts);
    bool base_ambiguous = ctx->callback_alias_state_ambiguous;
    if (snapshots_ok) {
      for (size_t i = 0; i < s->as.match.arms.len; ++i) {
        if (!ny_effect_fact_restore(ctx, base_facts, base_len)) {
          snapshots_ok = false;
          break;
        }
        ctx->callback_alias_state_ambiguous = base_ambiguous;
        match_arm_t *arm = &s->as.match.arms.data[i];
        ny_visit_expr(v, arm->guard);
        ny_visit_stmt(v, arm->conseq);
        branch_facts[i] = ny_effect_fact_snapshot(ctx, &branch_lens[i]);
        branch_ambiguous[i] = ctx->callback_alias_state_ambiguous;
        if (branch_lens[i] && !branch_facts[i]) {
          snapshots_ok = false;
          break;
        }
      }
    }
    if (snapshots_ok && ny_effect_fact_restore(ctx, base_facts, base_len)) {
      ctx->callback_alias_state_ambiguous = base_ambiguous;
      if (s->as.match.default_conseq)
        ny_visit_stmt(v, s->as.match.default_conseq);
      branch_facts[branch_count - 1] =
          s->as.match.default_conseq
              ? ny_effect_fact_snapshot(ctx, &branch_lens[branch_count - 1])
              : base_facts;
      if (s->as.match.default_conseq && branch_lens[branch_count - 1] &&
          !branch_facts[branch_count - 1])
        snapshots_ok = false;
      branch_ambiguous[branch_count - 1] = ctx->callback_alias_state_ambiguous;
      if (!s->as.match.default_conseq)
        branch_lens[branch_count - 1] = base_len;
    } else {
      snapshots_ok = false;
    }
    if (snapshots_ok && ny_effect_fact_restore(ctx, base_facts, base_len) &&
        ny_effect_fact_merge_branches(ctx, branch_facts, branch_lens,
                                      branch_count)) {
      ctx->callback_alias_state_ambiguous = false;
      for (size_t i = 0; i < branch_count; ++i)
        ctx->callback_alias_state_ambiguous |= branch_ambiguous[i];
    } else {
      ctx->callback_alias_state_ambiguous = true;
      ctx->callback_control_flow_depth++;
      for (size_t i = 0; i < s->as.match.arms.len; ++i) {
        match_arm_t *arm = &s->as.match.arms.data[i];
        ny_visit_expr(v, arm->guard);
        ny_visit_stmt(v, arm->conseq);
      }
      ny_visit_stmt(v, s->as.match.default_conseq);
      ctx->callback_control_flow_depth--;
    }
    for (size_t i = 0; i < branch_count; ++i)
      if (branch_facts && branch_facts[i] != base_facts)
        free((void *)branch_facts[i]);
    free(base_facts);
    free(branch_facts);
    free(branch_lens);
    free(branch_ambiguous);
    return false;
  case NY_S_GUARD:
    ny_visit_expr(v, s->as.guard.value);
    ctx->callback_control_flow_depth++;
    ny_visit_stmt(v, s->as.guard.fallback);
    ctx->callback_control_flow_depth--;
    return false;
  case NY_S_EXPR: {
    expr_t *expr = s->as.expr.expr;
    bool discarded_builtin_list_append = false;
    if (expr && expr->kind == NY_E_MEMCALL && expr->as.memcall.name &&
        strcmp(expr->as.memcall.name, "append") == 0 &&
        expr->as.memcall.target &&
        expr->as.memcall.target->kind == NY_E_IDENT &&
        expr->as.memcall.target->as.ident.name &&
        ny_effect_local_is_list(ctx, expr->as.memcall.target->as.ident.name)) {
      fun_sig *sig = ny_purity_resolve_memcall_sig(
          ctx->cg, &expr->as.memcall, ctx->local_names, ctx->local_hashes,
          ctx->local_bloom ? ctx->local_bloom : (uint64_t[4]){0});
      discarded_builtin_list_append =
          sig && ny_sig_is_std_for_effects(sig) && sig->name &&
          (strcmp(sig->name, "core.append") == 0 ||
           strcmp(sig->name, "core.list.append") == 0);
    }
    const expr_t *saved_discarded_append =
        ctx->callback_nonmutating_list_append;
    if (discarded_builtin_list_append)
      ctx->callback_nonmutating_list_append = expr;
    ny_visit_expr(v, expr);
    ctx->callback_nonmutating_list_append = saved_discarded_append;
    return false;
  }
  case NY_S_TRY: {
    if (!s->as.tr.handler) {
      ny_visit_stmt(v, s->as.tr.body);
      return false;
    }
    size_t try_base_len = 0, try_body_len = 0, try_handler_len = 0;
    ny_effect_local_fact_t *try_base =
        ny_effect_fact_snapshot(ctx, &try_base_len);
    bool try_base_ambiguous = ctx->callback_alias_state_ambiguous;
    if (try_base_len && !try_base) {
      ctx->callback_alias_state_ambiguous = true;
      ctx->known = false;
      ctx->callback_control_flow_depth++;
      ny_visit_stmt(v, s->as.tr.body);
      ny_visit_stmt(v, s->as.tr.handler);
      ctx->callback_control_flow_depth--;
      return false;
    }

    ctx->callback_try_body_depth++;
    ny_visit_stmt(v, s->as.tr.body);
    ctx->callback_try_body_depth--;
    ny_effect_local_fact_t *try_body =
        ny_effect_fact_snapshot(ctx, &try_body_len);
    bool body_ambiguous = ctx->callback_alias_state_ambiguous;
    bool body_captured = !try_body_len || try_body;
    bool body_restored = ny_effect_fact_restore(ctx, try_base, try_base_len);
    bool body_snapshot_ok = body_captured && body_restored;
    if (!body_snapshot_ok) {
      free(try_base);
      free(try_body);
      ctx->callback_alias_state_ambiguous = true;
      ctx->known = false;
      ctx->callback_control_flow_depth++;
      ny_visit_stmt(v, s->as.tr.handler);
      ctx->callback_control_flow_depth--;
      return false;
    }

    /*
     * A catch edge can originate after any body
     * instruction that may throw. Once the body
     * changed/called through a tracked
     * callback, the handler must not start from
     * the pre-try alias snapshot: its incoming
     * row is an intermediate, so resolve
     * captured callbacks as unknown there too.
     */
    ctx->callback_alias_state_ambiguous = try_base_ambiguous || body_ambiguous;
    ny_visit_stmt(v, s->as.tr.handler);
    ny_effect_local_fact_t *try_handler =
        ny_effect_fact_snapshot(ctx, &try_handler_len);
    bool handler_ambiguous = ctx->callback_alias_state_ambiguous;
    bool handler_captured = !try_handler_len || try_handler;
    bool handler_restored = ny_effect_fact_restore(ctx, try_base, try_base_len);
    bool handler_snapshot_ok = handler_captured && handler_restored;
    const ny_effect_local_fact_t *branches[] = {try_body, try_handler};
    size_t try_branch_lens[] = {try_body_len, try_handler_len};
    bool merged = handler_snapshot_ok && ny_effect_fact_merge_branches(
                                             ctx, branches, try_branch_lens, 2);
    ctx->callback_alias_state_ambiguous =
        merged ? body_ambiguous || handler_ambiguous : true;
    if (ctx->callback_alias_state_ambiguous)
      ctx->known = false;
    free(try_base);
    free(try_body);
    free(try_handler);
    return false;
  }
  case NY_S_DEFER:
    ctx->callback_control_flow_depth++;
    ny_visit_stmt(v, s->as.de.body);
    ctx->callback_control_flow_depth--;
    return false;
  case NY_S_OPERATOR:
  case NY_S_IMPL:
    return false;
  case NY_S_MACRO:
  case NY_S_INCLUDE: {
    ctx->result |= NY_FX_ALL;
    ctx->known = false;
    return true;
  }
  default:
    return true;
  }
}

static uint32_t ny_stmt_effects_in_context(ny_effects_ctx *ctx, stmt_t *s,
                                           const ny_param_list *params,
                                           const stmt_func_t *owner_fn) {
  if (!ctx)
    return NY_FX_ALL;
  if (!s)
    return NY_FX_NONE;
  ctx->owner_fn = owner_fn;
  if (params)
    for (size_t i = 0; i < params->len; ++i) {
      const param_t *param = &params->data[i];
      if (!param->type && param->name)
        ny_effect_record_local_container(ctx, param->name, NULL, NULL, NULL,
                                         NY_FX_ALL, true, true, false, 0, false,
                                         false, 0, false);
      if (param->type && (strcmp(param->type, "list") == 0 ||
                          strcmp(param->type, "dict") == 0 ||
                          strcmp(param->type, "set") == 0 ||
                          strcmp(param->type, "tuple") == 0))
        ny_effect_record_local_container(ctx, param->name, param->type, NULL,
                                         param->type, NY_FX_ALL, false, false,
                                         false, 0, false, false, 0, false);
    }
  ny_visitor_t v = {.ctx = ctx,
                    .visit_expr_pre = effects_visit_expr,
                    .visit_expr_post = effects_visit_expr_post,
                    .visit_stmt_pre = effects_visit_stmt,
                    .visit_stmt_post = effects_visit_stmt_post};
  ny_visit_stmt(&v, s);
  return ctx->result;
}

static uint32_t
ny_stmt_effects(codegen_t *cg, stmt_t *s, assigned_name_list *local_names,
                assigned_hash_list *local_hashes, uint64_t local_bloom[4],
                const ny_param_list *params, const stmt_func_t *owner_fn,
                const ny_type_t *owner_effect_row,
                bool owner_effect_contract_known, bool *known) {
  if (!s) {
    if (known)
      *known = true;
    return NY_FX_NONE;
  }
  ny_type_arena_t fact_arena;
  ny_type_arena_init(&fact_arena);
  ny_effects_ctx ctx = {
      .cg = cg,
      .fact_arena = &fact_arena,
      .owner_fn = owner_fn,
      .owner_effect_row = owner_effect_row,
      .owner_effect_contract_known = owner_effect_contract_known,
      .owner_effect_contract_override = owner_effect_contract_known,
      .local_names = local_names,
      .local_hashes = local_hashes,
      .local_bloom = local_bloom,
      .result = NY_FX_NONE,
      .known = true};
  uint32_t result = ny_stmt_effects_in_context(&ctx, s, params, owner_fn);
  if (known)
    *known = ctx.known;
  free(ctx.local_containers);
  ny_type_arena_reset(&fact_arena);
  return result;
}

static uint32_t ny_func_decl_effects(codegen_t *cg, stmt_t *fn_stmt,
                                     bool *known) {
  if (!cg || !fn_stmt || fn_stmt->kind != NY_S_FUNC || !fn_stmt->as.fn.body) {
    if (known)
      *known = false;
    return NY_FX_ALL;
  }
  uint32_t fx = NY_FX_NONE;
  if (fn_stmt->as.fn.attr_thread)
    fx |= NY_FX_THREAD;
  if (fn_stmt->as.fn.attr_naked)
    fx |= NY_FX_FFI;
  assigned_name_list local_names = {0};
  assigned_hash_list local_hashes = {0};
  uint64_t local_bloom[4] = {0, 0, 0, 0};
  for (size_t i = 0; i < fn_stmt->as.fn.params.len; i++) {
    const param_t *param = &fn_stmt->as.fn.params.data[i];
    assigned_name_add(&local_names, &local_hashes, local_bloom, param->name);
  }
  bool body_known = true;
  fx |= ny_stmt_effects(cg, fn_stmt->as.fn.body, &local_names, &local_hashes,
                        local_bloom, &fn_stmt->as.fn.params, &fn_stmt->as.fn,
                        NULL, false, &body_known);
  vec_free(&local_names);
  vec_free(&local_hashes);
  if (known)
    *known = body_known;
  return fx;
}

void ny_analyze_function_call_effect_facts(codegen_t *cg, stmt_t *fn_stmt) {
  const char *saved_module = cg ? cg->current_module_name : NULL;
  if (cg) {
    for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
      fun_sig *sig = &cg->fun_sigs.data[i];
      if (sig->stmt_t == fn_stmt && sig->name) {
        cg->current_module_name = ny_module_prefix_stable(cg, sig->name);
        break;
      }
    }
  }
  bool known = false;
  (void)ny_func_decl_effects(cg, fn_stmt, &known);
  if (cg)
    cg->current_module_name = saved_module;
}

bool ny_sig_callable_effect_facts(const fun_sig *sig, uint32_t *effects) {
  /*
   * Read-only summary for semantic artifacts: a computed declaration row when
   * the fixpoint produced one, otherwise the audited runtime contract for
   * builtin and std-spelled callables. Returns false when no closed row is
   * known so callers can distinguish "proven" from "unknown".
   */
  if (!sig)
    return false;
  if (sig->effects_known) {
    if (effects)
      *effects = sig->effects;
    return true;
  }
  return ny_sig_builtin_contract(sig, effects);
}

static bool ny_comptime_scan_body_io(codegen_t *cg, stmt_t *stmt,
                                     const stmt_func_t *owner,
                                     ny_comptime_io_scan_t *scan) {
  assigned_name_list names = {0};
  assigned_hash_list hashes = {0};
  uint64_t bloom[4] = {0};
  if (owner)
    for (size_t i = 0; i < owner->params.len; ++i)
      assigned_name_add(&names, &hashes, bloom, owner->params.data[i].name);
  ny_type_arena_t fact_arena;
  ny_type_arena_init(&fact_arena);
  ny_effects_ctx ctx = {.cg = cg,
                        .fact_arena = &fact_arena,
                        .local_names = &names,
                        .local_hashes = &hashes,
                        .local_bloom = bloom,
                        .comptime_io_scan = scan,
                        .known = true};
  ny_stmt_effects_in_context(&ctx, stmt, owner ? &owner->params : NULL, owner);
  free(ctx.local_containers);
  ny_type_arena_reset(&fact_arena);
  vec_free(&names);
  vec_free(&hashes);
  return ctx.has_known_io;
}

static bool ny_effect_comptime_callee_io(ny_effects_ctx *ctx, fun_sig *sig) {
  if (!ctx->comptime_io_scan || !sig || !sig->stmt_t ||
      sig->stmt_t->kind != NY_S_FUNC || ny_sig_is_std_for_effects(sig))
    return false;
  ny_comptime_io_scan_t *scan = ctx->comptime_io_scan;
  if (scan->steps++ >= 1024) {
    scan->incomplete = true;
    return false;
  }
  for (size_t i = 0; i < scan->len; ++i)
    if (scan->seen[i] == sig->stmt_t)
      return false;
  if (scan->len == sizeof(scan->seen) / sizeof(scan->seen[0])) {
    scan->incomplete = true;
    return false;
  }
  scan->seen[scan->len++] = sig->stmt_t;
  bool has_io = ny_comptime_scan_body_io(ctx->cg, sig->stmt_t->as.fn.body,
                                         &sig->stmt_t->as.fn, scan);
  --scan->len;
  return has_io;
}

ny_comptime_effect_status_t ny_stmt_comptime_effect_status(codegen_t *cg,
                                                           stmt_t *stmt) {
  /*
   * Report proven IO separately from a
   * conservative unknown effect mask. Direct
   * user calls are inspected even before their
   * inferred summaries are available; the
   * bounded visited set terminates recursive
   * call graphs.
   */
  ny_comptime_io_scan_t scan = {0};
  bool has_io = ny_comptime_scan_body_io(cg, stmt, NULL, &scan);
  if (has_io)
    return NY_COMPTIME_EFFECTS_IO;
  return scan.incomplete ? NY_COMPTIME_EFFECTS_INCOMPLETE
                         : NY_COMPTIME_EFFECTS_SAFE;
}

static fun_sig *ny_purity_resolve_call_sig(codegen_t *cg, expr_call_t *call,
                                           assigned_name_list *local_names,
                                           assigned_hash_list *local_hashes,
                                           uint64_t local_bloom[4]) {
  if (!cg || !call || !call->callee)
    return NULL;
  expr_t *callee = call->callee;
  while (callee && callee->kind == NY_E_SPLICE)
    callee = callee->as.splice.expr;
  if (!callee || callee->kind != NY_E_IDENT)
    return NULL;
  const char *name = callee->as.ident.name;
  if (!name || !*name)
    return NULL;
  if (assigned_name_contains(local_names, local_hashes, local_bloom, name))
    return NULL;
  fun_sig *sig = resolve_overload(cg, name, call->args.len, 0);
  if (!sig)
    sig = lookup_use_module_fun(cg, name, call->args.len);
  if (!sig)
    sig = lookup_fun(cg, name, 0);
  if (!sig) {
    /*
     * Module-private helpers flatten to
     * qualified spellings
     * (`core.reflect._is_str`) with no
     * short-name entry.  When every registered
     * tail match agrees on an effect contract,
     * that contract applies to the call.
     */
    size_t nlen = strlen(name);
    fun_sig *hit = NULL;
    bool ambiguous = false;
    for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
      fun_sig *cand = &cg->fun_sigs.data[i];
      if (!cand->name)
        continue;
      const char *leaf = strrchr(cand->name, '.');
      if (!leaf || strncmp(leaf + 1, name, nlen) != 0 || leaf[1 + nlen] != '\0')
        continue;
      if (!hit) {
        hit = cand;
        continue;
      }
      if (ny_sig_effects(hit) != ny_sig_effects(cand) ||
          ny_sig_effects_known(hit) != ny_sig_effects_known(cand)) {
        ambiguous = true;
        break;
      }
    }
    if (!ambiguous)
      sig = hit;
  }
  return sig;
}

static fun_sig *ny_purity_resolve_memcall_sig(codegen_t *cg, expr_memcall_t *mc,
                                              assigned_name_list *local_names,
                                              assigned_hash_list *local_hashes,
                                              uint64_t local_bloom[4]) {
  if (!cg || !mc || !mc->name || !mc->target) {
    return NULL;
  }
  /*
   * A method on a non-identifier receiver
   * (`dict(4).set(k, v)`) has the same
   * free-function form as instance sugar:
   * resolve it so chained container calls keep
   * their (stdlib, known) contracts instead of
   * degrading the whole enclosing function to
   * unknown effects.
   */
  if (mc->target->kind != NY_E_IDENT) {
    char module_path[1024];
    char resolved_fun[1280];
    if (ny_resolve_module_expr_path(cg, NULL, 0, mc->target, module_path,
                                    sizeof(module_path)) &&
        ny_resolve_module_function_path(cg, module_path, mc->name, resolved_fun,
                                        sizeof(resolved_fun)))
      return lookup_fun(cg, resolved_fun, 0);
    char qualified_fun[1280] = {0};
    size_t qualified_len = 0;
    if (ny_effect_expr_qualified_name(mc->target, qualified_fun,
                                      sizeof(qualified_fun), &qualified_len) &&
        qualified_len < sizeof(qualified_fun)) {
      int n = snprintf(qualified_fun + qualified_len,
                       sizeof(qualified_fun) - qualified_len, ".%s", mc->name);
      if (n > 0 && (size_t)n < sizeof(qualified_fun) - qualified_len) {
        fun_sig *sig = lookup_fun(cg, qualified_fun, 0);
        if (sig)
          return sig;
      }
    }
    return lookup_fun(cg, mc->name, 0);
  }
  const char *target_name = mc->target->as.ident.name;
  if (!target_name || !*target_name)
    return NULL;
  if (assigned_name_contains(local_names, local_hashes, local_bloom,
                             target_name)) {
    /*
     * Instance-method sugar on a local
     * receiver: `built.append(v)` is the
     * free-function call `append(built, v)` for
     * effect purposes.  Resolving that global
     * keeps the callee's (stdlib, therefore
     * known) contract. A miss falls through to
     * the module path; attached-method-only
     * names (SelfBox.maybe) resolve via the
     * visitor's last-resort suffix scan so
     * container-specific contracts keep
     * priority.
     */
    return lookup_fun(cg, mc->name, 0);
  }
  const char *module_name = resolve_import_alias(cg, target_name);
  bool module_like = module_name != NULL;
  if (!module_name)
    module_name = target_name;
  if (!module_like) {
    if (lookup_global(cg, target_name) || lookup_fun(cg, target_name, 0))
      return NULL;
    module_like = true;
  }
  if (!module_like)
    return NULL;
  char resolved_fun[1280];
  if (!ny_resolve_module_function_path(cg, module_name, mc->name, resolved_fun,
                                       sizeof(resolved_fun)))
    return lookup_fun(cg, mc->name, 0);
  return lookup_fun(cg, resolved_fun, 0);
}

#define CHECK_PURE_EXPR(e)                                                     \
  do {                                                                         \
    if (!ny_expr_check_safe_internal(cg, (e), local_names, local_hashes,       \
                                     local_bloom, memo_safe_only))             \
      return false;                                                            \
  } while (0)

#define CHECK_PURE_STMT(s)                                                     \
  do {                                                                         \
    if (!ny_stmt_check_safe_internal(cg, (s), local_names, local_hashes,       \
                                     local_bloom, memo_safe_only))             \
      return false;                                                            \
  } while (0)

static bool ny_expr_check_safe_internal(codegen_t *cg, expr_t *e,
                                        assigned_name_list *local_names,
                                        assigned_hash_list *local_hashes,
                                        uint64_t local_bloom[4],
                                        bool memo_safe_only);

static bool ny_stmt_check_safe_internal(codegen_t *cg, stmt_t *s,
                                        assigned_name_list *local_names,
                                        assigned_hash_list *local_hashes,
                                        uint64_t local_bloom[4],
                                        bool memo_safe_only);

static bool ny_purity_match_pattern(codegen_t *cg, expr_t *pattern,
                                    assigned_name_list *local_names,
                                    assigned_hash_list *local_hashes,
                                    uint64_t local_bloom[4],
                                    bool allow_payload_bindings,
                                    bool memo_safe_only) {
  if (!pattern)
    return false;
  if (pattern->kind == NY_E_LITERAL)
    return true;
  if (pattern->kind == NY_E_IDENT) {
    const char *name = pattern->as.ident.name;
    if (ny_expr_is_wildcard_ident(pattern))
      return true;
    return name && lookup_enum_member_owner(cg, name, NULL)
               ? true
               : ny_expr_check_safe_internal(cg, pattern, local_names,
                                             local_hashes, local_bloom,
                                             memo_safe_only);
  }
  if (pattern->kind == NY_E_MEMBER || pattern->kind == NY_E_CALL ||
      pattern->kind == NY_E_MEMCALL) {
    char *full_name = pattern->kind == NY_E_MEMBER
                          ? codegen_full_name(cg, pattern, cg->arena)
                          : ny_adt_member_call_full_name(cg, pattern);
    enum_member_def_t *member =
        full_name ? lookup_enum_member_owner(cg, full_name, NULL) : NULL;
    if (member) {
      if (pattern->kind == NY_E_MEMBER || pattern->kind == NY_E_IDENT)
        return true;
      if (!allow_payload_bindings || !member->has_payload)
        return false;
      call_arg_t *args = NULL;
      size_t count = 0;
      if (!ny_expr_call_args(pattern, &args, &count) ||
          count > member->fields.len)
        return false;
      for (size_t i = 0; i < count; ++i) {
        call_arg_t *arg = &args[i];
        ssize_t field = arg->name
                            ? ny_enum_member_field_index(member, arg->name)
                            : (ssize_t)i;
        if (field < 0 || (size_t)field >= member->fields.len || !arg->val)
          return false;
        if (ny_expr_is_wildcard_ident(arg->val))
          continue;
        if (arg->val->kind != NY_E_IDENT || !arg->val->as.ident.name)
          return false;
        assigned_name_add(local_names, local_hashes, local_bloom,
                          arg->val->as.ident.name);
      }
      return true;
    }
    return ny_expr_check_safe_internal(cg, pattern, local_names, local_hashes,
                                       local_bloom, memo_safe_only);
  }
  return ny_expr_check_safe_internal(cg, pattern, local_names, local_hashes,
                                     local_bloom, memo_safe_only);
}

static bool ny_stmt_is_pure(codegen_t *cg, stmt_t *s,
                            assigned_name_list *local_names,
                            assigned_hash_list *local_hashes,
                            uint64_t local_bloom[4]) {
  return ny_stmt_check_safe_internal(cg, s, local_names, local_hashes,
                                     local_bloom, false);
}

static bool ny_stmt_is_memo_safe(codegen_t *cg, stmt_t *s,
                                 assigned_name_list *local_names,
                                 assigned_hash_list *local_hashes,
                                 uint64_t local_bloom[4]) {
  return ny_stmt_check_safe_internal(cg, s, local_names, local_hashes,
                                     local_bloom, true);
}

static bool ny_expr_check_safe_internal(codegen_t *cg, expr_t *e,
                                        assigned_name_list *local_names,
                                        assigned_hash_list *local_hashes,
                                        uint64_t local_bloom[4],
                                        bool memo_safe_only) {
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
    if (lookup_fun(cg, name, 0))
      return true;
    return false;
  }
  case NY_E_UNARY:
    if (e->as.unary.op && (strcmp(e->as.unary.op, "async") == 0 ||
                           strcmp(e->as.unary.op, "await") == 0))
      return false;
    CHECK_PURE_EXPR(e->as.unary.right);
    return true;
  case NY_E_BINARY:
  case NY_E_LOGICAL:
    CHECK_PURE_EXPR(e->as.binary.left);
    CHECK_PURE_EXPR(e->as.binary.right);
    return true;
  case NY_E_TERNARY:
    CHECK_PURE_EXPR(e->as.ternary.cond);
    CHECK_PURE_EXPR(e->as.ternary.true_expr);
    CHECK_PURE_EXPR(e->as.ternary.false_expr);
    return true;
  case NY_E_CALL: {
    for (size_t i = 0; i < e->as.call.args.len; i++) {
      CHECK_PURE_EXPR(e->as.call.args.data[i].val);
    }
    fun_sig *sig = ny_purity_resolve_call_sig(cg, &e->as.call, local_names,
                                              local_hashes, local_bloom);
    return memo_safe_only ? ny_sig_is_memo_safe(sig) : ny_sig_is_pure(sig);
  }
  case NY_E_MEMCALL: {
    for (size_t i = 0; i < e->as.memcall.args.len; i++) {
      CHECK_PURE_EXPR(e->as.memcall.args.data[i].val);
    }
    fun_sig *sig = ny_purity_resolve_memcall_sig(
        cg, &e->as.memcall, local_names, local_hashes, local_bloom);
    return memo_safe_only ? ny_sig_is_memo_safe(sig) : ny_sig_is_pure(sig);
  }
  case NY_E_INDEX:
    if (!memo_safe_only)
      return false;
    CHECK_PURE_EXPR(e->as.index.target);
    CHECK_PURE_EXPR(e->as.index.start);
    CHECK_PURE_EXPR(e->as.index.stop);
    CHECK_PURE_EXPR(e->as.index.step);
    return true;
  case NY_E_MEMBER:
  case NY_E_DEREF:
    if (!memo_safe_only)
      return false;
    CHECK_PURE_EXPR(e->as.member.target);
    return true;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    if (!memo_safe_only)
      return false;
    for (size_t i = 0; i < e->as.list_like.len; i++) {
      CHECK_PURE_EXPR(e->as.list_like.data[i]);
    }
    return true;
  case NY_E_DICT:
    if (!memo_safe_only)
      return false;
    for (size_t i = 0; i < e->as.dict.pairs.len; i++) {
      CHECK_PURE_EXPR(e->as.dict.pairs.data[i].key);
      CHECK_PURE_EXPR(e->as.dict.pairs.data[i].value);
    }
    return true;
  case NY_E_PTR_TYPE:
    CHECK_PURE_EXPR(e->as.ptr_type.target);
    return true;
  case NY_E_SIZEOF:
    if (e->as.szof.is_type)
      return true;
    CHECK_PURE_EXPR(e->as.szof.target);
    return true;
  case NY_E_MATCH: {
    CHECK_PURE_EXPR(e->as.match.test);
    for (size_t i = 0; i < e->as.match.arms.len; i++) {
      match_arm_t *arm = &e->as.match.arms.data[i];
      ny_purity_scope_checkpoint_t cp = {0};
      ny_purity_scope_save(local_names, local_hashes, local_bloom, &cp);
      for (size_t j = 0; j < arm->patterns.len; j++) {
        if (!ny_purity_match_pattern(cg, arm->patterns.data[j], local_names,
                                     local_hashes, local_bloom,
                                     arm->patterns.len == 1, memo_safe_only)) {
          ny_purity_scope_restore(local_names, local_hashes, local_bloom, &cp);
          return false;
        }
      }
      bool safe =
          ny_expr_check_safe_internal(cg, arm->guard, local_names, local_hashes,
                                      local_bloom, memo_safe_only) &&
          ny_stmt_check_safe_internal(cg, arm->conseq, local_names,
                                      local_hashes, local_bloom,
                                      memo_safe_only);
      ny_purity_scope_restore(local_names, local_hashes, local_bloom, &cp);
      if (!safe)
        return false;
    }
    CHECK_PURE_STMT(e->as.match.default_conseq);
    return true;
  }
  case NY_E_FSTRING:
    if (!memo_safe_only)
      return false;
    for (size_t i = 0; i < e->as.fstring.parts.len; i++) {
      fstring_part_t *part = &e->as.fstring.parts.data[i];
      if (part->kind == NY_FSP_EXPR) {
        CHECK_PURE_EXPR(part->as.e);
      }
    }
    return true;
  case NY_E_TRY:
  case NY_E_ASM:
  case NY_E_COMPTIME:
  case NY_E_QUOTE:
  case NY_E_SPLICE:
  case NY_E_INFERRED_MEMBER:
  case NY_E_EMBED:
  case NY_E_LAMBDA:
  case NY_E_FN:
    return false;
  case NY_E_EXISTS:
    CHECK_PURE_EXPR(e->as.exists.body);
    return true;
  }
  return false;
}

static bool ny_stmt_check_safe_internal(codegen_t *cg, stmt_t *s,
                                        assigned_name_list *local_names,
                                        assigned_hash_list *local_hashes,
                                        uint64_t local_bloom[4],
                                        bool memo_safe_only) {
  if (!s)
    return true;
  switch (s->kind) {
  case NY_S_BLOCK: {
    ny_purity_scope_checkpoint_t cp = {0};
    ny_purity_scope_save(local_names, local_hashes, local_bloom, &cp);
    bool safe = true;
    for (size_t i = 0; i < s->as.block.body.len; i++) {
      if (!ny_stmt_check_safe_internal(cg, s->as.block.body.data[i],
                                       local_names, local_hashes, local_bloom,
                                       memo_safe_only)) {
        safe = false;
        break;
      }
    }
    ny_purity_scope_restore(local_names, local_hashes, local_bloom, &cp);
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
      CHECK_PURE_EXPR(s->as.var.exprs.data[i]);
    }
    if (s->as.var.is_decl) {
      for (size_t i = 0; i < s->as.var.names.len; i++) {
        assigned_name_add(local_names, local_hashes, local_bloom,
                          s->as.var.names.data[i]);
      }
    }
    return true;
  case NY_S_EXPR:
    CHECK_PURE_EXPR(s->as.expr.expr);
    return true;
  case NY_S_AUG:
    /*
     * Raw-mode statements never reach semantic analysis; reject them
     * fail-closed if one ever does.
     */
    return false;
  case NY_S_RETURN:
    CHECK_PURE_EXPR(s->as.ret.value);
    return true;
  case NY_S_IF:
    CHECK_PURE_EXPR(s->as.iff.test);
    CHECK_PURE_STMT(s->as.iff.conseq);
    CHECK_PURE_STMT(s->as.iff.alt);
    return true;
  case NY_S_WHILE:
    CHECK_PURE_EXPR(s->as.whl.test);
    CHECK_PURE_STMT(s->as.whl.body);
    if (s->as.whl.update) {
      CHECK_PURE_STMT(s->as.whl.update);
    }
    if (s->as.whl.init) {
      CHECK_PURE_STMT(s->as.whl.init);
    }
    return true;
  case NY_S_FOR: {
    if (s->as.fr.init)
      CHECK_PURE_STMT(s->as.fr.init);
    if (s->as.fr.cond)
      CHECK_PURE_EXPR(s->as.fr.cond);
    if (s->as.fr.iterable)
      CHECK_PURE_EXPR(s->as.fr.iterable);
    ny_purity_scope_checkpoint_t cp = {0};
    ny_purity_scope_save(local_names, local_hashes, local_bloom, &cp);
    if (s->as.fr.iter_var)
      assigned_name_add(local_names, local_hashes, local_bloom,
                        s->as.fr.iter_var);
    if (s->as.fr.iter_index_var)
      assigned_name_add(local_names, local_hashes, local_bloom,
                        s->as.fr.iter_index_var);
    bool safe =
        ny_stmt_check_safe_internal(cg, s->as.fr.body, local_names,
                                    local_hashes, local_bloom, memo_safe_only);
    if (s->as.fr.update)
      CHECK_PURE_STMT(s->as.fr.update);
    ny_purity_scope_restore(local_names, local_hashes, local_bloom, &cp);
    return safe;
  }
  case NY_S_LEMMA:
    CHECK_PURE_EXPR(s->as.lemma.proposition);
    return true;
  case NY_S_MATCH: {
    CHECK_PURE_EXPR(s->as.match.test);
    for (size_t i = 0; i < s->as.match.arms.len; i++) {
      match_arm_t *arm = &s->as.match.arms.data[i];
      ny_purity_scope_checkpoint_t cp = {0};
      ny_purity_scope_save(local_names, local_hashes, local_bloom, &cp);
      for (size_t j = 0; j < arm->patterns.len; j++) {
        if (!ny_purity_match_pattern(cg, arm->patterns.data[j], local_names,
                                     local_hashes, local_bloom,
                                     arm->patterns.len == 1, memo_safe_only)) {
          ny_purity_scope_restore(local_names, local_hashes, local_bloom, &cp);
          return false;
        }
      }
      bool safe =
          ny_expr_check_safe_internal(cg, arm->guard, local_names, local_hashes,
                                      local_bloom, memo_safe_only) &&
          ny_stmt_check_safe_internal(cg, arm->conseq, local_names,
                                      local_hashes, local_bloom,
                                      memo_safe_only);
      ny_purity_scope_restore(local_names, local_hashes, local_bloom, &cp);
      if (!safe)
        return false;
    }
    CHECK_PURE_STMT(s->as.match.default_conseq);
    return true;
  }
  case NY_S_GUARD:
    CHECK_PURE_EXPR(s->as.guard.value);
    CHECK_PURE_STMT(s->as.guard.fallback);
    return true;
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
  case NY_S_LINK:
  case NY_S_INCLUDE:
  case NY_S_DEFINE:
  case NY_S_OPERATOR:
  case NY_S_IMPL:
    return false;
  case NY_S_REFINE:
  case NY_S_TRAIT:
  case NY_S_BREAK:
  case NY_S_CONTINUE:
    return true;
  }
  return false;
}

#undef CHECK_PURE_EXPR
#undef CHECK_PURE_STMT

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
  case NY_E_QUOTE:
    if (e->as.quote.body)
      return ny_stmt_refs_params(e->as.quote.body, param_names, param_hashes,
                                 param_bloom);
    if (e->as.quote.expr)
      return ny_expr_refs_params(e->as.quote.expr, param_names, param_hashes,
                                 param_bloom);
    return false;
  case NY_E_SPLICE:
    return ny_expr_refs_params(e->as.splice.expr, param_names, param_hashes,
                               param_bloom);
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
                               param_bloom) ||
           (s->as.whl.update &&
            ny_stmt_refs_params(s->as.whl.update, param_names, param_hashes,
                                param_bloom)) ||
           (s->as.whl.init && ny_stmt_refs_params(s->as.whl.init, param_names,
                                                  param_hashes, param_bloom));
  case NY_S_FOR: {
    bool refs = false;
    if (s->as.fr.init)
      refs |= ny_stmt_refs_params(s->as.fr.init, param_names, param_hashes,
                                  param_bloom);
    if (s->as.fr.cond)
      refs |= ny_expr_refs_params(s->as.fr.cond, param_names, param_hashes,
                                  param_bloom);
    if (s->as.fr.iterable)
      refs |= ny_expr_refs_params(s->as.fr.iterable, param_names, param_hashes,
                                  param_bloom);
    if (s->as.fr.update)
      refs |= ny_stmt_refs_params(s->as.fr.update, param_names, param_hashes,
                                  param_bloom);
    refs |= ny_stmt_refs_params(s->as.fr.body, param_names, param_hashes,
                                param_bloom);
    return refs;
  }
  case NY_S_LEMMA:
    return ny_expr_refs_params(s->as.lemma.proposition, param_names,
                               param_hashes, param_bloom);
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
  sema_func_t *sema = ny_sig_func_sema(sig);
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
    break;
  case NY_E_MEMBER:
    ny_collect_escape_expr(cg, e->as.member.target, param_names, param_hashes,
                           param_bloom, local_names, local_hashes, local_bloom,
                           out);
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
  case NY_E_QUOTE:
    if (e->as.quote.body)
      ny_collect_escape_stmt(cg, e->as.quote.body, param_names, param_hashes,
                             param_bloom, local_names, local_hashes,
                             local_bloom, out);
    if (e->as.quote.expr)
      ny_collect_escape_expr(cg, e->as.quote.expr, param_names, param_hashes,
                             param_bloom, local_names, local_hashes,
                             local_bloom, out);
    if (ny_expr_refs_params(e, param_names, param_hashes, param_bloom))
      out->args_escape = true;
    break;
  case NY_E_SPLICE:
    if (e->as.splice.expr)
      ny_collect_escape_expr(cg, e->as.splice.expr, param_names, param_hashes,
                             param_bloom, local_names, local_hashes,
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
    ny_purity_scope_checkpoint_t cp = {0};
    ny_purity_scope_save(local_names, local_hashes, local_bloom, &cp);
    for (size_t i = 0; i < s->as.block.body.len; i++) {
      ny_collect_escape_stmt(cg, s->as.block.body.data[i], param_names,
                             param_hashes, param_bloom, local_names,
                             local_hashes, local_bloom, out);
    }
    ny_purity_scope_restore(local_names, local_hashes, local_bloom, &cp);
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
    if (s->as.whl.update)
      ny_collect_escape_stmt(cg, s->as.whl.update, param_names, param_hashes,
                             param_bloom, local_names, local_hashes,
                             local_bloom, out);
    if (s->as.whl.init)
      ny_collect_escape_stmt(cg, s->as.whl.init, param_names, param_hashes,
                             param_bloom, local_names, local_hashes,
                             local_bloom, out);
    break;
  case NY_S_FOR: {
    if (s->as.fr.init)
      ny_collect_escape_stmt(cg, s->as.fr.init, param_names, param_hashes,
                             param_bloom, local_names, local_hashes,
                             local_bloom, out);
    if (s->as.fr.cond)
      ny_collect_escape_expr(cg, s->as.fr.cond, param_names, param_hashes,
                             param_bloom, local_names, local_hashes,
                             local_bloom, out);
    if (s->as.fr.iterable)
      ny_collect_escape_expr(cg, s->as.fr.iterable, param_names, param_hashes,
                             param_bloom, local_names, local_hashes,
                             local_bloom, out);
    ny_purity_scope_checkpoint_t cp = {0};
    ny_purity_scope_save(local_names, local_hashes, local_bloom, &cp);
    if (s->as.fr.iter_var)
      assigned_name_add(local_names, local_hashes, local_bloom,
                        s->as.fr.iter_var);
    if (s->as.fr.iter_index_var)
      assigned_name_add(local_names, local_hashes, local_bloom,
                        s->as.fr.iter_index_var);
    ny_collect_escape_stmt(cg, s->as.fr.body, param_names, param_hashes,
                           param_bloom, local_names, local_hashes, local_bloom,
                           out);
    if (s->as.fr.update)
      ny_collect_escape_stmt(cg, s->as.fr.update, param_names, param_hashes,
                             param_bloom, local_names, local_hashes,
                             local_bloom, out);
    ny_purity_scope_restore(local_names, local_hashes, local_bloom, &cp);
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
  case NY_S_LEMMA:
    ny_collect_escape_expr(cg, s->as.lemma.proposition, param_names,
                           param_hashes, param_bloom, local_names, local_hashes,
                           local_bloom, out);
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
      /*
       * Constructor-shaped patterns describe
       * values; they are not calls.
       */
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
    ny_purity_scope_checkpoint_t cp = {0};
    ny_purity_scope_save(local_names, local_hashes, local_bloom, &cp);
    for (size_t i = 0; i < s->as.block.body.len; i++) {
      ny_collect_calls_stmt(cg, s->as.block.body.data[i], local_names,
                            local_hashes, local_bloom, out_calls);
    }
    ny_purity_scope_restore(local_names, local_hashes, local_bloom, &cp);
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
    if (s->as.whl.update)
      ny_collect_calls_stmt(cg, s->as.whl.update, local_names, local_hashes,
                            local_bloom, out_calls);
    if (s->as.whl.init)
      ny_collect_calls_stmt(cg, s->as.whl.init, local_names, local_hashes,
                            local_bloom, out_calls);
    break;
  case NY_S_FOR: {
    if (s->as.fr.init)
      ny_collect_calls_stmt(cg, s->as.fr.init, local_names, local_hashes,
                            local_bloom, out_calls);
    if (s->as.fr.cond)
      ny_collect_calls_expr(cg, s->as.fr.cond, local_names, local_hashes,
                            local_bloom, out_calls);
    if (s->as.fr.iterable)
      ny_collect_calls_expr(cg, s->as.fr.iterable, local_names, local_hashes,
                            local_bloom, out_calls);
    ny_purity_scope_checkpoint_t cp = {0};
    ny_purity_scope_save(local_names, local_hashes, local_bloom, &cp);
    if (s->as.fr.iter_var)
      assigned_name_add(local_names, local_hashes, local_bloom,
                        s->as.fr.iter_var);
    if (s->as.fr.iter_index_var)
      assigned_name_add(local_names, local_hashes, local_bloom,
                        s->as.fr.iter_index_var);
    ny_collect_calls_stmt(cg, s->as.fr.body, local_names, local_hashes,
                          local_bloom, out_calls);
    if (s->as.fr.update)
      ny_collect_calls_stmt(cg, s->as.fr.update, local_names, local_hashes,
                            local_bloom, out_calls);
    ny_purity_scope_restore(local_names, local_hashes, local_bloom, &cp);
    break;
  }
  case NY_S_MATCH:
    ny_collect_calls_expr(cg, s->as.match.test, local_names, local_hashes,
                          local_bloom, out_calls);
    for (size_t i = 0; i < s->as.match.arms.len; i++) {
      match_arm_t *arm = &s->as.match.arms.data[i];
      /*
       * Constructor-shaped patterns describe
       * values; they are not calls.
       */
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
  case NY_S_LEMMA:
    ny_collect_calls_expr(cg, s->as.lemma.proposition, local_names,
                          local_hashes, local_bloom, out_calls);
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
  size_t *component_ids;
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
  if (ctx->component_ids)
    for (size_t i = 0; i < component_len; ++i)
      ctx->component_ids[ctx->component[i]] = node_idx;
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
                                         size_t *recursive_count,
                                         size_t *component_ids) {
  if (!edges || !enabled || !recursive_flags || !recursive_count)
    return false;
  if (node_count == 0)
    return true;
  size_t sz = sizeof(size_t) * node_count;
  size_t bool_off = sz * 4;
  unsigned char *block = malloc(bool_off + sizeof(bool) * node_count);
  if (!block)
    return false;
  size_t *indices = (size_t *)(void *)block;
  size_t *lowlink = (size_t *)(void *)(block + sz);
  size_t *stack = (size_t *)(void *)(block + sz * 2);
  size_t *component = (size_t *)(void *)(block + sz * 3);
  bool *on_stack = (bool *)(void *)(block + bool_off);
  for (size_t i = 0; i < node_count; i++) {
    indices[i] = SIZE_MAX;
    if (component_ids)
      component_ids[i] = SIZE_MAX;
  }
  memset(on_stack, 0, sizeof(bool) * node_count);
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
      .component_ids = component_ids,
  };
  for (size_t i = 0; i < node_count; i++) {
    if (!enabled[i] || indices[i] != SIZE_MAX)
      continue;
    ny_mark_recursive_components_dfs(&ctx, i);
  }
  free(block);
  return true;
}

typedef struct {
  codegen_t *cg;
  size_t caller;
  const size_t *components;
  const ny_sig_idx_map_entry *signature_indices;
  bool valid;
  bool saw_recursive_edge;
} ny_totality_edges_t;

static bool ny_totality_edge_pre(ny_visitor_t *visitor, expr_t *expr) {
  ny_totality_edges_t *ctx = visitor->ctx;
  if (expr->kind == NY_E_QUOTE || expr->kind == NY_E_COMPTIME)
    return false;
  if (expr->kind == NY_E_MATCH) {
    ny_visit_expr(visitor, expr->as.match.test);
    for (size_t i = 0; i < expr->as.match.arms.len; ++i) {
      match_arm_t *arm = &expr->as.match.arms.data[i];
      ny_visit_expr(visitor, arm->guard);
      ny_visit_stmt(visitor, arm->conseq);
    }
    ny_visit_stmt(visitor, expr->as.match.default_conseq);
    return false;
  }
  if (expr->kind != NY_E_CALL && expr->kind != NY_E_MEMCALL)
    return true;
  fun_sig *callee =
      expr->semantic.canonical_callee
          ? lookup_fun_exact(ctx->cg, expr->semantic.canonical_callee)
          : NULL;
  if (!callee || callee->stmt_t != expr->semantic.canonical_callee_stmt) {
    ctx->valid = false;
    return false;
  }
  long callee_index = ny_lookup_fun_sig_index_map(
      ctx->signature_indices, ctx->cg->fun_sigs.len, callee);
  if (callee_index >= 0 && callee->is_recursive &&
      ctx->components[callee_index] == ctx->components[ctx->caller]) {
    ctx->saw_recursive_edge = true;
    if (!expr->semantic.decreases_proven_by_caller)
      ctx->valid = false;
  }
  return ctx->valid;
}

static bool ny_totality_nested_function_pre(ny_visitor_t *visitor,
                                            stmt_t *stmt) {
  if (stmt->kind == NY_S_MATCH) {
    ny_visit_expr(visitor, stmt->as.match.test);
    for (size_t i = 0; i < stmt->as.match.arms.len; ++i) {
      match_arm_t *arm = &stmt->as.match.arms.data[i];
      ny_visit_expr(visitor, arm->guard);
      ny_visit_stmt(visitor, arm->conseq);
    }
    ny_visit_stmt(visitor, stmt->as.match.default_conseq);
    return false;
  }
  return stmt->kind != NY_S_FUNC && stmt->kind != NY_S_MACRO &&
         stmt->kind != NY_S_LEMMA;
}

static bool
ny_totality_component_proved(codegen_t *cg, size_t component,
                             const size_t *components,
                             const ny_sig_idx_map_entry *signature_indices) {
  for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
    fun_sig *sig = &cg->fun_sigs.data[i];
    if (components[i] != component)
      continue;
    if (!sig->stmt_t || sig->stmt_t->kind != NY_S_FUNC ||
        !sig->stmt_t->as.fn.decreases_expr)
      return false;
    ny_totality_edges_t ctx = {
        .cg = cg,
        .caller = i,
        .components = components,
        .signature_indices = signature_indices,
        .valid = true,
    };
    ny_visitor_t visitor = {
        .ctx = &ctx,
        .visit_expr_pre = ny_totality_edge_pre,
        .visit_stmt_pre = ny_totality_nested_function_pre,
    };
    ny_visit_stmt(&visitor, sig->stmt_t->as.fn.body);
    if (!ctx.valid || !ctx.saw_recursive_edge)
      return false;
  }
  return true;
}

static bool
ny_analyze_recursion(codegen_t *cg, size_t *components,
                     ny_sig_idx_map_entry **retained_signature_indices,
                     size_t *recursive_count) {
  size_t count = cg->fun_sigs.len;
  ny_idx_list *edges = calloc(count, sizeof(*edges));
  bool *enabled = calloc(count, sizeof(*enabled));
  bool *recursive = calloc(count, sizeof(*recursive));
  ny_sig_idx_map_entry *indices =
      ny_build_fun_sig_index_map(cg->fun_sigs.data, count);
  bool valid = edges && enabled && recursive && indices;
  const char *saved_module = cg->current_module_name;
  if (valid) {
    for (size_t i = 0; i < count; ++i) {
      fun_sig *sig = &cg->fun_sigs.data[i];
      if (!sig->stmt_t || sig->stmt_t->kind != NY_S_FUNC || sig->is_extern ||
          ny_is_stdlib_tok(sig->stmt_t->tok))
        continue;
      enabled[i] = true;
      cg->current_module_name = ny_module_prefix_stable(cg, sig->name);
      ny_sig_ptr_list calls = {0};
      ny_collect_direct_calls_for_sig(cg, sig, &calls);
      cg->current_module_name = saved_module;
      for (size_t j = 0; j < calls.len; ++j) {
        long callee =
            ny_lookup_fun_sig_index_map(indices, count, calls.data[j]);
        if (callee >= 0)
          vec_push(&edges[i], (size_t)callee);
      }
      ny_idx_list_sort_unique(&edges[i]);
      vec_free(&calls);
    }
    valid = ny_mark_recursive_components(edges, enabled, count, recursive,
                                         recursive_count, components);
    if (valid) {
      for (size_t i = 0; i < count; ++i) {
        fun_sig *sig = &cg->fun_sigs.data[i];
        sig->is_recursive = recursive[i];
        sema_func_t *sema = ny_sig_func_sema(sig);
        if (sema)
          sema->is_recursive = recursive[i];
      }
    }
  }
  cg->current_module_name = saved_module;
  if (edges)
    for (size_t i = 0; i < count; ++i)
      vec_free(&edges[i]);
  free(edges);
  free(enabled);
  free(recursive);
  if (valid && retained_signature_indices)
    *retained_signature_indices = indices;
  else
    free(indices);
  return valid;
}

bool ny_verify_totality(codegen_t *cg) {
  if (!cg)
    return false;
  bool has_claims = false;
  for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
    const fun_sig *sig = &cg->fun_sigs.data[i];
    if (sig->stmt_t && sig->stmt_t->kind == NY_S_FUNC &&
        sig->stmt_t->as.fn.attr_total)
      has_claims = true;
  }
  if (!has_claims)
    return true;
  size_t *components = malloc(cg->fun_sigs.len * sizeof(*components));
  int8_t *certificates = calloc(cg->fun_sigs.len, sizeof(*certificates));
  ny_sig_idx_map_entry *indices = NULL;
  size_t recursive_count = 0;
  bool graph_known =
      components && certificates &&
      ny_analyze_recursion(cg, components, &indices, &recursive_count);
  for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
    fun_sig *sig = &cg->fun_sigs.data[i];
    if (!sig->stmt_t || sig->stmt_t->kind != NY_S_FUNC ||
        !sig->stmt_t->as.fn.attr_total)
      continue;
    sig->is_total = false;
    bool measure_proved = graph_known;
    if (measure_proved && sig->is_recursive) {
      size_t component = components[i];
      if (component == SIZE_MAX) {
        measure_proved = false;
      } else {
        if (certificates[component] == 0)
          certificates[component] =
              ny_totality_component_proved(cg, component, components, indices)
                  ? 1
                  : -1;
        measure_proved = certificates[component] > 0;
      }
    }
    if (!sig->is_pure || !sig->effects_known || sig->effects != NY_FX_NONE) {
      ny_diag_error(sig->stmt_t->tok,
                    "@total claim refused for '%s': "
                    "function has side effects, "
                    "unknown effects, or is impure",
                    sig->name ? sig->name : "<anon>");
      cg->had_error = 1;
    } else if (!measure_proved) {
      ny_diag_error(sig->stmt_t->tok,
                    "@total claim refused for '%s': "
                    "recursive termination is not "
                    "certified by a checked well-founded "
                    "measure on every cycle edge",
                    sig->name ? sig->name : "<anon>");
      cg->had_error = 1;
    } else {
      sig->is_total = true;
    }
  }
  free(indices);
  free(components);
  free(certificates);
  return !cg->had_error;
}

static bool ny_func_decl_is_pure(codegen_t *cg, stmt_t *fn_stmt) {
  if (!cg || !fn_stmt || fn_stmt->kind != NY_S_FUNC || !fn_stmt->as.fn.body)
    return false;
  if (!fn_stmt->as.fn.name || !*fn_stmt->as.fn.name)
    return false;
  if (fn_stmt->as.fn.attr_thread || fn_stmt->as.fn.attr_naked)
    return false;
  assigned_name_list local_names = {0};
  assigned_hash_list local_hashes = {0};
  uint64_t local_bloom[4] = {0, 0, 0, 0};
  if (fn_stmt->as.fn.params.len > 0 && fn_stmt->as.fn.params.data != NULL) {
    for (size_t i = 0; i < fn_stmt->as.fn.params.len; i++) {
      const char *pname = fn_stmt->as.fn.params.data[i].name;
      if (pname && *pname) {
        assigned_name_add(&local_names, &local_hashes, local_bloom, pname);
      }
    }
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
  if (!fn_stmt->as.fn.name || !*fn_stmt->as.fn.name)
    return false;
  if (fn_stmt->as.fn.attr_thread || fn_stmt->as.fn.attr_naked)
    return false;
  assigned_name_list local_names = {0};
  assigned_hash_list local_hashes = {0};
  uint64_t local_bloom[4] = {0, 0, 0, 0};
  if (fn_stmt->as.fn.params.len > 0 && fn_stmt->as.fn.params.data != NULL) {
    for (size_t i = 0; i < fn_stmt->as.fn.params.len; i++) {
      const char *pname = fn_stmt->as.fn.params.data[i].name;
      if (pname && *pname) {
        assigned_name_add(&local_names, &local_hashes, local_bloom, pname);
      }
    }
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

/*
 * The environment switch is useful for narrow
 * repros; the decision-trace tier (-vvvv)
 * exposes per-call and per-lambda effect
 * reasoning alongside optimizer heuristics.
 */
static bool ny_effect_diag_verbose_enabled(void) {
  return verbose_enabled >= 4 || ny_env_enabled("NYTRIX_EFFECT_DIAG_VERBOSE");
}

#define NY_FOREACH_FUNC_SIG(cg, sig_var)                                       \
  for (size_t _ny_i = 0; _ny_i < (cg)->fun_sigs.len; _ny_i++)                  \
    for (fun_sig *sig_var = &(cg)->fun_sigs.data[_ny_i]; sig_var != NULL;      \
         sig_var = NULL)                                                       \
      if (!sig_var->stmt_t || sig_var->stmt_t->kind != NY_S_FUNC) {            \
      } else

#define NY_FOREACH_NON_STD_FUNC_SIG(cg, sig_var)                               \
  for (size_t _ny_i = 0; _ny_i < (cg)->fun_sigs.len; _ny_i++)                  \
    for (fun_sig *sig_var = &(cg)->fun_sigs.data[_ny_i]; sig_var != NULL;      \
         sig_var = NULL)                                                       \
      if (!sig_var->stmt_t || sig_var->stmt_t->kind != NY_S_FUNC ||            \
          ny_is_stdlib_tok(sig_var->stmt_t->tok)) {                            \
      } else

#define NY_FOREACH_POLICY_SIG(cg, include_std_flag, sig_var)                   \
  for (size_t _ny_i = 0; _ny_i < (cg)->fun_sigs.len; _ny_i++)                  \
    for (fun_sig *sig_var = &(cg)->fun_sigs.data[_ny_i]; sig_var != NULL;      \
         sig_var = NULL)                                                       \
      if (!sig_var->stmt_t || sig_var->stmt_t->kind != NY_S_FUNC ||            \
          (!(include_std_flag) &&                                              \
           (ny_sig_is_std_for_effects(sig_var) ||                              \
            (sig_var->stmt_t->tok.filename &&                                  \
             strcmp(sig_var->stmt_t->tok.filename, "<stdlib>") == 0)))) {      \
      } else

static bool ny_apply_infer_pass_to_sig(codegen_t *cg, size_t sig_idx,
                                       ny_infer_pass_kind_t pass) {
  if (!cg || sig_idx >= cg->fun_sigs.len)
    return false;
  fun_sig *sig = &cg->fun_sigs.data[sig_idx];
  if (!sig->stmt_t || sig->stmt_t->kind != NY_S_FUNC)
    return false;
  sema_func_t *sema = ny_sig_func_sema(sig);
  if (!sema)
    return false;

  if (verbose_enabled >= 3 && ny_dbg_function_selected(sig->name)) {
    fprintf(stderr, "[*] Purity: analyzing %s (pass=%d)\n", sig->name,
            (int)pass);
  }
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
    bool effects_known = false;
    uint32_t effects = ny_func_decl_effects(cg, sig->stmt_t, &effects_known);
    if (sema->effects_known != effects_known || sema->effects != effects) {
      sema->effects_known = effects_known;
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
    for (size_t i = 0; i < cg->fun_sigs.len; i++) {
      fun_sig *sig = &cg->fun_sigs.data[i];
      if (!sig->stmt_t || sig->stmt_t->kind != NY_S_FUNC ||
          ny_is_stdlib_tok(sig->stmt_t->tok))
        continue;
      if (ny_apply_infer_pass_to_sig(cg, i, pass))
        changed = true;
    }
    if (!changed)
      break;
  }
}

void infer_pure_functions(codegen_t *cg) {
  if (!cg || !cg->auto_purity_infer)
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
    if (ny_sig_is_std_for_effects(sig))
      continue;
    has_functions = true;
    sema_func_t *sema = ny_sig_func_sema(sig);
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
  size_t effect_async_lowered_count = 0;
  size_t effect_contract_violations = 0;
  size_t effect_contract_unknown = 0;
  size_t total_count = 0;
  size_t recursive_count = 0;
  size_t effect_policy_violations = 0;
  size_t effect_policy_unknown = 0;
  size_t alias_policy_violations = 0;
  size_t alias_policy_unknown = 0;
  bool effect_async_global_lowering =
      ny_env_enabled("NYTRIX_EFFECT_ASYNC_LOWERING");
  NY_FOREACH_FUNC_SIG(cg, sig) {
    if (ny_sig_is_std_for_effects(sig)) {
      sig->is_pure = false;
      sig->is_memo_safe = false;
      sig->args_escape = true;
      sig->args_mutated = true;
      sig->returns_alias = true;
      sig->effects = NY_FX_ALL;
      sig->effects_known = true;
      continue;
    }
    sema_func_t *sema = ny_sig_func_sema(sig);
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
      if (ny_sig_has_polymorphic_effect_contract(sig)) {
        /*
         * An open row admits effects supplied
         * by its row variable; it does not
         * erase concrete effects found in this
         * function body. When the body summary
         * is known, check those labels against
         * the annotation's fixed builtin
         * portion. Unknown callback effects
         * remain represented by the open row
         * and are substituted at call sites.
         */
        uint32_t declared_mask = sig->stmt_t->as.fn.effect_contract_mask;
        uint32_t undeclared = effects_known ? effects & ~declared_mask : 0;
        if (undeclared != 0) {
          char declared_buf[96];
          char inferred_buf[96];
          ny_effect_mask_to_buf(declared_mask, declared_buf,
                                sizeof(declared_buf));
          ny_effect_mask_to_buf(effects, inferred_buf, sizeof(inferred_buf));
          ny_diag_error(sig->stmt_t->tok,
                        "effect contract violation in "
                        "'%s': declared @effects(%s) but "
                        "inferred direct effects=%s "
                        "outside the open row",
                        sig->name ? sig->name : "<anon>", declared_buf,
                        inferred_buf);
          cg->had_error = 1;
          effect_contract_violations++;
        }
        sig->effects = effects;
      } else {
        uint32_t declared_mask = sig->stmt_t->as.fn.effect_contract_mask;
        /*
         * An explicit all-effects contract is a
         * known upper bound for an indirect
         * callback whose implementation is
         * unavailable here.
         */
        if (!effects_known && !sig->stmt_t->as.fn.attr_pure &&
            declared_mask == NY_FX_ALL) {
          effects = NY_FX_ALL;
          effects_known = true;
          sig->effects = effects;
          sig->effects_known = true;
          if (sema) {
            sema->effects = effects;
            sema->effects_known = true;
          }
        }
        if (!effects_known) {
          ny_diag_error(sig->stmt_t->tok,
                        "effect contract violation in "
                        "'%s': inferred effects are "
                        "unknown",
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
                            "effect contract violation "
                            "in '%s': declared @pure "
                            "but inferred effects=%s",
                            sig->name ? sig->name : "<anon>", inferred_buf);
            } else {
              ny_diag_error(sig->stmt_t->tok,
                            "effect contract violation "
                            "in '%s': declared "
                            "@effects(%s) but inferred "
                            "effects=%s",
                            sig->name ? sig->name : "<anon>", declared_buf,
                            inferred_buf);
            }
            cg->had_error = 1;
            effect_contract_violations++;
          }
        }
      }
    }
    bool can_effect_async_lower =
        effects_known && (effects & NY_FX_IO) != 0 &&
        ny_sig_can_effect_async_lower(sig, effect_async_global_lowering);
    if (sig->stmt_t->as.fn.attr_async_effects && !can_effect_async_lower) {
      ny_diag_error(sig->stmt_t->tok,
                    "@async_effects requires a known IO effect, a supported "
                    "thread-safe return type, and the normal function ABI");
      cg->had_error = 1;
    }
    if (can_effect_async_lower) {
      sig->stmt_t->as.fn.attr_async_effects_lowered = true;
      sig->stmt_t->as.fn.attr_noinline = true;
      sig->stmt_t->as.fn.attr_cold = true;
      sig->effects = effects;
      if (sema) {
        sema->effects = effects;
        sema->effects_known = true;
      }
      effect_async_lowered_count++;
    }
    char reads[32], writes[32], effect_buf[96];
    snprintf(reads, sizeof(reads), "%s",
             effects & NY_FX_READ_MEMORY ? "memory" : "-");
    snprintf(writes, sizeof(writes), "%s",
             effects & NY_FX_WRITE_MEMORY ? "memory" : "-");
    ny_effect_mask_to_buf(effects, effect_buf, sizeof(effect_buf));
    ny_dbg_effect_row(sig->name ? sig->name : "<anonymous>", reads, writes,
                      (effects & NY_FX_ALLOC) != 0);
    NY_DBG(NY_DBG_EFFECT,
           "effect lattice %-24s known=%s "
           "mask={%s}",
           sig->name ? sig->name : "<anonymous>", effects_known ? "yes" : "no",
           effect_buf);
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
  if (need_recursion)
    (void)ny_analyze_recursion(cg, NULL, NULL, &recursive_count);
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
        bool symbolic_effect_contract =
            ny_sig_has_polymorphic_effect_contract(sig);
        if ((require_known_effects && !symbolic_effect_contract) ||
            forbid_effects != NY_FX_NONE) {
          ny_diag_error(sig->stmt_t->tok,
                        "effect policy violation in "
                        "'%s': effects are unknown",
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
        ny_diag_error(sig->stmt_t->tok,
                      "effect policy violation in '%s': "
                      "effects=%s forbidden=%s",
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
      sema_func_t *sema = ny_sig_func_sema(sig);
      bool escape_known = sema && sema->escape_known;
      if (!escape_known) {
        if (require_known_alias || require_no_alias_escape) {
          ny_diag_error(sig->stmt_t->tok,
                        "alias policy violation in '%s': "
                        "escape/alias facts are unknown",
                        sig->name ? sig->name : "<anon>");
          cg->had_error = 1;
          alias_policy_unknown++;
        }
        continue;
      }
      if (require_no_alias_escape && (sig->args_escape || sig->returns_alias)) {
        ny_diag_error(sig->stmt_t->tok,
                      "alias policy violation in '%s': "
                      "args_escape=%s returns_alias=%s",
                      sig->name ? sig->name : "<anon>",
                      sig->args_escape ? "yes" : "no",
                      sig->returns_alias ? "yes" : "no");
        cg->had_error = 1;
        alias_policy_violations++;
      }
    }
  }
  if (ny_env_enabled("NYTRIX_PURITY_DIAG")) {
    fprintf(stderr,
            "[purity] functions=%zu pure=%zu "
            "memo_safe=%zu "
            "args_escape=%zu args_mutated=%zu "
            "returns_alias=%zu recursive=%zu\n",
            total_count, pure_count, memo_safe_count, args_escape_count,
            args_mutated_count, returns_alias_count, recursive_count);
  }
  if (ny_env_enabled("NYTRIX_EFFECT_DIAG")) {
    ny_effect_diag_log("summary",
                       "functions=%zu none=%zu io=%zu "
                       "alloc=%zu ffi=%zu "
                       "thread=%zu unknown=%zu",
                       total_count, effects_none_count, effects_io_count,
                       effects_alloc_count, effects_ffi_count,
                       effects_thread_count, effects_unknown_count);
    ny_effect_diag_log("async", "lowered=%zu mode=%s",
                       effect_async_lowered_count,
                       effect_async_global_lowering ? "global" : "attribute");
  }
  if (ny_effect_diag_verbose_enabled()) {
    NY_FOREACH_POLICY_SIG(cg, include_std_in_policy, sig) {
      char fx_buf[96];
      ny_effect_mask_to_buf(sig->effects_known ? sig->effects : NY_FX_ALL,
                            fx_buf, sizeof(fx_buf));
      ny_effect_diag_log(
          "function",
          "name=%s effects=%s known=%s pure=%s "
          "memo_safe=%s args_escape=%s "
          "args_mutated=%s returns_alias=%s "
          "recursive=%s",
          sig->name ? sig->name : "<anon>", fx_buf,
          sig->effects_known ? "yes" : "no", sig->is_pure ? "yes" : "no",
          sig->is_memo_safe ? "yes" : "no", sig->args_escape ? "yes" : "no",
          sig->args_mutated ? "yes" : "no", sig->returns_alias ? "yes" : "no",
          sig->is_recursive ? "yes" : "no");
    }
  }
  if (ny_env_enabled("NYTRIX_EFFECT_DIAG")) {
    char forbid_buf[96];
    ny_effect_mask_to_buf(forbid_effects, forbid_buf, sizeof(forbid_buf));
    ny_effect_diag_log("policy",
                       "active=%s forbid=%s include_std=%s "
                       "violations=%zu unknown=%zu",
                       effect_policy_active ? "yes" : "no", forbid_buf,
                       include_std_in_policy ? "yes" : "no",
                       effect_policy_violations, effect_policy_unknown);
  }
  if (ny_env_enabled("NYTRIX_ALIAS_DIAG")) {
    fprintf(stderr,
            "[alias] functions=%zu "
            "args_escape=%zu args_mutated=%zu "
            "returns_alias=%zu\n",
            total_count, args_escape_count, args_mutated_count,
            returns_alias_count);
  }
  if (ny_env_enabled("NYTRIX_ALIAS_DIAG")) {
    fprintf(stderr,
            "[alias.policy] active=%s "
            "require_known=%s require_no_escape=%s "
            "include_std=%s violations=%zu "
            "unknown=%zu\n",
            alias_policy_active ? "yes" : "no",
            require_known_alias ? "yes" : "no",
            require_no_alias_escape ? "yes" : "no",
            include_std_in_alias_policy ? "yes" : "no", alias_policy_violations,
            alias_policy_unknown);
  }
  if (ny_env_enabled("NYTRIX_EFFECT_DIAG")) {
    ny_effect_diag_log("contract", "violations=%zu unknown=%zu",
                       effect_contract_violations, effect_contract_unknown);
  }
}

#undef NY_FOREACH_POLICY_SIG
#undef NY_FOREACH_NON_STD_FUNC_SIG
#undef NY_FOREACH_FUNC_SIG
