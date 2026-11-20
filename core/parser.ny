;; Keywords: parser combinators recursive parse errors span bounded
;; Bounded parser combinators with structured byte-span diagnostics.
;; References:
;; - core
;; - core.str
module core.parser(pure, literal, byte_where, sequence, choice, map, many,
   optional, integer, eof, forward, resolve, run_prefix,
   run, parse)
use core
use core.str as str

def _SCHEMA = "nytrix.parser"
def _DEFAULT_STEPS = 100000
def _DEFAULT_DEPTH = 128
def _DEFAULT_BYTES = 1048576

fn _node(str kind) dict {
   mut p = dict(5)
   p["schema"] = _SCHEMA
   p["version"] = 1
   p["kind"] = kind
   p
}

fn _result(bool ok, any value, int offset) list {
   [ok ? 1 : 0, value, offset]
}

fn _is_parser(any parser) bool {
   if !is_dict(parser) { return false }
   return parser.get("schema", "") == _SCHEMA &&
   int(parser.get("version", 0)) == 1 &&
   is_str(parser.get("kind", ""))
}

fn pure(any value) dict {
   "Creates a parser that succeeds without consuming input."
   mut p = _node("pure")
   p["value"] = value
   p
}

fn literal(str text) dict {
   "Matches an exact byte string."
   mut p = _node("literal")
   p["text"] = text
   p
}

fn byte_where(str expected, fnptr predicate) dict {
   "Matches one byte when predicate(byte) returns true."
   mut p = _node("byte_where")
   p["expected"] = expected
   p["predicate"] = predicate
   p
}

fn sequence(list parsers) dict {
   "Runs parsers in order and returns their values as a list."
   mut p = _node("sequence")
   p["parsers"] = parsers
   p
}

fn choice(list parsers) dict {
   "Tries alternatives from the same input offset, left to right."
   mut p = _node("choice")
   p["parsers"] = parsers
   p
}

fn map(any parser, fnptr transform) dict {
   "Maps a successful parser value through transform(value)."
   mut p = _node("map")
   p["parser"] = parser
   p["transform"] = transform
   p
}

fn many(any parser, int max_items=100000) dict {
   "Repeats parser; zero-width success is rejected to prevent infinite loops."
   mut p = _node("many")
   p["parser"] = parser
   p["max_items"] = max_items
   p
}

fn optional(any parser) dict {
   "Tries parser and returns nil without consuming input when it fails."
   mut p = _node("optional")
   p["parser"] = parser
   p
}

fn integer() dict {
   "Parses a checked signed base-10 integer; overflow is a structured error."
   _node("integer")
}

fn eof() dict {
   "Succeeds only at the end of the input."
   _node("eof")
}

fn forward() dict {
   "Creates a one-time forward reference for a recursive grammar."
   mut p = _node("forward")
   p["target"] = nil
   p
}

fn resolve(any reference, any parser) bool {
   "Sets a forward reference once; unresolved references fail when run."
   if !_is_parser(reference) || reference.get("kind", "") != "forward" ||
   !_is_parser(parser) || !is_nil(reference.get("target")) {
      return false
   }
   reference["target"] = parser
   true
}

fn _copy_expected(list values) list {
   mut copied = []
   mut i = 0
   while i < values.len {
      copied = copied.append(values.get(i))
      i += 1
   }
   copied
}

fn _failure_state(dict state, int offset, str expected) any {
   def farthest = int(state.get("farthest", -1))
   if offset > farthest {
      state["farthest"] = offset
      state["expected"] = [expected]
   } else if offset == farthest {
      def current = state.get("expected", [])
      if !current.contains(expected) {
         state["expected"] = current.append(expected)
      }
   }
   0
}

fn _fatal(dict state, int offset, str message) any {
   if state.get("fatal", "") == "" {
      state["fatal"] = message
      state["fatal_offset"] = offset
   }
   0
}

@effects(all)
fn _eval(any parser, str source, int offset, dict state, int depth) list {
   def steps = int(state.get("steps", 0)) + 1
   state["steps"] = steps
   if steps > int(state.get("max_steps", _DEFAULT_STEPS)) {
      _fatal(state, offset, "parser step limit exceeded")
      return _result(false, nil, offset)
   }
   if depth > int(state.get("max_depth", _DEFAULT_DEPTH)) {
      _fatal(state, offset, "parser nesting limit exceeded")
      return _result(false, nil, offset)
   }
   if !_is_parser(parser) {
      _fatal(state, offset, "invalid parser node or unsupported schema version")
      return _result(false, nil, offset)
   }
   def kind = str(parser.get("kind", ""))
   if kind == "pure" {
      return _result(true, parser.get("value"), offset)
   }
   if kind == "literal" {
      def text = parser.get("text", "")
      if !is_str(text) {
         _fatal(state, offset, "literal parser has invalid text")
         return _result(false, nil, offset)
      }
      mut i = 0
      while i < text.len {
         def at = offset + i
         if at >= source.len || load8(source, at) != load8(text, i) {
            _failure_state(state, at, "'" + text + "'")
            return _result(false, nil, offset)
         }
         i += 1
      }
      return _result(true, text, offset + text.len)
   }
   if kind == "byte_where" {
      def expected = str(parser.get("expected", "byte predicate"))
      def predicate = parser.get("predicate")
      if type(predicate) != "ptr" && type(predicate) != "fnptr" {
         _fatal(state, offset,
            "byte predicate is not callable(type " + type(predicate) + ")")
         return _result(false, nil, offset)
      }
      if offset >= source.len {
         _failure_state(state, offset, expected)
         return _result(false, nil, offset)
      }
      def byte = load8(source, offset)
      if predicate(byte) {
         return _result(true, byte, offset + 1)
      }
      _failure_state(state, offset, expected)
      return _result(false, nil, offset)
   }
   if kind == "integer" {
      mut cursor = offset
      mut negative = false
      if cursor < source.len &&
      (load8(source, cursor) == 45 || load8(source, cursor) == 43) {
         negative = load8(source, cursor) == 45
         cursor += 1
      }
      def digit_start = cursor
      mut value = 0
      while cursor < source.len {
         def byte = load8(source, cursor)
         if byte < 48 || byte > 57 { break }
         def digit = byte - 48
         if negative {
            ;; Accumulate negatively so the signed minimum remains representable.
            if value < -461168601842738790 ||
            (value == -461168601842738790 && digit > 4) {
               _fatal(state, cursor, "signed decimal integer overflow")
               return _result(false, nil, offset)
            }
            value = value * 10 - digit
         } else {
            if value > 461168601842738790 ||
            (value == 461168601842738790 && digit > 3) {
               _fatal(state, cursor, "signed decimal integer overflow")
               return _result(false, nil, offset)
            }
            value = value * 10 + digit
         }
         cursor += 1
      }
      if cursor == digit_start {
         _failure_state(state, cursor, "signed decimal integer")
         return _result(false, nil, offset)
      }
      return _result(true, value, cursor)
   }
   if kind == "eof" {
      if offset == source.len { return _result(true, true, offset) }
      _failure_state(state, offset, "end of input")
      return _result(false, nil, offset)
   }
   if kind == "forward" {
      def target = parser.get("target")
      if !_is_parser(target) {
         _fatal(state, offset, "unresolved recursive parser reference")
         return _result(false, nil, offset)
      }
      return _eval(target, source, offset, state, depth + 1)
   }
   if kind == "map" {
      def inner = _eval(parser.get("parser"), source, offset, state, depth + 1)
      if inner.get(0, 0) == 0 { return inner }
      def transform = parser.get("transform")
      if type(transform) != "ptr" && type(transform) != "fnptr" {
         _fatal(state, offset, "map transform is not callable")
         return _result(false, nil, offset)
      }
      return _result(true, transform(inner.get(1)),
         int(inner.get(2, offset)))
   }
   if kind == "sequence" || kind == "choice" {
      def children = parser.get("parsers", [])
      if !is_list(children) {
         _fatal(state, offset, "combinator children must be a list")
         return _result(false, nil, offset)
      }
      if kind == "choice" {
         def base_farthest = int(state.get("farthest", -1))
         def base_expected = _copy_expected(state.get("expected", []))
         mut best_farthest = base_farthest
         mut best_expected = base_expected
         mut i = 0
         while i < children.len {
            def alternative = _eval(children.get(i), source, offset, state,
               depth + 1)
            if alternative.get(0, 0) != 0 {
               ;; Failed alternatives are speculative once a later branch wins.
               state["farthest"] = base_farthest
               state["expected"] = base_expected
               return alternative
            }
            if state.get("fatal", "") != "" { return alternative }
            def branch_farthest = int(state.get("farthest", -1))
            def branch_expected = state.get("expected", [])
            if branch_farthest > best_farthest {
               best_farthest = branch_farthest
               best_expected = _copy_expected(branch_expected)
            } elif branch_farthest == best_farthest {
               mut merged = _copy_expected(best_expected)
               mut j = 0
               while j < branch_expected.len {
                  def expected = branch_expected.get(j)
                  if !merged.contains(expected) {
                     merged = merged.append(expected)
                  }
                  j += 1
               }
               best_expected = merged
            }
            ;; Each alternative starts with the same diagnostic baseline.
            state["farthest"] = base_farthest
            state["expected"] = base_expected
            i += 1
         }
         state["farthest"] = best_farthest
         state["expected"] = best_expected
         if best_expected.len == 0 {
            _failure_state(state, offset, "one of the alternatives")
         }
         return _result(false, nil, offset)
      }
      mut values = []
      mut cursor = offset
      mut i = 0
      while i < children.len {
         def part = _eval(children.get(i), source, cursor, state, depth + 1)
         if part.get(0, 0) == 0 { return part }
         if state.get("fatal", "") != "" { return part }
         values = values.append(part.get(1))
         cursor = int(part.get(2, cursor))
         i += 1
      }
      return _result(true, values, cursor)
   }
   if kind == "optional" {
      def base_farthest = int(state.get("farthest", -1))
      def base_expected = _copy_expected(state.get("expected", []))
      def inner = _eval(parser.get("parser"), source, offset, state, depth + 1)
      if inner.get(0, 0) != 0 { return inner }
      if state.get("fatal", "") != "" { return inner }
      state["farthest"] = base_farthest
      state["expected"] = base_expected
      return _result(true, nil, offset)
   }
   if kind == "many" {
      def max_items = int(parser.get("max_items", 100000))
      if max_items < 0 {
         _fatal(state, offset, "many max_items cannot be negative")
         return _result(false, nil, offset)
      }
      mut values = []
      if max_items == 0 { return _result(true, values, offset) }
      mut cursor = offset
      while true {
         def base_farthest = int(state.get("farthest", -1))
         def base_expected = _copy_expected(state.get("expected", []))
         def item = _eval(parser.get("parser"), source, cursor, state,
            depth + 1)
         if item.get(0, 0) == 0 {
            if state.get("fatal", "") != "" { return item }
            state["farthest"] = base_farthest
            state["expected"] = base_expected
            return _result(true, values, cursor)
         }
         def next = int(item.get(2, cursor))
         if next <= cursor {
            _fatal(state, cursor,
               "many parser succeeded without consuming input")
            return _result(false, nil, cursor)
         }
         if values.len >= max_items {
            _fatal(state, cursor, "many item limit exceeded")
            return _result(false, nil, cursor)
         }
         values = values.append(item.get(1))
         cursor = next
      }
   }
   _fatal(state, offset, "unknown parser node kind: " + kind)
   _result(false, nil, offset)
}

fn _line_column(str source, int offset) list {
   mut line = 1
   mut column = 1
   mut i = 0
   def end = min(max(offset, 0), source.len)
   while i < end {
      if load8(source, i) == 10 {
         line += 1
         column = 1
      } else {
         column += 1
      }
      i += 1
   }
   [line, column]
}

fn _error(str source, dict state, int fallback_offset) dict {
   def fatal = str(state.get("fatal", ""))
   mut offset = fallback_offset
   if fatal != "" {
      offset = int(state.get("fatal_offset", fallback_offset))
   } else {
      offset = int(state.get("farthest", fallback_offset))
   }
   def lc = _line_column(source, offset)
   def expected = state.get("expected", [])
   mut message = fatal
   if message == "" {
      if expected.len > 0 {
         message = "expected " + str.join(expected, " or ")
      } else {
         message = "input did not match"
      }
   }
   mut span = dict(2)
   span["start"] = offset
   span["end"] = offset
   mut error = dict(7)
   error["code"] = "E_PARSE"
   error["message"] = message
   error["span"] = span
   error["line"] = lc.get(0)
   error["column"] = lc.get(1)
   error["expected"] = expected
   error["found_byte"] = offset < source.len ? load8(source, offset) : -1
   error
}

@effects(all)
fn _run_with(any parser, str source, int max_steps, int max_depth,
   int max_bytes) dict {
   mut state = dict(8)
   state["steps"] = 0
   state["max_steps"] = max(max_steps, 1)
   state["max_depth"] = max(max_depth, 1)
   state["farthest"] = -1
   state["expected"] = []
   state["fatal"] = ""
   state["fatal_offset"] = 0
   if source.len > max_bytes {
      _fatal(state, 0, "parser input byte limit exceeded")
      return {"ok":false, "value":nil, "offset":0,
         "error":_error(source, state, 0)}
   }
   mut result = _eval(parser, source, 0, state, 0)
   if result.get(0, 0) == 0 || state.get("fatal", "") != "" {
      def error = _error(source, state, int(result.get(2, 0)))
      return {"ok":false, "value":nil, "offset":error.get("span").get("start"),
         "error":error}
   }
   {"ok":true, "value":result.get(1),
      "offset":result.get(2), "error":nil}
}

@effects(all)
fn run_prefix(any parser, str source, int max_steps=100000,
   int max_depth=128, int max_bytes=1048576) dict {
   "Runs a bounded parser and permits unconsumed suffix input."
   _run_with(parser, source, max_steps, max_depth, max_bytes)
}

@effects(all)
fn run(any parser, str source, int max_steps=100000,
   int max_depth=128, int max_bytes=1048576) dict {
   "Runs a bounded parser and requires it to consume the complete input."
   def checked = sequence([parser, eof()])
   mut result = _run_with(checked, source, max_steps, max_depth, max_bytes)
   if !result.get("ok", false) { return result }
   def pair = result.get("value", [])
   result["value"] = pair.get(0)
   result
}

@effects(all)
fn parse(any parser, str source, int max_steps=100000,
   int max_depth=128, int max_bytes=1048576) any {
   "Returns the parsed value, or nil when parsing fails. Use run for errors."
   def result = run(parser, source, max_steps, max_depth, max_bytes)
   if result.get("ok", false) { return result.get("value") }
   nil
}
