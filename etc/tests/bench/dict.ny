use std.core
use std.os.time

;; Dict (Benchmark)
mut d = dict()
def t_start = ticks()
mut i = 0
while(i < 1000){
   d = d.set(to_str(i), i)
   i = i + 1
}

def t_mid = ticks()
print("Insert(100k items) took(ns): ", t_mid - t_start)
mut idx = 0
while(idx < 1000){
   d.get(to_str(idx), -1)
   idx = idx + 1
}

def t_end = ticks()
print("Lookup(100k items) took(ns): ", t_end - t_mid)
print("Time(ns): ", t_end - t_start)
