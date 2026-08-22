; Memory-tooling smoke: exercises mutable locals and a loop in the compiler
; process run under ASan, UBSan, TSan, Valgrind, and coverage CI jobs.
mut i = 0
mut sum = 0
while i < 5 {
   sum = sum + i
   i = i + 1
}

assert_eq(sum, 10, "loop reduction")
