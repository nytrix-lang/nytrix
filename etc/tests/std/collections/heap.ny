use std.io
use std.collections.heap
use std.core.error

;; collections.heap (Test)
;; Tests min-heap creation, push, pop, peek, and mixed operation consistency.

print("Testing heap basic...")
def h = heap()
h = heap_push(h, 50)
h = heap_push(h, 10)
assert(heap_peek(h) == 10, "Min 10")
h = heap_push(h, 30)
assert(heap_peek(h) == 10, "Min still 10")
assert(heap_pop(h) == 10, "Pop 10")
assert(heap_peek(h) == 30, "Min now 30")
assert(heap_pop(h) == 30, "Pop 30")
assert(heap_pop(h) == 50, "Pop 50")
assert(heap_pop(h) == 0, "Empty pop")

print("Testing heap mixed...")
def h2 = heap()
; Push 10..1
def i = 10
while(i > 0){
   h2 = heap_push(h2, i)
   i = i - 1
}
assert(heap_peek(h2) == 1, "Min 1")
; Pop 5 items (1, 2, 3, 4, 5)
i = 1
while(i <= 5){
   assert(heap_pop(h2) == i, "Popped in order")
   i = i + 1
}
; Push new randoms
h2 = heap_push(h2, 100)
h2 = heap_push(h2, 8)
assert(heap_peek(h2) == 6, "Min 6 (from 6..10 left)")
assert(heap_pop(h2) == 6, "Pop 6")
; Drain rest
; Expected: 7, 8, 8, 9, 10, 100
assert(heap_pop(h2) == 7, "Pop 7")
assert(heap_pop(h2) == 8, "Pop 8 (pushed)")
assert(heap_pop(h2) == 8, "Pop 8 (original)")
assert(heap_pop(h2) == 9, "Pop 9")
assert(heap_pop(h2) == 10, "Pop 10")
assert(heap_pop(h2) == 100, "Pop 100")

print("✓ std.collections.heap tests passed")
