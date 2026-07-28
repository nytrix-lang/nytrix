use std.core
use std.core.error

def s = "baba" * 2
def r1 = match s {
   "babababa" -> "ok"
   _ -> "fail"
}

assert_eq(r1, "ok", "string repeat")
def l = [1, 2] * 2
def r2 = match l {
   [1, 2, 1, 2] -> "ok"
   _ -> "fail"
}

assert_eq(r2, "ok", "list repeat ints")
def ll = ["baba", "baba"] * 2
def r3 = match ll {
   ["baba", "baba", "baba", "baba"] -> "ok"
   _ -> "fail"
}

assert_eq(r3, "ok", "list repeat strings")
def z1 = "baba" * 0
def r4 = match z1 {
   "" -> "ok"
   _ -> "fail"
}

assert_eq(r4, "ok", "string repeat zero")
def z2 = [1,2] * 0
def r5 = match z2 {
   [] -> "ok"
   _ -> "fail"
}

assert_eq(r5, "ok", "list repeat zero")
print("✓ sequence repetition tests passed")
