;; Keywords: parser expression arithmetic grammar emitter bounded
;; Small arithmetic expression DSL built on the bounded parser combinators.
;; References:
;; - core
;; - core.parser
module core.math.parse.arithmetic(parse, emit)
use core
use core.parser as parser

fn _spaces() dict {
   parser.many(parser.byte_where("whitespace", fn(byte) {
            (byte >= 9 && byte <= 13) || (byte >= 32 && byte <= 32)
         }), 4096)
}

fn _token(any grammar) dict {
   parser.map(parser.sequence([_spaces(), grammar, _spaces()]),
      fn(values) any { values.get(1) })
}

fn _tail(any operator, any operand) dict {
   parser.map(parser.sequence([operator, operand]),
      fn(values) dict {
         {"operator":values.get(0), "operand":values.get(1)}
      })
}

fn _operator_kind(str operator) str {
   if operator == "+" { return "add" }
   if operator == "-" { return "subtract" }
   if operator == "*" { return "multiply" }
   if operator == "/" { return "divide" }
   ""
}

fn _fold_chain(any parts) dict {
   mut dict tree = parts.get(0)
   def tails = parts.get(1, [])
   if tails.len == 0 { return dict_clone(tree) }
   mut i = 0
   while i < tails.len {
      def tail = tails.get(i)
      tree = {"kind":_operator_kind(to_str(tail.get("operator"))),
         "left":tree, "right":tail.get("operand")}
      i += 1
   }
   tree
}

fn _chain(any first, any operators, any operand) dict {
   parser.map(parser.sequence([first, parser.many(_tail(operators, operand))]),
      fn(parts) dict { _fold_chain(parts) })
}

fn _grammar() dict {
   def expression = parser.forward()
   def number = parser.map(parser.integer(), fn(value) dict {
         {"kind":"integer", "value":int(value)}
      })
   def group = parser.map(parser.sequence([
            _token(parser.literal("(")), expression, _token(parser.literal(")"))
         ]), fn(values) any { values.get(1) })
   def atom = parser.choice([number, group])
   def multiply = parser.choice([
         _token(parser.literal("*")), _token(parser.literal("/"))
      ])
   def factor = _chain(atom, multiply, atom)
   def add = parser.choice([
         _token(parser.literal("+")), _token(parser.literal("-"))
      ])
   def sum = _chain(factor, add, factor)
   parser.resolve(expression, sum)
   _token(expression)
}

@effects(all)
fn parse(str source, int max_steps=100000, int max_depth=128,
   int max_bytes=1048576) dict {
   "Parses bounded integer arithmetic with precedence and parentheses."
   parser.run(_grammar(), source, max_steps, max_depth, max_bytes)
}

fn _emit_error(str message) dict {
   {"ok":false, "text":nil, "error":message}
}

fn _emit_node(any tree, int depth, int max_depth) dict {
   if depth > max_depth {
      return _emit_error("expression tree depth limit exceeded")
   }
   if !is_dict(tree) {
      return _emit_error("expression node must be a dict(got " + type(tree) + ")")
   }
   def kind = to_str(tree.get("kind", ""))
   if kind == "integer" {
      def value = tree.get("value")
      if !is_int(value) || type(value) == "bool" {
         return _emit_error("integer expression node has invalid value")
      }
      return {"ok":true, "text":to_str(value), "error":nil}
   }
   def operator = case kind {
      "add" -> "+"
      "subtract" -> "-"
      "multiply" -> "*"
      "divide" -> "/"
      _ -> ""
   }
   if operator == "" {
      return _emit_error("unknown arithmetic expression node kind")
   }
   def left = _emit_node(tree.get("left"), depth + 1, max_depth)
   if !left.get("ok", false) { return left }
   def right = _emit_node(tree.get("right"), depth + 1, max_depth)
   if !right.get("ok", false) { return right }
   {"ok":true,
      "text":"(" + to_str(left.get("text")) + " " + operator + " " +
      to_str(right.get("text")) + ")",
      "error":nil}
}

fn emit(any tree, int max_depth=128) dict {
   "Emits validated, canonical fully-parenthesized arithmetic text."
   _emit_node(tree, 0, max(max_depth, 0))
}
