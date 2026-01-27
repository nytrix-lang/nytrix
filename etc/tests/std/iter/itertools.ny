use std.io
use std.iter.itertools
use std.iter
use std.core.error

;; std.iter + std.iter.itertools (Test)

print("Testing iter + itertools")

def a = [1,2]
def b = ["a","b"]
def p = product(a, b)
assert(list_len(p) == 4, "product len")
assert(get(get(p,0),0) == 1 && get(get(p,0),1) == "a", "p0")
assert(get(get(p,1),0) == 1 && get(get(p,1),1) == "b", "p1")
assert(get(get(p,2),0) == 2 && get(get(p,2),1) == "a", "p2")
assert(get(get(p,3),0) == 2 && get(get(p,3),1) == "b", "p3")

assert(sum([1,2,3,4]) == 10, "sum")

def e = enumerate(["x","y"])
assert(get(get(e,0),0) == 0 && get(get(e,0),1) == "x", "enumerate0")
assert(get(get(e,1),0) == 1 && get(get(e,1),1) == "y", "enumerate1")

def z = zip([1,2,3],[4,5])
assert(get(get(z,0),0) == 1 && get(get(z,0),1) == 4, "zip0")
assert(get(get(z,1),0) == 2 && get(get(z,1),1) == 5, "zip1")

def inc = lambda(x){ x + 1 }
def dbl = lambda(x){ x * 2 }
assert(compose(dbl, inc, 3) == 8, "compose")
assert(iter_pipe(3, [inc, dbl, inc]) == 9, "iter_pipe")

def r = range(5)
assert(list_len(r) == 5, "range len")
assert(get(r,0) == 0, "r0")
assert(get(r,4) == 4, "r4")

def r2 = range(2,5)
assert(get(r2,0) == 2, "range2_0")
assert(get(r2,2) == 4, "range2_2")

def r3 = range(0,10,2)
assert(get(r3,1) == 2, "range3_1")

def r4 = range(5,0,-1)
assert(get(r4,0) == 5, "range4_0")
assert(get(r4,4) == 1, "range4_4")

def m = map(r, fn(x){ x * 2 })
assert(get(m,0) == 0, "map0")
assert(get(m,1) == 2, "map1")

def f = filter(r, fn(x){ x % 2 == 0 })
assert(get(f,0) == 0, "filter0")
assert(get(f,1) == 2, "filter1")

def acc = 0
def i = 0
while(i < 5){
   acc = acc + get([1,2,3,4,5], i)
   i = i + 1
}
assert(acc == 15, "manual reduce")

print("✓ all iter tests passed")
