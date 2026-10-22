#ifndef NY_BACKEND_LLVM_BRIDGE_H
#define NY_BACKEND_LLVM_BRIDGE_H

#include <llvm-c/Core.h>
#include <llvm-c/DebugInfo.h>
#include <llvm-c/TargetMachine.h>
#include <stdbool.h>

#include <nyr/ir.h>

#include "code/backend/ir/module.h"

/* LLVM state required by the neutral NYIR emitter. Language-specific symbol
 * provenance and debug scope construction enter only through callbacks. */
typedef struct ny_llvm_context_t {
  LLVMContextRef context;
  LLVMModuleRef module;
  LLVMBuilderRef builder;
  LLVMTypeRef type_i8;
  LLVMTypeRef type_i64;
  LLVMTypeRef type_f32;
  LLVMTypeRef type_f64;
  LLVMDIBuilderRef di_builder;
  LLVMMetadataRef di_loc;
  LLVMMetadataRef di_scope;
  bool debug_symbols;
  bool module_declarations_only;
  bool emit_entry;
  const char *default_filename;
  const char *(*origin_section)(void *userdata, const char *symbol);
  LLVMMetadataRef (*debug_scope)(void *userdata, const char *filename,
                                 unsigned line, unsigned column);
  LLVMMetadataRef (*debug_subprogram)(void *userdata, LLVMValueRef function,
                                      const char *name, const char *filename,
                                      unsigned line, unsigned column);
  LLVMValueRef (*global_symbol)(void *userdata, const char *name);
  void *userdata;
} ny_llvm_context_t;

bool ny_llvm_init_native(void);
bool ny_llvm_init_all_targets(void);
void ny_llvm_prepare_module(LLVMModuleRef module, int opt_level);
void ny_llvm_optimize_module(LLVMModuleRef module, int opt_level, int opt_loops,
                             const char *opt_pipeline);
bool ny_llvm_apply_sanitize(LLVMModuleRef module, const char *sanitize_kind);
void ny_llvm_apply_host_attrs(LLVMModuleRef module);
bool ny_llvm_emit_object(LLVMModuleRef module, const char *path, int opt_level);
bool ny_llvm_emit_file(LLVMModuleRef module, const char *path,
                       LLVMCodeGenFileType kind, int opt_level);
LLVMTypeRef ny_llvm_ptr_type(LLVMContextRef ctx);
LLVMValueRef ny_llvm_const_gep2(LLVMTypeRef elem_ty, LLVMValueRef base,
                                LLVMValueRef *indices, unsigned count);
void ny_llvm_clear_function(LLVMValueRef f);

bool ny_llvm_emit_nyir_func(ny_llvm_context_t *context, nyr_func_t *f,
                            const char *name, bool tag_return, char *err,
                            size_t err_len);
/* Consumes and frees each function body as it is emitted. */
bool ny_llvm_emit_nyir_module(ny_llvm_context_t *context,
                              nyr_backend_module_t *module, char *err,
                              size_t err_len);

#endif
