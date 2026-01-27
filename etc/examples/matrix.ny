use std.io
use std.os.time
use std.math.random
use std.cli.tui

;; Matrix Rain (Example)

def t = get_terminal_size()
def [W, H] = t
if W <= 0 { W = 80 }
if H <= 0 { H = 24 }

def canv = canvas(W, H)
def intensities, chars = bytes(W * H), bytes(W * H)
def drop_y, drop_speed, drop_tail = list(W), list(W), list(W)

for i in range(W) {
    drop_y = append(drop_y, -(rand() % (H * 20)))
    drop_speed = append(drop_speed, 15 + rand() % 35)
    drop_tail = append(drop_tail, 8 + rand() % 15)
}

clear_screen()
cursor_hide()

try {
    while 1 {
        ;; Update
        for x in range(W) {
            def yf = get(drop_y, x) + get(drop_speed, x)
            set_idx(drop_y, x, yf)
            def y = yf / 20

            if y >= 0 && y < H {
                def idx = y * W + x
                bytes_set(chars, idx, 33 + rand() % 94)
                bytes_set(intensities, idx, 32)
            }

            if y - get(drop_tail, x) >= H {
                set_idx(drop_y, x, -(rand() % 40))
            }
        }

        ;; Draw to Canvas
        for i in range(W * H) {
            def intensity = bytes_get(intensities, i)
            if intensity > 0 {
                def char = bytes_get(chars, i)
                if rand() % 60 == 0 { char = 33 + rand() % 94 }

                def color = 2 ;; Green
                def bold = 0
                if intensity > 28 { color = 7 bold = 1 }
                elif intensity > 12 { color = 2 bold = 1 }
                elif intensity > 4 { color = 2 bold = 0 }
                else { color = 8 bold = 0 }

                canvas_set(canv, i % W, i / W, char, color, bold)
                bytes_set(intensities, i, intensity - 1)
            } else {
                canvas_set(canv, i % W, i / W, 32, 0, 0)
            }
        }

        canvas_refresh(canv)
        msleep(10)
    }
} catch e {
    screen_reset()
}
