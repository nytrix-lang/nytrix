;; Keywords: core cps continuation
;; Continuation-Passing Style.

module std.core.cps (
   cps_return, cps_run,
   cps_transform_unary, cps_transform_binary,
   cps_map, cps_bind, cps_apply, cps_lift2, cps_pipe,
   cps_step_done, cps_step_more, cps_is_done, cps_step_value, cps_trampoline
)
use std.core *
use std.core.error *

fn cps_return(v){
   "Wraps a plain value `v` into a CPS computation that immediately passes it to the continuation."
   return fn(k){
      "Auto-generated docstring: anonymous function."
      k(v)
   }
}

fn cps_run(c, k=0){
   "Runs a CPS computation `c`. If no continuation `k` is provided, the computation's final value is returned directly."
   if(k == 0){
      return c(fn(v){
         "Auto-generated docstring: anonymous function."
         v
      })
   }
   c(k)
}

fn cps_transform_unary(f){
   "Wraps a standard unary function `f(x)` to behave as a CPS function `f(x, k)`."
   return fn(x, k){
      "Auto-generated docstring: anonymous function."
      k(f(x))
   }
}

fn cps_transform_binary(f){
   "Wraps a binary function `f(a, b)` to behave as a CPS function `f(a, b, k)`."
   return fn(a, b, k){
      "Auto-generated docstring: anonymous function."
      k(f(a, b))
   }
}

fn cps_map(c, f){
   "Applies a transformation function `f` to the result of a CPS computation `c`."
   return fn(k){
      "Auto-generated docstring: anonymous function."
      c(fn(v){
         "Auto-generated docstring: anonymous function."
         k(f(v))
      })
   }
}

fn cps_bind(c, f){
   "Chains two CPS computations together. Passes the result of `c` to function `f`, which must return a new CPS computation."
   return fn(k){
      "Auto-generated docstring: anonymous function."
      c(fn(v){
         "Auto-generated docstring: anonymous function."
         def next = f(v)
         next(k)
      })
   }
}

fn cps_apply(cf, ca){
   "Applies a CPS computation containing a function `cf` to a CPS computation containing an argument `ca`."
   cps_bind(cf, fn(f){
      "Auto-generated docstring: anonymous function."
      cps_map(ca, f)
   })
}

fn cps_lift2(f, ca, cb){
   "Lifts a standard binary function `f` to operate on the results of two CPS computations `ca` and `cb`."
   cps_bind(ca, fn(a){
      "Auto-generated docstring: anonymous function."
      cps_map(cb, fn(b){
         "Auto-generated docstring: anonymous function."
         f(a, b)
      })
   })
}

fn cps_pipe(v, funcs){
   "Threads an initial value `v` through a sequence of unary CPS functions in `funcs`."
   mut c = cps_return(v)
   mut i = 0
   def n = len(funcs)
   while(i < n){
      def f = get(funcs, i)
      c = cps_bind(c, f)
      i += 1
   }
   c
}

fn cps_step_done(v){
   "Creates a terminal trampoline step containing the final result `v`."
   return [0, v]
}

fn cps_step_more(thunk){
   "Creates a continuation trampoline step that will execute `thunk()` to obtain the next step."
   return [1, thunk]
}

fn cps_is_done(step){
   "Returns true if the given trampoline `step` is a terminal step."
   if(!is_list(step) || len(step) < 2){ return true }
   get(step, 0, 0) == 0
}

fn cps_step_value(step){
   "Extracts the result value from a terminal step or the thunk from a continuation step."
   if(!is_list(step) || len(step) < 2){ return step }
   get(step, 1, 0)
}

fn cps_trampoline(step, max_steps=0){
   "Executes trampoline steps iteratively until a terminal step is reached. Prevents stack overflow for deep recursion."
   mut cur = step
   mut iters = 0
   while(true){
      if(cps_is_done(cur)){ return cps_step_value(cur) }
      if(max_steps > 0 && iters >= max_steps){
         panic("cps_trampoline exceeded max_steps")
      }
      def th = cps_step_value(cur)
      cur = th()
      iters += 1
   }
}

if(comptime{__main()}){
   use std.core *
   use std.core.error *
   use std.core.cps *
   use std.os.mod
   use std.str.mod

   print("Testing std.core.cps...")

   fn inc(x){
       "Test helper."
       x + 1
   }
   fn mul(a, b){
       "Test helper."
       a * b
   }

   ; direct -> CPS transforms
   def inc_cps = cps_transform_unary(inc)
   def mul_cps = cps_transform_binary(mul)

   assert(inc_cps(41, fn(v){
       "Auto-generated docstring: anonymous function."
       v
   }) == 42, "cps_transform_unary")
   assert(mul_cps(6, 7, fn(v){
       "Auto-generated docstring: anonymous function."
       v
   }) == 42, "cps_transform_binary")

   ; return/map/bind/run
   mut c = cps_return(10)
   c = cps_map(c, fn(v){
       "Auto-generated docstring: anonymous function."
       v + 2
   })
   c = cps_bind(c, fn(v){
       "Auto-generated docstring: anonymous function."
       cps_return(v * 3)
   })
   assert(cps_run(c) == 36, "cps_return/map/bind/run")

   ; apply/lift2
   def cf = cps_return(fn(v){
       "Auto-generated docstring: anonymous function."
       v * 2
   })
   def cv = cps_return(21)
   assert(cps_run(cps_apply(cf, cv)) == 42, "cps_apply")

   def ca = cps_return(20)
   def cb = cps_return(22)
   def csum = cps_lift2(fn(a, b){
       "Auto-generated docstring: anonymous function."
       a + b
   }, ca, cb)
   assert(cps_run(csum) == 42, "cps_lift2")

   ; pipe
   fn add1_c(x){
       "Test helper."
       cps_return(x + 1)
   }
   fn mul2_c(x){
       "Test helper."
       cps_return(x * 2)
   }
   def piped = cps_pipe(10, [add1_c, mul2_c, add1_c])
   assert(cps_run(piped) == 23, "cps_pipe")

   ; trampoline for deep CPS recursion
   fn sum_cps_step(n, acc, k){
       "Test helper."
       if(n == 0){
          return cps_step_more(fn(){
             "Auto-generated docstring: anonymous function."
             k(acc)
          })
       }
       cps_step_more(fn(){
          "Auto-generated docstring: anonymous function."
          sum_cps_step(n - 1, acc + n, k)
       })
   }

   def arch_name = lower(arch())
   def is_arm32 = str_contains(arch_name, "arm") && !str_contains(arch_name, "64")
   def n = is_arm32 ? 200 : 500
   def got = cps_trampoline(sum_cps_step(n, 0, fn(v){
       "Auto-generated docstring: anonymous function."
       cps_step_done(v)
   }))
   def want = (n * (n + 1)) / 2
   assert(got == want, "cps_trampoline deep recursion")

   print("✓ std.core.cps tests passed")
}
