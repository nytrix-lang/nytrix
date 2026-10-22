;; Keywords: ast syntax-tree core
;; AST parsing and source-tree access for tools that inspect Nytrix code.
;; References:
;; - core
module core.compiler.ast(parse_ast, parse_ast_snapshot, parse_code,
   parse_code_typed, code_str,
   code_kind, code_category, code_name, code_has_attribute,
   code_origin, code_attributes, code_signature,
   code_contract, Code, CodeExpr, CodeStmt, CodeDecl,
   CodeIdent, code_expr, code_stmt, code_decl, code_ident,
   code_expr_from_json, code_stmt_from_json,
   code_decl_from_json, code_ident_from_json,
   code_expr_json, code_stmt_json, code_decl_json,
   code_ident_json, code_expr_syntax, code_stmt_syntax,
   code_decl_syntax, code_ident_syntax,
   code_expr_is_sequence, code_stmt_is_sequence,
   code_decl_is_sequence, code_ident_is_sequence)
use core
use core.primitives as core_prim
use core.math.parse.data.json
use core.math.float as f

;; Category marker types make code categories distinct in Ny's ordinary type
;; system; Code<T> also carries canonical syntax and the sequence shape.
enum CodeExpr { CodeExpr }

enum CodeStmt { CodeStmt }

enum CodeDecl { CodeDecl }

enum CodeIdent { CodeIdent }

enum Code<T> { CodeValue(T category, str json) }
fn parse_ast(str source) any {
   "Returns the nested AST, or nil if the source has any syntax error."
   def json_str = __parse_ast(source)
   if is_nil(json_str) { return nil }
   def ast = json_decode(json_str)
   free(json_str)
   ast
}

fn parse_ast_snapshot(str source) any {
   "Returns a versioned syntax snapshot with a source origin on each node."
   def json_str = __parse_ast_snapshot(source)
   if is_nil(json_str) { return nil }
   def snapshot = json_decode(json_str)
   free(json_str)
   snapshot
}

fn _is_declaration_type(str kind) bool {
   kind == "use" || kind == "var" || kind == "func" || kind == "lemma" ||
   kind == "extern" || kind == "link" || kind == "layout" ||
   kind == "module" || kind == "export" || kind == "struct" ||
   kind == "enum" || kind == "macro" || kind == "include" ||
   kind == "define" || kind == "operator" || kind == "impl" ||
   kind == "refine" || kind == "trait"
}

fn _is_declaration_record(any node) bool {
   if !is_dict(node) || !is_str(node.get("type")) { return false }
   def kind = str(node.get("type"))
   if kind == "var" && is_bool(node.get("is_decl")) {
      return node.get("is_decl")
   }
   ;; Legacy parse_ast records predate is_decl; retain their historical shape
   ;; interpretation while versioned snapshots distinguish assignments.
   _is_declaration_type(kind)
}

fn _code_record_category(any node) str {
   if !is_dict(node) || !is_str(node.get("type")) { return "invalid" }
   def kind = str(node.get("type"))
   if kind == "var" && is_bool(node.get("is_decl")) &&
   !node.get("is_decl") {
      return "statements"
   }
   _code_node_category(kind)
}

fn _code_node_category(str kind) str {
   if _is_declaration_type(kind) { return "declarations" }
   if kind == "block" || kind == "expr_stmt" || kind == "if" ||
   kind == "layout_guard" || kind == "while" || kind == "for" ||
   kind == "for_cstyle" || kind == "try" || kind == "return" ||
   kind == "defer" || kind == "break" || kind == "continue" ||
   kind == "label" || kind == "goto" || kind == "match_stmt" {
      return "statements"
   }
   if kind == "literal" || kind == "ident" || kind == "unary" ||
   kind == "binary" || kind == "logical" || kind == "ternary" ||
   kind == "call" || kind == "memcall" || kind == "index" ||
   kind == "lambda" || kind == "fn" || kind == "list" ||
   kind == "tuple" || kind == "set" || kind == "dict" ||
   kind == "asm" || kind == "comptime" || kind == "quote" ||
   kind == "exists" || kind == "splice" || kind == "fstring" ||
   kind == "inferred_member" || kind == "embed" || kind == "match" ||
   kind == "member" || kind == "ptr_type" || kind == "deref" ||
   kind == "sizeof" || kind == "try_expr" {
      return "expressions"
   }
   "unknown"
}

fn _code_quote_stmt_category(any statement) str {
   if !is_dict(statement) || !is_str(statement.get("type")) {
      return "invalid"
   }
   if _is_declaration_record(statement) { return "decl" }
   def kind = str(statement.get("type"))
   if kind == "expr_stmt" {
      def expression = statement.get("expr_t")
      if is_dict(expression) && expression.get("type") == "binary" &&
      expression.get("op") == "=" {
         return "stmt"
      }
      return "expr"
   }
   if _code_record_category(statement) == "statements" { return "stmt" }
   "invalid"
}

fn _code_quote_category(any quote) str {
   if !is_dict(quote) || quote.get("type") != "quote" { return "invalid" }
   def expression = quote.get("expr")
   if !is_nil(expression) { return "expr" }
   def body = quote.get("body")
   if !is_dict(body) || body.get("type") != "block" {
      return _code_quote_stmt_category(body)
   }
   def statements = body.get("body")
   if !is_list(statements) { return "invalid" }
   if statements.len == 0 { return "empty" }
   def first_category = _code_quote_stmt_category(statements.get(0))
   if first_category == "invalid" { return "invalid" }
   if statements.len == 1 && first_category == "expr" { return "expr" }
   mut i = 1
   while i < statements.len {
      def category = _code_quote_stmt_category(statements.get(i))
      if category == "invalid" { return "invalid" }
      if category != first_category { return "mixed" }
      i += 1
   }
   first_category
}

fn _code_sequence_category(any code) str {
   if code.len == 0 { return "empty" }
   mut category = ""
   mut i = 0
   while i < code.len {
      def node = code.get(i)
      if !is_dict(node) || !is_str(node.get("type")) { return "invalid" }
      def item_category = _code_record_category(node)
      if item_category == "unknown" || item_category == "invalid" {
         return item_category
      }
      if category == "" { category = item_category }
      else if category != item_category { return "mixed" }
      i += 1
   }
   category
}

fn parse_code(str source) any {
   "Parses declaration-only source into versioned AST records or returns nil on errors."
   def snapshot = parse_ast_snapshot(source)
   if is_nil(snapshot) || !is_dict(snapshot) ||
   snapshot.get("schema", "") != "nytrix.ast" ||
   int(snapshot.get("version", 0)) != 1 {
      return nil
   }
   def declarations = snapshot.get("nodes")
   if is_nil(declarations) || !is_list(declarations) || declarations.len == 0 {
      return nil
   }
   mut index = 0
   while index < declarations.len {
      def declaration = declarations.get(index)
      if _code_record_category(declaration) != "declarations" {
         return nil
      }
      index += 1
   }
   declarations
}

fn parse_code_typed(str source) Result<Code<CodeDecl>, str> {
   "Parses complete declaration-only source and returns a typed declaration code value."
   def declarations = parse_code(source)
   if is_nil(declarations) {
      return err("source must contain only complete declarations")
   }
   code_decl(declarations)
}

fn _code_merge_keys(list keys, int start, int stop) list {
   mut result = []
   mut i = start
   while i < stop {
      result = result.append(keys.get(i))
      i += 1
   }
   mut width = 1
   while width < result.len {
      mut merged = []
      mut base = 0
      while base < result.len {
         def middle = min(base + width, result.len)
         def end = min(base + width * 2, result.len)
         mut left = base
         mut right = middle
         while left < middle || right < end {
            if left >= middle {
               merged = merged.append(result.get(right))
               right += 1
            } elif right >= end {
               merged = merged.append(result.get(left))
               left += 1
            } else {
               def str left_key = result.get(left)
               def str right_key = result.get(right)
               if left_key <= right_key {
                  merged = merged.append(left_key)
                  left += 1
               } else {
                  merged = merged.append(right_key)
                  right += 1
               }
            }
         }
         base = end
      }
      result = merged
      width *= 2
   }
   result
}

fn _code_sorted_keys(any value) any {
   def keys = dict_keys(value)
   for key in keys {
      if !is_str(key) { return nil }
   }
   _code_merge_keys(keys, 0, keys.len)
}

fn _code_json(any value, int depth) str {
   if depth > 64 { return "" }
   if is_dict(value) {
      def keys = _code_sorted_keys(value)
      if is_nil(keys) { return "" }
      mut out = Builder(max(16, keys.len * 16 + 2))
      out = builder_append(out, "{")
      mut i = 0
      while i < keys.len {
         def key = keys.get(i)
         if !is_str(key) {
            builder_free(out)
            return ""
         }
         def encoded = _code_json(value.get(key), depth + 1)
         if encoded == "" {
            builder_free(out)
            return ""
         }
         if i > 0 { out = builder_append(out, ",") }
         out = builder_append(out, json_encode(key))
         out = builder_append(out, ":")
         out = builder_append(out, encoded)
         i += 1
      }
      out = builder_append(out, "}")
      def result = builder_to_str(out)
      builder_free(out)
      return result
   }
   if is_list(value) || is_tuple(value) {
      mut out = Builder(max(16, value.len * 8 + 2))
      out = builder_append(out, "[")
      mut i = 0
      while i < value.len {
         def encoded = _code_json(value.get(i), depth + 1)
         if encoded == "" {
            builder_free(out)
            return ""
         }
         if i > 0 { out = builder_append(out, ",") }
         out = builder_append(out, encoded)
         i += 1
      }
      out = builder_append(out, "]")
      def result = builder_to_str(out)
      builder_free(out)
      return result
   }
   ; A code value must have a deterministic JSON scalar representation. Sets
   ; have no stable iteration order, and NaN/infinities are not JSON numbers.
   if is_set(value) { return "" }
   if f.is_float(value) && (f.is_nan(value) || f.is_inf(value)) { return "" }
   if !is_nil(value) && type(value) != "bool" && !is_int(value) &&
   !f.is_float(value) && !is_str(value) { return "" }
   return json_encode(value)
}

fn code_str(any code) str {
   "Returns canonical compact JSON for AST records or a versioned AST snapshot."
   if !is_list(code) && !is_tuple(code) && !is_dict(code) { return "" }
   _code_json(code, 0)
}

fn code_expr(any syntax) Result<Code<CodeExpr>, str> {
   "Builds a typed expression code value after validating scalar, homogeneous, or empty expression syntax."
   def category = code_category(syntax)
   if category != "expr" && category != "expressions" &&
   category != "empty" {
      return err("code_expr expects expression syntax, got " + category)
   }
   def sequence = is_list(syntax) || is_tuple(syntax)
   def json = _code_value_json("expr", sequence, syntax)
   if json == "" { return err("code_expr cannot serialize this syntax") }
   return ok(Code.CodeValue(CodeExpr.CodeExpr, json))
}

fn code_stmt(any syntax) Result<Code<CodeStmt>, str> {
   "Builds a typed statement code value after validating scalar, homogeneous, or empty statement syntax."
   def category = code_category(syntax)
   if category != "stmt" && category != "statements" &&
   category != "empty" {
      return err("code_stmt expects statement syntax, got " + category)
   }
   def sequence = is_list(syntax) || is_tuple(syntax)
   def json = _code_value_json("stmt", sequence, syntax)
   if json == "" { return err("code_stmt cannot serialize this syntax") }
   return ok(Code.CodeValue(CodeStmt.CodeStmt, json))
}

fn code_decl(any syntax) Result<Code<CodeDecl>, str> {
   "Builds a typed declaration code value after validating scalar, homogeneous, or empty declaration syntax."
   def category = code_category(syntax)
   if category != "decl" && category != "declarations" &&
   category != "empty" {
      return err("code_decl expects declaration syntax, got " + category)
   }
   def sequence = is_list(syntax) || is_tuple(syntax)
   def json = _code_value_json("decl", sequence, syntax)
   if json == "" { return err("code_decl cannot serialize this syntax") }
   return ok(Code.CodeValue(CodeDecl.CodeDecl, json))
}

fn code_ident(any syntax) Result<Code<CodeIdent>, str> {
   "Builds identifier code only from an AST identifier carrying its syntax context; strings are rejected."
   if !is_dict(syntax) || syntax.get("type", "") != "ident" {
      return err("code_ident expects an AST identifier; use ident(...) at a template boundary")
   }
   def json = _code_value_json("ident", false, syntax)
   if json == "" { return err("code_ident cannot serialize this identifier") }
   return ok(Code.CodeValue(CodeIdent.CodeIdent, json))
}

fn _code_value_json(str category, bool sequence, any syntax) str {
   def syntax_json = code_str(syntax)
   if syntax_json == "" { return "" }
   code_str({"schema": "nytrix.code.value", "version": 1,
         "category": category, "sequence": sequence,
         "syntax": syntax})
}

fn _code_decode_bounded(str json) any {
   if json.len > 1048576 { return nil }
   def decoded = json_try_decode_bounded(json, 64)
   if !bool(decoded.get("ok", false)) { return nil }
   decoded.get("value")
}

fn _code_value_valid(str json, str category, bool sequence) bool {
   if json == "" { return false }
   def value = _code_decode_bounded(json)
   if !is_dict(value) { return false }
   def version = value.get("version")
   def encoded_sequence = value.get("sequence")
   if value.get("schema", "") != "nytrix.code.value" ||
   !is_int(version) || type(version) == "bool" || str(version) != "1" ||
   value.get("category", "") != category ||
   !is_int(encoded_sequence) || type(encoded_sequence) == "bool" ||
   (str(encoded_sequence) != "0" && str(encoded_sequence) != "1") ||
   str(encoded_sequence) != (bool(sequence) ? "1" : "0") ||
   code_str(value) != json {
      return false
   }
   def syntax = value.get("syntax")
   def actual_sequence = is_list(syntax) || is_tuple(syntax)
   if bool(actual_sequence) != bool(sequence) { return false }
   if category == "ident" {
      if sequence || !is_dict(syntax) || syntax.get("type", "") != "ident" {
         return false
      }
   } else {
      def actual_category = code_category(syntax)
      mut expected_sequence_category = "declarations"
      if category == "expr" {
         expected_sequence_category = "expressions"
      } else if category == "stmt" {
         expected_sequence_category = "statements"
      }
      if sequence {
         if actual_category != "empty" &&
         actual_category != expected_sequence_category { return false }
      } else if actual_category != category {
         return false
      }
   }
   true
}

fn _code_value_valid_json(str json, str category) bool {
   def value = _code_decode_bounded(json)
   if !is_dict(value) { return false }
   def sequence = value.get("sequence")
   if !is_int(sequence) || type(sequence) == "bool" ||
   (str(sequence) != "0" && str(sequence) != "1") { return false }
   _code_value_valid(json, category, str(sequence) == "1")
}

fn _code_value_json_checked(str json, str category) str {
   if !_code_value_valid_json(json, category) { return "" }
   json
}

fn _code_value_syntax(str json, str category) any {
   if !_code_value_valid_json(json, category) { return nil }
   def value = _code_decode_bounded(json)
   value.get("syntax")
}

fn _code_value_sequence(str json, str category) bool {
   if !_code_value_valid_json(json, category) { return false }
   def value = _code_decode_bounded(json)
   str(value.get("sequence")) == "1"
}

fn code_expr_json(Code<CodeExpr> code) str {
   "Returns the canonical envelope, or an empty string for an invalid code value."
   match code { Code.CodeValue(_, json) ->
      _code_value_json_checked(json, "expr") }
}

fn code_stmt_json(Code<CodeStmt> code) str {
   "Returns the canonical envelope, or an empty string for an invalid code value."
   match code { Code.CodeValue(_, json) ->
      _code_value_json_checked(json, "stmt") }
}

fn code_decl_json(Code<CodeDecl> code) str {
   "Returns the canonical envelope, or an empty string for an invalid code value."
   match code { Code.CodeValue(_, json) ->
      _code_value_json_checked(json, "decl") }
}

fn code_ident_json(Code<CodeIdent> code) str {
   "Returns the canonical envelope, or an empty string for an invalid code value."
   match code { Code.CodeValue(_, json) ->
      _code_value_json_checked(json, "ident") }
}

fn code_expr_from_json(str json) Result<Code<CodeExpr>, str> {
   "Restores an expression code value from a validated canonical envelope."
   if !_code_value_valid_json(json, "expr") {
      return err("invalid expression code envelope")
   }
   return ok(Code.CodeValue(CodeExpr.CodeExpr, json))
}

fn code_stmt_from_json(str json) Result<Code<CodeStmt>, str> {
   "Restores a statement code value from a validated canonical envelope."
   if !_code_value_valid_json(json, "stmt") {
      return err("invalid statement code envelope")
   }
   return ok(Code.CodeValue(CodeStmt.CodeStmt, json))
}

fn code_decl_from_json(str json) Result<Code<CodeDecl>, str> {
   "Restores a declaration code value from a validated canonical envelope."
   if !_code_value_valid_json(json, "decl") {
      return err("invalid declaration code envelope")
   }
   return ok(Code.CodeValue(CodeDecl.CodeDecl, json))
}

fn code_ident_from_json(str json) Result<Code<CodeIdent>, str> {
   "Restores an identifier code value from a validated canonical envelope."
   if !_code_value_valid_json(json, "ident") {
      return err("invalid identifier code envelope")
   }
   return ok(Code.CodeValue(CodeIdent.CodeIdent, json))
}

fn code_expr_syntax(Code<CodeExpr> code) any {
   "Returns syntax only when its envelope and category are valid; otherwise nil."
   _code_value_syntax(code_expr_json(code), "expr")
}

fn code_stmt_syntax(Code<CodeStmt> code) any {
   "Returns syntax only when its envelope and category are valid; otherwise nil."
   _code_value_syntax(code_stmt_json(code), "stmt")
}

fn code_decl_syntax(Code<CodeDecl> code) any {
   "Returns syntax only when its envelope and category are valid; otherwise nil."
   _code_value_syntax(code_decl_json(code), "decl")
}

fn code_ident_syntax(Code<CodeIdent> code) any {
   "Returns syntax only when its envelope and identifier node are valid; otherwise nil."
   _code_value_syntax(code_ident_json(code), "ident")
}

fn code_expr_is_sequence(Code<CodeExpr> code) bool {
   _code_value_sequence(code_expr_json(code), "expr")
}

fn code_stmt_is_sequence(Code<CodeStmt> code) bool {
   _code_value_sequence(code_stmt_json(code), "stmt")
}

fn code_decl_is_sequence(Code<CodeDecl> code) bool {
   _code_value_sequence(code_decl_json(code), "decl")
}

fn code_ident_is_sequence(Code<CodeIdent> code) bool {
   _code_value_sequence(code_ident_json(code), "ident")
}

fn code_kind(any code) str {
   "Returns the syntax category of an AST node or homogeneous code sequence. Sequences are declarations, statements, expressions, empty, mixed, invalid, or unknown."
   if is_dict(code) {
      def kind = code.get("type")
      if !is_str(kind) { return "invalid" }
      ;; A quote record is a code envelope. Report the quoted expression's
      ;; concrete AST node kind so callers can distinguish literals, calls,
      ;; operators, and identifiers.
      if kind == "quote" || kind == "block" {
         def expression = code.get("expr")
         if is_dict(expression) && is_str(expression.get("type")) {
            return str(expression.get("type"))
         }
         def body = code.get("body")
         if kind == "block" && is_list(body) && body.len == 1 {
            def only = body.get(0)
            if is_dict(only) && only.get("type") == "expr_stmt" {
               def body_expression = only.get("expr_t")
               if is_dict(body_expression) &&
               is_str(body_expression.get("type")) {
                  return str(body_expression.get("type"))
               }
            }
         }
         if is_dict(body) && body.get("type") == "block" {
            def statements = body.get("body", [])
            if is_list(statements) && statements.len == 1 {
               def only = statements.get(0)
               if is_dict(only) && only.get("type") == "expr_stmt" {
                  def body_expression = only.get("expr_t")
                  if is_dict(body_expression) &&
                  is_str(body_expression.get("type")) {
                     return str(body_expression.get("type"))
                  }
               }
            }
         }
      }
      def category = _code_record_category(code)
      if category == "unknown" { return "unknown" }
      return str(kind)
   }
   if is_list(code) || is_tuple(code) {
      return _code_sequence_category(code)
   }
   "invalid"
}

fn code_category(any code) str {
   "Returns the typed code category when present in a versioned syntax record."
   if is_dict(code) {
      def code_type = code.get("code_type")
      if !is_nil(code_type) {
         def version = code_type.get("version")
         def category = code_type.get("category", "")
         if !is_dict(code_type) ||
         code_type.get("schema", "") != "nytrix.code" ||
         !is_int(version) || type(version) == "bool" || version != 1 ||
         !is_str(category) || code.get("type", "") != "quote" ||
         (category != "expr" && category != "stmt" &&
            category != "decl" && category != "mixed" &&
            category != "empty") {
            return "invalid"
         }
         def body_category = _code_quote_category(code)
         if body_category != category { return "invalid" }
         return str(category)
      }
      def category = _code_record_category(code)
      if category == "expressions" { return "expr" }
      if category == "statements" { return "stmt" }
      if category == "declarations" { return "decl" }
      return category
   }
   if is_list(code) || is_tuple(code) { return _code_sequence_category(code) }
   "invalid"
}

fn code_name(any code) str {
   "Returns the declaration name, or an empty string for unnamed syntax."
   if is_list(code) || is_tuple(code) {
      if code.len != 1 { return "" }
      code = code.get(0)
   }
   if !is_dict(code) { return "" }
   def kind_value = code.get("type", "")
   if !is_str(kind_value) || !_is_declaration_record(code) { return "" }
   def kind = str(kind_value)
   if kind == "var" {
      def names = code.get("names")
      if !is_list(names) || names.len != 1 || !is_str(names.get(0)) {
         return ""
      }
      return str(names.get(0))
   }
   def name = code.get("name", "")
   if !is_str(name) { return "" }
   str(name)
}

fn code_has_attribute(any code, str wanted) bool {
   "Returns whether one declaration record contains the named attribute."
   if is_list(code) || is_tuple(code) {
      if code.len != 1 { return false }
      code = code.get(0)
   }
   if !is_dict(code) || !is_str(code.get("type")) ||
   !_is_declaration_record(code) { return false }
   ;; Versioned snapshots expose `attributes`; legacy parse_ast records keep
   ;; their public field name unchanged and carry the same data privately.
   def attributes = code.get("attributes", code.get("_attributes", []))
   if !is_list(attributes) { return false }
   for attribute in attributes {
      if is_dict(attribute) && is_str(attribute.get("name")) &&
      str(attribute.get("name")) == wanted {
         return true
      }
   }
   false
}

fn _code_origin_valid(any origin) bool {
   if !is_dict(origin) || !is_str(origin.get("file")) { return false }
   def line = origin.get("line")
   def column = origin.get("column")
   is_int(line) && type(line) != "bool" && line >= 0 &&
   is_int(column) && type(column) != "bool" && column >= 0
}

fn code_origin(any node) any {
   "Returns a versioned source and expansion origin record for one AST node."
   if !is_dict(node) { return nil }
   def source = node.get("source_origin")
   def expansion = node.get("expansion_origin")
   if !is_dict(source) && !is_dict(expansion) { return nil }
   if (!is_nil(source) && !_code_origin_valid(source)) ||
   (!is_nil(expansion) && !_code_origin_valid(expansion)) {
      return nil
   }
   {"schema": "nytrix.origin", "version": 1,
      "source": source, "expansion": expansion}
}

fn code_attributes(any code) any {
   "Returns ordered declaration attributes as an immutable, versioned syntax snapshot. Argument entries are canonical code_str JSON."
   if is_list(code) || is_tuple(code) {
      if code.len != 1 { return nil }
      code = code.get(0)
   }
   if !is_dict(code) || !_is_declaration_record(code) { return nil }
   def attributes = code.get("attributes", [])
   if !is_list(attributes) { return nil }
   mut entries = []
   for attribute in attributes {
      if !is_dict(attribute) || !is_str(attribute.get("name")) {
         return nil
      }
      def args = attribute.get("args", [])
      if !is_list(args) { return nil }
      mut encoded_args = []
      for arg in args {
         def encoded = code_str(arg)
         if encoded == "" { return nil }
         encoded_args = encoded_args.append(encoded)
      }
      def source = code_str(attribute.get("source_origin"))
      def expansion_value = attribute.get("expansion_origin")
      def expansion = if is_nil(expansion_value) { "" }
      else { code_str(expansion_value) }
      if source == "" || (!is_nil(expansion_value) && expansion == "") {
         return nil
      }
      entries = entries.append(core_prim.list_as_tuple_raw([
               str(attribute.get("name")),
               core_prim.list_as_tuple_raw(encoded_args), source, expansion]))
   }
   core_prim.list_as_tuple_raw([
         "nytrix.code.attributes", 1, core_prim.list_as_tuple_raw(entries)])
}

fn code_signature(any code) any {
   "Returns an immutable, versioned function signature snapshot. Parameter defaults and origins use canonical code_str JSON."
   if is_list(code) || is_tuple(code) {
      if code.len != 1 { return nil }
      code = code.get(0)
   }
   if !is_dict(code) { return nil }
   def kind = code.get("type", "")
   if kind != "func" && kind != "extern" && kind != "lambda" &&
   kind != "fn" { return nil }
   def name = code.get("name", "")
   def return_type = code.get("return", "")
   ;; Tuple retagging stores raw immediates in an untyped list slot.  The Ny
   ;; false immediate (2) is therefore observed as integer 2 by tuple readers;
   ;; expose this fact as the conventional 0/1 flag so `bool()` round-trips.
   def variadic = bool(code.get("variadic", false)) ? 1 : 0
   def params = code.get("params", [])
   if !is_str(name) || !is_str(return_type) || !is_list(params) { return nil }
   mut rows = []
   for parameter in params {
      if !is_dict(parameter) || !is_str(parameter.get("name")) {
         return nil
      }
      def parameter_type = parameter.get("type", "")
      if !is_str(parameter_type) { return nil }
      def default_value = parameter.get("default")
      def default_json = if is_nil(default_value) { "" }
      else { code_str(default_value) }
      def source = parameter.get("source_origin")
      def source_json = if is_nil(source) { "" } else { code_str(source) }
      def expansion = parameter.get("expansion_origin")
      def expansion_json = if is_nil(expansion) { "" }
      else { code_str(expansion) }
      if (!is_nil(default_value) && default_json == "") ||
      (!is_nil(source) && source_json == "") ||
      (!is_nil(expansion) && expansion_json == "") {
         return nil
      }
      rows = rows.append(core_prim.list_as_tuple_raw([
               str(parameter.get("name")), str(parameter_type), default_json,
               source_json, expansion_json]))
   }
   def attributes = if kind == "lambda" || kind == "fn" {
      core_prim.list_as_tuple_raw(["nytrix.code.attributes", 1, ()])
   } else { code_attributes(code) }
   if is_nil(attributes) { return nil }
   core_prim.list_as_tuple_raw([
         "nytrix.ast.signature", 1, str(kind), str(name), str(return_type),
         variadic, core_prim.list_as_tuple_raw(rows),
         attributes])
}

fn code_contract(any code) any {
   "Returns the requires and decreases syntax of one function as canonical JSON in an immutable versioned tuple."
   if is_list(code) || is_tuple(code) {
      if code.len != 1 { return nil }
      code = code.get(0)
   }
   if !is_dict(code) || code.get("type", "") != "func" { return nil }
   def requires = code.get("requires")
   def decreases = code.get("decreases")
   def requires_json = if is_nil(requires) { "" } else { code_str(requires) }
   def decreases_json = if is_nil(decreases) { "" } else { code_str(decreases) }
   if (!is_nil(requires) && requires_json == "") ||
   (!is_nil(decreases) && decreases_json == "") {
      return nil
   }
   core_prim.list_as_tuple_raw([
         "nytrix.ast.contract", 1, requires_json, decreases_json])
}

#main {
   def ast = parse_ast("def x = 1")
   assert(is_list(ast), "ast parse returns list")
   if ast.len > 0 {
      def stmt = ast.get(0)
      assert(is_dict(stmt), "ast statement is dict")
      assert(stmt.len > 0, "ast statement has fields")
   }
   def snapshot = parse_ast_snapshot("def x = 1")
   def node = snapshot.get("nodes").get(0)
   assert(str(snapshot.get("schema")) == "nytrix.ast" &&
      int(snapshot.get("version")) == 1 &&
      int(node.get("source_origin").get("line")) == 1 &&
      int(node.get("source_origin").get("column")) == 1,
      "versioned syntax snapshot carries per-node source origins")
   def origin = code_origin(node)
   assert(origin.get("schema") == "nytrix.origin" &&
      int(origin.get("version")) == 1 &&
      origin.get("source") == node.get("source_origin") &&
      is_nil(origin.get("expansion")) && is_nil(code_origin(42)),
      "code_origin exposes a stable versioned origin record")
   def code = parse_code("fn answer() int { return 42 }")
   assert(is_list(code) && code.len == 1 &&
      str(code.get(0).get("type")) == "func",
      "parse_code returns a complete declaration record")
   def attributed_code = parse_code("@trace @memoize(3) fn traced() int { return 1 }")
   assert(code_has_attribute(attributed_code, "trace") &&
      code_has_attribute(attributed_code.get(0), "memoize") &&
      !code_has_attribute(attributed_code, "inline") &&
      !code_has_attribute(code, "trace") &&
      !code_has_attribute({"type": "literal",
            "attributes": [{"name": "trace"}]},
         "trace"),
      "code_has_attribute inspects ordered declaration attributes")
   def attribute_snapshot = code_attributes(attributed_code)
   def parsed_expr = parse_ast("def value = 1 + 2").get(0).get("exprs").get(0)
   assert(is_tuple(attribute_snapshot) &&
      attribute_snapshot[0] == "nytrix.code.attributes" &&
      attribute_snapshot[1] == 1 &&
      is_tuple(attribute_snapshot[2]) &&
      attribute_snapshot[2][0][0] == "trace" &&
      attribute_snapshot[2][0][1].len == 0 &&
      attribute_snapshot[2][1][0] == "memoize" &&
      attribute_snapshot[2][1][1][0] == "3" &&
      code_attributes(parsed_expr) == nil,
      "code_attributes returns ordered immutable declaration syntax and argument origins")
   def parsed_stmt = parse_ast("return 1").get(0)
   def two_declarations = parse_code(
      "fn first() int { return 1 } fn second() int { return 2 }")
   assert(code_kind(code) == "declarations" &&
      code_kind([parsed_expr, parsed_expr]) == "expressions" &&
      code_kind((parsed_expr, parsed_expr)) == "expressions" &&
      code_kind([parsed_stmt, parsed_stmt]) == "statements" &&
      code_kind([]) == "empty" &&
      code_kind([code.get(0), parsed_stmt]) == "mixed" &&
      code_kind([42]) == "invalid",
      "code_kind distinguishes declaration, statement, expression, empty, mixed, and invalid sequences")
   assert(code_kind({"type": "future_syntax_node"}) == "unknown" &&
      code_kind([{"type": "future_syntax_node"}]) == "unknown" &&
      code_kind({"type": 17}) == "invalid",
      "code_kind distinguishes unknown node kinds from malformed records")
   assert(code_name({"type": "member", "name": "field"}) == "" &&
      code_name({"type": "func", "name": 17}) == "" &&
      code_name({"type": "var", "names": [17]}) == "" &&
      code_name(parsed_expr) == "" && code_name(two_declarations) == "" &&
      !code_has_attribute(two_declarations, "trace") &&
      code_kind(42) == "invalid",
      "code_name rejects non-declarations and malformed names")
   assert(is_nil(parse_code("print(1)")),
      "parse_code rejects executable statements")
   assert(is_nil(parse_code("answer = 42")),
      "parse_code rejects top-level assignments")
   assert(is_nil(parse_code("fn good() int { return 1 }\ndef bad =")),
      "parse_code rejects malformed input without partial declarations")
   def code_text = code_str(snapshot)
   def code_roundtrip = json_decode(code_text)
   assert(code_text == code_str(code_roundtrip) &&
      int(code_roundtrip.get("version")) == 1 &&
      code_roundtrip.get("nodes").get(0).get("source_origin") != nil,
      "code_str emits stable canonical JSON with schema and origins")
   def tuple_code_text = code_str((parsed_expr, parsed_expr))
   def tuple_code_roundtrip = json_decode(tuple_code_text)
   assert(tuple_code_roundtrip.len == 2 &&
      code_kind(tuple_code_roundtrip) == "expressions" &&
      tuple_code_text == code_str(tuple_code_roundtrip),
      "code_str canonically serializes tuple code sequences")
   print("✓ core.compiler.ast self-test passed")
}
