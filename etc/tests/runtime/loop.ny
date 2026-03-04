use std.core *
use std.core.error *
use std.core.reflect *
use std.core.list *
use std.core.dict *
use std.str.io *
use std.str *

;; Loop strict syntax (Test)

mut i = 0
while(i < 3){
   i = i + 1
}
assert(i == 3, "while strict")

mut sum = 0
def list_vals = [1, 2, 3]
for(x in list_vals){
   sum = sum + x
}
assert(sum == 6, "for strict")

mut loop=0
while(mut i=0 i<10 ++i){
   loop = i
}
assert(loop == 9, "loop sequence failed")

print("✓ Loop strict syntax tests passed")
