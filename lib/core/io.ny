;; Keywords: io input output stream core
;; Core input/output operations for files, streams, and process pipes.
;; References:
;; - std.core
module std.core.io(_print_write, print)
use std.core
use std.core.primitives as prim

fn _write_str(str s) int {
   def n = load64(s, -16)
   if n > 0 { __write_off(1, s, n, 0) }
   0
}

fn _print_join3(str a, str b, str c) str { __str_concat(__str_concat(a, b), c) }

@returns_owned
fn _print_list_repr(any v, str open, str close) str {
   def n = load64(v, 0)
   use std.core.str
   mut b = Builder(64)
   b = builder_append(b, open)
   mut i = 0
   while i < n {
      def item = load64(v, 16 + i * 8)
      b = builder_append(b, _print_to_str(item))
      if i + 1 < n { b = builder_append(b, ", ") }
      i += 1
   }
   b = builder_append(b, close)
   def s = builder_to_str(b)
   builder_free(b)
   s
}

fn _print_bytes_repr(any v) str {
   def n = load64(v, -16)
   _print_join3("<bytes ", __to_str(n), ">")
}

fn _print_to_str(any v) str {
   if v == true { return "true" }
   if v == false { return "false" }
   if __is_int(v) { return __to_str(v) }
   if !v { return "none" }
   if is_str(v) { return v }
   if __is_ny_obj(v) {
      def big_tag = prim.runtime_tag_raw("bigint")
      def got_tag = __tagof(v)
      if got_tag == big_tag || got_tag == __tag(big_tag) { return __bigint_to_str(v) }
      if is_list(v) { return _print_list_repr(v, "[", "]") }
      if is_tuple(v) { return _print_list_repr(v, "(", ")") }
      if is_dict(v) || is_set(v) { return to_str(v) }
      if is_bytes(v) { return _print_bytes_repr(v) }
      return __to_str(v)
   }
   return __to_str(v)
}

fn _print_write(any v) int {
   def s = _print_to_str(v)
   _write_str(s)
}

fn print(...args) int {
   "Prints values with optional keyword args `sep` and `end`."
   mut sep = " "
   mut end = "\n"
   def n = load64(args, 0)
   mut vals = 0
   mut i = 0
   while i < n {
      def arg = load64(args, 16 + i * 8)
      if is_kwargs(arg) {
         def k, v = get_kwarg_key(arg), get_kwarg_val(arg)
         if k == "sep" { sep = is_str(v) ? v : __to_str(v) } else if k == "end" { end = is_str(v) ? v : __to_str(v) }
      } else {
         vals += 1
      }
      i += 1
   }
   i = 0
   mut seen = 0
   while i < n {
      def arg = load64(args, 16 + i * 8)
      if !is_kwargs(arg) {
         _print_write(arg)
         seen += 1
         if seen < vals { _write_str(sep) }
      }
      i += 1
   }
   _write_str(end)
   0
}

#main {
   assert(_print_to_str(true) == "true" && _print_to_str(false) == "false", "io bool text")
   assert(_print_to_str([1, "x"]) == "[1, x]", "io list text")
   assert(_write_str("") == 0 && _print_write("") == 0, "io empty writes")
   print("✓ std.core.io self-test passed")
}
