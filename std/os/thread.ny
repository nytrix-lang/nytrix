;; Keywords: os thread
;; Os Thread module.

use core.core
module std.os.thread (
   thread_spawn, thread_join, mutex_new, mutex_lock, mutex_unlock, mutex_free
)

fn thread_spawn(func, arg=0){
   "Spawns a new thread executing `func(arg)`. Returns the thread handle."
   ; func must be a function pointer or closure.
   ; runtime expects pure function pointer?
   ; Nytrix functions are just pointers.
   return __thread_spawn(func, arg) ; TODO use syscalls instead
}

fn thread_join(handle){
   "Waits for the thread `handle` to finish and returns its result."
   return __thread_join(handle)
}

; Synchronization

fn mutex_new(){
   "Create a new mutex."
   return __mutex_new()
}

fn mutex_lock(m){
   "Acquires the mutex `m`. Blocks if unavailable."
   return __mutex_lock64(m)
}

fn mutex_unlock(m){
   "Releases the mutex `m`."
   return __mutex_unlock64(m)
}

fn mutex_free(m){
   "Destroys the mutex `m`."
   return __mutex_free(m)
}
