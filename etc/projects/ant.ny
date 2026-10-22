#!/usr/bin/env ny
;; Keywords: cli terminal automata langton ant example
;; Langton's Ant - https://en.wikipedia.org/wiki/Langton%27s_ant
use core
use core.os (exit)
use core.term
use core.os.fixture as fixture

def CH_BLACK = "█"
def CH_TRAIL = "░"
def COLOR_WHITE = 7
def COLOR_GRAY  = 8
def COLOR_HEAD  = 5
def DIR_UP    = 0
def DIR_RIGHT = 1
def DIR_DOWN  = 2
def DIR_LEFT  = 3
def H_UP_L    = "▄"
def H_UP_R    = "█"
def H_DOWN_L  = "█"
def H_DOWN_R  = "▀"
def H_LEFT_L  = "▄"
def H_LEFT_R  = "█"
def H_RIGHT_L = "█"
def H_RIGHT_R = "▄"

fn key_of(int x, int y) str { f"{x}:{y}" }

mut black = own(dict(1024))
mut seen = own(dict(1024))
mut x = 0
mut y = 0
mut dir = DIR_UP
mut steps = 0

fn draw_world(any c, int width, int height, int half, int cx, int cy) int {
   canvas_clear(c)
   canvas_print(c, 0, 0, "ESC/q quit · Langton's Ant", COLOR_HEAD, 1)
   def ox = cx - half / 2
   def oy = cy - (height - 1) / 2
   mut py = 0
   while py < height - 1 {
      mut px = 0
      while px < half {
         def k = key_of(ox + px, oy + py)
         def sx = px * 2
         if black.contains(k) {
            canvas_set(c, sx, py + 1, CH_BLACK, COLOR_WHITE, 0)
            canvas_set(c, sx + 1, py + 1, CH_BLACK, COLOR_WHITE, 0)
         } elif seen.contains(k) {
            canvas_set(c, sx, py + 1, CH_TRAIL, COLOR_GRAY, 0)
            canvas_set(c, sx + 1, py + 1, CH_TRAIL, COLOR_GRAY, 0)
         }
         px += 1
      }
      py += 1
   }
   mut left = H_UP_L
   mut right = H_UP_R
   if dir == DIR_DOWN { left, right = H_DOWN_L, H_DOWN_R
   } elif dir == DIR_LEFT { left, right = H_LEFT_L, H_LEFT_R
   } elif dir == DIR_RIGHT { left, right = H_RIGHT_L, H_RIGHT_R }
   def head_x = (half / 2) * 2
   def head_y = height / 2
   canvas_set(c, head_x, head_y, left, COLOR_HEAD, 1)
   canvas_set(c, head_x + 1, head_y, right, COLOR_HEAD, 1)
   canvas_refresh(c)
   0
}

fixture.ready("ant")
seen = seen.set(key_of(x, y), true)
exit(fixture.canvas_loop(fn(c, z, w, h) {
   def ew = w - (w % 2)
   def k = key_of(x, y)
   if black.contains(k) {
      black = black.delete(k)
      dir = (dir + 3) % 4
   } else {
      black = black.set(k, true)
      dir = (dir + 1) % 4
   }
   seen = seen.set(k, true)
   if dir == DIR_UP { y -= 1 }
   elif dir == DIR_RIGHT { x += 1 }
   elif dir == DIR_DOWN { y += 1 }
   else { x -= 1 }
   seen = seen.set(key_of(x, y), true)
   draw_world(c, ew, h, ew / 2, x, y)
   steps += 1
}, 600, 100, 0))
