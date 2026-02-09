use std.core *
use std.core.dict *
use std.str.term *
use std.str *

;; std.str.term (Test)

def sz = get_terminal_size()
assert(is_list(sz), "terminal size list")
assert(len(sz) == 2, "terminal size len")

def w = get(sz, 0)
def h = get(sz, 1)
assert(w > 0, "terminal width")
assert(h > 0, "terminal height")

def names = color_names()
assert(len(names) >= 5, "color_names")
assert(is_str(get_color_name(0)), "get_color_name")

def colored = get_color("x", 1)
assert(str_len(colored) > 1, "get_color")

def shp = shapes()
assert(is_dict(shp), "shapes dict")
assert(str_len(dict_get(shp, "h_line", "")) > 0, "shape h_line")

def c = canvas(4, 2)
canvas_set(c, 0, 0, 65)
canvas_print(c, 1, 0, "B")
canvas_clear(c)

print("✓ std.str.term tests passed")
