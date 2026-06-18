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
def THRESH_HI = 29
def THRESH_MD = 16
def PAL = [1, 2, 3, 4, 5, 6, 9, 10, 11, 12, 13, 14]
def HEAD = [15, 15, 14, 11, 10, 7]
def GLYPHS = ["a","b","c","d","e","f","g","h","i","j","k","l","m","n","o","p","q","r","s","t","u","v","w","x","y","z","A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z","0","1","2","3","4","5","6","7","8","9","!","@","#","$","%","^","&","*","(",")","[","]","{","}","<",">","?","/","\\","|","+","=","-","_","~",":",";",".",",","`","'","日","一","木","山","水","火","土","金","月","天","地","人","雨","風","花","龍","鳥","虫","牛","馬","竹","弓","大","中","小","上","下","左","右","出"]

fn rand_mod(int n) int {
   if n <= 1 { return 0 }
   (rng.rand() / 65536) % n
}

fn pick(a) {
   a.get(rand_mod(a.len), 15)
}

def term_size = get_terminal_size()
mut W = term_size.get(0, 80)
mut H = term_size.get(1, 24)

if W < 1 { W = 80 }
if H < 1 { H = 24 }
def CANV = canvas(W, H)
def CBUF = CANV.get(2)
def ATTR = CANV.get(3)
def COL = CANV.get(4)
def BLEN = CANV.get(5)
def INTENSITY = bytes(W * H)
def CHARS = bytes(W * H)
def max_frames = cli.first_positive_int()
mut frame = 0

rng.seed(time.ticks())
mut drop_y = list(W)
mut drop_speed = list(W)
mut drop_color = list(W)
mut drop_seq = list(W)
mut col = 0
while col < W {
   drop_y = drop_y.append(0 - rand_mod(H * 4))
   drop_speed = drop_speed.append(12 + rand_mod(50))
   drop_color = drop_color.append(pick(PAL))
   drop_seq = drop_seq.append(rand_mod(GLYPHS.len))
   col += 1
}

fn should_quit(int key) bool { is_quit_key(key) || key == 113 || key == 81 }
tui_begin()
defer { tui_end() }
while true {
   if should_quit(poll_key()) { break }
   mut x = 0
   while x < W {
      def next_y = drop_y.get(x, 0) + drop_speed.get(x, 0)
      drop_y.set(x, next_y)
       def y = next_y / 20
       if y >= 0 && y < H {
          def idx = y * W + x
          def seq = drop_seq.get(x, 0)
          bytes_set(CHARS, idx, seq % GLYPHS.len)
          drop_seq.set(x, seq + 1)
          bytes_set(INTENSITY, idx, THRESH_HI + 8)
       }
       if y > H + rand_mod(6) {
          drop_y.set(x, 0 - rand_mod(H * 4))
          drop_speed.set(x, 12 + rand_mod(50))
          drop_color.set(x, pick(PAL))
          drop_seq.set(x, rand_mod(GLYPHS.len))
       }
      x += 1
   }
   mut idx = 0
   def n = W * H
   while idx < n {
      def fade = bytes_get(INTENSITY, idx)
      if fade > 0 {
         mut fg = drop_color.get(idx % W, COLOR_DARK)
         mut bold = 0
         if fade > THRESH_HI {
            fg = pick(HEAD)
            bold = 1
         } elif fade > THRESH_MD {
            bold = 1
         }
         def code = bytes_get(CHARS, idx)
         def ch = GLYPHS.get(code, " ")
         CBUF[idx] = ch
         store8(BLEN, ch.len, idx)
         store8(COL, fg, idx)
         store8(ATTR, bold, idx)
         bytes_set(INTENSITY, idx, fade - 1)
      } else {
         CBUF[idx] = " "
         store8(BLEN, 1, idx)
         store8(COL, COLOR_BG, idx)
         store8(ATTR, 0, idx)
      }
      idx += 1
   }
   canvas_refresh(CANV)
   frame += 1
   if max_frames > 0 && frame >= max_frames { break }
}