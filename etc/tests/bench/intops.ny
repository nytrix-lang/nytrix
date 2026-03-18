use std.core
use std.os.time
use benchmark.helpers

def start = ticks()
mut acc = 0
mut i = 1
def n = _bench_scale(200000, 50000)
print("n=", n)
def xs = [3, 7, 11, 13]
while(i < n){
   def x = xs.get(i % 4)
   if(i < 5){ print("i=", i, "x=", x, "acc=", acc) }
   acc = acc + x
   acc = acc - (x / 3)
   acc = acc + ((x * 7) % 97)
   i = i + 1
}

def end = ticks()
print("Intops benchmark passed, acc =", acc)
print("Time(ns): ", end - start)
