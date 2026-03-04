use std.core *

fn double(x){ x * 2 }

; Pipeline
def x = 10 |> double() |> double()
assert(x == 40, "pipeline double")

def list = [100, 200, 300]
def first = list |> [0]
assert(first == 100, "pipeline index")

def obj = {"val": 500}
def v = obj |> .val
assert(v == 500, "pipeline member")

; Nil-coalesce
def n = nil
assert((n ?? 123) == 123, "nil ?? fallback")
assert((456 ?? 123) == 456, "value ?? fallback")

; Optional chaining
def o = {"a": 777}
def o_nil = nil
assert(o?.a == 777, "optional chaining value")
assert(o_nil?.a == nil, "optional chaining nil")

; if-def
if(def a = 100 a == 100){
   assert(a == 100, "if-def scope")
} else {
   assert(false, "if-def failed")
}

; while-def
mut j = 0
mut seen = false
while(def k = 10 j < k){
   seen = true
   assert(k == 10, "while-def binding")
   j = j + 5
}
assert(seen == true, "while-def entered")
assert(j == 10, "while-def result")

print("✓ all pipeline/option tests passed")
