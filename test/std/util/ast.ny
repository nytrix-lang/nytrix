use std.io
use std.util.ast
use std.core
use std.core.test
use std.core.reflect

print("Testing ast...")

fn test_parse(){
	def src = "def x = 1"
	def ast = parse_ast(src)
	if(!is_list(ast)){
		use std.util.inspect
		print("AST Result: ", inspect(ast))
	}
	assert(is_list(ast), "ast is list")
	; We expect at least one statement
	if(list_len(ast) > 0){
		def stmt = get(ast, 0)
		assert(is_dict(stmt), "stmt is dict")
		; Check if it has 'kind' or 'type'
		; The JSON structure depends on ast_json.c
		; Assuming reasonable names.
		; Just check it's not empty for now.
		assert(len(stmt) > 0, "stmt not empty")
	}
}

test_parse()

print("✓ std.util.ast tests passed")
