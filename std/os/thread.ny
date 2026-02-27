;; Keywords: os thread
;; Os Thread module.

module std.os.thread (
   thread_spawn, thread_join, mutex_new, mutex_lock, mutex_unlock, mutex_free
)
use std.core *

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

if(comptime{__main()}){
    use std.os.thread *
    use std.core *
    use std.core.list *
    use std.core.error *

    print("Testing Threads...")

    def counter_ptr = malloc(8)
    store64(counter_ptr, 0)
    def mtx = mutex_new()
    assert(load64(counter_ptr) == 0, "init counter")

    fn worker(args){
       "Test helper."
     def m = get(args, 0)
     def c = get(args, 1)
     mutex_lock(m)
     store64(c, load64(c) + 1)
     mutex_unlock(m)
     return 0
    }

    mut args = list()
    args = append(args, mtx)
    args = append(args, counter_ptr)

    def t1 = thread_spawn(worker, args)
    def t2 = thread_spawn(worker, args)
    def t3 = thread_spawn(worker, args)

    thread_join(t1)
    thread_join(t2)
    thread_join(t3)

    mut final = load64(counter_ptr)
    assert(final == 3, "thread mutex counter")

    mutex_free(mtx)
    free(counter_ptr)

    print("✓ std.os.thread tests passed")
}
