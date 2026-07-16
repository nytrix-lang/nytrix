#!/usr/bin/env ny

;; Keywords: cli terminal automata langton ant example
;; Langton's Ant - https://en.wikipedia.org/wiki/Langton%27s_ant
use std.core
use std.core.term
use std.os.args as cli

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
def term_size = get_terminal_size()
mut W = term_size.get(0, 80)
mut H = term_size.get(1, 24)

if W < 2 { W = 80 }
if H < 1 { H = 24 }
if W % 2 == 1 { W -= 1 }
def HALF_W = W / 2
def CANV = canvas(W, H)
mut black = own(dict(1024))
mut seen = own(dict(1024))
mut x = 0
mut y = 0
mut dir = DIR_UP
mut steps = 0
def max_steps = cli.first_positive_int()

fn set_pair(int x, int y, str left, str right, int color_idx) int {
   canvas_set(CANV, x, y, left, color_idx, 0)
   canvas_set(CANV, x + 1, y, right, color_idx, 0)
   0
}

fn draw_world() int {
   canvas_clear(CANV)
   def cx = HALF_W / 2
   def cy = H / 2
   def min_x = x - cx
   def min_y = y - cy
   mut sy = 0
   while sy < H {
      mut sx = 0
      while sx < HALF_W {
         def k = key_of(min_x + sx, min_y + sy)
         def px = sx * 2
         if black.contains(k) {
            set_pair(px, sy, CH_BLACK, CH_BLACK, COLOR_WHITE)
         } elif seen.contains(k) {
            set_pair(px, sy, CH_TRAIL, CH_TRAIL, COLOR_GRAY)
         }
         sx += 1
      }
      sy += 1
   }
   mut left = H_UP_L
   mut right = H_UP_R
   if dir == DIR_DOWN {
      left = H_DOWN_L
      right = H_DOWN_R
   } elif dir == DIR_LEFT {
      left = H_LEFT_L
      right = H_LEFT_R
   } elif dir == DIR_RIGHT {
      left = H_RIGHT_L
      right = H_RIGHT_R
   }
   set_pair((HALF_W / 2) * 2, H / 2, left, right, COLOR_HEAD)
   0
}

fn should_quit(int key) bool { is_quit_key(key) || key == 113 || key == 81 }
tui_begin()
defer { tui_end() }
seen = seen.set(key_of(x, y), true)
draw_world()
canvas_refresh(CANV)
while true {
   if should_quit(poll_key()) { break }
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
   draw_world()
   canvas_refresh(CANV)
   steps += 1
   if max_steps > 0 && steps >= max_steps { break }
}
