#!/usr/bin/env ny
;; Keywords: cli terminal automata conway life example
;; Conway's Game of Life - https://en.wikipedia.org/wiki/Conway%27s_Game_of_Life
use core
use core.os (exit)
use core.term
use core.math.random as rng
use core.os.time as time
use core.os.fixture as fixture

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

fn seed_grid(any g, int total) int {
   mut i = 0
   while i < total {
      ;; Keep the initial population sparse enough to show distinct Life
      ;; patterns instead of a noisy, nearly full field.
      bytes_set(g, i, rand_mod(12) == 0 ? CELL_ALIVE : CELL_DEAD)
      i += 1
   }
   0
}

mut grid = own(bytes(1))
mut next_grid = own(bytes(1))
mut grid_w = 0
mut grid_h = 0
mut first = 1

fn size_grid(int w, int h) int {
   "Sizes the cell grids for the current canvas; a resize re-seeds the world."
   ;; Leave a terminal column unused so writing the final cell cannot trigger
   ;; automatic wrap on terminals with delayed-wrap semantics.
   def int half = int((w - 1) / 2)
   def rows = h - 1
   if half < 1 || rows < 1 { return 0 }
   if half == grid_w && rows == grid_h { return 0 }
   grid_w = half
   grid_h = rows
   grid = own(bytes(half * rows))
   next_grid = own(bytes(half * rows))
   seed_grid(grid, half * rows)
   first = 1
   0
}

fn render_grid(any c, any g, int width, int height) int {
   def int half = int((width - 1) / 2)
   canvas_clear(c)
   canvas_print(c, 0, 0, "ESC/q quit · Conway's Game of Life", COLOR_ALIVE, 1)
   mut y = 0
   while y < height - 1 {
      mut x = 0
      while x < half {
         if bytes_get(g, y * half + x) {
            def sx = x * 2
            canvas_set(c, sx, y + 1, CHAR_ALIVE, COLOR_ALIVE, 0)
            canvas_set(c, sx + 1, y + 1, CHAR_ALIVE, COLOR_ALIVE, 0)
         }
         x += 1
      }
      y += 1
   }
   canvas_refresh(c)
   0
}

fn evolve(any cur, any out, int width, int height) int {
   mut int alive = 0
   mut int y = 0
   while y < height {
      def int row = y * width
      mut int x = 0
      while x < width {
         def int idx = row + x
         mut int n = 0
         if y > 0 {
            def int prev_row = (y - 1) * width
            if x > 0 { n += bytes_get(cur, int(prev_row) + x - 1) }
            n += bytes_get(cur, int(prev_row) + x)
            if x < width - 1 { n += bytes_get(cur, int(prev_row) + x + 1) }
         }
         if x > 0 { n += bytes_get(cur, row + x - 1) }
         if x < width - 1 { n += bytes_get(cur, row + x + 1) }
         if y < height - 1 {
            def int next_row = (y + 1) * width
            if x > 0 { n += bytes_get(cur, int(next_row) + x - 1) }
            n += bytes_get(cur, int(next_row) + x)
            if x < width - 1 { n += bytes_get(cur, int(next_row) + x + 1) }
         }
         def int was = bytes_get(cur, idx)
         mut int live = CELL_DEAD
         if was == CELL_ALIVE {
            if n == 2 || n == 3 { live = CELL_ALIVE }
         } elif n == 3 {
            live = CELL_ALIVE
         }
         bytes_set(out, idx, live)
         if live { alive += 1 }
         x += 1
      }
      y += 1
   }
   alive
}

rng.seed(time.ticks())
fixture.ready("conway")
exit(fixture.canvas_loop(fn(c, z, w, h) {
   size_grid(w, h)
   if !first {
      def alive = evolve(grid, next_grid, grid_w, grid_h)
      if alive == 0 { seed_grid(next_grid, grid_w * grid_h) }
      def tmp = grid
      grid = own(next_grid)
      next_grid = own(tmp)
   }
   first = 0
   render_grid(c, grid, w, h)
}, 600, 100, 0))
