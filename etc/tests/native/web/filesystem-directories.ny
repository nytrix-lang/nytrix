;; Keywords: browser web filesystem directory mkdir rmdir persistence fixture
;; Verify that empty virtual directories can be created, listed, and removed.
use std.os.fs as fs
use std.os.time as time
use std.os.ui.render as gfx
use std.os.ui.window as window

def name = "nytrix-web-dir-" + to_str(time.now_ms())
def path = name
def make_result = fs.make_dir(path)
def made = fs.is_dir(path)
def entries = fs.list_dir(".")
mut listed = false
mut entry_i = 0
while entry_i < entries.len {
   if entries.get(entry_i) == name { listed = true }
   entry_i += 1
}

def remove_result = fs.remove_dir(path)
def removed = !fs.is_dir(path)
window.test_report_touch(made ? 1 : 0, listed ? 1 : 0, removed ? 1 : 0)
def win = gfx.init_window(320, 180, "Nytrix browser directories", 0, true, false, 1)
gfx.begin_frame_clear(gfx.BLACK)
gfx.draw_rect(24.0, 24.0, 272.0, 132.0, gfx.WHITE)
gfx.end_frame()
