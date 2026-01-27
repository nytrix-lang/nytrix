use std.io
use std.core

;; Lambda closures – capture semantics (Test)
;; NOTE: Nytrix currently captures by reference, not snapshot.

print("Testing basic capture (by reference)...")

def x = 10
def f_basic = lambda(y){ x + y }
assert(f_basic(20) == 30, "initial capture")

x = 20
; capture observes rebinding
assert(f_basic(20) == 40, "capture by reference")

print("Testing multiple captures...")

def a = 1
def b = 2
def c = 3
def f_multi = lambda(){ a + b + c }
assert(f_multi() == 6, "capture a,b,c")

print("Testing nested closures...")

def x2 = 10
def outer = lambda(y){
   lambda(z){ x2 + y + z }
}
def fn_inner = outer(20)
assert(fn_inner(30) == 60, "nested capture")

print("Testing escaping closure...")

def make_adder = lambda(n){
   lambda(x){ x + n }
}
def add5 = make_adder(5)
def add10 = make_adder(10)
assert(add5(10) == 15, "add5")
assert(add10(10) == 20, "add10")

print("Testing mutable object capture...")

def list_ref = list()
list_ref = append(list_ref, 1)
def add_to_list = lambda(v){
   append(list_ref, v)
   0
}
add_to_list(2)
assert(list_len(list_ref) == 2, "list len")
assert(get(list_ref, 1) == 2, "list val")

print("Testing capture shadowing...")

def x3 = 10
def f_shadow = lambda(x){ x * 2 }
assert(f_shadow(5) == 10, "shadow param")
assert(x3 == 10, "outer unchanged")

def higher_order_map = lambda(lst, f){
   def out = list()
   def i = 0
   def n = list_len(lst)
   while(i < n){
      out = append(out, f(get(lst, i)))
      i = i + 1
   }
   out
}

print("Testing lambda as argument...")

def l = [1,2,3]
def res = higher_order_map(l, lambda(x){ x * 10 })
assert(get(res, 0) == 10, "map 0")
assert(get(res, 1) == 20, "map 1")
assert(get(res, 2) == 30, "map 2")

print("✓ std.core.lambda_closure tests passed")
