;; Keywords: compiler reflection semantic type effect ownership obligation
;; Read-only access to versioned semantic facts emitted by the compiler.
;; References:
;; - core.reflect
;; - core.math.parse.data.json
module core.compiler.reflect.semantic(semantic_snapshot, semantic_get, semantic_items,
   semantic_functions, semantic_find_function,
   semantic_find_function_at,
   semantic_layouts, semantic_find_layout,
   semantic_expressions,
   semantic_find_expression, semantic_calls,
   semantic_expression_type,
   semantic_expression_representation,
   semantic_expression_ownership,
   semantic_expression_mutable,
   semantic_expression_alias_class,
   semantic_expression_effects,
   semantic_expression_custom_effect_row,
   semantic_find_call, semantic_call_effects,
   semantic_call_effect_row,
   semantic_obligations,
   semantic_find_obligation,
   semantic_callable_effects,
   semantic_find_callable_effect,
   semantic_callable_effect_row,
   semantic_proof_obligations,
   semantic_find_invariant, semantic_find_law,
   semantic_find_lemma,
   semantic_find_fin_bound,
   semantic_statement_effects,
   semantic_statement_custom_effect_row,
   semantic_find_statement_effect,
   semantic_diagnostics,
   semantic_find_diagnostic,
   semantic_signature, semantic_parameters,
   semantic_return,
   semantic_signature_effects)
use core
use core.primitives as core_prim
use core.dict_mod
use core.math.parse.data.json as json

fn _semantic_tuple(list values) tuple {
   core_prim.list_as_tuple_raw(values)
}

fn _semantic_freeze(any value, int depth) dict {
   if depth > 64 { return {"ok":false, "value":nil, "error":"semantic artifact exceeds nesting limit"} }
   if is_nil(value) || is_bool(value) || is_int(value) ||
   is_float(value) || is_str(value) {
      return {"ok":true, "value":value, "error":""}
   }
   if is_list(value) || is_tuple(value) {
      if value.len > 65536 {
         return {"ok":false, "value":nil, "error":"semantic artifact sequence exceeds item limit"}
      }
      mut items = []
      mut i = 0
      while i < value.len {
         def item = _semantic_freeze(value.get(i), depth + 1)
         if !bool(item.get("ok", false)) { return item }
         items = items.append(item.get("value"))
         i += 1
      }
      return {"ok":true, "value":_semantic_tuple([
               "nytrix.reflect.sequence", 1, _semantic_tuple(items)
            ]), "error":""}
   }
   if is_dict(value) {
      if dict_len(value) > 65536 {
         return {"ok":false, "value":nil, "error":"semantic artifact record exceeds field limit"}
      }
      def keys = sorted(dict_keys(value))
      mut pairs = []
      mut i = 0
      while i < keys.len {
         def key = keys.get(i)
         def item = _semantic_freeze(value.get(key), depth + 1)
         if !bool(item.get("ok", false)) { return item }
         pairs = pairs.append(_semantic_tuple([key, item.get("value")]))
         i += 1
      }
      return {"ok":true, "value":_semantic_tuple([
               "nytrix.reflect.record", 1, _semantic_tuple(pairs)
            ]), "error":""}
   }
   {"ok":false, "value":nil, "error":"semantic artifact contains a non-JSON value"}
}

fn _semantic_schema_known(str schema, any version) bool {
   if !is_int(version) || type(version) == "bool" || version != 1 {
      return false
   }
   schema == "nytrix.semantic.call-effects" ||
   schema == "nytrix.semantic.callable-effects" ||
   schema == "nytrix.semantic.expression-effects" ||
   schema == "nytrix.semantic.expressions" ||
   schema == "nytrix.semantic.layouts" ||
   schema == "nytrix.semantic.obligations" ||
   schema == "nytrix.semantic.proof-obligations" ||
   schema == "nytrix.semantic.signature" ||
   schema == "nytrix.semantic.statement-effects" ||
   schema == "nytrix.semantic.statements"
}

fn _semantic_schemas_supported(any value, int depth) bool {
   if depth > 64 { return false }
   if is_dict(value) {
      if value.contains("schema") && value.contains("version") &&
      !_semantic_schema_known(str(value.get("schema")),
         value.get("version")) {
         return false
      }
      def keys = dict_keys(value)
      mut i = 0
      while i < keys.len {
         if !_semantic_schemas_supported(value.get(keys.get(i)), depth + 1) {
            return false
         }
         i += 1
      }
   } elif is_list(value) {
      mut i = 0
      while i < value.len {
         if !_semantic_schemas_supported(value.get(i), depth + 1) {
            return false
         }
         i += 1
      }
   }
   true
}

fn _semantic_value_within_depth(any value, int depth) bool {
   if depth > 64 { return false }
   if is_dict(value) {
      def keys = dict_keys(value)
      mut i = 0
      while i < keys.len {
         if !_semantic_value_within_depth(value.get(keys.get(i)), depth + 1) {
            return false
         }
         i += 1
      }
   } elif is_list(value) {
      mut i = 0
      while i < value.len {
         if !_semantic_value_within_depth(value.get(i), depth + 1) {
            return false
         }
         i += 1
      }
   }
   true
}

fn semantic_snapshot(str source) dict {
   "Parses and deeply freezes a compiler typed-AST artifact as versioned tuples."
   if source.len > 16777216 {
      return {"ok":false, "value":nil,
         "error":"semantic artifact exceeds 16 MiB limit"}
   }
   def parsed = json.json_try_decode_bounded(source, 64)
   if !bool(parsed.get("ok", false)) {
      if str(parsed.get("error", "")) == "JSON nesting limit exceeded" {
         return {"ok":false, "value":nil,
            "error":"semantic artifact exceeds nesting limit"}
      }
      return {"ok":false, "value":nil,
         "error":"invalid semantic artifact JSON: " +
         str(parsed.get("error", "decode failed"))}
   }
   def root = parsed.get("value")
   if !is_dict(root) || root.get("schema") != "typed_ast.v1" ||
   root.get("stage") != "hm" {
      return {"ok":false, "value":nil,
         "error":"expected a typed_ast.v1 HM artifact"}
   }
   def typed = root.get("typed")
   if !is_dict(typed) || typed.get("schema") != "typed_ast.v1" ||
   typed.get("engine") != "type-pipeline-v1" ||
   !is_list(typed.get("functions")) ||
   !(is_list(typed.get("layouts")) ||
      (is_dict(typed.get("layouts")) &&
         typed.get("layouts").get("schema") ==
         "nytrix.semantic.layouts" &&
         typed.get("layouts").get("version") == 1 &&
         is_list(typed.get("layouts").get("items")))) ||
   !is_list(typed.get("diagnostics")) {
      return {"ok":false, "value":nil,
         "error":"typed semantic facts have an unsupported shape"}
   }
   if !_semantic_value_within_depth(typed, 0) {
      return {"ok":false, "value":nil,
         "error":"semantic artifact exceeds nesting limit"}
   }
   if !_semantic_schemas_supported(typed, 0) {
      return {"ok":false, "value":nil,
         "error":"typed semantic artifact contains an unknown schema or version"}
   }
   def frozen = _semantic_freeze(typed, 0)
   if !bool(frozen.get("ok", false)) {
      return {"ok":false, "value":nil,
         "error":frozen.get("error", "invalid typed semantic facts")}
   }
   {"ok":true, "value":_semantic_tuple([
            "nytrix.reflect.semantic", 1, frozen.get("value")
         ]), "error":""}
}

fn semantic_get(any record, str key, any fallback=nil) any {
   "Reads a named field from a frozen semantic record."
   if !is_tuple(record) || record.len != 3 ||
   record.get(0) != "nytrix.reflect.record" || record.get(1) != 1 {
      return fallback
   }
   def pairs = record.get(2)
   mut i = 0
   while i < pairs.len {
      def pair = pairs.get(i)
      if pair.get(0) == key { return pair.get(1) }
      i += 1
   }
   fallback
}

fn _semantic_origin_matches(any origin, int line, int column, str file) bool {
   def origin_line = semantic_get(origin, "line")
   def origin_column = semantic_get(origin, "column")
   def origin_file = semantic_get(origin, "file")
   if type(origin_line) != "int" || type(origin_column) != "int" ||
   type(origin_file) != "str" { return false }
   origin_line == line && (column <= 0 || origin_column == column) &&
   (file == "" || origin_file == file)
}

fn semantic_items(any sequence) tuple {
   "Returns the immutable items of a frozen semantic sequence, or an empty tuple."
   if is_tuple(sequence) && sequence.len == 3 &&
   sequence.get(0) == "nytrix.reflect.record" && sequence.get(1) == 1 {
      def items = semantic_get(sequence, "items")
      if is_tuple(items) && items.len == 3 &&
      items.get(0) == "nytrix.reflect.sequence" && items.get(1) == 1 {
         return items.get(2)
      }
   }
   if !is_tuple(sequence) || sequence.len != 3 ||
   sequence.get(0) != "nytrix.reflect.sequence" || sequence.get(1) != 1 {
      return _semantic_tuple([])
   }
   sequence.get(2)
}

fn _semantic_typed(any snapshot) any {
   if !is_tuple(snapshot) || snapshot.len != 3 ||
   snapshot.get(0) != "nytrix.reflect.semantic" || snapshot.get(1) != 1 {
      return nil
   }
   snapshot.get(2)
}

fn _semantic_find_named(tuple records, str name, str owner="") any {
   mut found = nil
   mut i = 0
   while i < records.len {
      def record = records.get(i)
      if str(semantic_get(record, "name", "")) == name &&
      (owner == "" || str(semantic_get(record, "owner", "")) == owner) {
         if !is_nil(found) { return nil }
         found = record
      }
      i += 1
   }
   found
}

fn semantic_functions(any snapshot) tuple {
   "Returns the immutable typed function records from a semantic snapshot."
   def typed = _semantic_typed(snapshot)
   if is_nil(typed) { return _semantic_tuple([]) }
   semantic_items(semantic_get(typed, "functions"))
}

fn semantic_find_function(any snapshot, str name, str owner="") any {
   "Finds one typed function record by name and optional owner; ambiguous matches return nil."
   _semantic_find_named(semantic_functions(snapshot), name, owner)
}

fn semantic_find_function_at(any snapshot, str name, int line,
   int column=0, str file="") any {
   "Finds one function by name and authored declaration location; ambiguity returns nil."
   def functions = semantic_functions(snapshot)
   mut found = nil
   mut i = 0
   while i < functions.len {
      def function_record = functions.get(i)
      def origin = semantic_get(function_record, "source_origin")
      if str(semantic_get(function_record, "name", "")) == name &&
      _semantic_origin_matches(origin, line, column, file) {
         if !is_nil(found) { return nil }
         found = function_record
      }
      i += 1
   }
   found
}

fn semantic_layouts(any snapshot) tuple {
   "Returns the immutable resolved layout records from a semantic snapshot."
   def typed = _semantic_typed(snapshot)
   if is_nil(typed) { return _semantic_tuple([]) }
   semantic_items(semantic_get(typed, "layouts"))
}

fn semantic_find_layout(any snapshot, str name, str owner="") any {
   "Finds one resolved layout by name and optional owner; ambiguous names return nil."
   def layouts = semantic_layouts(snapshot)
   mut found = nil
   mut i = 0
   while i < layouts.len {
      def layout_record = layouts.get(i)
      def full_name = str(semantic_get(layout_record, "name", ""))
      def declared_name = str(semantic_get(layout_record, "declared_name", full_name))
      if (full_name == name || declared_name == name) &&
      (owner == "" || str(semantic_get(layout_record, "owner", "")) == owner) {
         if !is_nil(found) { return nil }
         found = layout_record
      }
      i += 1
   }
   found
}

fn semantic_signature(any function_record) any {
   "Returns a function's versioned semantic signature, or nil when absent or unsupported."
   def signature = semantic_get(function_record, "semantic_signature")
   if semantic_get(signature, "schema", "") !=
   "nytrix.semantic.signature" ||
   semantic_get(signature, "version") != 1 {
      return nil
   }
   signature
}

fn semantic_parameters(any function_record) tuple {
   "Returns immutable ordered semantic parameter facts for one function."
   semantic_items(semantic_get(semantic_signature(function_record),
         "parameters"))
}

fn semantic_return(any function_record) any {
   "Returns the resolved return slot facts, or nil when unavailable."
   semantic_get(semantic_signature(function_record), "return")
}

fn semantic_signature_effects(any function_record) any {
   "Returns declared and inferred effect facts from the semantic signature."
   semantic_get(semantic_signature(function_record), "effects")
}

fn semantic_expressions(any function_record) tuple {
   "Returns immutable expression facts from one typed function record."
   semantic_items(semantic_get(function_record, "expression_facts"))
}

fn semantic_expression_type(any expression) any {
   "Returns the resolved expression type, or nil when unavailable."
   semantic_get(expression, "type")
}

fn semantic_expression_representation(any expression) any {
   "Returns the resolved runtime representation for an expression."
   semantic_get(expression, "representation")
}

fn semantic_expression_ownership(any expression) any {
   "Returns the resolved ownership classification for an expression."
   semantic_get(expression, "ownership")
}

fn semantic_expression_mutable(any expression) any {
   "Returns whether the expression denotes a mutable value, or nil if absent."
   semantic_get(expression, "mutable")
}

fn semantic_expression_alias_class(any expression) any {
   "Returns the compiler alias-class identifier, or nil if unavailable."
   semantic_get(expression, "alias_class")
}

fn semantic_expression_effects(any expression) any {
   "Returns a supported versioned expression-effect record, or nil."
   semantic_get(expression, "expression_effects")
}

fn semantic_expression_custom_effect_row(any expression) any {
   "Returns a closed inferred custom effect row string, or nil when unknown."
   def effects = semantic_expression_effects(expression)
   if semantic_get(effects, "schema") !=
   "nytrix.semantic.expression-effects" ||
   semantic_get(effects, "version") != 1 { return nil }
   def row = semantic_get(effects, "inferred_custom_row")
   if is_str(row) { return row }
   nil
}

fn semantic_find_expression(any function_record, str kind, int line,
   int column=0, str file="") any {
   "Finds one expression by kind and source location; ambiguous matches return nil."
   def expressions = semantic_expressions(function_record)
   mut found = nil
   mut i = 0
   while i < expressions.len {
      def expression = expressions.get(i)
      def origin = semantic_get(expression, "source_origin")
      if str(semantic_get(expression, "kind", "")) == kind &&
      _semantic_origin_matches(origin, line, column, file) {
         if !is_nil(found) { return nil }
         found = expression
      }
      i += 1
   }
   found
}

fn semantic_calls(any function_record) tuple {
   "Returns immutable call-expression facts from one typed function record."
   def expressions = semantic_expressions(function_record)
   mut calls = []
   mut i = 0
   while i < expressions.len {
      def expression = expressions.get(i)
      def kind = str(semantic_get(expression, "kind", ""))
      if kind == "call" || kind == "memcall" {
         calls = calls.append(expression)
      }
      i += 1
   }
   _semantic_tuple(calls)
}

fn semantic_find_call(any function_record, int line, int column=0,
   str file="") any {
   "Finds one call expression by source location; ambiguous matches return nil."
   def calls = semantic_calls(function_record)
   mut found = nil
   mut i = 0
   while i < calls.len {
      def call = calls.get(i)
      def origin = semantic_get(call, "source_origin")
      if _semantic_origin_matches(origin, line, column, file) {
         if !is_nil(found) { return nil }
         found = call
      }
      i += 1
   }
   found
}

fn semantic_call_effects(any call) any {
   "Returns a direct or member call's versioned effect summary, or nil otherwise."
   def kind = str(semantic_get(call, "kind", ""))
   if kind != "call" && kind != "memcall" { return nil }
   def effects = semantic_get(call, "callee_effects")
   if semantic_get(effects, "schema") == "nytrix.semantic.call-effects" &&
   semantic_get(effects, "version") == 1 { return effects }
   nil
}

fn semantic_call_effect_row(any call) any {
   "Returns a known serialized effect row for a direct or member call, or nil."
   def effects = semantic_call_effects(call)
   if semantic_get(effects, "known") != true { return nil }
   def row = semantic_get(effects, "row")
   if is_str(row) { return row }
   nil
}

fn semantic_obligations(any function_record) tuple {
   "Returns immutable proof-obligation records attached to calls in a typed function."
   def expressions = semantic_expressions(function_record)
   mut obligations = []
   mut i = 0
   while i < expressions.len {
      def expression = expressions.get(i)
      def facts = semantic_get(expression, "obligations")
      def kind = str(semantic_get(expression, "kind", ""))
      if (kind == "call" || kind == "memcall") &&
      semantic_get(facts, "schema", "") ==
      "nytrix.semantic.obligations" &&
      semantic_get(facts, "version") == 1 &&
      (!is_nil(semantic_get(facts, "requires")) ||
         !is_nil(semantic_get(facts, "decreases"))) {
         obligations = obligations.append(facts)
      }
      i += 1
   }
   _semantic_tuple(obligations)
}

fn semantic_callable_effects(any snapshot) tuple {
   "Returns the immutable unit-wide callable effect rows from a semantic snapshot."
   def typed = _semantic_typed(snapshot)
   if is_nil(typed) { return _semantic_tuple([]) }
   def table = semantic_get(typed, "callable_effects")
   if semantic_get(table, "schema", "") !=
   "nytrix.semantic.callable-effects" ||
   semantic_get(table, "version") != 1 {
      return _semantic_tuple([])
   }
   semantic_items(table)
}

fn _semantic_callable_matches(any record, str name, int arity) bool {
   if str(semantic_get(record, "name", "")) != name { return false }
   if arity < 0 { return true }
   def record_arity = semantic_get(record, "arity")
   if type(record_arity) != "int" || type(record_arity) == "bool" {
      return false
   }
   if record_arity == arity { return true }
   bool(semantic_get(record, "variadic", false)) && arity > record_arity
}

fn semantic_find_callable_effect(any snapshot, str name, int arity=0) any {
   "Finds one callable effect row by name and argument count; ambiguous matches return nil."
   if name == "" { return nil }
   def rows = semantic_callable_effects(snapshot)
   mut found = nil
   mut i = 0
   while i < rows.len {
      def row = rows.get(i)
      if _semantic_callable_matches(row, name, arity) {
         if !is_nil(found) { return nil }
         found = row
      }
      i += 1
   }
   found
}

fn semantic_callable_effect_row(any callable) any {
   "Returns the closed serialized effect row of a callable record, or nil."
   if semantic_get(callable, "known") != true { return nil }
   def row = semantic_get(callable, "row")
   if is_str(row) { return row }
   nil
}

fn semantic_proof_obligations(any snapshot) tuple {
   "Returns immutable proof records, including lemmas and Fin<N> parameter bounds."
   def typed = _semantic_typed(snapshot)
   if is_nil(typed) { return _semantic_tuple([]) }
   def table = semantic_get(typed, "proof_obligations")
   if semantic_get(table, "schema", "") !=
   "nytrix.semantic.proof-obligations" ||
   semantic_get(table, "version") != 1 {
      return _semantic_tuple([])
   }
   semantic_items(table)
}

fn _semantic_origin_line(any record) any {
   semantic_get(semantic_get(record, "source_origin"), "line")
}

fn _semantic_find_proof_record(any snapshot, str kind, int line, int column,
   str file) any {
   if kind != "invariant" && kind != "impl-law" && kind != "refinement" &&
   kind != "lemma" && kind != "fin-bound" {
      return nil
   }
   def records = semantic_proof_obligations(snapshot)
   mut found = nil
   mut i = 0
   while i < records.len {
      def record = records.get(i)
      if str(semantic_get(record, "kind", "")) == kind &&
      (line <= 0 || _semantic_origin_line(record) == line) &&
      (column <= 0 ||
         semantic_get(semantic_get(record, "source_origin"), "column") ==
         column) &&
      (file == "" ||
         str(semantic_get(semantic_get(record, "source_origin"), "file", "")) ==
         file) {
         if !is_nil(found) { return nil }
         found = record
      }
      i += 1
   }
   found
}

fn semantic_find_lemma(any snapshot, str name, int line=0) any {
   "Finds one lemma obligation by name and optional source line."
   if name == "" { return nil }
   def records = semantic_proof_obligations(snapshot)
   mut found = nil
   mut i = 0
   while i < records.len {
      def record = records.get(i)
      if semantic_get(record, "kind", "") == "lemma" &&
      semantic_get(record, "name", "") == name &&
      (line <= 0 || _semantic_origin_line(record) == line) {
         if !is_nil(found) { return nil }
         found = record
      }
      i += 1
   }
   found
}

fn semantic_find_fin_bound(any snapshot, str name, int line=0) any {
   "Finds one Fin<N> parameter-domain record by name and optional source line."
   if name == "" { return nil }
   def records = semantic_proof_obligations(snapshot)
   mut found = nil
   mut i = 0
   while i < records.len {
      def record = records.get(i)
      if semantic_get(record, "kind", "") == "fin-bound" &&
      semantic_get(record, "name", "") == name &&
      (line <= 0 || _semantic_origin_line(record) == line) {
         if !is_nil(found) { return nil }
         found = record
      }
      i += 1
   }
   found
}

fn semantic_find_invariant(any snapshot, int line, int column=0,
   str file="") any {
   "Finds one loop invariant obligation by source origin; ambiguous matches return nil."
   _semantic_find_proof_record(snapshot, "invariant", line, column, file)
}

fn semantic_find_law(any snapshot, str trait_name, str type_name,
   int line=0) any {
   "Finds one impl law obligation by trait and type names; ambiguous matches return nil."
   if trait_name == "" && type_name == "" { return nil }
   def records = semantic_proof_obligations(snapshot)
   mut found = nil
   mut i = 0
   while i < records.len {
      def record = records.get(i)
      if str(semantic_get(record, "kind", "")) == "impl-law" &&
      (trait_name == "" ||
         str(semantic_get(record, "trait", "")) == trait_name) &&
      (type_name == "" ||
         str(semantic_get(record, "type", "")) == type_name) &&
      (line <= 0 || _semantic_origin_line(record) == line) {
         if !is_nil(found) { return nil }
         found = record
      }
      i += 1
   }
   found
}

fn semantic_find_obligation(any record, str kind, int line,
   int column=0, str file="") any {
   "Finds one obligation by kind and source origin; ambiguous matches return nil."
   if kind != "requires" && kind != "decreases" &&
   kind != "invariant" && kind != "impl-law" && kind != "refinement" &&
   kind != "lemma" && kind != "fin-bound" {
      return nil
   }
   if kind == "invariant" || kind == "impl-law" || kind == "refinement" ||
   kind == "lemma" || kind == "fin-bound" {
      return _semantic_find_proof_record(record, kind, line, column, file)
   }
   if !is_nil(_semantic_typed(record)) {
      def functions = semantic_functions(record)
      mut found = nil
      mut i = 0
      while i < functions.len {
         def candidate = semantic_find_obligation(functions.get(i), kind, line,
            column, file)
         if !is_nil(candidate) {
            if !is_nil(found) { return nil }
            found = candidate
         }
         i += 1
      }
      return found
   }
   def call = semantic_find_call(record, line, column, file)
   if is_nil(call) { return nil }
   def obligation = semantic_get(call, "obligations")
   if is_nil(semantic_get(obligation, kind)) { return nil }
   obligation
}

fn semantic_statement_effects(any function_record) tuple {
   "Returns immutable statement effect records from one typed function."
   def statement_facts = semantic_get(function_record, "statement_effects")
   semantic_items(statement_facts)
}

fn semantic_statement_custom_effect_row(any statement) any {
   "Returns a closed inferred custom effect row string, or nil when unknown."
   if semantic_get(statement, "schema") !=
   "nytrix.semantic.statement-effects" ||
   semantic_get(statement, "version") != 1 { return nil }
   def row = semantic_get(statement, "inferred_custom_row")
   if is_str(row) { return row }
   nil
}

fn semantic_find_statement_effect(any function_record, str kind, int line,
   int column=0, str file="") any {
   "Finds one statement effect record by kind and source location; ambiguous matches return nil."
   def statements = semantic_statement_effects(function_record)
   mut found = nil
   mut i = 0
   while i < statements.len {
      def statement = statements.get(i)
      def origin = semantic_get(statement, "source_origin")
      if str(semantic_get(statement, "kind", "")) == kind &&
      _semantic_origin_matches(origin, line, column, file) {
         if !is_nil(found) { return nil }
         found = statement
      }
      i += 1
   }
   found
}

fn semantic_diagnostics(any snapshot) tuple {
   "Returns immutable diagnostic records from a typed semantic snapshot."
   def typed = _semantic_typed(snapshot)
   if is_nil(typed) { return _semantic_tuple([]) }
   semantic_items(semantic_get(typed, "diagnostics"))
}

fn semantic_find_diagnostic(any snapshot, str code, str file="",
   int line=0, int column=0) any {
   "Finds one diagnostic by stable code and optional source location; ambiguity returns nil."
   if code == "" { return nil }
   def diagnostics = semantic_diagnostics(snapshot)
   mut found = nil
   mut i = 0
   while i < diagnostics.len {
      def diagnostic = diagnostics.get(i)
      if str(semantic_get(diagnostic, "code", "")) == code &&
      (file == "" || str(semantic_get(diagnostic, "file", "")) == file) &&
      (line <= 0 || semantic_get(diagnostic, "line") == line) &&
      (column <= 0 || semantic_get(diagnostic, "col") == column) {
         if !is_nil(found) { return nil }
         found = diagnostic
      }
      i += 1
   }
   found
}
