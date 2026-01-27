use std.io
use std.os.thread
use std.collections
use std.core.error

;; std.os.thread (Test)
;; Tests threads, mutex, and shared memory.

print("Testing Threads...")

def counter_ptr = __malloc(8)
store64(counter_ptr, 0)
def mtx = mutex_new()
assert(load64(counter_ptr) == 0, "init counter")

fn worker(args){
 def m = get(args, 0)
 def c = get(args, 1)
 mutex_lock(m)
 store64(c, load64(c) + 1)
 mutex_unlock(m)
 return 0
}

def args = list()
args = append(args, mtx)
args = append(args, counter_ptr)

def t1 = thread_spawn(worker, args)
def t2 = thread_spawn(worker, args)
def t3 = thread_spawn(worker, args)

thread_join(t1)
thread_join(t2)
thread_join(t3)

def final = load64(counter_ptr)
assert(final == 3, "thread mutex counter")

mutex_free(mtx)
__free(counter_ptr)

print("✓ std.os.thread tests passed")
