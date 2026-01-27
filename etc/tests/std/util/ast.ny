use std.util.ast *
use std.core.error *
use std.core.reflect *

;; std.util.ast (Test)
;; Tests basic AST parsing.

print("Testing ast...")

def src = "def x = 1"
def ast = parse_ast(src)

assert(is_list(ast), "ast is list")

if(list_len(ast) > 0){
 def stmt = get(ast, 0)
 assert(is_dict(stmt), "stmt is dict")
 assert(len(stmt) > 0, "stmt not empty")
}

print("✓ std.util.ast tests passed")
