;;; set.ny --- collections set module

;; Keywords: collections set

;;; Commentary:

;; Collections Set module.

use std.core
use std.core.reflect
use std.collections.dict ; for dict(16), setitem(), has(), delitem(), dict_clear(), items()
module std.collections.set (
	set, set_add, set_contains, set_remove, set_clear, set_copy, set_clone, set_union, set_intersection,
	set_difference, add, remove
)

fn set(){
	def d = dict(16)
	store64(d, 102, -8)
	d
}

fn set_add(s, v){ return setitem(s, v, 1) }
fn add(s, v){ return setitem(s, v, 1) }

fn set_contains(s, v){ !!has(s, v) }

fn set_remove(s, v){ delitem(s, v) }
fn remove(s, v){ delitem(s, v) }

fn set_clear(s){ dict_clear(s) }
fn set_clone(s){ set_copy(s) }
fn set_copy(s){
	def out = set()  def its = items(s)
	def i = 0  def n = list_len(its)
	while(i < n){ out = set_add(out, get(get(its, i), 0))  i = i + 1 }
	out
}

fn set_union(a, b){
	def out = set_copy(a)  def its = items(b)
	def i = 0  def n = list_len(its)
	while(i < n){ out = set_add(out, get(get(its, i), 0))  i = i + 1 }
	out
}

fn set_intersection(a, b){
	def out = set()  def its = items(a)
	def i = 0  def n = list_len(its)
	while(i < n){
		def v = get(get(its, i), 0)
		if(set_contains(b, v)){ out = set_add(out, v) }
		i = i + 1
	}
	out
}

fn set_difference(a, b){
	def out = set()  def its = items(a)
	def i = 0  def n = list_len(its)
	while(i < n){
		def v = get(get(its, i), 0)
		if(set_contains(b, v) == false){ out = set_add(out, v) }
		i = i + 1
	}
	out
}
