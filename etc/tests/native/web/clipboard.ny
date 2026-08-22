;; Keywords: browser clipboard asyncify permission fixture
;; Verify that the public std.os clipboard facade reaches the browser bridge.
use std.os as os
use std.os.ui.window as window

def marker = "nytrix-clipboard-fixture"
def wrote = os.set_clipboard_text(marker)
def read = os.get_clipboard_text()
def read_ok = read == marker
def ok = wrote && read_ok

;; The browser host exposes the exact write/read evidence as data attributes;
;; this constant marker only proves execution reached the end of the fixture.
window.test_report_touch(wrote ? 1 : 0, 1, 1)
ok ? 1 : 0
