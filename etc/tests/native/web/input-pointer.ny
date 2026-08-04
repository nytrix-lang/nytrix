;; Keywords: browser input mouse pointer portability fixture
;; Browser mouse position and button state must use the public input facade.
use std.os.ui.render as gfx
use std.os.ui.window.input as input
use std.os.ui.window as window

def win = gfx.init_window(320, 180, "Nytrix browser input", 0, true, false, 1)
def pos = input.mouse_pos()
def held = input.mouse_button_down(input.MOUSE_LEFT)
def edge = input.mouse_button_pressed(input.MOUSE_LEFT)
def scroll = window.scroll_pos(win)
gfx.begin_frame_clear(gfx.BLACK)
gfx.draw_rect(pos.get(0, 0.0) + scroll.get(0, 0.0), pos.get(1, 0.0) + scroll.get(1, 0.0), held ? 8.0 : 4.0, edge ? 8.0 : 4.0, gfx.WHITE)
gfx.end_frame()
