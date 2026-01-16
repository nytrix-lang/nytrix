;;; debug.ny --- core debug module

;; Keywords: core debug

;;; Commentary:

;; Core Debug module.

use std.core
use std.core.reflect
use std.strings.str
module std.core.debug (
	debug_print_val, debug_print
)

fn debug_print_val(val){
	"Prints a detailed debug representation of a single value."
	_print_write("Value(raw: ")
	_print_write(itoa(val))
	_print_write(", type: ")
	_print_write(type(val))
	if(is_ptr(val)){
		_print_write(", addr: ")
		_print_write(itoa(val))
	}
	_print_write(")\n")
}

fn debug_print(...args){
	"Prints a detailed debug representation of one or more values."
	def xs = args
	if(len(args) == 1){
		def first = get(args, 0)
		if(eq(type(first), "list")){ xs = first }
	}
	def n = len(xs)
	def i = 0
	while(i < n){
		def v = get(xs, i)
		debug_print_val(v)
		i = i + 1
	}
}
