use std.io
use std.collections.heap
use std.core.error

fn test_heap_basic(){
	print("Testing heap basic...")
	def h = heap()
	h = hpush(h, 50)
	h = hpush(h, 10)
	assert(hpeek(h) == 10, "Min 10")
	h = hpush(h, 30)
	assert(hpeek(h) == 10, "Min still 10")
	assert(hpop(h) == 10, "Pop 10")
	assert(hpeek(h) == 30, "Min now 30")
	assert(hpop(h) == 30, "Pop 30")
	assert(hpop(h) == 50, "Pop 50")
	assert(hpop(h) == 0, "Empty pop")
	print("Basic heap passed")
}

fn test_heap_mixed(){
	print("Testing heap mixed...")
	def h = heap()
	; Push 10..1
	def i = 10
	while(i > 0){
		h = hpush(h, i)
		i = i - 1
	}
	assert(hpeek(h) == 1, "Min 1")
	; Pop 5 items (1, 2, 3, 4, 5)
	i = 1
	while(i <= 5){
		assert(hpop(h) == i, "Popped in order")
		i = i + 1
	}
	; Push new randoms
	h = hpush(h, 100)
	h = hpush(h, 8)
	assert(hpeek(h) == 6, "Min 6 (from 6..10 left)")
	assert(hpop(h) == 6, "Pop 6")
	; Drain rest
	; Expected: 7, 8, 8, 9, 10, 100
	assert(hpop(h) == 7, "Pop 7")
	assert(hpop(h) == 8, "Pop 8 (pushed)")
	assert(hpop(h) == 8, "Pop 8 (original)") ; Or vice versa, order of duplicates undef but values same
	assert(hpop(h) == 9, "Pop 9")
	assert(hpop(h) == 10, "Pop 10")
	assert(hpop(h) == 100, "Pop 100")
	print("Mixed heap passed")
}

fn test_main(){
	test_heap_basic()
	test_heap_mixed()
	print("✓ std.collections.heap passed")
}

test_main()
