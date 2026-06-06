;; Keywords: thread threads concurrency os
;; Os Thread for Nytrix
;; References:
;; - std.os
module std.os.thread(thread_spawn, thread_spawn_call, thread_launch, thread_launch_call, thread_join, mutex_new, mutex_lock, mutex_unlock, mutex_free)
use std.core

fn thread_spawn(fnptr target, any arg=0) any {
   "Spawns a new thread executing `func(arg)`. Returns the thread handle."
   return __thread_spawn(target, arg)
}

fn _thread_pack_args(any args) any {
   if(!is_list(args)){ args = [args] }
   def n = args.len
   if(n <= 0){ return 0 }
   def argv = malloc(n * 8)
   if(!argv){ return 0 }
   mut i = 0
   while(i < n){
      store64(argv, args.get(i, 0), i * 8)
      i += 1
   }
   argv
}

fn thread_spawn_call(fnptr target, any args=[]) any {
   "Spawns a new thread executing `func(args...)`. Supports up to 15 arguments."
   if(!is_list(args)){ args = [args] }
   def n = args.len
   if(n > 15){ panic("thread_spawn_call: max 15 arguments(got " + to_str(n) + ")") }
   def argv = _thread_pack_args(args)
   if(n > 0 && !argv){ return -1 }
   def handle = __thread_spawn_call(target, n, argv)
   if(argv){ free(argv) }
   handle
}

fn thread_launch_call(fnptr target, any args=[]) int {
   "Launches a detached thread executing `func(args...)`. Supports up to 15 arguments."
   if(!is_list(args)){ args = [args] }
   def n = args.len
   if(n > 15){ panic("thread_launch_call: max 15 arguments(got " + to_str(n) + ")") }
   def argv = _thread_pack_args(args)
   if(n > 0 && !argv){ return -1 }
   def rc = __thread_launch_call(target, n, argv)
   if(argv){ free(argv) }
   rc
}

fn thread_launch(fnptr target, any arg=0) int {
   "Launches a detached thread executing `func(arg)`."
   thread_launch_call(target, [arg])
}

fn thread_join(any th) any {
   "Waits for the thread `handle` to finish and returns its result."
   return __thread_join(th)
}

fn mutex_new() any {
   "Returns a new mutex object for thread synchronization."
   return __mutex_new()
}

fn mutex_lock(any m) any {
   "Locks the mutex `m`. If the mutex is already locked, the calling thread will block until it becomes available."
   return __mutex_lock64(m)
}

fn mutex_unlock(any m) any {
   "Unlocks the mutex `m`, allowing other threads to acquire it."
   return __mutex_unlock64(m)
}

fn mutex_free(any m) any {
   "Frees the resources associated with mutex `m`. The mutex must be unlocked before freeing."
   return __mutex_free(m)
}

fn _thread_selftest_catches(fnptr thunk) bool {
   try {
      thunk()
      false
   } catch _ {
      true
   }
}

#main {
   mut too_many = []
   mut i = 0
   while(i < 16){
      too_many = too_many.append(i)
      i += 1
   }
   assert(_thread_selftest_catches(fn() {
            thread_spawn_call(fn() { 0 }, too_many)
   }), "thread_spawn_call diagnoses too many args")
   print("✓ std.os.thread self-test passed")
}
