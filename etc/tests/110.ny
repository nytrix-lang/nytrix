#!/bin/ny
;; Rule 110 (Example) - https://en.wikipedia.org/wiki/Rule_110

use std.core *
use std.core.error *
use std.os.args *
use std.os.sys *
use std.str *
use std.str.bytes *

;; Constants
def CHAR_FULL = "\xe2\x96\x88"
def CHAR_WS   = " "
def SX        = 2
def SY        = 1

fn rule(l, c, r){
   (c ^ l) | (c & !r)
}

fn step(cur, w){
   def nxt = bytes(w)
   mut i = 0
   while(i < w){
      mut l = 0
      if(i > 0){ l = bytes_get(cur, i - 1) }
      def c = bytes_get(cur, i)
      mut r = 0
      if(i < w - 1){ r = bytes_get(cur, i + 1) }
      def v = rule(l == 1, c == 1, r == 1)
      if(v){ bytes_set(nxt, i, 1) } else { bytes_set(nxt, i, 0) }
      i += 1
   }
   nxt
}

;; Main Line
def tSize = get_terminal_size()
mut tW = get(tSize, 0, 0)
if(tW <= 0){ tW = 80 }

mut w = tW / SX
if(w < 1){ w = 1 }

mut u = bytes(w)
bytes_set(u, 0, 1)

mut tH = get(tSize, 1, 0)
if(tH <= 0){ tH = 24 }
mut max_gen = (tH - 2) / SY
def av = args()
if(len(av) > 1){
   def g = atoi(get(av, 1, ""))
   if(g > 0){ max_gen = g }
}
mut gen = 0

while(gen < max_gen){
   def out = bytes(w * SX * 3 + 1)
   mut o = 0
   mut i = 0
   while(i < w){
      def v = bytes_get(u, i)
      mut k = 0
      if(v){
         while(k < SX){
            bytes_set(out, o,   load8(CHAR_FULL, 0))
            bytes_set(out, o+1, load8(CHAR_FULL, 1))
            bytes_set(out, o+2, load8(CHAR_FULL, 2))
            o += 3
            k += 1
         }
      } else {
         while(k < SX){
            bytes_set(out, o, 32)
            o += 1
            k += 1
         }
      }
      i += 1
   }
   bytes_set(out, o, 10) o += 1

   mut ry = 0
   while(ry < SY){
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
