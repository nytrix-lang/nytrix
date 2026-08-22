;; Keywords: browser networking fetch asyncify fixture
;; Verify that browser fetch returns same-origin text after an async resume.
use std.os as os
use std.os.ui.window as window

def body = os.fetch("index.html")
def ok = is_str(body) && body.len > 100
window.test_report_touch(ok ? 1 : 0, ok ? 1 : 0, ok ? 1 : 0)
ok ? 1 : 0
