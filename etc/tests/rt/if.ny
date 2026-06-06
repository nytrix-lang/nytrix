use std.core
use std.core.error
use std.core.reflect
use std.core.dict
use std.core.io
use std.core.str

;; If strict syntax (Test)
fn early_return(x) {
   if(x < 0){ return 0 }
   if(x == 10){
      return 20
   } else {
      return x * 2
   }
}

fn non_tail_if_expr_return(bool flag) dict {
   mut xs = [0]
   if(flag){ xs[0] = 7 }
   {"status": "done", "xs": xs}
}

fn require_text_after_panic(?str candidate) str {
   if(candidate == nil){ panic("missing text") }
   def str: out = candidate
   out
}

assert(early_return(-1) == 0, "if return")
assert(early_return(10) == 20, "if else return")
assert(early_return(5) == 10, "fallthrough")

if(1){ print("if true ok") }
if(0){
   panic("if false taken")
} else {
   print("if false else ok")
}

mut val = 0

if(1){
   val = 1
   val = 2
}

assert(val == 2, "block exec")

if(0){
   val = 10
}

val = 20
assert(val == 20, "skip block")
def binding_if_expr = if(true){ 41 } else { 0 }
assert(binding_if_expr == 41, "binding-level if expression returns branch value")
def nested_if_expr = if(false){ 0 } else { if(true){ 9 } else { 3 } }
assert(nested_if_expr == 9, "nested if expression returns branch value")

;; Compile-time #if syntax/selection checks
mut os_tag = "unknown"
#linux { os_tag = "linux" }
#elif macos { os_tag = "macos" }
#elif windows { os_tag = "windows" }
#endif
assert(os_tag == __os_name(), "compile-time #if os selection")

fn compile_if_arch_bits() {
   #x86_64 { return 64 }
   #elif aarch64 { return 64 }
   #else { return 32 }
   #endif
}

def bits = compile_if_arch_bits()
assert(bits == 64 || bits == 32, "compile-time #if arch selection")
def non_tail = non_tail_if_expr_return(true)
assert(is_dict(non_tail), "non-tail if expression must not become an implicit return")
assert(non_tail.get("status") == "done", "final expression remains the return value")
assert(non_tail.get("xs").get(0) == 7, "non-tail branch side effect still runs")
assert(require_text_after_panic("ok") == "ok", "panic branch narrows nullable values")
print("✓ if strict syntax tests passed")
