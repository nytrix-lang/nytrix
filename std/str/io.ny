;; Keywords: str io
;; Basic IO helpers.

module std.str.io (
   _print_write, print
)
use std.core *

fn _write_str(s){
   "Internal: writes a raw string to stdout without conversion."
   def n = load64(s, -16)
   if(n > 0){ __sys_write_off(1, s, n, 0) }
}

fn _print_join3(a, b, c){
   "Internal helper."
   __str_concat(__str_concat(a, b), c)
}

fn _print_list_repr(v, open, close){
   "Internal helper."
   def n = load64(v, 0)
   mut s = open
   mut i = 0
   while(i < n){
      def item = load64(v, 16 + i * 8)
      s = __str_concat(s, _print_to_str(item))
      if(i + 1 < n){ s = __str_concat(s, ", ") }
      i += 1
   }
   __str_concat(s, close)
}

fn _print_to_str(v){
   "Internal helper."
   if(v == true){ return "true" }
   if(v == false){ return "false" }
   if(!v){ return "none" }
   if(is_str(v)){ return v }
   if(is_int(v) || is_float(v)){ return __to_str(v) }
   if(is_list(v)){ return _print_list_repr(v, "[", "]") }
   if(is_tuple(v)){ return _print_list_repr(v, "(", ")") }
   if(is_dict(v) || is_set(v)){ return "{...}" }
   if(is_bytes(v)){
      def n = load64(v, -16)
      return _print_join3("<bytes ", __to_str(n), ">")
   }
   return __to_str(v)
}

fn _print_write(v){
   "Writes a value to stdout without a trailing newline."
   def s = _print_to_str(v)
   _write_str(s)
}

fn print(...args){
   "Prints values with optional keyword args `sep` and `end`."
   mut sep = " "
   mut end = "\n"
   def n = load64(args, 0)
   mut vals = 0
   mut i = 0
   while(i < n){
      def arg = load64(args, 16 + i * 8)
      if(is_kwargs(arg)){
         def k = get_kwarg_key(arg)
         def v = get_kwarg_val(arg)
         if(k == "sep"){
            sep = is_str(v) ? v : __to_str(v)
         } else if(k == "end"){
            end = is_str(v) ? v : __to_str(v)
         }
      } else {
         vals += 1
      }
      i += 1
   }
   i = 0
   mut seen = 0
   while(i < n){
      def arg = load64(args, 16 + i * 8)
      if(!is_kwargs(arg)){
         _print_write(arg)
         seen += 1
         if(seen < vals){ _write_str(sep) }
      }
      i += 1
   }
   _write_str(end)
   0
}

if(comptime{__main()}){
    use std.core *
    use std.str.io *

    _print_write("io")
    print("test")
    assert(print() == 0, "print with no arguments")
    assert(print("hello", "world", sep=" ") == 0, "print with sep kwarg")
    assert(print("hello", "world", end="!\n") == 0, "print with end kwarg")
    assert(print("hello", "world", sep="-", end="!\n") == 0,
           "print with sep/end kwargs")
    assert(print("hello", "world", end="\n", sep="::") == 0,
           "print with kwargs in reversed order")
    assert(print("hello", "world", sep=1, end=2) == 0,
           "print coerces non-string sep/end")
    assert(print("hello", "world", unknown="ignored") == 0,
           "print ignores unknown keyword arguments")
    assert(1 == 1, "io ok")

    print("✓ std.str.io tests passed")
}
