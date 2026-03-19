;; expect: unknown field 'missing' in layout 'BadLayout'
use std.core

layout BadLayout {
   i32: x
}

def ptr: p = malloc(__layout_size("BadLayout"))
load_layout(p, "BadLayout", "missing")
