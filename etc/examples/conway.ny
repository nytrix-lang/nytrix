#!/bin/ny
;; Conway's Game of Life (Example) - https://en.wikipedia.org/wiki/Conway%27s_Game_of_Life

use std.core *
use std.math.random *
use std.os.time *
use std.str.term *

def t = get_terminal_size()
mut W = get(t, 0)
mut H = get(t, 1)

if (W < 1) { W = 80 }
if (H < 1) { H = 24 }

def canv = canvas(W, H)
cursor_hide()

mut g  = list(W * H)
mut ng = list(W * H)

mut i = 0
while (i < W * H) {
    def r = (to_int(rand()) % 6) == 0
    g  = append(g,  r ? 1 : 0)
    ng = append(ng, 0)
    i += 1
}

while (1) {
    mut y = 0
    while (y < H) {
        def yw = y * W
        def ym = (y == 0     ? H - 1 : y - 1) * W
        def yp = (y == H - 1 ? 0     : y + 1) * W

        mut x = 0
        while (x < W) {
            def xm  = (x == 0     ? W - 1 : x - 1)
            def xp  = (x == W - 1 ? 0     : x + 1)
            def idx = yw + x

            mut n =
                get(g, ym + xm) + get(g, ym + x) + get(g, ym + xp) +
                get(g, yw + xm)                  + get(g, yw + xp) +
                get(g, yp + xm) + get(g, yp + x) + get(g, yp + xp)

            def cur  = get(g, idx)
            def live = (cur == 1 ? (n == 2 || n == 3) : (n == 3)) ? 1 : 0

            set_idx(ng, idx, live)

            if (live) {
                canvas_set(canv, x, y, 64, 2, 0)
            } else {
                canvas_set(canv, x, y, 32, 0, 0)
            }

            x += 1
        }
        y += 1
    }

    canvas_refresh(canv)

    def tmp = g
    g  = ng
    ng = tmp

    msleep(50)
}

screen_reset()
cursor_show()
