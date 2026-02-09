#include "base/util.h"
#include "priv.h"
#include <alloca.h>
#include <llvm-c/Core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t align_up_size(size_t value, size_t align) {
  if (align == 0)
    return value;
  size_t rem = value % align;
  return rem == 0 ? value : value + (align - rem);
}

static bool layout_has_field(layout_def_t *def, const char *name) {
  if (!def || !name)
    return false;
  for (size_t i = 0; i < def->fields.len; i++) {
    if (def->fields.data[i].name &&
        strcmp(def->fields.data[i].name, name) == 0) {
      return true;
    }
  }
  return false;
}

static bool layout_add_field(codegen_t *cg, layout_def_t *def,
                             layout_field_t *field, token_t tok,
                             size_t *offset, size_t *max_align) {
  if (!cg || !def || !field || !field->name || !field->type_name)
    return false;
  if (layout_has_field(def, field->name)) {
    ny_diag_error(tok, "duplicate field '%s' in layout '%s'", field->name,
                  def->name ? def->name : "<anon>");
    cg->had_error = 1;
    return false;
  }
  type_layout_t tl = resolve_raw_layout(cg, field->type_name, tok);
  if (!tl.is_valid)
    return false;
  if (tl.size == 0) {
    ny_diag_error(tok, "layout field '%s' has zero-sized type '%s'",
                  field->name, field->type_name);
    cg->had_error = 1;
    return false;
  }
  size_t align = tl.align ? tl.align : 1;
  if (field->width > 0)
    align = (size_t)field->width;
  if (def->pack > 0 && align > def->pack)
    align = def->pack;
  *offset = align_up_size(*offset, align);
  layout_field_info_t info = {field->name, field->type_name, *offset, tl.size,
                              align};
  vec_push(&def->fields, info);
  *offset += tl.size;
  if (align > *max_align)
    *max_align = align;
  return true;
}

static void emit_trace_func(codegen_t *cg, const char *name) {
  if (!cg || !cg->trace_exec || !cg->builder || !name)
    return;
  fun_sig *ts = lookup_fun(cg, "__trace_func");
  if (!ts)
    return;
  LLVMValueRef nstr_g = const_string_ptr(cg, name, strlen(name));
  LLVMValueRef nstr = LLVMBuildLoad2(cg->builder, cg->type_i64, nstr_g, "");
  LLVMBuildCall2(cg->builder, ts->type, ts->value, &nstr, 1, "");
}

static void register_layout_def(codegen_t *cg, stmt_t *s, bool is_layout) {
  const char *name = is_layout ? s->as.layout.name : s->as.struc.name;
  ny_layout_field_list *fields =
      is_layout ? &s->as.layout.fields : &s->as.struc.fields;
  size_t align_override =
      is_layout ? s->as.layout.align_override : s->as.struc.align_override;
  size_t pack = is_layout ? s->as.layout.pack : s->as.struc.pack;
  if (!name || !fields)
    return;

  for (size_t i = 0; i < cg->layouts.len; i++) {
    layout_def_t *def = cg->layouts.data[i];
    if (def && def->name && strcmp(def->name, name) == 0) {
      ny_diag_error(s->tok, "redefinition of layout '%s'", name);
      ny_diag_note_tok(def->stmt->tok, "previous definition here");
      cg->had_error = 1;
      return;
    }
  }

  layout_def_t *def = arena_alloc(cg->arena, sizeof(layout_def_t));
  memset(def, 0, sizeof(layout_def_t));
  def->name = ny_strdup(name);
  def->stmt = s;
  def->is_layout = is_layout;
  def->align_override = align_override;
  def->pack = pack;

  size_t offset = 0;
  size_t max_align = 1;
  for (size_t i = 0; i < fields->len; i++) {
    layout_field_t *field = &fields->data[i];
    layout_add_field(cg, def, field, s->tok, &offset, &max_align);
  }

  size_t effective_align = max_align;
  if (def->pack > 0 && def->pack < effective_align)
    effective_align = def->pack;
  if (def->align_override > 0) {
    if (def->align_override < effective_align) {
      ny_diag_error(s->tok,
                    "layout '%s' align(%zu) is smaller than field alignment",
                    def->name ? def->name : "<anon>", def->align_override);
      cg->had_error = 1;
    } else {
      effective_align = def->align_override;
    }
  }
  def->align = effective_align;
  def->size = align_up_size(offset, def->align);
  s->sema = def;
  vec_push(&cg->layouts, def);
}

void gen_func(codegen_t *cg, stmt_t *fn, const char *name, scope *scopes,
              size_t depth, binding_list *captures) {
  if (!fn->as.fn.body)
    return;
  LLVMValueRef f = LLVMGetNamedFunction(cg->module, name);
  if (!f) {
    size_t n_params = fn->as.fn.params.len;
    sema_func_t *sema = (sema_func_t *)fn->sema;
    // If captures pointer is non-null, this is a closure/lambda context, so we
    // MUST accept 'env' param.
    size_t total_args = captures ? n_params + 1 : n_params;
    LLVMTypeRef *pt = alloca(sizeof(LLVMTypeRef) * total_args);
    if (captures) {
      pt[0] = cg->type_i64; // Environment pointer type
    }
    for (size_t i = 0; i < n_params; i++) {
      LLVMTypeRef pty = cg->type_i64;
      if (sema && i < sema->resolved_param_types.len)
        pty = sema->resolved_param_types.data[i];
      pt[i + (captures ? 1 : 0)] = pty;
    }
    LLVMTypeRef rty = cg->type_i64;
    if (sema)
      rty = sema->resolved_return_type;
    LLVMTypeRef ft = LLVMFunctionType(rty, pt, (unsigned)total_args, 0);
    f = LLVMAddFunction(cg->module, name, ft);
    // Store explicit params count for callers
    fun_sig sig = {.name = ny_strdup(name),
                   .type = ft,
                   .value = f,
                   .stmt_t = fn,
                   .arity = (int)n_params,
                   .is_variadic = fn->as.fn.is_variadic,
                   .is_extern = false,
                   .link_name = NULL,
                   .return_type = fn->as.fn.return_type
                                      ? ny_strdup(fn->as.fn.return_type)
                                      : NULL};
    vec_push(&cg->fun_sigs, sig);
  } else {
    // Overwrite: remove existing basic blocks if any
    LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(f);
    while (bb) {
      LLVMBasicBlockRef next = LLVMGetNextBasicBlock(bb);
      LLVMDeleteBasicBlock(bb);
      bb = next;
    }
  }
  LLVMBasicBlockRef cur = LLVMGetInsertBlock(cg->builder);
  LLVMPositionBuilderAtEnd(cg->builder, LLVMAppendBasicBlock(f, "entry"));
  LLVMMetadataRef prev_scope = cg->di_scope;
  if (cg->debug_symbols && cg->di_builder) {
    LLVMMetadataRef sp = codegen_debug_subprogram(cg, f, name, fn->tok);
    if (sp)
      cg->di_scope = sp;
  }
  ny_dbg_loc(cg, fn->tok);
  emit_trace_func(cg, name ? name : "<anon>");
  size_t fd = depth + 1;
  size_t root = fd;
  // Init scope
  scopes[fd].vars.len = scopes[fd].vars.cap = 0;
  scopes[fd].vars.data = NULL;
  scopes[fd].defers.len = scopes[fd].defers.cap = 0;
  scopes[fd].defers.data = NULL;
  scopes[fd].break_bb = NULL;
  scopes[fd].continue_bb = NULL;
  size_t param_offset = 0;
  if (captures) {
    param_offset = 1;
    LLVMValueRef env_arg = LLVMGetParam(f, 0);
    LLVMValueRef env_raw = LLVMBuildIntToPtr(
        cg->builder, env_arg, LLVMPointerType(cg->type_i64, 0), "env_raw");
    for (size_t i = 0; i < captures->len; i++) {
      LLVMValueRef src = LLVMBuildGEP2(
          cg->builder, cg->type_i64, env_raw,
          (LLVMValueRef[]){LLVMConstInt(cg->type_i64, (uint64_t)i, false)}, 1,
          "");
      LLVMValueRef val = LLVMBuildLoad2(cg->builder, cg->type_i64, src, "");
      // For closures, we copy captures into local variables of the new scope
      // Note: Bind to the captured name
      LLVMValueRef lv = build_alloca(cg, captures->data[i].name, cg->type_i64);
      LLVMBuildStore(cg->builder, val, lv);
      bind(scopes, fd, captures->data[i].name, lv, fn, true,
           captures->data[i].type_name);
      // Mark as used in the closure's local scope since they were implicitly
      // captured
      scopes[fd].vars.data[scopes[fd].vars.len - 1].is_used = true;
    }
  }
  for (size_t i = 0; i < fn->as.fn.params.len; i++) {
    LLVMValueRef a =
        build_alloca(cg, fn->as.fn.params.data[i].name, cg->type_i64);
    LLVMBuildStore(cg->builder, LLVMGetParam(f, (unsigned)(i + param_offset)),
                   a);
    bind(scopes, fd, fn->as.fn.params.data[i].name, a, fn, true,
         fn->as.fn.params.data[i].type);
  }
  size_t old_root = cg->func_root_idx;
  cg->func_root_idx = root;

  // Infer current module name from function name
  const char *prev_mod = cg->current_module_name;
  char *temp_mod = NULL;
  const char *last_dot = strrchr(name, '.');
  if (last_dot) {
    size_t len = last_dot - name;
    temp_mod = malloc(len + 1);
    memcpy(temp_mod, name, len);
    temp_mod[len] = '\0';
    cg->current_module_name = temp_mod;
  }

  const char *prev_ret = cg->current_fn_ret_type;
  cg->current_fn_ret_type = fn->as.fn.return_type;
  gen_stmt(cg, scopes, &fd, fn->as.fn.body, root, true);
  cg->current_fn_ret_type = prev_ret;

  cg->current_module_name = prev_mod;
  if (temp_mod)
    free(temp_mod);

  cg->func_root_idx = old_root;
  if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
    LLVMBuildRet(cg->builder, LLVMConstInt(cg->type_i64, 1, false));
  scope_pop(scopes, &fd);
  cg->di_scope = prev_scope;
  if (cur)
    LLVMPositionBuilderAtEnd(cg->builder, cur);
}

void collect_sigs(codegen_t *cg, stmt_t *s) {
  if (s->kind == NY_S_FUNC) {
    sema_func_t *sema_func = arena_alloc(cg->arena, sizeof(sema_func_t));
    memset(sema_func, 0, sizeof(sema_func_t));
    sema_func->resolved_return_type =
        resolve_type_name(cg, s->as.fn.return_type, s->tok);
    for (size_t j = 0; j < s->as.fn.params.len; j++) {
      LLVMTypeRef param_ty =
          resolve_type_name(cg, s->as.fn.params.data[j].type, s->tok);
      vec_push(&sema_func->resolved_param_types, param_ty);
    }
    s->sema = (void *)sema_func;

    LLVMTypeRef *pt =
        alloca(sizeof(LLVMTypeRef) * sema_func->resolved_param_types.len);
    for (size_t j = 0; j < sema_func->resolved_param_types.len; j++)
      pt[j] = sema_func->resolved_param_types.data[j];
    LLVMTypeRef ft =
        LLVMFunctionType(sema_func->resolved_return_type, pt,
                         (unsigned)sema_func->resolved_param_types.len, 0);
    const char *final_name =
        codegen_qname(cg, s->as.fn.name, cg->current_module_name);

    LLVMValueRef f = LLVMGetNamedFunction(cg->module, final_name);
    if (!f)
      f = LLVMAddFunction(cg->module, final_name, ft);
    LLVMSetAlignment(f, 16);
    fun_sig sig = {.name = ny_strdup(final_name),
                   .type = ft,
                   .value = f,
                   .stmt_t = s,
                   .arity = (int)s->as.fn.params.len,
                   .is_variadic = s->as.fn.is_variadic,
                   .is_extern = false,
                   .link_name = NULL,
                   .return_type = s->as.fn.return_type
                                      ? ny_strdup(s->as.fn.return_type)
                                      : NULL};
    vec_push(&cg->fun_sigs, sig);
  } else if (s->kind == NY_S_EXTERN) {
    sema_func_t *sema_func = arena_alloc(cg->arena, sizeof(sema_func_t));
    memset(sema_func, 0, sizeof(sema_func_t));
    sema_func->resolved_return_type =
        resolve_abi_type_name(cg, s->as.ext.return_type, s->tok);
    for (size_t j = 0; j < s->as.ext.params.len; j++) {
      LLVMTypeRef param_ty =
          resolve_abi_type_name(cg, s->as.ext.params.data[j].type, s->tok);
      vec_push(&sema_func->resolved_param_types, param_ty);
    }
    s->sema = (void *)sema_func;

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
    const char *ln = s->as.ext.link_name ? s->as.ext.link_name : final_name;
    LLVMValueRef f = LLVMGetNamedFunction(cg->module, ln);
    if (!f)
      f = LLVMAddFunction(cg->module, ln, ft);
    fun_sig sig = {
        .name = ny_strdup(final_name),
        .type = ft,
        .value = f,
        .stmt_t = s,
        .arity = (int)param_count,
        .is_variadic = s->as.ext.is_variadic,
        .is_extern = true,
        .link_name =
            s->as.ext.link_name ? ny_strdup(s->as.ext.link_name) : NULL,
        .return_type =
            s->as.ext.return_type ? ny_strdup(s->as.ext.return_type) : NULL};
    vec_push(&cg->fun_sigs, sig);
  } else if (s->kind == NY_S_VAR) {
    for (size_t j = 0; j < s->as.var.names.len; j++) {
      const char *n = s->as.var.names.data[j];
      const char *final_name = codegen_qname(cg, n, cg->current_module_name);
      // Use simple exact lookup here to see if we already created this global
      bool found = false;
      for (size_t k = 0; k < cg->global_vars.len; k++) {
        if (strcmp(cg->global_vars.data[k].name, final_name) == 0) {
          found = true;
          break;
        }
      }
      if (!found) {
        LLVMValueRef g = LLVMAddGlobal(cg->module, cg->type_i64, final_name);
        LLVMSetInitializer(g, LLVMConstInt(cg->type_i64, 0, false));
        const char *type_name = NULL;
        if (s->as.var.types.len > j)
          type_name = s->as.var.types.data[j];
        binding b = {ny_strdup(final_name), g,     s, s->as.var.is_mut,
                     false,                false, type_name};
        vec_push(&cg->global_vars, b);
      }
    }
    // Semantic analysis for types
    sema_var_t *sema_var = arena_alloc(cg->arena, sizeof(sema_var_t));
    memset(sema_var, 0, sizeof(sema_var_t));
    for (size_t j = 0; j < s->as.var.types.len; j++) {
      LLVMTypeRef var_ty =
          resolve_type_name(cg, s->as.var.types.data[j], s->tok);
      vec_push(&sema_var->resolved_types, var_ty);
    }
    s->sema = (void *)sema_var;
  } else if (s->kind == NY_S_ENUM) {
    enum_def_t *enu = arena_alloc(cg->arena, sizeof(enum_def_t));
    memset(enu, 0, sizeof(enum_def_t));
    enu->name = ny_strdup(s->as.enu.name);
    enu->stmt = s;
    cg->current_enum_val = 0; // Reset for each new enum

    // Check for duplicate enum definition
    for (size_t i = 0; i < cg->enums.len; i++) {
      if (strcmp(cg->enums.data[i]->name, enu->name) == 0) {
        ny_diag_error(s->tok, "redefinition of enum '%s'", enu->name);
        ny_diag_note_tok(cg->enums.data[i]->stmt->tok,
                         "previous definition here");
        cg->had_error = 1;
        goto end_enum_processing;
      }
    }

    for (size_t i = 0; i < s->as.enu.items.len; i++) {
      stmt_enum_item_t *item = &s->as.enu.items.data[i];
      enum_member_def_t member = {0};
      member.name = ny_strdup(item->name);

      // Check for duplicate member name within this enum
      for (size_t j = 0; j < enu->members.len; j++) {
        if (strcmp(enu->members.data[j].name, member.name) == 0) {
          ny_diag_error(s->tok, "redefinition of enum member '%s' in enum '%s'",
                        member.name, enu->name);
          cg->had_error = 1;
          // Continue to avoid cascading errors, but mark as error
          goto next_enum_member;
        }
      }

      if (item->value) {
        // If explicit value, we need to evaluate it. For now, assume it's a
        // literal. In a real compiler, this would involve a recursive call to
        // gen_expr and ensuring it resolves to a compile-time constant integer.
        if (item->value->kind == NY_E_LITERAL &&
            item->value->as.literal.kind == NY_LIT_INT) {
          member.value = item->value->as.literal.as.i;
          cg->current_enum_val = member.value + 1; // Update for next implicit
        } else {
          ny_diag_error(s->tok,
                        "enum member value must be an integer literal for now");
          cg->had_error = 1;
          member.value = cg->current_enum_val++; // Fallback to implicit
        }
      } else {
        member.value = cg->current_enum_val++;
      }
      vec_push(&enu->members, member);
    next_enum_member:;
    }
    vec_push(&cg->enums, enu);
  end_enum_processing:; // Label for goto in case of error
  } else if (s->kind == NY_S_STRUCT) {
    register_layout_def(cg, s, false);
  } else if (s->kind == NY_S_LAYOUT) {
    register_layout_def(cg, s, true);
  } else if (s->kind == NY_S_MODULE) {
    const char *prev = cg->current_module_name;
    cg->current_module_name = s->as.module.name;
    for (size_t i = 0; i < s->as.module.body.len; i++)
      collect_sigs(cg, s->as.module.body.data[i]);
    cg->current_module_name = prev;
  }
}
