;; Keywords: async futures tasks scheduler
;; Async task operations for futures, scheduling, sleeping, waiting, and detaching work.
module std.os.async(Task, Future, future, async, await, await_all, detach, future_wait, yield_now, sleep_ms, run, backend, state)
use std.core

fn backend(): str {
   "Returns the active async backend name."
   "stackless"
}

fn _task_from_call(fnptr: work, any: arg=0): any {
   def argv = malloc(8)
   if(!argv){ return 0 }
   store64(argv, arg, 0)
   def h = __async_task_new(work, 1, argv)
   free(argv)
   h
}

fn Task(fnptr: work, any: arg=0): any {
   "Creates a stackless task for `work(arg)` and schedules it on the cooperative runtime."
   _task_from_call(work, arg)
}

fn Future(fnptr: work, any: arg=0): any {
   "Compatibility alias for Task(work, arg)."
   Task(work, arg)
}

fn future(fnptr: work, any: arg=0): any {
   "Compatibility alias for Task(work, arg)."
   Task(work, arg)
}

fn async(fnptr: work, any: arg=0): any {
   "Creates a stackless task for `work(arg)`."
   Task(work, arg)
}

fn await(any: h): any {
   "Runs the cooperative scheduler until `handle` completes and returns its result."
   __async_await_blocking(h)
}

fn run(any: h): any {
   "Runs a task to completion."
   __async_run(h)
}

fn future_wait(any: h): any {
   "Compatibility alias for await(handle)."
   await(h)
}

fn await_all(list: handles): list {
   "Awaits each task handle in order and returns their results."
   mut out = list(handles.len)
   mut i = 0
   while(i < handles.len){
      out = out.append(await(handles.get(i)))
      i += 1
   }
   out
}

fn detach(fnptr: work, any: arg=0): any {
   "Schedules a stackless task and returns its handle; the task runs when the scheduler is driven."
   Task(work, arg)
}

fn yield_now(): any {
   "Returns a task that completes on the next scheduler turn."
   __async_yield()
}

fn sleep_ms(int: ms): any {
   "Returns a timer task that completes after `ms` milliseconds."
   __async_sleep_ms(ms)
}

fn state(any: h): int {
   "Returns the internal task state for diagnostics."
   __async_state_of(h)
}
