#!/bin/ny
use std.core *
use std.core.iter *
use std.math.random *
use std.os.time *
use std.str.bytes *
use std.str.term *

;; Conway's Game of Life (Example) - https://en.wikipedia.org/wiki/Conway%27s_Game_of_Life

def t = get_terminal_size()
def [W, H] = t
if W <= 0 { W = 80 }
if H <= 0 { H = 24 }

def g, ng = bytes(W * H), bytes(W * H)
def canv = canvas(W, H)

for i in range(W * H) {
    bytes_set(g, i, (rand() % 6 == 0 ? 1 : 0))
}

clear_screen()
cursor_hide()

try {
    while 1 {
        canvas_clear(canv)
        mut y = 0
        while y < H {
            def yw = y * W
            def ym = (y == 0 ? H - 1 : y - 1) * W
            def yp = (y == H - 1 ? 0 : y + 1) * W
            mut x = 0
            while x < W {
                def xm = (x == 0 ? W - 1 : x - 1)
                def xp = (x == W - 1 ? 0 : x + 1)
                mut n = bytes_get(g, ym+xm) + bytes_get(g, ym+x) + bytes_get(g, ym+xp) +
                        bytes_get(g, yw+xm)                      + bytes_get(g, yw+xp) +
                        bytes_get(g, yp+xm) + bytes_get(g, yp+x) + bytes_get(g, yp+xp)
                def idx = yw + x
                mut cur = bytes_get(g, idx)
                def live = (cur == 1 ? (n == 2 || n == 3) : (n == 3)) ? 1 : 0
                bytes_set(ng, idx, live)
                if live {
                    canvas_set(canv, x, y, 35, 7, 1)
                }
                x = x + 1
            }
            y = y + 1
        }
        canvas_refresh(canv)
        def tmp = g g = ng ng = tmp
        msleep(15)
    }
} catch e {
    screen_reset()
}

