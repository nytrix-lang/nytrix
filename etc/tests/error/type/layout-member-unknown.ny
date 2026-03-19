;; expect: unknown member 'y' for layout 'HMPair'
use std.core

layout HMPair {
   i32: x
}

def *HMPair: p = nil
print(p.y)
