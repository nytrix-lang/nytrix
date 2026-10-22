#!/usr/bin/env ny
;; Keywords: cli terminal automata rule110 example
;; Rule 110 - https://en.wikipedia.org/wiki/Rule_110
use core
use core.term
use core.str as str
def SX = 2
fn rule(int l, int c, int r) int { (110 >> (l * 4 + c * 2 + r)) & 1 }
fn mirror_index(int i, int w, int cells) int { i < w ? i : cells - 1 - i }
fn step(bytes cur, bytes nxt, int w) {
   for i in 0..(w - 1) { bytes_set(nxt, int(i), rule(int(i) > 0 ? bytes_get(cur, int(i) - 1) : 0, bytes_get(cur, int(i)), int(i) + 1 < w ? bytes_get(cur, int(i) + 1) : 0)) }
}
def size = get_terminal_size()
def int tW, tH = int(size.get(0, 80)), int(size.get(1, 24))
def cell = tW >= SX ? SX : 1
def cells, w, height_gens = tW / cell, cells - cells / 2, tH > 2 ? tH - 2 : (tH > 1 ? 1 : 0)
def int gens = int(min(height_gens, w))
def title = f"Terminal Size: {tW}x{tH} | Displaying: {gens} Generations"
def heading = tH > gens + 1 ? (title.len <= tW ? title : (tW >= 8 ? "Rule 110" : "")) : ""
def width, left = cells * cell, (tW - cells * cell) / 2
def block, blank, margin = cell == SX ? "██" : "█", str.repeat(" ", cell), str.repeat(" ", left)
if heading.len > 0 {
   write_str(str.repeat(" ", (tW - heading.len) / 2) + color(heading, "magenta") +
      (gens > 0 ? "\n" : ""))
}
mut u = bytes(w).set(w - 1, 1)
mut next = bytes(w)
mut art = str.Builder(gens > 0 ? (tW + width * 2 + 1) * gens : 1)
mut gen = 0
while gen < gens {
   if gen > 0 { art = str.builder_append(art, "\n") }
   art = str.builder_append(art, margin)
   for p in 0..(cells - 1) {
      art = str.builder_append(art, bytes_get(u, mirror_index(int(p), w, cells)) ? block : blank)
   }
   gen += 1
   if gen < gens {
      step(u, next, w)
      u, next = next, u
   }
}
if gens > 0 { write_str(color(str.builder_to_str(art), "magenta")) }
str.builder_free(art)
write_str("\n")
0
