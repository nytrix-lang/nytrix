#!/bin/ny
;; Langton's Ant - https://en.wikipedia.org/wiki/Langton%27s_ant

use std.core *
use std.os.time *
use std.str.term *

def t = get_terminal_size()
mut W = get(t, 0)
mut H = get(t, 1)

if (W < 1) { W = 80 }
if (H < 1) { H = 24 }

def canv = canvas(W, H)
cursor_hide()

mut grid = list(W * H)
mut i = 0
while (i < W * H) {
    grid = append(grid, 0)
    i += 1
}

mut x    = W / 2
mut y    = H / 2
mut dir  = 0
mut step = 0

while (1) {
    def k = y * W + x
    def v = get(grid, k)

    set_idx(grid, k, v ? 0 : 1)
    dir = (dir + (v ? 1 : 3)) % 4

    if (dir == 0) {
        y -= 1
        if (y < 0) { y = H - 1 }
    } elif (dir == 1) {
        x += 1
        if (x >= W) { x = 0 }
    } elif (dir == 2) {
        y += 1
        if (y >= H) { y = 0 }
    } else {
        x -= 1
        if (x < 0) { x = W - 1 }
    }

    if (step % 10 == 0) {
        mut r = 0
        while (r < H) {
            mut c = 0
            while (c < W) {
                def is_ant = (c == x && r == y)
                def cell   = get(grid, r * W + c)

                if (is_ant) {
                    canvas_set(canv, c, r, 64, 1, 0)
                } elif (cell) {
                    canvas_set(canv, c, r, 35, 7, 0)
                } else {
                    canvas_set(canv, c, r, 32, 0, 0)
                }

                c += 1
            }
            r += 1
        }

        canvas_refresh(canv)
        msleep(20)
    }

    step += 1
}

screen_reset()
cursor_show()
