;; Keywords: progress progressbar core
;; Progress-bar state and terminal progress rendering operations.
;; References:
;; - std.core
module std.core.progress(progress, progress_each, update, advance, finish, note, current, total, percent, reset)
use std.core
use std.core.term as term

fn _progress_total(any target) int {
   if is_int(target) { return target }
   target.len
}

fn _progress_value(any target, int i) any {
   if is_int(target) { return i }
   target.get(i)
}

fn _progress_bar(any target, str desc, int width, str color) list { term.bar(_progress_total(target), desc, width, color, 1, 1) }

fn _progress_run(any target, str desc, int width, str color, any body) any {
   def p, n = _progress_bar(target, desc, width, color), total(p)
   mut out = list(n)
   term.bar_update(p, 0)
   mut i = 0
   while i < n {
      out = out.append(body(_progress_value(target, i)))
      term.bar_update(p, i + 1)
      i += 1
   }
   term.bar_finish(p)
   out
}

fn _progress_from_args(any target, any label, list args) any {
   mut desc = "Progress"
   mut width = 40
   mut color = "green"
   mut body = nil
   if is_str(label) { desc = label }
   elif label != nil { body = label }
   mut i = 0
   while i < args.len {
      def arg = args.get(i)
      if is_int(arg) { width = arg }
      elif is_str(arg) { color = arg }
      else { body = arg }
      i += 1
   }
   if body != nil { return _progress_run(target, desc, width, color, body) }
   _progress_bar(target, desc, width, color)
}

fn progress(any target=100, any label="", ...args) any {
   "Creates a progress bar, or runs `body(value)` over a range/sequence with progress.
   Examples:
   - `def p = progress(100, \"build\")`
   - `progress(range(100), \"build\", fn(i){ ... })`
   - `progress(1..100, \"build\", fn(i){ ... })`
   "
   _progress_from_args(target, label, args)
}

fn progress_each(any target, any body, str desc="", int width=40, str color="green") any {
   "Runs `body(value)` for each item in `target`, updating a progress bar."
   _progress_run(target, desc, width, color, body)
}

fn update(list p, int value) int {
   "Sets the current progress value."
   term.bar_update(p, value)
}

fn advance(list p, int step=1) int {
   "Advances the progress bar by `step`."
   term.bar_update(p, current(p) + step)
}

fn finish(list p) int {
   "Completes and closes the progress bar."
   term.bar_finish(p)
   0
}

fn note(list p, str msg) int {
   "Prints a message without losing the current progress line."
   term.bar_write(p, msg)
   0
}

fn current(list p) int {
   "Returns the current progress value."
   p.get(1, 0)
}

fn total(list p) int {
   "Returns the progress total."
   p.get(0, 0)
}

fn percent(list p) int {
   "Returns integer completion percent."
   def t = total(p)
   if t <= 0 { return 100 }
   def c = current(p)
   if c <= 0 { return 0 }
   if c >= t { return 100 }
   (c * 100) / t
}

fn reset(list p, int n=-1) int {
   "Resets progress state and optionally changes the total."
   if n >= 0 { p.set(0, n) }
   p.set(1, 0)
   p.set(8, p.get(7, 0))
   p.set(9, 0)
   p.set(10, 0)
   p.set(11, 0.0)
   0
}

#main {
   def p = [4, 0, "self", 8, "green", 1, 1, 0, 0, 0, 0, 0.0]
   assert(total(p) == 4 && current(p) == 0 && percent(p) == 0, "progress initial state")
   p.set(1, 2)
   assert(current(p) == 2 && percent(p) == 50, "progress percent mid")
   p.set(1, 4)
   assert(percent(p) == 100, "progress percent complete")
   reset(p, 2)
   assert(total(p) == 2 && current(p) == 0 && percent(p) == 0, "progress reset")
   assert(_progress_total([10, 20, 30]) == 3 && _progress_value([10, 20, 30], 1) == 20, "progress sequence helpers")
   assert(_progress_total(3) == 3 && _progress_value(3, 2) == 2, "progress integer helpers")
   print("✓ std.core.progress self-test passed")
}
