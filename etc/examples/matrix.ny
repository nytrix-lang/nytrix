#!/bin/ny
;; Matrix Rain (Example) - https://en.wikipedia.org/wiki/Digital_rain

use std.core *
use std.math.random *
use std.os.time *
use std.str.bytes *
use std.str.term *

def sz = get_terminal_size()
def W  = get(sz, 0, 0)
def H  = get(sz, 1, 0)

def canv        = canvas(W, H)
def intensities = bytes(W * H)
def chars       = bytes(W * H)

mut drop_y     = list(W)
mut drop_speed = list(W)

mut i = 0
while (i < W) {
    drop_y     = append(drop_y, -(rand() % (H * 20)))
    drop_speed = append(drop_speed, 15 + (rand() % 35))
    i += 1
}

cursor_hide()
clear_screen()

while (1) {
    mut x = 0
    while (x < W) {
        def yf = get(drop_y, x, 0) + get(drop_speed, x, 0)
        set_idx(drop_y, x, yf)

        def y = yf / 20
        if (y >= 0 && y < H) {
            def idx = y * W + x
            bytes_set(chars, idx, 33 + (rand() % 94))
            bytes_set(intensities, idx, 31)
        }

        if (y > H + 10) {
            set_idx(drop_y, x, 0)
        }

        x += 1
    }

    mut idx = 0
    def total = W * H
    while (idx < total) {
        def intensity = bytes_get(intensities, idx)
        def cur_x = idx % W
        def cur_y = idx / W

        if (intensity > 0) {
            def char = bytes_get(chars, idx)
            mut color = 2
            mut bold  = 0

            if (intensity > 28) {
                color = 7
                bold  = 1
            } elif (intensity > 15) {
                bold = 1
            }

            canvas_set(canv, cur_x, cur_y, char, color, bold)
            bytes_set(intensities, idx, intensity - 1)
        } else {
            canvas_set(canv, cur_x, cur_y, 32, 0, 0)
        }

        idx += 1
    }

    canvas_refresh(canv)
    msleep(20)
}

screen_reset()
cursor_show()
