#!/usr/bin/env ny

;; Keywords: cli terminal automata rule110 example
;; Rule 110 - https://en.wikipedia.org/wiki/Rule_110
use std.core
use std.core.term

def SX = 2

fn rule(l, c, r) {
   if(l && c && r){ return 0 }
   if(c || r){ return 1 }
   0
}

fn step(cur, w) {
   def nxt = bytes(w)
   mut i = 0
   while(i < w){
      def l = (i > 0) && (bytes_get(cur, i - 1) == 1)
      def c = (bytes_get(cur, i) == 1)
      def r = (i < w - 1) && (bytes_get(cur, i + 1) == 1)
      if(rule(l, c, r)){ bytes_set(nxt, i, 1) }
      else { bytes_set(nxt, i, 0) }
      i += 1
   }
   nxt
}

def tSize = get_terminal_size()
mut tW = tSize.get(0, 0)
mut tH = tSize.get(1, 0)

if(tW <= 0){ tW = 80 }
if(tH <= 0){ tH = 24 }
def target_gens = 27
def prefix_len  = 7
mut w = (tW - prefix_len) / (SX * 2)

if(w < 1){ w = 1 }
write_str(color(f"Terminal Size: {tW}x{tH} | Displaying: {target_gens} Generations", "magenta") + "\n")
mut u = bytes(w)
bytes_set(u, w - 1, 1)
def block = "██"
def space = "  "
mut gen = 0
while(gen < target_gens){
   mut line = ""
   mut i = 0
   while(i < w){
      if(bytes_get(u, i)){ line = line + block }
      else { line = line + space }
      i += 1
   }
   mut i2 = w - 1
   while(i2 >= 0){
      if(bytes_get(u, i2)){ line = line + block }
      else { line = line + space }
      i2 -= 1
   }
   write_str(color(line, "magenta") + "\n")
   u = step(u, w)
   gen += 1
}
