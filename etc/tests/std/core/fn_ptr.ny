use std.io
use std
use std.core ; for itoa

fn add_impl(a, b) { return a + b }
fn sub_impl(a, b) { return a - b }

fn test_basic_ptr() {
   def f = add_impl
   assert(f(10, 20) == 30, "direct fn assign call")
   f = sub_impl
   assert(f(10, 20) == -10, "reassigned fn call")
}

fn test_lambda_ptr() {
   def l = lambda(x, y) { return x * y }
   assert(l(3, 4) == 12, "lambda call")
   def l2 = fn(x) { return x + 1 }
   assert(l2(10) == 11, "fn expr call")
}

fn apply_op(a, b, op) {
   print("Op: ", to_str(op))
   def res = op(a, b)
   print("Op res: ", to_str(res))
   return res
}

fn test_higher_order() {
   assert(apply_op(5, 3, add_impl) == 8, "passed fn ptr")
   assert(apply_op(5, 3, sub_impl) == 2, "passed fn ptr 2")
   assert(apply_op(5, 3, lambda(a, b){ return a * b }) == 15, "passed lambda")
}

fn test_main() {
   print("Running function pointer tests...")
   test_basic_ptr()
   test_lambda_ptr()
   test_higher_order()
   print("Function pointer tests passed")
}

test_main()
