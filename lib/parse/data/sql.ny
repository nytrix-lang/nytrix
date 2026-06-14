;; Keywords: data sql database query ast parse serialization
;; SQL tokenizer and parser for statement inspection, routing, normalization, and query-building tools.
;; References:
;; - std.parse.data
;; - std.parse
module std.parse.data.sql(tokenize, parse, try_parse, parse_all, normalize, statement_kind)
use std.core
use std.core.dict_mod as _d
use std.core.str

fn _tok(str kind, any value, int pos) dict {
   return {"kind": kind, "value": value, "pos": pos}
}

fn _is_ws(int c) bool { c == 32 || c == 9 || c == 10 || c == 13 || c == 11 || c == 12 }

fn _is_alpha(int c) bool { (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c == 95 }

fn _is_digit(int c) bool { c >= 48 && c <= 57 }

fn _is_ident(int c) bool { _is_alpha(c) || _is_digit(c) || c == 36 }

fn _keyword(str s) bool {
   def u = upper(s)
   u == "SELECT" || u == "DISTINCT" || u == "ALL" || u == "FROM" || u == "WHERE" ||
   u == "GROUP" || u == "BY" || u == "HAVING" || u == "ORDER" || u == "LIMIT" ||
   u == "OFFSET" || u == "AS" || u == "ASC" || u == "DESC" || u == "JOIN" ||
   u == "INNER" || u == "LEFT" || u == "RIGHT" || u == "FULL" || u == "OUTER" ||
   u == "CROSS" || u == "ON" || u == "USING" || u == "AND" || u == "OR" ||
   u == "NOT" || u == "NULL" || u == "TRUE" || u == "FALSE" || u == "IS" ||
   u == "IN" || u == "LIKE" || u == "BETWEEN" || u == "INSERT" || u == "INTO" ||
   u == "VALUES" || u == "UPDATE" || u == "SET" || u == "DELETE" || u == "CREATE" ||
   u == "TABLE" || u == "DROP" || u == "IF" || u == "EXISTS" || u == "PRIMARY" ||
   u == "KEY" || u == "DEFAULT" || u == "REFERENCES" || u == "UNIQUE" ||
   u == "CHECK" || u == "RETURNING" || u == "BEGIN" || u == "COMMIT" || u == "ROLLBACK"
}

fn _quoted(str sql, int pos, int quote) list {
   mut b = Builder(32)
   mut i = pos + 1
   while i < sql.len {
      def c = load8(sql, i)
      if c == quote {
         if i + 1 < sql.len && load8(sql, i + 1) == quote {
            b = builder_append(b, chr(quote))
            i += 2
            continue
         }
         def out = builder_to_str(b)
         builder_free(b)
         return [out, i + 1]
      }
      b = builder_append(b, chr(c))
      i += 1
   }
   def out = builder_to_str(b)
   builder_free(b)
   [out, i]
}

fn _bracket_ident(str sql, int pos) list {
   mut b = Builder(32)
   mut i = pos + 1
   while i < sql.len {
      def c = load8(sql, i)
      if c == 93 {
         def out = builder_to_str(b)
         builder_free(b)
         return [out, i + 1]
      }
      b = builder_append(b, chr(c))
      i += 1
   }
   def out = builder_to_str(b)
   builder_free(b)
   [out, i]
}

fn tokenize(any sql) list {
   "Tokenizes SQL into `{kind, value, pos}` dictionaries. Comments and whitespace are skipped."
   if !is_str(sql) { return [_tok("eof", "", 0)] }
   mut out = []
   mut i = 0
   while i < sql.len {
      def c = load8(sql, i)
      if _is_ws(c) { i += 1 continue }
      if c == 45 && i + 1 < sql.len && load8(sql, i + 1) == 45 {
         i += 2
         while i < sql.len && load8(sql, i) != 10 { i += 1 }
         continue
      }
      if c == 47 && i + 1 < sql.len && load8(sql, i + 1) == 42 {
         i += 2
         while i + 1 < sql.len && !(load8(sql, i) == 42 && load8(sql, i + 1) == 47) { i += 1 }
         i = (i + 1 < sql.len) ? i + 2 : sql.len
         continue
      }
      if _is_alpha(c) {
         def start = i
         i += 1
         while i < sql.len && _is_ident(load8(sql, i)) { i += 1 }
         def text = slice(sql, start, i)
         out = out.append(_tok(_keyword(text) ? "kw" : "ident", _keyword(text) ? upper(text) : text, start))
         continue
      }
      if _is_digit(c) {
         def start = i
         i += 1
         while i < sql.len && _is_digit(load8(sql, i)) { i += 1 }
         if i < sql.len && load8(sql, i) == 46 {
            i += 1
            while i < sql.len && _is_digit(load8(sql, i)) { i += 1 }
         }
         if i < sql.len && (load8(sql, i) == 101 || load8(sql, i) == 69) {
            i += 1
            if i < sql.len && (load8(sql, i) == 43 || load8(sql, i) == 45) { i += 1 }
            while i < sql.len && _is_digit(load8(sql, i)) { i += 1 }
         }
         def text = slice(sql, start, i)
         out = out.append(_tok("number", text, start))
         continue
      }
      if c == 39 {
         def q = _quoted(sql, i, 39)
         out = out.append(_tok("string", q[0], i))
         i = q[1]
         continue
      }
      if c == 34 || c == 96 {
         def q = _quoted(sql, i, c)
         out = out.append(_tok("ident", q[0], i))
         i = q[1]
         continue
      }
      if c == 91 {
         def q = _bracket_ident(sql, i)
         out = out.append(_tok("ident", q[0], i))
         i = q[1]
         continue
      }
      if c == 63 {
         out = out.append(_tok("param", "?", i))
         i += 1
         continue
      }
      if c == 58 || c == 36 {
         def start = i
         i += 1
         while i < sql.len && _is_ident(load8(sql, i)) { i += 1 }
         out = out.append(_tok("param", slice(sql, start, i), start))
         continue
      }
      def two = (i + 1 < sql.len) ? slice(sql, i, i + 2) : ""
      if case two { "<=", ">=", "<>", "!=", "||", "::" -> true _ -> false }{
         out = out.append(_tok("op", two, i))
         i += 2
         continue
      }
      out = out.append(_tok("op", chr(c), i))
      i += 1
   }
   out.append(_tok("eof", "", sql.len))
}

fn _result(bool ok_v, any value, str error, int pos) dict {
   return {"ok": ok_v, "value": value, "error": error, "pos": pos}
}

fn _st(list toks) list { [toks, 0, ""] }

fn _peek(list st, int off=0) dict { st[0].get(st[1] + off, _tok("eof", "", 0)) }

fn _pos(list st) int { _peek(st).get("pos", 0) }

fn _err(list st, str msg) int {
   if st[2].len == 0 { st[2] = msg }
   return 0
}

fn _eof(list st) bool { _peek(st).get("kind", "") == "eof" }

fn _val(list st, int off=0) str { to_str(_peek(st, off).get("value", "")) }

fn _consume(list st) dict { def t = _peek(st) st[1] = st[1] + 1 t }

fn _match_val(list st, str v) bool { upper(_val(st)) == upper(v) }

fn _accept_val(list st, str v) bool { if _match_val(st, v) { _consume(st) return true } false }

fn _expect_val(list st, str v) bool { if _accept_val(st, v) { return true } _err(st, "expected " + v) false }

fn _is_name_tok(dict t) bool { t.get("kind", "") == "ident" || t.get("kind", "") == "kw" }

fn _name(list st) str {
   def t = _peek(st)
   if !_is_name_tok(t) { _err(st, "expected identifier") return "" }
   _consume(st)
   to_str(t.get("value", ""))
}

fn _qualified_name(list st) str {
   mut out = _name(st)
   while st[2].len == 0 && _accept_val(st, ".") {
      if _accept_val(st, "*") { out = out + ".*" }
      else { out = out + "." + _name(st) }
   }
   out
}

fn _literal(any v) dict { {"node": "literal", "value": v} }

fn _ident(str name) dict { {"node": "identifier", "name": name} }

fn _parse_expr_list(list st, str end_val=")") list {
   mut xs = []
   if _match_val(st, end_val) { return xs }
   while !_eof(st) && st[2].len == 0 {
      xs = xs.append(_parse_expr(st, 1))
      if !_accept_val(st, ",") { break }
   }
   xs
}

fn _parse_clause_expr_list(list st) list {
   mut xs = []
   while !_eof(st) && st[2].len == 0 && !_clause_start(st) {
      xs = xs.append(_parse_expr(st, 1))
      if !_accept_val(st, ",") { break }
   }
   xs
}

fn _parse_primary(list st) dict {
   def t = _peek(st)
   def k = t.get("kind", "")
   def v = to_str(t.get("value", ""))
   if k == "number" {
      _consume(st)
      return {"node": "number", "value": v}
   }
   if k == "string" {
      _consume(st)
      return {"node": "string", "value": v}
   }
   if k == "param" {
      _consume(st)
      return {"node": "param", "name": v}
   }
   if _match_val(st, "NULL") { _consume(st) return _literal(nil) }
   if _match_val(st, "TRUE") { _consume(st) return _literal(true) }
   if _match_val(st, "FALSE") { _consume(st) return _literal(false) }
   if _accept_val(st, "*") { return {"node": "star"} }
   if _accept_val(st, "(") {
      if _match_val(st, "SELECT") {
         def sub = _parse_select(st)
         _expect_val(st, ")")
         return {"node": "subquery", "query": sub}
      }
      def e = _parse_expr(st, 1)
      _expect_val(st, ")")
      return e
   }
   if _is_name_tok(t) {
      def name = _qualified_name(st)
      if _accept_val(st, "(") {
         def args = _parse_expr_list(st, ")")
         _expect_val(st, ")")
         return {"node": "call", "name": name, "args": args}
      }
      return _ident(name)
   }
   _err(st, "expected expression")
   _literal(0)
}

fn _op_prec(str op) int {
   def u = upper(op)
   case u {
      "OR" -> 1
      "AND" -> 2
      "=", "!=", "<>", "<", "<=", ">", ">=", "LIKE", "IN", "IS" -> 3
      "||" -> 4
      "+", "-" -> 5
      "*", "/", "%" -> 6
      _ -> 0
   }
}

fn _parse_prefix(list st) dict {
   if _match_val(st, "NOT") || _match_val(st, "+") || _match_val(st, "-") {
      mut op = _val(st)
      _consume(st)
      return {"node": "unary", "op": upper(op), "expr": _parse_expr(st, 7)}
   }
   _parse_primary(st)
}

fn _parse_expr(list st, int min_prec=1) dict {
   mut left = _parse_prefix(st)
   while st[2].len == 0 {
      mut op = _val(st)
      def prec = _op_prec(op)
      if prec < min_prec || prec == 0 { break }
      _consume(st)
      if upper(op) == "IS" && _accept_val(st, "NOT") {
         op = "IS NOT"
      }
      if upper(op) == "IN" && _accept_val(st, "(") {
         def vals = _parse_expr_list(st, ")")
         _expect_val(st, ")")
         left = {"node": "in", "expr": left, "values": vals}
      } else {
         def right = _parse_expr(st, prec + 1)
         left = {"node": "binary", "op": upper(op), "left": left, "right": right}
      }
   }
   left
}

fn _clause_start(list st) bool {
   _match_val(st, "FROM") || _match_val(st, "WHERE") || _match_val(st, "GROUP") ||
   _match_val(st, "HAVING") || _match_val(st, "ORDER") || _match_val(st, "LIMIT") ||
   _match_val(st, "OFFSET") || _match_val(st, "RETURNING") || _match_val(st, ";") ||
   _eof(st)
}

fn _select_item(list st) dict {
   def expr = _parse_expr(st, 1)
   mut alias = ""
   if _accept_val(st, "AS") { alias = _name(st) }
   elif !_clause_start(st) && _peek(st).get("kind", "") == "ident" { alias = _name(st) }
   return {"expr": expr, "alias": alias}
}

fn _parse_select_list(list st) list {
   mut cols = []
   while !_eof(st) && st[2].len == 0 {
      if _clause_start(st) { break }
      cols = cols.append(_select_item(st))
      if !_accept_val(st, ",") { break }
   }
   cols
}

fn _parse_table_ref(list st) dict {
   mut item = {"name": _qualified_name(st), "alias": "", "joins": []}
   if _accept_val(st, "AS") { item["alias"] = _name(st) }
   elif _peek(st).get("kind", "") == "ident" { item["alias"] = _name(st) }
   mut joins = []
   while st[2].len == 0 {
      mut kind = ""
      if _match_val(st, "INNER") || _match_val(st, "LEFT") || _match_val(st, "RIGHT") ||
      _match_val(st, "FULL") || _match_val(st, "CROSS"){
         kind = upper(_val(st))
         _consume(st)
         if _match_val(st, "OUTER") { kind = kind + " OUTER" _consume(st) }
      }
      if !_accept_val(st, "JOIN") {
         if kind.len > 0 { _err(st, "expected JOIN") }
         break
      }
      mut j = {"kind": kind.len > 0 ? kind : "JOIN", "table": _qualified_name(st), "alias": "", "on": 0, "using": []}
      if _accept_val(st, "AS") { j["alias"] = _name(st) }
      elif _peek(st).get("kind", "") == "ident" { j["alias"] = _name(st) }
      if _accept_val(st, "ON") { j["on"] = _parse_expr(st, 1) }
      elif _accept_val(st, "USING") {
         _expect_val(st, "(")
         mut ns = []
         while !_eof(st) && !_match_val(st, ")") {
            ns = ns.append(_name(st))
            if !_accept_val(st, ",") { break }
         }
         _expect_val(st, ")")
         j["using"] = ns
      }
      joins = joins.append(j)
   }
   item["joins"] = joins
   item
}

fn _parse_from(list st) list {
   mut xs = []
   if !_accept_val(st, "FROM") { return xs }
   while !_eof(st) && st[2].len == 0 {
      if _clause_start(st) && !_match_val(st, "FROM") { break }
      xs = xs.append(_parse_table_ref(st))
      if !_accept_val(st, ",") { break }
   }
   xs
}

fn _parse_order(list st) list {
   mut xs = []
   if !(_accept_val(st, "ORDER") && _expect_val(st, "BY")) { return xs }
   while !_eof(st) && st[2].len == 0 {
      def e = _parse_expr(st, 1)
      mut dir = ""
      if _match_val(st, "ASC") || _match_val(st, "DESC") { dir = upper(_val(st)) _consume(st) }
      xs = xs.append({"expr": e, "dir": dir})
      if !_accept_val(st, ",") { break }
   }
   xs
}

fn _parse_select(list st) dict {
   _expect_val(st, "SELECT")
   mut out = {"kind": "select", "distinct": false, "columns": [], "from": [], "where": 0, "group_by": [], "having": 0, "order_by": [], "limit": 0, "offset": 0}
   if _accept_val(st, "DISTINCT") { out["distinct"] = true }
   elif _accept_val(st, "ALL") { out["distinct"] = false }
   out["columns"] = _parse_select_list(st)
   out["from"] = _parse_from(st)
   if _accept_val(st, "WHERE") { out["where"] = _parse_expr(st, 1) }
   if _accept_val(st, "GROUP") { _expect_val(st, "BY") out["group_by"] = _parse_clause_expr_list(st) }
   if _accept_val(st, "HAVING") { out["having"] = _parse_expr(st, 1) }
   if _match_val(st, "ORDER") { out["order_by"] = _parse_order(st) }
   if _accept_val(st, "LIMIT") { out["limit"] = _parse_expr(st, 1) }
   if _accept_val(st, "OFFSET") { out["offset"] = _parse_expr(st, 1) }
   out
}

fn _parse_names_in_parens(list st) list {
   mut xs = []
   if !_accept_val(st, "(") { return xs }
   while !_eof(st) && st[2].len == 0 && !_match_val(st, ")") {
      xs = xs.append(_name(st))
      if !_accept_val(st, ",") { break }
   }
   _expect_val(st, ")")
   xs
}

fn _parse_insert(list st) dict {
   _expect_val(st, "INSERT")
   _expect_val(st, "INTO")
   mut out = {"kind": "insert", "table": _qualified_name(st), "columns": [], "values": [], "query": 0, "returning": []}
   if _match_val(st, "(") { out["columns"] = _parse_names_in_parens(st) }
   if _accept_val(st, "VALUES") {
      mut rows = []
      while _accept_val(st, "(") {
         rows = rows.append(_parse_expr_list(st, ")"))
         _expect_val(st, ")")
         if !_accept_val(st, ",") { break }
      }
      out["values"] = rows
   } elif _match_val(st, "SELECT") {
      out["query"] = _parse_select(st)
   }
   if _accept_val(st, "RETURNING") { out["returning"] = _parse_select_list(st) }
   out
}

fn _parse_update(list st) dict {
   _expect_val(st, "UPDATE")
   mut out = {"kind": "update", "table": _qualified_name(st), "set": [], "where": 0, "returning": []}
   _expect_val(st, "SET")
   mut assigns = []
   while !_eof(st) && st[2].len == 0 {
      def name = _qualified_name(st)
      _expect_val(st, "=")
      assigns = assigns.append({"column": name, "value": _parse_expr(st, 1)})
      if !_accept_val(st, ",") { break }
   }
   out["set"] = assigns
   if _accept_val(st, "WHERE") { out["where"] = _parse_expr(st, 1) }
   if _accept_val(st, "RETURNING") { out["returning"] = _parse_select_list(st) }
   out
}

fn _parse_delete(list st) dict {
   _expect_val(st, "DELETE")
   _expect_val(st, "FROM")
   mut out = {"kind": "delete", "table": _qualified_name(st), "where": 0, "returning": []}
   if _accept_val(st, "WHERE") { out["where"] = _parse_expr(st, 1) }
   if _accept_val(st, "RETURNING") { out["returning"] = _parse_select_list(st) }
   out
}

fn _parse_create_table(list st) dict {
   _expect_val(st, "CREATE")
   _expect_val(st, "TABLE")
   mut out = {"kind": "create_table", "if_not_exists": false, "name": "", "columns": []}
   if _accept_val(st, "IF") { _expect_val(st, "NOT") _expect_val(st, "EXISTS") out["if_not_exists"] = true }
   out["name"] = _qualified_name(st)
   if _accept_val(st, "(") {
      mut cols = []
      while !_eof(st) && st[2].len == 0 && !_match_val(st, ")") {
         def cname = _name(st)
         mut parts = []
         while !_eof(st) && !_match_val(st, ",") && !_match_val(st, ")") {
            parts = parts.append(_val(st))
            _consume(st)
         }
         cols = cols.append({"name": cname, "definition": join(parts, " ")})
         if !_accept_val(st, ",") { break }
      }
      _expect_val(st, ")")
      out["columns"] = cols
   }
   out
}

fn _parse_drop_table(list st) dict {
   _expect_val(st, "DROP")
   _expect_val(st, "TABLE")
   mut out = {"kind": "drop_table", "if_exists": false, "name": ""}
   if _accept_val(st, "IF") { _expect_val(st, "EXISTS") out["if_exists"] = true }
   out["name"] = _qualified_name(st)
   out
}

fn _parse_statement(list st) dict {
   if _match_val(st, "SELECT") { return _parse_select(st) }
   if _match_val(st, "INSERT") { return _parse_insert(st) }
   if _match_val(st, "UPDATE") { return _parse_update(st) }
   if _match_val(st, "DELETE") { return _parse_delete(st) }
   if _match_val(st, "CREATE") { return _parse_create_table(st) }
   if _match_val(st, "DROP") { return _parse_drop_table(st) }
   if _match_val(st, "BEGIN") || _match_val(st, "COMMIT") || _match_val(st, "ROLLBACK") {
      def k = lower(_val(st))
      _consume(st)
      return {"kind": k}
   }
   _err(st, "unknown SQL statement")
   return {"kind": "unknown"}
}

fn try_parse(any sql) dict {
   "Parses one SQL statement and returns `{ok, value, error, pos}`."
   def st = _st(tokenize(sql))
   while _accept_val(st, ";") {}
   def ast = _parse_statement(st)
   if st[2].len == 0 && _match_val(st, ";") { _consume(st) }
   if st[2].len == 0 && !_eof(st) { _err(st, "trailing tokens after SQL statement") }
   if st[2].len > 0 { return _result(false, 0, st[2], _pos(st)) }
   _result(true, ast, "", _pos(st))
}

fn parse(any sql) any {
   "Parses one SQL statement and returns its AST, or `0` on error."
   def r = try_parse(sql)
   r.get("ok", false) ? r.get("value", 0) : 0
}

fn parse_all(any sql) dict {
   "Parses semicolon-separated SQL statements and returns `{ok, value, error, pos}`."
   def st = _st(tokenize(sql))
   mut xs = []
   while !_eof(st) && st[2].len == 0 {
      while _accept_val(st, ";") {}
      if _eof(st) { break }
      xs = xs.append(_parse_statement(st))
      if _match_val(st, ";") { _consume(st) }
      elif !_eof(st) { _err(st, "expected semicolon between SQL statements") }
   }
   if st[2].len > 0 { return _result(false, 0, st[2], _pos(st)) }
   _result(true, xs, "", _pos(st))
}

fn normalize(any sql) str {
   "Returns a whitespace-normalized SQL token stream."
   def toks = tokenize(sql)
   mut parts = []
   mut i = 0
   while i < toks.len {
      def t = toks[i]
      if t.get("kind", "") != "eof" { parts = parts.append(to_str(t.get("value", ""))) }
      i += 1
   }
   join(parts, " ")
}

fn statement_kind(any sql) str {
   "Returns the parsed statement kind, or an empty string on parse error."
   def ast = parse(sql)
   is_dict(ast) ? ast.get("kind", "") : ""
}

#main {
   def toks = tokenize("select a, 'b''c' from t where id = :id")
   assert(toks.len > 6, "sql tokenizer returns tokens")
   assert_eq(toks[0].get("value", ""), "SELECT", "sql keyword normalized")
   assert_eq(toks[3].get("value", ""), "b'c", "sql string unescaped")
   def ast = parse("SELECT DISTINCT u.id, count(*) AS n FROM users u LEFT JOIN logs l ON l.user_id = u.id WHERE u.id = :id ORDER BY n DESC LIMIT 10")
   assert(is_dict(ast), "sql parse returns ast")
   assert_eq(ast.get("kind", ""), "select", "select kind")
   assert_eq(ast.get("distinct", false), true, "select distinct")
   assert_eq(ast.get("columns", []).len, 2, "select columns")
   assert_eq(ast.get("from", [])[0].get("name", ""), "users", "from table")
   assert_eq(ast.get("from", [])[0].get("alias", ""), "u", "from alias")
   assert_eq(ast.get("from", [])[0].get("joins", [])[0].get("kind", ""), "LEFT", "join kind")
   assert_eq(ast.get("where", 0).get("node", ""), "binary", "where expression")
   assert_eq(ast.get("order_by", [])[0].get("dir", ""), "DESC", "order direction")
   def ins = parse("INSERT INTO audit(user_id, action) VALUES(7, 'login'), (8, 'logout') RETURNING id")
   assert_eq(ins.get("kind", ""), "insert", "insert kind")
   assert_eq(ins.get("columns", []).len, 2, "insert columns")
   assert_eq(ins.get("values", []).len, 2, "insert values")
   assert_eq(ins.get("returning", []).len, 1, "insert returning")
   def upd = parse("UPDATE users SET name = 'ny', visits = visits + 1 WHERE id = ?")
   assert_eq(upd.get("kind", ""), "update", "update kind")
   assert_eq(upd.get("set", []).len, 2, "update assignments")
   def ddl = parse("CREATE TABLE IF NOT EXISTS users(id INTEGER PRIMARY KEY, name TEXT DEFAULT 'x')")
   assert_eq(ddl.get("kind", ""), "create_table", "create table kind")
   assert_eq(ddl.get("if_not_exists", false), true, "create if not exists")
   assert_eq(ddl.get("columns", []).len, 2, "create columns")
   def all = parse_all("BEGIN; DELETE FROM users WHERE id = 1; COMMIT;")
   assert_eq(all.get("ok", false), true, "parse_all ok")
   assert_eq(all.get("value", []).len, 3, "parse_all statements")
   assert_eq(statement_kind("drop table if exists tmp"), "drop_table", "statement kind")
   print("✓ std.parse.data.sql self-test passed")
}
