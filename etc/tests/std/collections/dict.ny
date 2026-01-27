use std.io
use std.collections.dict
use std.core.error
use std.core

;; Dict Collections (Test)
;; Tests dictionary basic operations, resizing, methods, copying, updates, and stress cycles.

print("Testing dict basic operations...")
def d = dict()
assert(load64(d) == 0, "Initial count 0")
d = dict_set(d, "key1", 100)
assert(load64(d) == 1, "Count 1 after insert")
assert(dict_get(d, "key1", 0) == 100, "Get existing key")
assert(dict_get(d, "missing", 999) == 999, "Get missing key returns default")
assert(contains(d, "key1"), "Has existing key")
assert(!contains(d, "missing"), "Has missing key")
d = dict_set(d, "key1", 200)
assert(dict_get(d, "key1", 0) == 200, "Update existing key")
assert(load64(d) == 1, "Count remains 1 after update")
d = dict_set(d, "key2", 300)
assert(load64(d) == 2, "Count 2 after second insert")
assert(dict_get(d, "key2", 0) == 300, "Get second key")
d = dict_del(d, "key1")
assert(load64(d) == 1, "Count 1 after delete")
assert(!contains(d, "key1"), "Deleted key gone")
assert(contains(d, "key2"), "Other key remains")

print("Testing dict resizing...")
def d2 = dict(8)
def i = 0
while(i < 50){
   d2 = dict_set(d2, i, i * 10)
   i = i + 1
}
assert(load64(d2) == 50, "Count correct after many inserts")
i = 0
while(i < 50){
   assert(dict_get(d2, i, -1) == i * 10, "Get value after resize")
   i = i + 1
}

print("Testing dict methods (keys, values, items)...")
def d3 = dict()
d3 = dict_set(d3, "a", 1)
d3 = dict_set(d3, "b", 2)
def k = keys(d3)
assert(list_len(k) == 2, "keys length")
def v = values(d3)
assert(list_len(v) == 2, "values length")
def it = items(d3)
assert(list_len(it) == 2, "items length")

print("Testing dict copy and update...")
def d4 = dict()
dict_set(d4, "a", 1)
def d5 = dict_copy(d4)
assert(dict_get(d5, "a", 0) == 1, "Copy has item")
dict_set(d5, "b", 2)
assert(!contains(d4, "b"), "Original unmodified by copy modification")
def d6 = dict()
dict_set(d6, "c", 3)
dict_update(d4, d6)
assert(dict_get(d4, "c", 0) == 3, "Update from dict")

; Update from list of pairs
def pairs = [["d", 4], ["e", 5]]
dict_update(d4, pairs)
assert(dict_get(d4, "d", 0) == 4, "Update from list pair 1")
assert(dict_get(d4, "e", 0) == 5, "Update from list pair 2")

print("Testing mixed key types...")
def d7 = dict()
d7 = dict_set(d7, 123, "int")
d7 = dict_set(d7, "123", "str")
assert(dict_get(d7, 123, 0) == "int", "Int key")
assert(dict_get(d7, "123", 0) == "str", "Str key")
assert(123 != "123", "Keys are different")

print("Testing stress cycle (add/del)...")
def d8 = dict()
; Add 100 items
i = 0
while(i < 100){
   d8 = dict_set(d8, i, i)
   i = i + 1
}
assert(load64(d8) == 100, "100 items added")
; Delete evens
i = 0
while(i < 100){
   if(i % 2 == 0){
      d8 = dict_del(d8, i)
   }
   i = i + 1
}
assert(load64(d8) == 50, "50 items remaining")
; Check odds present, evens missing
i = 0
while(i < 100){
    if(i % 2 != 0){
       assert(contains(d8, i), "Odd key present")
    } else {
       assert(!contains(d8, i), "Even key removed")
    }
    i = i + 1
}
; Re-add some deleted
dict_set(d8, 0, 999)
assert(contains(d8, 0), "Re-added key 0")
assert(dict_get(d8, 0, -1) == 999, "Re-added value correct")

print("Testing dict_clear...")
def d9 = dict()
dict_set(d9, "a", 1)
dict_set(d9, "b", 2)
assert(load64(d9) == 2, "Items present")
dict_clear(d9)
assert(load64(d9) == 0, "Count 0 after clear")
assert(!contains(d9, "a"), "Item a gone")
dict_set(d9, "c", 3)
assert(load64(d9) == 1, "Can insert after clear")

print("✓ std.collections.dict tests passed")
