#include "code/priv.h"
#include "base/common.h"
#include <llvm-c/Core.h>
#include <llvm-c/DebugInfo.h>
#include <string.h>

static LLVMMetadataRef debug_file_for(codegen_t *cg, const char *filename) {
  if (!cg || !cg->di_builder)
    return NULL;
  const char *use_name =
      (filename && *filename) ? filename
                              : (cg->debug_main_file ? cg->debug_main_file
                                                     : "<unknown>");
  const char *base = use_name;
  const char *dir = ".";
  char *dir_buf = NULL;
  const char *slash = strrchr(use_name, '/');
  if (slash && slash != use_name) {
    base = slash + 1;
    dir_buf = ny_strndup(use_name, (size_t)(slash - use_name));
    dir = dir_buf;
  }
  LLVMMetadataRef file =
      LLVMDIBuilderCreateFile(cg->di_builder, base, strlen(base), dir,
                              strlen(dir));
  if (dir_buf)
    free(dir_buf);
  return file ? file : cg->di_file;
}

void codegen_debug_init(codegen_t *cg, const char *main_file) {
  if (!cg || !cg->debug_symbols || cg->di_builder)
    return;
  cg->debug_main_file = (main_file && *main_file) ? main_file : "<inline>";
  cg->di_builder = LLVMCreateDIBuilder(cg->module);
  cg->di_file = debug_file_for(cg, cg->debug_main_file);
  cg->di_subroutine_type = LLVMDIBuilderCreateSubroutineType(
      cg->di_builder, cg->di_file, NULL, 0, LLVMDIFlagZero);
  cg->di_cu = LLVMDIBuilderCreateCompileUnit(
      cg->di_builder, LLVMDWARFSourceLanguageC, cg->di_file, "nytrix", 6, 0, "",
      0, 0, "", 0, LLVMDWARFEmissionFull, 0, 0, 0, "", 0, "", 0);

  LLVMMetadataRef dbg_ver = LLVMValueAsMetadata(LLVMConstInt(
      LLVMInt32TypeInContext(cg->ctx), LLVMDebugMetadataVersion(), 0));
  LLVMAddModuleFlag(cg->module, LLVMModuleFlagBehaviorWarning,
                    "Debug Info Version", strlen("Debug Info Version"),
                    dbg_ver);
  LLVMMetadataRef dwarf_ver = LLVMValueAsMetadata(
      LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 4, 0));
  LLVMAddModuleFlag(cg->module, LLVMModuleFlagBehaviorWarning,
                    "Dwarf Version", strlen("Dwarf Version"), dwarf_ver);
}

LLVMMetadataRef codegen_debug_subprogram(codegen_t *cg, LLVMValueRef func,
                                         const char *name, token_t tok) {
  if (!cg || !cg->di_builder || !func)
    return NULL;
  if (!name)
    name = "<anon>";
  LLVMMetadataRef file = debug_file_for(cg, tok.filename);
  if (!cg->di_subroutine_type) {
    cg->di_subroutine_type = LLVMDIBuilderCreateSubroutineType(
        cg->di_builder, file ? file : cg->di_file, NULL, 0, LLVMDIFlagZero);
  }
  unsigned line = tok.line ? (unsigned)tok.line : 1;
  LLVMMetadataRef sp = LLVMDIBuilderCreateFunction(
      cg->di_builder, file ? file : cg->di_file, name, strlen(name), name,
      strlen(name), file ? file : cg->di_file, line, cg->di_subroutine_type, 0,
      1, line, LLVMDIFlagZero, 0);
  if (sp)
    LLVMSetSubprogram(func, sp);
  return sp;
}

void codegen_debug_finalize(codegen_t *cg) {
  if (!cg || !cg->di_builder)
    return;
  LLVMDIBuilderFinalize(cg->di_builder);
  LLVMDisposeDIBuilder(cg->di_builder);
  cg->di_builder = NULL;
}
