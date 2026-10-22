#!/usr/bin/env ny

;; Keywords: cli terminal matrix rain digital example
;; Matrix rain - https://en.wikipedia.org/wiki/Digital_rain
use core
use core.os (exit)
use core.term
use core.math.random as rng
use core.os.time as time
use core.os.fixture as fixture

def COLOR_RAIN = 2
def COLOR_CYAN = 6
def COLOR_WHITE = 7
def HEAD_TAIL_BRIGHT_ROWS = 1
def TRAIL_CYAN_ROWS = 2
def str GLYPHS = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
def str SYMBOLS = "+*/=<>|"

fn rand_mod(int n) int {
   if n <= 1 { return 0 }
   rng.randrange(0, n)
}

fn random_glyph() int {
   if rand_mod(100) < 12 { return GLYPHS.len + rand_mod(SYMBOLS.len) }
   rand_mod(GLYPHS.len)
}

fn glyph_char(int code) str {
   if code < GLYPHS.len { return GLYPHS[code] }
   SYMBOLS[code - GLYPHS.len]
}

fn random_drop_length(int h) int {
   mut int max_len = h - 3
   if max_len < 3 { max_len = 3 }
   3 + rand_mod(max_len - 2)
}

mut bytes CHARS = own(bytes(1))
;; Drop positions use twentieths of a row; speeds below 20 never skip a cell.
mut list<int> drop_y = own([0])
mut list<int> drop_speed = own([0])
mut list<int> drop_len = own([0])
mut int buf_w = 0
mut int buf_h = 0

fn size_state(int w, int h) int {
   "Sizes the rain buffers for the current canvas; a resize re-seeds the drops."
   if w < 1 || h < 2 { return 0 }
   if w == buf_w && h == buf_h { return 0 }
   buf_w = w
   buf_h = h
   CHARS = own(bytes(w * h))
   mut list<int> next_drop_y = []
   mut list<int> next_drop_speed = []
   mut list<int> next_drop_len = []
   mut int col = 0
   while col < w {
      next_drop_y = next_drop_y.append(0 - rand_mod(h * 40))
      next_drop_speed = next_drop_speed.append(12 + rand_mod(8))
      next_drop_len = next_drop_len.append(random_drop_length(h))
      col += 1
   }
   drop_y = own(next_drop_y)
   drop_speed = own(next_drop_speed)
   drop_len = own(next_drop_len)
   0
}

fn draw(any c, int w, int h) int {
   mut int x = 0
   while x < w {
      def int old_y = drop_y[x] / 20
      def int next_y = drop_y[x] + drop_speed[x]
      drop_y[x] = next_y
      def int y = next_y / 20
      if y > old_y {
         if y > 0 && y < h {
            def int idx = y * w + x
            bytes_set(CHARS, idx, random_glyph())
            canvas_set(c, x, y, glyph_char(CHARS[idx]), COLOR_WHITE, 1)
         }
         if old_y > 0 && old_y < h {
            def int idx = old_y * w + x
            canvas_set(c, x, old_y, glyph_char(CHARS[idx]), COLOR_CYAN, 1)
         }
         if old_y - HEAD_TAIL_BRIGHT_ROWS > 0 &&
            old_y - HEAD_TAIL_BRIGHT_ROWS < h {
            def int idx = (old_y - HEAD_TAIL_BRIGHT_ROWS) * w + x
            canvas_set(c, x, old_y - HEAD_TAIL_BRIGHT_ROWS,
                       glyph_char(CHARS[idx]), COLOR_CYAN, 0)
         }
         if old_y - TRAIL_CYAN_ROWS > 0 && old_y - TRAIL_CYAN_ROWS < h {
            def int idx = (old_y - TRAIL_CYAN_ROWS) * w + x
            canvas_set(c, x, old_y - TRAIL_CYAN_ROWS,
                       glyph_char(CHARS[idx]), COLOR_RAIN, 0)
         }
         def int tail_clear_y = y - drop_len[x]
         if tail_clear_y > 0 && tail_clear_y < h {
            canvas_set(c, x, tail_clear_y, " ", 0, 0)
         }
      }
      if y > h + drop_len[x] {
         drop_y[x] = 0 - (rand_mod(h) + 1) * 20
         drop_speed[x] = 12 + rand_mod(8)
         drop_len[x] = random_drop_length(h)
      }
      x += 2
   }
   canvas_print(c, 0, 0, "MATRIX RAIN", COLOR_CYAN, 1)
   canvas_print(c, 11, 0, " · q/ESC quit", COLOR_RAIN, 0)
   canvas_refresh(c)
   0
}

rng.seed(time.ticks())
fixture.ready("matrix")
exit(fixture.canvas_loop(fn(c, z, w, h) {
   size_state(w, h)
   draw(c, w, h)
}, 900, 30, 0))
