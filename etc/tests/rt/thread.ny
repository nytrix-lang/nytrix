use std.core
use std.os.thread
use std.os.time

fn _thread_counter_worker(any: args): int {
   def m, c = args.get(0), args.get(1)
   mutex_lock(m)
   store64(c, load64(c) + 1)
   mutex_unlock(m)
   0
}

fn _thread_add(any: a, any: b): any { a + b }

fn _thread_typed_echo(any: a, int: b, int: c): dict {
   {"a": a, "b": b, "c": c}
}

fn _thread_launch_store(any: ptr, int: value): any {
   store64(ptr, value, 0)
   value
}

def counter_ptr = malloc(8)
store64(counter_ptr, 0)
def mtx = mutex_new()
def args = [mtx, counter_ptr]

def t1 = thread_spawn(_thread_counter_worker, args)
def t2 = thread_spawn(_thread_counter_worker, args)
def t3 = thread_spawn(_thread_counter_worker, args)
thread_join(t1)
thread_join(t2)
thread_join(t3)
assert(load64(counter_ptr) == 3, "thread mutex counter")

def h = thread_spawn_call(_thread_add, [20, 22])
assert(thread_join(h) == 42, "thread_spawn_call returns value")

def typed_h = thread_spawn_call(_thread_typed_echo, [0, 3937, 7874])
def typed_r = thread_join(typed_h)
assert(typed_r.get("a") == 0, "thread_spawn_call any arg")
assert(typed_r.get("b") == 3937, "thread_spawn_call typed arg")
assert(typed_r.get("c") == 7874, "thread_spawn_call second typed arg")

def launch_ptr = malloc(8)
store64(launch_ptr, 0, 0)
assert(thread_launch_call(_thread_launch_store, [launch_ptr, 17]) == 0, "thread_launch_call starts")
msleep(30)
assert(load64(launch_ptr, 0) == 17, "thread_launch_call worker ran")

mutex_free(mtx)
free(launch_ptr)
free(counter_ptr)
