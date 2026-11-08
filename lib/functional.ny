;; Keywords: functions curry uncurry partial application closures
;; Explicit binary function adapters. Automatic partial application does not require this module.
;; References:
;; - core
module core.functional(curry, uncurry)
use core

fn curry(fnptr f) fnptr {
   "Converts a binary callable into two successive unary calls. Captures the callable once; the first unary call captures its argument without invoking it."
   return fn(a) {
      return fn(b) { f(a, b) }
   }
}

fn uncurry(fnptr f) fnptr {
   "Converts two successive unary calls into one binary call. Arguments are evaluated before either unary call runs."
   return fn(a, b) { f(a)(b) }
}

#main {
   def add = fn(a, b) { a + b }
   def curried = curry(add)
   assert(curried(20)(22) == 42, "curry binary callable")
   def binary = uncurry(curried)
   assert(binary(19, 23) == 42, "uncurry successive unary calls")
}
