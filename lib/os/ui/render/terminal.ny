;; Keywords: render terminal ansi framebuffer tty
;; True-color terminal output for CPU-rendered RGBA frames.
module core.os.ui.render.terminal(init, frame_to_ansi, present)
use core
use core.str as str
use core.term (write_str)
use core.os.ui.render as gfx
use core.os.ui.render.tty as render_tty

fn init(int width=320, int height=180) any {
   "Creates a CPU render surface without opening a desktop window."
   gfx.set_backend_type(gfx.BACKEND_MOCK)
   gfx.init_window(width, height, "Nytrix terminal")
}

fn frame_to_ansi(dict frame, int columns=80, int rows=24) str {
   "Samples an RGBA8 frame into true-color terminal cells; does not own frame memory."
   render_tty.frame_to_ansi(frame, columns, rows)
}

fn _frame_free(any frame) bool {
   if !is_dict(frame) { return false }
   def ptr data = __ffi_untag_ptr(frame.get("data", 0))
   if data { free(data) }
   true
}

fn present(int columns=80, int rows=24) bool {
   "Writes the current CPU framebuffer to stdout and frees its readback copy."
   def frame = gfx.read_framebuffer()
   if !is_dict(frame) { return false }
   defer { _frame_free(frame) }
   def output = frame_to_ansi(frame, columns, rows)
   if output.len <= 0 { return false }
   write_str(output)
   true
}
