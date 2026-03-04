#include "base/util.h"

#include "llvm.h"
#include "priv.h"
#ifndef _WIN32
#include <alloca.h>
#else
#include <malloc.h>
#endif
#include <ctype.h>
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
                             layout_field_t *field, token_t tok, size_t *offset,
                             size_t *max_align) {
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

static void emit_trace_enter(codegen_t *cg, const char *name, token_t tok) {
  (void)cg;
  (void)name;
  (void)tok;
  /* Tracing disabled - causes crashes during codegen */
}

void ny_cg_emit_trace_exit(codegen_t *cg) {
  (void)cg;
  /* No-op */
}

void add_fn_enum_attr(codegen_t *cg, LLVMValueRef fn, const char *name,
                      uint64_t val) {
  if (!cg || !fn || !name)
    return;
  unsigned kind_id =
      LLVMGetEnumAttributeKindForName(name, (unsigned)strlen(name));
  if (kind_id == 0)
    return;
  LLVMAttributeRef attr = LLVMCreateEnumAttribute(cg->ctx, kind_id, val);
  LLVMAddAttributeAtIndex(fn, LLVMAttributeFunctionIndex, attr);
}

void add_fn_string_attr(codegen_t *cg, LLVMValueRef fn, const char *name,
                        const char *value) {
  if (!cg || !fn || !name || !*name)
    return;
  if (!value)
    value = "";
  LLVMAttributeRef attr = LLVMCreateStringAttribute(
      cg->ctx, name, (unsigned)strlen(name), value, (unsigned)strlen(value));
  LLVMAddAttributeAtIndex(fn, LLVMAttributeFunctionIndex, attr);
}

static inline bool attr_name_eq(const attribute_t *attr, const char *name) {
  if (!attr || !attr->name || !name)
    return false;
  return strcmp(attr->name, name) == 0;
}

static token_t attr_diag_tok(const stmt_t *fn_stmt, const attribute_t *attr,
                             size_t arg_index) {
  if (attr && arg_index < attr->args.len) {
    expr_t *arg = attr->args.data[arg_index];
    if (arg)
      return arg->tok;
  }
  if (attr)
    return attr->tok;
  if (fn_stmt)
    return fn_stmt->tok;
  token_t empty = {0};
  return empty;
}

static bool attr_arg_text_view(expr_t *arg, const char **text, size_t *len) {
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
  // 'none' / 'nil' are tokenized as NY_T_NIL (literal int 0), but in
  // attribute context we want the original keyword text.
  if (arg->kind == NY_E_LITERAL && arg->as.literal.kind == NY_LIT_INT &&
      arg->as.literal.as.i == 0 && arg->tok.lexeme && arg->tok.len > 0) {
    *text = arg->tok.lexeme;
    *len = arg->tok.len;
    return true;
  }
  return false;
}

static bool effect_attr_name_mask(const char *name, size_t len,
                                  uint32_t *mask_out) {
  if (!name || len == 0 || !mask_out)
    return false;
  if (len == 2 && strncmp(name, "io", 2) == 0) {
    *mask_out = NY_FX_IO;
    return true;
  }
  if (len == 5 && strncmp(name, "alloc", 5) == 0) {
    *mask_out = NY_FX_ALLOC;
    return true;
  }
  if (len == 3 && strncmp(name, "ffi", 3) == 0) {
    *mask_out = NY_FX_FFI;
    return true;
  }
  if (len == 6 && strncmp(name, "thread", 6) == 0) {
    *mask_out = NY_FX_THREAD;
    return true;
  }
  if (len == 3 && strncmp(name, "all", 3) == 0) {
    *mask_out = NY_FX_ALL;
    return true;
  }
  if ((len == 4 && strncmp(name, "none", 4) == 0) ||
      (len == 4 && strncmp(name, "pure", 4) == 0)) {
    *mask_out = NY_FX_NONE;
    return true;
  }
  return false;
}

static bool parse_effect_attr_args(codegen_t *cg, const stmt_t *fn_stmt,
                                   const attribute_t *attr,
                                   uint32_t *mask_out) {
  if (!cg || !fn_stmt || !attr || !mask_out)
    return false;
  uint32_t mask = NY_FX_NONE;
  bool saw_any = false;
  for (size_t i = 0; i < attr->args.len; i++) {
    const char *name = NULL;
    size_t len = 0;
    if (!attr_arg_text_view(attr->args.data[i], &name, &len)) {
      ny_diag_error(attr_diag_tok(fn_stmt, attr, i),
                    "expected effect name in @effects(...)");
      ny_diag_hint("supported: io, alloc, ffi, thread, all, none");
      cg->had_error = 1;
      continue;
    }
    uint32_t tok_mask = NY_FX_NONE;
    if (!effect_attr_name_mask(name, len, &tok_mask)) {
      ny_diag_error(attr_diag_tok(fn_stmt, attr, i),
                    "unknown effect name in @effects(...)");
      ny_diag_hint("supported: io, alloc, ffi, thread, all, none");
      cg->had_error = 1;
      continue;
    }
    mask |= tok_mask;
    saw_any = true;
  }
  if (!saw_any) {
    ny_diag_error(attr_diag_tok(fn_stmt, attr, 0),
                  "@effects(...) requires at least one effect name");
    ny_diag_hint("use @pure or @effects(none) for effect-free functions");
    cg->had_error = 1;
  }
  *mask_out = mask;
  return saw_any;
}

static void mark_simple_flag_attr(codegen_t *cg, const stmt_t *fn_stmt,
                                  const attribute_t *attr,
                                  const char *attr_spelling, bool *flag) {
  if (!cg || !fn_stmt || !attr || !attr_spelling || !flag)
    return;
  if (*flag) {
    ny_diag_error(attr_diag_tok(fn_stmt, attr, 0), "duplicate attribute '%s'",
                  attr_spelling);
    cg->had_error = 1;
  }
  if (attr->args.len != 0) {
    ny_diag_error(attr_diag_tok(fn_stmt, attr, 0), "%s does not take arguments",
                  attr_spelling);
    cg->had_error = 1;
  }
  *flag = true;
}

static void resolve_fn_attrs(codegen_t *cg, stmt_t *fn_stmt) {
  if (!cg || !fn_stmt || fn_stmt->kind != NY_S_FUNC)
    return;
  stmt_func_t *decl = &fn_stmt->as.fn;
  if (decl->attrs_resolved)
    return;
  bool is_naked = false;
  bool is_jit = false;
  bool is_thread = false;
  bool is_pure = false;
  bool is_extern = false;
  const char *link_name = NULL;
  bool has_effect_contract = false;
  uint32_t effect_contract_mask = NY_FX_NONE;
  for (size_t i = 0; i < fn_stmt->attributes.len; i++) {
    attribute_t *attr = &fn_stmt->attributes.data[i];
    if (attr_name_eq(attr, "naked")) {
      mark_simple_flag_attr(cg, fn_stmt, attr, "@naked", &is_naked);
      continue;
    }
    if (attr_name_eq(attr, "jit")) {
      mark_simple_flag_attr(cg, fn_stmt, attr, "@jit", &is_jit);
      continue;
    }
    if (attr_name_eq(attr, "thread")) {
      mark_simple_flag_attr(cg, fn_stmt, attr, "@thread", &is_thread);
      continue;
    }
    if (attr_name_eq(attr, "pure")) {
      mark_simple_flag_attr(cg, fn_stmt, attr, "@pure", &is_pure);
      continue;
    }
    if (attr_name_eq(attr, "inline")) {
      mark_simple_flag_attr(cg, fn_stmt, attr, "@inline", &decl->attr_inline);
      continue;
    }
    if (attr_name_eq(attr, "noinline")) {
      mark_simple_flag_attr(cg, fn_stmt, attr, "@noinline",
                            &decl->attr_noinline);
      continue;
    }
    if (attr_name_eq(attr, "extern")) {
      is_extern = true;
      if (attr->args.len > 0) {
        expr_t *arg = attr->args.data[0];
        if (arg->kind == NY_E_LITERAL && arg->as.literal.kind == NY_LIT_STR) {
          link_name = arg->as.literal.as.s.data;
        }
      }
      continue;
    }
    if (attr_name_eq(attr, "readnone")) {
      mark_simple_flag_attr(cg, fn_stmt, attr, "@readnone",
                            &decl->attr_readnone);
      continue;
    }
    if (attr_name_eq(attr, "readonly")) {
      mark_simple_flag_attr(cg, fn_stmt, attr, "@readonly",
                            &decl->attr_readonly);
      continue;
    }
    if (attr_name_eq(attr, "writeonly")) {
      mark_simple_flag_attr(cg, fn_stmt, attr, "@writeonly",
                            &decl->attr_writeonly);
      continue;
    }
    if (attr_name_eq(attr, "argmemonly")) {
      mark_simple_flag_attr(cg, fn_stmt, attr, "@argmemonly",
                            &decl->attr_argmemonly);
      continue;
    }
    if (attr_name_eq(attr, "nounwind")) {
      mark_simple_flag_attr(cg, fn_stmt, attr, "@nounwind",
                            &decl->attr_nounwind);
      continue;
    }
    if (attr_name_eq(attr, "mustprogress")) {
      mark_simple_flag_attr(cg, fn_stmt, attr, "@mustprogress",
                            &decl->attr_mustprogress);
      continue;
    }
    if (attr_name_eq(attr, "willreturn")) {
      mark_simple_flag_attr(cg, fn_stmt, attr, "@willreturn",
                            &decl->attr_willreturn);
      continue;
    }
    if (attr_name_eq(attr, "cold")) {
      mark_simple_flag_attr(cg, fn_stmt, attr, "@cold", &decl->attr_cold);
      continue;
    }
    if (attr_name_eq(attr, "hot")) {
      mark_simple_flag_attr(cg, fn_stmt, attr, "@hot", &decl->attr_hot);
      continue;
    }
    if (attr_name_eq(attr, "flatten")) {
      mark_simple_flag_attr(cg, fn_stmt, attr, "@flatten", &decl->attr_flatten);
      continue;
    }
    if (attr_name_eq(attr, "effects")) {
      if (has_effect_contract) {
        ny_diag_error(attr_diag_tok(fn_stmt, attr, 0),
                      "duplicate attribute '@effects(...)'");
        cg->had_error = 1;
        continue;
      }
      if (parse_effect_attr_args(cg, fn_stmt, attr, &effect_contract_mask))
        has_effect_contract = true;
      continue;
    }
  }
  if (is_jit && is_thread) {
    ny_diag_error(fn_stmt->tok, "conflicting attributes '@jit' and '@thread'");
    ny_diag_hint("use only one of them on the same function");
    cg->had_error = 1;
    is_jit = false;
  }
  if (is_pure && has_effect_contract && effect_contract_mask != NY_FX_NONE) {
    ny_diag_error(fn_stmt->tok,
                  "conflicting attributes '@pure' and '@effects(...)'");
    ny_diag_hint("use @effects(none) when combining with @pure");
    cg->had_error = 1;
  }
  decl->attr_naked = is_naked;
  decl->attr_jit = is_jit;
  decl->attr_thread = is_thread;
  decl->attr_pure = is_pure;
  decl->is_extern = is_extern;
  decl->link_name = link_name ? ny_strdup(link_name) : NULL;
  if (is_pure) {
    decl->effect_contract_known = true;
    decl->effect_contract_mask = NY_FX_NONE;
    decl->attr_readnone = true;
    decl->attr_nounwind = true;
    decl->attr_willreturn = true;
    decl->attr_mustprogress = true;
  } else {
    decl->effect_contract_known = has_effect_contract;
    decl->effect_contract_mask =
        has_effect_contract ? effect_contract_mask : NY_FX_ALL;
  }
  if (decl->attr_jit) {
    decl->attr_inline = true;
    decl->attr_hot = true;
  }
  if (decl->attr_thread) {
    decl->attr_noinline = true;
    decl->attr_cold = true;
  }
  decl->attrs_resolved = true;
}

static void apply_fn_attrs(codegen_t *cg, LLVMValueRef fn,
                           const stmt_t *fn_stmt) {
  if (!cg || !fn || !fn_stmt || fn_stmt->kind != NY_S_FUNC)
    return;
  const stmt_func_t *decl = &fn_stmt->as.fn;
  ny_apply_base_fn_attrs(cg, fn);
  if (decl->attr_naked) {
    add_fn_enum_attr(cg, fn, "naked", 0);
  }
  if (decl->attr_jit) {
    add_fn_enum_attr(cg, fn, "alwaysinline", 0);
    add_fn_enum_attr(cg, fn, "hot", 0);
  }
  if (decl->attr_thread) {
    add_fn_enum_attr(cg, fn, "noinline", 0);
    add_fn_enum_attr(cg, fn, "cold", 0);
  }
  if (decl->attr_inline && decl->attr_noinline) {
    ny_diag_error(fn_stmt->tok,
                  "conflicting attributes '@inline' and '@noinline'");
    cg->had_error = 1;
  } else if (decl->attr_inline) {
    add_fn_enum_attr(cg, fn, "alwaysinline", 0);
  } else if (decl->attr_noinline) {
    add_fn_enum_attr(cg, fn, "noinline", 0);
  }

  if (decl->attr_readnone)
    add_fn_enum_attr(cg, fn, "readnone", 0);
  if (decl->attr_readonly)
    add_fn_enum_attr(cg, fn, "readonly", 0);
  if (decl->attr_writeonly)
    add_fn_enum_attr(cg, fn, "writeonly", 0);
  if (decl->attr_argmemonly)
    add_fn_enum_attr(cg, fn, "argmemonly", 0);
  if (decl->attr_nounwind)
    add_fn_enum_attr(cg, fn, "nounwind", 0);
  if (decl->attr_mustprogress)
    add_fn_enum_attr(cg, fn, "mustprogress", 0);
  if (decl->attr_willreturn)
    add_fn_enum_attr(cg, fn, "willreturn", 0);
  if (decl->attr_cold)
    add_fn_enum_attr(cg, fn, "cold", 0);
  if (decl->attr_hot)
    add_fn_enum_attr(cg, fn, "hot", 0);
  if (decl->attr_flatten)
    add_fn_enum_attr(cg, fn, "flatten", 0);
  for (size_t i = 0; i < fn_stmt->attributes.len; i++) {
    const attribute_t *attr = &fn_stmt->attributes.data[i];
    if (!attr_name_eq(attr, "llvm"))
      continue;
    if (attr->args.len < 1 || attr->args.len > 2) {
      ny_diag_error(attr_diag_tok(fn_stmt, attr, 0),
                    "@llvm expects 1 or 2 arguments");
      ny_diag_hint("use @llvm(name) or @llvm(name, value)");
      cg->had_error = 1;
      continue;
    }
    const char *attr_name = NULL;
    size_t attr_name_len = 0;
    if (!attr_arg_text_view(attr->args.data[0], &attr_name, &attr_name_len) ||
        attr_name_len == 0) {
      ny_diag_error(attr_diag_tok(fn_stmt, attr, 0),
                    "@llvm first argument must be an identifier or string");
      cg->had_error = 1;
      continue;
    }
    if (attr->args.len == 1) {
      unsigned kind_id =
          LLVMGetEnumAttributeKindForName(attr_name, (unsigned)attr_name_len);
      if (kind_id != 0) {
        LLVMAttributeRef llvm_attr =
            LLVMCreateEnumAttribute(cg->ctx, kind_id, 0);
        LLVMAddAttributeAtIndex(fn, LLVMAttributeFunctionIndex, llvm_attr);
      } else {
        char *name_owned = ny_strndup(attr_name, attr_name_len);
        add_fn_string_attr(cg, fn, name_owned, "");
        free(name_owned);
      }
      continue;
    }
    const char *attr_value = NULL;
    size_t attr_value_len = 0;
    if (!attr_arg_text_view(attr->args.data[1], &attr_value, &attr_value_len)) {
      ny_diag_error(attr_diag_tok(fn_stmt, attr, 1),
                    "@llvm second argument must be an identifier or string");
      cg->had_error = 1;
      continue;
    }
    char *name_owned = ny_strndup(attr_name, attr_name_len);
    char *value_owned = ny_strndup(attr_value, attr_value_len);
    add_fn_string_attr(cg, fn, name_owned, value_owned);
    free(name_owned);
    free(value_owned);
  }
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

static void collect_assigned_names_expr(expr_t *e,
                                        assigned_name_list *out_names,
                                        assigned_hash_list *out_hashes,
                                        uint64_t out_bloom[4]);

static void collect_assigned_names_stmt(stmt_t *s,
                                        assigned_name_list *out_names,
                                        assigned_hash_list *out_hashes,
                                        uint64_t out_bloom[4]);

static void collect_assigned_names_expr_list(const ny_expr_list *exprs,
                                             assigned_name_list *out_names,
                                             assigned_hash_list *out_hashes,
                                             uint64_t out_bloom[4]) {
  if (!exprs)
    return;
  for (size_t i = 0; i < exprs->len; i++)
    collect_assigned_names_expr(exprs->data[i], out_names, out_hashes,
                                out_bloom);
}

static void collect_assigned_names_call_args(const ny_call_arg_list *args,
                                             assigned_name_list *out_names,
                                             assigned_hash_list *out_hashes,
                                             uint64_t out_bloom[4]) {
  if (!args)
    return;
  for (size_t i = 0; i < args->len; i++)
    collect_assigned_names_expr(args->data[i].val, out_names, out_hashes,
                                out_bloom);
}

static void collect_assigned_names_match_arms(const ny_match_arm_list *arms,
                                              assigned_name_list *out_names,
                                              assigned_hash_list *out_hashes,
                                              uint64_t out_bloom[4]) {
  if (!arms)
    return;
  for (size_t i = 0; i < arms->len; i++) {
    for (size_t j = 0; j < arms->data[i].patterns.len; j++)
      collect_assigned_names_expr(arms->data[i].patterns.data[j], out_names,
                                  out_hashes, out_bloom);
    collect_assigned_names_expr(arms->data[i].guard, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_stmt(arms->data[i].conseq, out_names, out_hashes,
                                out_bloom);
  }
}

static void collect_assigned_names_stmt_list(const ny_stmt_list *stmts,
                                             assigned_name_list *out_names,
                                             assigned_hash_list *out_hashes,
                                             uint64_t out_bloom[4]) {
  if (!stmts)
    return;
  for (size_t i = 0; i < stmts->len; i++)
    collect_assigned_names_stmt(stmts->data[i], out_names, out_hashes,
                                out_bloom);
}

static void collect_assigned_names_expr(expr_t *e,
                                        assigned_name_list *out_names,
                                        assigned_hash_list *out_hashes,
                                        uint64_t out_bloom[4]) {
  if (!e || !out_names || !out_hashes)
    return;
  switch (e->kind) {
  case NY_E_UNARY:
    collect_assigned_names_expr(e->as.unary.right, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_E_BINARY:
  case NY_E_LOGICAL:
    collect_assigned_names_expr(e->as.binary.left, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_expr(e->as.binary.right, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_E_TERNARY:
    collect_assigned_names_expr(e->as.ternary.cond, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_expr(e->as.ternary.true_expr, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_expr(e->as.ternary.false_expr, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_E_CALL:
    collect_assigned_names_expr(e->as.call.callee, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_call_args(&e->as.call.args, out_names, out_hashes,
                                     out_bloom);
    break;
  case NY_E_MEMCALL:
    collect_assigned_names_expr(e->as.memcall.target, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_call_args(&e->as.memcall.args, out_names, out_hashes,
                                     out_bloom);
    break;
  case NY_E_INDEX:
    collect_assigned_names_expr(e->as.index.target, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_expr(e->as.index.start, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_expr(e->as.index.stop, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_expr(e->as.index.step, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_E_MEMBER:
    collect_assigned_names_expr(e->as.member.target, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_E_PTR_TYPE:
    collect_assigned_names_expr(e->as.ptr_type.target, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_E_DEREF:
    collect_assigned_names_expr(e->as.deref.target, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_E_SIZEOF:
    collect_assigned_names_expr(e->as.szof.target, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_E_TRY:
    collect_assigned_names_expr(e->as.try_expr.target, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_E_LAMBDA:
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    collect_assigned_names_expr_list(&e->as.list_like, out_names, out_hashes,
                                     out_bloom);
    break;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; i++) {
      collect_assigned_names_expr(e->as.dict.pairs.data[i].key, out_names,
                                  out_hashes, out_bloom);
      collect_assigned_names_expr(e->as.dict.pairs.data[i].value, out_names,
                                  out_hashes, out_bloom);
    }
    break;
  case NY_E_COMPTIME:
    collect_assigned_names_stmt(e->as.comptime_expr.body, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; i++) {
      fstring_part_t *part = &e->as.fstring.parts.data[i];
      if (part->kind == NY_FSP_EXPR)
        collect_assigned_names_expr(part->as.e, out_names, out_hashes,
                                    out_bloom);
    }
    break;
  case NY_E_MATCH:
    collect_assigned_names_expr(e->as.match.test, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_match_arms(&e->as.match.arms, out_names, out_hashes,
                                      out_bloom);
    collect_assigned_names_stmt(e->as.match.default_conseq, out_names,
                                out_hashes, out_bloom);
    break;
  default:
    break;
  }
}

static void collect_assigned_names_stmt(stmt_t *s,
                                        assigned_name_list *out_names,
                                        assigned_hash_list *out_hashes,
                                        uint64_t out_bloom[4]) {
  if (!s || !out_names || !out_hashes)
    return;
  switch (s->kind) {
  case NY_S_BLOCK:
    collect_assigned_names_stmt_list(&s->as.block.body, out_names, out_hashes,
                                     out_bloom);
    break;
  case NY_S_VAR:
    if (!s->as.var.is_decl) {
      for (size_t i = 0; i < s->as.var.names.len; i++)
        assigned_name_add(out_names, out_hashes, out_bloom,
                          s->as.var.names.data[i]);
    }
    for (size_t i = 0; i < s->as.var.exprs.len; i++)
      collect_assigned_names_expr(s->as.var.exprs.data[i], out_names,
                                  out_hashes, out_bloom);
    break;
  case NY_S_EXPR:
    collect_assigned_names_expr(s->as.expr.expr, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_S_IF:
    collect_assigned_names_expr(s->as.iff.test, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_stmt(s->as.iff.conseq, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_stmt(s->as.iff.alt, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_S_WHILE:
    collect_assigned_names_expr(s->as.whl.test, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_stmt(s->as.whl.body, out_names, out_hashes,
                                out_bloom);
    if (s->as.whl.update)
      collect_assigned_names_stmt(s->as.whl.update, out_names, out_hashes,
                                  out_bloom);
    if (s->as.whl.init)
      collect_assigned_names_stmt(s->as.whl.init, out_names, out_hashes,
                                  out_bloom);
    break;
  case NY_S_FOR:
    collect_assigned_names_expr(s->as.fr.iterable, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_stmt(s->as.fr.body, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_S_TRY:
    collect_assigned_names_stmt(s->as.tr.body, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_stmt(s->as.tr.handler, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_S_DEFER:
    collect_assigned_names_stmt(s->as.de.body, out_names, out_hashes,
                                out_bloom);
    break;
  case NY_S_MATCH:
    collect_assigned_names_expr(s->as.match.test, out_names, out_hashes,
                                out_bloom);
    collect_assigned_names_match_arms(&s->as.match.arms, out_names, out_hashes,
                                      out_bloom);
    collect_assigned_names_stmt(s->as.match.default_conseq, out_names,
                                out_hashes, out_bloom);
    break;
  case NY_S_FUNC:
    break;
  case NY_S_MODULE:
    collect_assigned_names_stmt_list(&s->as.module.body, out_names, out_hashes,
                                     out_bloom);
    break;
  case NY_S_MACRO:
    for (size_t i = 0; i < s->as.macro.args.len; i++) {
      collect_assigned_names_expr(s->as.macro.args.data[i], out_names,
                                  out_hashes, out_bloom);
    }
    collect_assigned_names_stmt(s->as.macro.body, out_names, out_hashes,
                                out_bloom);
    break;
  default:
    break;
  }
}

typedef struct {
  const char **names;
  const char **types;
  int count;
} type_env_t;

static const char *env_lookup(const type_env_t *env, const char *name) {
  if (!env || !name)
    return NULL;
  for (int i = 0; i < env->count; i++)
    if (env->names[i] && env->types[i] && strcmp(name, env->names[i]) == 0)
      return env->types[i];
  return NULL;
}

static const char *ast_infer_type(expr_t *e, const type_env_t *env) {
  if (!e)
    return NULL;
  if (e->kind == NY_E_LITERAL) {
    if (e->as.literal.kind == NY_LIT_FLOAT)
      return "f64";
    if (e->as.literal.kind == NY_LIT_INT)
      return "int";
    return NULL;
  }
  if (e->kind == NY_E_IDENT)
    return env_lookup(env, e->as.ident.name);
  if (e->kind == NY_E_UNARY)
    return ast_infer_type(e->as.unary.right, env);
  if (e->kind == NY_E_BINARY) {
    const char *lt = ast_infer_type(e->as.binary.left, env);
    const char *rt = ast_infer_type(e->as.binary.right, env);
    bool lf = lt && strcmp(lt, "f64") == 0;
    bool rf = rt && strcmp(rt, "f64") == 0;
    if (lf || rf)
      return "f64";
    bool li = lt && strcmp(lt, "int") == 0;
    bool ri = rt && strcmp(rt, "int") == 0;
    if (li && ri)
      return "int";
    return NULL;
  }
  return NULL;
}

static void mark_expr_params(expr_t *e, type_env_t *env,
                             const char **param_names, const char **param_types,
                             int nparam) {
  if (!e)
    return;
  if (e->kind == NY_E_BINARY) {
    const char *lt = ast_infer_type(e->as.binary.left, env);
    const char *rt = ast_infer_type(e->as.binary.right, env);
    bool lf = lt && strcmp(lt, "f64") == 0;
    bool rf = rt && strcmp(rt, "f64") == 0;
    const char *op = e->as.binary.op;
    bool is_arith = strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
                    strcmp(op, "*") == 0 || strcmp(op, "/") == 0;
    bool li = lt && strcmp(lt, "int") == 0;
    bool ri = rt && strcmp(rt, "int") == 0;
    bool is_bitwise = strcmp(op, "&") == 0 || strcmp(op, "|") == 0 ||
                      strcmp(op, "^") == 0 || strcmp(op, "<<") == 0 ||
                      strcmp(op, ">>") == 0;

    bool is_cmp = strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
                  strcmp(op, ">") == 0 || strcmp(op, ">=") == 0 ||
                  strcmp(op, "==") == 0 || strcmp(op, "!=") == 0;
    bool is_mod = strcmp(op, "%") == 0;
    const char *inferred = NULL;
    if ((lf || rf) && is_arith)
      inferred = "f64";
    else if ((li || ri) && (is_bitwise || is_arith || is_cmp || is_mod) &&
             !(lf || rf))
      inferred = "int";

    if (inferred) {
      for (int side = 0; side < 2; side++) {
        expr_t *other = side == 0 ? e->as.binary.right : e->as.binary.left;
        if (other && other->kind == NY_E_IDENT) {
          const char *oname = other->as.ident.name;
          for (int i = 0; i < nparam; i++) {
            if (param_names[i] && strcmp(oname, param_names[i]) == 0) {
              if (!param_types[i])
                param_types[i] = inferred;
              break;
            }
          }
          // Propagate to env (local vars) — both f64 and int are safe
          bool found = false;
          for (int k = 0; k < env->count; k++) {
            if (strcmp(env->names[k], oname) == 0) {
              if (!env->types[k])
                env->types[k] = inferred;
              found = true;
              break;
            }
          }
          if (!found && env->count < 64) {
            env->names[env->count] = oname;
            env->types[env->count] = inferred;
            env->count++;
          }
        }
      }
    }
    mark_expr_params(e->as.binary.left, env, param_names, param_types, nparam);
    mark_expr_params(e->as.binary.right, env, param_names, param_types, nparam);
    return;
  }
  if (e->kind == NY_E_LOGICAL) {
    mark_expr_params(e->as.logical.left, env, param_names, param_types, nparam);
    mark_expr_params(e->as.logical.right, env, param_names, param_types,
                     nparam);
    return;
  }
  if (e->kind == NY_E_UNARY) {
    mark_expr_params(e->as.unary.right, env, param_names, param_types, nparam);
    return;
  }
  if (e->kind == NY_E_CALL) {
    for (size_t i = 0; i < e->as.call.args.len; i++)
      mark_expr_params(e->as.call.args.data[i].val, env, param_names,
                       param_types, nparam);
    return;
  }
}

static void scan_body_for_param_types(stmt_t *body, type_env_t *env,
                                      const char **param_names,
                                      const char **param_types, int nparam) {
  if (!body)
    return;
  if (body->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < body->as.block.body.len; i++)
      scan_body_for_param_types(body->as.block.body.data[i], env, param_names,
                                param_types, nparam);
    return;
  }
  if (body->kind == NY_S_VAR) {
    for (size_t i = 0; i < body->as.var.names.len; i++) {
      expr_t *init =
          (i < body->as.var.exprs.len) ? body->as.var.exprs.data[i] : NULL;
      if (init) {
        mark_expr_params(init, env, param_names, param_types, nparam);
        const char *t = ast_infer_type(init, env);

        // If this is an assignment to a parameter, update its type
        for (int j = 0; j < nparam; j++) {
          if (param_names[j] &&
              strcmp(body->as.var.names.data[i], param_names[j]) == 0) {
            if (t) {
              if (strcmp(t, "f64") == 0)
                param_types[j] = "f64";
              else if (!param_types[j])
                param_types[j] = t;
            }
          }
        }

        if (t && env->count < 64) {
          bool found = false;
          for (int k = 0; k < env->count; k++) {
            if (strcmp(env->names[k], body->as.var.names.data[i]) == 0) {
              env->types[k] = t;
              found = true;
              break;
            }
          }
          if (!found) {
            env->names[env->count] = body->as.var.names.data[i];
            env->types[env->count] = t;
            env->count++;
          }
        }

        // Reverse propagation: If LHS has a type in env, and RHS is a param,
        // give param that type.
        if (init->kind == NY_E_IDENT) {
          const char *lhs_type = NULL;
          for (int k = 0; k < env->count; k++) {
            if (strcmp(env->names[k], body->as.var.names.data[i]) == 0) {
              lhs_type = env->types[k];
              break;
            }
          }
          if (lhs_type) {
            for (int j = 0; j < nparam; j++) {
              if (param_names[j] &&
                  strcmp(init->as.ident.name, param_names[j]) == 0) {
                if (!param_types[j])
                  param_types[j] = lhs_type;
              }
            }
          }
        }
      }
    }
    return;
  }
  if (body->kind == NY_S_EXPR) {
    mark_expr_params(body->as.expr.expr, env, param_names, param_types, nparam);
    return;
  }
  if (body->kind == NY_S_WHILE) {
    mark_expr_params(body->as.whl.test, env, param_names, param_types, nparam);
    scan_body_for_param_types(body->as.whl.body, env, param_names, param_types,
                              nparam);
    if (body->as.whl.update)
      scan_body_for_param_types(body->as.whl.update, env, param_names,
                                param_types, nparam);
    if (body->as.whl.init)
      scan_body_for_param_types(body->as.whl.init, env, param_names,
                                param_types, nparam);
    return;
  }
  if (body->kind == NY_S_IF) {
    mark_expr_params(body->as.iff.test, env, param_names, param_types, nparam);
    scan_body_for_param_types(body->as.iff.conseq, env, param_names,
                              param_types, nparam);
    scan_body_for_param_types(body->as.iff.alt, env, param_names, param_types,
                              nparam);
    return;
  }
  if (body->kind == NY_S_FOR) {
    mark_expr_params(body->as.fr.iterable, env, param_names, param_types,
                     nparam);
    scan_body_for_param_types(body->as.fr.body, env, param_names, param_types,
                              nparam);
    return;
  }
  if (body->kind == NY_S_TRY) {
    scan_body_for_param_types(body->as.tr.body, env, param_names, param_types,
                              nparam);
    scan_body_for_param_types(body->as.tr.handler, env, param_names,
                              param_types, nparam);
    return;
  }
  if (body->kind == NY_S_DEFER) {
    scan_body_for_param_types(body->as.de.body, env, param_names, param_types,
                              nparam);
    return;
  }
  if (body->kind == NY_S_MATCH) {
    mark_expr_params(body->as.match.test, env, param_names, param_types,
                     nparam);
    for (size_t i = 0; i < body->as.match.arms.len; i++)
      scan_body_for_param_types(body->as.match.arms.data[i].conseq, env,
                                param_names, param_types, nparam);
    if (body->as.match.default_conseq)
      scan_body_for_param_types(body->as.match.default_conseq, env, param_names,
                                param_types, nparam);
    return;
  }
  if (body->kind == NY_S_RETURN) {
    mark_expr_params(body->as.ret.value, env, param_names, param_types, nparam);
    return;
  }
}

// Check if a param name appears as argument to float(), store32_f32(),
// or other float-consuming calls.
static void scan_float_usage_expr(expr_t *e, const char **pnames,
                                  bool *used_float, int np) {
  if (!e)
    return;
  if (e->kind == NY_E_CALL) {
    // Check if this is float(param) or store32_f32(..., param, ...)
    expr_t *callee = e->as.call.callee;
    bool is_float_fn = false;
    if (callee && callee->kind == NY_E_IDENT) {
      const char *fn_name = callee->as.ident.name;
      is_float_fn =
          (strcmp(fn_name, "float") == 0 || strcmp(fn_name, "to_float") == 0 ||
           strcmp(fn_name, "store32_f32") == 0 ||
           strcmp(fn_name, "store64_f64") == 0 ||
           strcmp(fn_name, "is_int") == 0 || strcmp(fn_name, "is_float") == 0 ||
           strcmp(fn_name, "is_str") == 0 || strcmp(fn_name, "is_dict") == 0 ||
           strcmp(fn_name, "is_list") == 0 || strcmp(fn_name, "type_of") == 0);
    }
    if (is_float_fn) {
      for (size_t a = 0; a < e->as.call.args.len; a++) {
        expr_t *arg = e->as.call.args.data[a].val;
        if (arg && arg->kind == NY_E_IDENT) {
          for (int i = 0; i < np; i++) {
            if (pnames[i] && strcmp(arg->as.ident.name, pnames[i]) == 0)
              used_float[i] = true;
          }
        }
      }
    }
    for (size_t a = 0; a < e->as.call.args.len; a++)
      scan_float_usage_expr(e->as.call.args.data[a].val, pnames, used_float,
                            np);
    return;
  }
  if (e->kind == NY_E_BINARY) {
    scan_float_usage_expr(e->as.binary.left, pnames, used_float, np);
    scan_float_usage_expr(e->as.binary.right, pnames, used_float, np);
    return;
  }
  if (e->kind == NY_E_LOGICAL) {
    scan_float_usage_expr(e->as.logical.left, pnames, used_float, np);
    scan_float_usage_expr(e->as.logical.right, pnames, used_float, np);
    return;
  }
  if (e->kind == NY_E_UNARY) {
    scan_float_usage_expr(e->as.unary.right, pnames, used_float, np);
    return;
  }
  if (e->kind == NY_E_TERNARY) {
    scan_float_usage_expr(e->as.ternary.cond, pnames, used_float, np);
    scan_float_usage_expr(e->as.ternary.true_expr, pnames, used_float, np);
    scan_float_usage_expr(e->as.ternary.false_expr, pnames, used_float, np);
    return;
  }
}

static void scan_float_usage(stmt_t *s, const char **pnames, bool *used_float,
                             int np) {
  if (!s)
    return;
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; i++)
      scan_float_usage(s->as.block.body.data[i], pnames, used_float, np);
    return;
  }
  if (s->kind == NY_S_EXPR) {
    scan_float_usage_expr(s->as.expr.expr, pnames, used_float, np);
    return;
  }
  if (s->kind == NY_S_VAR) {
    for (size_t i = 0; i < s->as.var.exprs.len; i++)
      scan_float_usage_expr(s->as.var.exprs.data[i], pnames, used_float, np);
    return;
  }
  if (s->kind == NY_S_RETURN) {
    scan_float_usage_expr(s->as.ret.value, pnames, used_float, np);
    return;
  }
  if (s->kind == NY_S_IF) {
    scan_float_usage_expr(s->as.iff.test, pnames, used_float, np);
    scan_float_usage(s->as.iff.conseq, pnames, used_float, np);
    scan_float_usage(s->as.iff.alt, pnames, used_float, np);
    return;
  }
  if (s->kind == NY_S_WHILE) {
    scan_float_usage_expr(s->as.whl.test, pnames, used_float, np);
    scan_float_usage(s->as.whl.body, pnames, used_float, np);
    if (s->as.whl.update)
      scan_float_usage(s->as.whl.update, pnames, used_float, np);
    if (s->as.whl.init)
      scan_float_usage(s->as.whl.init, pnames, used_float, np);
    return;
  }
  if (s->kind == NY_S_FOR) {
    scan_float_usage_expr(s->as.fr.iterable, pnames, used_float, np);
    scan_float_usage(s->as.fr.body, pnames, used_float, np);
    return;
  }
}

static bool is_numeric_fn(const char *fn_name) {
  if (!fn_name)
    return false;
  // Functions that only make sense with numeric args
  static const char *numeric_fns[] = {"abs",
                                      "sqrt",
                                      "sin",
                                      "cos",
                                      "tan",
                                      "floor",
                                      "ceil",
                                      "round",
                                      "min",
                                      "max",
                                      "pow",
                                      "log",
                                      "log2",
                                      "log10",
                                      "exp",
                                      "float",
                                      "to_float",
                                      "int",
                                      "to_int",
                                      "clamp",
                                      "fib_naive",
                                      "fib_linear",
                                      "fib_fast_doubling",
                                      "fib_matrix",
                                      "fib_fast_squaring",
                                      NULL};
  for (const char **p = numeric_fns; *p; p++)
    if (strcmp(fn_name, *p) == 0)
      return true;
  return false;
}

static void scan_poly_usage_expr(expr_t *e, const char **pnames, bool *poly,
                                 int np) {
  if (!e)
    return;
  if (e->kind == NY_E_CALL) {
    expr_t *callee = e->as.call.callee;
    bool is_numeric = false;
    if (callee && callee->kind == NY_E_IDENT)
      is_numeric = is_numeric_fn(callee->as.ident.name);
    // If calling a non-numeric function with a param directly as arg,
    // mark param as potentially polymorphic
    if (!is_numeric && callee) {
      for (size_t a = 0; a < e->as.call.args.len; a++) {
        expr_t *arg = e->as.call.args.data[a].val;
        if (arg && arg->kind == NY_E_IDENT) {
          for (int i = 0; i < np; i++) {
            if (pnames[i] && strcmp(arg->as.ident.name, pnames[i]) == 0)
              poly[i] = true;
          }
        }
      }
    }
    for (size_t a = 0; a < e->as.call.args.len; a++)
      scan_poly_usage_expr(e->as.call.args.data[a].val, pnames, poly, np);
    return;
  }
  if (e->kind == NY_E_BINARY) {
    scan_poly_usage_expr(e->as.binary.left, pnames, poly, np);
    scan_poly_usage_expr(e->as.binary.right, pnames, poly, np);
    return;
  }
  if (e->kind == NY_E_LOGICAL) {
    scan_poly_usage_expr(e->as.logical.left, pnames, poly, np);
    scan_poly_usage_expr(e->as.logical.right, pnames, poly, np);
    return;
  }
  if (e->kind == NY_E_UNARY) {
    scan_poly_usage_expr(e->as.unary.right, pnames, poly, np);
    return;
  }
  if (e->kind == NY_E_TERNARY) {
    scan_poly_usage_expr(e->as.ternary.cond, pnames, poly, np);
    scan_poly_usage_expr(e->as.ternary.true_expr, pnames, poly, np);
    scan_poly_usage_expr(e->as.ternary.false_expr, pnames, poly, np);
    return;
  }
  // Index access: param[x] or x[param] indicates non-int usage
  if (e->kind == NY_E_INDEX) {
    expr_t *obj = e->as.index.target;
    if (obj && obj->kind == NY_E_IDENT) {
      for (int i = 0; i < np; i++) {
        if (pnames[i] && strcmp(obj->as.ident.name, pnames[i]) == 0)
          poly[i] = true;
      }
    }
    scan_poly_usage_expr(e->as.index.target, pnames, poly, np);
    scan_poly_usage_expr(e->as.index.start, pnames, poly, np);
    return;
  }
  if (e->kind == NY_E_MEMBER) {
    expr_t *obj = e->as.member.target;
    if (obj && obj->kind == NY_E_IDENT) {
      for (int i = 0; i < np; i++) {
        if (pnames[i] && strcmp(obj->as.ident.name, pnames[i]) == 0)
          poly[i] = true;
      }
    }
    scan_poly_usage_expr(e->as.member.target, pnames, poly, np);
    return;
  }
}

static void scan_poly_usage(stmt_t *s, const char **pnames, bool *poly,
                            int np) {
  if (!s)
    return;
  if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; i++)
      scan_poly_usage(s->as.block.body.data[i], pnames, poly, np);
    return;
  }
  if (s->kind == NY_S_EXPR) {
    scan_poly_usage_expr(s->as.expr.expr, pnames, poly, np);
    return;
  }
  if (s->kind == NY_S_VAR) {
    for (size_t i = 0; i < s->as.var.exprs.len; i++)
      scan_poly_usage_expr(s->as.var.exprs.data[i], pnames, poly, np);
    return;
  }
  if (s->kind == NY_S_RETURN) {
    scan_poly_usage_expr(s->as.ret.value, pnames, poly, np);
    return;
  }
  if (s->kind == NY_S_IF) {
    scan_poly_usage_expr(s->as.iff.test, pnames, poly, np);
    scan_poly_usage(s->as.iff.conseq, pnames, poly, np);
    scan_poly_usage(s->as.iff.alt, pnames, poly, np);
    return;
  }
  if (s->kind == NY_S_WHILE) {
    scan_poly_usage_expr(s->as.whl.test, pnames, poly, np);
    scan_poly_usage(s->as.whl.body, pnames, poly, np);
    if (s->as.whl.update)
      scan_poly_usage(s->as.whl.update, pnames, poly, np);
    if (s->as.whl.init)
      scan_poly_usage(s->as.whl.init, pnames, poly, np);
    return;
  }
  if (s->kind == NY_S_FOR) {
    scan_poly_usage_expr(s->as.fr.iterable, pnames, poly, np);
    scan_poly_usage(s->as.fr.body, pnames, poly, np);
    return;
  }
}

static void infer_param_types(stmt_t *fn, const char **param_types) {
  int nparam = (int)fn->as.fn.params.len;
  if (nparam == 0 || nparam > 16)
    return;
  // Skip int inference for stdlib functions (they're typically polymorphic)
  bool is_stdlib = ny_is_stdlib_tok(fn->tok);
  if (!is_stdlib && fn->tok.filename) {
    is_stdlib = strstr(fn->tok.filename, "nytrix/lib/") != NULL ||
                strstr(fn->tok.filename, "nytrix\\lib\\") != NULL;
  }
  if (is_stdlib) {
    for (int i = 0; i < nparam; i++)
      param_types[i] = fn->as.fn.params.data[i].type;
    return;
  }
  const char *param_names[16];
  for (int i = 0; i < nparam; i++) {
    param_names[i] = fn->as.fn.params.data[i].name;
    param_types[i] = fn->as.fn.params.data[i].type;
  }
  const char *env_names[64];
  const char *env_types[64];
  type_env_t env = {env_names, env_types, 0};
  for (int i = 0; i < nparam; i++) {
    if (param_types[i]) {
      env.names[env.count] = param_names[i];
      env.types[env.count] = param_types[i];
      env.count++;
    }
  }
  // Multiple passes: each pass propagates types further (local vars → params)
  for (int pass = 0; pass < 3; pass++)
    scan_body_for_param_types(fn->as.fn.body, &env, param_names, param_types,
                              nparam);
  // Safety: clear int inference for params used in float or polymorphic
  // contexts
  bool param_used_as_float[16] = {0};
  bool param_used_poly[16] = {0};
  scan_float_usage(fn->as.fn.body, param_names, param_used_as_float, nparam);
  scan_poly_usage(fn->as.fn.body, param_names, param_used_poly, nparam);
  for (int i = 0; i < nparam; i++) {
    if (param_types[i] && strcmp(param_types[i], "int") == 0 &&
        !fn->as.fn.params.data[i].type) {
      if (param_used_as_float[i] || param_used_poly[i])
        param_types[i] = NULL;
      expr_t *dv = fn->as.fn.params.data[i].def;
      if (dv && dv->kind == NY_E_LITERAL && dv->as.literal.kind == NY_LIT_FLOAT)
        param_types[i] = NULL;
    }
  }
}

static const char *infer_return_type_walk(stmt_t *body, const type_env_t *env,
                                          const char *cur) {
  if (!body)
    return cur;
  if (body->kind == NY_S_RETURN) {
    const char *t =
        body->as.ret.value ? ast_infer_type(body->as.ret.value, env) : NULL;
    if (!t)
      return cur;
    if (!cur)
      return t;
    if (strcmp(cur, t) != 0)
      return "?";
    return cur;
  }
  if (body->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < body->as.block.body.len; i++) {
      cur = infer_return_type_walk(body->as.block.body.data[i], env, cur);
      if (cur && cur[0] == '?')
        return cur;
    }
    return cur;
  }
  if (body->kind == NY_S_IF) {
    cur = infer_return_type_walk(body->as.iff.conseq, env, cur);
    if (cur && cur[0] == '?')
      return cur;
    cur = infer_return_type_walk(body->as.iff.alt, env, cur);
    return cur;
  }
  if (body->kind == NY_S_WHILE) {
    const char *r = infer_return_type_walk(body->as.whl.body, env, cur);
    if (r)
      return r;
    if (body->as.whl.update)
      r = infer_return_type_walk(body->as.whl.update, env, cur);
    return r;
  }
  if (body->kind == NY_S_FOR) {
    return infer_return_type_walk(body->as.fr.body, env, cur);
  }
  return cur;
}

static const char *infer_fn_return_type(stmt_t *fn, const char **param_types) {
  if (fn->as.fn.return_type)
    return NULL;
  int nparam = (int)fn->as.fn.params.len;
  if (nparam > 16)
    return NULL;
  bool is_stdlib = ny_is_stdlib_tok(fn->tok);
  if (!is_stdlib && fn->tok.filename) {
    is_stdlib = strstr(fn->tok.filename, "nytrix/lib/") != NULL ||
                strstr(fn->tok.filename, "nytrix\\lib\\") != NULL;
  }
  if (is_stdlib)
    return NULL;
  const char *env_names[16];
  const char *env_types[16];
  type_env_t env = {env_names, env_types, 0};
  for (int i = 0; i < nparam; i++) {
    const char *t =
        param_types[i] ? param_types[i] : fn->as.fn.params.data[i].type;
    if (t) {
      env.names[env.count] = fn->as.fn.params.data[i].name;
      env.types[env.count] = t;
      env.count++;
    }
  }
  const char *ret = infer_return_type_walk(fn->as.fn.body, &env, NULL);
  if (ret && ret[0] != '?')
    return ret;
  return NULL;
}

void gen_func(codegen_t *cg, stmt_t *fn, const char *name, scope *scopes,
              size_t depth, binding_list *captures) {
  resolve_fn_attrs(cg, fn);
  if (!fn->as.fn.body)
    return;
  LLVMValueRef f = LLVMGetNamedFunction(cg->module, name);
  if (!f) {
    size_t n_params = fn->as.fn.params.len;
    sema_func_t *sema = (sema_func_t *)fn->sema;
    size_t total_args = captures ? n_params + 1 : n_params;
    LLVMTypeRef *pt = alloca(sizeof(LLVMTypeRef) * total_args);
    if (captures) {
      pt[0] = cg->type_i64;
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
    fun_sig sig = {.name = ny_strdup(name),
                   .type = ft,
                   .value = f,
                   .stmt_t = fn,
                   .arity = (int)n_params,
                   .is_variadic = fn->as.fn.is_variadic,
                   .is_extern = false,
                   .effects = NY_FX_ALL,
                   .args_escape = true,
                   .args_mutated = true,
                   .returns_alias = true,
                   .effects_known = false,
                   .link_name = NULL,
                   .return_type = fn->as.fn.return_type
                                      ? ny_strdup(fn->as.fn.return_type)
                                      : NULL,
                   .owned = false,
                   .name_hash = 0};
    vec_push(&cg->fun_sigs, sig);
    apply_fn_attrs(cg, f, fn);
  } else {
    ny_llvm_clear_function(f);
  }
  if (cg->skip_stdlib && fn->tok.filename &&
      (strstr(fn->tok.filename, "std.ny") ||
       strstr(fn->tok.filename, "lib.ny") ||
       strncmp(fn->tok.filename, "<stdlib>", 8) == 0 ||
       strstr(fn->tok.filename, "/share/nytrix/"))) {
    return;
  }
  LLVMBasicBlockRef cur = LLVMGetInsertBlock(cg->builder);
  LLVMBasicBlockRef entry_bb = ny_llvm_append_block(f, "entry");
  LLVMPositionBuilderAtEnd(cg->builder, entry_bb);

  // Entry block has no predecessors — seal it immediately so Braun SSA never
  // creates incomplete PHIs at the function entry.

  LLVMMetadataRef prev_scope = cg->di_scope;
  LLVMMetadataRef prev_loc = cg->di_loc;
  if (cg->debug_symbols && cg->di_builder) {
    LLVMMetadataRef sp = codegen_debug_subprogram(cg, f, name, fn->tok);
    if (sp)
      cg->di_scope = sp;
  }
  ny_dbg_loc(cg, fn->tok);
  if (!fn->as.fn.attr_naked)
    emit_trace_enter(cg, name ? name : "<anon>", fn->tok);
  size_t fd = depth + 1;
  size_t root = fd;
  assigned_name_list assigned_names = {0};
  assigned_hash_list assigned_hashes = {0};
  uint64_t assigned_bloom[4] = {0, 0, 0, 0};
  bool use_assigned_prepass =
      !ny_env_enabled("NYTRIX_DISABLE_ASSIGNED_PREPASS");
  if (use_assigned_prepass) {
    collect_assigned_names_stmt(fn->as.fn.body, &assigned_names,
                                &assigned_hashes, assigned_bloom);
  }
  memset(&scopes[fd], 0, sizeof(scopes[fd]));
  size_t param_offset = 0;
  if (!fn->as.fn.attr_naked) {
    if (captures) {
      param_offset = 1;
      unsigned actual_params = LLVMCountParams(f);
      LLVMValueRef env_arg = (actual_params > 0) ? LLVMGetParam(f, 0) : NULL;
      LLVMValueRef env_raw =
          env_arg
              ? LLVMBuildIntToPtr(cg->builder, env_arg,
                                  LLVMPointerType(cg->type_i64, 0), "env_raw")
              : NULL;
      for (size_t i = 0; i < captures->len; i++) {
        LLVMValueRef src =
            env_raw ? LLVMBuildGEP2(cg->builder, cg->type_i64, env_raw,
                                    (LLVMValueRef[]){LLVMConstInt(
                                        cg->type_i64, (uint64_t)i, false)},
                                    1, "")
                    : NULL;
        LLVMValueRef val =
            src ? LLVMBuildLoad2(cg->builder, cg->type_i64, src, "") : NULL;
        bool needs_slot =
            !use_assigned_prepass ||
            (captures->data[i].is_mut &&
             assigned_name_contains(&assigned_names, &assigned_hashes,
                                    assigned_bloom, captures->data[i].name));
        if (needs_slot) {
          LLVMValueRef lv =
              build_alloca(cg, captures->data[i].name, cg->type_i64);
          if (lv && val) {
            LLVMBuildStore(cg->builder, val, lv);
          }
          scope_bind(cg, scopes, fd, captures->data[i].name, lv,
                     captures->data[i].stmt_t ? captures->data[i].stmt_t : fn,
                     true, captures->data[i].type_name, true);
        } else {
          scope_bind(cg, scopes, fd, captures->data[i].name, val,
                     captures->data[i].stmt_t ? captures->data[i].stmt_t : fn,
                     captures->data[i].is_mut, captures->data[i].type_name,
                     false);
        }
        scopes[fd].vars.data[scopes[fd].vars.len - 1].is_used = true;
      }
    }
    const char *inferred_types[16] = {0};
    if (fn->as.fn.params.len <= 16)
      infer_param_types(fn, inferred_types);
    static int debug_infer = -1;
    if (debug_infer < 0)
      debug_infer = (getenv("NYTRIX_DEBUG_INFER") != NULL);
    if (debug_infer) {
      for (size_t di = 0; di < fn->as.fn.params.len && di < 16; di++) {
        if (inferred_types[di] && !fn->as.fn.params.data[di].type)
          fprintf(stderr, "[INFER] %s param[%zu] '%s' -> %s (%s)\n", name, di,
                  fn->as.fn.params.data[di].name, inferred_types[di],
                  fn->tok.filename ? fn->tok.filename : "<null>");
      }
    }
    unsigned actual_params = LLVMCountParams(f);
    for (size_t i = 0; i < fn->as.fn.params.len; i++) {
      const char *param_name = fn->as.fn.params.data[i].name;
      unsigned param_idx = (unsigned)(i + param_offset);
      LLVMValueRef param_val =
          (param_idx < actual_params) ? LLVMGetParam(f, param_idx) : NULL;
      if (param_val && fn->as.fn.params.data[i].type != NULL &&
          !ny_type_is_tagged(fn->as.fn.params.data[i].type)) {
        LLVMTypeRef pllty = LLVMTypeOf(param_val);
        LLVMTypeKind pk = LLVMGetTypeKind(pllty);
        if (pk == LLVMDoubleTypeKind || pk == LLVMFloatTypeKind) {
          LLVMValueRef f64val = param_val;
          if (pk == LLVMFloatTypeKind)
            f64val = LLVMBuildFPExt(cg->builder, param_val, cg->type_f64, "");
          LLVMValueRef bits =
              LLVMBuildBitCast(cg->builder, f64val, cg->type_i64, "");
          fun_sig *box_sig = lookup_fun(cg, "__flt_box_val", 0);
          if (box_sig) {
            param_val = LLVMBuildCall2(cg->builder, box_sig->type,
                                       box_sig->value, &bits, 1, "");
          } else {
            param_val = bits;
          }
        } else if (pk == LLVMPointerTypeKind) {
          param_val =
              LLVMBuildPtrToInt(cg->builder, param_val, cg->type_i64, "");
        } else if (pk == LLVMIntegerTypeKind) {
          unsigned w = LLVMGetIntTypeWidth(pllty);
          if (w < 64)
            param_val = LLVMBuildSExt(cg->builder, param_val, cg->type_i64, "");
          param_val = ny_tag_int(cg, param_val);
        }
      }
      bool is_inferred_f64 = !fn->as.fn.params.data[i].type && i < 16 &&
                             inferred_types[i] &&
                             strcmp(inferred_types[i], "f64") == 0;
      bool is_inferred_int = !fn->as.fn.params.data[i].type && i < 16 &&
                             inferred_types[i] &&
                             strcmp(inferred_types[i], "int") == 0;
      bool needs_slot =
          !use_assigned_prepass ||
          assigned_name_contains(&assigned_names, &assigned_hashes,
                                 assigned_bloom, param_name);
      if (is_inferred_f64 && param_val) {
        LLVMValueRef f64v = ny_unbox_float(cg, param_val);
        if (needs_slot) {
          LLVMValueRef slot = build_alloca(cg, param_name, cg->type_f64);
          LLVMBuildStore(cg->builder, f64v, slot);
          scope_bind(cg, scopes, fd, param_name, slot, fn, true, "f64", true);
          binding *b = &scopes[fd].vars.data[scopes[fd].vars.len - 1];
          b->is_f64_slot = true;
        } else {
          scope_bind(cg, scopes, fd, param_name, f64v, fn, true, "f64", false);
          binding *b = &scopes[fd].vars.data[scopes[fd].vars.len - 1];
          b->is_f64_slot = true;
        }
      } else if (needs_slot) {
        LLVMValueRef slot = build_alloca(cg, param_name, cg->type_i64);
        if (slot && param_val) {
          LLVMBuildStore(cg->builder, param_val, slot);
        }
        const char *ptype = fn->as.fn.params.data[i].type;
        if (!ptype && is_inferred_int)
          ptype = "int";
        scope_bind(cg, scopes, fd, param_name, slot, fn, true, ptype, true);
        if (cg->debug_symbols && cg->di_builder && slot) {
          codegen_debug_variable(cg, param_name, slot, fn->tok, true,
                                 (int)i + 1 + (int)param_offset, true);
        }
      } else {
        const char *ptype = fn->as.fn.params.data[i].type;
        if (!ptype && is_inferred_int)
          ptype = "int";
        scope_bind(cg, scopes, fd, param_name, param_val, fn, true, ptype,
                   false);
        if (cg->debug_symbols && cg->di_builder && param_val) {
          codegen_debug_variable(cg, param_name, param_val, fn->tok, true,
                                 (int)i + 1 + (int)param_offset, false);
        }
      }
    }
  }
  size_t old_root = cg->func_root_idx;
  cg->func_root_idx = root;
  const char **old_assigned_names_data = cg->assigned_names_data;
  size_t old_assigned_names_len = cg->assigned_names_len;
  const uint64_t *old_assigned_name_hashes_data = cg->assigned_name_hashes_data;
  size_t old_assigned_name_hashes_len = cg->assigned_name_hashes_len;
  uint64_t old_assigned_names_bloom[4] = {
      cg->assigned_names_bloom[0], cg->assigned_names_bloom[1],
      cg->assigned_names_bloom[2], cg->assigned_names_bloom[3]};
  cg->assigned_names_data = assigned_names.data;
  cg->assigned_names_len = assigned_names.len;
  cg->assigned_name_hashes_data = assigned_hashes.data;
  cg->assigned_name_hashes_len = assigned_hashes.len;
  cg->assigned_names_bloom[0] = assigned_bloom[0];
  cg->assigned_names_bloom[1] = assigned_bloom[1];
  cg->assigned_names_bloom[2] = assigned_bloom[2];
  cg->assigned_names_bloom[3] = assigned_bloom[3];
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
  bool prev_naked = cg->current_fn_attr_naked;
  LLVMValueRef prev_fn_value = cg->current_fn_value;
  cg->current_fn_ret_type = fn->as.fn.return_type;
  cg->current_fn_attr_naked = fn->as.fn.attr_naked;
  cg->current_fn_value = f;
  gen_stmt(cg, scopes, &fd, fn->as.fn.body, root, true);
  cg->current_fn_ret_type = prev_ret;
  cg->current_fn_attr_naked = prev_naked;
  cg->current_fn_value = prev_fn_value;
  cg->current_module_name = prev_mod;
  if (temp_mod)
    free(temp_mod);
  cg->func_root_idx = old_root;
  cg->assigned_names_data = old_assigned_names_data;
  cg->assigned_names_len = old_assigned_names_len;
  cg->assigned_name_hashes_data = old_assigned_name_hashes_data;
  cg->assigned_name_hashes_len = old_assigned_name_hashes_len;
  cg->assigned_names_bloom[0] = old_assigned_names_bloom[0];
  cg->assigned_names_bloom[1] = old_assigned_names_bloom[1];
  cg->assigned_names_bloom[2] = old_assigned_names_bloom[2];
  cg->assigned_names_bloom[3] = old_assigned_names_bloom[3];
  if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
    if (fn->as.fn.attr_naked) {
      LLVMBuildUnreachable(cg->builder);
    } else {
      ny_cg_emit_trace_exit(cg);
      LLVMBuildRet(cg->builder, LLVMConstInt(cg->type_i64, 1, false));
    }
  }
  scope_pop(scopes, &fd);
  vec_free(&assigned_names);
  vec_free(&assigned_hashes);
  cg->di_scope = prev_scope;
  cg->di_loc = prev_loc;
  if (cg->debug_symbols && cg->builder) {
    LLVMSetCurrentDebugLocation2(cg->builder, prev_loc);
  }
  if (cur)
    LLVMPositionBuilderAtEnd(cg->builder, cur);
}

void collect_sigs(codegen_t *cg, stmt_t *s) {
  if (!cg || !s)
    return;
  if (s->kind == NY_S_FUNC) {
    sema_func_t *sema_func = arena_alloc(cg->arena, sizeof(sema_func_t));
    memset(sema_func, 0, sizeof(sema_func_t));
    sema_func->resolved_return_type =
        s->as.fn.return_type
            ? resolve_abi_type_name(cg, s->as.fn.return_type, s->tok)
            : cg->type_i64;
    for (size_t j = 0; j < s->as.fn.params.len; j++) {
      const char *ptype = s->as.fn.params.data[j].type;
      LLVMTypeRef param_ty =
          ptype ? resolve_abi_type_name(cg, ptype, s->tok) : cg->type_i64;
      vec_push_arena(cg->arena, &sema_func->resolved_param_types, param_ty);
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
    resolve_fn_attrs(cg, s);
    const char *ln = s->as.fn.link_name ? s->as.fn.link_name : final_name;
    LLVMValueRef f = LLVMGetNamedFunction(cg->module, ln);
    if (!f)
      f = LLVMAddFunction(cg->module, ln, ft);
    apply_fn_attrs(cg, f, s);
    LLVMSetAlignment(f, 16);
    fun_sig sig;
    ny_fun_sig_init(&sig, final_name, ft, f, s, (int)s->as.fn.params.len,
                    s->as.fn.is_variadic, s->as.fn.is_extern);
    sig.link_name = s->as.fn.link_name ? ny_strdup(s->as.fn.link_name) : NULL;
    sig.return_type =
        s->as.fn.return_type ? ny_strdup(s->as.fn.return_type) : NULL;
    {
      const char *inferred_types[16] = {0};
      if (!sig.return_type && s->as.fn.params.len <= 16) {
        infer_param_types(s, inferred_types);
        const char *inferred_ret = infer_fn_return_type(s, inferred_types);
        sig.inferred_return_type =
            inferred_ret ? ny_strdup(inferred_ret) : NULL;
      } else {
        sig.inferred_return_type = NULL;
      }
    }
    vec_push(&cg->fun_sigs, sig);
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
    fun_sig sig;
    ny_fun_sig_init(&sig, final_name, ft, f, s, (int)param_count,
                    s->as.ext.is_variadic, true);
    sig.link_name = s->as.ext.link_name ? ny_strdup(s->as.ext.link_name) : NULL;
    sig.return_type =
        s->as.ext.return_type ? ny_strdup(s->as.ext.return_type) : NULL;
    vec_push(&cg->fun_sigs, sig);
  } else if (s->kind == NY_S_VAR) {
    for (size_t j = 0; j < s->as.var.names.len; j++) {
      const char *n = s->as.var.names.data[j];
      const char *final_name = codegen_qname(cg, n, cg->current_module_name);
      bool found = lookup_global_exact(cg, final_name) != NULL;
      if (!found) {
        LLVMValueRef g = LLVMAddGlobal(cg->module, cg->type_i64, final_name);
        bool define_here = true;
        if (cg->skip_stdlib && ny_is_stdlib_tok(s->tok)) {
          define_here = false;
        }
        if (cg->emit_module_decls_only) {
          if (cg->emit_module_name && cg->emit_module_name[0]) {
            define_here =
                (cg->current_module_name &&
                 strcmp(cg->current_module_name, cg->emit_module_name) == 0);
          } else {
            define_here =
                (!cg->current_module_name || !*cg->current_module_name);
          }
        }
        if (define_here) {
          if (!(cg->skip_stdlib && ny_is_stdlib_tok(s->tok)))
            LLVMSetInitializer(g, LLVMConstInt(cg->type_i64, 0, false));
        } else {
          LLVMSetLinkage(g, LLVMExternalLinkage);
        }
        const char *type_name = NULL;
        if (s->as.var.types.len > j)
          type_name = s->as.var.types.data[j];
        binding b = {.name = ny_strdup(final_name),
                     .value = g,
                     .stmt_t = s,
                     .is_slot = true,
                     .is_mut = s->as.var.is_mut,
                     .is_used = false,
                     .owned = true,
                     .type_name = type_name,
                     .decl_type_name = type_name,
                     .name_hash = 0};
        vec_push(&cg->global_vars, b);
        if (cg->debug_symbols && cg->di_builder) {
          codegen_debug_global_variable(cg, final_name, g, s->tok);
        }
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
  } else if (s->kind == NY_S_ENUM) {
    enum_def_t *enu = arena_alloc(cg->arena, sizeof(enum_def_t));
    memset(enu, 0, sizeof(enum_def_t));
    enu->name = ny_strdup(s->as.enu.name);
    enu->stmt = s;
    cg->current_enum_val = 0;
    for (size_t i = 0; i < cg->enums.len; i++) {
      if (strcmp(cg->enums.data[i]->name, enu->name) == 0) {
        return;
      }
    }
    for (size_t i = 0; i < s->as.enu.items.len; i++) {
      stmt_enum_item_t *item = &s->as.enu.items.data[i];
      enum_member_def_t member = {0};
      member.name = ny_strdup(item->name);
      for (size_t j = 0; j < enu->members.len; j++) {
        if (strcmp(enu->members.data[j].name, member.name) == 0) {
          ny_diag_error(s->tok, "redefinition of enum member '%s' in enum '%s'",
                        member.name, enu->name);
          cg->had_error = 1;
          goto next_enum_member;
        }
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
      vec_push(&enu->members, member);
    next_enum_member:;
    }
    vec_push(&cg->enums, enu);
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
  } else if (s->kind == NY_S_IF) {
    bool truthy = false;
    bool determined = false;
    if (s->as.iff.test->kind == NY_E_COMPTIME) {
      LLVMValueRef val =
          gen_comptime_eval(cg, s->as.iff.test->as.comptime_expr.body);
      if (val && LLVMIsAConstantInt(val)) {
        uint64_t raw = LLVMConstIntGetZExtValue(val);
        truthy = (raw != 0 && raw != 4 && raw != 1);
        determined = true;
      }
    }
    if (determined) {
      if (truthy) {
        collect_sigs(cg, s->as.iff.conseq);
      } else if (s->as.iff.alt) {
        collect_sigs(cg, s->as.iff.alt);
      }
    } else {
      collect_sigs(cg, s->as.iff.conseq);
      if (s->as.iff.alt)
        collect_sigs(cg, s->as.iff.alt);
    }
  } else if (s->kind == NY_S_BLOCK) {
    for (size_t i = 0; i < s->as.block.body.len; i++)
      collect_sigs(cg, s->as.block.body.data[i]);
  } else if (s->kind == NY_S_MACRO) {
    collect_sigs(cg, s->as.macro.body);
  }
}
