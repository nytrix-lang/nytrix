use std.io
use std.os.time
use std.core
use std.strings.str

print("Benchmarking Fibonacci...")

fn fib(n){
	if(n < 2){ return n }
	return fib(n-1) + fib(n-2)
}

def t_start = ticks()
def fib_res = fib(30)
def t_end = ticks()

print("Fib(30) = ", fib_res)
print("Time (ns): ", t_end - t_start)
