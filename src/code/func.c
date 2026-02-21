#include "base/util.h"
#include "braun.h"
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

static inline void braun_seed_local_value(codegen_t *cg, const char *name,
                                          LLVMValueRef value) {
  if (!cg || !cg->braun || !name || !value)
    return;
  ny_braun_mark_current_block(cg);
  braun_ssa_write_var(cg->braun, name, value);
}

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

static void add_fn_enum_attr(codegen_t *cg, LLVMValueRef fn, const char *name) {
  if (!cg || !fn || !name)
    return;
  unsigned kind_id =
      LLVMGetEnumAttributeKindForName(name, (unsigned)strlen(name));
  if (kind_id == 0)
    return;
  LLVMAttributeRef attr = LLVMCreateEnumAttribute(cg->ctx, kind_id, 0);
  LLVMAddAttributeAtIndex(fn, LLVMAttributeFunctionIndex, attr);
}

static void add_fn_string_attr(codegen_t *cg, LLVMValueRef fn, const char *name,
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
  if (is_pure) {
    decl->effect_contract_known = true;
    decl->effect_contract_mask = NY_FX_NONE;
  } else {
    decl->effect_contract_known = has_effect_contract;
    decl->effect_contract_mask =
        has_effect_contract ? effect_contract_mask : NY_FX_ALL;
  }
  decl->attrs_resolved = true;
}

static void apply_fn_attrs(codegen_t *cg, LLVMValueRef fn,
                           const stmt_t *fn_stmt) {
  if (!cg || !fn || !fn_stmt || fn_stmt->kind != NY_S_FUNC)
    return;
  const stmt_func_t *decl = &fn_stmt->as.fn;
  if (cg->debug_symbols) {
    /*
     * Keep call stacks and stepping stable in debug binaries even when users
     * enable optimization passes.
     */
    add_fn_string_attr(cg, fn, "frame-pointer", "all");
    add_fn_string_attr(cg, fn, "disable-tail-calls", "true");
    add_fn_string_attr(cg, fn, "no-frame-pointer-elim", "true");
    add_fn_string_attr(cg, fn, "no-frame-pointer-elim-non-leaf", "true");
    add_fn_enum_attr(cg, fn, "uwtable");
  }
  if (decl->attr_naked) {
    add_fn_enum_attr(cg, fn, "naked");
  }
  if (decl->attr_jit) {
    add_fn_enum_attr(cg, fn, "alwaysinline");
    add_fn_enum_attr(cg, fn, "hot");
  }
  if (decl->attr_thread) {
    add_fn_enum_attr(cg, fn, "noinline");
    add_fn_enum_attr(cg, fn, "cold");
  }
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
    // Lambda has its own function scope; assignments there should not force
    // stack slots in the enclosing function.
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
    // Nested function body has its own scope and capture model; do not treat
    // its assignments as local mutations of the enclosing function.
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

void gen_func(codegen_t *cg, stmt_t *fn, const char *name, scope *scopes,
              size_t depth, binding_list *captures) {
  resolve_fn_attrs(cg, fn);
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
    // Overwrite: remove existing basic blocks if any
    LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(f);
    while (bb) {
      LLVMBasicBlockRef next = LLVMGetNextBasicBlock(bb);
      LLVMDeleteBasicBlock(bb);
      bb = next;
    }
  }
  if (cg->skip_stdlib && fn->tok.filename &&
      (strstr(fn->tok.filename, "std.ny") ||
       strstr(fn->tok.filename, "lib.ny") ||
       strncmp(fn->tok.filename, "<stdlib>", 8) == 0 ||
       strstr(fn->tok.filename, "/share/nytrix/"))) {
    return;
  }
  LLVMBasicBlockRef cur = LLVMGetInsertBlock(cg->builder);
  LLVMPositionBuilderAtEnd(cg->builder, LLVMAppendBasicBlock(f, "entry"));
  if (cg->braun)
    braun_ssa_reset(cg->braun);
  ny_braun_mark_current_block(cg);
  LLVMMetadataRef prev_scope = cg->di_scope;
  if (cg->debug_symbols && cg->di_builder) {
    LLVMMetadataRef sp = codegen_debug_subprogram(cg, f, name, fn->tok);
    if (sp)
      cg->di_scope = sp;
  }
  ny_dbg_loc(cg, fn->tok);
  if (!fn->as.fn.attr_naked)
    emit_trace_func(cg, name ? name : "<anon>");
  size_t fd = depth + 1;
  size_t root = fd;
  assigned_name_list assigned_names = {0};
  assigned_hash_list assigned_hashes = {0};
  uint64_t assigned_bloom[4] = {0, 0, 0, 0};
  bool use_assigned_prepass = ny_env_enabled("NYTRIX_ASSIGNED_PREPASS");
  if (use_assigned_prepass) {
    collect_assigned_names_stmt(fn->as.fn.body, &assigned_names,
                                &assigned_hashes, assigned_bloom);
  }
  // Init scope
  memset(&scopes[fd], 0, sizeof(scopes[fd]));
  size_t param_offset = 0;

  if (!fn->as.fn.attr_naked) {
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
        bool needs_slot =
            !use_assigned_prepass ||
            (captures->data[i].is_mut &&
             assigned_name_contains(&assigned_names, &assigned_hashes,
                                    assigned_bloom, captures->data[i].name));
        // Keep mutable captures addressable only when reassigned in this body.
        if (needs_slot) {
          LLVMValueRef lv =
              build_alloca(cg, captures->data[i].name, cg->type_i64);
          LLVMBuildStore(cg->builder, val, lv);
          scope_bind(cg, scopes, fd, captures->data[i].name, lv,
                     captures->data[i].stmt_t ? captures->data[i].stmt_t : fn,
                     true, captures->data[i].type_name, true);
          braun_seed_local_value(cg, captures->data[i].name, val);
        } else {
          scope_bind(cg, scopes, fd, captures->data[i].name, val,
                     captures->data[i].stmt_t ? captures->data[i].stmt_t : fn,
                     captures->data[i].is_mut, captures->data[i].type_name,
                     false);
        }
        // Mark as used in the closure's local scope since they were implicitly
        // captured
        scopes[fd].vars.data[scopes[fd].vars.len - 1].is_used = true;
      }
    }
    for (size_t i = 0; i < fn->as.fn.params.len; i++) {
      const char *param_name = fn->as.fn.params.data[i].name;
      LLVMValueRef param_val = LLVMGetParam(f, (unsigned)(i + param_offset));
      bool needs_slot =
          !use_assigned_prepass ||
          assigned_name_contains(&assigned_names, &assigned_hashes,
                                 assigned_bloom, param_name);
      if (needs_slot) {
        LLVMValueRef slot = build_alloca(cg, param_name, cg->type_i64);
        LLVMBuildStore(cg->builder, param_val, slot);
        scope_bind(cg, scopes, fd, param_name, slot, fn, true,
                   fn->as.fn.params.data[i].type, true);
        if (cg->debug_symbols && cg->di_builder) {
          codegen_debug_variable(cg, param_name, slot, fn->tok, true,
                                 (int)i + 1 + (int)param_offset, true);
        }
        braun_seed_local_value(cg, param_name, param_val);
      } else {
        // SSA fast path: keep immutable-in-practice params as values.
        scope_bind(cg, scopes, fd, param_name, param_val, fn, true,
                   fn->as.fn.params.data[i].type, false);
        if (cg->debug_symbols && cg->di_builder) {
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
  bool prev_naked = cg->current_fn_attr_naked;
  cg->current_fn_ret_type = fn->as.fn.return_type;
  cg->current_fn_attr_naked = fn->as.fn.attr_naked;
  gen_stmt(cg, scopes, &fd, fn->as.fn.body, root, true);
  cg->current_fn_ret_type = prev_ret;
  cg->current_fn_attr_naked = prev_naked;

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
      LLVMBuildRet(cg->builder, LLVMConstInt(cg->type_i64, 1, false));
    }
  }
  scope_pop(scopes, &fd);
  vec_free(&assigned_names);
  vec_free(&assigned_hashes);
  cg->di_scope = prev_scope;
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
        resolve_type_name(cg, s->as.fn.return_type, s->tok);
    for (size_t j = 0; j < s->as.fn.params.len; j++) {
      LLVMTypeRef param_ty =
          resolve_type_name(cg, s->as.fn.params.data[j].type, s->tok);
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

    LLVMValueRef f = LLVMGetNamedFunction(cg->module, final_name);
    if (!f)
      f = LLVMAddFunction(cg->module, final_name, ft);

    resolve_fn_attrs(cg, s);
    apply_fn_attrs(cg, f, s);

    LLVMSetAlignment(f, 16);
    fun_sig sig = {.name = ny_strdup(final_name),
                   .type = ft,
                   .value = f,
                   .stmt_t = s,
                   .arity = (int)s->as.fn.params.len,
                   .is_variadic = s->as.fn.is_variadic,
                   .is_extern = false,
                   .effects = NY_FX_ALL,
                   .args_escape = true,
                   .args_mutated = true,
                   .returns_alias = true,
                   .effects_known = false,
                   .link_name = NULL,
                   .return_type = s->as.fn.return_type
                                      ? ny_strdup(s->as.fn.return_type)
                                      : NULL,
                   .owned = false,
                   .name_hash = 0};
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
    fun_sig sig = {
        .name = ny_strdup(final_name),
        .type = ft,
        .value = f,
        .stmt_t = s,
        .arity = (int)param_count,
        .is_variadic = s->as.ext.is_variadic,
        .is_extern = true,
        .effects = NY_FX_ALL,
        .args_escape = true,
        .args_mutated = true,
        .returns_alias = true,
        .effects_known = false,
        .link_name =
            s->as.ext.link_name ? ny_strdup(s->as.ext.link_name) : NULL,
        .return_type =
            s->as.ext.return_type ? ny_strdup(s->as.ext.return_type) : NULL,
        .owned = false,
        .name_hash = 0};
    vec_push(&cg->fun_sigs, sig);
  } else if (s->kind == NY_S_VAR) {
    for (size_t j = 0; j < s->as.var.names.len; j++) {
      const char *n = s->as.var.names.data[j];
      const char *final_name = codegen_qname(cg, n, cg->current_module_name);
      // Exact indexed lookup avoids repeated linear scans while collecting
      // module/global signatures.
      bool found = lookup_global_exact(cg, final_name) != NULL;
      if (!found) {
        LLVMValueRef g = LLVMAddGlobal(cg->module, cg->type_i64, final_name);
        LLVMSetInitializer(g, LLVMConstInt(cg->type_i64, 0, false));
        const char *type_name = NULL;
        if (s->as.var.types.len > j)
          type_name = s->as.var.types.data[j];
        binding b = {.name = ny_strdup(final_name),
                     .value = g,
                     .stmt_t = s,
                     .is_slot = true,
                     .is_mut = s->as.var.is_mut,
                     .is_used = false,
                     .owned = false,
                     .type_name = type_name,
                     .decl_type_name = type_name,
                     .name_hash = 0};
        vec_push(&cg->global_vars, b);
        if (cg->debug_symbols && cg->di_builder) {
          codegen_debug_global_variable(cg, final_name, g, s->tok);
        }
      }
    }
    // Semantic analysis for types
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
          member.value = cg->current_enum_val++;
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
  } else if (s->kind == NY_S_IF) {
    bool truthy = false;
    bool determined = false;
    if (s->as.iff.test->kind == NY_E_COMPTIME) {
      LLVMValueRef val =
          gen_comptime_eval(cg, s->as.iff.test->as.comptime_expr.body);
      if (val && LLVMIsAConstantInt(val)) {
        uint64_t raw = LLVMConstIntGetZExtValue(val);
        // Nytrix truthiness: not None (0), not false (4), not 0 (1)
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
