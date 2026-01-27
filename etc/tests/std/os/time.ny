use std.os.time *
use std.math.timefmt *
use std.core.error *

;; std.math.time (Test)
;; Tests time, ticks, sleep, and time formatting.

print("Testing Math Time...")

def t1 = time()
assert(t1 > 0, "time > 0")

def start = ticks()
sleep(1)
def end = ticks()

if(start > 0 && end > 0){
 if(end < start){
  print("Warning: ticks went backwards")
 }
} else {
 print("Warning: ticks unavailable")
}

ticks()

def fmt = format_time(0)
assert(eq(fmt, "1970-01-01 00:00:00"), "epoch")

def fmt2 = format_time(1672531200)
assert(eq(fmt2, "2023-01-01 00:00:00"), "2023")

print("✓ std.math.time tests passed")
