;; Keywords: util progress
;; Util Progress module.

use std.cli.tui
use std.core.reflect
module std.util.progress (
   progress, progress_update, progress_finish, progress_range, progress_map
)

fn progress(total, desc=""){
   "Create a progress bar and return a bar object."
   return bar(total, desc)
}

fn progress_update(bar_obj, current){
   "Update progress bar to current value."
   return bar_update(bar_obj, current)
}

fn progress_finish(bar_obj){
   "Finish progress bar and render final state."
   return bar_finish(bar_obj)
}

fn progress_range(n, desc=""){
   "Create a progress bar for a range of n items."
   return bar_range(n, desc)
}

fn progress_map(f, xs, desc="Processing"){
   "Apply function f to each element of xs while showing a progress bar."
   def n = len(xs)
   def b = bar(n, desc)
   def res = list(8)
   def i = 0
   while(i < n){
      res = append(res, f(get(xs, i)))
      bar_update(b, i + 1)
      i = i + 1
   }
   bar_finish(b)
   return res
}