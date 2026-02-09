#!/bin/ny
;; Matrix Rain (Example) - https://en.wikipedia.org/wiki/Digital_rain

use std.core *
use std.math.random *
use std.os.time *
use std.str *
use std.str.bytes *
use std.str.term *

def C_G = 2
def C_L = 7
def C_W = 7
def C_B = 0
def HI = 28
def MD = 15

def sz = get_terminal_size()
mut W = get(sz, 0, 0)
mut H = get(sz, 1, 0)
if (W < 1) { W = 80 }
if (H < 1) { H = 24 }

def canv = canvas(W, H)
def cbuf = get(canv, 2)
def attr = get(canv, 3)
def col = get(canv, 4)
def blen = get(canv, 5)
def inten = bytes(W * H)
def ch = bytes(W * H)

mut ascii = list(128)
mut i = 0
while (i < 128) {
    ascii = append(ascii, chr(i))
    i += 1
}

mut dy = list(W)
mut ds = list(W)

i = 0
while (i < W) {
    dy = append(dy, -mod(to_int(rand()), H * 20))
    ds = append(ds, 15 + mod(to_int(rand()), 35))
    i += 1
}

seed(ticks())
cursor_hide()
clear_screen()

while (1) {
    mut x = 0
    while (x < W) {
        def yf = get(dy, x, 0) + get(ds, x, 0)
        set_idx(dy, x, yf)
        def y = yf / 20
        if (y >= 0 && y < H) {
            def idx = y * W + x
            bytes_set(ch, idx, 33 + mod(to_int(rand()), 94))
            bytes_set(inten, idx, 31)
        }
        if (y > H + 10) {
            set_idx(dy, x, -mod(to_int(rand()), H * 20))
            set_idx(ds, x, 15 + mod(to_int(rand()), 35))
        }
        x += 1
    }

    mut idx = 0
    def total = W * H
    while (idx < total) {
        def t = bytes_get(inten, idx)
        if (t > 0) {
            def cc = bytes_get(ch, idx)
            mut fg = C_G
            mut b = 0
            if (t > HI) {
                fg = C_W
                b = 1
            } elif (t > MD) {
                fg = C_L
                b = 1
            }
            set_idx(cbuf, idx, get(ascii, cc, " "))
            bytes_set(blen, idx, 1)
            bytes_set(col, idx, fg)
            bytes_set(attr, idx, b)
            bytes_set(inten, idx, t - 1)
        } else {
            set_idx(cbuf, idx, " ")
            bytes_set(blen, idx, 1)
            bytes_set(col, idx, C_B)
            bytes_set(attr, idx, 0)
        }
        idx += 1
    }
    canvas_refresh(canv)
}

screen_reset()
cursor_show()
