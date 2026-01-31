#!/bin/ny
;; Rule 110 (Example) - https://en.wikipedia.org/wiki/Rule_110

use std.core *
use std.core.mem *
use std.os.sys *
use std.str *
use std.str.bytes *
use std.str.term *

fn rule110(l, c, r) {
   (c ^ l) | (c & !r)
}

fn step(curr, w) {
   def next = bytes(w)
   mut i = 0
   while(i < w){
      def l = (i > 0) ? bytes_get(curr, i-1) : 0
      def c = bytes_get(curr, i)
      def r = (i < w-1) ? bytes_get(curr, i+1) : 0
      bytes_set(next, i, rule110(l==1, c==1, r==1) ? 1 : 0)
      i += 1
   }
   next
}

def t = get_terminal_size()
mut w = get(t, 0, 0)
if(w <= 0){ w = 80 }
w -= 1
mut u = bytes(w)
mut i = 0 
while (i < w) { bytes_set(u, i, 0) i+=1 }
bytes_set(u, 0, 1)

mut h = get(t, 1, 0)
if(h <= 0){ h = 24 }
mut gen = 0
while (gen < h - 2) {
   def out = bytes(w + 1)
   mut i = 0
   while(i < w){
      def v = bytes_get(u, i)
      bytes_set(out, i, v ? 35 : 32)
      i += 1
   }
   bytes_set(out, w, 10)
   unwrap(sys_write(1, out, w + 1))
   free(out)
   def next = step(u, w)
   free(u)
   u = next
   gen += 1
}
free(u)
