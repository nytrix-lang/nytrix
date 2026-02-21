#!/bin/ny
;; Matrix Rain (Example) - https://en.wikipedia.org/wiki/Digital_rain

use std.core *
use std.math.random *
use std.os.args *
use std.os.time *
use std.str *
use std.str.bytes *
use std.str.term *

;; Constants
def COLOR_DARK  = 2
def COLOR_LIGHT = 7
def COLOR_WHITE = 7
def COLOR_BG    = 0
def THRESH_HI   = 28
def THRESH_MD   = 15

;; State Init
def tSize = get_terminal_size()
mut W = get(tSize, 0, 0)
mut H = get(tSize, 1, 0)
if(W < 1){ W = 80 }
if(H < 1){ H = 24 }
mut max_frames = 0
mut frame_count = 0

def av = args()
mut ai = 0
while(ai < len(av)){
   def n = atoi(get(av, ai, ""))
   if(n > 0){
      max_frames = n
      break
   }
   ai += 1
}

def CANV  = canvas(W, H)
def CBUF  = get(CANV, 2)
def ATTR  = get(CANV, 3)
def COL   = get(CANV, 4)
def BLEN  = get(CANV, 5)
def INTEN = bytes(W * H)
def CHARS = bytes(W * H)

mut ASCII = list(94)
mut i = 0
while(i < 94){
   ASCII = append(ASCII, chr(i + 33))
   i += 1
}

mut DY = list(W)
mut DS = list(W)
mut j = 0
while(j < W){
   DY = append(DY, -mod(to_int(rand()), H * 20))
   DS = append(DS, 15 + mod(to_int(rand()), 35))
   j += 1
}

seed(ticks())
tui_begin()
defer { tui_end() }

while(1){
   def key = poll_key()
   if(is_quit_key(key)){ break }

   ;; Update columns
   mut x = 0
   while(x < W){
      def yf = get(DY, x, 0) + get(DS, x, 0)
      set_idx(DY, x, yf)
      def y = yf / 20
      if(y >= 0 && y < H){
         def idx = y * W + x
         bytes_set(CHARS, idx, 33 + mod(to_int(rand()), 94))
         bytes_set(INTEN, idx, 31)
      }
      if(y > H + 10){
         set_idx(DY, x, -mod(to_int(rand()), H * 20))
         set_idx(DS, x, 15 + mod(to_int(rand()), 35))
      }
      x += 1
   }

   ;; Render buffer
   mut idx = 0
   def n = W * H
   while(idx < n){
      def t = bytes_get(INTEN, idx)
      if(t > 0){
         def cc = bytes_get(CHARS, idx)
         mut fg = COLOR_DARK
         mut b  = 0
         if(t > THRESH_HI){
            fg = COLOR_WHITE
            b  = 1
         } elif(t > THRESH_MD){
            fg = COLOR_LIGHT
            b  = 1
         }

         set_idx(CBUF, idx, get(ASCII, cc - 33, " "))
         bytes_set(BLEN, idx, 1)
         bytes_set(COL, idx, fg)
         bytes_set(ATTR, idx, b)
         bytes_set(INTEN, idx, t - 1)
      } else {
         set_idx(CBUF, idx, " ")
         bytes_set(BLEN, idx, 1)
         bytes_set(COL, idx, COLOR_BG)
         bytes_set(ATTR, idx, 0)
      }
      idx += 1
   }
   canvas_refresh(CANV)
   frame_count += 1
   if(max_frames > 0 && frame_count >= max_frames){ break }
}
