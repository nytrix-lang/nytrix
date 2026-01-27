use std.core
use std.io
use std.core.error

;; Loop strict syntax (Test)

def i = 0
while(i < 3){
   i = i + 1
}
assert(i == 3, "while strict")

def sum = 0
def list_vals = [1, 2, 3]
for(x in list_vals){
   sum = sum + x
}
assert(sum == 6, "for strict")

print("✓ Loop strict syntax tests passed")
