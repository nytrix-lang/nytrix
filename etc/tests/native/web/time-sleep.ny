;; Keywords: browser time asyncify fixture
;; Verify that browser msleep suspends and resumes the same Wasm stack.
use std.os.time as time
use std.os.ui.window as window

def before = time.now_ms()
time.msleep(50)
def after = time.now_ms()
def elapsed = after - before
window.test_report_touch(elapsed, elapsed, elapsed)
elapsed
