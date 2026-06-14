;; Keywords: cps continuation-passing-style core
;; Continuation-Passing Style.
;; References:
;; - std.core
module std.core.cps(cps_return, cps_run, cps_transform_unary, cps_transform_binary, cps_map, cps_bind, cps_apply, cps_lift2, cps_pipe, cps_step_done, cps_step_more, cps_is_done, cps_step_value, cps_trampoline)
use std.core
use std.core.error

fn cps_return(any v) fnptr {
   "Wraps a plain value `v` into a CPS computation that immediately passes it to the continuation."
   return fn(k) {
      k(v)
   }
}

fn cps_run(fnptr c, ?fnptr k=nil) any {
   "Runs a CPS computation `c`. If no continuation `k` is provided, the computation's final value is returned directly."
   if !k {
      return c(fn(v) {
            v
      })
   }
   c(k)
}

fn cps_transform_unary(fnptr f) fnptr {
   "Wraps a standard unary function `f(x)` to behave as a CPS function `f(x, k)`."
   return fn(x, k) {
      k(f(x))
   }
}

fn cps_transform_binary(fnptr f) fnptr {
   "Wraps a binary function `f(a, b)` to behave as a CPS function `f(a, b, k)`."
   return fn(a, b, k) {
      k(f(a, b))
   }
}

fn cps_map(fnptr c, fnptr f) fnptr {
   "Applies a transformation function `f` to the result of a CPS computation `c`."
   return fn(k) {
      c(fn(v) {
            k(f(v))
      })
   }
}

fn cps_bind(fnptr c, fnptr f) fnptr {
   "Chains two CPS computations together. Passes the result of `c` to
   function `f`, which must return a new CPS computation."
   return fn(k) {
      c(fn(v) {
            def next = f(v)
            next(k)
      })
   }
}

fn cps_apply(fnptr cf, fnptr ca) fnptr {
   "Applies a CPS computation containing a function `cf` to a CPS computation containing an argument `ca`."
   cps_bind(cf, fn(f) {
         cps_map(ca, f)
   })
}

fn cps_lift2(fnptr f, fnptr ca, fnptr cb) fnptr {
   "Lifts a standard binary function `f` to operate on the results of two CPS computations `ca` and `cb`."
   cps_bind(ca, fn(a) {
         cps_map(cb, fn(b) {
               f(a, b)
         })
   })
}

fn cps_pipe(any v, list funcs) fnptr {
   "Threads an initial value `v` through a sequence of unary CPS functions in `funcs`."
   mut c, i = cps_return(v), 0
   def n = funcs.len
   while i < n {
      def f = funcs.get(i)
      c = cps_bind(c, f)
      i += 1
   }
   c
}

fn cps_step_done(any v) list {
   "Creates a terminal trampoline step containing the final result `v`."
   return [0, v]
}

fn cps_step_more(fnptr thunk) list {
   "Creates a continuation trampoline step that will execute `thunk()` to obtain the next step."
   return [1, thunk]
}

fn cps_is_done(any step) bool {
   "Returns true if the given trampoline `step` is a terminal step."
   if !is_list(step) || step.len < 2 { return true }
   step.get(0, 0) == 0
}

fn cps_step_value(any step) any {
   "Extracts the result value from a terminal step or the thunk from a continuation step."
   if !is_list(step) || step.len < 2 { return step }
   step.get(1, 0)
}

fn cps_trampoline(any step, int max_steps=0) any {
   "Executes trampoline steps iteratively until a terminal step is reached. Prevents stack overflow for deep recursion."
   mut cur = step
   mut iters = 0
   while true {
      if cps_is_done(cur) { return cps_step_value(cur) }
      if max_steps > 0 && iters >= max_steps { panic("cps_trampoline exceeded max_steps") }
      def th = cps_step_value(cur)
      cur = th()
      iters += 1
   }
}

#main {
   fn _selftest_value(any k) any { k(10) }
   fn _selftest_inc(any v) any { v + 1 }
   fn _selftest_more() list { cps_step_done(42) }
   assert(cps_run(_selftest_value) == 10, "cps_run default continuation")
   assert(cps_run(_selftest_value, _selftest_inc) == 11, "cps_run explicit continuation")
   def done = cps_step_done(42)
   assert(cps_is_done(done), "cps_step_done")
   assert(cps_step_value(done) == 42, "cps_step_value")
   assert(cps_trampoline(cps_step_more(_selftest_more)) == 42, "cps_trampoline")
   print("✓ std.core.cps self-test passed")
}
