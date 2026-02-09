#!/bin/ny
;; Langton's Ant - https://en.wikipedia.org/wiki/Langton%27s_ant

use std.core *
use std.str.term *

def CELL_WHITE = 0
def CELL_BLACK = 1
def CH_WHITE = " "
def CH_BLACK = "\xe2\x96\x88"
def CH_TRAIL = "\xe2\x96\x91"
def LEN_WHITE = 1
def LEN_BLACK = 3
def LEN_TRAIL = 3
def COLOR_WHITE = 7
def COLOR_GRAY = 8
def COLOR_BLUE = 4
def COLOR_BG = 0

def t = get_terminal_size()
mut W = get(t, 0, 0)
mut H = get(t, 1, 0)
if (W < 2) { W = 80 }
if (H < 1) { H = 24 }
if (W % 2 == 1) { W = W - 1 }

def LW = W / 2
def canv = canvas(W, H)
def cbuf = get(canv, 2)
def attr = get(canv, 3)
def col  = get(canv, 4)
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

clear_screen()
cursor_hide()

mut grid = bytes(LW * H)
mut visited = bytes(LW * H)

mut x = LW / 2
mut y = H / 2

def DIR_UP = 0
def DIR_RIGHT = 1
def DIR_DOWN = 2
def DIR_LEFT = 3
mut dir = DIR_UP

set_pair(x * 2, y, CH_BLACK, CH_BLACK, LEN_BLACK, LEN_BLACK, COLOR_BLUE)
bytes_set(visited, y * LW + x, 1)
canvas_refresh(canv)

while (1) {
    def prev_x = x
    def prev_y = y
    def k = y * LW + x
    def v = bytes_get(grid, k)
    if (v) {
        bytes_set(grid, k, CELL_WHITE)
        dir = (dir + 3) % 4
    } else {
        bytes_set(grid, k, CELL_BLACK)
        dir = (dir + 1) % 4
    }
    bytes_set(visited, k, 1)

    if (dir == DIR_UP) {
        y -= 1
        if (y < 0) { y = H - 1 }
    } elif (dir == DIR_RIGHT) {
        x += 1
        if (x >= LW) { x = 0 }
    } elif (dir == DIR_DOWN) {
        y += 1
        if (y >= H) { y = 0 }
    } else {
        x -= 1
        if (x < 0) { x = LW - 1 }
    }

    def prev_idx = prev_y * LW + prev_x
    def prev_sx = prev_x * 2
    def prev_state = bytes_get(grid, prev_idx)
    if (prev_state == CELL_BLACK) {
        set_pair(prev_sx, prev_y, CH_BLACK, CH_BLACK, LEN_BLACK, LEN_BLACK, COLOR_WHITE)
    } elif (bytes_get(visited, prev_idx)) {
        set_pair(prev_sx, prev_y, CH_TRAIL, CH_TRAIL, LEN_TRAIL, LEN_TRAIL, COLOR_GRAY)
    } else {
        set_pair(prev_sx, prev_y, CH_WHITE, CH_WHITE, LEN_WHITE, LEN_WHITE, COLOR_BG)
    }

    def sx = x * 2
    bytes_set(visited, y * LW + x, 1)
    set_pair(sx, y, CH_BLACK, CH_BLACK, LEN_BLACK, LEN_BLACK, COLOR_BLUE)

    canvas_refresh(canv)
}

screen_reset()
cursor_show()
