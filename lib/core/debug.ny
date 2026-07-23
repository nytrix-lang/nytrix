;; Keywords: core debug diagnostics trace inspect
;; Core Debug for Nytrix
;; References:
;; - std.core
module std.core.debug(debug_print_val, debug_print, breakpoint, stack_trace, memory_dump, inspect)
use std.core
use std.core.io

fn debug_print_val(any val) any {
   "Prints a detailed debug representation of a single value."
   _print_write("Value(raw: ")
   _print_write(to_str(val))
   _print_write(", type: ")
   _print_write(type(val))
   if is_ptr(val) {
      _print_write(", addr: ")
      _print_write(to_str(val))
   }
   _print_write(")\n")
}

fn debug_print(...args) any {
   "Prints a detailed debug representation of one or more values."
   mut xs = args
   if args.len == 1 {
      def first = args.get(0)
      if is_list(first) { xs = first }
   }
   def n = xs.len
   mut i = 0
   while i < n {
      def v = xs.get(i)
      debug_print_val(v)
      i += 1
   }
}

fn breakpoint() any {
   "Triggers a debugger trap on supported architectures."
   __breakpoint()
}

fn stack_trace(int limit=32) list {
   "Returns up to `limit` runtime frames as [file, line, column, function]."
   if limit < 0 { return [] }
   __get_backtrace(limit)
}

fn memory_dump(any ptr, int count=64) list {
   "Returns up to `count` bytes read from `ptr` for debugger inspection."
   if count <= 0 { return [] }
   mut out = []
   mut i = 0
   while i < count {
      out = out.append(load8(ptr, i))
      i = i + 1
   }
   out
}

fn inspect(any value) dict {
   "Returns a stable, machine-readable summary for a runtime value."
   mut out = dict()
   out.set("type", type(value))
   out.set("repr", repr(value))
   out.set("is_ptr", is_ptr(value))
   out.set("is_int", is_int(value))
   out
}

#main {
   assert(is_list(stack_trace(4)), "stack_trace returns a list")
   def info = inspect(42)
   assert(info.get("type") == "int", "inspect type")
   assert(info.get("is_int"), "inspect integer")
   print("✓ std.core.debug self-test passed")
}
