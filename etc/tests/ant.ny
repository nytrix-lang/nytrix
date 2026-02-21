#!/bin/ny
;; Langton's Ant - https://en.wikipedia.org/wiki/Langton%27s_ant

use std.core *
use std.core.dict *
use std.os.args *
use std.str.term *

;; Constants
def CELL_WHITE  = 0
def CELL_BLACK  = 1
def CH_WHITE    = " "
def CH_BLACK    = "\xe2\x96\x88"
def CH_TRAIL    = "\xe2\x96\x91"

def COLOR_WHITE = 7
def COLOR_GRAY  = 8
def COLOR_BLUE  = 4
def COLOR_BG    = 0

def DIR_UP      = 0
def DIR_RIGHT   = 1
def DIR_DOWN    = 2
def DIR_LEFT    = 3

def H_U_L = "\xe2\x96\x84"
def H_U_R = "\xe2\x96\x88"
def H_D_L = "\xe2\x96\x88"
def H_D_R = "\xe2\x96\x80"
def H_L_L = "\xe2\x96\x84"
def H_L_R = "\xe2\x96\x88"
def H_R_L = "\xe2\x96\x88"
def H_R_R = "\xe2\x96\x84"

;; State Init
def t = get_terminal_size()
mut W = get(t, 0, 0)
mut H = get(t, 1, 0)
if(W < 2){ W = 80 }
if(H < 1){ H = 24 }
if(W % 2 == 1){ W -= 1 }

def LW   = W / 2
def CANV = canvas(W, H)

mut black   = dict(1024)
mut visited = dict(1024)
mut x       = 0
mut y       = 0
mut dir     = DIR_UP
mut max_steps = 0
mut step_count = 0

def av = args()
mut ai = 0
while(ai < len(av)){
    def n = atoi(get(av, ai, ""))
    if(n > 0){
        max_steps = n
        break
    }
    ai += 1
}

fn set_pair(x, y, left, right, color_idx){
    canvas_set(CANV, x, y, left, color_idx, 0)
    canvas_set(CANV, x + 1, y, right, color_idx, 0)
}

fn key_of(px, py){
    f"{px}:{py}"
}

fn render_world(){
    canvas_clear(CANV)

    def cx = LW / 2
    def cy = H / 2
    def min_x = x - cx
    def min_y = y - cy

    mut sy = 0
    while(sy < H){
        mut sx = 0
        while(sx < LW){
            def wx = min_x + sx
            def wy = min_y + sy
            def k = key_of(wx, wy)
            def px = sx * 2

            if(dict_has(black, k)){
                set_pair(px, sy, CH_BLACK, CH_BLACK, COLOR_WHITE)
            } elif(dict_has(visited, k)){
                set_pair(px, sy, CH_TRAIL, CH_TRAIL, COLOR_GRAY)
            }
            sx += 1
        }
        sy += 1
    }

    def hx = (LW / 2) * 2
    def hy = H / 2
    mut hl = H_U_L
    mut hr = H_U_R
    if(dir == DIR_DOWN){
        hl = H_D_L
        hr = H_D_R
    } elif(dir == DIR_LEFT){
        hl = H_L_L
        hr = H_L_R
    } elif(dir == DIR_RIGHT){
        hl = H_R_L
        hr = H_R_R
    }
    set_pair(hx, hy, hl, hr, COLOR_BLUE)
}

;; Main Line
tui_begin()
defer { tui_end() }

visited = dict_set(visited, key_of(x, y), 1)
render_world()
canvas_refresh(CANV)

while(1){
    def key = poll_key()
    if(is_quit_key(key)){ break }

    def k = key_of(x, y)
    if(dict_has(black, k)){
        black = dict_del(black, k)
        dir = (dir + 3) % 4
    } else {
        black = dict_set(black, k, 1)
        dir = (dir + 1) % 4
    }
    visited = dict_set(visited, k, 1)

    if(dir == DIR_UP){
        y -= 1
    } elif(dir == DIR_RIGHT){
        x += 1
    } elif(dir == DIR_DOWN){
        y += 1
    } else {
        x -= 1
    }

    visited = dict_set(visited, key_of(x, y), 1)
    render_world()
    canvas_refresh(CANV)
    step_count += 1
    if(max_steps > 0 && step_count >= max_steps){ break }
}
