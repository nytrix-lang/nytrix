use std.io
use std.collections.queue
use std.core.error

fn test_queue_basic(){
	print("Testing queue basic...")
	def q = queue()
	assert(queue_size(q) == 0, "size 0")
	q = enqueue(q, 1)
	q = enqueue(q, 2)
	assert(queue_size(q) == 2, "size 2")
	assert(dequeue(q) == 1, "dequeue 1")
	assert(dequeue(q) == 2, "dequeue 2")
	assert(queue_size(q) == 0, "size 0")
	print("Basic queue passed")
}

fn test_queue_stress(){
	print("Testing queue stress...")
	def q = queue()
	def n = 100
	def i = 0
	; Enqueue 100
	while(i < n){
		q = enqueue(q, i)
		i = i + 1
	}
	assert(queue_size(q) == n, "Size 100")
	; Dequeue 100
	i = 0
	while(i < n){
		assert(dequeue(q) == i, "Dequeued correct val")
		i = i + 1
	}
	assert(queue_size(q) == 0, "Empty after stress")
	print("Stress queue passed")
}

fn test_main(){
	test_queue_basic()
	test_queue_stress()
	print("✓ std.collections.queue passed")
}

test_main()
