#!/bin/ny
;; Rule 110 (Example) - https://en.wikipedia.org/wiki/Rule_110

use std.core *
use std.core.mem *
use std.os.sys *
use std.str.bytes *

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
mut tw = get(t, 0, 0)
if(tw <= 0){ tw = 80 }
if(tw < 1){ tw = 1 }

def SCALE_X = 2
def SCALE_Y = 1

mut w = tw / SCALE_X
if(w < 1){ w = 1 }

mut u = bytes(w)
mut i = 0
while (i < w) {
   bytes_set(u, i, 0)
   i += 1
}
bytes_set(u, 0, 1)

mut h = get(t, 1, 0)
if(h <= 0){ h = 24 }

def max_gen = (h - 2) / SCALE_Y
mut gen = 0
while (gen < max_gen) {
   def out = bytes(w * SCALE_X * 3 + 1)
   mut o = 0
   mut i = 0
   while(i < w){
      def v = bytes_get(u, i)
      if(v){
         mut k = 0
         while(k < SCALE_X){
            bytes_set(out, o,   load8("\xe2\x96\x88", 0))
            bytes_set(out, o+1, load8("\xe2\x96\x88", 1))
            bytes_set(out, o+2, load8("\xe2\x96\x88", 2))
            o += 3
            k += 1
         }
      } else {
         mut k = 0
         while(k < SCALE_X){
            bytes_set(out, o, 32)
            o += 1
            k += 1
         }
      }
      i += 1
   }
   bytes_set(out, o, 10)
   o += 1
   mut ry = 0
   while (ry < SCALE_Y) {
      unwrap(sys_write(1, out, o))
      ry += 1
   }
   free(out)
   def next = step(u, w)
   free(u)
   u = next
   gen += 1
}
free(u)
