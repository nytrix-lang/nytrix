#ifndef NT_CODEGEN_EMIT_H
#define NT_CODEGEN_EMIT_H

#include <stdbool.h>
#include <llvm-c/Core.h>
#include <llvm-c/TargetMachine.h>

bool nt_llvm_init_native(void);
bool nt_llvm_emit_object(LLVMModuleRef module, const char *path);
bool nt_llvm_emit_file(LLVMModuleRef module, const char *path, LLVMCodeGenFileType kind);

#endif
