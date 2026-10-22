/*
 * Emit immutable string and compile-time buffer storage for LLVM modules.
 */
#include "code/backend/abi.h"
#include "code/backend/internal.h"
#include "code/lang/llvm/internal.h"
#include "code/lang/priv.h"
#include "code/lang/code.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LLVMValueRef codegen_emit_string_or_array_global(codegen_t *cg,
                                                 const char *name) {
  if (!cg || !cg->module || !name)
    return NULL;

  size_t str_len = 0;
  const char *str_bytes = nyr_strtab_get(name, &str_len);
  if (str_bytes) {
    LLVMValueRef str_const =
        LLVMConstString(str_bytes, (unsigned)str_len, false);
    LLVMValueRef sym = LLVMAddGlobal(cg->module, LLVMTypeOf(str_const), name);
    LLVMSetInitializer(sym, str_const);
    LLVMSetGlobalConstant(sym, true);
    LLVMSetLinkage(sym, LLVMPrivateLinkage);
    LLVMSetAlignment(sym, 16);
    return sym;
  }

  const nyr_array_elem_t *elems = NULL;
  size_t count = 0;
  size_t stride = 0;
  if (!nyr_arraytab_get(name, &elems, &count, &stride))
    return NULL;

  size_t total_bytes = 32 + count * stride;
  if (stride == 24) {
    for (size_t k = 0; k < count; ++k)
      if (elems[k].str)
        total_bytes += elems[k].str_len + 1;
  }
  uint8_t *buf = calloc(1, total_bytes);
  if (!buf)
    return NULL;

  uint64_t *hdr = (uint64_t *)buf;
  hdr[0] = (uint64_t)NY_NATIVE_TBUF_MAGIC;
  hdr[1] = count;
  hdr[2] = stride;
  hdr[3] = count;
  size_t off = 32;
  size_t string_base = count * stride;
  size_t string_cursor = 0;
  for (size_t k = 0; k < count; ++k) {
    int64_t words[3] = {elems[k].value, 0, elems[k].tag};
    if (stride == 24 && elems[k].str) {
      words[0] = (int64_t)(string_base + string_cursor - k * 24);
      words[1] = (int64_t)elems[k].str_len;
      words[2] = 121;
    }
    for (size_t w = 0; w < stride / 8; ++w) {
      uint64_t raw = (uint64_t)words[w];
      memcpy(buf + off, &raw, 8);
      off += 8;
    }
    if (stride == 24 && elems[k].str)
      string_cursor += elems[k].str_len + 1;
  }
  if (stride == 24) {
    for (size_t k = 0; k < count; ++k) {
      if (elems[k].str) {
        memcpy(buf + off, elems[k].str, elems[k].str_len + 1);
        off += elems[k].str_len + 1;
      }
    }
  }

  LLVMValueRef bytes_const =
      LLVMConstString((const char *)buf, (unsigned)total_bytes, true);
  char raw_name[128];
  snprintf(raw_name, sizeof(raw_name), "%s.raw", name);
  LLVMValueRef base_global =
      LLVMAddGlobal(cg->module, LLVMTypeOf(bytes_const), raw_name);
  LLVMSetInitializer(base_global, bytes_const);
  LLVMSetGlobalConstant(base_global, false);
  LLVMSetLinkage(base_global, LLVMPrivateLinkage);
  LLVMSetAlignment(base_global, 16);
  LLVMValueRef indices[] = {LLVMConstInt(cg->type_i64, 0, false),
                            LLVMConstInt(cg->type_i64, 32, false)};
  LLVMValueRef gep =
      LLVMConstInBoundsGEP2(LLVMTypeOf(bytes_const), base_global, indices, 2);
  /*
   * Keep the payload address as a constant GEP. A GlobalAlias adds no
   * semantic value and can carry an inconsistent pointee type with opaque
   * pointers.
   */
  free(buf);
  return gep;
}

/*
 * String runtime initialization: const-string globals used by the
 * entry initializer (merged from string_init.c).
 */
static LLVMValueRef ny_replace_llvm_used_global(LLVMModuleRef module,
                                                LLVMTypeRef elem_ty,
                                                LLVMValueRef *elements,
                                                size_t count) {
  if (!module || !elem_ty || !elements || count == 0 || count > UINT_MAX)
    return NULL;
  LLVMValueRef old = LLVMGetNamedGlobal(module, "llvm.used");
  if (old)
    LLVMDeleteGlobal(old);
  LLVMTypeRef arr_ty = LLVMArrayType(elem_ty, (unsigned)count);
  LLVMValueRef arr = LLVMConstArray(elem_ty, elements, (unsigned)count);
  LLVMValueRef used = LLVMAddGlobal(module, arr_ty, "llvm.used");
  LLVMSetLinkage(used, LLVMAppendingLinkage);
  LLVMSetSection(used, "llvm.metadata");
  LLVMSetGlobalConstant(used, true);
  LLVMSetInitializer(used, arr);
  return used;
}

static LLVMValueRef ny_const_string_runtime_initializer(
    codegen_t *cg, LLVMValueRef str_array_global,
    LLVMValueRef runtime_ptr_global, LLVMTypeRef i8_ty) {
  if (!cg || !str_array_global || !runtime_ptr_global)
    return NULL;
  LLVMValueRef indices[] = {LLVMConstInt(cg->type_i64, 64, false)};
  LLVMValueRef str_data_ptr =
      LLVMConstInBoundsGEP2(i8_ty, str_array_global, indices, 1);
  LLVMTypeRef value_ty = LLVMGlobalGetValueType(runtime_ptr_global);
  LLVMTypeKind value_kind = LLVMGetTypeKind(value_ty);
  if (value_kind == LLVMPointerTypeKind)
    return LLVMConstPointerCast(str_data_ptr, value_ty);
  if (value_kind == LLVMIntegerTypeKind)
    return LLVMConstPtrToInt(str_data_ptr, value_ty);
  return NULL;
}

static bool ny_set_const_string_runtime_initializer(
    codegen_t *cg, LLVMValueRef str_array_global,
    LLVMValueRef runtime_ptr_global, LLVMTypeRef i8_ty) {
  LLVMValueRef init = ny_const_string_runtime_initializer(
      cg, str_array_global, runtime_ptr_global, i8_ty);
  if (!init)
    return false;
  LLVMSetInitializer(runtime_ptr_global, init);
  return true;
}

void codegen_emit_string_init(codegen_t *cg) {
  LLVMTypeRef i8_ptr_ty = LLVMPointerType(ny_i8_ty(cg), 0);
  LLVMTypeRef i8_ty = ny_i8_ty(cg);
  bool const_string_global_init =
      ny_codegen_speed_profile_enabled(cg) ||
      ny_fast_path_enabled(cg, "NYTRIX_CONST_STRING_GLOBAL_INIT");

  size_t str_count = 0;
  bool init_all = (cg->current_module_name == NULL);
  for (size_t i = 0; i < cg->interns.len; i++) {
    if ((init_all || cg->interns.data[i].module == cg->module) &&
        cg->interns.data[i].gv && cg->interns.data[i].val)
      str_count++;
  }

  if (str_count > 0) {
    LLVMValueRef *elements = malloc(str_count * sizeof(LLVMValueRef));
    if (!elements)
      return;
    size_t idx = 0;
    for (size_t i = 0; i < cg->interns.len; i++) {
      string_intern *si = &cg->interns.data[i];
      if ((!init_all && si->module != cg->module) || !si->gv || !si->val)
        continue;
      elements[idx++] = LLVMConstPointerCast(si->gv, i8_ptr_ty);
    }

    LLVMValueRef used_global = LLVMGetNamedGlobal(cg->module, "llvm.used");
    if (used_global) {
      LLVMValueRef old_init = LLVMGetInitializer(used_global);
      size_t old_count = LLVMGetArrayLength(LLVMTypeOf(old_init));
      if (old_count > SIZE_MAX - str_count ||
          old_count + str_count > SIZE_MAX / sizeof(LLVMValueRef)) {
        free(elements);
        return;
      }
      size_t new_count = old_count + str_count;
      LLVMValueRef *new_elements = malloc(new_count * sizeof(LLVMValueRef));
      if (!new_elements) {
        free(elements);
        return;
      }
      for (size_t j = 0; j < old_count; j++)
        new_elements[j] = LLVMGetAggregateElement(old_init, (unsigned)j);
      for (size_t j = 0; j < str_count; j++)
        new_elements[old_count + j] = elements[j];
      ny_replace_llvm_used_global(cg->module, i8_ptr_ty, new_elements,
                                  new_count);
      free(new_elements);
    } else {
      ny_replace_llvm_used_global(cg->module, i8_ptr_ty, elements, str_count);
    }
    free(elements);
  }

  if (cg->emit_module_decls_only) {
    LLVMValueRef g = LLVMGetFirstGlobal(cg->module);
    while (g) {
      const char *gname = LLVMGetValueName(g);
      if (gname && strncmp(gname, ".str.data.", 10) == 0) {
        bool found = false;
        LLVMValueRef ug = LLVMGetNamedGlobal(cg->module, "llvm.used");
        if (ug) {
          LLVMValueRef init = LLVMGetInitializer(ug);
          size_t uc = LLVMGetArrayLength(LLVMTypeOf(init));
          for (size_t j = 0; j < uc; j++) {
            LLVMValueRef elem = LLVMGetAggregateElement(init, (unsigned)j);
            if (elem == g ||
                (elem && LLVMIsAConstantExpr(elem) &&
                 LLVMGetOperand(LLVMIsAConstantExpr(elem), 0) == g)) {
              found = true;
              break;
            }
          }
        }
        if (!found) {
          LLVMValueRef cast = LLVMConstPointerCast(g, i8_ptr_ty);
          LLVMValueRef ug2 = LLVMGetNamedGlobal(cg->module, "llvm.used");
          if (ug2) {
            LLVMValueRef old_init = LLVMGetInitializer(ug2);
            size_t old_count = LLVMGetArrayLength(LLVMTypeOf(old_init));
            if (old_count == SIZE_MAX ||
                old_count + 1 > SIZE_MAX / sizeof(LLVMValueRef))
              return;
            size_t new_count = old_count + 1;
            LLVMValueRef *new_elements =
                malloc(new_count * sizeof(LLVMValueRef));
            if (!new_elements)
              return;
            for (size_t j = 0; j < old_count; j++)
              new_elements[j] = LLVMGetAggregateElement(old_init, (unsigned)j);
            new_elements[old_count] = cast;
            ny_replace_llvm_used_global(cg->module, i8_ptr_ty, new_elements,
                                        new_count);
            free(new_elements);
          }
        }
      }
      g = LLVMGetNextGlobal(g);
    }
  }

  for (size_t i = 0; i < cg->interns.len; i++) {
    if (!init_all && cg->interns.data[i].module != cg->module)
      continue;
    LLVMValueRef str_array_global = cg->interns.data[i].gv;
    LLVMValueRef runtime_ptr_global = cg->interns.data[i].val;
    if (!str_array_global || !runtime_ptr_global)
      continue;
    if (const_string_global_init &&
        ny_set_const_string_runtime_initializer(cg, str_array_global,
                                                runtime_ptr_global, i8_ty))
      continue;
    LLVMTypeRef rt_ty = LLVMTypeOf(runtime_ptr_global);

    LLVMValueRef indices[] = {LLVMConstInt(cg->type_i64, 64, 0)};
    LLVMValueRef str_data_ptr = LLVMBuildInBoundsGEP2(
        cg->builder, i8_ty, str_array_global, indices, 1, "");
    if (LLVMGetTypeKind(rt_ty) == LLVMPointerTypeKind) {
      ny_store(cg, runtime_ptr_global, str_data_ptr);
    } else {
      LLVMValueRef str_data_int = ny_ptr2i64(cg, str_data_ptr, "");
      ny_store(cg, runtime_ptr_global, str_data_int);
    }
  }

  if (init_all) {
    size_t old_intern_len = cg->interns.len;
    codegen_repopulate_interns(cg);
    for (size_t i = old_intern_len; i < cg->interns.len; i++) {
      if (cg->interns.data[i].module != cg->module)
        continue;
      LLVMValueRef str_array_global = cg->interns.data[i].gv;
      LLVMValueRef runtime_ptr_global = cg->interns.data[i].val;
      if (!str_array_global || !runtime_ptr_global)
        continue;
      if (const_string_global_init &&
          ny_set_const_string_runtime_initializer(cg, str_array_global,
                                                  runtime_ptr_global, i8_ty))
        continue;
      LLVMTypeRef rt_ty = LLVMTypeOf(runtime_ptr_global);

      LLVMValueRef indices[] = {LLVMConstInt(cg->type_i64, 64, 0)};
      LLVMValueRef str_data_ptr = LLVMBuildInBoundsGEP2(
          cg->builder, i8_ty, str_array_global, indices, 1, "");
      if (LLVMGetTypeKind(rt_ty) == LLVMPointerTypeKind) {
        ny_store(cg, runtime_ptr_global, str_data_ptr);
      } else {
        LLVMValueRef str_data_int = ny_ptr2i64(cg, str_data_ptr, "");
        ny_store(cg, runtime_ptr_global, str_data_int);
      }
    }
  }
}
