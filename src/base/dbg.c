#include "dbg.h"
#include "base/common.h"
#include "code/code.h"
#include "parse/parser.h"
#include <llvm-c/DebugInfo.h>

void ny_dbg_loc(codegen_t *cg, token_t tok) {
  if (cg && cg->debug_symbols && cg->di_builder && cg->builder && cg->di_scope && tok.line > 0) {
    LLVMMetadataRef debug_scope = codegen_debug_loc_scope(cg, tok);
    if (!debug_scope)
      debug_scope = cg->di_scope;
    LLVMMetadataRef loc = LLVMDIBuilderCreateDebugLocation(cg->ctx, (unsigned)tok.line,
                                                           (unsigned)tok.col, debug_scope, NULL);
    if (loc) {
      cg->di_loc = loc;
      LLVMSetCurrentDebugLocation2(cg->builder, loc);
      if (cg->alloca_builder) {
        LLVMSetCurrentDebugLocation2(cg->alloca_builder, loc);
      }
    }
  }
  if (verbose_enabled >= 2) {
    NY_LOG_DEBUG("DBG_LOC: %s:%d:%d\n", tok.filename ? tok.filename : "<unknown>", tok.line,
                 tok.col);
  }
}
