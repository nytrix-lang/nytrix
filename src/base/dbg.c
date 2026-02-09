#include "dbg.h"
#include "base/common.h"  // For NY_LOG_DEBUG and verbose_enabled
#include "code/code.h"    // For codegen_t definition
#include "parse/parser.h" // For token_t definition
#include <llvm-c/DebugInfo.h>

void ny_dbg_loc(codegen_t *cg, token_t tok) {
    if (cg && cg->debug_symbols && cg->di_builder && cg->builder &&
        cg->di_scope && tok.line > 0) {
        LLVMMetadataRef loc =
            LLVMDIBuilderCreateDebugLocation(cg->ctx, (unsigned)tok.line,
                                             (unsigned)tok.col, cg->di_scope,
                                             NULL);
        if (loc)
            LLVMSetCurrentDebugLocation2(cg->builder, loc);
    }
    if (verbose_enabled >= 2) {
        NY_LOG_DEBUG("DBG_LOC: %s:%d:%d\n", tok.filename ? tok.filename : "<unknown>", tok.line, tok.col);
    }
}
