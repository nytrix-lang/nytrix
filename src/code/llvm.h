#ifndef NY_CODEGEN_EMIT_H
#define NY_CODEGEN_EMIT_H

#include <llvm-c/Core.h>
#include <llvm-c/TargetMachine.h>
#include <stdbool.h>

bool ny_llvm_init_native(void);
void ny_llvm_prepare_module(LLVMModuleRef module);
void ny_llvm_apply_host_attrs(LLVMModuleRef module);
bool ny_llvm_emit_object(LLVMModuleRef module, const char *path);
bool ny_llvm_emit_file(LLVMModuleRef module, const char *path,
                       LLVMCodeGenFileType kind);
LLVMTypeRef ny_llvm_ptr_type(LLVMContextRef ctx);
LLVMValueRef ny_llvm_const_gep2(LLVMTypeRef elem_ty, LLVMValueRef base,
                                LLVMValueRef *indices, unsigned count);

#endif
