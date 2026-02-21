;; Keywords: util ast
;; Util Ast module.

module std.util.ast (
   parse_ast
)
use std.core *
use std.str.json *

fn parse_ast(source){
   "Parses Nytrix source code and returns the AST as a nested structure (list of dicts)."
   mut json_str = __parse_ast(source)
   if(json_str == 0){ return 0 }
   def ast = json_decode(json_str)
   free(json_str)
   return ast
}

if(comptime{__main()}){
    use std.util.ast *
    use std.core *
    use std.core.error *
    use std.core.reflect *

    print("Testing ast...")

    def src = "def x = 1"
    def ast = parse_ast(src)

    assert(is_list(ast), "ast is list")

    if(len(ast) > 0){
     def stmt = get(ast, 0)
     assert(is_dict(stmt), "stmt is dict")
     assert(len(stmt) > 0, "stmt not empty")
    }

    print("✓ std.util.ast tests passed")
}
