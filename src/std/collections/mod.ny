;;; mod.ny --- collections mod module

;; Keywords: collections mod

;;; Commentary:

;; Collections Mod module.

use std.core
use std.core.reflect
use std.collections.dict
use std.collections.set
use std.collections.queue
use std.collections.heap
module std.collections (
	list, list_len, list_get, list_set, list_push, list_pop, list_extend, list_clear,
	list_clone, list_reverse, list_sort, list_sorted, list_has,
	dict, dict_len, dict_get, dict_set, dict_has, dict_del, dict_items,
	dict_keys, dict_values, dict_clear, dict_copy, dict_clone, dict_update,
	set, set_len, set_has, set_add, set_remove, set_clear, set_copy, set_clone, set_union,
	set_intersection, set_difference,
	queue, queue_push, queue_pop, queue_len,
	heap, heap_push, heap_pop, heap_peek,
	col_len, col_get, col_set, col_has, col_clear,
	col_clone, col_items, col_keys, col_values,
	add, remove, contains, items, keys, values, has
)

; Unified Collections API

; define QUEUE_TAG = 106

; list from std.core
; list_len from std.core
; list_get/set etc from std.core

fn list_get(lst, i){
	"Return item at index i in list lst."
	return get(lst, i)
}

fn list_set(lst, i, v){
	"Set item at index i in list lst to v."
	return set_idx(lst, i, v)
}

fn list_push(lst, v){
	"Append v to list lst and return the list."
	return append(lst, v)
}

fn list_pop(lst){
	"Remove and return the last item in list lst."
	return pop(lst)
}

fn list_extend(lst, other){
	"Append all items from other into lst."
	return extend(lst, other)
}

fn list_clear(lst){
	"Remove all items from list lst."
	if(is_ptr(lst)){
		store64(lst, 0, 0)
	}
	return lst
}

fn list_clone(lst){
	"Return a shallow copy of list lst."
	if(lst == 0){ return 0 }
	if(is_list(lst) == false){ return 0 }
	def n = list_len(lst)
	def out = list(n)
	def i = 0
	while(i < n){
		def val = get(lst, i)
		out = append(out, val)
		i = i + 1
	}
	return out
}

fn list_reverse(lst){
	"Return a new list with items in reverse order."
	def n = list_len(lst)
	def out = list(8)
	def i = n - 1
	while(i >= 0){
		out = append(out, get(lst, i))
		i = i - 1
	}
	return out
}

fn _list_partition(xs, low, high){
	"Internal: partition list for quicksort, return pivot index."
	def pivot = get(xs, high)
	def i = low - 1
	def j = low
	while(j < high){
		if(get(xs, j) <= pivot){
			i = i + 1
			def tmp = get(xs, i)
			store_item(xs, i, get(xs, j))
			store_item(xs, j, tmp)
		}
		j = j + 1
	}
	def tmp2 = get(xs, i + 1)
	store_item(xs, i + 1, get(xs, high))
	store_item(xs, high, tmp2)
	return i + 1
}

fn _list_quicksort(xs, low, high){
	"Internal: in-place quicksort over [low, high]."
	if(low < high){
		def p = _list_partition(xs, low, high)
		_list_quicksort(xs, low, p - 1)
		_list_quicksort(xs, p + 1, high)
	}
	return xs
}

fn list_sort(lst){
	"Sort list lst in place using QuickSort."
	def n = list_len(lst)
	if(n < 2){ return lst }
	return _list_quicksort(lst, 0, n - 1)
}

fn list_sorted(lst){
	"Return a new sorted list containing the items of lst."
	def out = list_clone(lst)
	return list_sort(out)
}

fn list_has(lst, x){
	"Return true if list lst contains item x."
	def i = 0  def n = list_len(lst)
	while(i < n){
		if(eq(get(lst, i), x)){ return true }
		i = i + 1
	}
	return false
}

; dict from std.core
fn dict_len(d){ len(d) }

fn dict_get(d, key, default=0){
	"Return d[key] or default if missing."
	return getitem(d, key, default)
}

fn dict_set(d, key, val){
	"Set d[key] to val."
	return setitem(d, key, val)
}

fn dict_has(d, key){
	"Return true if key exists in dictionary d."
	return has(d, key)
}

fn dict_del(d, key){
	"Delete key from dictionary d."
	return delitem(d, key)
}

fn dict_items(d){
	"Return list of [key, value] pairs."
	return items(d)
}

fn dict_keys(d){
	"Return list of keys in d."
	return keys(d)
}

fn dict_values(d){
	"Return list of values in d."
	return values(d)
}

fn dict_clear(d){
	"Remove all entries from dictionary d."
	def cap = load64(d, 8)
	def i = 0
	while(i < cap){
		store64(d, 0, 16 + i * 24 + 16)
		i = i + 1
	}
	store64(d, 0, 0)
	return d
}

fn dict_copy(d){
	"Return a shallow copy of dictionary d."
	def cap = load64(d, 8)
	def out = dict(cap)
	def i = 0
	while(i < cap){
		def off = 16 + i * 24
		if(load64(d, off + 16) == 1){
			out = setitem(out, load64(d, off), load64(d, off + 8))
		}
		i = i + 1
	}
	return out
}

fn dict_clone(d){
	"Return a shallow copy of dictionary d."
	return dict_copy(d)
}

fn dict_update(d, other){
	"Update dictionary d with entries from other."
	case type(other) {
		"dict" -> {
			def its = items(other)
			def i = 0
			def n = list_len(its)
			while(i < n){
				def p = get(its, i)
				d = setitem(d, p[0], p[1])
				i = i + 1
			}
		}
		_ -> {
			def i = 0
			def n = list_len(other)
			while(i < n){
				def p = get(other, i)
				d = setitem(d, p[0], p[1])
				i = i + 1
			}
		}
	}
	return d
}

; set from std.collections.set
fn set_len(s){ len(s) }

fn set_has(s, v){
	"Return true if v is in set s."
	return set_contains(s, v)
}

fn set_add(s, v){
	"Add v to set s."
	return add(s, v)
}

fn set_remove(s, v){
	"Remove v from set s."
	return remove(s, v)
}

fn set_clear(s){
	"Remove all items from set s."
	return dict_clear(s)
}

fn set_copy(s){
	"Return a shallow copy of set s."
	def out = set()
	def its = items(s)
	def i = 0 def n = list_len(its)
	while(i < n){
		def p = get(its, i)
		out = add(out, p[0])
		i = i + 1
	}
	return out
}

fn set_clone(s){
	"Return a shallow copy of set s."
	return set_copy(s)
}

fn set_union(a, b){
	"Return the union of sets a and b."
	def out = set_copy(a)
	def its = items(b)
	def i = 0 def n = list_len(its)
	while(i < n){
		def p = get(its, i)
		out = add(out, p[0])
		i = i + 1
	}
	return out
}

fn set_intersection(a, b){
	"Return the intersection of sets a and b."
	def out = set()
	def its = items(a)
	def i = 0 def n = list_len(its)
	while(i < n){
		def p = get(its, i)
		def v = p[0]
		if(set_contains(b, v)){
			out = add(out, v)
		}
		i = i + 1
	}
	return out
}

fn set_difference(a, b){
	"Return the difference of sets a and b."
	def out = set()
	def its = items(a)
	def i = 0 def n = list_len(its)
	while(i < n){
		def p = get(its, i)
		def v = p[0]
		if(set_contains(b, v) == false){
			out = add(out, v)
		}
		i = i + 1
	}
	return out
}

; queue from std.collections.queue
fn queue_len(q){
	if(!is_ptr(q) || load64(q, -8) != 106){ 0 } else { load64(q, 0) }
}

; queue_push etc from queue module

; heap from std.collections.heap

fn heap_push(h, v){
	"Push v into min-heap h."
	return hpush(h, v)
}

fn heap_pop(h){
	"Pop and return smallest value from min-heap h."
	return hpop(h)
}

fn heap_peek(h){
	"Return smallest value from min-heap h without popping."
	return hpeek(h)
}

; Generic collection helpers (list/dict/set/tuple/str)

fn col_len(x){ len(x) }

fn col_get(x, i){
	"Return element at index/key i for lists/tuples/dicts/strings."
	return get(x, i)
}

fn col_set(x, i, v){
	"Set element at index/key i for lists/dicts. Returns 0 for unsupported types."
	return case type(x) {
	  "dict" -> setitem(x, i, v)
	  "list" -> set_idx(x, i, v)
	  _      -> 0
	}
}

fn col_has(x, item){
	"Return true if item is contained in x."
	if(!x){ return false }
	return case type(x) {
		"set"  -> set_contains(x, item)
		"dict" -> dict_has(x, item)
		"list" -> list_has(x, item)
		"str"  -> {
			use std.strings.str
			find(x, item) >= 0
		}
		_      -> false
	}
}

fn col_clear(x){
	"Clear collection x in place (list/dict/set)."
	return case type(x) {
	  "list" -> list_clear(x)
	  "dict" -> dict_clear(x)
	  "set"  -> set_clear(x)
	  _      -> x
	}
}

fn col_clone(x){
	"Return a shallow clone of list/dict/set, or x for other types."
	return case type(x) {
	  "list" -> list_clone(x)
	  "dict" -> dict_copy(x)
	  "set"  -> set_copy(x)
	  _      -> x
	}
}

fn col_items(x){
	"Return items for dict/set/list/tuple/str."
	return case type(x) {
		"dict" -> items(x)
		"set"  -> {
			def its = items(x)
			def out = list(8)
			def i = 0  def n = list_len(its)
			while(i < n){
				out = append(out, get(get(its, i), 0))
				i = i + 1
			}
			out
		}
		"list", "tuple", "str" -> {
			def out = list(8)
			def n = len(x)
			def i = 0
			while(i < n){
				def pair = list(2)
				pair = append(pair, i)
				pair = append(pair, get(x, i))
				out = append(out, pair)
				i = i + 1
			}
			out
		}
		_ -> list(0)
	}
}

fn col_keys(x){
	"Return keys for dict/set/list/tuple/str."
	return case type(x) {
		"dict" -> keys(x)
		"set"  -> col_items(x)
		"list", "tuple", "str" -> {
			def out = list(8)
			def n = len(x)
			def i = 0
			while(i < n){ out = append(out, i)  i = i + 1 }
			out
		}
		_ -> list(0)
	}
}

fn col_values(x){
	return case type(x) {
		"dict" -> values(x)
		"set"  -> col_items(x)
		"list", "tuple", "str" -> {
			def out = list(8)
			def n = len(x)
			def i = 0
			while(i < n){ out = append(out, get(x, i))  i = i + 1 }
			out
		}
		_ -> list(0)
	}
}

; Generic Aliases
fn add(s, v){ set_add(s, v) }
fn remove(s, v){ set_remove(s, v) }
fn contains(x, v){ col_has(x, v) }
fn has(x, k){ col_has(x, k) }
fn items(x){ col_items(x) }
fn keys(x){ col_keys(x) }
fn values(x){ col_values(x) }
