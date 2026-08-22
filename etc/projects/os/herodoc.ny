#!/usr/bin/env ny

;; Keywords: overview comptime table pattern matching collections
;; A compile-time opcode table decoded through ordinary collection code.
comptime table Opcode {
   0x00 -> "halt"
   0x10..0x1f -> "load"
   0x20..0x2f -> "math"
   0x80..0xff -> "system"
   _ -> "data"
}
fn decode(i32 op) str = comptime match Opcode(op, "data")
def trace = [0x12, 0x24, 0x42, 0x9f, 0x00].map(decode)
def known = trace.filter(fn(name){ name != "data" }).reduce(0, fn(n){ n+1 })
print(f"{trace=} known={known}/{trace.len}")
