#!/usr/bin/env ny

;; Keywords: overview comptime pattern matching collections
;; A compile-time opcode decoded through ordinary collection code.
;; NOTE: the [W2101] lint on the trace[i]/code[i] reads is expected today:
;; element types are not yet inferred through list.map, so those reads stay
;; on the boxed dynamic path (values are still correct).  Run with
;; NYTRIX_TYPE_FALLBACK_WARN=0 for fully silent output.
layout Instr pack(4) { i32 op, i32 arg }
def base,step=comptime{0x1000},sizeof(Instr)
def code=[0x12,0x24,0x42,0x9f,0x00]
def trace=code.map(fn(i32 op) str match op {
  0x00 -> "halt"
  0x10..0x1f -> "load"
  0x20..0x2f -> "math"
  0x80..0xff -> "sys "
  _ -> "data"
})
for i in 0..(trace.len-1) {
  print(f"{base+i*step}: {trace[i]=} {code[i]=}")
}
