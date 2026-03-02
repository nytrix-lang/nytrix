#!/bin/ny
;; Conway's Game of Life - https://en.wikipedia.org/wiki/Conway%27s_Game_of_Life

use std.core *
use std.math.random *
use std.os.args *
use std.os.time *
use std.os *
use std.text *
use std.text.bytes *
use std.text.term *

;; Constants
def CELL_ALIVE  = 1
def CELL_DEAD   = 0
def COLOR_ALIVE = 2
def COLOR_DEAD  = 0
def CHAR_ALIVE  = "\xe2\x96\x88"
def CHAR_DEAD   = " "
def LEN_ALIVE   = str_len(CHAR_ALIVE)
def LEN_DEAD    = 1

;; State Init
def tSize = get_terminal_size()
mut W = get(tSize, 0, 0)
mut H = get(tSize, 1, 0)
if(W < 2){ W = 80 }
if(H < 2){ H = 25 }
if(W % 2 == 1){ W -= 1 }
H -= 0

def LW    = W / 2
def CANV  = canvas(W, H)
def CBUF  = get(CANV, 2)
def ATTR  = get(CANV, 3)
def COL   = get(CANV, 4)
def BLEN  = get(CANV, 5)

def TOTAL = LW * H
mut G     = bytes(TOTAL)
mut G2    = bytes(TOTAL)
mut max_steps = 0

if(env("CI") || env("NYTRIX_TEST_MODE") == "1"){
   max_steps = 100
}

def av = args()
mut ai = 0
while(ai < len(av)){
   def n = atoi(get(av, ai, ""))
   if(n > 0){
      max_steps = n
      break
   }
   ai += 1
}
mut step_count = 0

fn set_pair(x, y, ch, lch, c){
   def idx = y * W + x
   set_idx(CBUF, idx, ch)
   bytes_set(BLEN, idx, lch)
   bytes_set(COL, idx, c)
   bytes_set(ATTR, idx, 0)

   def idx2 = idx + 1
   set_idx(CBUF, idx2, ch)
   bytes_set(BLEN, idx2, lch)
   bytes_set(COL, idx2, c)
   bytes_set(ATTR, idx2, 0)
}

fn seed_grid(g, n){
   mut i = 0
   while(i < n){
      def r = randint(0, 4) ;; Lower density: ~20%
      if(r == 1){ __store8_idx(g, i, CELL_ALIVE) } else { __store8_idx(g, i, CELL_DEAD) }
      i += 1
   }
}

fn draw_full(g){
   mut y = 0
   while(y < H){
      def yw = y * LW
      mut x = 0
      while(x < LW){
         def idx = yw + x
         def sx = x * 2
         if(__load8_idx(g, idx)){
            set_pair(sx, y, CHAR_ALIVE, LEN_ALIVE, COLOR_ALIVE)
         } else {
            set_pair(sx, y, CHAR_DEAD, LEN_DEAD, COLOR_DEAD)
         }
         x += 1
      }
      y += 1
   }
}

fn step(cur, nxt){
   mut alive = 0
   mut y = 0
   while(y < H){
      def yw = y * LW
      mut x = 0
      while(x < LW){
         def idx = yw + x
         mut n = 0
         def xm = x - 1
         def xp = x + 1

         if(y > 0){
            def ym = (y - 1) * LW
            if(x > 0){ n += __load8_idx(cur, ym + xm) }
            n += __load8_idx(cur, ym + x)
            if(x < LW - 1){ n += __load8_idx(cur, ym + xp) }
         }
         if(x > 0){ n += __load8_idx(cur, yw + xm) }
         if(x < LW - 1){ n += __load8_idx(cur, yw + xp) }
         if(y < H - 1){
            def yp = (y + 1) * LW
            if(x > 0){ n += __load8_idx(cur, yp + xm) }
            n += __load8_idx(cur, yp + x)
            if(x < LW - 1){ n += __load8_idx(cur, yp + xp) }
         }

         def curv = __load8_idx(cur, idx)
         mut live = CELL_DEAD
         if(curv == CELL_ALIVE){
            if(n == 2 || n == 3){ live = CELL_ALIVE }
         } else {
            if(n == 3){ live = CELL_ALIVE }
         }

         __store8_idx(nxt, idx, live)
         if(live){ alive += 1 }
         x += 1
      }
      y += 1
   }
   alive
}

;; Main Line
seed(ticks())
seed_grid(G, TOTAL)

tui_begin()
defer { tui_end() }

while(1){
   def key = poll_key()
   if(is_quit_key(key)){ break }

   draw_full(G)
   canvas_print(CANV, 0, 0, "ESC: Quit", 7, 0)
   canvas_refresh(CANV)

   def alive = step(G, G2)

   if(alive == 0){
      seed_grid(G, TOTAL)
   } else {
      def tmp = G
      G = G2
      G2 = tmp
   }

   msleep(35)
   step_count += 1
   if(max_steps > 0 && step_count >= max_steps){ break }
}
tui_end()
