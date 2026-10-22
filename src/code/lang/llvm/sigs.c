#include "base/loader.h"
#include "base/util.h"
#include "code/lang/c/clang.h"
#include "code/lang/code.h"
#include "code/lang/parse/proof.h"
#include "code/lang/priv.h"

#ifdef _WIN32
#include <malloc.h>
#ifndef alloca
#define alloca _alloca
#endif
#else
#include <alloca.h>
#endif
#include <stdlib.h>
#include <string.h>

static char *codegen_strdup(codegen_t *cg, const char *s) {
  if (!s)
    s = "";
  if (cg && cg->arena)
    return arena_strndup(cg->arena, s, strlen(s));
  return ny_strdup(s);
}

static void collect_attached_method_sig(codegen_t *cg, stmt_t *method,
                                        const char *owner_type) {
  if (!cg || !method)
    return;
  collect_sigs(cg, method);
  for (size_t i = 0; i < cg->fun_sigs.len; ++i) {
    fun_sig *sig = &cg->fun_sigs.data[i];
    if (sig->stmt_t == method) {
      sig->is_attached_method = true;
      sig->owner_type = owner_type;
    }
  }
}

static expr_t *collect_var_init_expr(stmt_t *s, size_t idx) {
  if (!s || s->kind != NY_S_VAR || s->as.var.exprs.len == 0)
    return NULL;
  if (s->as.var.exprs.len == s->as.var.names.len && idx < s->as.var.exprs.len)
    return s->as.var.exprs.data[idx];
  return s->as.var.exprs.data[0];
}

static bool collect_expr_is_floatish(codegen_t *cg, expr_t *e) {
  if (!e)
    return false;
  switch (e->kind) {
  case NY_E_LITERAL:
    return e->as.literal.kind == NY_LIT_FLOAT;
  case NY_E_CALL:
    if (e->as.call.callee && e->as.call.callee->kind == NY_E_IDENT) {
      const char *name = e->as.call.callee->as.ident.name;
      if (name &&
          (strcmp(name, "float") == 0 || strcmp(name, "to_float") == 0)) {
        fun_sig *sig = lookup_fun(cg, name, e->as.call.callee->as.ident.hash);
        if (sig && sig->stmt_t && sig->stmt_t->kind == NY_S_FUNC)
          return sig->return_type && (strcmp(sig->return_type, "f64") == 0 ||
                                      strcmp(sig->return_type, "float") == 0);
        return true;
      }
    }
    return false;
  case NY_E_BINARY:
    if (!e->as.binary.op || (strcmp(e->as.binary.op, "+") != 0 &&
                             strcmp(e->as.binary.op, "-") != 0 &&
                             strcmp(e->as.binary.op, "*") != 0 &&
                             strcmp(e->as.binary.op, "/") != 0))
      return false;
    return collect_expr_is_floatish(cg, e->as.binary.left) ||
           collect_expr_is_floatish(cg, e->as.binary.right);
  default:
    return false;
  }
}

void collect_sigs(codegen_t *cg, stmt_t *s) {
  if (!cg || !s)
    return;
  if (s->kind == NY_S_FUNC) {
    sema_func_t *sema_func = arena_alloc(cg->arena, sizeof(sema_func_t));
    memset(sema_func, 0, sizeof(sema_func_t));
    bool use_native_abi = ny_fn_uses_native_abi(cg, s);
    sema_func->resolved_return_type = ny_fn_resolve_user_abi_type(
        cg, s, use_native_abi, s->as.fn.return_type, s->tok);
    for (size_t j = 0; j < s->as.fn.params.len; j++) {
      const char *ptype = s->as.fn.params.data[j].type;
      LLVMTypeRef param_ty =
          ny_fn_resolve_user_abi_type(cg, s, use_native_abi, ptype, s->tok);
      vec_push_arena(cg->arena, &sema_func->resolved_param_types, param_ty);
    }
    s->sema = (void *)sema_func;
    s->sema_kind = NY_STMT_SEMA_FUNC;
    LLVMTypeRef *pt =
        alloca(sizeof(LLVMTypeRef) * sema_func->resolved_param_types.len);
    for (size_t j = 0; j < sema_func->resolved_param_types.len; j++)
      pt[j] = sema_func->resolved_param_types.data[j];
    LLVMTypeRef ft =
        LLVMFunctionType(sema_func->resolved_return_type, pt,
                         (unsigned)sema_func->resolved_param_types.len, 0);
    const char *final_name =
        codegen_qname(cg, s->as.fn.name, cg->current_module_name);
    ny_resolve_fn_attrs(cg, s);
    const char *ln = s->as.fn.link_name ? s->as.fn.link_name : final_name;
    LLVMValueRef f = ny_get_named_fn(cg, ln);
    if (!f)
      f = LLVMAddFunction(cg->module, ln, ft);
    ny_apply_decl_fn_attrs(cg, f, s);
    if (ny_stmt_func_body_has_try(s))
      ny_apply_longjmp_fn_attrs(cg, f);
    LLVMSetAlignment(f, 16);
    fun_sig sig;
    ny_fun_sig_init(&sig, final_name, ft, f, s, (int)s->as.fn.params.len,
                    s->as.fn.is_variadic, s->as.fn.is_extern);
    sig.module_name =
        cg->current_module_name ? ny_strdup(cg->current_module_name) : NULL;
    ny_fun_sig_set_params(&sig, &s->as.fn.params);
    sig.is_native_abi = use_native_abi;
    sig.tailcall = s->as.fn.attr_tailcall;
    sig.link_name = s->as.fn.link_name ? ny_strdup(s->as.fn.link_name) : NULL;
    sig.return_type =
        s->as.fn.return_type ? ny_strdup(s->as.fn.return_type) : NULL;
    sig.returns_owned = s->as.fn.attr_returns_owned;
    ny_fun_sig_copy_contracts(&sig, &s->as.fn);
    {
      const char *inferred_types[16] = {0};
      if (!sig.return_type && s->as.fn.params.len <= 16) {
        ny_infer_param_types(s, inferred_types);
        char *inferred_ret = ny_infer_fn_return_type(s, inferred_types);
        sig.inferred_return_type = inferred_ret;
      } else {
        sig.inferred_return_type = NULL;
      }
    }
    vec_push(&cg->fun_sigs, sig);
  } else if (s->kind == NY_S_LEMMA) {
    lemma_def_t *def = arena_alloc(cg->arena, sizeof(*def));
    if (!def)
      return;
    memset(def, 0, sizeof(*def));
    def->name = s->as.lemma.name;
    def->module_name = cg->current_module_name;
    def->proposition = s->as.lemma.proposition;
    def->stmt = s;
    def->proved = ny_proof_check_lemma_definition(cg, s);
    if (def->proved) {
      char *prop_form = ny_proof_type_from_expr(s->as.lemma.proposition);
      def->proposition_hash = ny_hash64_cstr(prop_form ? prop_form : "");
      free(prop_form);
    } else {
      def->proposition_hash = 0;
    }
    for (size_t i = 0; i < cg->registry.lemmas.len; ++i) {
      lemma_def_t *old = cg->registry.lemmas.data[i];
      if (old && old->name && strcmp(old->name, def->name) == 0)
        return;
    }
    vec_push(&cg->registry.lemmas, def);
  } else if (s->kind == NY_S_EXTERN) {
    sema_func_t *sema_func = arena_alloc(cg->arena, sizeof(sema_func_t));
    memset(sema_func, 0, sizeof(sema_func_t));
    sema_func->resolved_return_type =
        resolve_abi_type_name(cg, s->as.ext.return_type, s->tok);
    for (size_t j = 0; j < s->as.ext.params.len; j++) {
      LLVMTypeRef param_ty =
          resolve_abi_type_name(cg, s->as.ext.params.data[j].type, s->tok);
      vec_push_arena(cg->arena, &sema_func->resolved_param_types, param_ty);
    }
    s->sema = (void *)sema_func;
    s->sema_kind = NY_STMT_SEMA_FUNC;
    const char *final_name =
        codegen_qname(cg, s->as.ext.name, cg->current_module_name);
    size_t param_count = s->as.ext.params.len;
    LLVMTypeRef *pt = NULL;
    if (param_count > 0)
      pt = alloca(sizeof(LLVMTypeRef) * param_count);
    for (size_t j = 0; j < param_count; j++)
      pt[j] = sema_func->resolved_param_types.data[j];
    LLVMTypeRef ft = LLVMFunctionType(sema_func->resolved_return_type, pt,
                                      (unsigned)param_count, 0);
    /*
     * Keep the IR declaration distinct from the external linker symbol.
     * A C symbol may legitimately be exposed through multiple typed extern
     * declarations (notably objc_msgSend); LLVM cannot represent those as
     * one declaration with incompatible function types.
     */
    LLVMValueRef f = ny_get_named_fn(cg, final_name);
    if (!f)
      f = LLVMAddFunction(cg->module, final_name, ft);
    fun_sig sig;
    ny_fun_sig_init(&sig, final_name, ft, f, s, (int)param_count,
                    s->as.ext.is_variadic, true);
    sig.module_name =
        cg->current_module_name ? ny_strdup(cg->current_module_name) : NULL;
    ny_fun_sig_set_params(&sig, &s->as.ext.params);
    sig.is_native_abi = true;
    sig.link_name = s->as.ext.link_name ? ny_strdup(s->as.ext.link_name) : NULL;
    sig.llvm_name = ny_strdup(final_name);
    sig.return_type =
        s->as.ext.return_type ? ny_strdup(s->as.ext.return_type) : NULL;
    vec_push(&cg->fun_sigs, sig);
  } else if (s->kind == NY_S_VAR) {
    for (size_t j = 0; j < s->as.var.names.len; j++) {
      const char *n = s->as.var.names.data[j];
      const char *final_name = codegen_qname(cg, n, cg->current_module_name);
      bool found = lookup_global_exact(cg, final_name) != NULL;
      if (!found) {
        const char *type_name = NULL;
        if (s->as.var.types.len > j)
          type_name = s->as.var.types.data[j];
        if (!type_name &&
            collect_expr_is_floatish(cg, collect_var_init_expr(s, j)))
          type_name = "float";
        LLVMTypeRef global_type = cg->type_i64;
        bool global_is_f64 = type_name && (strcmp(type_name, "f64") == 0 ||
                                           strcmp(type_name, "float") == 0);
        bool global_is_f32 = type_name && strcmp(type_name, "f32") == 0;
        if (global_is_f64)
          global_type = cg->type_f64;
        else if (global_is_f32)
          global_type = cg->type_f32;
        LLVMValueRef g = LLVMGetNamedGlobal(cg->module, final_name);
        if (!g) {
          g = LLVMAddGlobal(cg->module, global_type, final_name);
          if (s->tok.module_features & NY_MODULE_FEATURE_STDLIB)
            LLVMSetSection(g, "ny.std");
          else if (s->tok.module_features & NY_MODULE_FEATURE_USER)
            LLVMSetSection(g, "ny.user");
          if ((!cg->current_module_name || !*cg->current_module_name) &&
              !ny_is_stdlib_tok(s->tok))
            LLVMSetLinkage(g, LLVMPrivateLinkage);
          bool define_here = true;
          bool direct_source_global =
              ny_codegen_stmt_is_source_file(cg, s) ||
              ny_codegen_module_is_source_file(cg, cg->current_module_name);
          if (cg->skip_stdlib && ny_is_stdlib_tok(s->tok) &&
              !direct_source_global)
            define_here = false;
          if (cg->emit_module_decls_only)
            define_here = ny_emit_module_match(cg, cg->current_module_name);
          if (define_here) {
            if (!(cg->skip_stdlib && ny_is_stdlib_tok(s->tok) &&
                  !direct_source_global))
              LLVMSetInitializer(g, LLVMConstNull(global_type));
          } else {
            LLVMSetLinkage(g, LLVMExternalLinkage);
          }
        }
        binding b = {0};
        b.name = ny_strdup(final_name);
        b.value = g;
        b.stmt_t = s;
        b.is_slot = true;
        b.is_mut = s->as.var.is_mut;
        b.owned = true;
        b.type_name = type_name;
        b.decl_type_name = type_name;
        b.is_f64_slot = global_is_f64;
        b.is_f32_slot = global_is_f32;
        vec_push(&cg->global_vars, b);
        if (cg->debug_symbols && cg->di_builder)
          codegen_debug_global_variable(cg, final_name, g, type_name, s->tok);
      }
    }
    sema_var_t *sema_var = arena_alloc(cg->arena, sizeof(sema_var_t));
    memset(sema_var, 0, sizeof(sema_var_t));
    for (size_t j = 0; j < s->as.var.types.len; j++) {
      LLVMTypeRef var_ty =
          resolve_type_name(cg, s->as.var.types.data[j], s->tok);
      vec_push_arena(cg->arena, &sema_var->resolved_types, var_ty);
    }
    s->sema = (void *)sema_var;
    s->sema_kind = NY_STMT_SEMA_VAR;
  } else if (s->kind == NY_S_ENUM) {
    enum_def_t *enu = arena_alloc(cg->arena, sizeof(enum_def_t));
    memset(enu, 0, sizeof(enum_def_t));
    enu->name = codegen_strdup(cg, s->as.enu.name);
    enu->stmt = s;
    enu->is_indexed_family = s->as.enu.is_indexed_family;
    enu->index_param_name = s->as.enu.index_param_name
                                ? codegen_strdup(cg, s->as.enu.index_param_name)
                                : NULL;
    enu->adt_tag_base = 200000 + (int64_t)cg->enums.len * 1024;
    for (size_t i = 0; i < s->as.enu.type_params.len; i++)
      vec_push_arena(cg->arena, &enu->type_params,
                     codegen_strdup(cg, s->as.enu.type_params.data[i]));
    cg->current_enum_val = 0;
    for (size_t i = 0; i < cg->enums.len; i++)
      if (strcmp(cg->enums.data[i]->name, enu->name) == 0)
        return;
    for (size_t i = 0; i < s->as.enu.items.len; i++) {
      stmt_enum_item_t *item = &s->as.enu.items.data[i];
      enum_member_def_t member = {0};
      member.name = codegen_strdup(cg, item->name);
      member.has_payload = item->fields.len > 0;
      if (member.has_payload)
        enu->has_payload = true;
      for (size_t j = 0; j < item->fields.len; j++) {
        enum_field_def_t field = {
            .name = codegen_strdup(cg, item->fields.data[j].name),
            .type_name = codegen_strdup(cg, item->fields.data[j].type_name),
        };
        if (field.type_name) {
          const char *arr = strstr(field.type_name, "->");
          if (arr) {
            const char *occ = strstr(field.type_name, enu->name);
            if (occ && occ < arr) {
              ny_diag_error(
                  s->tok,
                  "strict positivity violation: recursive enum '%s' appears in "
                  "negative position in variant '%s' field '%s'",
                  enu->name, member.name, field.name ? field.name : "<anon>");
              cg->had_error = 1;
            }
          }
        }
        for (size_t k = 0; k < member.fields.len; k++)
          if (strcmp(member.fields.data[k].name, field.name) == 0) {
            ny_diag_error(s->tok,
                          "duplicate field '%s' in enum variant '%s.%s'",
                          field.name, enu->name, member.name);
            cg->had_error = 1;
          }
        vec_push_arena(cg->arena, &member.fields, field);
      }
      for (size_t j = 0; j < enu->members.len; j++)
        if (strcmp(enu->members.data[j].name, member.name) == 0) {
          ny_diag_error(s->tok, "redefinition of enum member '%s' in enum '%s'",
                        member.name, enu->name);
          cg->had_error = 1;
          goto next_enum_member;
        }
      if (item->value) {
        if (item->value->kind == NY_E_LITERAL &&
            item->value->as.literal.kind == NY_LIT_INT) {
          member.value = item->value->as.literal.as.i;
          cg->current_enum_val = member.value + 1;
        } else {
          ny_diag_error(s->tok,
                        "enum member value must be an integer literal for now");
          cg->had_error = 1;
          member.value = cg->current_enum_val++;
        }
      } else {
        member.value = cg->current_enum_val++;
      }
      member.runtime_tag =
          member.has_payload ? enu->adt_tag_base + (int64_t)i : member.value;
      vec_push_arena(cg->arena, &enu->members, member);
    next_enum_member:;
    }
    if (enu->has_payload)
      ny_register_tagged_type(cg, enu->name, s->tok);
    s->sema = (void *)enu;
    s->sema_kind = NY_STMT_SEMA_ENUM;
    vec_push(&cg->enums, enu);
  } else if (s->kind == NY_S_STRUCT) {
    ny_register_layout_def(cg, s, false);
    for (size_t i = 0; i < s->as.struc.methods.len; i++)
      collect_attached_method_sig(cg, s->as.struc.methods.data[i],
                                  s->as.struc.name);
  } else if (s->kind == NY_S_LAYOUT) {
    ny_register_layout_def(cg, s, true);
    for (size_t i = 0; i < s->as.layout.methods.len; i++)
      collect_attached_method_sig(cg, s->as.layout.methods.data[i],
                                  s->as.layout.name);
  } else if (s->kind == NY_S_OPERATOR) {
    ny_operator_def_t def = {0};
    def.op = s->as.oper.op;
    def.left_type = s->as.oper.left_type;
    def.right_type = s->as.oper.right_type;
    def.return_type = s->as.oper.return_type;
    def.target_name = s->as.oper.target;
    def.module_name = cg->current_module_name;
    def.stmt = s;
    vec_push(&cg->operators, def);
  } else if (s->kind == NY_S_IMPL) {
    ny_register_tagged_type(cg, s->as.impl.type_name, s->tok);
    for (size_t i = 0; i < s->as.impl.methods.len; i++)
      collect_attached_method_sig(cg, s->as.impl.methods.data[i],
                                  s->as.impl.type_name);
  } else if (s->kind == NY_S_REFINE) {
    refine_def_t *def = arena_alloc(cg->arena, sizeof(*def));
    if (def) {
      memset(def, 0, sizeof(*def));
      def->name = codegen_strdup(cg, s->as.refine.name);
      def->base_type = codegen_strdup(cg, s->as.refine.base_type);
      def->param = codegen_strdup(cg, s->as.refine.param);
      def->predicate = s->as.refine.predicate;
      def->stmt = s;
      for (size_t i = 0; i < cg->refines.len; i++)
        if (cg->refines.data[i] && cg->refines.data[i]->name &&
            strcmp(cg->refines.data[i]->name, def->name) == 0)
          goto skip_refine;
      vec_push(&cg->refines, def);
    }
  skip_refine:;
  } else if (s->kind == NY_S_TRAIT) {
    trait_def_t *def = arena_alloc(cg->arena, sizeof(*def));
    if (def) {
      memset(def, 0, sizeof(*def));
      def->name = codegen_strdup(cg, s->as.trait.name);
      def->methods = s->as.trait.methods;
      def->stmt = s;
      for (size_t i = 0; i < cg->traits.len; i++)
        if (cg->traits.data[i] && cg->traits.data[i]->name &&
            strcmp(cg->traits.data[i]->name, def->name) == 0)
          goto skip_trait;
      vec_push(&cg->traits, def);
    }
  skip_trait:;
  } else if (s->kind == NY_S_MODULE) {
    const char *prev = cg->current_module_name;
    cg->current_module_name = s->as.module.name;
    for (size_t i = 0; i < s->as.module.body.len; i++)
      collect_sigs(cg, s->as.module.body.data[i]);
    cg->current_module_name = prev;
  } else if (s->kind == NY_S_IF) {
    bool truthy = false;
    bool determined = ny_eval_comptime_if(cg, s, &truthy);
    if (determined) {
      if (truthy)
        collect_sigs(cg, s->as.iff.conseq);
      else if (s->as.iff.alt)
        collect_sigs(cg, s->as.iff.alt);
    } else {
      collect_sigs(cg, s->as.iff.conseq);
      if (s->as.iff.alt)
        collect_sigs(cg, s->as.iff.alt);
    }
  } else if (s->kind == NY_S_GUARD) {
    if (s->as.guard.fallback)
      collect_sigs(cg, s->as.guard.fallback);
  } else if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; i++)
      collect_sigs(cg, s->as.block.body.data[i]);
  } else if (s->kind == NY_S_MACRO) {
    collect_sigs(cg, s->as.macro.body);
  } else if (s->kind == NY_S_INCLUDE) {
    if (cg->skip_stdlib && ny_is_stdlib_tok(s->tok) &&
        !ny_env_enabled("NYTRIX_STDBC_IMPORT_FFI"))
      return;
    ny_ffi_clang_import(cg, s->as.inc.path, s->as.inc.prefix, s->as.inc.is_std,
                        s->as.inc.lib);
  } else if (s->kind == NY_S_DEFINE) {
    if (cg->skip_stdlib && ny_is_stdlib_tok(s->tok) &&
        !ny_env_enabled("NYTRIX_STDBC_IMPORT_FFI"))
      return;
    if (s->as.def.value && s->as.def.value[0]) {
      size_t nlen = strlen(s->as.def.name);
      size_t vlen = strlen(s->as.def.value);
      char *buf = malloc(nlen + 1 + vlen + 1);
      memcpy(buf, s->as.def.name, nlen);
      buf[nlen] = '=';
      memcpy(buf + nlen + 1, s->as.def.value, vlen);
      buf[nlen + 1 + vlen] = '\0';
      ny_ffi_clang_define(cg, buf);
      free(buf);
    } else {
      ny_ffi_clang_define(cg, s->as.def.name);
    }
  }
}
