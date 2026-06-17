#!/usr/bin/env ny

;; Keywords: cli terminal automata conway life example
;; Conway's Game of Life - https://en.wikipedia.org/wiki/Conway%27s_Game_of_Life
use std.core
use std.core.term
use std.math.random as rng
use std.os.args as cli
use std.os.time as time

def CELL_ALIVE = 1
def CELL_DEAD = 0
def CHAR_ALIVE = "█"
def CHAR_DEAD = " "
def COLOR_ALIVE = 5
def COLOR_DEAD = 0

fn rand_mod(int n) int {
   if n <= 1 { return 0 }
   (rng.rand() / 65536) % n
}

def term_size = get_terminal_size()
mut W = term_size.get(0, 80)
mut H = term_size.get(1, 24)

if W < 2 { W = 80 }
if H < 1 { H = 24 }
if W % 2 == 1 { W -= 1 }
def HALF_W = W / 2
def CANV = canvas(W, H)
def TOTAL = HALF_W * H
mut grid = bytes(TOTAL)
mut next_grid = bytes(TOTAL)
mut frames = 0
def max_frames = cli.first_positive_int()

fn set_pair(int x, int y, str ch, int color_idx) int {
   canvas_set(CANV, x, y, ch, color_idx, 0)
   canvas_set(CANV, x + 1, y, ch, color_idx, 0)
   0
}

fn seed_grid(any g) int {
   mut i = 0
   while i < TOTAL {
      bytes_set(g, i, rand_mod(2) == 0 ? CELL_ALIVE : CELL_DEAD)
      i += 1
   }
   0
}

fn draw_full(any g) int {
   mut y = 0
   while y < H {
      def row = y * HALF_W
      mut x = 0
      while x < HALF_W {
         def idx = row + x
         def sx = x * 2
         if bytes_get(g, idx) {
            set_pair(sx, y, CHAR_ALIVE, COLOR_ALIVE)
         } else {
            set_pair(sx, y, CHAR_DEAD, COLOR_DEAD)
         }
         x += 1
      }
      y += 1
   }
   0
}

fn evolve(any cur, any out) int {
   mut alive = 0
   mut y = 0
   while y < H {
      def row = y * HALF_W
      mut x = 0
      while x < HALF_W {
         def idx = row + x
         mut n = 0
         if y > 0 {
            def prev = (y - 1) * HALF_W
            if x > 0 { n += bytes_get(cur, prev + x - 1) }
            n += bytes_get(cur, prev + x)
            if x < HALF_W - 1 { n += bytes_get(cur, prev + x + 1) }
         }
         if x > 0 { n += bytes_get(cur, row + x - 1) }
         if x < HALF_W - 1 { n += bytes_get(cur, row + x + 1) }
         if y < H - 1 {
            def nxt = (y + 1) * HALF_W
            if x > 0 { n += bytes_get(cur, nxt + x - 1) }
            n += bytes_get(cur, nxt + x)
            if x < HALF_W - 1 { n += bytes_get(cur, nxt + x + 1) }
         }
         def was = bytes_get(cur, idx)
         mut live = CELL_DEAD
         if was == CELL_ALIVE {
            if n == 2 || n == 3 { live = CELL_ALIVE }
         } elif n == 3 {
            live = CELL_ALIVE
         }
         bytes_set(out, idx, live)
         if live { alive += 1 }
         if live != was {
            if live { set_pair(x * 2, y, CHAR_ALIVE, COLOR_ALIVE) }
            else { set_pair(x * 2, y, CHAR_DEAD, COLOR_DEAD) }
         }
         x += 1
      }
      y += 1
   }
   alive
}

fn should_quit(int key) bool { is_quit_key(key) || key == 113 || key == 81 }
rng.seed(time.ticks())
tui_begin()
defer { tui_end() }
seed_grid(grid)
draw_full(grid)
canvas_refresh(CANV)
while true {
   if should_quit(poll_key()) { break }
   def alive = evolve(grid, next_grid)
   if alive == 0 {
      seed_grid(next_grid)
      draw_full(next_grid)
   }
   canvas_refresh(CANV)
   def tmp = grid
   grid = next_grid
   next_grid = tmp
   frames += 1
   if max_frames > 0 && frames >= max_frames { break }
}
