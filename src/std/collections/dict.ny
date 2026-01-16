;;; dict.ny --- collections dict module

;; Keywords: collections dict

;;; Commentary:

;; Collections Dict module.

use std.core
use std.core.reflect
module std.collections.dict (
	dict, dict_resize, dict_set, dict_get, dict_has, dict_del, dict_items, dict_keys, dict_values, dict_clear,
	dict_copy, dict_update, setitem, getitem, has, delitem, items, keys, values
)

; define DICT_MAGIC     = 101
; define STATE_EMPTY    = 0
; define STATE_OCCUPIED = 1
; define STATE_DELETED  = 2
def ENTRY_SIZE     = 24
def HEADER_SIZE    = 16

fn dict(cap=8){
	if(cap < 8){ cap = 8 }
	def c = 8
	while(c < cap){ c = c * 2 }
	def d = rt_malloc(16 + c * 24)
	store64(d, 101, -8)
	store64(d, 0, 0)
	store64(d, c, 8)
	def i = 0
	while(i < c){ store64(d, 0, 16 + i * 24 + 16)  i = i + 1 }
	d
}

fn dict_resize(d, new_cap){
	def old_cap = load64(d, 8)
	def new_d = dict(new_cap)
	def i = 0
	while(i < old_cap){
		def off = 16 + i * 24
		if(load64(d, off + 16) == 1){
			new_d = dict_set(new_d, load64(d, off), load64(d, off + 8))
		}
		i = i + 1
	}
	new_d
}

fn dict_set(d, key, val){
	def count = load64(d, 0)
	def cap = load64(d, 8)
	if(count * 2 >= cap){
		d = dict_resize(d, cap * 2)
		count = load64(d, 0)
		cap = load64(d, 8)
	}
	def h = hash(key)  def mask = cap - 1  def idx = h & mask  def perturb = h  def probes = 0
	def first_free = -1
	while(probes < cap){
		def off = 16 + idx * 24
		def st = load64(d, off + 16)
		if(st == 0){
			if(first_free != -1){ off = first_free }
			store64(d, key, off)
			store64(d, val, off + 8)
			store64(d, 1, off + 16)
			store64(d, count + 1, 0)
			return d
		}
		if(st == 2){ if(first_free == -1){ first_free = off } }
		if(st == 1){
			if(eq(load64(d, off), key)){ store64(d, val, off + 8)  return d }
		}
		idx = (idx * 5 + 1 + (perturb >> 5)) & mask
		perturb = perturb >> 5
		probes = probes + 1
	}
	if(first_free != -1){
		 def off = first_free
		 store64(d, key, off)
		 store64(d, val, off + 8)
		 store64(d, 1, off + 16)
		 store64(d, count + 1, 0)
	}
	d
}
fn setitem(d, key, val){ dict_set(d, key, val) }

fn dict_get(d, key, default_val=0){
	def cap = load64(d, 8)  def h = hash(key)  def mask = cap - 1  def idx = h & mask  def perturb = h  def probes = 0
	while(probes < cap){
		def off = 16 + idx * 24
		def st = load64(d, off + 16)
		if(st == 0){ return default_val }
		if(st == 1){
			if(eq(load64(d, off), key)){ return load64(d, off + 8) }
		}
		idx = (idx * 5 + 1 + (perturb >> 5)) & mask
		perturb = perturb >> 5
		probes = probes + 1
	}
	default_val
}
fn getitem(d, key, default_val=0){ dict_get(d, key, default_val) }

fn dict_has(d, key){
	def cap = load64(d, 8)  def h = hash(key)  def mask = cap - 1  def idx = h & mask  def perturb = h  def probes = 0
	while(probes < cap){
		def off = 16 + idx * 24
		def st = load64(d, off + 16)
		if(st == 0){ return false }
		if(st == 1){ if(eq(load64(d, off), key)){ return true } }
		idx = (idx * 5 + 1 + (perturb >> 5)) & mask
		perturb = perturb >> 5
		probes = probes + 1
	}
	false
}
fn has(d, key){ dict_has(d, key) }

fn dict_del(d, key){
	def cap = load64(d, 8)  def h = hash(key)  def mask = cap - 1  def idx = h & mask  def perturb = h  def probes = 0
	while(probes < cap){
		def off = 16 + idx * 24
		def st = load64(d, off + 16)
		if(st == 0){ return d }
		if(st == 1){
			if(eq(load64(d, off), key)){
				store64(d, 2, off + 16)
				store64(d, load64(d, 0) - 1, 0)
				return d
			}
		}
		idx = (idx * 5 + 1 + (perturb >> 5)) & mask
		perturb = perturb >> 5
		probes = probes + 1
	}
	d
}
fn delitem(d, key){ dict_del(d, key) }

fn dict_items(d){
	def res = list(8)  def cap = load64(d, 8)  def i = 0
	while(i < cap){
		def off = 16 + i * 24
		if(load64(d, off + 16) == 1){ res = append(res, [load64(d, off), load64(d, off + 8)]) }
		i = i + 1
	}
	res
}
fn items(d){ dict_items(d) }

fn dict_keys(d){
	def res = list(8)  def cap = load64(d, 8)  def i = 0
	while(i < cap){
		def off = 16 + i * 24
		if(load64(d, off + 16) == 1){ res = append(res, load64(d, off)) }
		i = i + 1
	}
	res
}
fn keys(d){ dict_keys(d) }

fn dict_values(d){
	def res = list(8)  def cap = load64(d, 8)  def i = 0
	while(i < cap){
		def off = 16 + i * 24
		if(load64(d, off + 16) == 1){ res = append(res, load64(d, off + 8)) }
		i = i + 1
	}
	res
}
fn values(d){ dict_values(d) }

fn dict_clear(d){
  "Removes all items from dictionary d."
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

fn dict_update(d, other){
	if(is_dict(other)){
		def its = items(other)  def i = 0  n = list_len(its)
		while(i < n){ def p = get(its, i)  dict_set(d, p[0], p[1])  i = i + 1 }
	} else {
		def i = 0  n = list_len(other)
		while(i < n){ def p = get(other, i)  dict_set(d, p[0], p[1])  i = i + 1 }
	}
	d
}
