use std.io
use std.collections.queue
use std.core.error

;; collections.queue (Test)
;; Tests FIFO queue operations, length tracking, and stress performance.

print("Testing queue basic...")
def q = queue()
assert(queue_len(q) == 0, "size 0")
q = queue_push(q, 1)
q = queue_push(q, 2)
assert(queue_len(q) == 2, "size 2")
assert(queue_pop(q) == 1, "dequeue 1")
assert(queue_pop(q) == 2, "dequeue 2")
assert(queue_len(q) == 0, "size 0")

print("Testing queue stress...")
def q2 = queue()
def n = 100
def i = 0
; Enqueue 100
while(i < n){
   q2 = queue_push(q2, i)
   i = i + 1
}
assert(queue_len(q2) == n, "Size 100")
; Dequeue 100
i = 0
while(i < n){
   assert(queue_pop(q2) == i, "Dequeued correct val")
   i = i + 1
}
assert(queue_len(q2) == 0, "Empty after stress")

print("✓ std.collections.queue tests passed")
