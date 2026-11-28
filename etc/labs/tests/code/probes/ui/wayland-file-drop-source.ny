;; Small visible source client for wayland-file-drop.nshape. A separate client
;; lets the target keep dispatching its wl_data_offer.receive pipe while this
;; process services wl_data_source.send on its own Wayland connection.
use core
use core.common as common
use core.os (file_write, msleep, now_ms)
use core.os.ui.window as window
use core.os.ui.window.consts as event_kind
use core.os.ui.render as gfx

def str path = common.env_trim("NY_WAYLAND_DROP_FILE")
def str ready = common.env_trim("NY_WAYLAND_DROP_READY")
if path.len == 0 || ready.len == 0 { panic("Wayland drop source needs file and ready paths") }
def str uri = "file://" + path + "\r\n"
gfx.apply_backend_name("vk")
def win = gfx.init_window(480, 300, "Wayland drop source", window.WINDOW_ALLOW_DND, false, true, 1)
if !win || window.backend() != "wayland" || gfx.get_backend_type() == gfx.BACKEND_MOCK {
   if win { gfx.close_window() }
   panic("Wayland drop source needs a Vulkan surface")
}
mut int frame = 0
while frame < 4 {
   if gfx.begin_frame_clear([0.08, 0.08, 0.08, 1.0]) { gfx.end_frame() }
   frame += 1
}
file_write(ready, "ready")
mut bool started = false
mut bool released = false
def int end = now_ms() + 30000
while !released && now_ms() < end {
   if gfx.begin_frame_clear([0.08, 0.08, 0.08, 1.0]) { gfx.end_frame() }
   window.poll_events()
   mut any e = window.check_event(win)
   while !is_nil(e) && !(is_int(e) && int(e) == 0) {
      def int typ = window.event_type(e)
      if typ == event_kind.EVENT_MOUSE_BUTTON_PRESSED && !started {
         started = window.start_file_drag(win, uri)
         if !started { panic("Wayland start_file_drag rejected the pointer serial") }
      } elif typ == event_kind.EVENT_MOUSE_BUTTON_RELEASED && started {
         released = true
      }
      e = window.check_event(win)
   }
   msleep(16)
}
gfx.close_window()
if !started { panic("Wayland source received no pointer press") }
