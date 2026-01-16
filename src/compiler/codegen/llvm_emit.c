#include "llvm_emit.h"
#include "common.h"
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Analysis.h>
#include <stdio.h>
#include <stdlib.h>

bool nt_llvm_init_native(void) {
	static bool initialized = false;
	if (initialized) return true;
	LLVMInitializeNativeTarget();
	LLVMInitializeNativeAsmPrinter();
	LLVMInitializeNativeAsmParser();
	initialized = true;
	return true;
}

bool nt_llvm_emit_object(LLVMModuleRef module, const char *path) {
	if (!module || !path) return false;
	if (!nt_llvm_init_native()) return false;
	char *triple = LLVMGetDefaultTargetTriple();
	if (!triple) return false;
	LLVMTargetRef target;
	char *err = NULL;
	if (LLVMGetTargetFromTriple(triple, &target, &err)) {
		NT_LOG_ERR("Invalid target triple: %s\n", err);
		LLVMDisposeMessage(err);
		LLVMDisposeMessage(triple);
		return false;
	}
	char *cpu = LLVMGetHostCPUName();
	char *features = LLVMGetHostCPUFeatures();
	LLVMTargetMachineRef tm = LLVMCreateTargetMachine(target, triple, cpu, features,
													  LLVMCodeGenLevelDefault,
													  LLVMRelocDefault,
													  LLVMCodeModelDefault);
	if (!tm) {
		NT_LOG_ERR("Failed to create target machine\n");
		LLVMDisposeMessage(triple);
		if (cpu) LLVMDisposeMessage(cpu);
		if (features) LLVMDisposeMessage(features);
		return false;
	}
	LLVMTargetDataRef td = LLVMCreateTargetDataLayout(tm);
	char *layout = LLVMCopyStringRepOfTargetData(td);
	if (layout) {
		LLVMSetDataLayout(module, layout);
		LLVMDisposeMessage(layout);
	}
	LLVMDisposeTargetData(td);
	LLVMSetTarget(module, triple);
	char *emit_err = NULL;
	int res = LLVMTargetMachineEmitToFile(tm, module, (char*)path, LLVMObjectFile, &emit_err);
	if (emit_err) {
		NT_LOG_ERR("Object emission failed: %s\n", emit_err);
		LLVMDisposeMessage(emit_err);
	}
	LLVMDisposeMessage(triple);
	if (cpu) LLVMDisposeMessage(cpu);
	if (features) LLVMDisposeMessage(features);
	LLVMDisposeTargetMachine(tm);
	return res == 0;
}

bool nt_llvm_emit_file(LLVMModuleRef module, const char *path, LLVMCodeGenFileType kind) {
	if (!module || !path) return false;
	if (!nt_llvm_init_native()) return false;
	char *triple = LLVMGetDefaultTargetTriple();
	if (!triple) return false;
	LLVMTargetRef target;
	char *err = NULL;
	if (LLVMGetTargetFromTriple(triple, &target, &err)) {
		NT_LOG_ERR("Invalid target triple: %s\n", err);
		LLVMDisposeMessage(err);
		LLVMDisposeMessage(triple);
		return false;
	}
	char *cpu = LLVMGetHostCPUName();
	char *features = LLVMGetHostCPUFeatures();
	LLVMTargetMachineRef tm = LLVMCreateTargetMachine(target, triple, cpu, features,
													  LLVMCodeGenLevelDefault,
													  LLVMRelocDefault,
													  LLVMCodeModelDefault);
	if (!tm) {
		NT_LOG_ERR("Failed to create target machine\n");
		LLVMDisposeMessage(triple);
		if (cpu) LLVMDisposeMessage(cpu);
		if (features) LLVMDisposeMessage(features);
		return false;
	}
	char *emit_err = NULL;
	int res = LLVMTargetMachineEmitToFile(tm, module, (char *)path, kind, &emit_err);
	if (emit_err) {
		NT_LOG_ERR("Emission failed: %s\n", emit_err);
		LLVMDisposeMessage(emit_err);
	}
	LLVMDisposeMessage(triple);
	if (cpu) LLVMDisposeMessage(cpu);
	if (features) LLVMDisposeMessage(features);
	LLVMDisposeTargetMachine(tm);
	return res == 0;
}
