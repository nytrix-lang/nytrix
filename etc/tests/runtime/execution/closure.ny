use std.core

fn make_counter() {
   mut n = 0
   def bump = fn() {
      n += 1
      n
   }
   bump
}

def c = make_counter()
assert(c() == 1, "mutable closure first call")
assert(c() == 2, "mutable closure second call")
def typed_fn = fn(int x) int { x + 1 }
def typed_lambda = lambda(int x) int { x + 1 }
assert(typed_fn(41) == 42, "typed fn expression callable")
assert(typed_lambda(41) == 42, "typed lambda expression callable")

fn make_adder(int n) fnptr {
   def add = fn(int x) int { x + n }
   add
}

fn call_once(fnptr f, int x) int {
   f(x)
}

def add5 = make_adder(5)
assert(add5(3) == 8, "explicit fnptr return preserves captured closure")
assert(call_once(add5, 4) == 9, "user fnptr parameter preserves captured closure")

fn make_counter_typed(int start=0) fnptr {
   mut n = start
   def bump = fn() {
      n += 1
      n
   }
   bump
}

def ct = make_counter_typed(10)
assert(ct() == 11, "explicit fnptr return preserves mutable closure first call")
assert(ct() == 12, "explicit fnptr return preserves mutable closure second call")

fn typed_named_add(int a, int b) int { a + b }
def named_fn_value = typed_named_add
assert(type(named_fn_value) == "ptr", "typed named function value has pointer runtime type")
assert(named_fn_value(20, 22) == 42, "typed named function value callable")
assert(type(range(1, 4)) == "range", "range tag is distinct from callable closure tag")
