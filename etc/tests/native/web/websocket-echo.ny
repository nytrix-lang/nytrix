;; Keywords: browser networking websocket asyncify fixture
;; Verify browser WebSocket open, send, receive, and close behavior.
use std.os.websocket as websocket
use std.os.time as time
use std.os.ui.window as window

def handle = websocket.open("ws://127.0.0.1:18790")
time.msleep(1000)
def sent = handle > 0 && websocket.send(handle, "nytrix-echo")
time.msleep(1000)
def received = websocket.receive(handle)
def ok = sent && contains(received, "nytrix-echo")
window.test_report_touch(handle, sent ? 1 : 0, ok ? 1 : 0)

if handle > 0 { websocket.close(handle) }
ok ? 1 : 0
