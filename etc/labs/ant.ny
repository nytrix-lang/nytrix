#!/usr/bin/env ny
;; Keywords: cli terminal automata langton ant example
;; Langton's Ant - https://en.wikipedia.org/wiki/Langton%27s_ant
use core
use core.os (exit)
use core.term
use core.os.fixture as fixture

def CH_BLACK = "█"
def CH_TRAIL = "░"
def COLOR_WHITE = 15
def COLOR_GRAY = 7
def COLOR_HEAD = 5

def DIR_UP = 0
def DIR_RIGHT = 1
def DIR_DOWN = 2
def DIR_LEFT = 3

def H_UP_L = "▄"
def H_UP_R = "█"
def H_DOWN_L = "█"
def H_DOWN_R = "▀"
def H_LEFT_L = "▄"
def H_LEFT_R = "█"
def H_RIGHT_L = "█"
def H_RIGHT_R = "▄"

fn key_of(int x, int y) str { f"{x}:{y}" }

mut black = own(dict(1024))
mut seen = own(dict(1024))
mut x = 0
mut y = 0
mut dir = DIR_UP
mut steps = 0
mut cam_x = 0
mut cam_y = 0

fn follow_camera(int cols, int rows) int {
   def mx = cols / 6
   def my = rows / 6
   def ox = cam_x - cols / 2
   def oy = cam_y - rows / 2

   if x < ox + mx { cam_x = x - mx + cols / 2 }
   elif x >= ox + cols - mx {
      cam_x = x - cols + mx + 1 + cols / 2
   }

   if y < oy + my { cam_y = y - my + rows / 2 }
   elif y >= oy + rows - my {
      cam_y = y - rows + my + 1 + rows / 2
   }
   0
}

fn draw_world(any c, int width, int height) int {
   canvas_clear(c)
   canvas_print(c, 0, 0, f"ESC/q quit · Langton's Ant · {steps} steps", COLOR_HEAD, 1)

   def cols = width / 2
   def rows = height - 1
   def ox = cam_x - cols / 2
   def oy = cam_y - rows / 2

   mut py = 0
   while py < rows {
      mut px = 0
      while px < cols {
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

   def hx = (x - ox) * 2
   def hy = y - oy + 1
   if hx >= 0 and hx + 1 < width and hy > 0 and hy < height {
      canvas_set(c, hx, hy, left, COLOR_HEAD, 1)
      canvas_set(c, hx + 1, hy, right, COLOR_HEAD, 1)
   }

   canvas_refresh(c)
   0
}

fixture.ready("ant")
seen = seen.set(key_of(x, y), true)

exit(fixture.canvas_loop(fn(c, z, w, h) {
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

   steps += 1
   if w >= 4 and h >= 3 {
      follow_camera(w / 2, h - 1)
      draw_world(c, w - w % 2, h)
   }
}, 600, 100, 0))
