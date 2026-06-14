;; Keywords: ast syntax-tree core
;; AST parsing and source-tree access for tools that inspect Nytrix code.
;; References:
;; - std.core
module std.core.ast(parse_ast)
use std.core
use std.parse.data.json

fn parse_ast(str source) any {
   "Parses Nytrix source code and returns the AST as a nested structure(list of dicts)."
   def json_str = __parse_ast(source)
   if json_str == 0 { return 0 }
   def ast = json_decode(json_str)
   free(json_str)
   ast
}

#main {
   def ast = parse_ast("def x = 1")
   assert(is_list(ast), "ast parse returns list")
   if ast.len > 0 {
      def stmt = ast.get(0)
      assert(is_dict(stmt), "ast statement is dict")
      assert(stmt.len > 0, "ast statement has fields")
   }
   print("✓ std.core.ast self-test passed")
}
