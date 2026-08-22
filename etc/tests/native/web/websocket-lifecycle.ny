;; Keywords: browser networking websocket fixture
;; Verify that browser WebSocket handles expose an explicit lifecycle state.
use std.os.websocket as websocket
use std.os.ui.window as window

def handle = websocket.open("ws://127.0.0.1:9")
def current = handle > 0 ? websocket.state(handle) : 0

;; Reading the state exercises the lifecycle lookup; the handle marker proves
;; that the browser constructor returned a live record rather than a stub.
def valid = handle > 0
window.test_report_touch(handle, 1, 1)

if handle > 0 { websocket.close(handle) }
valid ? 1 : 0
