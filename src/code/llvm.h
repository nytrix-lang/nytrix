#ifndef NY_CODEGEN_EMIT_H
#define NY_CODEGEN_EMIT_H

#include <llvm-c/Core.h>
#include <llvm-c/TargetMachine.h>
#include <stdbool.h>

bool ny_llvm_init_native(void);
bool ny_llvm_emit_object(LLVMModuleRef module, const char *path);
bool ny_llvm_emit_file(LLVMModuleRef module, const char *path,
                       LLVMCodeGenFileType kind);

#endif
