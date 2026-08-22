;; Keywords: browser networking websocket rejection fixture
;; Invalid WebSocket schemes must fail synchronously and never become handles.
use std.os.websocket as websocket
use std.os.time as time
use std.os.ui.render as gfx
use std.os.ui.window as window

def wall = time.now_ms()
def handle = websocket.open("http://127.0.0.1:9")
def sent = handle > 0 && websocket.send(handle, "must-not-send")
window.test_report_touch(wall, handle, sent ? wall : handle)
def win = gfx.init_window(320, 180, "Nytrix WebSocket rejection", 0, true, false, 1)
gfx.begin_frame_clear(gfx.BLACK)
gfx.end_frame()

if handle > 0 { websocket.close(handle) }
handle < 1 && !sent ? 1 : 0
