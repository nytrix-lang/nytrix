use std.io
use std.collections.dict
use std.core.error
use std.core ; for is_dict

fn test_basic(){
	print("Testing dict basic operations...")
	def d = dict()
	assert(load64(d) == 0, "Initial count 0")
	d = setitem(d, "key1", 100)
	assert(load64(d) == 1, "Count 1 after insert")
	assert(getitem(d, "key1", 0) == 100, "Get existing key")
	assert(getitem(d, "missing", 999) == 999, "Get missing key returns default")
	assert(has(d, "key1"), "Has existing key")
	assert(!has(d, "missing"), "Has missing key")
	d = setitem(d, "key1", 200)
	assert(getitem(d, "key1", 0) == 200, "Update existing key")
	assert(load64(d) == 1, "Count remains 1 after update")
	d = setitem(d, "key2", 300)
	assert(load64(d) == 2, "Count 2 after second insert")
	assert(getitem(d, "key2", 0) == 300, "Get second key")
	d = delitem(d, "key1")
	assert(load64(d) == 1, "Count 1 after delete")
	assert(!has(d, "key1"), "Deleted key gone")
	assert(has(d, "key2"), "Other key remains")
	print("Basic operations passed")
}

fn test_resize(){
	print("Testing dict resizing...")
	def d = dict(8)
	def i = 0
	while(i < 50){
		d = setitem(d, i, i * 10)
		i = i + 1
	}
	assert(load64(d) == 50, "Count correct after many inserts")
	i = 0
	while(i < 50){
		assert(getitem(d, i, -1) == i * 10, "Get value after resize")
		i = i + 1
	}
	print("Resize passed")
}

fn test_methods(){
	print("Testing dict methods (keys, values, items)...")
	def d = dict()
	d = setitem(d, "a", 1)
	d = setitem(d, "b", 2)
	def k = keys(d)
	assert(list_len(k) == 2, "keys length")
	def v = values(d)
	assert(list_len(v) == 2, "values length")
	def it = items(d)
	assert(list_len(it) == 2, "items length")
	print("Methods passed")
}

fn test_copy_update(){
	print("Testing dict copy and update...")
	def d1 = dict()
	setitem(d1, "a", 1)
	def d2 = dict_copy(d1)
	assert(getitem(d2, "a", 0) == 1, "Copy has item")
	setitem(d2, "b", 2)
	assert(!has(d1, "b"), "Original unmodified by copy modification")
	def d3 = dict()
	setitem(d3, "c", 3)
	dict_update(d1, d3)
	assert(getitem(d1, "c", 0) == 3, "Update from dict")
	; Update from list of pairs
	def pairs = [["d", 4], ["e", 5]]
	dict_update(d1, pairs)
	assert(getitem(d1, "d", 0) == 4, "Update from list pair 1")
	assert(getitem(d1, "e", 0) == 5, "Update from list pair 2")
	print("Copy/Update passed")
}

fn test_mixed_types(){
	print("Testing mixed key types...")
	def d = dict()
	d = setitem(d, 123, "int")
	d = setitem(d, "123", "str")
	assert(getitem(d, 123, 0) == "int", "Int key")
	assert(getitem(d, "123", 0) == "str", "Str key")
	assert(123 != "123", "Keys are different")
	print("Mixed types passed")
}

fn test_stress_cycle(){
	print("Testing stress cycle (add/del)...")
	def d = dict()
	; Add 100 items
	def i = 0
	while(i < 100){
		d = setitem(d, i, i)
		i = i + 1
	}
	assert(load64(d) == 100, "100 items added")
	; Delete evens
	i = 0
	while(i < 100){
		if(i % 2 == 0){
			d = delitem(d, i)
		}
		i = i + 1
	}
	assert(load64(d) == 50, "50 items remaining")
	; Check odds present, evens missing
	i = 0
	while(i < 100){
		 if(i % 2 != 0){
			 assert(has(d, i), "Odd key present")
		 } else {
			 assert(!has(d, i), "Even key removed")
		 }
		 i = i + 1
	}
	; Re-add some deleted
	setitem(d, 0, 999)
	assert(has(d, 0), "Re-added key 0")
	assert(getitem(d, 0, -1) == 999, "Re-added value correct")
	print("Stress cycle passed")
}

fn test_clear(){
	print("Testing dict_clear...")
	def d = dict()
	setitem(d, "a", 1)
	setitem(d, "b", 2)
	assert(load64(d) == 2, "Items present")
	dict_clear(d)
	assert(load64(d) == 0, "Count 0 after clear")
	assert(!has(d, "a"), "Item a gone")
	setitem(d, "c", 3)
	assert(load64(d) == 1, "Can insert after clear")
	print("Clear passed")
}

fn test_main(){
	test_basic()
	test_resize()
	test_methods()
	test_copy_update()
	test_mixed_types()
	test_stress_cycle()
	test_clear()
	print("✓ std.collections.dict passed")
}

test_main()
