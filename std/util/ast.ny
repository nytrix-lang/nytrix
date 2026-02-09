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

