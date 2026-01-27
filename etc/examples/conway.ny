use std.io
use std.os.time
use std.math.random
use std.cli.tui

;; Conway's Game of Life (Example)

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
        def y = 0
        while y < H {
            def yw = y * W
            def ym = (y == 0 ? H - 1 : y - 1) * W
            def yp = (y == H - 1 ? 0 : y + 1) * W

            def x = 0
            while x < W {
                def xm = (x == 0 ? W - 1 : x - 1)
                def xp = (x == W - 1 ? 0 : x + 1)

                ;; 8 Neighbors
                def n = bytes_get(g, ym+xm) + bytes_get(g, ym+x) + bytes_get(g, ym+xp) +
                        bytes_get(g, yw+xm)                      + bytes_get(g, yw+xp) +
                        bytes_get(g, yp+xm) + bytes_get(g, yp+x) + bytes_get(g, yp+xp)

                def idx = yw + x
                def cur = bytes_get(g, idx)
                def live = (cur == 1 ? (n == 2 || n == 3) : (n == 3)) ? 1 : 0
                bytes_set(ng, idx, live)

                if live {
                    canvas_set(canv, x, y, 35, 2, 1) ;; '#' green bold
                }
                x = x + 1
            }
            y = y + 1
        }
        canvas_refresh(canv)
        def tmp = g g = ng ng = tmp
        msleep(20)
    }
} catch e {
    screen_reset()
}
