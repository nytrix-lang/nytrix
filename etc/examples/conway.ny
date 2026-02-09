#!/bin/ny
;; Langton's Ant - https://en.wikipedia.org/wiki/Langton%27s_ant

use std.core *
use std.math.random *
use std.os.time *
use std.str.bytes *
use std.str.term *

def ALIVE = 1
def DEAD = 0
def COLOR_GREEN = 2
def COLOR_BG = 0
def CH = "\xe2\x96\x88"
def SP = " "
def LEN_CH = 3
def LEN_SP = 1

def sz = get_terminal_size()
mut W = get(sz, 0, 0)
mut H = get(sz, 1, 0)
if (W < 2) { W = 80 }
if (H < 1) { H = 24 }
if (W % 2 == 1) { W = W - 1 }

def LW = W / 2
def canv = canvas(W, H)
def cbuf = get(canv, 2)
def attr = get(canv, 3)
def col = get(canv, 4)
def blen = get(canv, 5)

fn set_pair(x, y, left, right, len_left, len_right, color_idx){
    def idx = y * W + x
    set_idx(cbuf, idx, left)
    bytes_set(blen, idx, len_left)
    bytes_set(col, idx, color_idx)
    bytes_set(attr, idx, 0)
    def idx2 = idx + 1
    set_idx(cbuf, idx2, right)
    bytes_set(blen, idx2, len_right)
    bytes_set(col, idx2, color_idx)
    bytes_set(attr, idx2, 0)
}

fn seed_grid(g, total) {
    mut i = 0
    while (i < total) {
        def r = randrange(0, 3)
        if (r == 0) {
            bytes_set(g, i, ALIVE)
        } else {
            bytes_set(g, i, DEAD)
        }
        i += 1
    }
}

fn draw_full(g, LW, H) {
    mut y = 0
    while (y < H) {
        def yw = y * LW
        mut x = 0
        while (x < LW) {
            def idx = yw + x
            def sx = x * 2
            if (bytes_get(g, idx)) {
                set_pair(sx, y, CH, CH, LEN_CH, LEN_CH, COLOR_GREEN)
            } else {
                set_pair(sx, y, SP, SP, LEN_SP, LEN_SP, COLOR_BG)
            }
            x += 1
        }
        y += 1
    }
}

fn step(curr, next, LW, H) {
    mut alive = 0
    mut y = 0
    while (y < H) {
        def yw = y * LW
        mut ym = y - 1
        mut yp = y + 1
        if (y == 0) { ym = H - 1 }
        if (y == H - 1) { yp = 0 }
        ym = ym * LW
        yp = yp * LW
        mut x = 0
        while (x < LW) {
            mut xm = x - 1
            mut xp = x + 1
            if (x == 0) { xm = LW - 1 }
            if (x == LW - 1) { xp = 0 }
            def idx = yw + x
            def n =
                bytes_get(curr, ym + xm) + bytes_get(curr, ym + x) + bytes_get(curr, ym + xp) +
                bytes_get(curr, yw + xm) +                             bytes_get(curr, yw + xp) +
                bytes_get(curr, yp + xm) + bytes_get(curr, yp + x) + bytes_get(curr, yp + xp)
            def cur = bytes_get(curr, idx)
            mut live = DEAD
            if (cur == ALIVE) {
                if (n == 2 || n == 3) { live = ALIVE }
            } else {
                if (n == 3) { live = ALIVE }
            }
            bytes_set(next, idx, live)
            if (live == ALIVE) { alive += 1 }
            if (live != cur) {
                def sx = x * 2
                if (live == ALIVE) {
                    set_pair(sx, y, CH, CH, LEN_CH, LEN_CH, COLOR_GREEN)
                } else {
                    set_pair(sx, y, SP, SP, LEN_SP, LEN_SP, COLOR_BG)
                }
            }
            x += 1
        }
        y += 1
    }
    alive
}

clear_screen()
cursor_hide()
seed(ticks())

def total = LW * H
mut g = bytes(total)
mut g2 = bytes(total)
seed_grid(g, total)
draw_full(g, LW, H)
canvas_refresh(canv)

while (1) {
    def alive = step(g, g2, LW, H)
    if (alive == 0) {
        seed_grid(g2, total)
        draw_full(g2, LW, H)
    }
    canvas_refresh(canv)
    def tmp = g
    g = g2
    g2 = tmp
}

screen_reset()
