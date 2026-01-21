use std.io
use std.os.time
; Benchmark: List operations
def start = ticks()

lst = []
i = 0
while(i < 1000){
   lst = append(lst, i)
   i = i + 1
}

assert(len(lst) == 1000, "list size")

sum = 0
for(x in lst){
   sum = sum + x
}

def end = ticks()

expected = 499500  ; Sum of 0..999
assert(sum == expected, "list sum")
print("List benchmark passed, sum =", sum)
print("Time (ns): ", end - start)
