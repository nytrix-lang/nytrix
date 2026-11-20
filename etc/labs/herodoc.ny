#!/usr/bin/env ny
;; Keywords: overview comptime pattern matching collections
;; A compile-time opcode decoded through ordinary collection code.
layout Instr pack(4) { i32 op, i32 arg }
def int base = comptime{0x1000}
def int step = sizeof(Instr)
def list<int> code=[0x12,0x24,0x42,0x9f,0x00]
def list<str> trace=code.map(fn(i32 op) str match op {
  0x00 -> "halt"
  0x10..0x1f -> "load"
  0x20..0x2f -> "math"
  0x80..0xff -> "sys "
  _ -> "data"
})
for i in 0..(trace.len-1) {
  print(f"{int(base) + int(i) * int(step)}: {trace[i]=} {code[i]=}")
}
