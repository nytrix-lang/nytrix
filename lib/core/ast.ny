;; Keywords: ast syntax-tree
;; AST parsing and source-tree access for tools that inspect Nytrix code.
module std.core.ast(parse_ast)
use std.core
use std.parse.data.json

fn parse_ast(str: source): any {
   "Parses Nytrix source code and returns the AST as a nested structure(list of dicts)."
   def json_str = __parse_ast(source)
   if(json_str == 0){ return 0 }
   def ast = json_decode(json_str)
   free(json_str)
   ast
}
