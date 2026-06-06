#!/usr/bin/env ny

;; Keywords: cli terminal matrix rain digital example
;; Matrix rain - https://en.wikipedia.org/wiki/Digital_rain
use std.core
use std.core.term
use std.math.random as rng
use std.os.args as cli
use std.os.time as time

def COLOR_DARK = 8
def COLOR_LIGHT = 5
def COLOR_WHITE = 7
def COLOR_BG = 0
def THRESH_HI = 28
def THRESH_MD = 15

fn rand_mod(int n) int {
   if(n <= 1){ return 0 }
   (rng.rand() / 65536) % n
}

def term_size = get_terminal_size()
mut W = term_size.get(0, 80)
mut H = term_size.get(1, 24)

if(W < 1){ W = 80 }
if(H < 1){ H = 24 }
def CANV = canvas(W, H)
def CBUF = CANV.get(2)
def ATTR = CANV.get(3)
def COL = CANV.get(4)
def BLEN = CANV.get(5)
def INTENSITY = bytes(W * H)
def CHARS = bytes(W * H)
def max_frames = cli.first_positive_int()
mut frame = 0
mut ASCII = list(94)
mut i = 0
while(i < 94){
   ASCII = ASCII.append(chr(i + 33))
   i += 1
}

rng.seed(time.ticks())
mut drop_y = list(W)
mut drop_speed = list(W)
mut col = 0
while(col < W){
   drop_y = drop_y.append(0 - rand_mod(H * 20))
   drop_speed = drop_speed.append(15 + rand_mod(35))
   col += 1
}

fn should_quit(int key) bool { is_quit_key(key) || key == 113 || key == 81 }
tui_begin()
defer { tui_end() }
while(true){
   if(should_quit(poll_key())){ break }
   mut x = 0
   while(x < W){
      def next_y = drop_y.get(x, 0) + drop_speed.get(x, 0)
      drop_y.set(x, next_y)
      def y = next_y / 20
      if(y >= 0 && y < H){
         def idx = y * W + x
         bytes_set(CHARS, idx, 33 + rand_mod(94))
         bytes_set(INTENSITY, idx, 31)
      }
      if(y > H + 10){
         drop_y.set(x, 0 - rand_mod(H * 20))
         drop_speed.set(x, 15 + rand_mod(35))
      }
      x += 1
   }
   mut idx = 0
   def n = W * H
   while(idx < n){
      def fade = bytes_get(INTENSITY, idx)
      if(fade > 0){
         mut fg = COLOR_DARK
         mut bold = 0
         if(fade > THRESH_HI){
            fg = COLOR_WHITE
            bold = 1
         } elif(fade > THRESH_MD){
            fg = COLOR_LIGHT
            bold = 1
         }
         def code = bytes_get(CHARS, idx)
         CBUF[idx] = ASCII.get(code - 33, " ")
         bytes_set(BLEN, idx, 1)
         bytes_set(COL, idx, fg)
         bytes_set(ATTR, idx, bold)
         bytes_set(INTENSITY, idx, fade - 1)
      } else {
         CBUF[idx] = " "
         bytes_set(BLEN, idx, 1)
         bytes_set(COL, idx, COLOR_BG)
         bytes_set(ATTR, idx, 0)
      }
      idx += 1
   }
   canvas_refresh(CANV)
   frame += 1
   if(max_frames > 0 && frame >= max_frames){ break }
}
