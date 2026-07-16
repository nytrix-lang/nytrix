#!/usr/bin/env ny

;; Keywords: native ffi raw-memory pointer example
;; Store vertices in raw memory and call C sqrt through a header import.
use std.core
use std.math (float)

#include <math.h> as "c"
def STRIDE = 12

fn near(any a, any b) bool {
   def d = float(a) - float(b)
   d > -0.0001 && d < 0.0001
}

fn store_vertex(any ptr, int idx, f64 x, f64 y, f64 z) {
   def at = idx * STRIDE
   store32_f32(ptr, x, at + 0)
   store32_f32(ptr, y, at + 4)
   store32_f32(ptr, z, at + 8)
}

fn read_vertex(any ptr, int idx) list {
   def p = ptr_add(ptr, idx * STRIDE)
   [load32_f32(p, 0), load32_f32(p, 4), load32_f32(p, 8)]
}

def vertices = own(malloc(STRIDE * 2))
memset(vertices, 0, STRIDE * 2)
store_vertex(vertices, 0, 3.0, 4.0, 0.0)
store_vertex(vertices, 1, 1.0, 2.0, 2.0)
def a = read_vertex(vertices, 0)
def b = read_vertex(vertices, 1)
def len_a = c.sqrt(float(a.get(0) * a.get(0) + a.get(1) * a.get(1)))
def b_ptr = vertices + STRIDE
assert(near(len_a, 5.0), "C sqrt through direct ABI")
assert(near(load32_f32(b_ptr, 8), 2.0), "raw pointer arithmetic")
assert(b == [1.0, 2.0, 2.0], "raw memory round trip")
print("a:", a, "length:", len_a)
print("b:", b)
free(vertices)
