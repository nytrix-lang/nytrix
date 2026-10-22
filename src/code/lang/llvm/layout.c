#include "base/util.h"
#include "code/lang/code.h"
#include "code/lang/priv.h"

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
    if (def->fields.data[i].name && strcmp(def->fields.data[i].name, name) == 0)
      return true;
  }
  return false;
}

static bool ny_layout_array_extent(codegen_t *cg, layout_def_t *def, expr_t *e,
                                   int64_t *out) {
  (void)cg;
  if (!e || !out)
    return false;
  if (e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_INT &&
      e->tok.kind != NY_T_NIL) {
    *out = e->as.literal.as.i;
    return true;
  }
  if (e->kind == NY_E_IDENT && e->as.ident.name && def && def->stmt &&
      def->stmt->kind == NY_S_LAYOUT) {
    for (size_t i = 0; i < def->stmt->as.layout.deftype_params.len; ++i) {
      const param_t *pr = &def->stmt->as.layout.deftype_params.data[i];
      if (pr->name && strcmp(pr->name, e->as.ident.name) == 0) {
        if (i < def->deftype_param_vals.len &&
            def->deftype_param_vals.data[i] >= 0) {
          *out = def->deftype_param_vals.data[i];
          return true;
        }
        if (pr->def && pr->def->kind == NY_E_LITERAL &&
            pr->def->as.literal.kind == NY_LIT_INT &&
            pr->def->tok.kind != NY_T_NIL) {
          *out = pr->def->as.literal.as.i;
          return true;
        }
      }
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
  size_t field_size = tl.size;
  size_t array_count = 0;
  if (field->is_array) {
    int64_t n = 0;
    if (!ny_layout_array_extent(cg, def, field->array_len, &n)) {
      ny_diag_error(tok,
                    "layout array field '%s' extent must be a comptime integer "
                    "(literal or def-bound deftype param already resolved)",
                    field->name);
      cg->had_error = 1;
      return false;
    }
    if (n <= 0) {
      ny_diag_error(tok, "layout array field '%s' extent must be positive",
                    field->name);
      cg->had_error = 1;
      return false;
    }
    field_size = tl.size * (size_t)n;
    array_count = (size_t)n;
  }
  size_t align = tl.align ? tl.align : 1;
  if (field->width > 0)
    align = (size_t)field->width;
  if (def->pack > 0 && align > def->pack)
    align = def->pack;
  *offset = align_up_size(*offset, align);
  layout_field_info_t info = {
      field->name, field->type_name, *offset,    field_size,
      align,       field->is_array,  array_count};
  vec_push(&def->fields, info);
  *offset += field_size;
  if (align > *max_align)
    *max_align = align;
  return true;
}

static LLVMTypeRef build_layout_llvm_type(codegen_t *cg, layout_def_t *def,
                                          token_t tok) {
  if (!cg || !def)
    return NULL;
  size_t elem_cap = def->fields.len * 2 + 1;
  if (elem_cap == 0)
    elem_cap = 1;
  LLVMTypeRef *elems = malloc(sizeof(LLVMTypeRef) * elem_cap);
  if (!elems) {
    ny_diag_error(tok, "out of memory building layout ABI type '%s'",
                  def->name ? def->name : "<anon>");
    cg->had_error = 1;
    return NULL;
  }
  size_t elem_count = 0;

  if (def->pack == 0 && def->align_override == 0) {
    for (size_t i = 0; i < def->fields.len; i++) {
      layout_field_info_t *field = &def->fields.data[i];
      type_layout_t tl = resolve_raw_layout(cg, field->type_name, tok);
      if (!tl.is_valid || !tl.llvm_type) {
        free(elems);
        return NULL;
      }
      elems[elem_count++] = tl.llvm_type;
    }
  } else {
    size_t cursor = 0;
    for (size_t i = 0; i < def->fields.len; i++) {
      layout_field_info_t *field = &def->fields.data[i];
      if (field->offset > cursor)
        elems[elem_count++] =
            LLVMArrayType(cg->type_i8, (unsigned)(field->offset - cursor));
      type_layout_t tl = resolve_raw_layout(cg, field->type_name, tok);
      if (!tl.is_valid || !tl.llvm_type) {
        free(elems);
        return NULL;
      }
      if (field->is_array && field->array_count > 0)
        elems[elem_count++] =
            LLVMArrayType(tl.llvm_type, (unsigned)field->array_count);
      else
        elems[elem_count++] = tl.llvm_type;
      cursor = field->offset + field->size;
    }
    if (def->size > cursor)
      elems[elem_count++] =
          LLVMArrayType(cg->type_i8, (unsigned)(def->size - cursor));
  }

  if (def->llvm_type) {
    LLVMStructSetBody(def->llvm_type, elems, (unsigned)elem_count, false);
    free(elems);
    return def->llvm_type;
  }

  LLVMTypeRef ty =
      LLVMStructTypeInContext(cg->ctx, elems, (unsigned)elem_count, false);
  free(elems);
  return ty;
}

void ny_register_layout_def(codegen_t *cg, stmt_t *s, bool is_layout) {
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
    if (def && def->name && strcmp(def->name, name) == 0)
      return;
  }
  layout_def_t *def = arena_alloc(cg->arena, sizeof(layout_def_t));
  memset(def, 0, sizeof(layout_def_t));
  def->name = ny_strdup(name);
  def->stmt = s;
  def->is_layout = is_layout;
  def->align_override = align_override;
  def->pack = pack;
  if (is_layout && s->as.layout.deftype_params.len > 0) {
    for (size_t i = 0; i < s->as.layout.deftype_params.len; i++) {
      const char *pname = s->as.layout.deftype_params.data[i].name;
      if (pname)
        vec_push(&def->deftype_param_names, pname);
      vec_push(&def->deftype_param_vals, (int64_t)-1);
    }
  }
  size_t offset = 0;
  size_t max_align = 1;
  for (size_t i = 0; i < fields->len; i++)
    layout_add_field(cg, def, &fields->data[i], s->tok, &offset, &max_align);
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
  def->llvm_type = build_layout_llvm_type(cg, def, s->tok);
  s->sema = def;
  s->sema_kind = NY_STMT_SEMA_LAYOUT;
  vec_push(&cg->layouts, def);
}

layout_def_t *ny_layout_instantiate(codegen_t *cg, const char *inst_name,
                                    const char *generic_name, int64_t bound) {
  if (!cg || !inst_name || !generic_name)
    return NULL;
  for (size_t i = 0; i < cg->layouts.len; i++)
    if (cg->layouts.data[i] && cg->layouts.data[i]->name &&
        strcmp(cg->layouts.data[i]->name, inst_name) == 0)
      return cg->layouts.data[i];
  layout_def_t *gen = lookup_layout(cg, generic_name);
  if (!gen || !gen->is_layout || gen->deftype_param_names.len == 0 ||
      !gen->stmt || gen->stmt->kind != NY_S_LAYOUT)
    return NULL;
  layout_def_t *def = arena_alloc(cg->arena, sizeof(layout_def_t));
  memset(def, 0, sizeof(layout_def_t));
  def->name = ny_strdup(inst_name);
  def->stmt = gen->stmt;
  def->is_layout = true;
  def->align_override = gen->align_override;
  def->pack = gen->pack;
  for (size_t i = 0; i < gen->deftype_param_names.len &&
                     i < gen->stmt->as.layout.deftype_params.len;
       i++) {
    vec_push(&def->deftype_param_names, gen->deftype_param_names.data[i]);
    vec_push(&def->deftype_param_vals, i == 0 ? bound : (int64_t)-1);
  }
  size_t offset = 0;
  size_t max_align = 1;
  ny_layout_field_list *fields = &gen->stmt->as.layout.fields;
  for (size_t i = 0; i < fields->len; i++)
    layout_add_field(cg, def, &fields->data[i], gen->stmt->tok, &offset,
                     &max_align);
  size_t effective_align = max_align;
  if (def->pack > 0 && def->pack < effective_align)
    effective_align = def->pack;
  if (def->align_override > 0 && def->align_override >= effective_align)
    effective_align = def->align_override;
  def->align = effective_align;
  def->size = align_up_size(offset, def->align);
  def->llvm_type = build_layout_llvm_type(cg, def, gen->stmt->tok);
  vec_push(&cg->layouts, def);
  return def;
}
