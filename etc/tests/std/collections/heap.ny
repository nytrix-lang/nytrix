use std.io
use std.collections.heap
use std.core.error

fn test_heap_basic(){
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
   print("Basic heap passed")
}

fn test_heap_mixed(){
   print("Testing heap mixed...")
   def h = heap()
   ; Push 10..1
   def i = 10
   while(i > 0){
      h = heap_push(h, i)
      i = i - 1
   }
   assert(heap_peek(h) == 1, "Min 1")
   ; Pop 5 items (1, 2, 3, 4, 5)
   i = 1
   while(i <= 5){
      assert(heap_pop(h) == i, "Popped in order")
      i = i + 1
   }
   ; Push new randoms
   h = heap_push(h, 100)
   h = heap_push(h, 8)
   assert(heap_peek(h) == 6, "Min 6 (from 6..10 left)")
   assert(heap_pop(h) == 6, "Pop 6")
   ; Drain rest
   ; Expected: 7, 8, 8, 9, 10, 100
   assert(heap_pop(h) == 7, "Pop 7")
   assert(heap_pop(h) == 8, "Pop 8 (pushed)")
   assert(heap_pop(h) == 8, "Pop 8 (original)") ; Or vice versa, order of duplicates undef but values same
   assert(heap_pop(h) == 9, "Pop 9")
   assert(heap_pop(h) == 10, "Pop 10")
   assert(heap_pop(h) == 100, "Pop 100")
   print("Mixed heap passed")
}

fn test_main(){
   test_heap_basic()
   test_heap_mixed()
   print("✓ std.collections.heap passed")
}

test_main()
